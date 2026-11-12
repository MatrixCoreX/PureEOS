#define BOOST_TEST_MODULE incoming_transaction_batch
#include <boost/test/included/unit_test.hpp>
#include <eosio/producer_plugin/incoming_transaction_batch.hpp>
#include <eosio/chain/parallel_production_admission.hpp>
#include <chrono>
#include <limits>
#include <stdexcept>

using eosio::detail::incoming_transaction_batch;
using eosio::detail::incoming_once;
using eosio::detail::incoming_microbatch_policy;

struct entry {
   std::unique_ptr<unsigned> id;
   bool api;
};

BOOST_AUTO_TEST_CASE(microbatch_wait_is_bounded_and_leaves_block_deadline_margin) {
   using policy = incoming_microbatch_policy;
   BOOST_CHECK_EQUAL(policy::delay_us(std::numeric_limits<int64_t>::min()), 0);
   BOOST_CHECK_EQUAL(policy::delay_us(-1), 0);
   BOOST_CHECK_EQUAL(policy::delay_us(0), 0);
   BOOST_CHECK_EQUAL(policy::delay_us(5'000), 0);
   BOOST_CHECK_EQUAL(policy::delay_us(5'001), 1);
   BOOST_CHECK_EQUAL(policy::delay_us(6'999), 1'999);
   BOOST_CHECK_EQUAL(policy::delay_us(7'000), 2'000);
   BOOST_CHECK_EQUAL(policy::delay_us(std::numeric_limits<int64_t>::max()), 2'000);
   BOOST_CHECK_EQUAL(policy::delay_us(std::numeric_limits<int64_t>::max(), false), 0);
   BOOST_CHECK_EQUAL(policy::delay_us(100'000, false), 0); // no useful parallel preparation window
   for (int64_t remaining = 1; remaining < 20'000; ++remaining) {
      const auto wait = policy::delay_us(remaining);
      BOOST_CHECK(wait >= 0 && wait <= policy::max_wait_us);
      if (wait) BOOST_CHECK(remaining - wait >= policy::deadline_reserve_us);
   }
}

BOOST_AUTO_TEST_CASE(microbatch_flush_target_is_useful_and_fits_the_holding_area) {
   BOOST_CHECK_EQUAL(incoming_microbatch_policy::target(0), 2u);
   BOOST_CHECK_EQUAL(incoming_microbatch_policy::target(1), 2u);
   BOOST_CHECK_EQUAL(incoming_microbatch_policy::target(12), 12u);
   BOOST_CHECK_EQUAL(incoming_microbatch_policy::target(24), 24u);
   BOOST_CHECK_EQUAL(incoming_microbatch_policy::target(64), 64u);
   BOOST_CHECK_EQUAL(incoming_microbatch_policy::target(std::numeric_limits<size_t>::max()), 64u);
}

BOOST_AUTO_TEST_CASE(fifo_and_refusal_do_not_consume_or_reorder) {
   incoming_transaction_batch<entry, 3> q(12);
   for (unsigned i = 0; i < 3; ++i) {
      entry value{std::make_unique<unsigned>(i), bool(i % 2)};
      BOOST_REQUIRE(q.push(std::move(value), 4));
      BOOST_CHECK(!value.id);
   }
   entry extra{std::make_unique<unsigned>(9), false};
   BOOST_CHECK(!q.push(std::move(extra), 0)); // count full, even zero bytes
   BOOST_REQUIRE(extra.id);
   BOOST_CHECK_EQUAL(*extra.id, 9u);
   BOOST_CHECK_EQUAL(q.bytes(), 12u);
   auto batch = q.take();
   BOOST_CHECK(q.empty());
   BOOST_CHECK_EQUAL(q.bytes(), 0u);
   BOOST_REQUIRE_EQUAL(batch.size(), 3u);
   for (unsigned i = 0; i < 3; ++i) {
      BOOST_CHECK_EQUAL(*batch[i].id, i);
      BOOST_CHECK_EQUAL(batch[i].api, bool(i % 2));
   }
   BOOST_CHECK(q.push(std::move(extra), 12));
   BOOST_CHECK(!q.can_fit(1));
   q.clear();
   BOOST_CHECK(q.empty());
   BOOST_CHECK_EQUAL(q.bytes(), 0u);
   BOOST_CHECK(q.can_fit(12));
}

BOOST_AUTO_TEST_CASE(byte_boundary_zero_and_overflow) {
   incoming_transaction_batch<entry> q(10);
   entry value{std::make_unique<unsigned>(7), true};
   BOOST_CHECK(!q.push(std::move(value), 11));
   BOOST_REQUIRE(value.id);
   BOOST_CHECK(!q.can_fit(std::numeric_limits<size_t>::max()));
   BOOST_CHECK(q.empty());
   BOOST_CHECK(q.push(std::move(value), 10));
   BOOST_CHECK(!q.can_fit(std::numeric_limits<size_t>::max()));
   incoming_transaction_batch<entry> zero(0);
   BOOST_CHECK(!zero.can_fit(1));
   incoming_transaction_batch<entry> huge(std::numeric_limits<size_t>::max());
   entry large{std::make_unique<unsigned>(1), false};
   BOOST_REQUIRE(huge.push(std::move(large), std::numeric_limits<size_t>::max() - 1));
   BOOST_CHECK(!huge.can_fit(2));
   BOOST_CHECK(huge.can_fit(1));
}

BOOST_AUTO_TEST_CASE(detached_batch_is_stable_during_reentrant_use) {
   incoming_transaction_batch<entry, 2> q(100);
   entry a{std::make_unique<unsigned>(1), true}, b{std::make_unique<unsigned>(2), false};
   BOOST_REQUIRE(q.push(std::move(a), 20));
   auto detached = q.take();
   BOOST_REQUIRE(q.push(std::move(b), 30));
   BOOST_CHECK_EQUAL(*detached.front().id, 1u);
   BOOST_CHECK_EQUAL(q.bytes(), 30u);
   BOOST_CHECK_EQUAL(*q.take().front().id, 2u);
}

BOOST_AUTO_TEST_CASE(clear_releases_callback_ownership) {
   auto owner = std::make_shared<int>(42);
   std::weak_ptr<int> observed = owner;
   incoming_transaction_batch<std::function<void()>> q(100);
   std::function<void()> fn = [owner] {};
   owner.reset();
   BOOST_REQUIRE(q.push(std::move(fn), 10));
   BOOST_CHECK(!observed.expired());
   q.clear();
   BOOST_CHECK(observed.expired());
}

BOOST_AUTO_TEST_CASE(delivery_aliases_share_one_result) {
   unsigned calls = 0;
   auto callback = incoming_once<unsigned>([&](unsigned value) { ++calls; BOOST_CHECK_EQUAL(value, 7u); });
   auto retry = callback;
   callback(7);
   retry(9);
   callback(11);
   BOOST_CHECK_EQUAL(calls, 1u);
}

BOOST_AUTO_TEST_CASE(delivery_is_consumed_before_reentrancy_and_throw) {
   unsigned calls = 0;
   std::function<void(const unsigned&)> callback;
   callback = incoming_once<unsigned>([&](unsigned value) {
      ++calls;
      callback(value);
      throw std::runtime_error("callback failure");
   });
   auto retry = callback;
   BOOST_CHECK_THROW(callback(1), std::runtime_error);
   BOOST_CHECK_NO_THROW(retry(2));
   BOOST_CHECK_EQUAL(calls, 1u);
   auto empty = incoming_once<unsigned>({});
   BOOST_CHECK_NO_THROW(empty(0));
}

BOOST_AUTO_TEST_CASE(single_item_recovery_probe_restores_parallel_admission_with_backlog) {
   using queue = incoming_transaction_batch<entry>;
   using admission = eosio::chain::experimental::parallel_production_admission<unsigned>;
   admission policy;
   const auto now = std::chrono::steady_clock::now();
   policy.observe(7, 4, 32, 4, 50000, 50000, admission::outcome::deadline, now);
   policy.observe(7, 4, 32, 2, 50000, 50000, admission::outcome::deadline, now);
   BOOST_REQUIRE_EQUAL(policy.recommend(7, 4, 32, 50000, now), 1u);
   // The old intake condition (>1) supplies no observations and stays stuck.
   for (unsigned i = 0; i < 8; ++i) {
      const auto hint = policy.recommend(7, 4, 32, 50000, now);
      BOOST_CHECK_EQUAL(hint > 1 ? hint : 0, 0u);
   }
   for (unsigned i = 0; i < 4; ++i) {
      const auto offered = queue::preparation_count(64, policy.recommend(7, 4, 32, 50000, now));
      BOOST_REQUIRE_EQUAL(offered, 1u);
      policy.observe(7, 4, 32, offered, 1000, 50000, admission::outcome::ready, now);
   }
   BOOST_CHECK_EQUAL(queue::preparation_count(64, policy.recommend(7, 4, 32, 50000, now)), 2u);
   policy.observe(7, 4, 32, 1, 50000, 50000, admission::outcome::deadline, now);
   BOOST_CHECK_EQUAL(queue::preparation_count(64, policy.recommend(7, 4, 32, 50000, now)), 0u);
   BOOST_CHECK_EQUAL(queue::preparation_count(64, policy.recommend(7, 4, 32, 50000, now + std::chrono::seconds(2))), 1u);
   BOOST_CHECK_EQUAL(queue::preparation_count(1, 4), 0u);
   BOOST_CHECK_EQUAL(queue::preparation_count(0, 4), 0u);
   BOOST_CHECK_EQUAL(queue::preparation_count(3, 32), 3u);
   BOOST_CHECK_EQUAL(queue::preparation_count(100, 100), 64u);
}
