#include <eosio/chain/performance_metrics.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <boost/test/unit_test.hpp>
#include <thread>
#include <vector>
#include <numeric>

using namespace eosio::chain;
BOOST_AUTO_TEST_SUITE(performance_metrics_tests)

BOOST_AUTO_TEST_CASE(default_disabled_and_fixed_bucket_boundaries) {
   performance::registry local;
   BOOST_CHECK(!local.enabled.load());
   local.record(performance::stage::onblock, 0);
   local.record(performance::stage::onblock, 1000);
   local.record(performance::stage::onblock, 1001);
   local.record(performance::stage::onblock, 2'000'000'000);
   const auto s = local.snapshot(performance::stage::onblock);
   BOOST_CHECK_EQUAL(s.count, 4u);
   BOOST_CHECK_EQUAL(s.buckets[0], 2u);
   BOOST_CHECK_EQUAL(s.buckets[1], 1u);
   BOOST_CHECK_EQUAL(s.buckets.back(), 1u);
   BOOST_CHECK_EQUAL(s.nanoseconds, 2'000'002'001u);
   BOOST_CHECK_EQUAL(s.maximum_ns, 2'000'000'000u);
}

BOOST_AUTO_TEST_CASE(concurrent_recording_has_coherent_snapshots) {
   performance::registry local;
   std::vector<std::thread> workers;
   for (unsigned i = 0; i < 8; ++i)
      workers.emplace_back([&] {
         for (unsigned j = 0; j < 1000; ++j) local.record(performance::stage::vote_queue, 10);
      });
   for (auto& worker : workers) worker.join();
   const auto s = local.snapshot(performance::stage::vote_queue);
   BOOST_CHECK_EQUAL(s.count, 8000u);
   BOOST_CHECK_EQUAL(s.nanoseconds, 80000u);
   BOOST_CHECK_EQUAL(std::accumulate(s.buckets.begin(), s.buckets.end(), uint64_t{0}), s.count);
   BOOST_CHECK_EQUAL(local.snapshot(performance::stage::onblock).count, 0u);
}

BOOST_AUTO_TEST_CASE(disabled_scopes_and_exception_unwinding) {
   const auto saved = performance::metrics().enabled.exchange(false);
   const auto before = performance::metrics().snapshot(performance::stage::signature_recovery).count;
   { performance::scope unused(performance::stage::signature_recovery); }
   BOOST_CHECK_EQUAL(performance::metrics().snapshot(performance::stage::signature_recovery).count, before);
   performance::metrics().enabled = true;
   try {
      performance::scope failed(performance::stage::signature_recovery);
      throw std::runtime_error("expected");
   } catch (const std::runtime_error&) {}
   const auto after = performance::metrics().snapshot(performance::stage::signature_recovery).count;
   performance::metrics().enabled = saved;
   BOOST_CHECK_EQUAL(after, before + 1);
}

BOOST_AUTO_TEST_CASE(timing_does_not_change_validated_chain_state) {
   using namespace eosio::testing;
   struct restore_flag {
      bool saved = performance::metrics().enabled.exchange(false);
      ~restore_flag() { performance::metrics().enabled = saved; }
   } restore;
   auto genesis = base_tester::default_genesis();
   auto [priv, pub, pop] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = pub;
   genesis.initial_savanna->proof_of_possession = pop;
   fc::temp_directory source_dir, off_dir, on_dir;
   tester source(base_tester::default_config(source_dir).first, genesis);
   tester off(base_tester::default_config(off_dir).first, genesis);
   tester on(base_tester::default_config(on_dir).first, genesis);
   source.set_node_finalizers(std::array<account_name, 1>{"eosio"_n});
   source.create_account("alice"_n);
   for (unsigned n = 0; n < 12; ++n) {
      performance::metrics().enabled = false;
      auto block = source.produce_block();
      off.push_block(block);
      performance::metrics().enabled = true;
      on.push_block(block);
      BOOST_CHECK(off.control->head().id() == on.control->head().id());
      BOOST_CHECK_EQUAL(off.last_irreversible_block_num(), on.last_irreversible_block_num());
   }
   off.control->abort_block();
   on.control->abort_block();
   BOOST_CHECK(off.control->calculate_integrity_hash() == on.control->calculate_integrity_hash());
   BOOST_CHECK_GT(performance::metrics().snapshot(performance::stage::transaction_execute).count, 0u);
   BOOST_CHECK_GT(performance::metrics().snapshot(performance::stage::onblock).count, 0u);
}
BOOST_AUTO_TEST_SUITE_END()
