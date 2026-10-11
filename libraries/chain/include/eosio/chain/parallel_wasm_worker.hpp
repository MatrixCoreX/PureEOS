#pragma once

#include <eosio/chain/parallel_state_overlay.hpp>
#include <fc/crypto/sha256.hpp>
#include <chrono>
#include <atomic>
#include <stop_token>

namespace eosio::chain::experimental {

// An owned atomic generation bridges the existing controller interrupt route
// without sending signals to workers or retaining any controller/DB pointer.
struct isolated_cancel {
   std::shared_ptr<const std::atomic<uint64_t>> generation;
   uint64_t observed = 0;
   // One absolute budget for the WHOLE production batch, including queued jobs.
   // Validation leaves it unlimited and uses the per-attempt watchdog instead.
   std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
   bool stop_requested() const {
      return (generation && generation->load(std::memory_order_relaxed) != observed) ||
             (deadline != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= deadline);
   }
};

// Restricted capability. No controller, database, permission manager, receipts,
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
   bool operator==(const isolated_wasm_limits&) const = default;
};

struct isolated_notification {
   name recipient;
   // Schedule at the same point relative to canonical writes. A later RAM
   // failure must preserve the original already-scheduled notification traces.
   size_t after_writes = 0;
   bool operator==(const isolated_notification&) const = default;
};

struct isolated_action_result {
   primary_overlay_result state;
   std::vector<char> return_value;
   fc::sha256 execution_fingerprint;
   // Root notification order; root itself is implicitly already notified.
   // The owner must preflight every recipient and replay the original scheduler.
   static constexpr size_t max_recipients = 64;
   std::vector<isolated_notification> recipients;
};

// One action's owned effects within a TRANSACTION-wide journal. Notifications
// use absolute write positions, including writes by earlier actions. Iterator
// caches, VM memory/start, recipient deduplication and return bytes are local
// to this action; the state and its resource budgets are shared by all steps.
struct isolated_action_effects {
   std::vector<char> return_value;
   fc::sha256 execution_fingerprint;
   std::vector<isolated_notification> recipients;
   size_t writes_begin = 0;
   size_t writes_end = 0;
   std::chrono::microseconds elapsed{}; // private execution only, not compilation
   std::chrono::steady_clock::time_point began, ended; // local overlap diagnostics, never fingerprinted
};

struct isolated_action_invocation {
   std::vector<char> code;
   isolated_action_input input;
   isolated_wasm_limits limits;
};

struct isolated_transaction_result {
   primary_overlay_result state;
   std::vector<isolated_action_effects> actions;
   fc::sha256 execution_fingerprint;
};

// Binds successful output to the actual code, fixed action inputs and observable
// runtime limits. This is NOT authorization or canonical state validation.
fc::sha256 isolated_execution_fingerprint(const fc::sha256& code_hash, const isolated_wasm_limits&,
                                          const isolated_action_input&);
fc::sha256 isolated_transaction_fingerprint(std::span<const fc::sha256> action_fingerprints);

// Each worker OWNS its compiled module, linear memory and execution context.
// Modules remain unshared, including parsing/initialization and mutable VM
// state. Local interpreter timeouts use cooperative opcode polling. This type
// is neither copyable nor movable and is thread-affine.
// Unknown imports fail linking. Explicit link-only imports always throw when
// called (they are NOT implemented hosts). All errors require WHOLE-transaction fallback,
// never a consensus rejection based on these local capability/timeout limits.
class isolated_wasm_module {
public:
   explicit isolated_wasm_module(std::span<const char> code, isolated_wasm_limits = {});
   ~isolated_wasm_module();
   isolated_wasm_module(const isolated_wasm_module&) = delete;
   isolated_wasm_module& operator=(const isolated_wasm_module&) = delete;
   isolated_action_result execute(std::shared_ptr<const frozen_primary_state>, const isolated_action_input&,
                                  std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   friend class isolated_transaction_executor;
   // Only the two owning execution entry points may call this. An exception
   // destroys their entire local overlay; no caller can seal a partial prefix.
   isolated_action_effects execute_step(primary_state_overlay&, const isolated_action_input&,
                                       std::stop_token, isolated_cancel);
   struct impl;
   std::unique_ptr<impl> impl_;
};

// Private execution does not itself authorize canonical adoption. One bounded
// module cache per worker thread; one owned overlay per call, sealed after ALL actions
// finish. No partial result is returned on missing state, cancellation or error.
class isolated_transaction_executor {
public:
   static constexpr size_t max_actions = 16;
   static constexpr size_t max_total_code_bytes = 256 * 1024;
   static constexpr size_t max_total_action_bytes = 256 * 1024;
   static constexpr std::chrono::milliseconds max_attempt_time{500};
   isolated_transaction_executor();
   ~isolated_transaction_executor();
   isolated_transaction_executor(const isolated_transaction_executor&) = delete;
   isolated_transaction_executor& operator=(const isolated_transaction_executor&) = delete;
   isolated_transaction_result execute(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_invocation>, std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
