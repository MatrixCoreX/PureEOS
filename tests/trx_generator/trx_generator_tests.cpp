#include <trx_provider.hpp>
#include <trx_generator.hpp>
#include <http_client_async.hpp>
#include <simple_rest_server.hpp>
#include <fc/io/json.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/raw.hpp>
#include <boost/asio/read.hpp>
#include <sys/stat.h>

#define BOOST_TEST_MODULE trx_generator_tests
#include <boost/test/included/unit_test.hpp>

using namespace eosio;
using namespace eosio::testing;
using namespace std::literals::string_literals;

static const char* api_name = "/v1/chain/test";

namespace http = boost::beast::http;
struct echo_server_impl : rest::simple_server<echo_server_impl> {

   std::string server_header() const { return "/"; }

   void log_error(char const* what, const std::string& message) {
      elog("${what}: ${message}", ("what", what)("message", message));
   }

   bool allow_method(http::verb method) const { return method == http::verb::post; }

   std::optional<http::response<http::string_body>> on_request(http::request<http::string_body>&& req) {
      if (req.target() != api_name)
         return {};
      http::response<http::string_body> res{http::status::ok, req.version()};
      // Respond to POST request
      res.set(http::field::server, server_header());
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      // echo request body back in response body
      res.body() = req.body();
      res.prepare_payload();
      return res;
   }

   eosio::chain::named_thread_pool<struct trxgen> _trx_gen_server_thread_pool;
   boost::asio::io_context::strand                _trx_gen_server_strand;

   echo_server_impl()
       : _trx_gen_server_strand(_trx_gen_server_thread_pool.get_executor()) {}

   void start(boost::asio::ip::tcp::endpoint endpoint) {
      run(_trx_gen_server_thread_pool.get_executor(), endpoint);
      _trx_gen_server_thread_pool.start(
          1, [](const fc::exception& e) { elog("Trx gen http server exception ${e}", ("e", e)); });
   }

   void shutdown() {
      _trx_gen_server_thread_pool.stop();
      ilog("echo_server_impl shutdown.");
   }
};

struct simple_tps_monitor {
   std::vector<tps_test_stats> _calls;
   bool monitor_test(const tps_test_stats& stats) {
      _calls.push_back(stats);
      return true;
   }

   simple_tps_monitor(size_t expected_num_calls) { _calls.reserve(expected_num_calls); }
};

struct mock_trx_generator {
   std::vector<fc::time_point> _calls;
   std::chrono::microseconds _delay;

   bool setup() {return true;}
   bool tear_down() {return true;}

   bool generate_and_send() {
      _calls.push_back(fc::time_point::now());
      if (_delay.count() > 0) {
         std::this_thread::sleep_for(_delay);
      }
      return true;
   }

   bool stop_on_trx_fail() {
      return false;
   }

   mock_trx_generator(size_t expected_num_calls, uint32_t delay=0) :_calls(), _delay(delay) {
      _calls.reserve(expected_num_calls);
   }
};

BOOST_AUTO_TEST_SUITE(trx_generator_tests)

#if defined(__linux__) // Evidence mode explicitly requires Linux CLOCK_MONOTONIC + boot identity.
BOOST_AUTO_TEST_CASE(send_evidence_is_exclusive_private_and_clock_ordered) {
   fc::temp_directory directory;
   const auto path = (directory.path() / "sends.jsonl").string();
   const auto before = benchmark_monotonic_ns();
   {
      send_evidence journal(path);
      BOOST_CHECK_THROW(send_evidence second(path), std::runtime_error);
      journal.sent(std::string(64, 'a'), before, 1);
      journal.error();
      journal.finish(1, 1, 1);
   }
   struct stat mode{};
   BOOST_REQUIRE_EQUAL(::stat(path.c_str(), &mode), 0);
   BOOST_CHECK_EQUAL(mode.st_mode & 0777, 0600);
   std::ifstream input(path);
   std::string line;
   uint64_t previous = before;
   unsigned count = 0;
   while (std::getline(input, line)) {
      auto record = fc::json::from_string(line);
      const auto at = record["at_ns"].as_uint64();
      BOOST_CHECK_GE(at, previous);
      previous = at;
      ++count;
   }
   BOOST_CHECK_EQUAL(count, 4u);
   BOOST_CHECK_LE(previous, benchmark_monotonic_ns());
}

