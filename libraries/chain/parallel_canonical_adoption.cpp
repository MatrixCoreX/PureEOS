#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/global_property_object.hpp>
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
      const isolated_wasm_limits& limits, const isolated_action_result& result) {
   // Validation first. Production CPU charging has a separate, not-yet-enabled
   // gate; private elapsed time must not silently disappear from producer bills.
   if (!context.trx_context.explicit_billed_cpu_time || context.trx_context.is_transient() ||
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
   // Defense in depth in addition to the ordered coordinator. Compare complete
   // bounded logical tables, including absent keys/ranges and object identities.
   // No state pointers from this check survive into the mutation loop.
   for (const auto& table : result.state.read_tables()) {
      context.trx_context.checktime();
      if (live.read_table(table) != result.state.base()->read_table(table)) return false;
      primary_query query{table, primary_query_kind::lower_bound, 0};
      while (true) {
         const auto expected = result.state.base()->locate_primary(query);
         if (live.locate_primary(query) != expected) return false;
         if (!expected.row) break;
         if (live.read_primary(query, expected.row->value_size) != result.state.base()->read_primary(query, expected.row->value_size)) return false;
         context.trx_context.checktime();
         query.kind = primary_query_kind::upper_bound; query.primary_key = expected.row->primary_key;
      }
   }
   for (const auto& write : result.state.writes()) {
      if (write.table.code != context.get_receiver() || !result.state.read_tables().contains(write.table)) return false;
      const auto row = live.locate_primary({write.table, primary_query_kind::find, write.primary_key}).row;
      if (!row || row->payer != write.payer || row->value_size != write.value.size()) return false;
   }
   auto return_value = result.return_value; // allocate before first canonical mutation
   for (const auto& write : result.state.writes()) {
      context.trx_context.checktime();
      const auto iterator = context.db_find_i64(write.table.code, write.table.scope, write.table.table, write.primary_key);
      context.db_update_i64(iterator, write.payer, write.value.data(), write.value.size());
   }
   context.action_return_value = std::move(return_value);
   return true;
}

} // namespace eosio::chain::experimental
