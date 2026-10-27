#pragma once
#include <eosio/chain/controller.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string_view>

namespace eosio::chain::experimental {
// Explicit diagnostic runs only. Per-stage sums can exceed wall time when
// workers overlap. Never use these measurements to decide a test's outcome.
template<typename F>
auto parallel_test_measure(const char* label, F&& run) {
   const auto* setting = std::getenv("PUREEOS_PARALLEL_TEST_METRICS");
   if (!setting || std::string_view(setting) != "1") return std::invoke(std::forward<F>(run));
   auto& metrics = performance::metrics();
   struct restore_flag {
      std::atomic<bool>& flag;
      bool value;
      ~restore_flag() { flag.store(value, std::memory_order_relaxed); }
   } restore{metrics.enabled, metrics.enabled.exchange(true, std::memory_order_relaxed)};
   const auto compile_before = metrics.snapshot(performance::stage::parallel_compile);
   const auto execute_before = metrics.snapshot(performance::stage::parallel_execute);
   const auto began = std::chrono::steady_clock::now();
   const auto report = [&] {
      const auto compile = metrics.snapshot(performance::stage::parallel_compile);
      const auto execute = metrics.snapshot(performance::stage::parallel_execute);
      const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
      std::fprintf(stderr, "PARALLEL_TEST_METRICS %s wall_us=%lld compile_count=%llu compile_us=%llu execute_count=%llu execute_us=%llu\n",
         label, static_cast<long long>(wall.count()),
         static_cast<unsigned long long>(compile.count - compile_before.count),
         static_cast<unsigned long long>((compile.nanoseconds - compile_before.nanoseconds) / 1000),
         static_cast<unsigned long long>(execute.count - execute_before.count),
         static_cast<unsigned long long>((execute.nanoseconds - execute_before.nanoseconds) / 1000));
   };
   try {
      auto result = std::invoke(std::forward<F>(run));
      report();
      return result;
   } catch (...) { report(); throw; }
}

// Evaluated only after the joined attempt, and printed by Boost on failure.
// No timing, budget or expected-adoption assertion is relaxed by this helper.
inline std::string parallel_attempt_diagnostics(const controller::parallel_execution_stats& before,
                                                const controller::parallel_execution_stats& after) {
   std::ostringstream out;
   out << "attempts=" << after.attempts - before.attempts
       << " succeeded=" << after.succeeded - before.succeeded
       << " prepared_successes=" << after.prepared_successes - before.prepared_successes
       << " discovery_exhaustions=" << after.discovery_exhaustions - before.discovery_exhaustions
       << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
       << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
       << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks
       << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
       << " worker_vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts
       << " worker_other=" << after.worker_other_failures - before.worker_other_failures;
   out << " prefix_refresh_requests=" << after.prefix_refresh_requests - before.prefix_refresh_requests
       << " prefix_refresh_successes=" << after.prefix_refresh_successes - before.prefix_refresh_successes;
   return out.str();
}
} // namespace eosio::chain::experimental
