// Dedicated normal/sanitizer executable; no extra main in the ordinary unit_test glob.
#ifdef PUREEOS_PARALLEL_DEADLINE_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Parallel_Deadline_Service
#include <eosio/chain/parallel_deadline_service.hpp>
#include <boost/test/included/unit_test.hpp>
#include <array>
#include <future>

using namespace eosio::chain::experimental;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(parallel_deadline_service_tests)

BOOST_AUTO_TEST_CASE(bounded_registry_releases_slots_and_fires_each_guard_at_most_once) {
   BOOST_CHECK_THROW(parallel_deadline_service service(0), std::invalid_argument);
   parallel_deadline_service service(1);
   BOOST_CHECK_THROW(static_cast<void>(service.watch(parallel_deadline_service::clock::now(), {}, std::function<void()>{})),
                     std::invalid_argument);
   std::atomic<unsigned> count = 0;
   for (unsigned iteration = 0; iteration < 32; ++iteration) {
      std::promise<void> fired;
      auto observed = fired.get_future();
      auto guard = service.watch(parallel_deadline_service::clock::now(), {}, [&] {
         ++count; fired.set_value();
      });
      BOOST_REQUIRE(observed.wait_for(3s) == std::future_status::ready);
      // An expired guard still owns its slot until its owner releases it.
      BOOST_CHECK_THROW(static_cast<void>(service.watch(parallel_deadline_service::clock::now(), {}, [] {})), std::length_error);
      BOOST_CHECK_EQUAL(count.load(), iteration + 1);
   }
}

BOOST_AUTO_TEST_CASE(released_callbacks_never_fire_and_new_earlier_deadlines_wake_the_service) {
   parallel_deadline_service service(2);
   std::atomic<unsigned> cancelled = 0;
   std::promise<void> observed;
   auto done = observed.get_future();
   const auto later = parallel_deadline_service::clock::now() + 100ms;
   {
      auto removed = service.watch(later, {}, [&] { ++cancelled; });
   }
   auto guard = service.watch(later, {}, [&] { observed.set_value(); });
   std::promise<void> early;
   auto early_done = early.get_future();
   auto sooner = service.watch(parallel_deadline_service::clock::now(), {}, [&] { early.set_value(); });
   BOOST_REQUIRE(early_done.wait_for(3s) == std::future_status::ready);
   BOOST_REQUIRE(done.wait_for(3s) == std::future_status::ready);
   BOOST_CHECK_EQUAL(cancelled.load(), 0u);
}

BOOST_AUTO_TEST_CASE(guard_destruction_waits_for_inflight_callback_before_releasing_captures) {
   parallel_deadline_service service(1);
   std::promise<void> entered, release;
   auto entry = entered.get_future();
   auto released = release.get_future().share();
   auto guard = std::unique_ptr<parallel_deadline_service::guard>(
      new parallel_deadline_service::guard(service.watch(parallel_deadline_service::clock::now(), {}, [&] {
         entered.set_value(); released.wait(); // deliberate synchronized test injection
      })));
   const bool started = entry.wait_for(3s) == std::future_status::ready;
   if (!started) release.set_value(); // do not deadlock teardown on a failed test
   BOOST_REQUIRE(started);
   std::promise<void> destroying;
   auto destroy_started = destroying.get_future();
   auto destroy = std::async(std::launch::async, [&] { destroying.set_value(); guard.reset(); });
   destroy_started.wait();
   const bool synchronized = destroy.wait_for(20ms) == std::future_status::timeout;
   release.set_value();
   BOOST_CHECK(synchronized);
   BOOST_REQUIRE(destroy.wait_for(3s) == std::future_status::ready);
   destroy.get();
}

BOOST_AUTO_TEST_CASE(stop_tokens_generations_and_shutdown_interrupt_live_guards_once) {
   for (unsigned mode = 0; mode < 3; ++mode) {
      auto service = std::make_unique<parallel_deadline_service>(1);
      auto generation = std::make_shared<std::atomic<uint64_t>>(7);
      std::stop_source stop;
      std::promise<void> fired;
      auto done = fired.get_future();
      std::atomic<unsigned> count = 0;
      auto guard = service->watch(parallel_deadline_service::clock::time_point::max(),
         {stop.get_token(), generation, 7}, [&] { ++count; fired.set_value(); });
      if (mode == 0) stop.request_stop();
      else if (mode == 1) generation->fetch_add(1);
      else service.reset(); // callback and registration survive service ownership
      BOOST_REQUIRE(done.wait_for(3s) == std::future_status::ready);
      stop.request_stop(); generation->fetch_add(1);
      service.reset();
      BOOST_CHECK_EQUAL(count.load(), 1u);
      // Guard release must remain safe after the service was destroyed.
   }
}

BOOST_AUTO_TEST_CASE(callback_captures_are_destroyed_without_holding_the_registry_lock) {
   parallel_deadline_service service(1);
   std::atomic<unsigned> destroyed = 0, errors = 0;
   auto capture = std::shared_ptr<int>(new int(1), [&](int* value) noexcept {
      delete value;
      try {
         // Capacity-one reuse also proves the old slot was fully released.
         auto nested = service.watch(parallel_deadline_service::clock::time_point::max(), {}, [] {});
         ++destroyed;
      } catch (...) { ++errors; }
   });
   {
      auto guard = service.watch(parallel_deadline_service::clock::time_point::max(), {}, [capture] {});
      capture.reset();
   }
   BOOST_CHECK_EQUAL(destroyed.load(), 1u);
   BOOST_CHECK_EQUAL(errors.load(), 0u);
}

