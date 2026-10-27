#include <map>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/parallel_pure_float.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/sync_call_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/webassembly/interface.hpp>
#include <fc/scoped_exit.hpp>
#include <algorithm>
#include <utility>

namespace eosio::chain::experimental {
namespace {
void replay_original_console(host_context& context, const isolated_console_event& event, size_t writes) {
   EOS_ASSERT(context.control.contracts_console() && event.after_writes == writes &&
              isolated_console_budget::argument_bytes(event), transaction_exception, "prevalidated console event mismatch");
   webassembly::interface host(context);
   using op = isolated_console_operation;
   // Stable owned arguments only. The original host owns formatting and trace
   // append, including locale/SoftFloat behaviour; workers never print to logs.
   char empty = 0;
   auto* bytes = event.bytes.empty() ? &empty : const_cast<char*>(event.bytes.data());
   switch (event.operation) {
      case op::prints: host.prints(null_terminated_ptr{bytes}); break;
      case op::prints_l: host.prints_l(legacy_span<const char>{bytes, uint32_t(event.bytes.size())}); break;
      case op::printhex: host.printhex(legacy_span<const char>{bytes, uint32_t(event.bytes.size())}); break;
      case op::printi: host.printi(std::bit_cast<int64_t>(event.bits[0])); break;
      case op::printui: host.printui(event.bits[0]); break;
      case op::printn: host.printn(name{event.bits[0]}); break;
      case op::printsf: host.printsf(float32_t{uint32_t(event.bits[0])}); break;
      case op::printdf: host.printdf(float64_t{event.bits[0]}); break;
      case op::printi128: {
         auto value = std::bit_cast<__int128>(event.bits);
         host.printi128(legacy_ptr<const __int128>{&value}); break;
      }
      case op::printui128: {
         auto value = std::bit_cast<unsigned __int128>(event.bits);
         host.printui128(legacy_ptr<const unsigned __int128>{&value}); break;
      }
      case op::printqf: {
         auto value = std::bit_cast<float128_t>(event.bits);
         host.printqf(legacy_ptr<const float128_t>{&value}); break;
      }
      default: EOS_ASSERT(false, transaction_exception, "prevalidated unknown console operation");
   }
}

// Each action/sync frame owns its console. Do not hold trace references across
// child calls or inline scheduling: those operations may grow trace vectors.
size_t original_console_size(host_context& context, uint32_t ordinal) {
   return ordinal ? context.get_call_trace(ordinal).console.size() : context.get_current_action_trace().console.size();
}
void capture_original_console_marker(host_context& context, uint32_t ordinal, std::span<const uint32_t> markers,
                                     uint32_t console_events, std::vector<uint32_t>& expected) {
   if (!context.control.contracts_console()) return;
   size_t next = expected.size();
   EOS_ASSERT(accept_isolated_console_marker(markers, next, console_events, true),
              transaction_exception, "prevalidated console event position mismatch");
   const auto bytes = original_console_size(context, ordinal);
   EOS_ASSERT(bytes <= UINT32_MAX, transaction_exception, "prevalidated console byte bound mismatch");
   expected.push_back(static_cast<uint32_t>(bytes));
}
void check_original_console_markers(host_context& context, uint32_t ordinal, std::span<const uint32_t> markers,
                                   std::span<const uint32_t> expected) {
   const auto& actual = ordinal ? context.get_call_trace(ordinal).console_markers :
                                  context.get_current_action_trace().console_markers;
   EOS_ASSERT(actual.size() == expected.size() && expected.size() == markers.size(),
              transaction_exception, "prevalidated console marker count mismatch");
   for (size_t i = 0; i < expected.size(); ++i)
      EOS_ASSERT(actual[i].value == expected[i], transaction_exception, "prevalidated console byte position mismatch");
}

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
isolated_action_input current_input(const controller& control, const action& act,
      std::shared_ptr<const isolated_transaction_environment> environment = {}) {
   isolated_action_input input;
   input.receiver = input.account = act.account;
   input.action = act.name; input.data = act.data;
   for (const auto& auth : act.authorization) input.authorizations.emplace_back(auth.actor, auth.permission);
   input.max_return_bytes = control.get_global_properties().configuration.max_action_return_value_size;
   input.environment = std::move(environment);
   return input;
}
// No environment means the old fingerprint/eligibility behavior. A captured
// environment is checked against the actual current input and block, never
// against another field copied from the same private result.
bool current_environment_matches(const controller& control, const packed_transaction& packed,
      fc::time_point published, const std::shared_ptr<const isolated_transaction_environment>& environment) {
   return !environment || environment->matches(packed.id(),
      static_cast<uint64_t>(control.pending_block_time().time_since_epoch().count()),
      static_cast<uint64_t>(published.time_since_epoch().count()), control.pending_block_num());
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

// Replay ONLY a fully proved transcript. Do not perform extra find operations
// here: they would allocate original iterators and change later handle values.
// The complete private proof plus live base-read validation binds each handle
// to its row; the original hosts retain ownership, RAM and cache invalidation.
template<class Native>
Native original_secondary_key(const secondary_key& key) {
   if constexpr (std::is_same_v<Native, float64_t>) return {std::get<secondary_float64>(key).bits};
   else if constexpr (std::is_same_v<Native, float128_t>) {
      Native result;
      fc::uint128_to_float128(std::get<secondary_float128>(key).bits, result);
      return result;
   } else return std::get<Native>(key);
}
template<class Native>
secondary_key owned_secondary_key(const Native& key) {
   if constexpr (std::is_same_v<Native, float64_t>) return secondary_float64{key.v};
   else if constexpr (std::is_same_v<Native, float128_t>) {
      secondary_float128 result;
      fc::float128_to_uint128(key, result.bits);
      return result;
   } else return key;
}
template<class Native>
decltype(auto) original_secondary_proxy(Native& key) {
   if constexpr (std::is_same_v<Native, std::array<uint128_t, 2>>) return key.data();
   else return (key);
}
template<class Replay>
void with_original_secondary_index(host_context& context, secondary_key_kind kind, Replay&& replay) {
   switch (kind) {
      case secondary_key_kind::uint64: replay(context.idx64); return;
      case secondary_key_kind::uint128: replay(context.idx128); return;
      case secondary_key_kind::uint256: replay(context.idx256); return;
      case secondary_key_kind::float64: replay(context.idx_double); return;
      case secondary_key_kind::float128: replay(context.idx_long_double); return;
   }
   EOS_ASSERT(false, transaction_exception, "prevalidated secondary index kind mismatch");
}
void replay_original_secondary(host_context& context, const isolated_secondary_cursor_event& event,
      const primary_overlay_result& state, size_t& next_write, size_t writes_end) {
   // These checks normally live in WASM registration, above generic_index.
   EOS_ASSERT(!context.is_context_free(), unaccessible_api, "only context free api's can be used in this context");
   const auto check_key = [](const secondary_key& key) {
      EOS_ASSERT(!secondary_key_is_nan(key), transaction_exception, "NaN is not an allowed value for a secondary key");
   };
   std::visit([&](const auto& operation) {
      using event_type = std::decay_t<decltype(operation)>;
      const auto kind = [&] {
         if constexpr (std::is_same_v<event_type, isolated_secondary_find_primary> ||
                       std::is_same_v<event_type, isolated_secondary_bound>) return secondary_kind(operation.key_before);
         else if constexpr (std::is_same_v<event_type, isolated_secondary_find_secondary>) return secondary_kind(operation.key);
         else return operation.kind;
      }();
      with_original_secondary_index(context, kind, [&](auto& index) {
         using native_key = typename std::decay_t<decltype(index)>::secondary_key_type;
         if constexpr (std::is_same_v<event_type, isolated_secondary_write>) {
            EOS_ASSERT(!context.is_read_only(), unaccessible_api, "this API is not allowed in read only action/call");
            EOS_ASSERT(operation.write_index == next_write && next_write < writes_end && next_write < state.writes().size(),
                       transaction_exception, "prevalidated secondary mutation order mismatch");
            const auto* write = std::get_if<secondary_mutation>(&state.writes()[next_write]);
            EOS_ASSERT(write && secondary_kind(write->key) == kind && write->table.code == context.get_receiver(),
                       transaction_exception, "prevalidated secondary mutation identity mismatch");
            if (write->operation == primary_operation::remove) index.remove(operation.iterator);
            else {
               check_key(write->key);
               auto key = original_secondary_key<native_key>(write->key);
               if (write->operation == primary_operation::store) {
                  const auto actual = index.store(write->table.scope.to_uint64_t(), write->table.table.to_uint64_t(),
                     write->payer, write->primary_key, original_secondary_proxy(key));
                  EOS_ASSERT(actual == operation.iterator, transaction_exception, "prevalidated secondary store cursor mismatch");
               } else if (write->operation == primary_operation::update) {
                  index.update(operation.iterator, write->payer, original_secondary_proxy(key));
               } else EOS_ASSERT(false, transaction_exception, "prevalidated secondary operation mismatch");
            }
            ++next_write;
         } else {
            int32_t actual = -1;
            if constexpr (std::is_same_v<event_type, isolated_secondary_find_primary>) {
               auto key = original_secondary_key<native_key>(operation.key_before);
               actual = index.find_primary(operation.table.code.to_uint64_t(), operation.table.scope.to_uint64_t(),
                  operation.table.table.to_uint64_t(), original_secondary_proxy(key), operation.primary);
               // find_primary's key is an output, so even NaN sentinel bits are valid.
               EOS_ASSERT(owned_secondary_key(key) == operation.key_after, transaction_exception, "prevalidated secondary output mismatch");
            } else if constexpr (std::is_same_v<event_type, isolated_secondary_find_secondary>) {
               check_key(operation.key);
               auto key = original_secondary_key<native_key>(operation.key);
               auto primary = operation.primary_before;
               actual = index.find_secondary(operation.table.code.to_uint64_t(), operation.table.scope.to_uint64_t(),
                  operation.table.table.to_uint64_t(), original_secondary_proxy(key), primary);
               EOS_ASSERT(primary == operation.primary_after, transaction_exception, "prevalidated secondary primary mismatch");
            } else if constexpr (std::is_same_v<event_type, isolated_secondary_bound>) {
               check_key(operation.key_before);
               auto key = original_secondary_key<native_key>(operation.key_before);
               auto primary = operation.primary_before;
               const auto& table = operation.table;
               actual = operation.upper ? index.upperbound_secondary(table.code.to_uint64_t(), table.scope.to_uint64_t(),
                  table.table.to_uint64_t(), original_secondary_proxy(key), primary) :
                  index.lowerbound_secondary(table.code.to_uint64_t(), table.scope.to_uint64_t(),
                     table.table.to_uint64_t(), original_secondary_proxy(key), primary);
               EOS_ASSERT(primary == operation.primary_after && owned_secondary_key(key) == operation.key_after,
                          transaction_exception, "prevalidated secondary bound output mismatch");
            } else if constexpr (std::is_same_v<event_type, isolated_secondary_end>) {
               actual = index.end_secondary(operation.table.code.to_uint64_t(), operation.table.scope.to_uint64_t(),
                  operation.table.table.to_uint64_t());
            } else if constexpr (std::is_same_v<event_type, isolated_secondary_adjacent>) {
               auto primary = operation.primary_before;
               actual = operation.previous ? index.previous_secondary(operation.iterator, primary) :
                                             index.next_secondary(operation.iterator, primary);
               EOS_ASSERT(primary == operation.primary_after, transaction_exception, "prevalidated secondary adjacent output mismatch");
            }
            EOS_ASSERT(actual == operation.result, transaction_exception, "prevalidated secondary cursor mismatch");
         }
      });
   }, event);
}
} // namespace

// Immediate,synchronous owner-only hashing. No borrowed reference escapes.
// The existing owned-input function remains the independent byte oracle.
fc::sha256 isolated_execution_fingerprint_from_action(const fc::sha256& code,const isolated_wasm_limits& limits,
      name receiver,const action& act,uint32_t max_return,
      const std::shared_ptr<const isolated_transaction_environment>& environment) {
   fc::sha256::encoder encoder;
   constexpr char domain[]="pureeos/isolated-action/v1";
   encoder.write(domain,sizeof(domain));
   fc::raw::pack(encoder,code);fc::raw::pack(encoder,limits.max_pages);
   fc::raw::pack(encoder,limits.max_call_depth);fc::raw::pack(encoder,receiver);
   fc::raw::pack(encoder,act.account);fc::raw::pack(encoder,act.name);
   fc::raw::pack(encoder,act.authorization);fc::raw::pack(encoder,act.data);
   fc::raw::pack(encoder,max_return);
   if(environment) {
      constexpr char extension[]="pureeos/isolated-action-environment/v1";
      encoder.write(extension,sizeof(extension));fc::raw::pack(encoder,environment->fingerprint());
   }
   return encoder.result();
}

std::optional<std::vector<prepared_isolated_root>> prepare_isolated_transaction_roots(
      const controller& control, const packed_transaction& packed, const std::function<void()>& checkpoint) {
   if (!isolated_numeric_environment_compatible()) return {};
   const auto& trx = packed.get_transaction();
   if (!eligible_transaction(trx)) return {};
   const auto limits = current_limits(control);
   std::vector<prepared_isolated_root> roots;
   roots.reserve(trx.actions.size()); // bounded by eligible_transaction
   for (const auto& act : trx.actions) {
      if (checkpoint) checkpoint();
      // Recheck EVERY (account, account, action) native-handler pair, including
      // later actions of the same receiver. Receiver deduplication is code-only.
      const auto* account = eligible_account(control, act);
      if (!account) return {};
      const auto* code = control.db().find<code_object, by_code_hash>(boost::make_tuple(account->code_hash, 0, 0));
      if (!code || code->code.size() == 0 || code->code.size() > limits.max_code_bytes) return {};
      auto input = current_input(control, act);
      if (input.max_return_bytes > limits.max_return_bytes) return {};
      roots.push_back({account->code_hash, std::move(input), limits});
   }
   if (checkpoint) checkpoint();
   return roots;
}

std::optional<std::vector<prepared_isolated_action>> prepare_isolated_transaction(const controller& control, const packed_transaction& packed) {
   if (!isolated_numeric_environment_compatible()) return {};
   const auto& trx = packed.get_transaction();
   if (!eligible_transaction(trx)) return {};
   auto limits = current_limits(control);
   std::vector<prepared_isolated_action> actions;
   size_t code_bytes = 0;
   for (const auto& act : trx.actions) {
      const auto* account = eligible_account(control, act);
      if (!account) return {};
      const auto* code = control.db().find<code_object, by_code_hash>(boost::make_tuple(account->code_hash, 0, 0));
      if (!code || code->code.size() > limits.max_code_bytes ||
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
   if (!isolated_numeric_environment_compatible() || !result.state.primary_only()) return false;
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
      fingerprints.reserve(trx.actions.size());
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
         small_seen_keys<name> notified{act.account};
         size_t previous_write_count = step.writes_begin;
         for (const auto& notification : step.recipients) {
            context.trx_context.checktime();
            if (notification.after_writes < previous_write_count || notification.after_writes > step.writes_end) return false;
            previous_write_count = notification.after_writes;
            const auto recipient = notification.recipient;
            if (!notified.insert(recipient) || !live.read_account(recipient) ||
                recipient == "eosio.msig"_n || recipient == "eosio.wrap"_n) return false;
            const auto* metadata = context.db.find<account_metadata_object, by_name>(recipient);
            if (!metadata || metadata->code_hash != fc::sha256{} ||
                context.control.find_apply_handler(recipient, act.account, act.name)) return false;
         }
         for (size_t w = step.writes_begin; w < step.writes_end; ++w) {
            const auto& write = result.state.primary_write(w);
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
      const auto& write = result.state.primary_write(written);
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

std::optional<isolated_receiver_program> prepare_isolated_tree_program(const controller& control, name receiver,
      size_t owned_code_budget) {
   if (!receiver || receiver == config::system_account_name || receiver == "eosio.msig"_n || receiver == "eosio.wrap"_n ||
       !control.db().find<account_object, by_name>(receiver)) return {};
   const auto* metadata = control.db().find<account_metadata_object, by_name>(receiver);
   if (!metadata || metadata->is_privileged() || metadata->vm_type != 0 || metadata->vm_version != 0) return {};
   isolated_receiver_program result{receiver, {}, current_limits(control)};
   if (metadata->code_hash != fc::sha256{}) {
      const auto* code = control.db().find<code_object, by_code_hash>(boost::make_tuple(metadata->code_hash, 0, 0));
      if (!code || code->code.size() > result.limits.max_code_bytes || code->code.size() == 0 ||
          code->code.size() > owned_code_budget) return {};
      // Code objects are content-addressed and their bytes are immutable.
      // Cache only owned bytes, never database/controller/VM pointers. Current
      // account privilege, VM version, code hash, size and limits are rechecked
      // above on EVERY call, including after setcode, fork undo and snapshot.
      struct byte_cache {
         std::map<fc::sha256, experimental::immutable_code> values;
         std::vector<fc::sha256> fifo;
         size_t bytes=0;
      };
      static thread_local byte_cache cached;
      const auto found=cached.values.find(metadata->code_hash);
      if(found!=cached.values.end()) {
         if(found->second.size()!=code->code.size()) return {};
         result.code=found->second;
      } else {
         result.code.assign(code->code.data(),code->code.data()+code->code.size());
         if(result.code.content_hash()!=metadata->code_hash) return {};
         constexpr size_t max_cached_bytes=4*1024*1024,max_cached_entries=16;
         if(result.code.size()<=max_cached_bytes) {
            while(!cached.fifo.empty() && (cached.values.size()>=max_cached_entries ||
                  result.code.size()>max_cached_bytes-cached.bytes)) {
               const auto old=cached.values.find(cached.fifo.front());
               cached.bytes-=old->second.size();cached.values.erase(old);
               cached.fifo.erase(cached.fifo.begin());
            }
            // Reserve before publication; allocation failure is still a local
            // preparation fallback and cannot corrupt the bounded cache.
            cached.fifo.reserve(max_cached_entries);
            cached.values.emplace(metadata->code_hash,result.code);
            cached.fifo.push_back(metadata->code_hash);cached.bytes+=result.code.size();
         }
      }
      if(result.code.content_hash()!=metadata->code_hash) return {};
   }
   return result;
}

isolated_action_tree_context current_action_tree_context(const controller& control) {
   const auto& configuration = control.get_global_properties().configuration;
   return {configuration.max_inline_action_size, configuration.max_inline_action_depth, {}, control.contracts_console()};
}

std::optional<isolated_sync_program> prepare_isolated_sync_program(const controller& control, name receiver) {
   auto program = prepare_isolated_tree_program(control, receiver);
   if (!program) return {};
   bool supported = false;
   if (!program->code.empty()) {
      const auto& account = control.db().get<account_metadata_object, by_name>(receiver);
      const auto* code = control.db().find<code_object, by_code_hash>(
         boost::make_tuple(account.code_hash, account.vm_type, account.vm_version));
      if (!code) return {};
      supported = code->sync_call_supported;
   }
   return isolated_sync_program{std::move(*program), supported};
}

isolated_sync_context current_sync_context(const controller& control) {
   const auto& config = control.get_global_properties().configuration;
   return {config.max_sync_call_depth, config.max_sync_call_data_size, false,
      control.is_builtin_activated(builtin_protocol_feature_t::get_sender_fix), control.contracts_console()};
}

namespace {
bool sync_catalog_matches_current(const controller& control, std::span<const isolated_sync_program> programs,
      const std::function<void()>& check) {
   if (programs.empty() || programs.size() > isolated_sync_executor::max_programs) return false;
   small_seen_keys<name> seen;
   size_t code_bytes = 0;
   for (const auto& captured : programs) {
      check();
      const auto& program = captured.program;
      if (program.serial_only || !seen.insert(program.receiver) ||
          program.code.size() > isolated_sync_executor::max_total_code_bytes - code_bytes) return false;
      code_bytes += program.code.size();
      // Never recapture only executed children: the execution fingerprint also
      // binds unused catalog entries and their order. No canonical pointers
      // from this temporary comparison survive into eventual replay.
      auto current = prepare_isolated_sync_program(control, program.receiver);
      if (!current || current->program.code != program.code ||
          current->sync_call_supported != captured.sync_call_supported ||
          current->program.limits != program.limits) return false;
   }
   return true;
}
} // namespace

bool validate_isolated_sync_current_state(const controller& control, const packed_transaction& packed,
      std::span<const isolated_sync_program> programs, const isolated_sync_result& result,
      const std::function<void()>& checkpoint) {
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   if (!isolated_numeric_environment_compatible()) return false;
   const auto& trx = packed.get_transaction();
   if (!control.is_builtin_activated(builtin_protocol_feature_t::sync_call) ||
       !eligible_transaction(trx) || trx.actions.size() != 1 || !eligible_account(control, trx.actions.front()) ||
       !sync_catalog_matches_current(control, programs, check)) return false;
   if (!current_environment_matches(control, packed, control.pending_block_time(), result.environment)) return false;
   const auto input = current_input(control, trx.actions.front(), result.environment);
   if (!validate_isolated_sync_journal(result, input, programs, current_sync_context(control), checkpoint)) return false;
   chainbase_transaction_state_view live(control.db());
   return result.state.matches_base_reads(live, check) && isolated_numeric_environment_compatible();
}

bool validate_isolated_sync_transaction_current_state(const controller& control, const packed_transaction& packed,
      std::span<const isolated_sync_program> programs, const isolated_sync_transaction_result& result,
      const std::function<void()>& checkpoint) {
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   if (!isolated_numeric_environment_compatible()) return false;
   const auto& trx = packed.get_transaction();
   if (!control.is_builtin_activated(builtin_protocol_feature_t::sync_call) || !eligible_transaction(trx) ||
       trx.actions.size() > isolated_sync_executor::max_roots || result.roots.size() != trx.actions.size() ||
       !sync_catalog_matches_current(control, programs, check)) return false;
   if (!current_environment_matches(control, packed, control.pending_block_time(), result.environment)) return false;
   std::vector<isolated_action_input> roots;
   roots.reserve(trx.actions.size());
   for (const auto& act : trx.actions) {
      check();
      if (!eligible_account(control, act)) return false;
      roots.push_back(current_input(control, act, result.environment));
   }
   if (!validate_isolated_sync_transaction_journal(result, roots, programs, current_sync_context(control), checkpoint))
      return false;
   chainbase_transaction_state_view live(control.db());
   return result.state.matches_base_reads(live, check) && isolated_numeric_environment_compatible();
}

bool validate_isolated_mixed_tree_current_state(const controller& control, const packed_transaction& packed,
      std::span<const isolated_sync_program> programs, const isolated_mixed_tree_result& result,
      const std::function<void()>& checkpoint) {
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   if (!isolated_numeric_environment_compatible()) return false;
   const auto& trx = packed.get_transaction();
   if (!control.is_builtin_activated(builtin_protocol_feature_t::sync_call) || !eligible_transaction(trx) ||
       trx.actions.size() > isolated_mixed_tree_executor::max_roots || !result.context_free_data ||
       result.groups.size() < trx.actions.size() || result.groups.size() > isolated_mixed_tree_executor::max_steps ||
       result.steps.size() < result.groups.size() || result.steps.size() > isolated_mixed_tree_executor::max_steps ||
       !sync_catalog_matches_current(control, programs, check)) return false;
   const auto& environment = result.groups.front().input.environment;
   if (!current_environment_matches(control, packed, control.pending_block_time(), environment)) return false;
   std::vector<isolated_action_input> roots;
   roots.reserve(trx.actions.size());
   for (const auto& act : trx.actions) {
      check();
      if (!eligible_account(control, act)) return false;
      roots.push_back(current_input(control, act, environment));
   }
   isolated_mixed_tree_context current{current_action_tree_context(control), current_sync_context(control)};
   check();
   // A transaction ID does not bind its context-free data. Capture the actual
   // packed input, never reuse the result's pointer as evidence of that input.
   // Keep callbacks OUTSIDE the budget catch so caller cancellation propagates.
   try {
      current.tree.context_free_data = isolated_context_free_data::capture(packed.get_context_free_data());
   } catch (const primary_state_budget_exceeded&) { return false; }
   if (current.tree.context_free_data->fingerprint() != result.context_free_data->fingerprint()) return false;
   for (const auto& step : result.steps) {
      check();
      if (step.group_index >= result.groups.size()) return false;
      const auto& act = result.groups[step.group_index].input;
      if (control.find_apply_handler(step.receiver, act.account, act.action)) return false;
   }
   if (!validate_isolated_mixed_tree_journal(result, roots, programs, current, checkpoint)) return false;
   chainbase_transaction_state_view live(control.db());
   return result.state.matches_base_reads(live, check) && isolated_numeric_environment_compatible();
}

// Defined only in this implementation. Its constructor is private and the
// borrowed transaction pointer is private too; public host hooks can consume,
// but cannot create, this capability. All allocations/preflight precede arming.
class isolated_sync_replay {
   transaction_context& trx_;
   const primary_overlay_result& state_;
   const isolated_sync_root_result& result_;
   host_context* frame_ = nullptr;
   uint32_t next_call_ = 0, pending_ordinal_ = 0;
   size_t next_write_ = 0;
   std::vector<bool> entered_;
   std::vector<fc::microseconds> subtree_cpu_;
   std::vector<std::vector<char>> returns_;

   isolated_sync_replay(apply_context& root, const isolated_sync_transaction_result& transaction, size_t index)
      : trx_(root.trx_context), state_(transaction.state), result_(transaction.roots.at(index)),
        next_write_(result_.root.writes_begin), entered_(result_.calls.size()+1),
        subtree_cpu_(result_.calls.size()+1), returns_(result_.calls.size()+1) {
      subtree_cpu_[0]=fc::microseconds(result_.root.elapsed.count());
      returns_[0]=result_.root.return_value;
      for(size_t i=0;i<result_.calls.size();++i) {
         subtree_cpu_[i+1]=fc::microseconds(result_.calls[i].exclusive_elapsed.count());
         returns_[i+1]=result_.calls[i].return_value;
      }
      for(size_t i=result_.calls.size();i>0;--i) {
         const auto parent=result_.calls[i-1].sender_ordinal;
         EOS_ASSERT(parent<i,transaction_exception,"prevalidated sync ancestry mismatch");
         subtree_cpu_[parent]+=subtree_cpu_[i];
      }
      EOS_ASSERT(!trx_.parallel_sync_replay_guard && !trx_.parallel_mixed_replay_guard,
                 transaction_exception,"sync replay already armed");
      trx_.parallel_sync_replay_guard=this;
   }

   void replay_frame(host_context& context, uint32_t ordinal, std::span<const isolated_sync_event> events,
                     size_t writes_begin, size_t writes_end, std::span<const uint32_t> markers) {
      EOS_ASSERT(next_write_==writes_begin,transaction_exception,"prevalidated sync write prefix mismatch");
      auto* previous=std::exchange(frame_,&context);
      auto restore=fc::make_scoped_exit([&]{frame_=previous;});
      EOS_ASSERT(original_console_size(context, ordinal) == 0, transaction_exception, "nonempty sync console prefix");
      uint32_t console_events = 0;
      std::vector<uint32_t> expected_markers;
      for(const auto& event:events) {
         trx_.checktime();
         if(const auto* call=std::get_if<isolated_sync_call_event>(&event)) {
            capture_original_console_marker(context, ordinal, markers, console_events, expected_markers);
            EOS_ASSERT(call->call_ordinal==next_call_+1 && next_call_<result_.calls.size() &&
                       call->after_writes==next_write_,transaction_exception,"prevalidated sync call order mismatch");
            const auto child_ordinal=++next_call_;
            const auto& child=result_.calls[child_ordinal-1];
            const auto previous_pending=std::exchange(pending_ordinal_,child_ordinal);
            auto restore_pending=fc::make_scoped_exit([&]{pending_ordinal_=previous_pending;});
            // ORIGINAL host creates the child context/trace, performs account,
            // flag/export/depth/data checks, normalizes errors and finalizes it.
            // No references into call_traces survive this recursive call.
            const auto status=context.execute_sync_call(child.receiver,child.flags,child.data);
            const auto expected=child.status<0?child.status:static_cast<int64_t>(child.return_value.size());
            EOS_ASSERT(status==expected && entered_[child_ordinal]==child.executed &&
                       context.last_sync_call_return_value==child.return_value && next_write_==child.writes_end,
                       transaction_exception,"prevalidated sync return mismatch");
            const auto& trace=context.get_call_trace(child_ordinal);
            EOS_ASSERT(trace.call_ordinal.value==child_ordinal && trace.sender_ordinal.value==ordinal &&
                       trace.receiver==child.receiver && trace.read_only==child.read_only && trace.data==child.data &&
                       trace.return_value==child.return_value && !trace.except && !trace.error_code &&
                       bool(trace.error_id)==(child.status<0) && (!trace.error_id || *trace.error_id==child.status),
                       transaction_exception,"prevalidated sync trace mismatch");
            continue;
         }
         const auto& cursor=std::get<isolated_cursor_event>(event);
         if(const auto* query=std::get_if<isolated_cursor_query>(&cursor)) {
            using kind=isolated_cursor_query_kind;
            int32_t actual=-1; auto primary=query->primary_before;
            switch(query->kind) {
               case kind::find: actual=context.db_find_i64(query->table.code,query->table.scope,query->table.table,query->key); break;
               case kind::lower_bound: actual=context.db_lowerbound_i64(query->table.code,query->table.scope,query->table.table,query->key); break;
               case kind::upper_bound: actual=context.db_upperbound_i64(query->table.code,query->table.scope,query->table.table,query->key); break;
               case kind::end: actual=context.db_end_i64(query->table.code,query->table.scope,query->table.table); break;
               case kind::next: actual=context.db_next_i64(query->iterator,primary); break;
               case kind::previous: actual=context.db_previous_i64(query->iterator,primary); break;
            }
            EOS_ASSERT(actual==query->result &&
               ((query->kind!=kind::next && query->kind!=kind::previous) || primary==query->primary_after),
               transaction_exception,"prevalidated sync cursor mismatch");
         } else if(const auto* mutation=std::get_if<isolated_cursor_write>(&cursor)) {
            // The original WASM registration owns this precondition; calling
            // lower-level DB hosts directly must not accidentally omit it.
            EOS_ASSERT(!context.is_read_only(),unaccessible_api,"this API is not allowed in read only action/call");
            EOS_ASSERT(mutation->write_index==next_write_ && next_write_<writes_end,
                       transaction_exception,"prevalidated sync mutation order mismatch");
            const auto& write=state_.primary_write(next_write_++);
            EOS_ASSERT(write.table.code==context.get_receiver(),transaction_exception,"prevalidated sync write owner mismatch");
            if(write.operation==primary_operation::store) {
               const auto actual=context.db_store_i64(write.table.scope,write.table.table,write.payer,write.primary_key,
                                                      write.value.data(),write.value.size());
               EOS_ASSERT(actual==mutation->iterator,transaction_exception,"prevalidated sync store cursor mismatch");
            } else if(write.operation==primary_operation::update) {
               context.db_update_i64(mutation->iterator,write.payer,write.value.data(),write.value.size());
            } else if(write.operation==primary_operation::remove) {
               context.db_remove_i64(mutation->iterator);
            } else EOS_ASSERT(false,transaction_exception,"prevalidated sync operation mismatch");
         } else if (const auto* secondary=std::get_if<isolated_secondary_cursor_event>(&cursor)) {
            replay_original_secondary(context,*secondary,state_,next_write_,writes_end);
         } else if (const auto* console=std::get_if<isolated_console_event>(&cursor)) {
            replay_original_console(context,*console,next_write_);
            ++console_events;
         } else EOS_ASSERT(false,transaction_exception,"prevalidated sync replay received an unsupported cursor event");
      }
      EOS_ASSERT(next_write_==writes_end,transaction_exception,"prevalidated sync write suffix mismatch");
      check_original_console_markers(context, ordinal, markers, expected_markers);
   }

   int64_t enter(sync_call_context& context) {
      EOS_ASSERT(&context.trx_context==&trx_ && context.ordinal==pending_ordinal_ && pending_ordinal_>0 &&
                 pending_ordinal_<=result_.calls.size() && &context.parent_ctx==frame_ && !entered_[pending_ordinal_],
                 transaction_exception,"unbound sync replay frame");
      const auto ordinal=pending_ordinal_;
      const auto& call=result_.calls[ordinal-1];
      EOS_ASSERT(call.executed && context.sender==call.sender && context.get_receiver()==call.receiver &&
                 context.read_only==call.read_only && context.data.size()==call.data.size() &&
                 std::equal(context.data.begin(),context.data.end(),call.data.begin()),
                 transaction_exception,"prevalidated sync input mismatch");
      entered_[ordinal]=true;
      replay_frame(context,ordinal,call.events,call.writes_begin,call.writes_end,call.console_markers);
      context.return_value=std::move(returns_[ordinal]); // sizes/protocol checked before arming; no allocation here
      return call.status;
   }

public:
   isolated_sync_replay(const isolated_sync_replay&)=delete;
   isolated_sync_replay& operator=(const isolated_sync_replay&)=delete;
   ~isolated_sync_replay() { trx_.parallel_sync_replay_guard=nullptr; }

   static bool adopt(apply_context& context,const fc::sha256& code_hash,uint8_t vm_type,uint8_t vm_version,
                     const isolated_sync_transaction_job& input,const isolated_sync_transaction_result& result,
                     std::optional<fc::microseconds> production_elapsed) {
      auto& trx=context.trx_context;
      const auto index=trx.parallel_next_action;
      if(trx.parallel_sync_result!=&result || trx.parallel_sync_input!=&input || trx.parallel_sync_replay_guard ||
         trx.parallel_transaction_result || trx.parallel_notification_result || trx.parallel_tree_result ||
         trx.parallel_mixed_result || trx.parallel_mixed_input || trx.parallel_mixed_replay_guard ||
         index>=result.roots.size() || input.roots.size()!=result.roots.size() ||
         result.roots.size()!=trx.packed_trx.get_transaction().actions.size() ||
         (!trx.explicit_billed_cpu_time && !production_elapsed) ||
         trx.is_transient() || trx.is_implicit() || trx.is_scheduled() || context.control.skip_trx_checks() ||
         context.is_context_free() || context.is_read_only() || context.is_privileged() || vm_type || vm_version ||
         context.get_receiver()!=context.get_action().account || context.get_sender()!=name{} || context.get_inline_depth()!=0 ||
         context.get_first_receiver_action_ordinal()!=index+1) return false;
      const auto& trace=context.get_current_action_trace();
      if(trace.action_ordinal.value!=index+1 || trace.creator_action_ordinal.value || !trace.call_traces.empty()) return false;
      const auto& current=result.roots[index];
      performance::scope adoption_timer(performance::stage::parallel_adopt);
      std::unique_ptr<isolated_sync_replay> replay;
      try {
         const auto checkpoint=[&]{trx.checktime();};
         if (!same_isolated_environment(input.roots[index].environment, result.environment) ||
             !current_environment_matches(context.control, trx.packed_trx, trx.published, result.environment)) return false;
         if(isolated_execution_fingerprint(code_hash,current_limits(context.control),
               current_input(context.control,context.get_action(),result.environment))
               !=current.root.execution_fingerprint) return false;
         if(index==0) {
            // Prove the WHOLE frozen attempt and current ledger before its
            // first event. Later roots legitimately see this prefix's writes.
            if(isolated_sync_transaction_fingerprint(input.roots,input.programs,input.context)!=result.execution_fingerprint ||
               !validate_isolated_sync_transaction_current_state(context.control,trx.packed_trx,input.programs,result,checkpoint))
               return false;
            fc::microseconds total{};
            for(const auto& root:result.roots) {
               total+=fc::microseconds(root.root.elapsed.count());
               for(const auto& call:root.calls) total+=fc::microseconds(call.exclusive_elapsed.count());
            }
            if(production_elapsed && *production_elapsed!=total) return false;
         }
         replay.reset(new isolated_sync_replay(context,result,index));
      } catch(const fc::exception&) { throw; }
        catch(const std::exception&) { return false; } // this root has not run; earlier roots MUST NOT be repeated
      // This root's successful private CPU is billed once, in top-level order.
      // Missing/empty/unsupported calls consume ordinals but never enter a VM;
      // they must not break a fictional contiguous child billing sequence.
      if(!trx.explicit_billed_cpu_time) trx.charge_isolated_cpu(replay->subtree_cpu_[0],index+1);
      replay->replay_frame(context,0,current.root_events,current.root.writes_begin,current.root.writes_end,current.root_console_markers);
      EOS_ASSERT(replay->next_call_==current.calls.size(),transaction_exception,"prevalidated sync unused calls");
      context.action_return_value=std::move(replay->returns_[0]);
      return true;
   }

   static std::optional<int64_t> child(sync_call_context& context) {
      if(auto* replay=context.trx_context.parallel_sync_replay_guard) return replay->enter(context);
      return {};
   }
   static fc::microseconds trace_elapsed(host_context& context,uint32_t ordinal) {
      const auto* replay=context.trx_context.parallel_sync_replay_guard;
      // Match existing action/transaction diagnostics: explicit validation uses
      // its fixed bill and original replay wall, never adds/bills local VM CPU.
      if(!replay || context.trx_context.explicit_billed_cpu_time) return fc::microseconds{};
      EOS_ASSERT(ordinal>0 && ordinal<replay->subtree_cpu_.size(),transaction_exception,"unbound sync elapsed trace");
      return replay->subtree_cpu_[ordinal]; // inclusive diagnostic only, never a second CPU charge
   }
};

bool try_adopt_sync_result(apply_context& context,const fc::sha256& code_hash,uint8_t vm_type,uint8_t vm_version,
      const isolated_sync_transaction_job& input,const isolated_sync_transaction_result& result,std::optional<fc::microseconds> production_elapsed) {
   if (!isolated_numeric_environment_compatible()) return false;
   return isolated_sync_replay::adopt(context,code_hash,vm_type,vm_version,input,result,production_elapsed);
}
// Separate capability from the root-only sync replay. Each ordinary receiver
// owns local call ordinals, while the ORIGINAL apply_context retains its group
// cursors, notification queue and most recent sync return across receivers.
class isolated_mixed_replay {
   transaction_context& trx_;
   const primary_overlay_result& state_;
   const isolated_mixed_tree_step& step_;
   host_context* frame_ = nullptr;
   uint32_t next_call_ = 0, pending_ordinal_ = 0;
   size_t next_write_;
   std::vector<bool> entered_;
   std::vector<fc::microseconds> subtree_cpu_;
   std::vector<std::vector<char>> returns_;

   isolated_mixed_replay(apply_context& context, const isolated_mixed_tree_result& result, size_t index)
      : trx_(context.trx_context), state_(result.state), step_(result.steps.at(index)),
        next_write_(step_.effects.writes_begin), entered_(step_.calls.size()+1),
        subtree_cpu_(step_.calls.size()+1), returns_(step_.calls.size()+1) {
      subtree_cpu_[0] = fc::microseconds(step_.effects.elapsed.count());
      returns_[0] = step_.effects.return_value;
      for (size_t i=0; i<step_.calls.size(); ++i) {
         subtree_cpu_[i+1] = fc::microseconds(step_.calls[i].exclusive_elapsed.count());
         returns_[i+1] = step_.calls[i].return_value;
      }
      for (size_t i=step_.calls.size(); i>0; --i) {
         const auto parent = step_.calls[i-1].sender_ordinal;
         EOS_ASSERT(parent<i, transaction_exception, "prevalidated mixed ancestry mismatch");
         subtree_cpu_[parent] += subtree_cpu_[i];
      }
      EOS_ASSERT(!trx_.parallel_sync_replay_guard && !trx_.parallel_mixed_replay_guard,
                 transaction_exception, "mixed replay already armed");
      trx_.parallel_mixed_replay_guard = this; // after every allocation
   }

   void replay_call(host_context& context, uint32_t ordinal, const isolated_sync_call_event& event) {
      EOS_ASSERT(event.call_ordinal==next_call_+1 && next_call_<step_.calls.size() &&
                 event.after_writes==next_write_, transaction_exception, "prevalidated mixed call order mismatch");
      const auto child_ordinal = ++next_call_;
      const auto& child = step_.calls[child_ordinal-1];
      const auto previous = std::exchange(pending_ordinal_,child_ordinal);
      auto restore = fc::make_scoped_exit([&]{ pending_ordinal_=previous; });
      const auto status = context.execute_sync_call(child.receiver,child.flags,child.data);
      const auto expected = child.status<0 ? child.status : static_cast<int64_t>(child.return_value.size());
      EOS_ASSERT(status==expected && entered_[child_ordinal]==child.executed &&
                 context.last_sync_call_return_value==child.return_value && next_write_==child.writes_end,
                 transaction_exception, "prevalidated mixed return mismatch");
      // Reacquire only AFTER recursion. Neither action nor call trace references
      // may survive original scheduling or child execution.
      const auto& trace = context.get_call_trace(child_ordinal);
      EOS_ASSERT(trace.call_ordinal.value==child_ordinal && trace.sender_ordinal.value==ordinal &&
                 trace.receiver==child.receiver && trace.read_only==child.read_only && trace.data==child.data &&
                 trace.return_value==child.return_value && !trace.except && !trace.error_code &&
                 bool(trace.error_id)==(child.status<0) && (!trace.error_id || *trace.error_id==child.status),
                 transaction_exception, "prevalidated mixed call trace mismatch");
   }

   void replay_cursor(host_context& context, apply_context* ordinary, const isolated_cursor_event& event,
                      size_t writes_end) {
      if (const auto* console=std::get_if<isolated_console_event>(&event)) {
         replay_original_console(context,*console,next_write_);
         return;
      }
      EOS_ASSERT(!context.is_context_free(), unaccessible_api, "only context free api's can be used in this context");
      if (const auto* query=std::get_if<isolated_cursor_query>(&event)) {
         using kind=isolated_cursor_query_kind;
         int32_t actual=-1;
         auto primary=query->primary_before;
         switch (query->kind) {
            case kind::find: actual=context.db_find_i64(query->table.code,query->table.scope,query->table.table,query->key); break;
            case kind::lower_bound: actual=context.db_lowerbound_i64(query->table.code,query->table.scope,query->table.table,query->key); break;
            case kind::upper_bound: actual=context.db_upperbound_i64(query->table.code,query->table.scope,query->table.table,query->key); break;
            case kind::end: actual=context.db_end_i64(query->table.code,query->table.scope,query->table.table); break;
            case kind::next: actual=context.db_next_i64(query->iterator,primary); break;
            case kind::previous: actual=context.db_previous_i64(query->iterator,primary); break;
         }
         EOS_ASSERT(actual==query->result &&
                    ((query->kind!=kind::next && query->kind!=kind::previous) || primary==query->primary_after),
                    transaction_exception, "prevalidated mixed cursor mismatch");
      } else if (const auto* mutation=std::get_if<isolated_cursor_write>(&event)) {
         EOS_ASSERT(!context.is_read_only(), unaccessible_api, "this API is not allowed in read only action/call");
         EOS_ASSERT(mutation->write_index==next_write_ && next_write_<writes_end,
                    transaction_exception, "prevalidated mixed mutation order mismatch");
         const auto& write=state_.primary_write(next_write_++);
         EOS_ASSERT(write.table.code==context.get_receiver(),transaction_exception,"prevalidated mixed write owner mismatch");
         if (write.operation==primary_operation::store) {
            const auto actual=context.db_store_i64(write.table.scope,write.table.table,write.payer,write.primary_key,
                                                  write.value.data(),write.value.size());
            EOS_ASSERT(actual==mutation->iterator,transaction_exception,"prevalidated mixed store cursor mismatch");
         } else if (write.operation==primary_operation::update) {
            context.db_update_i64(mutation->iterator,write.payer,write.value.data(),write.value.size());
         } else if (write.operation==primary_operation::remove) {
            context.db_remove_i64(mutation->iterator);
         } else EOS_ASSERT(false,transaction_exception,"prevalidated mixed operation mismatch");
      } else if (const auto* notification=std::get_if<isolated_cursor_notify>(&event)) {
         const auto index=notification->notification_index;
         EOS_ASSERT(ordinary && !ordinary->is_context_free() && index<step_.effects.recipients.size() &&
                    step_.effects.recipients[index].after_writes==next_write_,transaction_exception,
                    "prevalidated mixed notification mismatch");
         ordinary->require_recipient(step_.effects.recipients[index].recipient);
      } else if (const auto* secondary=std::get_if<isolated_secondary_cursor_event>(&event)) {
         replay_original_secondary(context,*secondary,state_,next_write_,writes_end);
      } else EOS_ASSERT(false,transaction_exception,"prevalidated mixed replay received an unsupported cursor event");
   }

   template<class Event>
   void replay_frame(host_context& context, apply_context* ordinary, uint32_t ordinal,
                     const std::vector<Event>& events, size_t writes_begin, size_t writes_end,
                     std::span<const uint32_t> markers) {
      EOS_ASSERT(next_write_==writes_begin,transaction_exception,"prevalidated mixed write prefix mismatch");
      auto* previous=std::exchange(frame_,&context);
      auto restore=fc::make_scoped_exit([&]{frame_=previous;});
      EOS_ASSERT(original_console_size(context, ordinal) == 0, transaction_exception, "nonempty mixed console prefix");
      uint32_t console_events = 0;
      std::vector<uint32_t> expected_markers;
      for (const auto& event:events) {
         trx_.checktime();
         if (const auto* call=std::get_if<isolated_sync_call_event>(&event)) {
            capture_original_console_marker(context, ordinal, markers, console_events, expected_markers);
            replay_call(context,ordinal,*call);
            continue;
         }
         if constexpr (std::is_same_v<Event,isolated_mixed_tree_event>) {
            if (const auto* send=std::get_if<isolated_tree_inline_event>(&event)) {
               EOS_ASSERT(ordinary && send->after_writes==next_write_,transaction_exception,
                          "prevalidated mixed send position mismatch");
               auto bytes=legacy_span<const char>{const_cast<char*>(send->sent.packed.data()),
                                                  static_cast<uint32_t>(send->sent.packed.size())};
               if (send->context_free) webassembly::interface(*ordinary).send_context_free_inline(std::move(bytes));
               else webassembly::interface(*ordinary).send_inline(std::move(bytes));
               EOS_ASSERT(trx_.trace->action_traces.size()==send->action_ordinal,
                          transaction_exception,"prevalidated mixed inline schedule mismatch");
               continue;
            }
         }
         const auto& cursor_event=std::get<isolated_cursor_event>(event);
         replay_cursor(context,ordinary,cursor_event,writes_end);
         if (std::holds_alternative<isolated_console_event>(cursor_event)) ++console_events;
      }
      EOS_ASSERT(next_write_==writes_end,transaction_exception,"prevalidated mixed write suffix mismatch");
      check_original_console_markers(context, ordinal, markers, expected_markers);
   }

   int64_t enter(sync_call_context& context) {
      EOS_ASSERT(&context.trx_context==&trx_ && context.ordinal==pending_ordinal_ && pending_ordinal_>0 &&
                 pending_ordinal_<=step_.calls.size() && &context.parent_ctx==frame_ && !entered_[pending_ordinal_],
                 transaction_exception,"unbound mixed sync replay frame");
      const auto ordinal=pending_ordinal_;
      const auto& call=step_.calls[ordinal-1];
      EOS_ASSERT(call.executed && context.sender==call.sender && context.get_receiver()==call.receiver &&
                 context.read_only==call.read_only && context.data.size()==call.data.size() &&
                 std::equal(context.data.begin(),context.data.end(),call.data.begin()),
                 transaction_exception,"prevalidated mixed sync input mismatch");
      entered_[ordinal]=true;
      replay_frame(context,nullptr,ordinal,call.events,call.writes_begin,call.writes_end,call.console_markers);
      context.return_value=std::move(returns_[ordinal]);
      return call.status;
   }

public:
   isolated_mixed_replay(const isolated_mixed_replay&)=delete;
   isolated_mixed_replay& operator=(const isolated_mixed_replay&)=delete;
   ~isolated_mixed_replay() { trx_.parallel_mixed_replay_guard=nullptr; }

   static bool adopt(apply_context& context,const fc::sha256& code_hash,uint8_t vm_type,uint8_t vm_version,
                     const isolated_mixed_tree_job& input,const isolated_mixed_tree_result& result,
                     std::optional<fc::microseconds> production_elapsed) {
      auto& trx=context.trx_context;
      const auto index=trx.parallel_next_receiver;
      if (trx.parallel_mixed_result!=&result || trx.parallel_mixed_input!=&input || trx.parallel_mixed_replay_guard ||
          trx.parallel_transaction_result || trx.parallel_notification_result || trx.parallel_tree_result ||
          trx.parallel_sync_result || trx.parallel_sync_input || trx.parallel_sync_replay_guard ||
          index>=result.steps.size() || result.steps.size()>isolated_mixed_tree_executor::max_steps ||
          result.groups.size()>isolated_mixed_tree_executor::max_steps ||
          (!trx.explicit_billed_cpu_time && !production_elapsed) ||
          (production_elapsed && (production_elapsed->count()<0 || *production_elapsed>fc::milliseconds(500))) ||
          trx.is_transient() || trx.is_implicit() || trx.is_scheduled() || context.control.skip_trx_checks() ||
          context.is_read_only() || context.is_privileged() || vm_type || vm_version ||
          !eligible_transaction(trx.packed_trx.get_transaction()) || !result.context_free_data ||
          !input.context.tree.context_free_data) return false;
      const auto& step=result.steps[index];
      if (step.group_index>=result.groups.size()) return false;
      const auto& group=result.groups[step.group_index];
      {
         const auto& trace=context.get_current_action_trace();
         if (group.root_index>=trx.packed_trx.get_transaction().actions.size() ||
             group.context_free!=context.is_context_free() || step.receiver!=context.get_receiver() ||
             step.code_hash!=code_hash || step.has_code!=(code_hash!=fc::sha256{}) ||
             trace.action_ordinal.value!=step.action_ordinal || trace.creator_action_ordinal.value!=step.creator_action_ordinal ||
             trace.closest_unnotified_ancestor_action_ordinal.value!=step.closest_unnotified_ancestor || !trace.call_traces.empty() ||
             context.get_inline_depth()!=group.depth || context.get_first_receiver_action_ordinal()!=group.action_ordinal ||
             context.get_sender()!=step.sender ||
             context.control.find_apply_handler(context.get_receiver(),context.get_action().account,context.get_action().name)) return false;
      }
      performance::scope adoption_timer(performance::stage::parallel_adopt);
      std::unique_ptr<isolated_mixed_replay> replay;
      try {
         const auto checkpoint=[&]{trx.checktime();};
         const auto current_tree=current_action_tree_context(context.control);
         const auto current_sync=current_sync_context(context.control);
         const auto& captured=input.context.sync;
         if (current_tree.max_inline_action_size!=input.context.tree.max_inline_action_size ||
             current_tree.max_inline_action_depth!=input.context.tree.max_inline_action_depth ||
             current_tree.console_enabled!=input.context.tree.console_enabled ||
             current_sync.max_depth!=captured.max_depth || current_sync.max_data_bytes!=captured.max_data_bytes ||
             current_sync.root_read_only!=captured.root_read_only || current_sync.get_sender_fix!=captured.get_sender_fix ||
             current_sync.console_enabled!=captured.console_enabled ||
             !context.control.is_builtin_activated(builtin_protocol_feature_t::sync_call) ||
             result.context_free_data->fingerprint()!=input.context.tree.context_free_data->fingerprint()) return false;
         const auto program=std::find_if(input.programs.begin(),input.programs.end(),[&](const auto& p){
            return p.program.receiver==step.receiver;
         });
         const auto limits=current_limits(context.control);
         if (program==input.programs.end() || program->program.limits!=limits) return false;
         if (group.root_index >= input.roots.size() ||
             !same_isolated_environment(group.input.environment, input.roots[group.root_index].environment) ||
             !current_environment_matches(context.control, trx.packed_trx, trx.published, group.input.environment)) return false;
         auto arguments=current_input(context.control,context.get_action(),group.input.environment);
         arguments.receiver=context.get_receiver();
         if (isolated_mixed_tree_step_fingerprint(isolated_execution_fingerprint(code_hash,limits,arguments),
                result.execution_fingerprint,input.context,group,step)!=step.effects.execution_fingerprint ||
             step.effects.return_value.size()>arguments.max_return_bytes) return false;
         if (index==0) {
            if (isolated_mixed_tree_fingerprint(input.roots,input.programs,input.context)!=result.execution_fingerprint ||
                !validate_isolated_mixed_tree_current_state(context.control,trx.packed_trx,input.programs,result,checkpoint)) return false;
            fc::microseconds total{};
            for (const auto& receiver:result.steps) {
               total+=fc::microseconds(receiver.effects.elapsed.count());
               for (const auto& call:receiver.calls) total+=fc::microseconds(call.exclusive_elapsed.count());
            }
            if (production_elapsed && *production_elapsed!=total) return false;
         }
         replay.reset(new isolated_mixed_replay(context,result,index));
      } catch (const fc::exception&) { throw; }
        catch (const std::exception&) { return false; } // this receiver has run NO event
      if (!trx.explicit_billed_cpu_time) trx.charge_isolated_mixed_cpu(replay->subtree_cpu_[0],index,step.action_ordinal);
      replay->replay_frame(context,&context,0,step.events,step.effects.writes_begin,step.effects.writes_end,step.console_markers);
      EOS_ASSERT(replay->next_call_==step.calls.size(),transaction_exception,"prevalidated mixed unused calls");
      context.action_return_value=std::move(replay->returns_[0]);
      return true;
   }

   static std::optional<int64_t> child(sync_call_context& context) {
      if (auto* replay=context.trx_context.parallel_mixed_replay_guard) return replay->enter(context);
      return {};
   }
   static std::optional<fc::microseconds> trace_elapsed(host_context& context,uint32_t ordinal) {
      const auto* replay=context.trx_context.parallel_mixed_replay_guard;
      if (!replay) return {};
      if (context.trx_context.explicit_billed_cpu_time) return fc::microseconds{};
      EOS_ASSERT(ordinal>0 && ordinal<replay->subtree_cpu_.size(),transaction_exception,"unbound mixed elapsed trace");
      return replay->subtree_cpu_[ordinal]; // inclusive diagnostic, never billed twice
   }
};

bool try_adopt_mixed_result(apply_context& context,const fc::sha256& code_hash,uint8_t vm_type,uint8_t vm_version,
      const isolated_mixed_tree_job& input,const isolated_mixed_tree_result& result,std::optional<fc::microseconds> production_elapsed) {
   if (!isolated_numeric_environment_compatible()) return false;
   return isolated_mixed_replay::adopt(context,code_hash,vm_type,vm_version,input,result,production_elapsed);
}
std::optional<int64_t> replay_isolated_sync_call(sync_call_context& context) {
   if (auto result=isolated_mixed_replay::child(context)) return result;
   return isolated_sync_replay::child(context);
}
fc::microseconds isolated_sync_trace_elapsed(host_context& context,uint32_t ordinal) {
   if (auto elapsed=isolated_mixed_replay::trace_elapsed(context,ordinal)) return *elapsed;
   return isolated_sync_replay::trace_elapsed(context,ordinal);
}

bool try_adopt_notification_result(apply_context& context, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
      const isolated_wasm_limits& limits, const isolated_notification_result& result, size_t step_index,
      std::optional<fc::microseconds> production_elapsed) {
   if (!isolated_numeric_environment_compatible()) return false;
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
         roots.reserve(trx.actions.size());
         for (const auto& act : trx.actions) roots.push_back(current_input(context.control, act));
         std::vector<isolated_receiver_program> programs;
         programs.reserve(std::min<size_t>(result.steps.size(),4));
         small_seen_keys<name> captured;
         size_t code_bytes = 0;
         fc::microseconds elapsed{};
         for (const auto& receiver : result.steps) {
            checkpoint();
            if (receiver.effects.elapsed.count() < 0 ||
                receiver.effects.elapsed.count() > (fc::milliseconds(500) - elapsed).count()) return false;
            elapsed += fc::microseconds(receiver.effects.elapsed.count());
            if (!captured.insert(receiver.receiver)) continue;
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
   size_t next_write = step.effects.writes_begin;
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
         EOS_ASSERT(!context.is_read_only(), unaccessible_api, "this API is not allowed in read only action/call");
         EOS_ASSERT(mutation->write_index == next_write && next_write < step.effects.writes_end,
                    transaction_exception, "prevalidated notification mutation order mismatch");
         const auto& write = result.state.primary_write(next_write++);
         EOS_ASSERT(write.table.code == context.get_receiver(), transaction_exception, "prevalidated notification write owner mismatch");
         if (write.operation == primary_operation::store) {
            const auto actual = context.db_store_i64(write.table.scope, write.table.table, write.payer, write.primary_key,
                                                     write.value.data(), write.value.size());
            EOS_ASSERT(actual == mutation->iterator, transaction_exception, "prevalidated notification store cursor mismatch");
         } else if (write.operation == primary_operation::update) {
            context.db_update_i64(mutation->iterator, write.payer, write.value.data(), write.value.size());
         } else if (write.operation == primary_operation::remove) {
            context.db_remove_i64(mutation->iterator);
         } else EOS_ASSERT(false, transaction_exception, "prevalidated notification operation mismatch");
      } else if (const auto* notification = std::get_if<isolated_cursor_notify>(&event)) {
         EOS_ASSERT(notification->notification_index < step.effects.recipients.size() &&
                    step.effects.recipients[notification->notification_index].after_writes == next_write,
                    transaction_exception, "prevalidated notification position mismatch");
         context.require_recipient(step.effects.recipients[notification->notification_index].recipient);
      } else if (const auto* secondary = std::get_if<isolated_secondary_cursor_event>(&event)) {
         replay_original_secondary(context, *secondary, result.state, next_write, step.effects.writes_end);
      } else EOS_ASSERT(false,transaction_exception,"prevalidated notification replay received an unsupported cursor event");
   }
   EOS_ASSERT(next_write == step.effects.writes_end, transaction_exception, "prevalidated notification write suffix mismatch");
   context.action_return_value = std::move(return_value);
   return true;
}

bool try_adopt_action_tree_result(apply_context& context, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
      const isolated_wasm_limits& limits, const isolated_action_tree_result& result, size_t step_index,
      std::optional<fc::microseconds> production_elapsed) {
   if (!isolated_numeric_environment_compatible()) return false;
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
      if (!current_environment_matches(context.control, context.trx_context.packed_trx,
            context.trx_context.published, group.input.environment)) return false;
      const auto max_return_bytes=context.control.get_global_properties().configuration.max_action_return_value_size;
      if(const auto* proof=context.trx_context.isolated_tree_preflight()) {
         // The worker checked every frozen digest. Exact object identity and
         // current full action bytes/protocol/code/limits bind that same proof.
         // Original fresh environment,ordinals,native handlers,state and
         // authority checks remain before any canonical event.
         if(!proof->binds_current_step(result,step_index,context.get_receiver(),code_hash,
               context.get_action(),limits,max_return_bytes,tree_context,
               [&]{context.trx_context.checktime();}))return false;
      } else {
         const auto action_fingerprint=isolated_execution_fingerprint_from_action(code_hash,limits,
            context.get_receiver(),context.get_action(),max_return_bytes,group.input.environment);
         if(isolated_action_tree_step_fingerprint(action_fingerprint,tree_context,group,step)!=
               step.effects.execution_fingerprint)return false;
      }
      if(step.effects.return_value.size()>max_return_bytes)return false;
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
         roots.reserve(trx.actions.size());
         for (const auto& act : trx.actions) roots.push_back(current_input(context.control, act, group.input.environment));
         std::vector<isolated_receiver_program> programs;
         programs.reserve(std::min<size_t>(result.steps.size(),4));
         small_seen_keys<name> captured;
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
            if (!captured.insert(receiver.receiver)) continue;
            auto program = prepare_isolated_tree_program(context.control, receiver.receiver);
            if (!program || program->code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return false;
            code_bytes += program->code.size();
            programs.push_back(std::move(*program));
         }
         if (production_elapsed && *production_elapsed != elapsed) return false;
         if(const auto* preflight=context.trx_context.isolated_tree_preflight()) {
            if(!preflight->matches(result,roots,programs,tree_context,checkpoint))return false;
            if(!context.trx_context.explicit_billed_cpu_time)
               context.trx_context.charge_isolated_preflight_cpu(preflight->cpu());
         } else if (!validate_isolated_action_tree_journal(result, roots, programs, tree_context, checkpoint)) return false;
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
   size_t next_write = step.effects.writes_begin;
   for (const auto& event : step.events) {
      context.trx_context.checktime();
      if (const auto* send = std::get_if<isolated_tree_inline_event>(&event)) {
         EOS_ASSERT(send->after_writes == next_write, transaction_exception, "prevalidated inline position mismatch");
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
      if (const auto* console = std::get_if<isolated_console_event>(&cursor)) {
         replay_original_console(context, *console, next_write);
         continue;
      }
      EOS_ASSERT(!context.is_context_free(), unaccessible_api, "only context free api's can be used in this context");
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
         EOS_ASSERT(!context.is_read_only(), unaccessible_api, "this API is not allowed in read only action/call");
         EOS_ASSERT(mutation->write_index == next_write && next_write < step.effects.writes_end,
                    transaction_exception, "prevalidated tree mutation order mismatch");
         const auto& write = result.state.primary_write(next_write++);
         EOS_ASSERT(write.table.code == context.get_receiver(), transaction_exception, "prevalidated tree write owner mismatch");
         if (write.operation == primary_operation::store) {
            const auto actual = context.db_store_i64(write.table.scope, write.table.table, write.payer, write.primary_key,
                                                     write.value.data(), write.value.size());
            EOS_ASSERT(actual == mutation->iterator, transaction_exception, "prevalidated tree store cursor mismatch");
         } else if (write.operation == primary_operation::update) {
            context.db_update_i64(mutation->iterator, write.payer, write.value.data(), write.value.size());
         } else if (write.operation == primary_operation::remove) {
            context.db_remove_i64(mutation->iterator);
         } else EOS_ASSERT(false, transaction_exception, "prevalidated tree operation mismatch");
      } else if (const auto* notification = std::get_if<isolated_cursor_notify>(&cursor)) {
         EOS_ASSERT(notification->notification_index < step.effects.recipients.size() &&
                    step.effects.recipients[notification->notification_index].after_writes == next_write,
                    transaction_exception, "prevalidated tree notification position mismatch");
         context.require_recipient(step.effects.recipients[notification->notification_index].recipient);
      } else if (const auto* secondary = std::get_if<isolated_secondary_cursor_event>(&cursor)) {
         replay_original_secondary(context, *secondary, result.state, next_write, step.effects.writes_end);
      } else EOS_ASSERT(false,transaction_exception,"prevalidated tree replay received an unsupported cursor event");
   }
   EOS_ASSERT(next_write == step.effects.writes_end, transaction_exception, "prevalidated tree write suffix mismatch");
   context.action_return_value = std::move(return_value);
   return true;
}

} // namespace eosio::chain::experimental
