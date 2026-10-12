#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/webassembly/interface.hpp>
#include <algorithm>

namespace eosio::chain::experimental {
namespace {
isolated_wasm_limits current_limits(const controller& control) {
   isolated_wasm_limits limits;
   uint32_t pages = wasm_constraints::maximum_linear_memory / wasm_constraints::wasm_page_size;
   uint32_t depth = wasm_constraints::maximum_call_depth;
   if (control.is_builtin_activated(builtin_protocol_feature_t::configurable_wasm_limits)) {
      const auto& config = control.get_global_properties().wasm_configuration;
      pages = config.max_pages; depth = config.max_call_depth;
   }
   limits.max_pages = std::min(limits.max_pages, pages);
   limits.max_call_depth = std::min(limits.max_call_depth, depth);
   return limits;
}
isolated_action_input current_input(const controller& control, const action& act) {
   isolated_action_input input;
   input.receiver = input.account = act.account;
   input.action = act.name; input.data = act.data;
   for (const auto& auth : act.authorization) input.authorizations.emplace_back(auth.actor, auth.permission);
   input.max_return_bytes = control.get_global_properties().configuration.max_action_return_value_size;
   return input;
}
bool eligible_transaction(const transaction& trx) {
   if (trx.actions.empty() || trx.actions.size() > isolated_transaction_executor::max_actions ||
       !trx.context_free_actions.empty() || trx.delay_sec.value != 0 || !trx.transaction_extensions.empty()) return false;
   size_t bytes = 0;
   for (const auto& act : trx.actions) {
      if (act.data.size() > 64 * 1024 || act.authorization.size() > 64 ||
          act.data.size() > isolated_transaction_executor::max_total_action_bytes - bytes) return false;
      bytes += act.data.size();
   }
   return true;
}

const account_metadata_object* eligible_account(const controller& control, const action& act) {
   if (act.account == config::system_account_name || act.account == "eosio.msig"_n || act.account == "eosio.wrap"_n ||
       control.find_apply_handler(act.account, act.account, act.name)) return nullptr;
   const auto* account = control.db().find<account_metadata_object, by_name>(act.account);
   if (!account || account->is_privileged() || account->vm_type != 0 || account->vm_version != 0 || account->code_hash == fc::sha256{}) return nullptr;
   return account;
}
} // namespace

std::optional<std::vector<prepared_isolated_action>> prepare_isolated_transaction(const controller& control, const packed_transaction& packed) {
   const auto& trx = packed.get_transaction();
   if (!eligible_transaction(trx)) return {};
   auto limits = current_limits(control);
   std::vector<prepared_isolated_action> actions;
   size_t code_bytes = 0;
   for (const auto& act : trx.actions) {
      const auto* account = eligible_account(control, act);
      if (!account) return {};
      const auto* code = control.db().find<code_object, by_code_hash>(boost::make_tuple(account->code_hash, 0, 0));
      if (!code || code->code.size() > 64 * 1024 ||
          code->code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return {};
      code_bytes += code->code.size();
      auto input = current_input(control, act);
      if (input.max_return_bytes > limits.max_return_bytes) return {};
      actions.push_back({{code->code.data(), code->code.data() + code->code.size()}, account->code_hash, std::move(input), limits});
   }
   return actions;
}

bool try_adopt_primary_result(apply_context& context, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
      const isolated_wasm_limits& limits, const isolated_transaction_result& result, size_t action_index,
      std::optional<fc::microseconds> production_elapsed) {
   // Non-explicit production must supply the successful private execution
   // duration. Never turn parallel work into free CPU or a fixed explicit bill.
   if ((!context.trx_context.explicit_billed_cpu_time && !production_elapsed) ||
       (production_elapsed && (production_elapsed->count() < 0 || *production_elapsed > fc::seconds(1))) ||
       context.trx_context.is_transient() ||
       context.trx_context.is_implicit() || context.trx_context.is_scheduled() || context.is_context_free() ||
       context.is_privileged() || vm_type != 0 || vm_version != 0 ||
       context.get_receiver() != context.get_action().account ||
       context.get_current_action_trace().creator_action_ordinal.value != 0 ||
       context.get_current_action_trace().action_ordinal.value != action_index + 1 ||
       !eligible_transaction(context.trx_context.packed_trx.get_transaction())) return false;
   const auto& trx = context.trx_context.packed_trx.get_transaction();
   if (result.actions.size() != trx.actions.size() || action_index >= result.actions.size()) return false;
   if (context.control.find_apply_handler(context.get_receiver(), context.get_action().account, context.get_action().name)) return false;
   const auto now_limits = current_limits(context.control);
   if (limits.max_pages != now_limits.max_pages || limits.max_call_depth != now_limits.max_call_depth) return false;
   auto input = current_input(context.control, context.get_action());
   const auto& effects = result.actions[action_index];
   if (isolated_execution_fingerprint(code_hash, limits, input) != effects.execution_fingerprint ||
       effects.return_value.size() > input.max_return_bytes) return false;

   chainbase_transaction_state_view live(context.db);
   performance::scope adoption_timer(performance::stage::parallel_adopt);
   // Defense in depth: preserve table metadata plus the exact selection and
   // complete original row value for EVERY observed query (including absence
   // and ranges). Structural changes invalidate metadata/selection, including
   // absence and skipped-neighbor proofs from private lifecycle operations.
   // No state pointers from this check survive into the mutation loop.
   if (action_index == 0) {
      if (!result.state.matches_base_reads(live, [&] { context.trx_context.checktime(); })) return false;
      // Root-only journals cannot justify an untracked child WASM/native
      // handler. Check ALL actions' recipients before changing any row or
      // scheduling a trace. Empty-code notifications still run exec_one and
      // produce original receipts, sequences and resource checks.
      std::vector<fc::sha256> fingerprints;
      size_t next_write = 0;
      fc::microseconds elapsed{};
      for (size_t i = 0; i < trx.actions.size(); ++i) {
         context.trx_context.checktime();
         const auto& act = trx.actions[i];
         const auto& step = result.actions[i];
         const auto* account = eligible_account(context.control, act);
         if (!account || step.return_value.size() > input.max_return_bytes || step.writes_begin != next_write ||
             step.writes_end < step.writes_begin || step.writes_end > result.state.writes().size() ||
             step.elapsed.count() < 0 || step.elapsed.count() > (fc::seconds(1) - elapsed).count()) return false;
         elapsed += fc::microseconds(step.elapsed.count());
         const auto fingerprint = isolated_execution_fingerprint(account->code_hash, limits, current_input(context.control, act));
         if (step.execution_fingerprint != fingerprint) return false;
         fingerprints.push_back(fingerprint);
         if (step.recipients.size() > isolated_action_result::max_recipients) return false;
         std::set<name> notified{act.account};
         size_t previous_write_count = step.writes_begin;
         for (const auto& notification : step.recipients) {
            context.trx_context.checktime();
            if (notification.after_writes < previous_write_count || notification.after_writes > step.writes_end) return false;
            previous_write_count = notification.after_writes;
            const auto recipient = notification.recipient;
            if (!notified.insert(recipient).second || !live.read_account(recipient) ||
                recipient == "eosio.msig"_n || recipient == "eosio.wrap"_n) return false;
            const auto* metadata = context.db.find<account_metadata_object, by_name>(recipient);
            if (!metadata || metadata->code_hash != fc::sha256{} ||
                context.control.find_apply_handler(recipient, act.account, act.name)) return false;
         }
         for (size_t w = step.writes_begin; w < step.writes_end; ++w) {
            const auto& write = result.state.writes()[w];
            if (write.table.code != act.account || !result.state.read_tables().contains(write.table) ||
                !result.state.read_queries().contains({write.table, primary_query_kind::find, write.primary_key})) return false;
            switch (write.operation) {
               case primary_operation::store: if (write.payer == name{}) return false; break;
               case primary_operation::update:
               case primary_operation::remove: break;
               default: return false;
            }
         }
         next_write = step.writes_end;
      }
      if (next_write != result.state.writes().size() ||
          isolated_transaction_fingerprint(fingerprints) != result.execution_fingerprint ||
          (production_elapsed && *production_elapsed != elapsed)) return false;
   }
   // Only primary writes and empty-code notifications can occur after this
   // preflight. Later steps use the already-proved shared journal, not a fresh
   // comparison of temporary row IDs against canonical prefix allocations.
   auto return_value = effects.return_value; // allocate before THIS action's first mutation
   if (!context.trx_context.explicit_billed_cpu_time)
      context.trx_context.charge_isolated_cpu(fc::microseconds(effects.elapsed.count()), action_index + 1);
   auto notification = effects.recipients.begin();
   for (size_t written = effects.writes_begin; ; ++written) {
      // Preserve scheduling before/between/after updates, including when the
      // NEXT original host throws. Scheduling everything at the end changes
      // failed traces even though transaction undo restores the ledger.
      while (notification != effects.recipients.end() && notification->after_writes == written) {
         context.require_recipient(notification->recipient);
         ++notification;
      }
      if (written == effects.writes_end) break;
      const auto& write = result.state.writes()[written];
      context.trx_context.checktime();
      // Replay EVERY update with the original host. It owns payer authorization,
      // positive/negative RAM deltas and resource checks inside transaction undo;
      // no worker-calculated resource state or coalesced final write is trusted.
      if (write.operation == primary_operation::store) {
         // Temporary worker IDs are never written into Chainbase. Original
         // hosts allocate table/row IDs and bill both fees in canonical order.
         context.db_store_i64(write.table.scope, write.table.table, write.payer, write.primary_key,
                              write.value.data(), write.value.size());
      } else {
         const auto iterator = context.db_find_i64(write.table.code, write.table.scope, write.table.table, write.primary_key);
         if (write.operation == primary_operation::remove) context.db_remove_i64(iterator);
         else context.db_update_i64(iterator, write.payer, write.value.data(), write.value.size());
      }
   }
   context.action_return_value = std::move(return_value);
   return true;
}

std::optional<isolated_receiver_program> prepare_isolated_receiver_program(const controller& control, name receiver,
      std::span<const isolated_action_input> roots) {
   if (roots.empty() || roots.size() > isolated_transaction_executor::max_actions) return {};
   for (const auto& root : roots)
      if (control.find_apply_handler(receiver, root.account, root.action)) return {};
   return prepare_isolated_tree_program(control, receiver);
}

std::optional<isolated_receiver_program> prepare_isolated_tree_program(const controller& control, name receiver) {
   if (!receiver || receiver == config::system_account_name || receiver == "eosio.msig"_n || receiver == "eosio.wrap"_n ||
       !control.db().find<account_object, by_name>(receiver)) return {};
   const auto* metadata = control.db().find<account_metadata_object, by_name>(receiver);
   if (!metadata || metadata->is_privileged() || metadata->vm_type != 0 || metadata->vm_version != 0) return {};
   isolated_receiver_program result{receiver, {}, current_limits(control)};
   if (metadata->code_hash != fc::sha256{}) {
      const auto* code = control.db().find<code_object, by_code_hash>(boost::make_tuple(metadata->code_hash, 0, 0));
      if (!code || code->code.size() > result.limits.max_code_bytes || code->code.size() == 0) return {};
      result.code.assign(code->code.data(), code->code.data() + code->code.size());
      if (fc::sha256::hash(result.code.data(), result.code.size()) != metadata->code_hash) return {};
   }
   return result;
}

isolated_action_tree_context current_action_tree_context(const controller& control) {
   const auto& configuration = control.get_global_properties().configuration;
   return {configuration.max_inline_action_size, configuration.max_inline_action_depth};
}

bool try_adopt_notification_result(apply_context& context, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
      const isolated_wasm_limits& limits, const isolated_notification_result& result, size_t step_index,
      std::optional<fc::microseconds> production_elapsed) {
   const auto& trx = context.trx_context.packed_trx.get_transaction();
   if ((!context.trx_context.explicit_billed_cpu_time && !production_elapsed) ||
       (production_elapsed && (production_elapsed->count() < 0 || *production_elapsed > fc::milliseconds(500))) ||
       context.trx_context.is_transient() || context.trx_context.is_implicit() || context.trx_context.is_scheduled() ||
       context.is_context_free() || context.is_privileged() || vm_type != 0 || vm_version != 0 ||
       !eligible_transaction(trx) || step_index >= result.steps.size() ||
       result.steps.size() > isolated_notification_executor::max_steps) return false;
   const auto& step = result.steps[step_index];
   const auto& trace = context.get_current_action_trace();
   if (step.root_index >= trx.actions.size() || step.receiver != context.get_receiver() || step.code_hash != code_hash ||
       step.has_code != (code_hash != fc::sha256{}) || trace.action_ordinal.value != step.action_ordinal ||
       trace.creator_action_ordinal.value != step.creator_action_ordinal ||
       trace.closest_unnotified_ancestor_action_ordinal.value != (step.creator_action_ordinal ? step.root_index + 1 : 0) ||
       context.control.find_apply_handler(context.get_receiver(), context.get_action().account, context.get_action().name)) return false;
   const auto now_limits = current_limits(context.control);
   if (limits.max_pages != now_limits.max_pages || limits.max_call_depth != now_limits.max_call_depth) return false;
   performance::scope adoption_timer(performance::stage::parallel_adopt);
   std::vector<char> return_value;
   try {
      auto arguments = current_input(context.control, context.get_action());
      arguments.receiver = context.get_receiver();
      if (isolated_execution_fingerprint(code_hash, limits, arguments) != step.effects.execution_fingerprint ||
          step.effects.return_value.size() > arguments.max_return_bytes) return false;
      if (step_index == 0) {
         const auto checkpoint = [&] { context.trx_context.checktime(); };
         std::vector<isolated_action_input> roots;
         for (const auto& act : trx.actions) roots.push_back(current_input(context.control, act));
         std::vector<isolated_receiver_program> programs;
         std::set<name> captured;
         size_t code_bytes = 0;
         fc::microseconds elapsed{};
         for (const auto& receiver : result.steps) {
            checkpoint();
            if (receiver.effects.elapsed.count() < 0 ||
                receiver.effects.elapsed.count() > (fc::milliseconds(500) - elapsed).count()) return false;
            elapsed += fc::microseconds(receiver.effects.elapsed.count());
            if (!captured.insert(receiver.receiver).second) continue;
            auto program = prepare_isolated_receiver_program(context.control, receiver.receiver, roots);
            if (!program || program->code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return false;
            code_bytes += program->code.size();
            programs.push_back(std::move(*program));
         }
         if (production_elapsed && *production_elapsed != elapsed) return false;
         if (!validate_isolated_notification_journal(result, roots, programs, checkpoint)) return false;
         chainbase_transaction_state_view live(context.db);
         if (!result.state.matches_base_reads(live, checkpoint)) return false;
      }
      return_value = step.effects.return_value; // allocate return storage before THIS receiver's first event
   } catch (const fc::exception&) {
      throw; // original checktime/interrupt exceptions must retain their meaning
   } catch (const std::exception&) {
      return false; // local preparation/allocation failed; NO canonical event has run
   }
   if (!context.trx_context.explicit_billed_cpu_time)
      context.trx_context.charge_isolated_notification_cpu(fc::microseconds(step.effects.elapsed.count()), step_index, step.action_ordinal);
   for (const auto& event : step.cursor_events) {
      context.trx_context.checktime();
      if (const auto* query = std::get_if<isolated_cursor_query>(&event)) {
         using kind = isolated_cursor_query_kind;
         int32_t actual = -1;
         auto primary = query->primary_before;
         switch (query->kind) {
            case kind::find: actual = context.db_find_i64(query->table.code, query->table.scope, query->table.table, query->key); break;
            case kind::lower_bound: actual = context.db_lowerbound_i64(query->table.code, query->table.scope, query->table.table, query->key); break;
            case kind::upper_bound: actual = context.db_upperbound_i64(query->table.code, query->table.scope, query->table.table, query->key); break;
            case kind::end: actual = context.db_end_i64(query->table.code, query->table.scope, query->table.table); break;
            case kind::next: actual = context.db_next_i64(query->iterator, primary); break;
            case kind::previous: actual = context.db_previous_i64(query->iterator, primary); break;
         }
         // Whole-tree preflight proves these values before ANY write. This is
         // an internal invariant, never a local-capacity or eligibility test.
         EOS_ASSERT(actual == query->result &&
            ((query->kind != kind::next && query->kind != kind::previous) || primary == query->primary_after),
            transaction_exception, "prevalidated notification cursor mismatch");
      } else if (const auto* mutation = std::get_if<isolated_cursor_write>(&event)) {
         const auto& write = result.state.writes()[mutation->write_index];
         if (write.operation == primary_operation::store) {
            const auto actual = context.db_store_i64(write.table.scope, write.table.table, write.payer, write.primary_key,
                                                     write.value.data(), write.value.size());
            EOS_ASSERT(actual == mutation->iterator, transaction_exception, "prevalidated notification store cursor mismatch");
         } else if (write.operation == primary_operation::update) {
            context.db_update_i64(mutation->iterator, write.payer, write.value.data(), write.value.size());
         } else {
            context.db_remove_i64(mutation->iterator);
         }
      } else {
         context.require_recipient(step.effects.recipients[std::get<isolated_cursor_notify>(event).notification_index].recipient);
      }
   }
   context.action_return_value = std::move(return_value);
   return true;
}

bool try_adopt_action_tree_result(apply_context& context, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
      const isolated_wasm_limits& limits, const isolated_action_tree_result& result, size_t step_index,
      std::optional<fc::microseconds> production_elapsed) {
   const auto& trx = context.trx_context.packed_trx.get_transaction();
   if ((!context.trx_context.explicit_billed_cpu_time && !production_elapsed) ||
       (production_elapsed && (production_elapsed->count() < 0 || *production_elapsed > fc::milliseconds(500))) ||
       context.trx_context.is_transient() || context.trx_context.is_implicit() || context.trx_context.is_scheduled() ||
       context.control.skip_trx_checks() || context.is_privileged() || vm_type != 0 || vm_version != 0 ||
       !eligible_transaction(trx) || step_index >= result.steps.size() ||
       result.steps.size() > isolated_action_tree_executor::max_steps ||
       result.groups.size() > isolated_action_tree_executor::max_steps) return false;
   const auto& step = result.steps[step_index];
   if (step.group_index >= result.groups.size()) return false;
   const auto& group = result.groups[step.group_index];
   if (group.context_free != context.is_context_free()) return false;
   // Inspect, but never retain a trace/act reference across the event loop:
   // ORIGINAL send_inline/require_recipient may grow the trace vector.
   {
      const auto& trace = context.get_current_action_trace();
      if (group.root_index >= trx.actions.size() || step.receiver != context.get_receiver() || step.code_hash != code_hash ||
          step.has_code != (code_hash != fc::sha256{}) || trace.action_ordinal.value != step.action_ordinal ||
          trace.creator_action_ordinal.value != step.creator_action_ordinal ||
          trace.closest_unnotified_ancestor_action_ordinal.value != step.closest_unnotified_ancestor ||
          context.get_inline_depth() != group.depth || context.get_first_receiver_action_ordinal() != group.action_ordinal ||
          context.get_sender() != step.sender ||
          context.control.find_apply_handler(context.get_receiver(), context.get_action().account, context.get_action().name)) return false;
   }
   const auto now_limits = current_limits(context.control);
   auto tree_context = current_action_tree_context(context.control);
   tree_context.context_free_data = result.context_free_data;
   if (limits.max_pages != now_limits.max_pages || limits.max_call_depth != now_limits.max_call_depth) return false;
   performance::scope adoption_timer(performance::stage::parallel_adopt);
   std::vector<char> return_value;
   try {
      auto arguments = current_input(context.control, context.get_action());
      arguments.receiver = context.get_receiver();
      if (isolated_action_tree_step_fingerprint(isolated_execution_fingerprint(code_hash, limits, arguments),
             tree_context, group, step) != step.effects.execution_fingerprint ||
          step.effects.return_value.size() > arguments.max_return_bytes) return false;
      if (step_index == 0) {
         const auto checkpoint = [&] { context.trx_context.checktime(); };
         // The result owns a const copy; verify against the ACTUAL packed input
         // once, before any canonical event. Later steps bind the same immutable
         // digest, without copying/hashing all data again at each receiver.
         if (result.context_free_data) {
            checkpoint();
            const auto actual = isolated_context_free_data::capture(context.trx_context.packed_trx.get_context_free_data());
            if (actual->fingerprint() != result.context_free_data->fingerprint()) return false;
         }
         std::vector<isolated_action_input> roots;
         for (const auto& act : trx.actions) roots.push_back(current_input(context.control, act));
         std::vector<isolated_receiver_program> programs;
         std::set<name> captured;
         size_t code_bytes = 0;
         fc::microseconds elapsed{};
         for (const auto& receiver : result.steps) {
            checkpoint();
            if (receiver.group_index >= result.groups.size()) return false;
            const auto& call = result.groups[receiver.group_index].input;
            // Check EVERY dynamic pair, even if this receiver's code was already
            // captured for a different root/inline. A name-only catalog is not
            // proof that a different action has no native implementation.
            if (context.control.find_apply_handler(receiver.receiver, call.account, call.action)) return false;
            if (receiver.effects.elapsed.count() < 0 ||
                receiver.effects.elapsed.count() > (fc::milliseconds(500) - elapsed).count()) return false;
            elapsed += fc::microseconds(receiver.effects.elapsed.count());
            if (!captured.insert(receiver.receiver).second) continue;
            auto program = prepare_isolated_tree_program(context.control, receiver.receiver);
            if (!program || program->code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return false;
            code_bytes += program->code.size();
            programs.push_back(std::move(*program));
         }
         if (production_elapsed && *production_elapsed != elapsed) return false;
         if (!validate_isolated_action_tree_journal(result, roots, programs, tree_context, checkpoint)) return false;
         chainbase_transaction_state_view live(context.db);
         if (!result.state.matches_base_reads(live, checkpoint)) return false;
      }
      return_value = step.effects.return_value;
   } catch (const fc::exception&) {
      throw; // retain original time/interrupt exceptions
   } catch (const std::exception&) {
      return false; // NO event in this step has run; prefix remains canonical
   }
   if (!context.trx_context.explicit_billed_cpu_time)
      context.trx_context.charge_isolated_tree_cpu(fc::microseconds(step.effects.elapsed.count()), step_index, step.action_ordinal);
   for (const auto& event : step.events) {
      context.trx_context.checktime();
      if (const auto* send = std::get_if<isolated_tree_inline_event>(&event)) {
         // legacy_span's historical constructor takes void*. const char and
         // alignment 1 mean a read-only view, no copy-back or mutation of bytes.
         auto bytes = legacy_span<const char>{const_cast<char*>(send->sent.packed.data()),
                                              static_cast<uint32_t>(send->sent.packed.size())};
         if (send->context_free) webassembly::interface(context).send_context_free_inline(std::move(bytes));
         else webassembly::interface(context).send_inline(std::move(bytes));
         EOS_ASSERT(context.trx_context.trace->action_traces.size() == send->action_ordinal,
                    transaction_exception, "prevalidated inline schedule mismatch");
         continue;
      }
      const auto& cursor = std::get<isolated_cursor_event>(event);
      if (const auto* query = std::get_if<isolated_cursor_query>(&cursor)) {
         using kind = isolated_cursor_query_kind;
         int32_t actual = -1;
         auto primary = query->primary_before;
         switch (query->kind) {
            case kind::find: actual = context.db_find_i64(query->table.code, query->table.scope, query->table.table, query->key); break;
            case kind::lower_bound: actual = context.db_lowerbound_i64(query->table.code, query->table.scope, query->table.table, query->key); break;
            case kind::upper_bound: actual = context.db_upperbound_i64(query->table.code, query->table.scope, query->table.table, query->key); break;
            case kind::end: actual = context.db_end_i64(query->table.code, query->table.scope, query->table.table); break;
            case kind::next: actual = context.db_next_i64(query->iterator, primary); break;
            case kind::previous: actual = context.db_previous_i64(query->iterator, primary); break;
         }
         EOS_ASSERT(actual == query->result &&
            ((query->kind != kind::next && query->kind != kind::previous) || primary == query->primary_after),
            transaction_exception, "prevalidated tree cursor mismatch");
      } else if (const auto* mutation = std::get_if<isolated_cursor_write>(&cursor)) {
         const auto& write = result.state.writes()[mutation->write_index];
         if (write.operation == primary_operation::store) {
            const auto actual = context.db_store_i64(write.table.scope, write.table.table, write.payer, write.primary_key,
                                                     write.value.data(), write.value.size());
            EOS_ASSERT(actual == mutation->iterator, transaction_exception, "prevalidated tree store cursor mismatch");
         } else if (write.operation == primary_operation::update) {
            context.db_update_i64(mutation->iterator, write.payer, write.value.data(), write.value.size());
         } else {
            context.db_remove_i64(mutation->iterator);
         }
      } else {
         context.require_recipient(step.effects.recipients[std::get<isolated_cursor_notify>(cursor).notification_index].recipient);
      }
   }
   context.action_return_value = std::move(return_value);
   return true;
}

} // namespace eosio::chain::experimental
