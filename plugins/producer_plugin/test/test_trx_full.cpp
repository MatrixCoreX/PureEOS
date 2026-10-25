#include <boost/test/unit_test.hpp>

#include <eosio/producer_plugin/producer_plugin.hpp>

#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>

#include <eosio/chain/genesis_state.hpp>
#include <eosio/chain/thread_utils.hpp>
#include <eosio/chain/transaction_metadata.hpp>
#include <eosio/chain/trace.hpp>
#include <eosio/chain/name.hpp>

#include <eosio/chain/application.hpp>
#include <fc/mock_time.hpp>
#include <map>

namespace eosio::test::detail {
using namespace eosio::chain::literals;
struct testit {
   uint64_t      id;

   testit( uint64_t id = 0 )
         :id(id){}

   static account_name get_account() {
      return chain::config::system_account_name;
   }

   static action_name get_name() {
      return "testit"_n;
   }
};
}
FC_REFLECT( eosio::test::detail::testit, (id) )

namespace {

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::test::detail;

auto make_unique_trx( const chain_id_type& chain_id ) {

   static uint64_t nextid = 0;
   ++nextid;

   account_name creator = config::system_account_name;

   signed_transaction trx;
   // if a transaction expires after it was aborted then it will not be included in a block
   trx.expiration = fc::time_point_sec{fc::time_point::now() + fc::seconds( nextid % 20 == 0 ? 0 : 60 )}; // fail some transactions via expired
   if( nextid % 15 == 0 ) { // fail some for invalid unlinkauth
      trx.actions.emplace_back( vector<permission_level>{{creator, config::active_name}},
                                unlinkauth{} );
      trx.actions.emplace_back( vector<permission_level>{{creator, config::active_name}},
                                testit{nextid} );
   } else {
      trx.actions.emplace_back( vector<permission_level>{{creator, config::active_name}},
                                testit{ nextid % 7 == 0 ? 0 : nextid} ); // fail some for duplicates
   }
   if( nextid % 13 == 0 ) { // fail some for invalid signature
      auto bad_priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("kevin")));
      trx.sign( bad_priv_key, chain_id );
   } else {
      auto priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("nathan")));
      trx.sign( priv_key, chain_id );
   }

   return std::make_shared<packed_transaction>( std::move(trx) );
}

// verify all trxs are in blocks only once
bool verify_equal( const std::deque<packed_transaction_ptr>& trxs, const std::deque<signed_block_ptr>& all_blocks) {
   std::set<transaction_id_type> trxs_ids; // trx can appear more than once if they were aborted
   std::set<transaction_id_type> blk_trxs_ids;

   for( const auto& trx : trxs ) {
      trxs_ids.emplace( trx->id() );
   }
   for( const auto& block : all_blocks ) {
      for( const auto& trx_receipt : block->transactions ) {
         const auto& trx = std::get<packed_transaction>( trx_receipt.trx ).get_transaction();
         blk_trxs_ids.emplace( trx.id() );
      }
   }
   BOOST_CHECK_EQUAL( trxs_ids.size(), blk_trxs_ids.size() );

   for( const auto& trx : trxs ) {
      BOOST_CHECK( blk_trxs_ids.count( trx->id() ) == 1 );
   }

   return true;
}


}

BOOST_AUTO_TEST_SUITE(ordered_trxs_full)

