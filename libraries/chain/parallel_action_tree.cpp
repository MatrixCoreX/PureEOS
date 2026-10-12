#include <eosio/chain/parallel_action_tree.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <map>

namespace eosio::chain::experimental {
namespace {
fc::sha256 context_free_data_hash(const std::vector<std::vector<char>>& data) {
   fc::sha256::encoder encoder;
   constexpr char domain[] = "pureeos/isolated-context-free-data/v1";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, data);
   return encoder.result();
}
bool same_input(const isolated_action_input& a, const isolated_action_input& b) {
   return a.receiver == b.receiver && a.account == b.account && a.action == b.action &&
      a.authorizations == b.authorizations && a.data == b.data && a.max_return_bytes == b.max_return_bytes;
}
bool same_action(const action& a, const action& b) {
   return a.account == b.account && a.name == b.name && a.authorization == b.authorization && a.data == b.data;
}
} // namespace

isolated_context_free_data::isolated_context_free_data(std::span<const std::vector<char>> input)
   : data_(input.begin(), input.end()), hash_(context_free_data_hash(data_)) {}

std::shared_ptr<const isolated_context_free_data> isolated_context_free_data::capture(
      std::span<const std::vector<char>> input) {
   if (input.size() > max_entries) throw primary_state_budget_exceeded();
   size_t bytes = 0;
   for (const auto& item : input) {
      if (item.size() > max_bytes - bytes) throw primary_state_budget_exceeded();
      bytes += item.size();
   }
   return std::shared_ptr<const isolated_context_free_data>(new isolated_context_free_data(input));
}

int32_t isolated_context_free_data::read(uint32_t index, std::span<char> destination) const {
   if (index >= data_.size()) return -1;
   const auto& data = data_[index];
   if (destination.empty()) return data.size();
   const auto count = std::min(data.size(), destination.size());
   std::copy_n(data.begin(), count, destination.begin());
   return count;
}

