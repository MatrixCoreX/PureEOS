#pragma once
#include <eosio/chain/controller.hpp>
#include <sstream>

namespace eosio::chain::experimental {
// Evaluated only after the joined attempt, and printed by Boost on failure.
// No timing, budget or expected-adoption assertion is relaxed by this helper.
inline std::string parallel_attempt_diagnostics(const controller::parallel_execution_stats& before,
                                                const controller::parallel_execution_stats& after) {
   std::ostringstream out;
   out << "attempts=" << after.attempts - before.attempts
       << " succeeded=" << after.succeeded - before.succeeded
       << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
       << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
       << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks
       << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
       << " worker_vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts
       << " worker_other=" << after.worker_other_failures - before.worker_other_failures;
   return out.str();
}
} // namespace eosio::chain::experimental
