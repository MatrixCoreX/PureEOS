#pragma once
#include <eosio/chain/parallel_ordered_batch.hpp>

namespace eosio::chain::experimental {

struct isolated_notification_job {
   std::vector<isolated_action_input> roots;
   std::vector<isolated_receiver_program> programs;
};

struct isolated_action_tree_job {
   std::vector<isolated_action_input> roots;
   std::vector<isolated_receiver_program> programs;
   isolated_action_tree_context context;
};

struct isolated_worker_job {
   ordered_attempt_batch::ticket ticket;
   std::vector<isolated_action_invocation> actions;
   isolated_cancel cancel;
   // Mutually exclusive with actions. Every code/input byte is owned by this
   // joined job; the worker may only request another owned receiver NAME.
   std::optional<isolated_notification_job> notifications;
   std::optional<isolated_action_tree_job> tree;
   std::optional<isolated_sync_job> sync; // all four modes are mutually exclusive
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<isolated_action_invocation> calls, isolated_cancel stop = {})
      : ticket(std::move(tag)), actions(std::move(calls)), cancel(std::move(stop)) {}
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<char> code, isolated_action_input input,
                       isolated_wasm_limits limits, isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)) {
      actions.push_back({std::move(code), std::move(input), limits});
   }
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<isolated_action_input> roots,
                       std::vector<isolated_receiver_program> programs, isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)),
        notifications(isolated_notification_job{std::move(roots), std::move(programs)}) {}
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<isolated_action_input> roots,
                       std::vector<isolated_receiver_program> programs, isolated_action_tree_context context,
                       isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)),
        tree(isolated_action_tree_job{std::move(roots), std::move(programs), context}) {}
   isolated_worker_job(ordered_attempt_batch::ticket tag, isolated_action_input root,
                       std::vector<isolated_sync_program> programs, isolated_sync_context context,
                       isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)),
        sync(isolated_sync_job{std::move(root), std::move(programs), context}) {}
};

// Bounded, process-local diagnostics, never authorization, a consensus error,
// a fingerprint or a billable CPU measurement. No exception/VM ownership.
enum class isolated_worker_failure : uint8_t {
   none, cancelled, deadline, missing_state, missing_receiver, budget,
   unsupported, sync_required, invalid_attempt, vm_timeout, vm_error, allocation, unexpected
};
constexpr const char* isolated_worker_failure_name(isolated_worker_failure failure) noexcept {
   switch (failure) {
      case isolated_worker_failure::none: return "none";
      case isolated_worker_failure::cancelled: return "cancelled";
      case isolated_worker_failure::deadline: return "deadline";
      case isolated_worker_failure::missing_state: return "missing_state";
      case isolated_worker_failure::missing_receiver: return "missing_receiver";
      case isolated_worker_failure::budget: return "budget";
      case isolated_worker_failure::unsupported: return "unsupported";
      case isolated_worker_failure::sync_required: return "sync_required";
      case isolated_worker_failure::invalid_attempt: return "invalid_attempt";
      case isolated_worker_failure::vm_timeout: return "vm_timeout";
      case isolated_worker_failure::vm_error: return "vm_error";
      case isolated_worker_failure::allocation: return "allocation";
      case isolated_worker_failure::unexpected: return "unexpected";
   }
   return "unknown";
}

struct isolated_worker_outcome {
   std::optional<isolated_transaction_result> result; // legacy root-only mode
   std::optional<isolated_notification_result> notification_result; // requires whole-tree canonical preflight
   std::optional<isolated_action_tree_result> tree_result; // requires tree preflight AND original send-time authorization
   std::optional<isolated_sync_result> sync_result; // whole-root preflight and original sync contexts required; never a prefix
   std::chrono::steady_clock::time_point began, ended; // local diagnostics only
   std::chrono::microseconds elapsed{}; // successful step sum; excludes compilation and queueing
   std::optional<state_capture_request> request; // discovery only; no partial result is adoptable
   std::optional<name> receiver_request;
   isolated_worker_failure failure = isolated_worker_failure::none;
   // Whole local job including compilation, success/failure cleanup, but NOT
   // time queued before a worker takes it. elapsed above keeps its old meaning.
   std::chrono::microseconds attempt_wall{};
};

// Bounded synchronous batches on a persistent pool. The serial owner waits for
// every worker before returning to canonical execution. Jobs own all inputs;
// workers never receive controller/database pointers. Switching execution modes
// destroys the previous executor, retaining at most ONE module cache per worker.
// Sync frames still use fresh, independently owned LIVE modules, not that cache.
// Neither a missing-state nor missing-program outcome can contain a prefix.
class isolated_worker_pool {
public:
   explicit isolated_worker_pool(size_t threads);
   ~isolated_worker_pool();
   isolated_worker_pool(const isolated_worker_pool&) = delete;
   isolated_worker_pool& operator=(const isolated_worker_pool&) = delete;
   std::vector<isolated_worker_outcome> run(std::vector<isolated_worker_job> jobs);
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