BOOST_AUTO_TEST_CASE(send_evidence_flush_failure_is_not_success) {
   fc::temp_directory directory;
   const auto path = (directory.path() / "write-failure.jsonl").string();
   send_evidence journal(path);
   // Linux-only evidence feature: replace ONLY this freshly created journal's
   // exact descriptor with /dev/full. No process-wide limits or signal changes.
   int journal_fd = -1;
   for (int fd = 3; fd < 256; ++fd) {
      char target[4096];
      const auto link = "/proc/self/fd/" + std::to_string(fd);
      const auto size = ::readlink(link.c_str(), target, sizeof(target));
      if (size > 0 && std::string(target, size) == path) { journal_fd = fd; break; }
   }
   BOOST_REQUIRE_GE(journal_fd, 3);
   const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
   BOOST_REQUIRE_GE(full, 0);
   const int replaced = ::dup2(full, journal_fd);
   ::close(full);
   BOOST_REQUIRE_EQUAL(replaced, journal_fd);
   BOOST_CHECK_THROW(journal.finish(0, 0, 0), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(account_config_does_not_log_signing_keys) {
   accounts_config config;
   config._priv_keys_vec.emplace_back(chain::private_key_type::generate());
   const auto secret = config._priv_keys_vec.front().to_string({});
   BOOST_CHECK(config.to_string().find(secret) == std::string::npos);
   user_specified_trx_config user;
   user._actions_auths_json_file_or_str = "SECRET_AUTH";
   BOOST_CHECK(user.to_string().find("SECRET_AUTH") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(pureeos_transfer_symbol_is_explicit_and_legacy_default_survives) {
   for (const std::string quantity : {"1.0000 CUR", "1.0000 SYS"}) {
      accounts_config accounts;
      BOOST_CHECK_EQUAL(accounts._transfer_quantity, "1.0000 CUR");
      accounts._transfer_quantity = quantity;
      accounts._acct_name_vec = {chain::name("alice"), chain::name("bob")};
      accounts._priv_keys_vec = {chain::private_key_type::generate(), chain::private_key_type::generate()};
      trx_generator_base_config base{};
      base._contract_owner_account = chain::name("eosio.token");
      provider_base_config provider;
      transfer_trx_generator generator(base, provider, accounts);
      generator.create_initial_transfer_actions("test", 20);
      BOOST_REQUIRE_EQUAL(generator._action_pairs_vector.size(), 1u);
      const auto& action = generator._action_pairs_vector[0]._first_act;
      fc::datastream<const char*> data(action.data.data(), action.data.size());
      chain::name from, to;
      chain::asset amount;
      fc::raw::unpack(data, from);
      fc::raw::unpack(data, to);
      fc::raw::unpack(data, amount);
      BOOST_CHECK(from == chain::name("alice"));
      BOOST_CHECK(to == chain::name("bob"));
      BOOST_CHECK_EQUAL(amount.to_string(), quantity);
   }
}

BOOST_AUTO_TEST_CASE(p2p_bound_rejects_without_unbounded_queue) {
   fc::temp_directory directory;
   provider_base_config config;
   config._evidence_log = (directory.path() / "sends.jsonl").string();
   config._max_pending_sends = 1;
   p2p_connection connection(config);
   // Do not start a worker: deterministically fill exactly one pending slot.
   chain::signed_transaction trx;
   connection.send_transaction(chain::packed_transaction(trx));
   BOOST_CHECK_THROW(connection.send_transaction(chain::packed_transaction(trx)), std::runtime_error);
   BOOST_CHECK_EQUAL(connection._sent.load(), 1u);
   BOOST_CHECK_EQUAL(connection._send_errors.load(), 1u);
}

BOOST_AUTO_TEST_CASE(p2p_socket_failure_is_not_a_successful_run) {
   using boost::asio::ip::tcp;
   boost::asio::io_context server;
   tcp::acceptor acceptor(server, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
   fc::temp_directory directory;
   provider_base_config config;
   config._port = acceptor.local_endpoint().port();
   config._evidence_log = (directory.path() / "failed.jsonl").string();
   p2p_connection connection(config);
   connection.init_and_connect();
   tcp::socket peer(server);
   acceptor.accept(peer);
   connection._p2p_socket.close(); // deterministic local transport failure
   chain::signed_transaction trx;
   connection.send_transaction(chain::packed_transaction(trx));
   BOOST_CHECK_THROW(connection.cleanup_and_disconnect(), std::runtime_error);
   BOOST_CHECK_EQUAL(connection._send_errors.load(), 1u);
   BOOST_CHECK_EQUAL(connection._sent_callback_num.load(), 1u);
   std::ifstream input(config._evidence_log);
   std::string line, last;
   while (std::getline(input, line)) last = line;
   auto end = fc::json::from_string(last);
   BOOST_CHECK_EQUAL(end["type"].as_string(), "generator_end");
   BOOST_CHECK_EQUAL(end["errors"].as_uint64(), 1u);
}

BOOST_AUTO_TEST_CASE(p2p_evidence_preserves_wire_transaction) {
   using boost::asio::ip::tcp;
   boost::asio::io_context server;
   tcp::acceptor acceptor(server, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
   fc::temp_directory directory;
   provider_base_config config;
   config._port = acceptor.local_endpoint().port();
   config._evidence_log = (directory.path() / "ok.jsonl").string();
   p2p_connection connection(config);
   connection.init_and_connect();
   tcp::socket peer(server);
   acceptor.accept(peer);
   chain::signed_transaction trx;
   chain::packed_transaction packed(trx);
   connection.send_transaction(packed);
   uint32_t size = 0;
   boost::asio::read(peer, boost::asio::buffer(&size, sizeof(size)));
   BOOST_REQUIRE_GT(size, 0u);
   BOOST_REQUIRE_LT(size, 1024u);
   std::vector<char> bytes(size);
   boost::asio::read(peer, boost::asio::buffer(bytes));
   fc::datastream<const char*> stream(bytes.data(), bytes.size());
   fc::unsigned_int which;
   fc::raw::unpack(stream, which);
   BOOST_CHECK_EQUAL(which.value, 8u);
   chain::packed_transaction decoded;
   fc::raw::unpack(stream, decoded);
   BOOST_CHECK(decoded.id() == packed.id());
   BOOST_CHECK_EQUAL(stream.remaining(), 0u);
   BOOST_REQUIRE_NO_THROW(connection.cleanup_and_disconnect());
   BOOST_CHECK_EQUAL(connection._send_errors.load(), 0u);
   BOOST_CHECK(connection.get_trx_ack_time(trx.id()) == fc::time_point::min());
}

BOOST_AUTO_TEST_CASE(tps_invalid_rates_and_overflow_rejected) {
   auto generator = std::make_shared<mock_trx_generator>(1);
   std::shared_ptr<null_tps_monitor> monitor;
   for (const auto config : {trx_tps_tester_config{1, 1000001}, {UINT32_MAX, 2}, {1, 0}}) {
      trx_tps_tester<mock_trx_generator, null_tps_monitor> test(generator, monitor, config);
      BOOST_CHECK(!test.run());
   }
   BOOST_CHECK(generator->_calls.empty());
}
#endif

BOOST_AUTO_TEST_CASE(tps_short_run_low_tps)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 5;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);
   BOOST_REQUIRE_LT(runtime_us.count(), maximum_runtime_us);
}

BOOST_AUTO_TEST_CASE(tps_short_run_high_tps)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 50000;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran ${rt}us vs expected max ${mx}us",
           ("rt", runtime_us.count())("mx", maximum_runtime_us));
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_short_run_med_tps_med_delay)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 10000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran ${rt}us vs expected max ${mx}us",
           ("rt", runtime_us.count())("mx", maximum_runtime_us));
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_med_run_med_tps_med_delay)
{
   constexpr uint32_t test_duration_s = 30;
   constexpr uint32_t test_tps = 10000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran ${rt}us vs expected max ${mx}us",
           ("rt", runtime_us.count())("mx", maximum_runtime_us));
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_cant_keep_up)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 100000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran ${rt}us vs expected max ${mx}us",
           ("rt", runtime_us.count())("mx", maximum_runtime_us));
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_med_run_med_tps_30us_delay)
{
   constexpr uint32_t test_duration_s = 15;
   constexpr uint32_t test_tps = 3000;
   constexpr uint32_t trx_delay_us = 30;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr uint64_t expected_runtime_us = test_duration_s * 1000000;
   constexpr uint64_t allowable_runtime_deviation_per = 20;
   constexpr uint64_t allowable_runtime_deviation_us = expected_runtime_us / allowable_runtime_deviation_per;
   constexpr int64_t minimum_runtime_us = expected_runtime_us - allowable_runtime_deviation_us;
   constexpr int64_t maximum_runtime_us = expected_runtime_us + allowable_runtime_deviation_us;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<simple_tps_monitor> monitor = std::make_shared<simple_tps_monitor>(expected_trxs);

   trx_tps_tester<mock_trx_generator, simple_tps_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_EQUAL(generator->_calls.size(), expected_trxs);
   BOOST_REQUIRE_GT(runtime_us.count(), minimum_runtime_us);

   if (runtime_us.count() > maximum_runtime_us) {
      ilog("couldn't sustain transaction rate.  ran ${rt}us vs expected max ${mx}us",
           ("rt", runtime_us.count())("mx", maximum_runtime_us));
      BOOST_REQUIRE_LT(monitor->_calls.back().time_to_next_trx_us, 0);
   }
}