bool validate_isolated_action_tree_journal(const isolated_action_tree_result& result,
      std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
      isolated_action_tree_context context, const std::function<void()>& checkpoint) {
   using executor = isolated_action_tree_executor;
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   const primary_overlay_limits bounds;
   if (!result.state.base() || roots.empty() || roots.size() > isolated_transaction_executor::max_actions ||
       programs.empty() || programs.size() > executor::max_programs ||
       result.groups.size() < roots.size() || result.groups.size() > executor::max_steps ||
       result.steps.size() < result.groups.size() || result.steps.size() > executor::max_steps ||
       result.state.writes().size() > bounds.max_write_operations ||
       result.state.read_tables().size() > bounds.max_read_tables ||
       result.state.read_queries().size() > bounds.max_read_queries ||
       result.state.read_accounts().size() > bounds.max_read_accounts) return false;
   struct entry { const isolated_receiver_program* program; fc::sha256 hash; };
   std::map<name, entry> catalog;
   size_t code_bytes = 0, data_bytes = 0, write_bytes = 0;
   for (const auto& program : programs) {
      check();
      if (program.code.size() > 64 * 1024 ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return false;
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, entry{&program,
            program.code.empty() ? fc::sha256{} : fc::sha256::hash(program.code.data(), program.code.size())}).second)
         return false;
   }
   std::vector<fc::sha256> fingerprints;
   for (size_t i = 0; i < roots.size(); ++i) {
      check();
      const auto& root = roots[i]; const auto& group = result.groups[i];
      if (root.receiver != root.account || root.data.size() > 64 * 1024 ||
          root.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes ||
          root.authorizations.size() > 64 || root.max_return_bytes > 4096 ||
          group.root_index != i || group.action_ordinal != i + 1 || group.creator_action_ordinal != 0 ||
          group.closest_unnotified_ancestor != 0 || group.depth != 0 || group.sender || group.context_free ||
          !same_input(group.input, root)) return false;
      data_bytes += root.data.size();
      const auto entry = catalog.find(root.receiver);
      if (entry == catalog.end() || entry->second.program->serial_only || entry->second.program->code.empty()) return false;
      fingerprints.push_back(isolated_execution_fingerprint(entry->second.hash, entry->second.program->limits, root));
   }
   if (isolated_action_tree_fingerprint(fingerprints, context) != result.root_fingerprint) return false;
   for (const auto& write : result.state.writes()) {
      check();
      if (write.value.size() > bounds.max_write_value_bytes - write_bytes) return false;
      write_bytes += write.value.size();
   }
   primary_state_overlay replay(result.state.base());
   size_t next_step = 0, next_group = roots.size(), next_write = 0, event_count = 0, inline_bytes = 0;
   uint32_t next_ordinal = roots.size() + 1;
   std::chrono::microseconds elapsed{};
   struct pending_receiver { name receiver; uint32_t ordinal, creator, ancestor; name sender; };
   std::function<bool(size_t)> replay_group = [&](size_t group_index) {
      check();
      const auto& group = result.groups[group_index];
      primary_overlay_cursor cursor(replay, group.input.receiver);
      std::set<name> notified{group.input.receiver};
      std::vector<pending_receiver> pending{{group.input.receiver, group.action_ordinal,
         group.creator_action_ordinal, group.closest_unnotified_ancestor, group.sender}};
      std::vector<size_t> children, context_free_children;
      for (size_t i = 0; i < pending.size(); ++i) {
         check();
         if (next_step == result.steps.size()) return false;
         const auto expected = pending[i]; const auto& step = result.steps[next_step++];
         const auto& effects = step.effects;
         if (step.group_index != group_index || step.receiver != expected.receiver ||
             step.action_ordinal != expected.ordinal || step.creator_action_ordinal != expected.creator ||
             step.closest_unnotified_ancestor != expected.ancestor || step.sender != expected.sender ||
             effects.writes_begin != next_write || effects.writes_end < next_write ||
             effects.writes_end > result.state.writes().size() ||
             effects.recipients.size() > isolated_action_result::max_recipients ||
             effects.return_value.size() > group.input.max_return_bytes || effects.elapsed.count() < 0 ||
             effects.elapsed > isolated_transaction_executor::max_attempt_time - elapsed ||
             step.events.size() > executor::max_events - event_count) return false;
         elapsed += effects.elapsed; event_count += step.events.size();
         const auto found = catalog.find(step.receiver);
         if (found == catalog.end()) return false;
         const auto& program = *found->second.program;
         if (program.serial_only || step.code_hash != found->second.hash || step.has_code != !program.code.empty()) return false;
         auto arguments = group.input; arguments.receiver = step.receiver;
         const auto action_fingerprint = isolated_execution_fingerprint(found->second.hash, program.limits, arguments);
         if (effects.execution_fingerprint != isolated_action_tree_step_fingerprint(action_fingerprint, context, group, step))
            return false;
         if (!step.has_code && (!step.events.empty() || !effects.recipients.empty() || !effects.return_value.empty() ||
               effects.writes_end != next_write || effects.elapsed.count() != 0)) return false;
         // A context-free step cannot read state, mutate cursors, schedule any
         // receiver/child, or use the context-aware action-return-value API.
         if (group.context_free && (!context.context_free_data || !group.input.authorizations.empty() ||
               !step.events.empty() || !effects.recipients.empty() || !effects.return_value.empty() ||
               effects.writes_end != next_write)) return false;
         cursor.set_receiver_for_notification(step.receiver);
         size_t next_notification = 0;
         for (const auto& event : step.events) {
            // The owner checkpoint is OUTSIDE the local replay catch. Its
            // interrupt/deadline exception must not become a successful fallback.
            check();
            try {
               if (const auto* nested = std::get_if<isolated_cursor_event>(&event)) {
                  if (const auto* query = std::get_if<isolated_cursor_query>(nested)) {
                     using kind = isolated_cursor_query_kind;
                     int32_t actual;
                     if (query->kind == kind::next || query->kind == kind::previous) {
                        if (query->table != table_key{} || query->key != 0) return false;
                        auto primary = query->primary_before;
                        actual = query->kind == kind::next ? cursor.next(query->iterator, primary) :
                                                            cursor.previous(query->iterator, primary);
                        if (primary != query->primary_after) return false;
                     } else {
                        if (query->iterator != -1 || query->primary_before != 0 || query->primary_after != 0) return false;
                        switch (query->kind) {
                           case kind::find: actual = cursor.find(query->table, query->key); break;
                           case kind::lower_bound: actual = cursor.lower_bound(query->table, query->key); break;
                           case kind::upper_bound: actual = cursor.upper_bound(query->table, query->key); break;
                           case kind::end:
                              if (query->key != 0) return false;
                              actual = cursor.end(query->table); break;
                           default: return false;
                        }
                     }
                     if (actual != query->result) return false;
                  } else if (const auto* write_event = std::get_if<isolated_cursor_write>(nested)) {
                     if (write_event->write_index != next_write || next_write >= effects.writes_end) return false;
                     const auto& write = result.state.writes()[next_write++];
                     if (write.table.code != step.receiver) return false;
                     switch (write.operation) {
                        case primary_operation::store:
                           if (cursor.store(write.table, write.primary_key, write.payer, write.value) != write_event->iterator)
                              return false;
                           break;
                        case primary_operation::update: cursor.update(write_event->iterator, write.payer, write.value); break;
                        case primary_operation::remove: cursor.remove(write_event->iterator); break;
                        default: return false;
                     }
                  } else if (const auto* notify = std::get_if<isolated_cursor_notify>(nested)) {
                     if (notify->notification_index != next_notification || next_notification >= effects.recipients.size()) return false;
                     const auto& recipient = effects.recipients[next_notification++];
                     if (recipient.after_writes != next_write || !recipient.recipient ||
                         notified.size() >= isolated_action_result::max_recipients + 1 ||
                         !notified.insert(recipient.recipient).second || next_ordinal > executor::max_steps) return false;
                     pending.push_back({recipient.recipient, next_ordinal++, step.action_ordinal, group.action_ordinal, step.receiver});
                  } else return false;
               } else if (const auto* send = std::get_if<isolated_tree_inline_event>(&event)) {
                  const auto& raw = send->sent.packed;
                  if (send->after_writes != next_write || send->action_ordinal != next_ordinal ||
                      next_ordinal > executor::max_steps || next_group >= result.groups.size() ||
                      raw.size() > executor::max_total_inline_bytes - inline_bytes ||
                      raw.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes ||
                      group.depth >= context.max_inline_action_depth || group.depth >= executor::max_depth) return false;
                  const auto decoded = decode_isolated_inline_action(raw, context.max_inline_action_size);
                  if (!decoded || decoded->decoded_bytes != send->sent.decoded_bytes ||
                      !same_action(decoded->decoded, send->sent.decoded)) return false;
                  if (send->context_free && (!context.context_free_data || !decoded->decoded.authorization.empty()))
                     return false;
                  inline_bytes += raw.size(); data_bytes += raw.size();
                  const auto& child = result.groups[next_group];
                  if (child.root_index != group.root_index || child.action_ordinal != next_ordinal ||
                      child.creator_action_ordinal != step.action_ordinal ||
                      child.closest_unnotified_ancestor != group.action_ordinal || child.depth != group.depth + 1 ||
                      child.sender != step.receiver || child.context_free != send->context_free) return false;
                  isolated_action_input input;
                  input.receiver = input.account = decoded->decoded.account; input.action = decoded->decoded.name;
                  input.data = decoded->decoded.data; input.max_return_bytes = group.input.max_return_bytes;
                  for (const auto& p : decoded->decoded.authorization) input.authorizations.emplace_back(p.actor, p.permission);
                  if (!same_input(input, child.input)) return false;
                  (send->context_free ? context_free_children : children).push_back(next_group++); ++next_ordinal;
               } else return false;
            } catch (const std::exception&) {
               return false; // private reconstruction only; canonical state is untouched
            }
         }
         if (next_write != effects.writes_end || next_notification != effects.recipients.size()) return false;
      }
      for (const auto child : context_free_children) if (!replay_group(child)) return false;
      for (const auto child : children) if (!replay_group(child)) return false;
      return true;
   };
   for (size_t i = 0; i < roots.size(); ++i) if (!replay_group(i)) return false;
   if (next_step != result.steps.size() || next_group != result.groups.size() || next_write != result.state.writes().size()) return false;
   const auto reconstructed = replay.seal();
   if (reconstructed.writes().size() != result.state.writes().size()) return false;
   for (size_t i = 0; i < reconstructed.writes().size(); ++i) {
      check();
      const auto& actual = reconstructed.writes()[i]; const auto& expected = result.state.writes()[i];
      if (actual.table != expected.table || actual.primary_key != expected.primary_key || actual.payer != expected.payer ||
          actual.value != expected.value || actual.operation != expected.operation ||
          actual.object_id != expected.object_id || actual.table_object_id != expected.table_object_id) return false;
   }
   for (const auto& table : reconstructed.read_tables()) {
      check(); if (!result.state.read_tables().contains(table)) return false;
   }
   for (const auto& query : reconstructed.read_queries()) {
      check(); if (!result.state.read_queries().contains(query)) return false;
   }
   check();
   return true;
}

} // namespace eosio::chain::experimental
