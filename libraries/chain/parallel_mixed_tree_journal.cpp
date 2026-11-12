#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <map>

namespace eosio::chain::experimental {
namespace {
bool same_input(const isolated_action_input& a, const isolated_action_input& b) {
   return a.receiver == b.receiver && a.account == b.account && a.action == b.action &&
      a.authorizations == b.authorizations && a.data == b.data && a.max_return_bytes == b.max_return_bytes &&
      same_isolated_environment(a.environment, b.environment);
}
bool valid_limits(const isolated_wasm_limits& l) {
   return l.max_pages && l.max_pages <= 16 && l.max_call_depth && l.max_call_depth <= 64 &&
      l.max_code_bytes <= isolated_wasm_limits::max_module_code_bytes && l.max_action_bytes <= 64 * 1024 && l.max_authorizations <= 64 &&
      l.max_return_bytes <= 4096 && l.max_attempt_time.count() > 0 && l.max_attempt_time <= std::chrono::seconds(1);
}
bool replay_query(primary_overlay_cursor& cursor, const isolated_cursor_query& q) {
   using kind = isolated_cursor_query_kind;
   int32_t actual;
   if (q.kind == kind::next || q.kind == kind::previous) {
      if (q.table != table_key{} || q.key) return false;
      auto primary = q.primary_before;
      actual = q.kind == kind::next ? cursor.next(q.iterator, primary) : cursor.previous(q.iterator, primary);
      if (primary != q.primary_after) return false;
   } else {
      if (q.iterator != -1 || q.primary_before || q.primary_after) return false;
      switch (q.kind) {
         case kind::find: actual = cursor.find(q.table, q.key); break;
         case kind::lower_bound: actual = cursor.lower_bound(q.table, q.key); break;
         case kind::upper_bound: actual = cursor.upper_bound(q.table, q.key); break;
         case kind::end:
            if (q.key) return false;
            actual = cursor.end(q.table); break;
         default: return false;
      }
   }
   return actual == q.result;
}
} // namespace

bool validate_isolated_mixed_tree_journal(const isolated_mixed_tree_result& result,
      std::span<const isolated_action_input> roots, std::span<const isolated_sync_program> programs,
      const isolated_mixed_tree_context& context, const std::function<void()>& checkpoint) {
   using executor = isolated_mixed_tree_executor;
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   const auto& state = result.state;
   const auto& protocol = context.sync;
   if (context.tree.console_enabled != protocol.console_enabled) return false;
   const primary_overlay_limits bounds;
   if (!state.base() || roots.empty() || roots.size() > isolated_sync_executor::max_roots ||
       programs.empty() || programs.size() > executor::max_programs ||
       result.groups.size() < roots.size() || result.groups.size() > executor::max_steps ||
       result.steps.size() < result.groups.size() || result.steps.size() > executor::max_steps ||
       state.writes().size() > bounds.max_write_operations || state.read_tables().size() > bounds.max_read_tables ||
       state.read_queries().size() > bounds.max_read_queries ||
       state.secondary_read_queries().size() > bounds.max_read_queries - state.read_queries().size() ||
       state.read_accounts().size() > bounds.max_read_accounts ||
       bool(result.context_free_data) != bool(context.tree.context_free_data) ||
       (result.context_free_data && result.context_free_data->fingerprint() != context.tree.context_free_data->fingerprint()))
      return false;
   struct entry { const isolated_sync_program* captured; fc::sha256 hash; };
   std::map<name, entry> catalog;
   size_t code_bytes = 0, data_bytes = 0, write_bytes = 0;
   for (const auto& captured : programs) {
      check();
      const auto& p = captured.program;
      if (!p.receiver || !valid_limits(p.limits) || p.code.size() > p.limits.max_code_bytes ||
          p.code.size() > isolated_sync_executor::max_total_code_bytes - code_bytes ||
          !catalog.emplace(p.receiver, entry{&captured,
             p.code.empty() ? fc::sha256{} : fc::sha256::hash(p.code.data(), p.code.size())}).second) return false;
      code_bytes += p.code.size();
   }
   for (size_t i = 0; i < roots.size(); ++i) {
      check();
      const auto& root = roots[i]; const auto& group = result.groups[i];
      if (!same_isolated_environment(root.environment, roots.front().environment) ||
          !root.receiver || root.receiver != root.account || root.data.size() > 64 * 1024 ||
          root.data.size() > executor::max_data_bytes - data_bytes || root.authorizations.size() > 64 ||
          root.max_return_bytes > 4096 || group.root_index != i || group.action_ordinal != i + 1 ||
          group.creator_action_ordinal || group.closest_unnotified_ancestor || group.depth || group.sender ||
          group.context_free || !same_input(root, group.input)) return false;
      const auto p = catalog.find(root.receiver);
      if (p == catalog.end() || p->second.captured->program.serial_only || p->second.captured->program.code.empty()) return false;
      data_bytes += root.data.size();
   }
   if (result.execution_fingerprint != isolated_mixed_tree_fingerprint(roots, programs, context)) return false;
   for (const auto& mutation : state.writes()) {
      check();
      const auto bytes = isolated_state_write_bytes(mutation);
      if (bytes > bounds.max_write_value_bytes - write_bytes) return false;
      write_bytes += bytes;
   }

   primary_state_overlay replay(state.base());
   isolated_console_budget console_budget;
   size_t next_step = 0, next_group = roots.size(), next_write = 0, event_count = 0, total_calls = 0;
   uint32_t next_ordinal = roots.size() + 1;
   std::chrono::microseconds elapsed{};
   const auto account_exists = [&](name account) -> std::optional<bool> {
      try { return bool(replay.read_account(account)); } catch (const std::exception&) { return {}; }
   };
   // No owner callback inside these catches. Only private state reconstruction
   // errors select fallback; an interrupt can never be mistaken for success.
   const auto replay_database = [&](primary_overlay_cursor& cursor, secondary_cursor_group& secondary, name receiver, bool read_only,
                                    const isolated_cursor_event& event, size_t writes_end) {
      try {
         if (const auto* query = std::get_if<isolated_cursor_query>(&event)) return replay_query(cursor, *query);
         if (const auto* secondary_event = std::get_if<isolated_secondary_cursor_event>(&event))
            return replay_isolated_secondary_cursor_event(secondary, *secondary_event, state, next_write, writes_end, read_only);
         const auto* mutation = std::get_if<isolated_cursor_write>(&event);
         if (!mutation || read_only || mutation->write_index != next_write || next_write >= writes_end) return false;
         const auto& write = state.primary_write(next_write++);
         if (write.table.code != receiver) return false;
         switch (write.operation) {
            case primary_operation::store:
               return cursor.store(write.table, write.primary_key, write.payer, write.value) == mutation->iterator;
            case primary_operation::update: cursor.update(mutation->iterator, write.payer, write.value); return true;
            case primary_operation::remove: cursor.remove(mutation->iterator); return true;
            default: return false;
         }
      } catch (const std::exception&) { return false; }
   };
   struct pending_receiver { name receiver; uint32_t ordinal, creator, ancestor; name sender; };
   std::function<bool(size_t)> replay_group = [&](size_t group_index) {
      check();
      const auto& group = result.groups[group_index];
      primary_overlay_cursor cursor(replay, group.input.receiver);
      secondary_cursor_group secondary(replay, group.input.receiver);
      std::set<name> notified{group.input.receiver};
      std::vector<pending_receiver> pending{{group.input.receiver, group.action_ordinal,
         group.creator_action_ordinal, group.closest_unnotified_ancestor, group.sender}};
      std::vector<size_t> children, cf_children;
      for (size_t i = 0; i < pending.size(); ++i) {
         check();
         if (next_step >= result.steps.size()) return false;
         const auto expected = pending[i]; const auto& step = result.steps[next_step++];
         const auto& effects = step.effects;
         if (step.group_index != group_index || step.receiver != expected.receiver || step.sender != expected.sender ||
             step.action_ordinal != expected.ordinal || step.creator_action_ordinal != expected.creator ||
             step.closest_unnotified_ancestor != expected.ancestor || effects.writes_begin != next_write ||
             effects.writes_end < next_write || effects.writes_end > state.writes().size() ||
             effects.recipients.size() > isolated_action_result::max_recipients ||
             effects.return_value.size() > group.input.max_return_bytes || effects.elapsed.count() < 0 ||
             effects.elapsed > isolated_sync_executor::max_attempt_time - elapsed ||
             step.events.size() > executor::max_events - event_count || step.calls.size() > executor::max_calls - total_calls)
            return false;
         elapsed += effects.elapsed; event_count += step.events.size(); total_calls += step.calls.size();
         if (account_exists(step.receiver) != std::optional<bool>{true}) return false;
         const auto found = catalog.find(step.receiver);
         if (found == catalog.end()) return false;
         const auto& p = found->second.captured->program;
         if (p.serial_only || step.code_hash != found->second.hash || step.has_code != !p.code.empty() ||
             group.input.data.size() > p.limits.max_action_bytes ||
             group.input.authorizations.size() > p.limits.max_authorizations || group.input.max_return_bytes > p.limits.max_return_bytes)
            return false;
         auto input = group.input; input.receiver = step.receiver;
         if (isolated_mixed_tree_step_fingerprint(isolated_execution_fingerprint(found->second.hash, p.limits, input),
               result.execution_fingerprint, context, group, step) != effects.execution_fingerprint) return false;
         if (!step.has_code && (!step.events.empty() || !step.calls.empty() || !step.console_markers.empty() ||
               !effects.recipients.empty() || !effects.return_value.empty() || effects.writes_end != next_write ||
               effects.elapsed.count() || effects.began != std::chrono::steady_clock::time_point{} ||
               effects.ended != std::chrono::steady_clock::time_point{})) return false;
         if (group.context_free && (!context.tree.context_free_data || !group.input.authorizations.empty() ||
               !step.calls.empty() || !step.console_markers.empty() || !effects.recipients.empty() ||
               !effects.return_value.empty() || effects.writes_end != next_write)) return false;
         cursor.set_receiver_for_notification(step.receiver);
         secondary.set_receiver_for_notification(step.receiver);
         size_t next_call = 0, next_notification = 0, marker_count = 0;
         uint32_t console_events = 0;
         std::function<bool(const isolated_sync_call_event&, name, uint32_t, uint32_t, bool, size_t)> replay_call;
         replay_call = [&](const isolated_sync_call_event& event, name receiver, uint32_t parent,
                           uint32_t depth, bool read_only, size_t writes_end) {
            check();
            if (event.call_ordinal != next_call + 1 || next_call >= step.calls.size() || event.after_writes != next_write ||
                depth >= protocol.max_depth || depth >= isolated_sync_executor::max_depth) return false;
            const auto& child = step.calls[next_call++];
            if (child.call_ordinal != event.call_ordinal || child.sender_ordinal != parent || child.sender != receiver ||
                child.read_only != (read_only || bool(child.flags & 1)) || child.data.size() > protocol.max_data_bytes ||
                child.data.size() > executor::max_data_bytes - data_bytes || child.writes_begin != next_write ||
                child.writes_end < next_write || child.writes_end > writes_end) return false;
            data_bytes += child.data.size();
            const auto exists = account_exists(child.receiver);
            if (!exists) return false;
            const isolated_sync_program* captured = nullptr;
            int64_t expected_failure = 0;
            if (!*exists) expected_failure = -1;
            else {
               if (child.flags > 1) return false;
               const auto entry = catalog.find(child.receiver);
               if (entry == catalog.end() || entry->second.captured->program.serial_only) return false;
               captured = entry->second.captured;
               if (captured->program.code.empty()) expected_failure = -1;
               else if (!captured->sync_call_supported) expected_failure = -2;
            }
            if (expected_failure) return !child.executed && child.status == expected_failure && child.return_value.empty() &&
               child.events.empty() && child.console_markers.empty() && child.writes_end == next_write &&
               !child.exclusive_elapsed.count() && child.execution_fingerprint == fc::sha256{} &&
               child.began == std::chrono::steady_clock::time_point{} && child.ended == std::chrono::steady_clock::time_point{};
            if (!child.executed || (child.status != 0 && child.status != -3 && child.status > -10000) ||
                (child.status < 0 && !child.return_value.empty()) || child.exclusive_elapsed.count() < 0 ||
                child.exclusive_elapsed > isolated_sync_executor::max_attempt_time - elapsed ||
                child.events.size() > executor::max_events - event_count) return false;
            elapsed += child.exclusive_elapsed; event_count += child.events.size();
            isolated_action_input input;
            input.receiver = input.account = child.receiver; input.data = child.data;
            input.environment = group.input.environment;
            input.max_return_bytes = std::min(protocol.max_data_bytes, captured->program.limits.max_return_bytes);
            if (child.data.size() > captured->program.limits.max_action_bytes || child.return_value.size() > input.max_return_bytes ||
                child.execution_fingerprint != isolated_execution_fingerprint(fc::sha256::hash(captured->program.code.data(),
                   captured->program.code.size()), captured->program.limits, input)) return false;
            primary_overlay_cursor child_cursor(replay, child.receiver);
            secondary_cursor_group child_secondary(replay, child.receiver);
            size_t markers = 0;
            uint32_t child_console_events = 0;
            for (const auto& nested : child.events) {
               check(); // intentionally OUTSIDE all private-error catches
               if (const auto* call = std::get_if<isolated_sync_call_event>(&nested)) {
                  if (!accept_isolated_console_marker(child.console_markers, markers, child_console_events,
                                                       protocol.console_enabled)) return false;
                  if (!replay_call(*call, child.receiver, child.call_ordinal, depth + 1, child.read_only, child.writes_end)) return false;
               } else {
                  const auto& cursor_event = std::get<isolated_cursor_event>(nested);
                  if (const auto* console = std::get_if<isolated_console_event>(&cursor_event)) {
                     if (!console_budget.accept(*console, next_write, protocol.console_enabled)) return false;
                     ++child_console_events;
                  } else if (!replay_database(child_cursor, child_secondary, child.receiver, child.read_only,
                                             cursor_event, child.writes_end)) return false;
               }
            }
            return next_write == child.writes_end && child.console_markers.size() == markers;
         };
         for (const auto& event : step.events) {
            check();
            if (const auto* cursor_event = std::get_if<isolated_cursor_event>(&event)) {
               if (const auto* console = std::get_if<isolated_console_event>(cursor_event)) {
                  if (!console_budget.accept(*console, next_write, protocol.console_enabled)) return false;
                  ++console_events;
                  continue;
               }
            }
            if (group.context_free) return false;
            if (const auto* call = std::get_if<isolated_sync_call_event>(&event)) {
               if (!accept_isolated_console_marker(step.console_markers, marker_count, console_events,
                                                    protocol.console_enabled)) return false;
               if (!replay_call(*call, step.receiver, 0, 0, protocol.root_read_only, effects.writes_end)) return false;
            } else if (const auto* send = std::get_if<isolated_tree_inline_event>(&event)) {
               const auto& raw = send->sent.packed;
               if (send->after_writes != next_write || send->action_ordinal != next_ordinal || next_ordinal > executor::max_steps ||
                   next_group >= result.groups.size() || raw.size() > executor::max_data_bytes - data_bytes ||
                   group.depth >= context.tree.max_inline_action_depth || group.depth >= isolated_action_tree_executor::max_depth)
                  return false;
               const auto decoded = decode_isolated_inline_action(raw, context.tree.max_inline_action_size);
               if (!decoded || decoded->decoded_bytes != send->sent.decoded_bytes ||
                   decoded->decoded.account != send->sent.decoded.account || decoded->decoded.name != send->sent.decoded.name ||
                   decoded->decoded.authorization != send->sent.decoded.authorization || decoded->decoded.data != send->sent.decoded.data ||
                   (send->context_free && (!context.tree.context_free_data || !decoded->decoded.authorization.empty()))) return false;
               data_bytes += raw.size();
               const auto& child = result.groups[next_group];
               if (child.root_index != group.root_index || child.action_ordinal != next_ordinal ||
                   child.creator_action_ordinal != step.action_ordinal || child.closest_unnotified_ancestor != group.action_ordinal ||
                   child.depth != group.depth + 1 || child.sender != step.receiver || child.context_free != send->context_free) return false;
               isolated_action_input input;
               input.receiver = input.account = decoded->decoded.account; input.action = decoded->decoded.name;
               input.data = decoded->decoded.data; input.max_return_bytes = group.input.max_return_bytes;
               input.environment = group.input.environment;
               for (const auto& permission : decoded->decoded.authorization)
                  input.authorizations.emplace_back(permission.actor, permission.permission);
               if (!same_input(input, child.input)) return false;
               (send->context_free ? cf_children : children).push_back(next_group++); ++next_ordinal;
            } else {
               const auto& nested = std::get<isolated_cursor_event>(event);
               if (const auto* notify = std::get_if<isolated_cursor_notify>(&nested)) {
                  if (notify->notification_index != next_notification || next_notification >= effects.recipients.size()) return false;
                  const auto& recipient = effects.recipients[next_notification++];
                  if (recipient.after_writes != next_write || !recipient.recipient || next_ordinal > executor::max_steps ||
                      notified.size() >= isolated_action_result::max_recipients + 1 || !notified.insert(recipient.recipient).second)
                     return false;
                  pending.push_back({recipient.recipient, next_ordinal++, step.action_ordinal, group.action_ordinal, step.receiver});
               } else if (!replay_database(cursor, secondary, step.receiver, protocol.root_read_only, nested, effects.writes_end)) return false;
            }
         }
         if (next_write != effects.writes_end || next_call != step.calls.size() || next_notification != effects.recipients.size() ||
             step.console_markers.size() != marker_count) return false;
      }
      for (const auto child : cf_children) if (!replay_group(child)) return false;
      for (const auto child : children) if (!replay_group(child)) return false;
      return true;
   };
   for (size_t i = 0; i < roots.size(); ++i) if (!replay_group(i)) return false;
   if (next_step != result.steps.size() || next_group != result.groups.size() || next_write != state.writes().size()) return false;
   const auto reconstructed = replay.seal();
   return validate_isolated_replay_state(reconstructed, state, checkpoint);
}
} // namespace eosio::chain::experimental
