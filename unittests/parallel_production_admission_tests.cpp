#ifdef PUREEOS_PARALLEL_PRODUCTION_ADMISSION_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Parallel_Production_Admission
#include <boost/test/included/unit_test.hpp>
#include <eosio/chain/parallel_production_admission.hpp>
#include <limits>

using model = eosio::chain::experimental::parallel_production_admission<unsigned, 3>;
using outcome = model::outcome;
using test_clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_CASE(cold_start_and_configuration_bounds) {
   model m;
   for (size_t workers : {1u, 2u, 4u, 8u, 12u, 16u})
      for (size_t cap : {1u, 4u, 8u, 32u, 64u})
         BOOST_CHECK_EQUAL(m.recommend(1, workers, cap, 50000), std::min(workers, cap));
   BOOST_CHECK_EQUAL(m.recommend(1, 0, 32, 50000), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 17, 32, 50000), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 65, 50000), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 0), 0u);
}

BOOST_AUTO_TEST_CASE(deadlines_shrink_only_the_observed_workload) {
   model m;
   m.observe(1, 4, 32, 4, 50000, 50000, outcome::deadline);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 2u);
   BOOST_CHECK_EQUAL(m.recommend(2, 4, 32, 50000), 4u);
   m.observe(1, 4, 32, 2, 50000, 50000, outcome::deadline);
   for (unsigned i = 0; i < 100; ++i) {
      const auto now = test_clock::time_point{} + 2s * i;
      m.observe(1, 4, 32, 1, 50000, 50000, outcome::deadline, now);
      BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now), 0u);
      BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now + 1s), 1u);
   }
}

BOOST_AUTO_TEST_CASE(growth_requires_four_complete_fast_batches) {
   model m;
   for (unsigned i = 0; i < 3; ++i) {
      m.observe(1, 4, 32, 4, 5000, 50000, outcome::ready);
      BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 4u);
   }
   m.observe(1, 4, 32, 4, 5000, 50000, outcome::ready);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 8u);
   for (unsigned i = 0; i < 20; ++i) m.observe(1, 4, 32, 1, 100, 50000, outcome::ready);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 8u); // short queue cannot inflate capacity
   for (unsigned i = 0; i < 40; ++i) m.observe(1, 4, 32, 32, 8000, 50000, outcome::ready);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 32u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 4, 50000), 4u);
}

BOOST_AUTO_TEST_CASE(remaining_budget_limits_waves_and_keeps_recovery_probe) {
   model m;
   for (unsigned i = 0; i < 4; ++i) m.observe(1, 4, 32, 4, 15000, 50000, outcome::ready);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 8u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 30000), 4u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 10000), 1u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 0), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, std::numeric_limits<uint64_t>::max()), 8u);
}

BOOST_AUTO_TEST_CASE(unsupported_is_not_timeout_and_cannot_evict_good_samples) {
   model m;
   m.observe(1, 4, 32, 4, 50000, 50000, outcome::deadline);
   for (unsigned key = 2; key < 20; ++key)
      m.observe(key, 4, 32, 4, 50000, 50000, outcome::other_failure);
   m.observe(1, 4, 32, 2, 50000, 50000, outcome::other_failure);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 2u);
   BOOST_CHECK_EQUAL(m.recommend(2, 4, 32, 50000), 4u);
}

BOOST_AUTO_TEST_CASE(bounded_lru_and_code_shape_identity) {
   model m;
   for (unsigned key : {1, 2, 3, 1, 4}) m.observe(key, 4, 32, 4, 50000, 50000, outcome::deadline);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 1u);
   BOOST_CHECK_EQUAL(m.recommend(2, 4, 32, 50000), 4u); // oldest observed evicted
   BOOST_CHECK_EQUAL(m.recommend(3, 4, 32, 50000), 2u);
   BOOST_CHECK_EQUAL(m.recommend(4, 4, 32, 50000), 2u);
}

BOOST_AUTO_TEST_CASE(light_heavy_alternation_never_changes_the_hard_bounds) {
   model m;
   for (unsigned i = 0; i < 1000; ++i) {
      const auto now = test_clock::time_point{} + 2s * i;
      const auto n = m.recommend(1, 4, 32, 50000, now);
      BOOST_CHECK_GE(n, 1u); BOOST_CHECK_LE(n, 32u);
      m.observe(1, 4, 32, n, i % 7 ? 3000 : 50000, 50000,
         i % 7 ? outcome::ready : outcome::deadline, now);
      m.observe(2, 4, 32, 4, 40000, 50000, outcome::ready);
      BOOST_CHECK_EQUAL(m.recommend(2, 4, 32, 50000), 1u); // no headroom for an estimated wave
   }
}

