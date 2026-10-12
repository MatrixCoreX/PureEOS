#include <eosio/chain/sync_call_memory_pool.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/scoped_exit.hpp>
#include <barrier>
#include <chrono>
#include <future>
#include <set>
#include <thread>

using eosio::chain::depth_resource_pools;
using namespace std::chrono_literals;

namespace {
struct memory_counts { std::atomic<unsigned> made{0}, destroyed{0}; };
struct test_memory {
   memory_counts& count;
   const uint32_t slices;
   std::atomic<bool> busy{false};
   uint64_t calls = 0; // exclusive borrowers only, checked by TSan
   test_memory(memory_counts& c, uint32_t s) : count(c), slices(s) { ++count.made; }
   ~test_memory() { ++count.destroyed; }
};
using memory_pool = depth_resource_pools<test_memory>;
using borrowed = std::vector<std::pair<uint32_t, test_memory*>>;
borrowed borrow_all(memory_pool& pool, unsigned threads, unsigned depth) {
   borrowed out;
   std::set<test_memory*> unique;
   for (unsigned level = 1; level <= depth; ++level) for (unsigned t = 0; t < threads; ++t) {
      auto* value = pool.acquire(level);
      out.emplace_back(level, value);
      BOOST_CHECK(unique.insert(value).second);
      BOOST_CHECK_EQUAL(value->slices, level < 5 ? 6 - level : 1u);
   }
   for (unsigned level = 1; level <= depth; ++level) BOOST_CHECK_THROW(pool.acquire(level), std::runtime_error);
   return out;
}
void return_all(memory_pool& pool, const borrowed& values) {
   for (const auto& [depth, value] : values) BOOST_CHECK(pool.release(depth, value));
}
} // namespace

BOOST_AUTO_TEST_SUITE(sync_call_memory_pool_tests)

BOOST_AUTO_TEST_CASE(every_new_depth_has_all_threads_and_deep_calls_share_the_last_pool) {
   for (bool depth_first : {false, true}) {
      memory_counts c;
      {
         memory_pool pool([&](uint32_t s) { return new test_memory(c, s); });
         if (depth_first) pool.set_max_call_depth(9);
         pool.set_num_threads(4);
         if (!depth_first) for (unsigned depth = 2; depth <= 9; ++depth) pool.set_max_call_depth(depth);
         BOOST_CHECK_EQUAL(c.made.load(), 36u);
         auto all = borrow_all(pool, 4, 9);
         return_all(pool, all);
         pool.set_num_threads(1); pool.set_max_call_depth(1); // no shrink
         BOOST_CHECK_EQUAL(c.made.load(), 36u);
         return_all(pool, borrow_all(pool, 4, 9));
      }
      BOOST_CHECK_EQUAL(c.destroyed.load(), 36u);
   }
}

BOOST_AUTO_TEST_CASE(failed_new_layout_is_not_published_and_retry_keeps_correct_threads) {
   memory_counts c;
   unsigned fail_slice = 0;
   {
      memory_pool pool([&](uint32_t s) {
         if (s == fail_slice) throw std::bad_alloc();
         return new test_memory(c, s);
      });
      pool.set_num_threads(4);
      fail_slice = 3;
      BOOST_CHECK_THROW(pool.set_max_call_depth(4), std::bad_alloc);
      BOOST_CHECK_EQUAL(c.made.load() - c.destroyed.load(), 4u);
      return_all(pool, borrow_all(pool, 4, 1));
      // No partial depth-2 pool was published; above-current depths still use
      // the old last pool, as in the original API (protocol guards are outside).
      auto* original = pool.acquire(2);
      BOOST_CHECK_EQUAL(original->slices, 5u); BOOST_CHECK(pool.release(2, original));
      fail_slice = 0; pool.set_max_call_depth(4);
      return_all(pool, borrow_all(pool, 4, 4));
   }
   BOOST_CHECK_EQUAL(c.made.load(), c.destroyed.load());
}

BOOST_AUTO_TEST_CASE(partial_thread_growth_keeps_old_logical_dimensions_and_can_be_retried) {
   memory_counts c;
   unsigned fail_slice = 0;
   memory_pool pool([&](uint32_t s) {
      if (s == fail_slice) throw std::bad_alloc();
      return new test_memory(c, s);
   });
   pool.set_max_call_depth(3); fail_slice = 3;
   BOOST_CHECK_THROW(pool.set_num_threads(4), std::bad_alloc);
   // First two existing pools may safely retain extra capacity. No config
   // shrink/rollback can invalidate a resource that is currently borrowed.
   BOOST_CHECK_EQUAL(c.made.load() - c.destroyed.load(), 9u);
   fail_slice = 0; pool.set_max_call_depth(4);
   auto* new_depth = pool.acquire(4);
   BOOST_CHECK_THROW(pool.acquire(4), std::runtime_error); // still ONE logical thread
   BOOST_CHECK(pool.release(4, new_depth));
   pool.set_num_threads(4);
   BOOST_CHECK_EQUAL(c.made.load() - c.destroyed.load(), 16u);
   return_all(pool, borrow_all(pool, 4, 4));
}

