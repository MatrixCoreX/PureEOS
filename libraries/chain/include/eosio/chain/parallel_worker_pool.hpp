#pragma once
#include <eosio/chain/parallel_ordered_batch.hpp>
#include <eosio/chain/parallel_immutable_input.hpp>

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
   bool prevalidate_tree=false; // trusted pool freezes and preflights before publishing
   // Mutually exclusive with actions. Code/input bytes have owned immutable
   // lifetime, possibly shared across joined waves; never VM/overlay/DB state.
   // The worker may only request another owned receiver NAME.
   std::optional<isolated_notification_job> notifications;
   immutable_worker_input<isolated_action_tree_job> tree;
   std::optional<isolated_sync_job> sync; // legacy single-root private API
   immutable_worker_input<isolated_sync_transaction_job> sync_transaction;
   immutable_worker_input<isolated_mixed_tree_job> mixed; // all six modes are mutually exclusive
   isolated_worker_job(ordered_attempt_batch::ticket tag, immutable_worker_input<isolated_action_tree_job> input,
                       isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)), tree(std::move(input)) {}
   isolated_worker_job(ordered_attempt_batch::ticket tag, immutable_worker_input<isolated_sync_transaction_job> input,
                       isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)), sync_transaction(std::move(input)) {}
   isolated_worker_job(ordered_attempt_batch::ticket tag, immutable_worker_input<isolated_mixed_tree_job> input,
                       isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)), mixed(std::move(input)) {}
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
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<isolated_action_input> roots,
                       std::vector<isolated_sync_program> programs, isolated_sync_context context,
                       isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)),
        sync_transaction(isolated_sync_transaction_job{std::move(roots), std::move(programs), context}) {}
   // Use an explicit bundle, avoiding an ambiguous {} context overload with
   // the existing multi-root synchronous constructor.
   isolated_worker_job(ordered_attempt_batch::ticket tag, isolated_mixed_tree_job input, isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)), mixed(std::move(input)) {}
};

// Bounded, process-local diagnostics, never authorization, a consensus error,
// a fingerprint or a billable CPU measurement. No exception/VM ownership.
enum class isolated_worker_failure : uint8_t {
   none, cancelled, deadline, missing_state, missing_receiver, budget,
   unsupported, sync_required, mixed_required, invalid_attempt, vm_timeout, vm_error, allocation, unexpected,
   environment_required
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
      case isolated_worker_failure::mixed_required: return "mixed_required";
      case isolated_worker_failure::environment_required: return "environment_required";
      case isolated_worker_failure::invalid_attempt: return "invalid_attempt";
      case isolated_worker_failure::vm_timeout: return "vm_timeout";
      case isolated_worker_failure::vm_error: return "vm_error";
      case isolated_worker_failure::allocation: return "allocation";
      case isolated_worker_failure::unexpected: return "unexpected";
   }
   return "unknown";
}


class isolated_worker_pool;

// Created only inside the trusted pool, before any outcome is published. The
// result and copied-input handle have immutable ownership and no mutable alias.
// This proves frozen journal consistency, NEVER current state or authorization.
class isolated_prevalidated_tree {
   friend class isolated_worker_pool;
   immutable_worker_input<isolated_action_tree_job> input_;
   std::optional<isolated_action_tree_result> result_;
   ordered_attempt_batch::ticket ticket_;
   std::chrono::nanoseconds cpu_{};
   isolated_prevalidated_tree(immutable_worker_input<isolated_action_tree_job> input,
      isolated_action_tree_result result,ordered_attempt_batch::ticket ticket,
      std::chrono::nanoseconds cpu)
      :input_(std::move(input)),result_(std::move(result)),ticket_(std::move(ticket)),cpu_(cpu) {}
   static std::shared_ptr<const isolated_prevalidated_tree> make(
      const immutable_worker_input<isolated_action_tree_job>&,isolated_action_tree_result&&,
      const ordered_attempt_batch::ticket&,const isolated_cancel&);
public:
   isolated_prevalidated_tree(const isolated_prevalidated_tree&)=delete;
   isolated_prevalidated_tree& operator=(const isolated_prevalidated_tree&)=delete;
   const std::optional<isolated_action_tree_result>& result()const {return result_;}
   std::chrono::nanoseconds cpu()const {return cpu_;}
   bool belongs_to(const ordered_attempt_batch::ticket& ticket)const noexcept {
      return ticket_.same_preparation(ticket);
   }
   bool matches(const isolated_action_tree_result&,std::span<const isolated_action_input>,
      std::span<const isolated_receiver_program>,const isolated_action_tree_context&,
      const std::function<void()>& checkpoint)const;
};

struct isolated_worker_outcome {
   std::optional<isolated_transaction_result> result; // legacy root-only mode
   std::optional<isolated_notification_result> notification_result; // requires whole-tree canonical preflight
   std::optional<isolated_action_tree_result> tree_result; // requires tree preflight AND original send-time authorization
   std::optional<isolated_sync_result> sync_result; // whole-root preflight and original sync contexts required; never a prefix
   std::optional<isolated_sync_transaction_result> sync_transaction_result; // whole-transaction proof, then original per-root replay
   std::optional<isolated_mixed_tree_result> mixed_result; // independent whole-tree proof/replay required
   std::shared_ptr<const isolated_prevalidated_tree> prevalidated_tree;
   const std::optional<isolated_action_tree_result>& tree_attempt()const noexcept {
      return prevalidated_tree ? prevalidated_tree->result() : tree_result;
   }
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
// destroys the previous executor but retains ONE shared nine-slot module cache
// on that worker thread; modules never migrate between worker threads.
// Sync/mixed frames exclusively lease distinct LIVE modules from the bounded
// cache; no child can reset or evict an ancestor's module.
// Neither a missing-state nor missing-program outcome can contain a prefix.
class isolated_worker_pool {
public:
   explicit isolated_worker_pool(size_t threads, isolated_vm_engine engine = isolated_vm_engine::interpreter);
   ~isolated_worker_pool();
   isolated_worker_pool(const isolated_worker_pool&) = delete;
   isolated_worker_pool& operator=(const isolated_worker_pool&) = delete;
   std::vector<isolated_worker_outcome> run(std::vector<isolated_worker_job> jobs);
   // Owner-side factory may freeze the next segment while earlier immutable
   // jobs execute. Every published job is joined even if the factory throws.
   std::vector<isolated_worker_outcome> run_streamed(std::span<const ordered_attempt_batch::ticket>,
      const std::function<isolated_worker_job(size_t)>&);
   size_t active_started_jobs()const; // owner-only local diagnostic, not a bill
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
