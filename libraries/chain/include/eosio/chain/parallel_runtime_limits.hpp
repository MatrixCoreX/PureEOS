#pragma once

#include <chrono>
#include <cstddef>

namespace eosio::chain::experimental {

// Process-local admission and memory limits, NOT chain capacity or CPU billing.
// No value here enters a transaction fingerprint, receipt, genesis or snapshot.
// Exceeding a preparation budget routes work to canonical execution; it cannot
// make an otherwise valid block invalid. A later larger-core implementation
// must review these budgets together, not just increase a CLI thread count.
struct parallel_runtime_limits {
   static constexpr size_t max_workers = 16;
   static constexpr size_t max_batch_transactions = 64;
   static constexpr size_t max_discovery_rounds = 64; // independent of batch size
   static constexpr size_t max_retained_snapshot_bytes = 4 * 1024 * 1024;
   static constexpr size_t max_environment_wire_bytes = 4 * 1024 * 1024;
   static constexpr auto preparation_timeout = std::chrono::milliseconds(500);
   static constexpr auto production_timeout = std::chrono::milliseconds(50);

   static constexpr bool valid_workers(size_t count) noexcept {
      return count > 0 && count <= max_workers;
   }
   static constexpr bool valid_batch(size_t count) noexcept {
      return count > 0 && count <= max_batch_transactions;
   }
   static constexpr bool valid_configuration(size_t workers, size_t batch) noexcept {
      return (workers == 0 || valid_workers(workers)) && valid_batch(batch);
   }
};

static_assert(parallel_runtime_limits::max_workers <= parallel_runtime_limits::max_batch_transactions);
static_assert(parallel_runtime_limits::max_batch_transactions <= 65535); // public config uses uint16_t
static_assert(parallel_runtime_limits::production_timeout <= parallel_runtime_limits::preparation_timeout);

} // namespace eosio::chain::experimental
