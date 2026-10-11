#include <eosio/producer_plugin/producer_plugin.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <fc/io/json.hpp>
#include <fc/mock_time.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <future>
#include <thread>

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::testing;

BOOST_AUTO_TEST_SUITE(native_savanna_producer_plugin_tests)

// Real application/producer threads and wall-clock block production, but NO
// net_plugin, HTTP listener or tester::produce_block. This is a clean restart
// test, not a substitute for OS crashes or independent P2P nodes.
BOOST_AUTO_TEST_CASE(single_node_produces_finalizes_and_restarts_without_peers) {
   BOOST_REQUIRE_MESSAGE(!fc::mock_time_traits::is_set(), "real producer integration requires an advancing wall clock");
   fc::temp_directory directory;
   const auto root = directory.path().string();
   const auto genesis_path = directory.path() / "genesis.json";
   const auto signing = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("nathan")));
   auto [bls, pub, pop] = get_bls_key("eosio"_n);
   auto genesis = base_tester::default_genesis();
   genesis.initial_timestamp = block_timestamp_type(fc::time_point::now()).to_time_point();
   genesis.initial_key = signing.get_public_key();
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->version = 2;
   genesis.initial_savanna->finalizer_key = pub;
   genesis.initial_savanna->proof_of_possession = pop;
   BOOST_REQUIRE(fc::json::save_to_file(genesis, genesis_path));

   struct checkpoint {
      uint32_t head = 1, lib = 1;
      block_id_type id;
      bool native = false, retained = false;
   } previous;
   for (unsigned phase = 0; phase < 3; ++phase) {
      appbase::scoped_app app;
      std::promise<checkpoint> promise;
      auto future = promise.get_future();
      std::atomic<bool> complete = false;
      std::thread worker([&] {
         try {
            fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::warn);
            std::vector<std::string> args{
               "native-restart-test", "--data-dir", root, "--config-dir", root,
               "-p", "eosio", "-e", "--chain-state-db-size-mb", "64",
               "--chain-state-db-guard-size-mb", "4", "--resource-monitor-space-absolute-gb", "1",
               "--signature-provider", genesis.initial_key.to_string({}) + "=KEY:" + signing.to_string({}),
               "--signature-provider", pub.to_string() + "=KEY:" + bls.to_string()};
            if (phase == 0) {
               args.push_back("--genesis-json");
               args.push_back(genesis_path.string());
            }
            std::vector<const char*> argv;
            for (const auto& arg : args) argv.push_back(arg.c_str());
            app->initialize<chain_plugin, producer_plugin>(argv.size(), const_cast<char**>(argv.data()));
            app->startup();
            auto& chain = app->get_plugin<chain_plugin>().chain();
            auto accepted = chain.accepted_block().connect([&](const block_signal_params&) {
               if (complete || chain.head().block_num() < previous.head + 8 ||
                   chain.fork_db_root().block_num() <= previous.lib) return;
               const auto old = phase ? chain.fetch_block_by_number(previous.head) : signed_block_ptr{};
               checkpoint result{chain.head().block_num(), chain.fork_db_root().block_num(), chain.head().id(),
                  chain.head().header().is_proper_svnn_block(),
                  phase == 0 || (old && old->calculate_id() == previous.id)};
               if (!complete.exchange(true)) promise.set_value(result);
            });
            app->exec();
         } catch (...) {
            if (!complete.exchange(true)) promise.set_exception(std::current_exception());
         }
      });
      const auto ready = future.wait_for(std::chrono::seconds(120));
      app->quit();
      worker.join(); // read observations only after all producer callbacks stop
      BOOST_REQUIRE_MESSAGE(ready == std::future_status::ready, "native producer failed to advance within 120 seconds");
      const auto current = future.get();
      BOOST_CHECK(current.native);
      BOOST_CHECK(current.retained);
      BOOST_CHECK_GE(current.head, previous.head + 8);
      BOOST_CHECK_GT(current.lib, previous.lib);
      BOOST_CHECK_LE(current.lib, current.head);
      BOOST_TEST_MESSAGE("Native producer phase " << phase << ": head=" << current.head << ", LIB=" << current.lib);
      previous = current;
   }
}

BOOST_AUTO_TEST_SUITE_END()
