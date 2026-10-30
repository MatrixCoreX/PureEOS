#ifdef PUREEOS_PARALLEL_PRODUCTION_ADMISSION_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Parallel_Production_Admission
#include <boost/test/included/unit_test.hpp>
#include <eosio/chain/parallel_production_admission.hpp>
#include <limits>
#include <eosio/chain/parallel_production_budget.hpp>

using model = eosio::chain::experimental::parallel_production_admission<unsigned, 3>;
using outcome = model::outcome;
using test_clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_CASE(cold_start_and_configuration_bounds) {
   model m;
   for (size_t workers : {1u, 2u, 4u, 8u, 12u, 16u, 32u, 64u, 128u})
      for (size_t cap : {1u, 4u, 8u, 32u, 64u, 96u, 128u, 256u})
         BOOST_CHECK_EQUAL(m.recommend(1, workers, cap, 50000), std::min(workers, cap));
   BOOST_CHECK_EQUAL(m.recommend(1, 0, 32, 50000), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 129, 32, 50000), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 257, 50000), 0u);
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

BOOST_AUTO_TEST_CASE(large_pool_growth_and_deadlines_keep_the_batch_bounded) {
   model m;
   const auto now = test_clock::time_point{};
   BOOST_CHECK_EQUAL(m.recommend(1, 128, 256, 50000, now), 128u);
   for (unsigned i = 0; i < 4; ++i)
      m.observe(1, 128, 256, 128, 1000, 50000, outcome::ready, now);
   BOOST_CHECK_EQUAL(m.recommend(1, 128, 256, 50000, now), 256u);
   m.observe(1, 128, 256, 256, 50000, 50000, outcome::deadline, now);
   BOOST_CHECK_EQUAL(m.recommend(1, 128, 256, 50000, now), 128u);
   m.observe(1, 128, 256, 128, 50000, 50000, outcome::deadline, now);
   BOOST_CHECK_EQUAL(m.recommend(1, 128, 256, 50000, now), 64u);
   BOOST_CHECK_EQUAL(m.recommend(2, 128, 256, 50000, now), 128u);
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
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 10000), 0u);
   BOOST_CHECK_EQUAL(m.recommend(1, 4, 32, 50000), 8u);
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
      m.observe_receipt(key, 4, 257, 10000, now);
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

using mixed_model=eosio::chain::experimental::parallel_mixed_head_admission<unsigned,3>;
using mixed_outcome=mixed_model::outcome;
static constexpr auto unlimited_cpu=std::numeric_limits<uint64_t>::max();

