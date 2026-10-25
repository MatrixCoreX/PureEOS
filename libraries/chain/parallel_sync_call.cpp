#include <eosio/chain/parallel_sync_call.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <map>

namespace eosio::chain::experimental {

namespace {
const isolated_receiver_program* pack_sync_catalog(fc::sha256::encoder& fingerprint,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol, name root_receiver) {
   fc::raw::pack(fingerprint, protocol.max_depth);
   fc::raw::pack(fingerprint, protocol.max_data_bytes);
   fc::raw::pack(fingerprint, protocol.root_read_only);
   fc::raw::pack(fingerprint, protocol.get_sender_fix);
   fc::raw::pack(fingerprint, protocol.console_enabled);
   fc::raw::pack(fingerprint, static_cast<uint32_t>(programs.size()));
   const isolated_receiver_program* root_program = nullptr;
   for (const auto& item : programs) {
      const auto& program = item.program;
      if (program.receiver == root_receiver) root_program = &program;
      fc::raw::pack(fingerprint, program.receiver);
      fc::raw::pack(fingerprint, program.serial_only);
      fc::raw::pack(fingerprint, item.sync_call_supported);
      fc::raw::pack(fingerprint, fc::sha256::hash(program.code.data(), program.code.size()));
      const auto& l = program.limits;
      fc::raw::pack(fingerprint, l.max_pages); fc::raw::pack(fingerprint, l.max_call_depth);
      fc::raw::pack(fingerprint, uint64_t(l.max_code_bytes)); fc::raw::pack(fingerprint, uint64_t(l.max_action_bytes));
      fc::raw::pack(fingerprint, uint64_t(l.max_authorizations)); fc::raw::pack(fingerprint, l.max_return_bytes);
      fc::raw::pack(fingerprint, l.max_attempt_time.count());
   }
   return root_program;
}
} // namespace

fc::sha256 isolated_sync_fingerprint(const isolated_action_input& root,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol) {
   fc::sha256::encoder fingerprint;
   constexpr char domain[] = "pureeos/isolated-sync-primary/v3";
   fingerprint.write(domain, sizeof(domain));
   const auto* root_program = pack_sync_catalog(fingerprint, programs, protocol, root.receiver);
   if (!root_program) throw isolated_receiver_missing(root.receiver);
   fc::raw::pack(fingerprint, isolated_execution_fingerprint(
      fc::sha256::hash(root_program->code.data(), root_program->code.size()), root_program->limits, root));
   return fingerprint.result();
}

fc::sha256 isolated_sync_transaction_fingerprint(std::span<const isolated_action_input> roots,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol) {
   if (roots.empty() || roots.size() > isolated_sync_executor::max_roots) throw primary_state_budget_exceeded();
   fc::sha256::encoder fingerprint;
   constexpr char domain[] = "pureeos/isolated-sync-transaction/v2";
   fingerprint.write(domain, sizeof(domain));
   // Shared catalog/protocol bytes are hashed once, not once per root.
   pack_sync_catalog(fingerprint, programs, protocol, {});
   fc::raw::pack(fingerprint, static_cast<uint32_t>(roots.size()));
   for (const auto& root : roots) {
      const auto p = std::find_if(programs.begin(), programs.end(),
         [&](const auto& candidate) { return candidate.program.receiver == root.receiver; });
      if (p == programs.end()) throw isolated_receiver_missing(root.receiver);
      fc::raw::pack(fingerprint, isolated_execution_fingerprint(
         fc::sha256::hash(p->program.code.data(), p->program.code.size()), p->program.limits, root));
   }
   return fingerprint.result();
}

namespace {
bool replay_cursor(primary_overlay_cursor& cursor, const isolated_cursor_query& query) {
   using kind = isolated_cursor_query_kind;
   int32_t actual;
   if (query.kind == kind::next || query.kind == kind::previous) {
      if (query.table != table_key{} || query.key != 0) return false;
      auto primary = query.primary_before;
      actual = query.kind == kind::next ? cursor.next(query.iterator, primary) : cursor.previous(query.iterator, primary);
      if (primary != query.primary_after) return false;
   } else {
      if (query.iterator != -1 || query.primary_before || query.primary_after) return false;
      switch (query.kind) {
         case kind::find: actual = cursor.find(query.table, query.key); break;
         case kind::lower_bound: actual = cursor.lower_bound(query.table, query.key); break;
         case kind::upper_bound: actual = cursor.upper_bound(query.table, query.key); break;
         case kind::end:
            if (query.key) return false;
            actual = cursor.end(query.table); break;
         default: return false;
      }
   }
   return actual == query.result;
}
bool valid_limits(const isolated_wasm_limits& l) {
   return l.max_pages && l.max_pages <= 16 && l.max_call_depth && l.max_call_depth <= 64 &&
      l.max_code_bytes <= isolated_wasm_limits::max_module_code_bytes && l.max_action_bytes <= 64 * 1024 && l.max_authorizations <= 64 &&
      l.max_return_bytes <= 4096 && l.max_attempt_time.count() > 0 && l.max_attempt_time <= std::chrono::seconds(1);
}
}

namespace {
struct sync_root_view {
   const isolated_action_effects& effects;
   std::span<const isolated_sync_trace> calls;
   std::span<const uint32_t> markers;
   std::span<const isolated_sync_event> events;
};

// Shared by the legacy one-root and transaction-wide frozen proofs. Only one
// replay overlay is created: later roots must observe earlier temporary IDs,
// table lifetimes and values. The owner still needs live-state/auth checks.
bool validate_sync_roots(const primary_overlay_result& state,
      std::span<const isolated_action_input> roots, std::span<const sync_root_view> results,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol,
      const fc::sha256& expected_fingerprint, bool single_root_fingerprint,
      const std::function<void()>& checkpoint) {
   using executor = isolated_sync_executor;
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   const primary_overlay_limits bounds;
   if (!state.base() || roots.empty() || roots.size() > executor::max_roots || roots.size() != results.size() ||
       (single_root_fingerprint && roots.size() != 1) ||
       programs.empty() || programs.size() > executor::max_programs ||
       state.writes().size() > bounds.max_write_operations ||
       state.read_tables().size() > bounds.max_read_tables ||
       state.read_queries().size() > bounds.max_read_queries ||
       state.secondary_read_queries().size() > bounds.max_read_queries - state.read_queries().size() ||
       state.read_accounts().size() > bounds.max_read_accounts) return false;
   std::map<name, const isolated_sync_program*> catalog;
   size_t code_bytes = 0, data_bytes = 0, write_bytes = 0;
   for (const auto& captured : programs) {
      check();
      const auto& p = captured.program;
      if (!p.receiver || !valid_limits(p.limits) || p.code.size() > p.limits.max_code_bytes ||
          p.code.size() > executor::max_total_code_bytes - code_bytes ||
          !catalog.emplace(p.receiver, &captured).second) return false;
      code_bytes += p.code.size();
   }
   for (size_t i = 0; i < roots.size(); ++i) {
      check();
      const auto& root = roots[i];
      const auto& effects = results[i].effects;
      if (root.receiver != root.account || !root.receiver ||
          root.data.size() > 64 * 1024 || root.authorizations.size() > 64 || root.max_return_bytes > 4096 ||
          root.data.size() > executor::max_total_data_bytes - data_bytes ||
          effects.return_value.size() > root.max_return_bytes || !effects.recipients.empty()) return false;
      data_bytes += root.data.size();
      const auto entry = catalog.find(root.receiver);
      if (entry == catalog.end() || entry->second->program.serial_only || entry->second->program.code.empty()) return false;
      const auto& p = entry->second->program;
      if (root.data.size() > p.limits.max_action_bytes || root.authorizations.size() > p.limits.max_authorizations ||
          root.max_return_bytes > p.limits.max_return_bytes ||
          effects.execution_fingerprint != isolated_execution_fingerprint(
             fc::sha256::hash(p.code.data(), p.code.size()), p.limits, root)) return false;
   }
   const auto actual_fingerprint = single_root_fingerprint ? isolated_sync_fingerprint(roots.front(), programs, protocol) :
                                                           isolated_sync_transaction_fingerprint(roots, programs, protocol);
   if (expected_fingerprint != actual_fingerprint) return false;
   for (const auto& mutation : state.writes()) {
      check();
      const auto bytes = isolated_state_write_bytes(mutation);
      if (bytes > bounds.max_write_value_bytes - write_bytes) return false;
      write_bytes += bytes;
   }
   primary_state_overlay replay(state.base());
   isolated_console_budget console_budget;
   size_t next_write = 0, total_calls = 0, event_count = 0;
   std::chrono::microseconds elapsed{};
   for (size_t root_index = 0; root_index < roots.size(); ++root_index) {
      check();
      const auto& root = roots[root_index];
      const auto& current = results[root_index];
      const auto& effects = current.effects;
      if (current.calls.size() > executor::max_calls - total_calls ||
          effects.writes_begin != next_write || effects.writes_end < next_write || effects.writes_end > state.writes().size() ||
          effects.elapsed.count() < 0 || effects.elapsed > executor::max_attempt_time - elapsed) return false;
      total_calls += current.calls.size();
      elapsed += effects.elapsed;
      try { if (!replay.read_account(root.receiver)) return false; } catch (const std::exception&) { return false; }
      size_t next_call = 0; // each canonical ACTION trace owns its own call ordinals
      std::function<bool(name,uint32_t,uint32_t,bool,std::span<const isolated_sync_event>,std::span<const uint32_t>,size_t)> frame;
      frame = [&](name receiver, uint32_t ordinal, uint32_t depth, bool read_only,
                  std::span<const isolated_sync_event> events, std::span<const uint32_t> markers, size_t writes_end) {
         check();
         if (events.size() > executor::max_events - event_count || writes_end < next_write ||
             writes_end > state.writes().size()) return false;
         event_count += events.size();
         primary_overlay_cursor cursor(replay, receiver);
         secondary_cursor_group secondary(replay, receiver);
         size_t marker_count = 0;
         uint32_t console_events = 0;
         for (const auto& event : events) {
            check(); // never catch the owner's cancellation/deadline as a valid fallback
            if (const auto* call = std::get_if<isolated_sync_call_event>(&event)) {
               if (call->call_ordinal != next_call + 1 || next_call >= current.calls.size() ||
                   call->after_writes != next_write || depth >= protocol.max_depth || depth >= executor::max_depth) return false;
               const auto& child = current.calls[next_call++];
               if (child.call_ordinal != call->call_ordinal || child.sender_ordinal != ordinal || child.sender != receiver ||
                   child.read_only != (read_only || bool(child.flags & 1)) || child.data.size() > protocol.max_data_bytes ||
                   child.data.size() > executor::max_total_data_bytes - data_bytes || child.writes_begin != next_write ||
                   child.writes_end < next_write || child.writes_end > writes_end) return false;
               data_bytes += child.data.size();
               if (!accept_isolated_console_marker(markers, marker_count, console_events, protocol.console_enabled)) return false;
               bool exists;
               try { exists = bool(replay.read_account(child.receiver)); } catch (const std::exception&) { return false; }
               const isolated_sync_program* captured = nullptr;
               int64_t expected_failure = 0;
               if (!exists) expected_failure = -1;
               else {
                  if (child.flags > 1) return false;
                  const auto found = catalog.find(child.receiver);
                  if (found == catalog.end() || found->second->program.serial_only) return false;
                  captured = found->second;
                  if (captured->program.code.empty()) expected_failure = -1;
                  else if (!captured->sync_call_supported) expected_failure = -2;
               }
               if (expected_failure) {
                  if (child.executed || child.status != expected_failure || !child.return_value.empty() || !child.events.empty() ||
                      !child.console_markers.empty() || child.writes_end != next_write || child.exclusive_elapsed.count() ||
                      child.execution_fingerprint != fc::sha256{} || child.began != std::chrono::steady_clock::time_point{} ||
                      child.ended != std::chrono::steady_clock::time_point{}) return false;
                  continue;
               }
               if (!child.executed || (child.status != 0 && child.status != -3 && child.status > -10000) ||
                   (child.status < 0 && !child.return_value.empty()) || child.exclusive_elapsed.count() < 0 ||
                   child.exclusive_elapsed > executor::max_attempt_time - elapsed) return false;
               elapsed += child.exclusive_elapsed;
               isolated_action_input input;
               input.receiver = input.account = child.receiver; input.data = child.data;
               input.environment = roots[root_index].environment;
               input.max_return_bytes = std::min(protocol.max_data_bytes,captured->program.limits.max_return_bytes);
               if (child.data.size() > captured->program.limits.max_action_bytes || child.return_value.size() > input.max_return_bytes ||
                   child.execution_fingerprint != isolated_execution_fingerprint(
                      fc::sha256::hash(captured->program.code.data(),captured->program.code.size()),captured->program.limits,input)) return false;
               if (!frame(child.receiver,child.call_ordinal,depth+1,child.read_only,child.events,child.console_markers,child.writes_end)) return false;
            } else {
               const auto& cursor_event = std::get<isolated_cursor_event>(event);
               try {
                  if (const auto* query = std::get_if<isolated_cursor_query>(&cursor_event)) {
                     if (!replay_cursor(cursor,*query)) return false;
                  } else if (const auto* mutation = std::get_if<isolated_cursor_write>(&cursor_event)) {
                     if (read_only || mutation->write_index != next_write || next_write >= writes_end) return false;
                     const auto& write = state.primary_write(next_write++);
                     if (write.table.code != receiver) return false;
                     switch (write.operation) {
                        case primary_operation::store:
                           if (cursor.store(write.table,write.primary_key,write.payer,write.value) != mutation->iterator) return false;
                           break;
                        case primary_operation::update: cursor.update(mutation->iterator,write.payer,write.value); break;
                        case primary_operation::remove: cursor.remove(mutation->iterator); break;
                        default: return false;
                     }
                  } else if (const auto* secondary_event = std::get_if<isolated_secondary_cursor_event>(&cursor_event)) {
                     if (!replay_isolated_secondary_cursor_event(secondary, *secondary_event, state,
                           next_write, writes_end, read_only)) return false;
                  } else if (const auto* console = std::get_if<isolated_console_event>(&cursor_event)) {
                     if (!console_budget.accept(*console, next_write, protocol.console_enabled)) return false;
                     ++console_events;
                  } else return false; // sync calls cannot enqueue notifications
               } catch (const std::exception&) { return false; }
            }
         }
         return next_write == writes_end && markers.size() == marker_count;
      };
      if (!frame(root.receiver, 0, 0, protocol.root_read_only, current.events, current.markers, effects.writes_end) ||
          next_write != effects.writes_end || next_call != current.calls.size()) return false;
   }
   if (next_write != state.writes().size()) return false;
   const auto actual = replay.seal();
   return validate_isolated_replay_state(actual, state, checkpoint);
}

} // namespace

bool validate_isolated_sync_journal(const isolated_sync_result& result, const isolated_action_input& root,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol,
      const std::function<void()>& checkpoint) {
   const sync_root_view view{result.root, result.calls, result.root_console_markers, result.root_events};
   if (checkpoint) checkpoint();
   if (!same_isolated_environment(result.environment, root.environment)) return false;
   return validate_sync_roots(result.state, {&root, 1}, {&view, 1}, programs, protocol,
                              result.execution_fingerprint, true, checkpoint);
}

bool validate_isolated_sync_transaction_journal(const isolated_sync_transaction_result& result,
      std::span<const isolated_action_input> roots, std::span<const isolated_sync_program> programs,
      isolated_sync_context protocol, const std::function<void()>& checkpoint) {
   if (checkpoint) checkpoint();
   if (roots.empty() || roots.size() > isolated_sync_executor::max_roots || result.roots.size() != roots.size()) return false;
   if (!same_isolated_environment(result.environment, roots.front().environment)) return false;
   for (const auto& root : roots)
      if (!same_isolated_environment(root.environment, result.environment)) return false;
   std::vector<sync_root_view> views;
   views.reserve(result.roots.size());
   for (const auto& root : result.roots)
      views.push_back({root.root, root.calls, root.root_console_markers, root.root_events});
   return validate_sync_roots(result.state, roots, views, programs, protocol, result.execution_fingerprint, false, checkpoint);
}
} // namespace eosio::chain::experimental
