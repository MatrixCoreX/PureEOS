#include <eosio/chain/parallel_sync_call.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <map>

namespace eosio::chain::experimental {

fc::sha256 isolated_sync_fingerprint(const isolated_action_input& root,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol) {
   fc::sha256::encoder fingerprint;
   constexpr char domain[] = "pureeos/isolated-sync-primary/v2";
   fingerprint.write(domain, sizeof(domain));
   fc::raw::pack(fingerprint, protocol.max_depth);
   fc::raw::pack(fingerprint, protocol.max_data_bytes);
   fc::raw::pack(fingerprint, protocol.root_read_only);
   fc::raw::pack(fingerprint, protocol.get_sender_fix);
   fc::raw::pack(fingerprint, protocol.console_enabled);
   fc::raw::pack(fingerprint, static_cast<uint32_t>(programs.size()));
   const isolated_receiver_program* root_program = nullptr;
   for (const auto& item : programs) {
      const auto& program = item.program;
      if (program.receiver == root.receiver) root_program = &program;
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
   if (!root_program) throw isolated_receiver_missing(root.receiver);
   fc::raw::pack(fingerprint, isolated_execution_fingerprint(
      fc::sha256::hash(root_program->code.data(), root_program->code.size()), root_program->limits, root));
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
      l.max_code_bytes <= 64 * 1024 && l.max_action_bytes <= 64 * 1024 && l.max_authorizations <= 64 &&
      l.max_return_bytes <= 4096 && l.max_attempt_time.count() > 0 && l.max_attempt_time <= std::chrono::seconds(1);
}
}

bool validate_isolated_sync_journal(const isolated_sync_result& result, const isolated_action_input& root,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol,
      const std::function<void()>& checkpoint) {
   using executor = isolated_sync_executor;
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   const primary_overlay_limits bounds;
   if (!result.state.base() || programs.empty() || programs.size() > executor::max_programs ||
       result.calls.size() > executor::max_calls || root.receiver != root.account || !root.receiver ||
       root.data.size() > 64 * 1024 || root.authorizations.size() > 64 || root.max_return_bytes > 4096 ||
       result.root.return_value.size() > root.max_return_bytes || !result.root.recipients.empty() ||
       result.root.writes_begin || result.root.writes_end != result.state.writes().size() ||
       result.state.writes().size() > bounds.max_write_operations ||
       result.state.read_tables().size() > bounds.max_read_tables ||
       result.state.read_queries().size() > bounds.max_read_queries ||
       result.state.read_accounts().size() > bounds.max_read_accounts) return false;
   std::map<name, const isolated_sync_program*> catalog;
   size_t code_bytes = 0, data_bytes = root.data.size(), write_bytes = 0;
   for (const auto& captured : programs) {
      check();
      const auto& p = captured.program;
      if (!p.receiver || !valid_limits(p.limits) || p.code.size() > p.limits.max_code_bytes ||
          p.code.size() > executor::max_total_code_bytes - code_bytes ||
          !catalog.emplace(p.receiver, &captured).second) return false;
      code_bytes += p.code.size();
   }
   const auto root_entry = catalog.find(root.receiver);
   if (root_entry == catalog.end() || root_entry->second->program.serial_only || root_entry->second->program.code.empty()) return false;
   const auto& rp = root_entry->second->program;
   if (root.data.size() > rp.limits.max_action_bytes || root.authorizations.size() > rp.limits.max_authorizations ||
       root.max_return_bytes > rp.limits.max_return_bytes ||
       result.root.execution_fingerprint != isolated_execution_fingerprint(fc::sha256::hash(rp.code.data(),rp.code.size()),rp.limits,root) ||
       result.execution_fingerprint != isolated_sync_fingerprint(root,programs,protocol)) return false;
   for (const auto& write : result.state.writes()) {
      check();
      if (write.value.size() > bounds.max_write_value_bytes - write_bytes) return false;
      write_bytes += write.value.size();
   }
   primary_state_overlay replay(result.state.base());
   try { if (!replay.read_account(root.receiver)) return false; } catch (const std::exception&) { return false; }
   size_t next_write = 0, next_call = 0, event_count = 0;
   std::chrono::microseconds elapsed = result.root.elapsed;
   if (elapsed.count() < 0 || elapsed > executor::max_attempt_time) return false;
   std::function<bool(name,uint32_t,uint32_t,bool,std::span<const isolated_sync_event>,std::span<const uint32_t>,size_t)> frame;
   frame = [&](name receiver, uint32_t ordinal, uint32_t depth, bool read_only,
               std::span<const isolated_sync_event> events, std::span<const uint32_t> markers, size_t writes_end) {
      check();
      if (events.size() > executor::max_events - event_count || writes_end < next_write ||
          writes_end > result.state.writes().size()) return false;
      event_count += events.size();
      primary_overlay_cursor cursor(replay, receiver);
      size_t marker_count = 0;
      for (const auto& event : events) {
         check(); // never catch the owner's cancellation/deadline as a valid fallback
         if (const auto* call = std::get_if<isolated_sync_call_event>(&event)) {
            if (call->call_ordinal != next_call + 1 || next_call >= result.calls.size() ||
                call->after_writes != next_write || depth >= protocol.max_depth || depth >= executor::max_depth) return false;
            const auto& child = result.calls[next_call++];
            if (child.call_ordinal != call->call_ordinal || child.sender_ordinal != ordinal || child.sender != receiver ||
                child.read_only != (read_only || bool(child.flags & 1)) || child.data.size() > protocol.max_data_bytes ||
                child.data.size() > executor::max_total_data_bytes - data_bytes || child.writes_begin != next_write ||
                child.writes_end < next_write || child.writes_end > writes_end) return false;
            data_bytes += child.data.size();
            if (protocol.console_enabled) ++marker_count;
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
                  const auto& write = result.state.writes()[next_write++];
                  if (write.table.code != receiver) return false;
                  switch (write.operation) {
                     case primary_operation::store:
                        if (cursor.store(write.table,write.primary_key,write.payer,write.value) != mutation->iterator) return false;
                        break;
                     case primary_operation::update: cursor.update(mutation->iterator,write.payer,write.value); break;
                     case primary_operation::remove: cursor.remove(mutation->iterator); break;
                     default: return false;
                  }
               } else return false; // sync calls cannot enqueue notifications
            } catch (const std::exception&) { return false; }
         }
      }
      return next_write == writes_end && markers.size() == marker_count &&
         std::all_of(markers.begin(),markers.end(),[](uint32_t n){ return n == 0; }); // printing still unsupported
   };
   if (!frame(root.receiver,0,0,protocol.root_read_only,result.root_events,result.root_console_markers,result.root.writes_end) ||
       next_write != result.state.writes().size() || next_call != result.calls.size()) return false;
   const auto actual = replay.seal();
   if (actual.writes().size() != result.state.writes().size()) return false;
   for (size_t i=0;i<actual.writes().size();++i) {
      check();
      const auto& a=actual.writes()[i]; const auto& b=result.state.writes()[i];
      if (a.table != b.table || a.primary_key != b.primary_key || a.payer != b.payer || a.value != b.value ||
          a.operation != b.operation || a.object_id != b.object_id || a.table_object_id != b.table_object_id) return false;
   }
   for (const auto& t:actual.read_tables()) { check(); if (!result.state.read_tables().contains(t)) return false; }
   for (const auto& q:actual.read_queries()) { check(); if (!result.state.read_queries().contains(q)) return false; }
   for (const auto& a:actual.read_accounts()) { check(); if (!result.state.read_accounts().contains(a)) return false; }
   return true;
}
} // namespace eosio::chain::experimental