BOOST_AUTO_TEST_CASE(single_deadline_briefly_bypasses_then_recovers_without_blocking_the_queue) {
   model m;
   const auto now = test_clock::time_point{};
   m.observe(1, 4, 32, 1, 50000, 50000, outcome::deadline, now);
   for (unsigned i = 0; i < 100; ++i)
      BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now + 1ms * i), 0u);
   BOOST_CHECK_EQUAL(m.recommend(2, 4, 32, 50000, now), 4u); // no other-workload penalty
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now + 999ms), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now + 1s), 1u);
   for (unsigned i = 0; i < 4; ++i)
      m.observe(1, 4, 32, 1, 5000, 50000, outcome::ready, now + 1s);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now + 1s), 2u);
   m.observe(1, 4, 32, 1, 50000, 50000, outcome::deadline, test_clock::time_point::max());
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, test_clock::time_point::max()), 1u); // no clock overflow
}
BOOST_AUTO_TEST_CASE(receipt_capacity_limits_preparation_but_never_changes_time_recommendation) {
   model m;
   const auto now = test_clock::time_point{};
   m.observe_receipt(1, 4, 32, 10000, now);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000, now), 4u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now), 4u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 49999, now), 3u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 12500, now), 1u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 12499, now), 0u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(2, 4, 32, 50000, 12499, now), 4u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(2, 4, 32, 50000, 0, now), 0u);
   // Same remaining wall time, next block has fresh CPU capacity: no penalty.
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now + 500ms), 4u);
}

BOOST_AUTO_TEST_CASE(receipt_cost_expires_and_handles_branch_changes_and_extreme_values) {
   model m;
   const auto now = test_clock::time_point{};
   m.observe_receipt(1, 4, 32, 200000, now);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now + 999ms), 0u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now + 1s), 4u);
   // Once expired, discard the stale cost rather than slowly decaying it.
   m.observe_receipt(1, 4, 32, 100, now + 1s);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 500, now + 1s), 4u);
   m.observe_receipt(1, 4, 32, std::numeric_limits<uint32_t>::max(), now + 1s);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, std::numeric_limits<uint64_t>::max(), now + 1s), 4u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now + 1s), 0u);
   m.observe_receipt(1, 4, 32, 100, test_clock::time_point::max());
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 100, test_clock::time_point::max()), 4u);
}

BOOST_AUTO_TEST_CASE(receipt_observation_cannot_clear_deadline_backoff_or_learn_invalid_cost) {
   model m;
   const auto now = test_clock::time_point{};
   m.observe(1, 4, 32, 1, 50000, 50000, outcome::deadline, now);
   m.observe_receipt(1, 4, 32, 100, now);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now), 0u);
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now + 1s), 1u);
   for (unsigned key = 2; key < 100; ++key) {
      m.observe_receipt(key, 4, 32, 0, now);
      m.observe_receipt(key, 4, 32, std::numeric_limits<uint64_t>::max(), now);
      m.observe_receipt(key, 0, 32, 10000, now);
      m.observe_receipt(key, 4, 65, 10000, now);
   }
   BOOST_CHECK_EQUAL(m.recommend_for_block(1, 4, 32, 50000, 200000, now + 1s), 1u);
}

BOOST_AUTO_TEST_CASE(receipt_capacity_alternation_preserves_configured_bounds_and_lru) {
   model m;
   for (unsigned i = 0; i < 1000; ++i) {
      const auto now = test_clock::time_point{} + 2s * i;
      const auto cost = i % 2 ? 100u : 20000u;
      m.observe_receipt(i % 5, 16, 64, cost, now);
      for (const uint64_t remaining : {0u, 99u, 125u, 24999u, 50000u, 200000u}) {
         const auto selected = m.recommend_for_block(i % 5, 16, 64, 50000, remaining, now);
         BOOST_CHECK_LE(selected, 16u);
         BOOST_CHECK_LE(selected * (cost + (cost + 3) / 4), remaining);
      }
   }
}
#endif // PUREEOS_PARALLEL_PRODUCTION_ADMISSION_FIXTURE
