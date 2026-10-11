#pragma once
#include <eosio/chain/parallel_ordered_batch.hpp>

namespace eosio::chain::experimental {

struct isolated_worker_job {
   ordered_attempt_batch::ticket ticket;
   std::vector<isolated_action_invocation> actions;
   isolated_cancel cancel;
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<isolated_action_invocation> calls, isolated_cancel stop = {})
      : ticket(std::move(tag)), actions(std::move(calls)), cancel(std::move(stop)) {}
   isolated_worker_job(ordered_attempt_batch::ticket tag, std::vector<char> code, isolated_action_input input,
                       isolated_wasm_limits limits, isolated_cancel stop = {})
      : ticket(std::move(tag)), cancel(std::move(stop)) {
      actions.push_back({std::move(code), std::move(input), limits});
   }
};

struct isolated_worker_outcome {
   std::optional<isolated_transaction_result> result; // empty always means original serial execution
   std::chrono::steady_clock::time_point began, ended; // local diagnostics only
   std::chrono::microseconds elapsed{}; // successful step sum; excludes compilation and queueing
   std::optional<state_capture_request> request; // discovery only; no partial result is adoptable
};

// Bounded synchronous batches on a persistent pool. The serial owner waits for
// every worker before returning to canonical execution. Jobs own all inputs;
// workers never receive controller/database pointers. One module cache per
// worker is bounded by max_code_bytes; invalid jobs cannot grow it indefinitely.
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
