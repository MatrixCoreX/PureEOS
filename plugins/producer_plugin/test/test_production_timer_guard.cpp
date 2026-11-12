#define BOOST_TEST_MODULE production_timer_guard
#include <boost/test/included/unit_test.hpp>
#include <eosio/producer_plugin/production_timer_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

using eosio::production_timer_guard;

BOOST_AUTO_TEST_CASE(asio_cancelled_microbatch_never_delivers) {
   boost::asio::io_context timers, owner;
   boost::asio::steady_timer timer(timers);
   production_timer_guard guard;
   const auto cid = guard.next();
   unsigned calls = 0, completions = 0;
   timer.expires_after(std::chrono::hours(1));
   timer.async_wait([&](const auto& ec) {
      ++completions;
      BOOST_CHECK(ec == boost::asio::error::operation_aborted);
      if (!ec && guard.current(cid))
         boost::asio::post(owner, guard.on_owner(cid, [&] { ++calls; }));
   });
   guard.invalidate();
   BOOST_CHECK_EQUAL(timer.cancel(), 1u); // actual modern Asio API
   timers.run(); owner.run();
   BOOST_CHECK_EQUAL(completions, 1u);
   BOOST_CHECK_EQUAL(calls, 0u);
}

BOOST_AUTO_TEST_CASE(asio_expired_microbatch_is_invalidated_after_post_before_owner) {
   boost::asio::io_context timers, owner;
   boost::asio::steady_timer timer(timers);
   production_timer_guard guard;
   const auto cid = guard.next();
   unsigned calls = 0;
   timer.expires_at(std::chrono::steady_clock::now());
   timer.async_wait([&](const auto& ec) {
      BOOST_REQUIRE(!ec);
      if (guard.current(cid)) boost::asio::post(owner, guard.on_owner(cid, [&] { ++calls; }));
   });
   timers.run(); // completion already posted, cancel alone is now too late
   BOOST_CHECK_EQUAL(timer.cancel(), 0u);
   guard.invalidate();
   owner.run();
   BOOST_CHECK_EQUAL(calls, 0u);
}

BOOST_AUTO_TEST_CASE(asio_target_flush_then_rearm_cannot_drain_successor_twice) {
   boost::asio::io_context timers, owner;
   boost::asio::steady_timer timer(timers);
   production_timer_guard guard;
   unsigned calls = 0;
   const auto arm = [&] {
      const auto cid = guard.next();
      timer.expires_at(std::chrono::steady_clock::now());
      timer.async_wait([&, cid](const auto& ec) {
         if (!ec && guard.current(cid))
            boost::asio::post(owner, guard.on_owner(cid, [&] { ++calls; guard.invalidate(); }));
      });
   };
   arm(); timers.run();
   guard.invalidate(); // useful target reached: drain the first batch now
   ++calls;
   timer.cancel();
   timers.restart();
   arm(); timers.run();
   owner.run();
   BOOST_CHECK_EQUAL(calls, 2u); // one target drain plus ONE successor timer drain
}

BOOST_AUTO_TEST_CASE(current_callback_runs_and_can_replace_its_timer) {
   production_timer_guard guard;
   const auto first = guard.next();
   unsigned calls = 0;
   auto posted = guard.on_owner(first, [&] { ++calls; guard.next(); });
   posted();
   posted(); // the old generation cannot act on the successor block
   BOOST_CHECK_EQUAL(calls, 1u);
   BOOST_CHECK(!guard.current(first));
}

BOOST_AUTO_TEST_CASE(replacement_after_timer_delivery_before_owner_dispatch_drops_stale_callback) {
   production_timer_guard guard;
   const auto first = guard.next();
   BOOST_REQUIRE(guard.current(first)); // timer-thread check already succeeded
   unsigned calls = 0;
   auto posted = guard.on_owner(first, [&] { ++calls; });
   const auto replacement = guard.next(); // callback is still in owner queue
   posted();
   BOOST_CHECK_EQUAL(calls, 0u);
   BOOST_CHECK(guard.current(replacement));
   guard.on_owner(replacement, [&] { ++calls; })();
   BOOST_CHECK_EQUAL(calls, 1u);
}

BOOST_AUTO_TEST_CASE(cancellation_without_replacement_invalidates_posted_work) {
   production_timer_guard guard;
   const auto scheduled = guard.next();
   unsigned calls = 0;
   auto posted = guard.on_owner(scheduled, [&] { ++calls; });
   guard.invalidate(); // abort / no replacement timer / shutdown
   posted();
   BOOST_CHECK_EQUAL(calls, 0u);
   BOOST_CHECK(!guard.current(scheduled));
}

BOOST_AUTO_TEST_CASE(timer_thread_may_read_while_owner_rearms) {
   production_timer_guard guard;
   auto first = guard.next();
   std::promise<void> ready;
   auto started = ready.get_future();
   std::thread timer([&] {
      ready.set_value();
      while (guard.current(first)) std::this_thread::yield();
   });
   started.wait();
   for (unsigned i = 0; i < 10000; ++i) guard.next();
   timer.join();
   BOOST_CHECK(!guard.current(first));
}

BOOST_AUTO_TEST_CASE(current_callback_errors_propagate_stale_callback_is_not_entered) {
   production_timer_guard guard;
   auto scheduled = guard.next();
   auto posted = guard.on_owner(scheduled, [] { throw std::runtime_error("expected"); });
   BOOST_CHECK_THROW(posted(), std::runtime_error);
   guard.invalidate();
   BOOST_CHECK_NO_THROW(posted());
}

BOOST_AUTO_TEST_CASE(callback_keeps_owned_capture_alive_until_dispatch) {
   production_timer_guard guard;
   unsigned result = 0;
   auto posted = guard.on_owner(guard.next(), [value = std::make_unique<unsigned>(42), &result] {
      result = *value;
   });
   posted();
   BOOST_CHECK_EQUAL(result, 42u);
}