BOOST_AUTO_TEST_CASE(mixed_hints_are_separate_and_require_complete_fast_observations) {
   mixed_model m;model original;const auto now=test_clock::time_point{};
   BOOST_CHECK(!m.recommend_for_block(1,8,24,50000,unlimited_cpu,now));
   for(unsigned i=0;i<8;++i)m.observe(1,false,8,24,8,1000,50000,mixed_outcome::ready,now);
   BOOST_CHECK(!m.recommend_for_block(1,8,24,50000,unlimited_cpu,now));
   for(unsigned i=0;i<3;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
   BOOST_REQUIRE((m.recommend_for_block(1,8,24,50000,unlimited_cpu,now)).has_value());
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,unlimited_cpu,now),8u);
   m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,unlimited_cpu,now),16u);
   for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,16,1000,50000,mixed_outcome::ready,now);
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,unlimited_cpu,now),24u);
   BOOST_CHECK_EQUAL(original.recommend(1,8,24,50000,now),8u);
   BOOST_CHECK(!m.recommend_for_block(2,8,24,50000,unlimited_cpu,now));
}
BOOST_AUTO_TEST_CASE(mixed_deadline_receipts_and_ttl_cannot_inflate_admission) {
   mixed_model m;const auto now=test_clock::time_point{};
   for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
   m.observe(1,true,8,24,16,50000,50000,mixed_outcome::deadline,now);
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,unlimited_cpu,now),8u);
   m.observe_receipt(1,8,24,10000,now+100ms);
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,24999,now+100ms),1u);
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,12499,now+100ms),0u);
   BOOST_CHECK(!m.recommend_for_block(1,8,24,50000,unlimited_cpu,now+1s));
   m.observe_receipt(1,8,24,1,now+2s);
   BOOST_CHECK(!m.recommend_for_block(1,8,24,50000,unlimited_cpu,now+2s));
   m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now+2s);
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,8,24,50000,unlimited_cpu,now+2s),8u);
   m.forget(1);BOOST_CHECK(!m.recommend_for_block(1,8,24,50000,unlimited_cpu,now+2s));
}
BOOST_AUTO_TEST_CASE(mixed_selection_preserves_original_backoff_and_receipt_ceiling) {
   using eosio::chain::experimental::recommend_parallel_production_admission;
   mixed_model m;model original;const auto now=test_clock::time_point{};
   for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
   for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,16,1000,50000,mixed_outcome::ready,now);
   BOOST_CHECK_EQUAL(recommend_parallel_production_admission(original,m,1u,8,24,50000,unlimited_cpu,now).second,24u);
   original.observe_receipt(1,8,24,10000,now);
   BOOST_CHECK_EQUAL(recommend_parallel_production_admission(original,m,1u,8,24,50000,24999,now).second,1u);
   BOOST_CHECK_EQUAL(recommend_parallel_production_admission(original,m,1u,8,24,50000,12499,now).second,0u);
   original.observe(1,8,24,1,50000,50000,outcome::deadline,now);
   BOOST_CHECK_EQUAL(recommend_parallel_production_admission(original,m,1u,8,24,50000,unlimited_cpu,now).second,0u);
   BOOST_CHECK_EQUAL(recommend_parallel_production_admission(original,m,1u,8,24,0,unlimited_cpu,now).second,0u);
}
BOOST_AUTO_TEST_CASE(mixed_invalid_or_unsupported_samples_do_not_evict_or_refresh_hints) {
   mixed_model m;const auto now=test_clock::time_point{};
   m.observe(1,true,4,32,4,50000,50000,mixed_outcome::deadline,now);
   for(unsigned i=2;i<100;++i) {
      m.observe(i,true,4,32,4,50000,50000,mixed_outcome::other_failure,now);
      m.observe(i,true,0,32,4,1,50000,mixed_outcome::ready,now);
      m.observe(i,true,4,257,4,1,50000,mixed_outcome::ready,now);
      m.observe(i,true,4,32,0,1,50000,mixed_outcome::ready,now);
      m.observe(i,true,4,32,33,1,50000,mixed_outcome::ready,now);
   }
   BOOST_CHECK_EQUAL(*m.recommend_for_block(1,4,32,50000,unlimited_cpu,now),2u);
   for(unsigned key:{2,3,1,4})m.observe(key,true,4,32,4,50000,50000,mixed_outcome::deadline,now);
   BOOST_CHECK(!m.recommend_for_block(2,4,32,50000,unlimited_cpu,now));
   BOOST_CHECK((m.recommend_for_block(1,4,32,50000,unlimited_cpu,now)).has_value());
}
BOOST_AUTO_TEST_CASE(mixed_alternation_and_clock_extremes_keep_all_configured_bounds) {
   mixed_model m;
   for(unsigned i=0;i<1000;++i) {
      const auto now=test_clock::time_point{}+2s*i;
      for(unsigned j=0;j<4;++j)m.observe(i%5,true,128,256,128,1000,50000,mixed_outcome::ready,now);
      const auto n=m.recommend_for_block(i%5,128,256,50000,unlimited_cpu,now);
      BOOST_REQUIRE(n.has_value());BOOST_CHECK_GE(*n,1u);BOOST_CHECK_LE(*n,256u);
      m.observe(i%5,true,128,256,*n,50000,50000,mixed_outcome::deadline,now);
      BOOST_CHECK_LE(*m.recommend_for_block(i%5,128,256,50000,unlimited_cpu,now),128u);
   }
   m.observe(9,true,8,24,8,1000,50000,mixed_outcome::ready,test_clock::time_point::max());
   BOOST_CHECK(!m.recommend_for_block(9,8,24,50000,unlimited_cpu,test_clock::time_point::max()));
}


