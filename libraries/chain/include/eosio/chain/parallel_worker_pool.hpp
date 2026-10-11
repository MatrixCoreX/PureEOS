#pragma once
#include <eosio/chain/parallel_ordered_batch.hpp>

namespace eosio::chain::experimental {

struct isolated_worker_job {
   ordered_attempt_batch::ticket ticket;
   std::vector<char> code;
   isolated_action_input input;
   isolated_wasm_limits limits;
   isolated_cancel cancel;
};

struct isolated_worker_outcome {
   std::optional<isolated_action_result> result; // empty always means original serial execution
   std::chrono::steady_clock::time_point began, ended; // local diagnostics only
   std::chrono::microseconds elapsed{}; // execution only; excludes compilation and queueing
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