BOOST_AUTO_TEST_CASE(tps_performance_monitor_during_spin_up)
{
   tps_test_stats stats;
   tps_performance_monitor monitor{std::chrono::microseconds(5s).count()};
   stats.total_trxs = 1000;
   stats.start_time = fc::time_point{fc::microseconds{0}};
   stats.expected_sent = 100;
   stats.trxs_sent = 90;

   // behind, but still within spin up window
   stats.last_run = fc::time_point{fc::microseconds{100000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // violation, but still within spin up window
   stats.last_run = fc::time_point{fc::microseconds{1100000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));
}

BOOST_AUTO_TEST_CASE(tps_performance_monitor_outside_spin_up)
{
   tps_test_stats stats;
   tps_performance_monitor monitor{std::chrono::microseconds(5s).count()};
   stats.total_trxs = 1000;
   stats.start_time = fc::time_point{fc::microseconds{0}};
   stats.expected_sent = 100;
   stats.trxs_sent = 90;

   // behind, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{5500000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // violation, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{6600000}};
   BOOST_REQUIRE(!monitor.monitor_test(stats));
}

BOOST_AUTO_TEST_CASE(tps_performance_monitor_outside_spin_up_within_limit)
{
   tps_test_stats stats;
   tps_performance_monitor monitor{std::chrono::microseconds(5s).count()};
   stats.total_trxs = 1000;
   stats.start_time = fc::time_point{fc::microseconds{0}};
   stats.expected_sent = 100;
   stats.trxs_sent = 90;

   // outside of limit,  out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{5500000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // outside of limit, less than max violation duration
   stats.last_run = fc::time_point{fc::microseconds{6000000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   stats.trxs_sent = 98;
   // behind, but within limit, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{6600000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   stats.expected_sent = 150;
   // outside of limit again, out of spin up window
   stats.last_run = fc::time_point{fc::microseconds{7000000}};
   BOOST_REQUIRE(monitor.monitor_test(stats));

   // outside of limit for too long
   stats.last_run = fc::time_point{fc::microseconds{8100000}};
   BOOST_REQUIRE(!monitor.monitor_test(stats));
}

BOOST_AUTO_TEST_CASE(tps_cant_keep_up_monitored)
{
   constexpr uint32_t test_duration_s = 5;
   constexpr uint32_t test_tps = 100000;
   constexpr uint32_t trx_delay_us = 10;
   constexpr uint32_t expected_trxs = test_duration_s * test_tps;
   constexpr int64_t expected_runtime_us = test_duration_s * 1000000;

   std::shared_ptr<mock_trx_generator> generator = std::make_shared<mock_trx_generator>(expected_trxs, trx_delay_us);
   std::shared_ptr<tps_performance_monitor> monitor = std::make_shared<tps_performance_monitor>();

   trx_tps_tester<mock_trx_generator, tps_performance_monitor> t1(generator, monitor, {test_duration_s, test_tps});

   fc::time_point start = fc::time_point::now();
   t1.run();
   fc::time_point end = fc::time_point::now();
   fc::microseconds runtime_us = end.time_since_epoch() - start.time_since_epoch();

   BOOST_REQUIRE_LT(runtime_us.count(), expected_runtime_us);
   BOOST_REQUIRE_LT(generator->_calls.size(), expected_trxs);
}

BOOST_AUTO_TEST_CASE(trx_generator_constructor)
{
   trx_generator_base_config tg_config{1, chain::chain_id_type("999"), chain::name("eosio"), fc::seconds(3600),
                                       fc::variant("00000062989f69fd251df3e0b274c3364ffc2f4fce73de3f1c7b5e11a4c92f21").as<chain::block_id_type>(), ".", true};
   provider_base_config p_config{"p2p", "127.0.0.1", 9876};
   const std::string abi_file = "../../unittests/contracts/eosio.token/eosio.token.abi";
   const std::string actions_data = "[{\"actionAuthAcct\": \"testacct1\",\"actionName\": \"transfer\",\"authorization\": {\"actor\": \"testacct1\",\"permission\": \"active\"},"
                                    "\"actionData\": {\"from\": \"testacct1\",\"to\": \"testacct2\",\"quantity\": \"0.0001 CUR\",\"memo\": \"transaction specified\"}}]";
   const std::string action_auths = "{\"testacct1\":\"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3\",\"testacct2\":\"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3\","
                                    "\"eosio\":\"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3\"}";
   user_specified_trx_config trx_config{abi_file, actions_data, action_auths};

   auto generator = std::make_shared<trx_generator>(tg_config, p_config, trx_config);
}

BOOST_AUTO_TEST_CASE(account_name_generator_tests)
{
   auto acct_gen = account_name_generator();
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "111111111111");

   //Test account name prefixes for differentiating between transaction generator instances
   acct_gen.setPrefix(1);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "121111111111");
   acct_gen.setPrefix(30);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "1z1111111111");
   acct_gen.setPrefix(31);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "211111111111");
   acct_gen.setPrefix(960);
   BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), "zz1111111111");

   //Test account name generation
   std::vector<std::string> expected = {
         "zz1111111111",
         "zz1111111112",
         "zz1111111113",
         "zz1111111114",
         "zz1111111115",
         "zz111111111a",
         "zz111111111b",
         "zz111111111c",
         "zz111111111d",
         "zz111111111e",
         "zz111111111f",
         "zz111111111g",
         "zz111111111h",
         "zz111111111i",
         "zz111111111j",
         "zz111111111k",
         "zz111111111l",
         "zz111111111m",
         "zz111111111n",
         "zz111111111o",
         "zz111111111p",
         "zz111111111q",
         "zz111111111r",
         "zz111111111s",
         "zz111111111t",
         "zz111111111u",
         "zz111111111v",
         "zz111111111w",
         "zz111111111x",
         "zz111111111y",
         "zz111111111z",
         "zz1111111121",
         "zz1111111122"};
   for(size_t i = 0; i < expected.size(); ++i) {
      BOOST_REQUIRE_EQUAL(acct_gen.calc_name(), expected.at(i));
      acct_gen.increment();
   }


   //Test account name generation starting at 31 ^ 5 - 1 = 28629150
   std::vector<std::string> expected2 = {
         "1211111zzzzz",
         "121111211111",
         "121111211112",
         "121111211113",
         "121111211114",
         "121111211115",
         "12111121111a",
         "12111121111b",
         "12111121111c",
         "12111121111d",
         "12111121111e",
         "12111121111f",
         "12111121111g",
         "12111121111h",
         "12111121111i",
         "12111121111j",
         "12111121111k",
         "12111121111l",
         "12111121111m",
         "12111121111n",
         "12111121111o",
         "12111121111p",
         "12111121111q",
         "12111121111r",
         "12111121111s",
         "12111121111t",
         "12111121111u",
         "12111121111v",
         "12111121111w",
         "12111121111x",
         "12111121111y",
         "12111121111z",
         "121111211121",
         "121111211122"};
   auto acct_gen2 = account_name_generator();
   acct_gen2.setPrefix(1);
   int initialVal = 28629150;
   for(int i = 0; i < initialVal; ++i) {
      acct_gen2.increment();
   }
   for(size_t i = 0; i < expected2.size(); ++i) {
      BOOST_REQUIRE_EQUAL(acct_gen2.calc_name(), expected2.at(i));
      acct_gen2.increment();
   }
}