static model warm_model() {
 model m;const auto now=test_clock::time_point{};
 for(unsigned i=0;i<4;++i)m.observe(1,8,24,8,800,50000,outcome::ready,now);
 for(unsigned i=0;i<4;++i)m.observe(1,8,24,16,1600,50000,outcome::ready,now);
 return m;
}
BOOST_AUTO_TEST_CASE(block_tail_unknown_shapes_wait_for_real_discovery_window) {
 model m;const auto now=test_clock::time_point{};
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,1,now),1u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,49999,now),1u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now+500ms),8u);
}
BOOST_AUTO_TEST_CASE(block_tail_cannot_fit_skips_attempt_and_keeps_learned_target) {
 const auto tiny=eosio::chain::experimental::production_preparation_budget_us(105001,200000,6400000,100000);
 const auto healthy=eosio::chain::experimental::production_preparation_budget_us(500000,200000,6400000,100000);
 BOOST_REQUIRE_EQUAL(tiny,1);BOOST_REQUIRE_EQUAL(healthy,50000);
 auto m=warm_model();const auto now=test_clock::time_point{};
 BOOST_REQUIRE_EQUAL(m.recommend(1,8,24,50000,now),24u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,1,now),0u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now+500ms),24u);
}
BOOST_AUTO_TEST_CASE(block_tail_censored_deadline_does_not_downgrade_next_block) {
 auto m=warm_model();const auto now=test_clock::time_point{};
 m.observe(1,8,24,1,1,1,outcome::deadline,now+1ms);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now+500ms),24u);
}
BOOST_AUTO_TEST_CASE(block_tail_censored_observations_cannot_evict_or_refresh_cost_history) {
 auto m=warm_model();const auto now=test_clock::time_point{};
 for(unsigned key=2;key<100;++key)m.observe(key,8,24,1,1,1,outcome::deadline,now);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now),24u);
}
BOOST_AUTO_TEST_CASE(block_tail_receipts_still_limit_cpu_and_do_not_clear_real_backoff) {
 auto m=warm_model();const auto now=test_clock::time_point{};
 m.observe_receipt(1,8,24,100,now);
 BOOST_CHECK_EQUAL(m.recommend_for_block(1,8,24,50000,124,now),0u);
 BOOST_CHECK_EQUAL(m.recommend_for_block(1,8,24,50000,125,now),1u);
 m.observe(1,8,24,1,50000,50000,outcome::deadline,now+1ms);
 m.observe(1,8,24,1,1,1,outcome::deadline,now+2ms);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now+500ms),0u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now+1002ms),1u);
}
BOOST_AUTO_TEST_CASE(block_tail_full_budget_never_loses_slow_shape_recovery_probe) {
 model m;const auto now=test_clock::time_point{};
 m.observe(1,8,24,8,1000000,50000,outcome::ready,now);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,50000,now),1u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,49999,now),0u);
 for(unsigned i=0;i<64;++i)m.observe(1,8,24,1,100,50000,outcome::ready,now);
 BOOST_CHECK_GE(m.recommend(1,8,24,50000,now),1u);
}
BOOST_AUTO_TEST_CASE(block_tail_known_fast_wave_can_still_use_sufficient_short_window) {
 auto m=warm_model();const auto now=test_clock::time_point{};
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,2000,now),8u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,4000,now),24u);
 BOOST_CHECK_EQUAL(m.recommend(1,8,24,0,now),0u);
}
BOOST_AUTO_TEST_CASE(block_tail_mixed_deadline_does_not_renew_a_success_hint) {
 mixed_model m;const auto now=test_clock::time_point{};
 for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
 m.observe(1,true,8,24,16,1,1,mixed_outcome::deadline,now+900ms);
 BOOST_CHECK(!m.recommend_for_block(1,8,24,50000,unlimited_cpu,now+1s).has_value());
}
BOOST_AUTO_TEST_CASE(block_tail_mixed_hints_cannot_override_homogeneous_budget_deferral) {
 using eosio::chain::experimental::recommend_parallel_production_admission;
 auto h=warm_model();mixed_model m;const auto now=test_clock::time_point{};
 for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
 auto n=recommend_parallel_production_admission(h,m,1u,8,24,1,unlimited_cpu,now);
 BOOST_CHECK_EQUAL(n.first,0u);BOOST_CHECK_EQUAL(n.second,0u);
 BOOST_CHECK_GT((recommend_parallel_production_admission(h,m,1u,8,24,50000,unlimited_cpu,now+500ms)).second,0u);
}


BOOST_AUTO_TEST_CASE(known_mixture_can_admit_a_short_window_without_homogeneous_cost_samples) {
 using eosio::chain::experimental::recommend_parallel_production_admission;
 model h;mixed_model m;const auto now=test_clock::time_point{};
 h.observe_receipt(1,8,24,100,now);
 for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
 auto n=recommend_parallel_production_admission(h,m,1u,8,24,10000,unlimited_cpu,now);
 BOOST_CHECK_EQUAL(n.first,16u);BOOST_CHECK_EQUAL(n.second,16u);
}
BOOST_AUTO_TEST_CASE(known_mixture_short_window_cannot_bypass_a_real_backoff_or_receipt_ceiling) {
 using eosio::chain::experimental::recommend_parallel_production_admission;
 model h;mixed_model m;const auto now=test_clock::time_point{};
 h.observe_receipt(1,8,24,100,now);
 for(unsigned i=0;i<4;++i)m.observe(1,true,8,24,8,1000,50000,mixed_outcome::ready,now);
 BOOST_CHECK_EQUAL((recommend_parallel_production_admission(h,m,1u,8,24,10000,124,now)).second,0u);
 h.observe(1,8,24,1,50000,50000,outcome::deadline,now+1ms);
 BOOST_CHECK_EQUAL((recommend_parallel_production_admission(h,m,1u,8,24,10000,unlimited_cpu,now+500ms)).second,0u);
 BOOST_CHECK_EQUAL((recommend_parallel_production_admission(h,m,1u,8,24,50000,unlimited_cpu,now+500ms)).second,0u);
}

#endif // PUREEOS_PARALLEL_PRODUCTION_ADMISSION_FIXTURE