// Integration test of producer_plugin
// Test verifies that transactions are processed, reported to caller, and not lost
// even when blocks are aborted and some transactions fail.
void run_producer(bool parallel) {
   BOOST_REQUIRE_MESSAGE(!fc::mock_time_traits::is_set(), "real producer integration requires an advancing wall clock");
   fc::temp_directory temp;
   appbase::scoped_app app;
   
   auto temp_dir_str = temp.path().string();
   const auto genesis_path = (temp.path() / "genesis.json").string();
   auto genesis = eosio::testing::base_tester::default_genesis();
   const auto signing = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("nathan")));
   auto [bls, pub, pop] = eosio::testing::get_bls_key("eosio"_n);
   if (parallel) {
      genesis.initial_timestamp = block_timestamp_type(fc::time_point::now()).to_time_point();
      genesis.initial_key = signing.get_public_key();
      genesis.initial_savanna.emplace();
      genesis.initial_savanna->version = 2;
      genesis.initial_savanna->finalizer_key = pub;
      genesis.initial_savanna->proof_of_possession = pop;
      BOOST_REQUIRE(fc::json::save_to_file(genesis, genesis_path));
   }
   
   {
      std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
      std::future<std::tuple<producer_plugin*, chain_plugin*>> plugin_fut = plugin_promise.get_future();
      std::thread app_thread( [&]() {
         try {
            fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::debug);
            std::vector<std::string> args{
               "test", "--data-dir", temp_dir_str, "--config-dir", temp_dir_str,
               "-p", "eosio", "-e", "--disable-subjective-p2p-billing=true", "--disable-subjective-api-billing=true",
               "--chain-state-db-size-mb", "64", "--chain-state-db-guard-size-mb", "4"};
            if (parallel) {
               const std::vector<std::string> extra{
                  "--genesis-json", genesis_path,
                  "--signature-provider", genesis.initial_key.to_string({}) + "=KEY:" + signing.to_string({}),
                  "--signature-provider", pub.to_string() + "=KEY:" + bls.to_string(),
                  "--wasm-runtime", "eos-vm-jit", "--experimental-parallel-engine", "jit",
                  "--experimental-parallel-validation-threads", "4",
                  "--experimental-parallel-validation-batch-size", "32", "--experimental-parallel-production",
                  // Native/unsupported actions intentionally fall back. Live CDT
                  // benchmarks separately require genuine WASM adoption.
                  "--experimental-parallel-code-hash", std::string(64, '0')};
               args.insert(args.end(), extra.begin(), extra.end());
            }
            std::vector<const char*> argv;
            for (const auto& arg : args) argv.push_back(arg.c_str());
            app->initialize<chain_plugin, producer_plugin>( argv.size(), (char**) &argv[0] );
            app->startup();
            plugin_promise.set_value(
                  {app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()} );
            app->exec();
            return;
         } FC_LOG_AND_DROP()
         BOOST_CHECK(!"app threw exception see logged error");
      } );

      auto[prod_plug, chain_plug] = plugin_fut.get();
      auto chain_id = chain_plug->get_chain_id();

      std::deque<signed_block_ptr> all_blocks;
      std::promise<void> empty_blocks_promise;
      std::future<void> empty_blocks_fut = empty_blocks_promise.get_future();
      std::atomic<size_t> num_acked = 0;
      std::atomic<size_t> next_calls = 0;
      const size_t num_pushes = 4242;
      // Retain each request, including identical transaction IDs, so pointer
      // identity cannot be recycled. Aggregate counts alone could hide one
      // missing delivery and a second duplicate delivery.
      std::vector<packed_transaction_ptr> all_requests;
      all_requests.reserve(num_pushes);
      std::vector<size_t> callbacks_per_request(num_pushes, 0);
      std::map<const packed_transaction*, size_t> acknowledgements_per_request;
      int num_empty = 10;
      bool completion_signaled = false;
      auto ab = chain_plug->chain().accepted_block().connect( [&](const chain::block_signal_params& t) {
         const auto& [ block, id ] = t;
         all_blocks.push_back( block );
         // Empty blocks alone do not prove the signature workers and posted
         // transactions have drained, especially on a busy debug build.
         if( block->transactions.empty() && next_calls == num_pushes && num_acked == num_pushes ) {
            if( !completion_signaled && --num_empty == 0 ) {
               completion_signaled = true;
               empty_blocks_promise.set_value();
            }
         } else {
            num_empty = 10;
         }
      } );
      auto bs = chain_plug->chain().block_start().connect( [&]( uint32_t bn ) {
      } );

      plugin_interface::compat::channels::transaction_ack::channel_type::handle incoming_transaction_ack_subscription =
            app->get_channel<plugin_interface::compat::channels::transaction_ack>().subscribe(
                  [&num_acked, &acknowledgements_per_request]( const std::pair<fc::exception_ptr, packed_transaction_ptr>& t){
                     ++num_acked;
                     ++acknowledgements_per_request[t.second.get()];
                  } );

      std::deque<packed_transaction_ptr> trxs;
      std::atomic<size_t> num_posts = 0;
      std::atomic<size_t> trace_with_except = 0;
      std::atomic<bool> trx_match = true;
      for( size_t i = 1; i <= num_pushes; ++i ) {
         auto ptrx = make_unique_trx( chain_id );
         all_requests.push_back(ptrx);
         dlog( "posting ${id}", ("id", ptrx->id()) );
         app->post( priority::low, [ptrx, request_index = i - 1, &callbacks_per_request,
                                    &next_calls, &num_posts, &trace_with_except, &trx_match, &trxs, &app]() {
            ++num_posts;
            bool return_failure_traces = num_posts % 2;
            app->get_method<plugin_interface::incoming::methods::transaction_async>()(ptrx,
               bool(num_posts % 2), // mix API and P2P arrivals
               transaction_metadata::trx_type::input, // trx_type
               return_failure_traces, // return_failure_traces
               [ptrx, request_index, &callbacks_per_request, &next_calls, &trace_with_except, &trx_match, &trxs, return_failure_traces]
               (const next_function_variant<transaction_trace_ptr>& result) {
                  ++callbacks_per_request[request_index];
                  if( !std::holds_alternative<fc::exception_ptr>( result ) && !std::get<chain::transaction_trace_ptr>( result )->except ) {
                     if( std::get<chain::transaction_trace_ptr>( result )->id == ptrx->id() ) {
                        trxs.push_back( ptrx );
                     } else {
                        elog( "trace not for trx ${id}: ${t}",
                              ("id", ptrx->id())("t", fc::json::to_pretty_string(*std::get<chain::transaction_trace_ptr>(result))) );
                        trx_match = false;
                     }
                  } else if( !return_failure_traces && !std::holds_alternative<fc::exception_ptr>( result ) && std::get<chain::transaction_trace_ptr>( result )->except ) {
                     elog( "trace with except ${e}",
                           ("e", fc::json::to_pretty_string( *std::get<chain::transaction_trace_ptr>( result ) )) );
                     ++trace_with_except;
                  }
                  ++next_calls;
           });
         });
         if( i % (num_pushes/3) == 0 ) {
            // need to sleep or a fast machine might put all trxs in one block
            usleep( config::block_interval_us / 2 );
         }
         if( i % 200 == 0 ) {
            // get_integrity_hash aborts block and places aborted trxs into unapplied_transaction_queue
            // verifying that aborting block does not lose transactions
            app->post(priority::high, [&](){
               app->find_plugin<producer_plugin>()->get_integrity_hash();
            });
         }
      }

      const auto drained = empty_blocks_fut.wait_for(std::chrono::seconds(120));

      // Join before inspecting the deques: a timeout must not race with the
      // callbacks which are still appending transactions and accepted blocks.
      app->quit();
      app_thread.join();

      BOOST_CHECK_MESSAGE(drained == std::future_status::ready, "transactions did not drain within 120 seconds");
      BOOST_CHECK_EQUAL( trace_with_except, 0u ); // should not have any traces with except in it
      BOOST_CHECK( all_blocks.size() > 3u ); // should have a few blocks otherwise test is running too fast
      BOOST_CHECK_EQUAL( num_pushes, num_posts );
      BOOST_CHECK_EQUAL( num_pushes, next_calls );
      BOOST_CHECK_EQUAL( num_pushes, num_acked );
      BOOST_CHECK( trx_match.load() );
      BOOST_CHECK_EQUAL(acknowledgements_per_request.size(), num_pushes);
      for (size_t i = 0; i < num_pushes; ++i) {
         BOOST_TEST_CONTEXT("request " << i) {
            BOOST_CHECK_EQUAL(callbacks_per_request[i], 1u);
            BOOST_CHECK_EQUAL(acknowledgements_per_request[all_requests[i].get()], 1u);
         }
      }

      BOOST_REQUIRE( verify_equal(trxs, all_blocks ) );

   } 
}

BOOST_AUTO_TEST_CASE(producer) { run_producer(false); }
BOOST_AUTO_TEST_CASE(producer_parallel_fresh) { run_producer(true); }


BOOST_AUTO_TEST_SUITE_END()
