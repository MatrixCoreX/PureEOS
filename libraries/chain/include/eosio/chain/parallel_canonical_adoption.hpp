#pragma once
#include <eosio/chain/parallel_wasm_worker.hpp>
#include <fc/time.hpp>

namespace eosio::chain {
class apply_context;
class controller;
class packed_transaction;
}

namespace eosio::chain::experimental {

// Captured on the canonical owner. Eligibility is deliberately narrow; the
// caller must additionally enforce its local reviewed-code allowlist and batch
// ordering/lifetime. No signature, permissions, resource or receipt checks are
// skipped by preparing an action.
struct prepared_isolated_action {
   std::vector<char> code;
   fc::sha256 code_hash;
   isolated_action_input input;
   isolated_wasm_limits limits;
};
std::optional<std::vector<prepared_isolated_action>> prepare_isolated_transaction(const controller&, const packed_transaction&);

// Owner-only, bounded byte/metadata capture. Empty code is an EXISTING ordinary
// account, never a missing/native/privileged receiver. Check every root's native
// handler pair; callers must still enforce their reviewed-code allowlist.
std::optional<isolated_receiver_program> prepare_isolated_receiver_program(
   const controller&, name receiver, std::span<const isolated_action_input> roots);

// Called at the original WASM execution point, AFTER transaction init/auth and
// native/blacklist guards. First step preflights the WHOLE transaction before
// any canonical write. False never changes THIS action; any previously adopted
// prefix is already canonical and must not be re-executed. True replays this
// action's ordered writes through original host functions, then lets the
// original action/transaction receipt, resource and undo paths continue.
// Any exception during replay MUST propagate through the original undo path;
// callers must never catch it and rerun WASM over partially changed state.
bool try_adopt_primary_result(apply_context&, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
                              const isolated_wasm_limits&, const isolated_transaction_result&, size_t action_index,
                              std::optional<fc::microseconds> production_elapsed = {});

// Same original-host/undo rule, but consumes ONE receiver in execution order.
// Step zero validates the complete owned cursor journal AND current code/state.
// Later false returns occur before THIS receiver's events; preserved prefix
// cursor state allows the original VM to continue without replaying that prefix.
bool try_adopt_notification_result(apply_context&, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
   const isolated_wasm_limits&, const isolated_notification_result&, size_t step_index,
   std::optional<fc::microseconds> production_elapsed = {});

} // namespace eosio::chain::experimental
