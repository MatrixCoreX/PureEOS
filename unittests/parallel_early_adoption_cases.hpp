#pragma once
#include <stdexcept>

// These are controller integration checks, not a network capacity benchmark.
BOOST_AUTO_TEST_SUITE(parallel_early_adoption_tests)
using parallel_production_tests::production_chain;
using parallel_production_tests::metadata;
using parallel_production_tests::production_push;
enum class scenario { independent, dependency, wrong_authority, stop_prefix, callback_throw, abort_prefix, network_interrupt, reentry };

void check_scenario(scenario which) {
   const auto code=token_fixture_code();
   const auto hash=fc::sha256::hash(reinterpret_cast<const char*>(code.data()),code.size());
   for(bool enabled:{false,true}) {
      fc::temp_directory source_dir,reference_dir;
      auto cfg=base_tester::default_config(source_dir).first;
      auto oracle=base_tester::default_config(reference_dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg);
      eosio::testing::configure_parallel_test_runtime(oracle);
      cfg.parallel_validation_threads=2;cfg.parallel_validation_batch_size=24;
      cfg.parallel_production_enabled=true;cfg.parallel_production_early_adoption=enabled;
      cfg.parallel_code_allowlist={hash};
      production_chain source(cfg,parallel_multitoken_tests::genesis());
      tester reference(oracle,parallel_multitoken_tests::genesis());
      source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      parallel_multitoken_tests::setup(source,code);
      const auto setup=source.produce_block();source.control->abort_block();
      reference.push_block(setup);reference.control->abort_block();source.start();
      const auto unchanged=reference.control->calculate_integrity_hash();
      auto first=parallel_multitoken_tests::transfer(source,"tokenone"_n,"alice"_n,"bob"_n,100,890001);
      auto second=which==scenario::dependency ?
         parallel_multitoken_tests::transfer(source,"tokenone"_n,"bob"_n,"carol"_n,20,890002) :
         parallel_multitoken_tests::transfer(source,"tokentwo"_n,"carol"_n,"dave"_n,30,890002);
      if(which==scenario::wrong_authority) {
         second.signatures.clear();
         second.sign(source.get_private_key("alice"_n,"active"),source.control->get_chain_id());
      }
      const std::vector<transaction_metadata_ptr> batch{metadata(source,first),metadata(source,second)};
      size_t callbacks=0,successful=0,rejected=0;
      const auto before=source.control->get_parallel_production_stats();
      const auto callback=[&](const transaction_metadata_ptr& item) {
         const auto i=callbacks++;
         BOOST_REQUIRE(i<batch.size());BOOST_CHECK(item==batch[i]);
         if(which==scenario::callback_throw && i==1)throw std::runtime_error("formal callback failure");
         if(which==scenario::reentry) {
            BOOST_CHECK_THROW(source.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),
               [](const auto&){return true;},fc::milliseconds(100)),transaction_exception);
         }
         const auto trace=production_push(source,item,fc::milliseconds(100));
         BOOST_REQUIRE(trace);
         if(which==scenario::wrong_authority && i==1) {
            BOOST_REQUIRE(trace->except);++rejected;
         } else {
            BOOST_REQUIRE(!trace->except);BOOST_REQUIRE(trace->receipt);++successful;
            BOOST_CHECK_GE(trace->receipt->cpu_usage_us,100u);
         }
         if(which==scenario::abort_prefix)source.control->abort_block();
         if(which==scenario::network_interrupt)
            source.control->interrupt_transaction(controller::interrupt_t::speculative_block_trx);
         return which!=scenario::stop_prefix && which!=scenario::abort_prefix;
      };
      if(which==scenario::callback_throw) {
         BOOST_CHECK_THROW(source.control->for_each_parallel_production_transaction(batch,
            fc::time_point::maximum(),callback,fc::milliseconds(100)),std::runtime_error);
      } else {
         const size_t expected=(which==scenario::stop_prefix || which==scenario::abort_prefix ||
            which==scenario::network_interrupt)?1:2;
         BOOST_CHECK_EQUAL(source.control->for_each_parallel_production_transaction(batch,
            fc::time_point::maximum(),callback,fc::milliseconds(100)),expected);
         BOOST_CHECK_EQUAL(callbacks,expected);
      }
      const auto after=source.control->get_parallel_production_stats();
      if(enabled)BOOST_CHECK_GT(after.early_ready_batches,before.early_ready_batches);
      else BOOST_CHECK_EQUAL(after.early_ready_batches,before.early_ready_batches);
      if(which==scenario::abort_prefix) {
         BOOST_CHECK(source.control->calculate_integrity_hash()==unchanged);
         BOOST_CHECK(source.control->calculate_integrity_hash()==reference.control->calculate_integrity_hash());
         source.start();
      } else {
         const auto block=source.finish_block();
         BOOST_REQUIRE_EQUAL(block->transactions.size(),successful);
         reference.push_block(block);reference.control->abort_block();
         BOOST_CHECK(source.head().id()==reference.head().id());
         BOOST_CHECK(source.control->calculate_integrity_hash()==reference.control->calculate_integrity_hash());
         source.start();
      }
      // The pool/coordinator must be reusable after stop, exception, undo or
      // P2P-style interrupt, without reusing an outcome from the old prefix.
      const auto next=metadata(source,parallel_multitoken_tests::transfer(source,
         "tokenone"_n,"dave"_n,"alice"_n,7,890099));
      const auto stats_before_next=source.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(source.control->for_each_parallel_production_transaction({next},
         fc::time_point::maximum(),[&](const auto& trx) {
            const auto trace=production_push(source,trx,fc::milliseconds(100));
            BOOST_REQUIRE(!trace->except);BOOST_REQUIRE(trace->receipt);return true;
         },fc::milliseconds(100)),1u);
      if(enabled)BOOST_CHECK_GT(source.control->get_parallel_production_stats().early_ready_callbacks,
         stats_before_next.early_ready_callbacks);
      const auto block=source.finish_block();
      reference.push_block(block);reference.control->abort_block();
      BOOST_CHECK(source.head().id()==reference.head().id());
      BOOST_CHECK(source.control->calculate_integrity_hash()==reference.control->calculate_integrity_hash());
      for(const auto contract:parallel_multitoken_tests::token_contracts) {
         BOOST_CHECK_EQUAL(parallel_multitoken_tests::supply(source,contract),parallel_multitoken_tests::supply(reference,contract));
         for(const auto user:parallel_multitoken_tests::token_users)
            BOOST_CHECK_EQUAL(parallel_multitoken_tests::balance(source,contract,user),parallel_multitoken_tests::balance(reference,contract,user));
      }
      BOOST_CHECK_EQUAL(rejected,which==scenario::wrong_authority?1u:0u);
   }
}
BOOST_AUTO_TEST_CASE(real_tokens_and_fresh_serial_replay){check_scenario(scenario::independent);}
BOOST_AUTO_TEST_CASE(dependency_joins_before_fresh_prefix){check_scenario(scenario::dependency);}
BOOST_AUTO_TEST_CASE(original_authority_rejects_without_mutation){check_scenario(scenario::wrong_authority);}
BOOST_AUTO_TEST_CASE(callback_stop_preserves_exact_consumed_prefix){check_scenario(scenario::stop_prefix);}
BOOST_AUTO_TEST_CASE(callback_exception_is_not_swallowed_by_preparation){check_scenario(scenario::callback_throw);}
BOOST_AUTO_TEST_CASE(block_abort_discards_owned_results_and_undoes_prefix){check_scenario(scenario::abort_prefix);}
BOOST_AUTO_TEST_CASE(network_priority_interrupt_stops_stale_owner_prefix){check_scenario(scenario::network_interrupt);}
BOOST_AUTO_TEST_CASE(owner_callback_reentry_remains_rejected){check_scenario(scenario::reentry);}
BOOST_AUTO_TEST_SUITE_END()
