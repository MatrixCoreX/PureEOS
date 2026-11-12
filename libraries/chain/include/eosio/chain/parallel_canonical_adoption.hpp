#pragma once
#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <fc/time.hpp>

namespace eosio::chain {
class apply_context;
class controller;
class packed_transaction;
class host_context;
class sync_call_context;
}

namespace eosio::chain::experimental {

struct isolated_mixed_tree_result;
struct isolated_mixed_tree_job;

// Owner-only action metadata/input capture for a receiver-deduplicated tree.
// Owns NO code bytes and is deliberately not a prepared_isolated_action.
// Callers must review every root's code hash, capture a bounded program catalog
// and keep all ordinary authorization/resource/adoption checks.
struct prepared_isolated_root {
   fc::sha256 code_hash;
   isolated_action_input input;
   isolated_wasm_limits limits;
};
std::optional<std::vector<prepared_isolated_root>> prepare_isolated_transaction_roots(
   const controller&, const packed_transaction&, const std::function<void()>& checkpoint = {});

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

// Owner-only code/metadata capture, NOT dynamic native-handler eligibility.
// Tree adoption must check every actual (receiver, account, action) separately.
// Check the remaining owned-code budget BEFORE allocating the byte copy.
std::optional<isolated_receiver_program> prepare_isolated_tree_program(const controller&, name receiver,
   size_t owned_code_budget = isolated_wasm_limits::max_module_code_bytes);
isolated_action_tree_context current_action_tree_context(const controller&);

// Sync export support is the ACTUAL code_object bit, never inferred from a
// private parser. The caller still enforces its reviewed-code allowlist.
std::optional<isolated_sync_program> prepare_isolated_sync_program(const controller&, name receiver);
isolated_sync_context current_sync_context(const controller&);

// Read-only OWNER check for one ordinary root. Binds the complete ordered
// catalog (including unused entries), current protocol/limits and every base
// dependency before any canonical event. NOT authorization or an adoption
// capability: original transaction/apply checks and replay are still required.
// The checkpoint's exceptions propagate; this function never mutates the DB.
bool validate_isolated_sync_current_state(const controller&, const packed_transaction&,
   std::span<const isolated_sync_program>, const isolated_sync_result&,
   const std::function<void()>& checkpoint = {});

// Whole-transaction equivalent: binds ALL ordinary roots before the first
// canonical write. Later roots must not compare the now-mutated live prefix
// against the original snapshot. This still provides no replay capability.
bool validate_isolated_sync_transaction_current_state(const controller&, const packed_transaction&,
   std::span<const isolated_sync_program>, const isolated_sync_transaction_result&,
   const std::function<void()>& checkpoint = {});

// Read-only owner proof for the complete mixed tree. Binds actual packed CF
// data even when empty, every ordinary root/dynamic native-handler pair, the
// entire ordered catalog and current limits/protocol before live dependencies.
// This is NOT an authorization or a canonical replay capability.
bool validate_isolated_mixed_tree_current_state(const controller&, const packed_transaction&,
   std::span<const isolated_sync_program>, const isolated_mixed_tree_result&,
   const std::function<void()>& checkpoint = {});

// Consume the next ordinary root from the controller-owned transaction result.
// First root preflights the entire attempt; false precedes THIS root's events,
// never re-executes earlier roots. Once replay starts, every error propagates
// through original transaction undo; no partially consumed frame may retry.
bool try_adopt_sync_result(apply_context&, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
   const isolated_sync_transaction_job&, const isolated_sync_transaction_result&,
   std::optional<fc::microseconds> production_elapsed = {});

// Consume one original receiver step, including its immediate sync subtree.
// Whole-tree proof precedes the first event; late false preserves the canonical
// prefix and notification-group host buffers. Post-event errors use original undo.
bool try_adopt_mixed_result(apply_context&, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
   const isolated_mixed_tree_job&, const isolated_mixed_tree_result&,
   std::optional<fc::microseconds> production_elapsed = {});

// Original host execution-point hooks. They can only CONSUME a private,
// stack-scoped preflight capability; callers cannot inject an arbitrary replay.
std::optional<int64_t> replay_isolated_sync_call(sync_call_context&);
fc::microseconds isolated_sync_trace_elapsed(host_context&, uint32_t ordinal);

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

// Inline tree: step zero binds ALL dynamic native/code pairs, packed CF data and
// the frozen journal to current state. Each send re-enters its ORIGINAL host and
// its immediate authorization/account checks at the recorded position. Never retains an action
// trace reference across scheduling (the underlying vector may reallocate).
bool try_adopt_action_tree_result(apply_context&, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
   const isolated_wasm_limits&, const isolated_action_tree_result&, size_t step_index,
   std::optional<fc::microseconds> production_elapsed = {});

} // namespace eosio::chain::experimental
