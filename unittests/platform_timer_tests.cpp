#include <eosio/chain/thread_utils.hpp>
#include <eosio/chain/platform_timer.hpp>
#include <eosio/chain/transaction_context.hpp>

#include <boost/test/unit_test.hpp>

namespace eosio {
using namespace std;
using namespace chain;

BOOST_AUTO_TEST_SUITE(platform_timer_tests)

BOOST_AUTO_TEST_CASE(transaction_interrupt_survives_billing_pause)
{
   platform_timer timer;
   // Cover interrupts before the first start, while running, and while paused.
   for (int phase = 0; phase < 3; ++phase) {
      {
         transaction_checktime_timer trx(timer);
         if (phase != 0) trx.start(fc::time_point::maximum());
         if (phase == 2) trx.stop();
         std::thread interrupter([&]() { timer.interrupt_timer(); });
         interrupter.join();
         BOOST_CHECK(trx.timer_state() == platform_timer::state_t::interrupted);
         trx.stop();
         trx.start(fc::time_point::maximum());
         BOOST_CHECK(trx.timer_state() == platform_timer::state_t::interrupted);
         BOOST_CHECK(timer.timer_state() == platform_timer::state_t::interrupted);
         // Additional billing pauses must not clear an already delivered request.
         trx.stop();
         trx.start(fc::time_point::maximum());
         BOOST_CHECK(trx.timer_state() == platform_timer::state_t::interrupted);
      }
      // Reusing the platform timer must not interrupt the next transaction.
      transaction_checktime_timer next(timer);
      next.start(fc::time_point::maximum());
      BOOST_CHECK(next.timer_state() == platform_timer::state_t::running);
   }
}

BOOST_AUTO_TEST_CASE(paused_interrupt_delivers_watchdog_callback_on_resume)
{
   platform_timer timer;
   transaction_checktime_timer trx(timer);
   std::atomic<size_t> calls{0};
   trx.start(fc::time_point::maximum());
   trx.stop();
   timer.interrupt_timer();
   trx.set_expiration_callback([](void* p) { ++*static_cast<std::atomic<size_t>*>(p); }, &calls);
   BOOST_CHECK_EQUAL(calls.load(), 0u);
   trx.start(fc::time_point::maximum());
   BOOST_CHECK_EQUAL(calls.load(), 1u);
   BOOST_CHECK(trx.timer_state() == platform_timer::state_t::interrupted);
}

BOOST_AUTO_TEST_CASE(transaction_interrupt_races_with_billing_pause)
{
   platform_timer timer;
   for (int i = 0; i < 200; ++i) {
      transaction_checktime_timer trx(timer);
      trx.start(fc::time_point::maximum());
      std::atomic<bool> go{false};
      std::thread interrupter([&]() {
         while (!go.load()) std::this_thread::yield();
         timer.interrupt_timer();
      });
      go.store(true);
      trx.stop();
      trx.start(fc::time_point::maximum());
      interrupter.join();
      BOOST_CHECK(trx.timer_state() == platform_timer::state_t::interrupted);
   }
}

BOOST_AUTO_TEST_CASE(transient_interrupt_allows_retry_but_does_not_clear_cancellation)
{
   platform_timer timer;
   for (int phase = 0; phase < 3; ++phase) {
      transaction_checktime_timer trx(timer);
      trx.start(fc::time_point::maximum());
      if (phase == 1) timer.interrupt_timer();
      timer.interrupt_timer(false); // internal OC tier-up notification
      if (phase == 2) timer.interrupt_timer();
      BOOST_CHECK(trx.timer_state() == platform_timer::state_t::interrupted);
      trx.stop();
      trx.start(fc::time_point::maximum());
      BOOST_CHECK(trx.timer_state() == (phase == 0 ? platform_timer::state_t::running
                                                  : platform_timer::state_t::interrupted));
   }
}

BOOST_AUTO_TEST_CASE(correct_num_callbacks_test)
{
   named_thread_pool<struct test> pool;
   const size_t num_threads = 16;
   pool.start(num_threads, [](const fc::exception& e) {
      BOOST_ERROR("exception: " + e.to_detail_string());
   });

   std::atomic<size_t> calls{0};
   platform_timer t;
   t.set_expiration_callback([](void* a) {
      auto atom = (std::atomic<size_t>*)a;
      ++(*atom);
   }, &calls);
   std::mutex m;
   std::atomic<size_t> barrier(num_threads);
   for (size_t i = 0; i < num_threads; ++i) {
      boost::asio::post(pool.get_executor(), [&]() {
         lock_guard lock(m);
         t.start(fc::time_point::now() + fc::milliseconds(15));
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
         t.stop();
         --barrier;
      });
      if (i % 2 == 0) {
         boost::asio::post(pool.get_executor(), [&, i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds((i+1)*10));
            t.interrupt_timer();
         });
      }
   }
   for (size_t i = 0; i < 5000; ++i) {
      if (barrier == 0)
         break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   BOOST_TEST_REQUIRE(barrier == 0);
   pool.stop();

   BOOST_TEST(calls == num_threads);
}

std::map<size_t, bool> callback_called;
std::mutex cc_mtx;
void called(size_t i) {
   std::lock_guard lock(cc_mtx);
   callback_called[i] = true;
}

/// Test would fail with a sleep in interrupt_timer() before fix
BOOST_AUTO_TEST_CASE(correct_callback_test)
{
   named_thread_pool<struct test> pool;
   named_thread_pool<struct interrupt> interrupt_pool;
   const size_t num_threads = 16;
   pool.start(num_threads, [](const fc::exception& e) {
      BOOST_ERROR("exception: " + e.to_detail_string());
   });
   interrupt_pool.start(num_threads, [](const fc::exception& e) {
      BOOST_ERROR("exception: " + e.to_detail_string());
   });

   platform_timer t;
   std::mutex m;
   std::atomic<size_t> barrier(num_threads*2);
   for (size_t i = 0; i < num_threads; ++i) {
      boost::asio::post(pool.get_executor(), [i, &t, &m, &barrier]() {
         lock_guard lock(m);
         t.set_expiration_callback(nullptr, nullptr);
         t.set_expiration_callback([](void* a) {
            called((size_t)a);
         }, (void*)i);
         t.start(fc::time_point::now() + fc::milliseconds(15));
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
         t.stop();
         --barrier;
      });
      boost::asio::post(interrupt_pool.get_executor(), [&, i]() {
         std::this_thread::sleep_for(std::chrono::milliseconds((i+1)*20));
         t.interrupt_timer();
         --barrier;
      });
   }
   for (size_t i = 0; i < 5000; ++i) {
      if (barrier == 0)
         break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   BOOST_TEST_REQUIRE(barrier == 0);
   pool.stop();

   std::lock_guard lock(cc_mtx);
   for (size_t i = 0; i < num_threads; ++i) {
      BOOST_TEST(callback_called[i]);
   }
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace eosio