BOOST_AUTO_TEST_CASE(simple_http_client_async_test) {

   const std::string host     = "127.0.0.1"s;
   constexpr unsigned short     port     = 8888;

   // Start Server
   echo_server_impl               server = echo_server_impl();
   auto                           addr   = boost::asio::ip::make_address(host);
   boost::asio::ip::tcp::endpoint endpoint(addr, port);

   server.start(endpoint);

   // Start Client

   // The io_context is required for all I/O
   boost::asio::io_context ioc;
   const std::string       target        = "/v1/chain/test"s;
   const int               version       = 11;
   const std::string       content_type  = "text/plain"s;
   const std::string       content_type2 = "application/json"s;

   http_client_async::http_request_params params{ioc, host, port, target, version, content_type};

   std::string test_body = "test request body"s;
   std::string test_body_copy = test_body;
   std::string test_body2 =
       "{\"return_failure_trace\":true,\"retry_trx\":false,\"transaction\":{\"signatures\":[\"SIG_K1_"
       "JyzLqbvpdybyujtiN1YdY2FWcBBi8dWWiFgZ515qyyqgKJJ6892i4rXTHdw5KGYut6EBuXPR3ExRwPSioSZ2bZ1RjNUXVj\"],"
       "\"compression\":\"none\",\"packed_context_free_data\":\"\",\"packed_trx\":"
       "\"848a34641800f994a24e00000000030000000000ea305500409e9a2264b89a0160ae423ad15b974a00000000a8ed32326660ae423ad15"
       "b974a1042088a4dd35057010000000100038d26b3d5ce8c7d76ef00d3d586a3d7bbc76c42f0b0719cc6f7b0cce1790622c3010000000100"
       "00000100028dc3921705c71d30b0b26674536fff934f8e43890c980aa1d2c168f00f406539010000000000000000ea3055000000004873b"
       "d3e0160ae423ad15b974a00000000a8ed32322060ae423ad15b974a1042088a4dd350570094357700000000045359530000000000000000"
       "00ea305500003f2a1ba6a24a0160ae423ad15b974a00000000a8ed32323160ae423ad15b974a1042088a4dd3505740420f0000000000045"
       "359530000000040420f000000000004535953000000000000\"}}"s;
   std::string test_body2_copy = test_body2;

   int callbackCalledCnt = 0;

   // Launch the asynchronous operation
   http_client_async::async_http_request(
       params, std::move(test_body),
       [&test_body_copy, &callbackCalledCnt](boost::beast::error_code ec, http::response<http::string_body> response) {
          BOOST_REQUIRE(!ec);
          BOOST_REQUIRE_EQUAL(test_body_copy, response.body());
          callbackCalledCnt++;
       });

   http_client_async::http_request_params params2{ioc, host, port, target, version, content_type2};
   http_client_async::async_http_request(
       params2, std::move(test_body2),
       [&test_body2_copy, &callbackCalledCnt](boost::beast::error_code ec, http::response<http::string_body> response) {
          BOOST_REQUIRE(!ec);
          BOOST_REQUIRE_EQUAL(test_body2_copy, response.body());
          callbackCalledCnt++;
       });

   // Run the I/O service. The call will return when
   // the get operation is complete.
   ioc.run();

   BOOST_REQUIRE_EQUAL(callbackCalledCnt, 2);

   server.shutdown();
}

BOOST_AUTO_TEST_SUITE_END()
