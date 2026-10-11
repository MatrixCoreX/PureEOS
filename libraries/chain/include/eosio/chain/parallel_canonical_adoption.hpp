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
std::optional<prepared_isolated_action> prepare_isolated_action(const controller&, const packed_transaction&);

// Called at the original WASM execution point, AFTER transaction init/auth and
// native/blacklist guards. False is returned ONLY before any canonical write.
// True applies ordered writes through original host functions, then lets the
// original action/transaction receipt, resource and undo paths continue.
// Any exception during replay MUST propagate through the original undo path;
// callers must never catch it and rerun WASM over partially changed state.
bool try_adopt_primary_result(apply_context&, const fc::sha256& code_hash, uint8_t vm_type, uint8_t vm_version,
                              const isolated_wasm_limits&, const isolated_action_result&,
                              std::optional<fc::microseconds> production_elapsed = {});

} // namespace eosio::chain::experimental
