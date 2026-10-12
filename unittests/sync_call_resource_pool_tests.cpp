#include <eosio/chain/sync_call_resource_pool.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using eosio::chain::call_resource_pool;
using namespace std::chrono_literals;

namespace {
struct counts { std::atomic<unsigned> made{0}, destroyed{0}; };
struct resource {
   counts& count;
   std::atomic<bool> occupied{false};
   uint64_t calls = 0; // intentionally NOT atomic: each checkout must be exclusive
   explicit resource(counts& c) : count(c) { ++count.made; }
   ~resource() { ++count.destroyed; }
};
using pool_type = call_resource_pool<resource>;
std::vector<resource*> drain(pool_type& pool, size_t size) {
   std::vector<resource*> out;
   for (size_t i = 0; i < size; ++i) out.push_back(pool.acquire());
   BOOST_CHECK_THROW(pool.acquire(), std::runtime_error);
   BOOST_CHECK_EQUAL(std::set(out.begin(), out.end()).size(), size);
   return out;
}
void give_back(pool_type& pool, const std::vector<resource*>& resources) {
   for (auto* value : resources) BOOST_CHECK(pool.release(value));
}
} // namespace

BOOST_AUTO_TEST_SUITE(sync_call_resource_pool_tests)

BOOST_AUTO_TEST_CASE(construction_failure_empty_pool_and_invalid_returns_are_checked_in_release_builds) {
   counts c;
   BOOST_CHECK_THROW(pool_type([]() -> resource* { return nullptr; }), std::invalid_argument);
   BOOST_CHECK_THROW(pool_type([]() -> resource* { throw std::bad_alloc(); }), std::bad_alloc);
   {
      pool_type pool([&] { return new resource(c); });
      resource foreign(c);
      auto* first = pool.acquire();
      BOOST_CHECK_THROW(pool.acquire(), std::runtime_error);
      static_assert(noexcept(pool.release(first)), "scope-exit return must not allocate or throw");
      BOOST_CHECK(!pool.release(nullptr));
      BOOST_CHECK(!pool.release(&foreign));
      BOOST_CHECK(pool.release(first));
      BOOST_CHECK(!pool.release(first));
      BOOST_CHECK(pool.acquire() == first);
      BOOST_CHECK(pool.release(first));
      BOOST_CHECK_EQUAL(c.destroyed.load(), 0u);
   }
   BOOST_CHECK_EQUAL(c.made.load(), 2u);
   BOOST_CHECK_EQUAL(c.destroyed.load(), 2u);
}

BOOST_AUTO_TEST_CASE(growth_counts_checked_out_resources_and_never_shrinks) {
   counts c;
   {
      pool_type pool([&] { return new resource(c); });
      auto* held = pool.acquire();
      auto create = [&] { return new resource(c); };
      pool.set_num_threads(3, create);
      pool.set_max_call_depth(4, create);
      BOOST_CHECK_EQUAL(c.made.load(), 12u);
      auto rest = drain(pool, 11);
      BOOST_CHECK(std::find(rest.begin(), rest.end(), held) == rest.end());
      give_back(pool, rest);
      BOOST_CHECK(pool.release(held));
      pool.set_num_threads(1, create);
      pool.set_max_call_depth(1, create);
      BOOST_CHECK_EQUAL(c.made.load(), 12u);
      give_back(pool, drain(pool, 12));
   }
   BOOST_CHECK_EQUAL(c.destroyed.load(), 12u);
}

BOOST_AUTO_TEST_CASE(failed_growth_rolls_back_resources_and_both_dimensions) {
   counts c;
   {
      pool_type pool([&] { return new resource(c); });
      auto* held = pool.acquire();
      unsigned attempted = 0;
      BOOST_CHECK_THROW(pool.set_num_threads(4, [&]() -> resource* {
         if (++attempted == 3) throw std::bad_alloc();
         return new resource(c);
      }), std::bad_alloc);
      BOOST_CHECK_EQUAL(c.made.load(), 3u);
      BOOST_CHECK_EQUAL(c.destroyed.load(), 2u);
      BOOST_CHECK_THROW(pool.acquire(), std::runtime_error);
      BOOST_CHECK(pool.release(held));
      // A failed increase to four threads must not leak into later depth growth.
      pool.set_max_call_depth(3, [&] { return new resource(c); });
      give_back(pool, drain(pool, 3));
      attempted = 0;
      BOOST_CHECK_THROW(pool.set_max_call_depth(6, [&]() -> resource* {
         if (++attempted == 2) return nullptr;
         return new resource(c);
      }), std::invalid_argument);
      give_back(pool, drain(pool, 3));
      pool.set_num_threads(2, [&] { return new resource(c); });
      give_back(pool, drain(pool, 6));
      BOOST_CHECK_EQUAL(c.made.load() - c.destroyed.load(), 6u);
   }
   BOOST_CHECK_EQUAL(c.made.load(), c.destroyed.load());
}

