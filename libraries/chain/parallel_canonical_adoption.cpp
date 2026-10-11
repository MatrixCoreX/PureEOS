#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/performance_metrics.hpp>
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
   return trx.actions.size() == 1 && trx.context_free_actions.empty() && trx.delay_sec.value == 0 &&
          trx.transaction_extensions.empty() && trx.actions[0].data.size() <= 64 * 1024 &&
          trx.actions[0].authorization.size() <= 64;
}
} // namespace

std::optional<prepared_isolated_action> prepare_isolated_action(const controller& control, const packed_transaction& packed) {
   const auto& trx = packed.get_transaction();
   if (!eligible_transaction(trx)) return {};
   const auto& act = trx.actions[0];
   if (act.account == config::system_account_name || act.account == "eosio.msig"_n || act.account == "eosio.wrap"_n ||
       control.find_apply_handler(act.account, act.account, act.name)) return {};
   const auto* account = control.db().find<account_metadata_object, by_name>(act.account);
   if (!account || account->is_privileged() || account->vm_type != 0 || account->vm_version != 0 || account->code_hash == fc::sha256{}) return {};
   const auto* code = control.db().find<code_object, by_code_hash>(boost::make_tuple(account->code_hash, 0, 0));
   if (!code || code->code.size() > 64 * 1024) return {};
   auto input = current_input(control, act);
   auto limits = current_limits(control);
   if (input.max_return_bytes > limits.max_return_bytes) return {};
   return prepared_isolated_action{{code->code.data(), code->code.data() + code->code.size()}, account->code_hash,
                                    std::move(input), limits};
}

bool try_adopt_primary_result(apply_context& context, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
      const isolated_wasm_limits& limits, const isolated_action_result& result,
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
       !eligible_transaction(context.trx_context.packed_trx.get_transaction())) return false;
   if (context.control.find_apply_handler(context.get_receiver(), context.get_action().account, context.get_action().name)) return false;
   const auto now_limits = current_limits(context.control);
   if (limits.max_pages != now_limits.max_pages || limits.max_call_depth != now_limits.max_call_depth) return false;
   auto input = current_input(context.control, context.get_action());
   if (isolated_execution_fingerprint(code_hash, limits, input) != result.execution_fingerprint ||
       result.return_value.size() > input.max_return_bytes) return false;

   chainbase_transaction_state_view live(context.db);
   performance::scope adoption_timer(performance::stage::parallel_adopt);
   // Defense in depth: preserve table metadata plus the exact selection and
   // complete original row value for EVERY observed query (including absence
   // and ranges). Unread rows no longer cause false value conflicts. Structural
   // changes still invalidate metadata/selection; writes remain update-only.
   // No state pointers from this check survive into the mutation loop.
   if (!result.state.matches_base_reads(live, [&] { context.trx_context.checktime(); })) return false;
   // A root-only write set cannot justify executing an untracked child WASM or
   // native handler. Recheck ALL recipients on the owner before changing any
   // canonical row or scheduling a trace. Empty-code notifications still run
   // exec_one and produce the original receipts, sequences and resource checks.
   if (result.recipients.size() > isolated_action_result::max_recipients) return false;
   std::set<name> notified{context.get_receiver()};
   size_t previous_write_count = 0;
   for (const auto& notification : result.recipients) {
      context.trx_context.checktime();
      if (notification.after_writes < previous_write_count || notification.after_writes > result.state.writes().size()) return false;
      previous_write_count = notification.after_writes;
      const auto recipient = notification.recipient;
      if (!notified.insert(recipient).second || !live.read_account(recipient) ||
          recipient == "eosio.msig"_n || recipient == "eosio.wrap"_n) return false;
      const auto* metadata = context.db.find<account_metadata_object, by_name>(recipient);
      if (!metadata || metadata->code_hash != fc::sha256{} ||
          context.control.find_apply_handler(recipient, context.get_action().account, context.get_action().name)) return false;
   }
   for (const auto& write : result.state.writes()) {
      if (write.table.code != context.get_receiver() || !result.state.read_tables().contains(write.table) ||
          !result.state.read_queries().contains({write.table, primary_query_kind::find, write.primary_key})) return false;
      const auto row = live.locate_primary({write.table, primary_query_kind::find, write.primary_key}).row;
      if (!row) return false;
   }
   auto return_value = result.return_value; // allocate before first canonical mutation
   if (!context.trx_context.explicit_billed_cpu_time)
      context.trx_context.charge_isolated_cpu(*production_elapsed);
   auto notification = result.recipients.begin();
   for (size_t written = 0; ; ++written) {
      // Preserve scheduling before/between/after updates, including when the
      // NEXT original host throws. Scheduling everything at the end changes
      // failed traces even though transaction undo restores the ledger.
      while (notification != result.recipients.end() && notification->after_writes == written) {
         context.require_recipient(notification->recipient);
         ++notification;
      }
      if (written == result.state.writes().size()) break;
      const auto& write = result.state.writes()[written];
      context.trx_context.checktime();
      // Replay EVERY update with the original host. It owns payer authorization,
      // positive/negative RAM deltas and resource checks inside transaction undo;
      // no worker-calculated resource state or coalesced final write is trusted.
      const auto iterator = context.db_find_i64(write.table.code, write.table.scope, write.table.table, write.primary_key);
      context.db_update_i64(iterator, write.payer, write.value.data(), write.value.size());
   }
   context.action_return_value = std::move(return_value);
   return true;
}

} // namespace eosio::chain::experimental