BOOST_AUTO_TEST_CASE(earlier_insertions_and_new_cancellation_polls_interrupt_a_distant_wait) {
   for (unsigned mode = 0; mode < 2; ++mode) {
      parallel_deadline_service service(3);
      std::atomic<unsigned> distant_calls = 0;
      auto distant = service.watch(parallel_deadline_service::clock::now() + 1h, {}, [&] { ++distant_calls; });
      // Handshake with the service, without relying on an arbitrary sleep.
      std::promise<void> ready;
      auto observed = ready.get_future();
      {
         auto handshake = service.watch(parallel_deadline_service::clock::now(), {}, [&] { ready.set_value(); });
         BOOST_REQUIRE(observed.wait_for(3s) == std::future_status::ready);
      }
      std::stop_source stop;
      std::promise<void> fired;
      auto done = fired.get_future();
      auto guard = service.watch(mode ? parallel_deadline_service::clock::time_point::max()
                                      : parallel_deadline_service::clock::now(),
         {mode ? stop.get_token() : std::stop_token{}, {}, 0}, [&] { fired.set_value(); });
      if (mode) stop.request_stop();
      BOOST_REQUIRE(done.wait_for(3s) == std::future_status::ready);
      BOOST_CHECK_EQUAL(distant_calls.load(), 0u);
   }
}

BOOST_AUTO_TEST_CASE(removing_the_earliest_registration_does_not_delay_the_remaining_deadline) {
   parallel_deadline_service service(2);
   std::atomic<unsigned> removed_calls = 0;
   std::promise<parallel_deadline_service::clock::time_point> fired;
   auto done = fired.get_future();
   const auto target = parallel_deadline_service::clock::now() + 100ms;
   auto later = service.watch(target, {}, [&] { fired.set_value(parallel_deadline_service::clock::now()); });
   {
      auto earlier = service.watch(target - 50ms, {}, [&] { ++removed_calls; });
   }
   BOOST_REQUIRE(done.wait_for(3s) == std::future_status::ready);
   BOOST_CHECK(done.get() >= target);
   BOOST_CHECK_EQUAL(removed_calls.load(), 0u);
}

BOOST_AUTO_TEST_CASE(short_lived_frames_do_not_hide_cancellation_of_a_live_frame) {
   parallel_deadline_service service(5);
   std::stop_source stop;
   std::promise<void> fired;
   auto done = fired.get_future();
   std::atomic<unsigned> removed_calls = 0;
   auto live = service.watch(parallel_deadline_service::clock::time_point::max(),
      {stop.get_token(), {}, 0}, [&] { fired.set_value(); });
   {
      std::array<std::jthread, 4> workers;
      for (auto& thread : workers) thread = std::jthread([&] {
         for (unsigned i = 0; i < 1000; ++i) {
            auto frame = service.watch(parallel_deadline_service::clock::now() + 1h, {}, [&] { ++removed_calls; });
         }
      });
      stop.request_stop();
   }
   BOOST_REQUIRE(done.wait_for(3s) == std::future_status::ready);
   BOOST_CHECK_EQUAL(removed_calls.load(), 0u);
}

BOOST_AUTO_TEST_CASE(one_timer_thread_covers_sixteen_workers_and_nine_nested_frames_each) {
   constexpr size_t workers = 16, depth = 9;
   parallel_deadline_service service(workers * depth);
   auto generation = std::make_shared<std::atomic<uint64_t>>(0);
   std::atomic<size_t> ready = 0, callbacks = 0, errors = 0;
   std::promise<void> prepared, fired;
   auto all_ready = prepared.get_future(), all_fired = fired.get_future();
   std::promise<void> release;
   auto released = release.get_future().share();
   std::array<std::thread::id, workers * depth> timer_ids{};
   std::array<std::thread, workers> threads;
   try {
   for (size_t i = 0; i < workers; ++i) threads[i] = std::thread([&, i] {
      try {
         auto nested = [&](auto&& self, size_t level) -> void {
            auto guard = service.watch(parallel_deadline_service::clock::time_point::max(),
               {{}, generation, 0}, [&, slot = i * depth + level] {
                  timer_ids[slot] = std::this_thread::get_id();
                  if (++callbacks == workers * depth) fired.set_value();
               });
            if (level + 1 < depth) self(self, level + 1);
            else {
               if (++ready == workers) prepared.set_value();
               released.wait();
            }
         };
         nested(nested, 0);
      } catch (...) { ++errors; }
   });
   } catch (...) {
      release.set_value();
      for (auto& thread : threads) if (thread.joinable()) thread.join();
      throw;
   }
   const bool prepared_all = all_ready.wait_for(3s) == std::future_status::ready;
   generation->fetch_add(1);
   const bool fired_all = all_fired.wait_for(3s) == std::future_status::ready;
   release.set_value();
   for (auto& thread : threads) thread.join();
   BOOST_REQUIRE(prepared_all); BOOST_REQUIRE(fired_all);
   BOOST_CHECK_EQUAL(errors.load(), 0u);
   BOOST_CHECK_EQUAL(callbacks.load(), workers * depth);
   BOOST_CHECK(timer_ids[0] != std::this_thread::get_id());
   for (const auto id : timer_ids) BOOST_CHECK(id == timer_ids[0]);
}

BOOST_AUTO_TEST_SUITE_END()

#endif
