#pragma once

#include <eosio/chain/parallel_state_overlay.hpp>
#include <chrono>
#include <stop_token>

namespace eosio::chain::experimental {

// TEST-ONLY capability. No controller, database, permission manager, receipts,
// resources, signals or native action handlers are accessible to this host.
// A caller must later validate code/context/dependencies before ordered adoption.
struct isolated_action_input {
   name receiver;
   name account;
   name action;
   std::vector<std::pair<name, name>> authorizations;
   std::vector<char> data;
   uint32_t max_return_bytes = 256;
};

struct isolated_wasm_limits {
   uint32_t max_pages = 16;
   uint32_t max_call_depth = 64;
   size_t max_code_bytes = 64 * 1024;
   size_t max_action_bytes = 64 * 1024;
   size_t max_authorizations = 64;
   uint32_t max_return_bytes = 4096;
   std::chrono::milliseconds max_attempt_time{500};
};

struct isolated_action_result {
   primary_overlay_result state;
   std::vector<char> return_value;
};

// Each worker OWNS its compiled module, linear memory and execution context.
// Sharing the VM backend module is unsafe: timeout temporarily disables its
// code pages. This type is neither copyable nor movable and is thread-affine.
// Unknown imports fail linking; all errors require WHOLE-transaction fallback,
// never a consensus rejection based on these local capability/timeout limits.
class isolated_wasm_module {
public:
   explicit isolated_wasm_module(std::span<const char> code, isolated_wasm_limits = {});
   ~isolated_wasm_module();
   isolated_wasm_module(const isolated_wasm_module&) = delete;
   isolated_wasm_module& operator=(const isolated_wasm_module&) = delete;
   isolated_action_result execute(std::shared_ptr<const frozen_primary_state>, const isolated_action_input&,
                                  std::stop_token = {}, primary_overlay_limits = {});
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