BOOST_AUTO_TEST_CASE(duplicate_factory_results_do_not_destroy_or_publish_owned_objects) {
   counts c;
   {
      pool_type pool([&] { return new resource(c); });
      auto* original = pool.acquire();
      BOOST_CHECK_THROW(pool.set_num_threads(2, [&] { return original; }), std::invalid_argument);
      BOOST_CHECK_EQUAL(c.destroyed.load(), 0u);
      BOOST_CHECK(pool.release(original));
      resource* staged = nullptr;
      BOOST_CHECK_THROW(pool.set_num_threads(3, [&] {
         if (!staged) staged = new resource(c);
         return staged;
      }), std::invalid_argument);
      BOOST_CHECK_EQUAL(c.destroyed.load(), 1u);
      give_back(pool, drain(pool, 1));
   }
   BOOST_CHECK_EQUAL(c.made.load(), c.destroyed.load());
}

BOOST_AUTO_TEST_CASE(zero_and_capacity_multiplication_overflow_fail_before_calling_factory) {
   counts c;
   pool_type pool([&] { return new resource(c); });
   auto create = [&] { return new resource(c); };
   BOOST_CHECK_THROW(pool.set_num_threads(0, create), std::invalid_argument);
   BOOST_CHECK_THROW(pool.set_max_call_depth(0, create), std::invalid_argument);
   pool.set_num_threads(2, create);
   BOOST_CHECK_THROW(pool.set_max_call_depth(std::numeric_limits<uint32_t>::max(), create), std::length_error);
   BOOST_CHECK_EQUAL(c.made.load(), 2u);
   give_back(pool, drain(pool, 2));
   pool.set_max_call_depth(2, create);
   BOOST_CHECK_THROW(pool.set_num_threads(std::numeric_limits<uint32_t>::max(), create), std::length_error);
   give_back(pool, drain(pool, 4));
}

BOOST_AUTO_TEST_CASE(nested_concurrent_borrowing_has_one_exclusive_owner_per_resource) {
   for (unsigned threads : {1u, 2u, 4u, 8u}) {
      counts c;
      pool_type pool([&] { return new resource(c); });
      auto create = [&] { return new resource(c); };
      pool.set_num_threads(threads, create);
      pool.set_max_call_depth(3, create);
      std::atomic<bool> good{true};
      std::barrier start(threads);
      std::vector<std::jthread> workers;
      for (unsigned t = 0; t < threads; ++t) workers.emplace_back([&] {
         start.arrive_and_wait();
         for (unsigned round = 0; round < 1000; ++round) {
            std::vector<resource*> held;
            try {
               for (unsigned depth = 0; depth < 3; ++depth) {
                  auto* r = pool.acquire(); held.push_back(r);
                  if (r->occupied.exchange(true)) good = false;
                  ++r->calls;
               }
               if (round % 11 == 0) std::this_thread::yield();
            } catch (...) { good = false; }
            for (auto* r : held) {
               if (!r->occupied.exchange(false) || !pool.release(r)) good = false;
            }
         }
      });
      workers.clear(); // join BEFORE observing any non-atomic resource data
      BOOST_CHECK(good.load());
      const auto all = drain(pool, threads * 3);
      uint64_t total = 0;
      for (auto* r : all) { total += r->calls; BOOST_CHECK(!r->occupied.load()); }
      BOOST_CHECK_EQUAL(total, uint64_t(threads) * 3000);
      give_back(pool, all);
   }
}

BOOST_AUTO_TEST_CASE(slow_factory_does_not_hold_the_borrow_return_mutex) {
   counts c;
   pool_type pool([&] { return new resource(c); });
   auto* held = pool.acquire();
   std::promise<void> entered, resume;
   auto entered_future = entered.get_future(), resume_future = resume.get_future();
   auto growth = std::async(std::launch::async, [&] {
      pool.set_num_threads(2, [&] {
         entered.set_value(); resume_future.wait();
         return new resource(c);
      });
   });
   const bool reached_factory = entered_future.wait_for(2s) == std::future_status::ready;
   auto existing = std::async(std::launch::async, [&] {
      if (!pool.release(held)) return false;
      auto* r = pool.acquire();
      const bool same = r == held;
      return pool.release(r) && same;
   });
   const bool returned_before_factory = existing.wait_for(2s) == std::future_status::ready;
   resume.set_value(); // ALWAYS unblock the worker, including failed assertions
   growth.get();
   BOOST_CHECK(reached_factory); BOOST_CHECK(returned_before_factory); BOOST_CHECK(existing.get());
   give_back(pool, drain(pool, 2));
}

BOOST_AUTO_TEST_CASE(concurrent_dimension_changes_publish_a_complete_product) {
   counts c;
   pool_type pool([&] { return new resource(c); });
   std::barrier start(3);
   auto grow = [&](bool threads) {
      start.arrive_and_wait();
      if (threads) pool.set_num_threads(4, [&] { return new resource(c); });
      else pool.set_max_call_depth(3, [&] { return new resource(c); });
   };
   auto a = std::async(std::launch::async, grow, true), b = std::async(std::launch::async, grow, false);
   start.arrive_and_wait(); a.get(); b.get();
   BOOST_CHECK_EQUAL(c.made.load(), 12u);
   give_back(pool, drain(pool, 12));
}

BOOST_AUTO_TEST_SUITE_END()
