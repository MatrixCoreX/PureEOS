#pragma once
#include <eosio/chain/controller.hpp>
#include <cstddef>

namespace eosio::chain::experimental {
// Test policy only. Production keeps the same absolute 50/500ms deadlines.
// Instrumentation can cause the coordinator to take its supported serial path.
// This predicate never substitutes for the caller's receipt/state/RAM/undo
// assertions, nor for ordinary builds' exact adoption expectations.
inline bool full_cdt_deadline_fallback_valid(const controller::parallel_execution_stats& before,
                                           const controller::parallel_execution_stats& after,
                                           size_t adopted, size_t fallback, size_t prepared) {
   const auto ordered = [&] (auto member) { return after.*member >= before.*member; };
   using stats = controller::parallel_execution_stats;
   for (auto member : {&stats::attempts, &stats::adopted, &stats::serial_fallbacks, &stats::prepared_successes,
                      &stats::preparation_deadlines, &stats::worker_deadlines, &stats::worker_vm_timeouts,
                      &stats::preparation_other_fallbacks, &stats::discovery_exhaustions})
      if (!ordered(member)) return false;
   const bool expired = after.preparation_deadlines > before.preparation_deadlines ||
      after.worker_deadlines > before.worker_deadlines || after.worker_vm_timeouts > before.worker_vm_timeouts;
   return expired && after.attempts > before.attempts &&
      after.adopted - before.adopted <= adopted &&
      after.serial_fallbacks - before.serial_fallbacks <= adopted + fallback &&
      after.prepared_successes - before.prepared_successes <= prepared &&
      after.discovery_exhaustions == before.discovery_exhaustions &&
      after.preparation_other_fallbacks == before.preparation_other_fallbacks;
}
} // namespace eosio::chain::experimental