BOOST_AUTO_TEST_CASE(return_uses_original_owner_and_invalid_inputs_do_not_corrupt_layout) {
   memory_counts c;
   memory_pool pool([&](uint32_t s) { return new test_memory(c, s); });
   BOOST_CHECK_THROW(pool.acquire(0), std::invalid_argument);
   BOOST_CHECK_THROW(pool.set_num_threads(0), std::invalid_argument);
   BOOST_CHECK_THROW(pool.set_max_call_depth(0), std::invalid_argument);
   auto* held = pool.acquire(7); // low-level API's old-last-pool fallback
   pool.set_max_call_depth(9);
   BOOST_CHECK(!pool.release(0, held));
   BOOST_CHECK(!pool.release(1, nullptr));
   BOOST_CHECK(pool.release(7, held)); // layout changed; must return to slice-5 owner
   BOOST_CHECK(!pool.release(7, held));
   auto* root = pool.acquire(1); BOOST_CHECK(root == held); BOOST_CHECK(pool.release(1, root));
   BOOST_CHECK_THROW(pool.set_num_threads(std::numeric_limits<uint32_t>::max()), std::length_error);
   pool.set_num_threads(2);
   BOOST_CHECK_THROW(pool.set_max_call_depth(std::numeric_limits<uint32_t>::max()), std::length_error);
   return_all(pool, borrow_all(pool, 2, 9));
}

BOOST_AUTO_TEST_CASE(concurrent_borrowers_see_only_initialized_pools_during_growth) {
   memory_counts c;
   memory_pool pool([&](uint32_t s) { return new test_memory(c, s); });
   pool.set_num_threads(4);
   std::atomic<unsigned> depth{1}; std::atomic<bool> good{true};
   std::barrier start(5); std::vector<std::jthread> workers;
   for (unsigned t = 0; t < 4; ++t) workers.emplace_back([&] {
      start.arrive_and_wait();
      for (unsigned round = 0; round < 5000; ++round) {
         const unsigned level = 1 + round % depth.load();
         try {
            auto* r = pool.acquire(level);
            if (r->busy.exchange(true) || r->slices != (level < 5 ? 6 - level : 1u)) good = false;
            ++r->calls;
            if (round % 17 == 0) std::this_thread::yield();
            if (!r->busy.exchange(false) || !pool.release(level, r)) good = false;
         } catch (...) { good = false; }
      }
   });
   start.arrive_and_wait();
   for (unsigned level = 2; level <= 9; ++level) { pool.set_max_call_depth(level); depth = level; }
   workers.clear();
   BOOST_CHECK(good.load());
   const auto all = borrow_all(pool, 4, 9);
   uint64_t total = 0;
   for (const auto& [level, r] : all) { total += r->calls; BOOST_CHECK(!r->busy.load()); }
   BOOST_CHECK_EQUAL(total, 20000u);
   return_all(pool, all);
}

BOOST_AUTO_TEST_CASE(slow_new_pool_factory_allows_old_pool_return_and_borrow) {
   memory_counts c;
   std::promise<void> entered, resume;
   auto entered_future = entered.get_future(), resume_future = resume.get_future();
   memory_pool pool([&](uint32_t s) {
      if (s == 4) { entered.set_value(); resume_future.wait(); }
      return new test_memory(c, s);
   });
   auto* held = pool.acquire(1);
   auto growth = std::async(std::launch::async, [&] { pool.set_max_call_depth(2); });
   const bool reached = entered_future.wait_for(2s) == std::future_status::ready;
   auto existing = std::async(std::launch::async, [&] {
      if (!pool.release(1, held)) return false;
      auto* r = pool.acquire(1); const bool same = r == held;
      return pool.release(1, r) && same;
   });
   const bool ready = existing.wait_for(2s) == std::future_status::ready;
   resume.set_value(); growth.get();
   BOOST_CHECK(reached); BOOST_CHECK(ready); BOOST_CHECK(existing.get());
   return_all(pool, borrow_all(pool, 1, 2));
}

BOOST_AUTO_TEST_CASE(each_resource_gets_a_guard_before_the_next_acquisition_or_execution) {
   memory_counts c;
   eosio::chain::call_resource_pool<test_memory> executors([&] { return new test_memory(c, 0); });
   memory_pool memories([&](uint32_t s) { return new test_memory(c, s); });
   auto call = [&] {
      auto* exec = executors.acquire();
      auto cleanup_exec = fc::make_scoped_exit([&] { executors.release(exec); });
      auto* mem = memories.acquire(1);
      auto cleanup_mem = fc::make_scoped_exit([&] { memories.release(1, mem); });
      throw std::runtime_error("execution failure");
   };
   auto* held = memories.acquire(1);
   BOOST_CHECK_THROW(call(), std::runtime_error); // SECOND allocation fails
   auto* exec = executors.acquire(); BOOST_CHECK(executors.release(exec));
   BOOST_CHECK(memories.release(1, held));
   BOOST_CHECK_THROW(call(), std::runtime_error); // execution fails after both
   exec = executors.acquire(); BOOST_CHECK(executors.release(exec));
   return_all(memories, borrow_all(memories, 1, 1));
}

BOOST_AUTO_TEST_SUITE_END()
