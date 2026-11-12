#include <eosio/chain/parallel_transaction_environment.hpp>
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/transaction.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
using transaction_environment = isolated_transaction_environment;
transaction environment_transaction(size_t cf_count, size_t ordinary_count, size_t data_size) {
   transaction trx;
   trx.expiration = fc::time_point_sec{UINT32_MAX};
   trx.ref_block_num = UINT16_MAX; trx.ref_block_prefix = UINT32_MAX;
   trx.max_net_usage_words = fc::unsigned_int{128}; trx.max_cpu_usage_ms = 255;
   trx.delay_sec = fc::unsigned_int{16384}; // format-only fixture, not canonical eligibility
   for (size_t i = 0; i < cf_count; ++i)
      trx.context_free_actions.push_back(action{{}, "test"_n, "cf"_n, std::vector<char>(data_size, char(i))});
   for (size_t i = 0; i < ordinary_count; ++i)
      trx.actions.push_back(action{{{"test"_n, "active"_n}}, "test"_n, "run"_n, std::vector<char>(data_size, char(i + 1))});
   trx.transaction_extensions.emplace_back(60000, std::vector<char>{'x', '\0', 'y'});
   return trx;
}
void check_environment_wire(const transaction& trx) {
   const auto captured = transaction_environment::capture(trx, UINT64_MAX, 7, UINT32_MAX);
   BOOST_REQUIRE(captured);
   const auto bytes = fc::raw::pack(trx);
   BOOST_CHECK(captured->transaction_id() == trx.id());
   BOOST_CHECK_EQUAL(captured->transaction_size(), static_cast<int32_t>(bytes.size()));
   BOOST_CHECK_EQUAL(captured->owned_wire_bytes(), bytes.size());
   BOOST_CHECK_EQUAL(captured->current_time(), UINT64_MAX);
   BOOST_CHECK_EQUAL(captured->publication_time(), 7u);
   BOOST_CHECK_EQUAL(captured->block_num(), UINT32_MAX);
   BOOST_CHECK_EQUAL(captured->expiration(), UINT32_MAX);
   BOOST_CHECK_EQUAL(captured->tapos_block_num(), uint32_t{UINT16_MAX});
   BOOST_CHECK_EQUAL(captured->tapos_block_prefix(), UINT32_MAX);
   for (size_t size : {size_t{0}, size_t{1}, bytes.size() - 1, bytes.size(), bytes.size() + 5}) {
      std::vector<char> output(size, '?'), expected = output;
      const auto count = std::min(size, bytes.size());
      std::copy_n(bytes.begin(), count, expected.begin());
      BOOST_CHECK_EQUAL(captured->read_transaction(output), static_cast<int32_t>(size ? count : bytes.size()));
      BOOST_CHECK(output == expected);
   }
   const std::array lists{&trx.context_free_actions, &trx.actions};
   for (uint32_t type = 0; type < 2; ++type) {
      const auto& actions = *lists[type]; BOOST_CHECK_EQUAL(captured->action_count(type), actions.size());
      for (uint32_t index = 0; index < actions.size(); ++index) {
         const auto packed = fc::raw::pack(actions[index]);
         for (size_t size : {size_t{0}, size_t{1}, packed.size() - 1, packed.size(), packed.size() + 5}) {
            std::vector<char> output(size, '?'), expected = output;
            if (size >= packed.size()) std::copy(packed.begin(), packed.end(), expected.begin());
            BOOST_CHECK_EQUAL(captured->get_action(type, index, output), static_cast<int32_t>(packed.size()));
            BOOST_CHECK(output == expected); // short buffers are untouched, NOT prefix copies
         }
      }
      std::array<char, 3> sentinel{'?', '?', '?'};
      BOOST_CHECK_EQUAL(captured->get_action(type, actions.size(), sentinel), -1);
      BOOST_CHECK((sentinel == std::array<char, 3>{'?', '?', '?'}));
   }
   std::array<char, 3> sentinel{'?', '?', '?'};
   BOOST_CHECK_THROW(captured->get_action(2, 0, sentinel), primary_state_unsupported);
   BOOST_CHECK_THROW(captured->get_action(UINT32_MAX, 0, sentinel), primary_state_unsupported);
   BOOST_CHECK((sentinel == std::array<char, 3>{'?', '?', '?'}));
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_transaction_environment_tests)

BOOST_AUTO_TEST_CASE(original_serialization_action_ranges_and_short_buffers_match_at_varuint_boundaries) {
   for (size_t cf : {0u, 1u, 127u, 128u}) for (size_t ordinary : {0u, 1u, 127u, 128u})
      for (size_t data : {0u, 1u, 127u, 128u})
         check_environment_wire(environment_transaction(cf, ordinary, data));
}

BOOST_AUTO_TEST_CASE(owned_bytes_are_immutable_compression_independent_and_bound_to_all_context_values) {
   auto trx = environment_transaction(1, 2, 128);
   const auto original_id = trx.id(); const auto wire = fc::raw::pack(trx);
   const auto captured = transaction_environment::capture(trx, 11, 12, 13);
   BOOST_REQUIRE(captured->matches(original_id, 11, 12, 13));
   const auto same = transaction_environment::capture(trx, 11, 12, 13);
   BOOST_CHECK(same.get() != captured.get()); BOOST_CHECK(same->fingerprint() == captured->fingerprint());
   for (unsigned part = 0; part < 4; ++part) {
      auto changed = trx; if (part == 0) ++changed.ref_block_prefix;
      const auto other = transaction_environment::capture(changed, 11 + (part == 1), 12 + (part == 2), 13 + (part == 3));
      BOOST_CHECK(other->fingerprint() != captured->fingerprint());
      BOOST_CHECK(!captured->matches(changed.id(), 11 + (part == 1), 12 + (part == 2), 13 + (part == 3)));
   }
   for (auto compression : {packed_transaction::compression_type::none, packed_transaction::compression_type::zlib}) {
      signed_transaction signed_trx; static_cast<transaction&>(signed_trx) = trx;
      signed_trx.context_free_data = {{'z', 'z'}};
      const packed_transaction packed(signed_trx, compression);
      const auto from_packet = transaction_environment::capture(packed.get_transaction(), 11, 12, 13);
      BOOST_CHECK(from_packet->fingerprint() == captured->fingerprint());
      BOOST_CHECK(from_packet->transaction_id() == packed.id());
   }
   trx.actions.front().data.assign(200, 'z');
   trx.actions.clear(); trx.context_free_actions.clear(); trx.transaction_extensions.clear();
   std::vector<char> output(wire.size());
   BOOST_CHECK_EQUAL(captured->read_transaction(output), static_cast<int32_t>(wire.size())); BOOST_CHECK(output == wire);
   BOOST_CHECK(captured->matches(original_id, 11, 12, 13));
}

BOOST_AUTO_TEST_CASE(byte_count_action_count_authority_and_data_caps_fail_before_owning_oversized_input) {
   auto trx = environment_transaction(0, 8, transaction_environment::max_action_bytes);
   const size_t overhead = fc::raw::pack_size(trx) - transaction_environment::max_wire_bytes;
   BOOST_REQUIRE_LT(overhead, trx.actions.back().data.size() - 16384u);
   trx.actions.back().data.resize(trx.actions.back().data.size() - overhead);
   BOOST_REQUIRE_EQUAL(fc::raw::pack_size(trx), transaction_environment::max_wire_bytes);
   BOOST_CHECK(transaction_environment::capture(trx, 1, 2, 3));
   trx.actions.back().data.push_back('x');
   BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3), primary_state_budget_exceeded);
   trx = environment_transaction(512, 512, 0);
   BOOST_CHECK(transaction_environment::capture(trx, 1, 2, 3));
   trx.actions.push_back(trx.actions.back());
   BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3), primary_state_budget_exceeded);
   trx = environment_transaction(0, 1, transaction_environment::max_action_bytes + 1);
   BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3), primary_state_budget_exceeded);
   trx = environment_transaction(0, 1, 0);
   trx.actions[0].authorization.resize(transaction_environment::max_authorizations);
   BOOST_CHECK(transaction_environment::capture(trx, 1, 2, 3));
   trx.actions[0].authorization.resize(transaction_environment::max_authorizations + 1);
   BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3), primary_state_budget_exceeded);
   trx = environment_transaction(0, 1, 0);
   trx.transaction_extensions.resize(transaction_environment::max_extensions);
   BOOST_CHECK(transaction_environment::capture(trx, 1, 2, 3));
   trx.transaction_extensions.resize(transaction_environment::max_extensions + 1);
   BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3), primary_state_budget_exceeded);
   trx = environment_transaction(0, 0, 0);
   trx.transaction_extensions.front().second.resize(transaction_environment::max_wire_bytes + 1);
   BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3), primary_state_budget_exceeded);
}

BOOST_AUTO_TEST_CASE(owner_checkpoint_errors_propagate_at_every_capture_boundary_without_changing_input) {
   const auto trx = environment_transaction(2, 3, 128); const auto original = fc::raw::pack(trx);
   size_t count = 0;
   BOOST_REQUIRE(transaction_environment::capture(trx, 1, 2, 3, [&] { ++count; }));
   BOOST_REQUIRE_GT(count, 10u);
   for (size_t cutoff = 1; cutoff <= count; ++cutoff) {
      size_t seen = 0;
      BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3, [&] {
         if (++seen == cutoff) throw std::runtime_error("cancelled environment capture");
      }), std::runtime_error);
      BOOST_CHECK_EQUAL(seen, cutoff); BOOST_CHECK(fc::raw::pack(trx) == original);
   }
   BOOST_CHECK(transaction_environment::capture(trx, 1, 2, 3)->transaction_id() == trx.id());
}

BOOST_AUTO_TEST_CASE(caller_wire_budget_is_checked_before_capture_and_equal_content_is_not_pointer_identity) {
   const auto trx = environment_transaction(1, 2, 128);
   const auto bytes = fc::raw::pack(trx);
   for (size_t budget : {size_t{0}, size_t{1}, bytes.size() - 1})
      BOOST_CHECK_THROW(transaction_environment::capture(trx, 1, 2, 3, {}, budget), primary_state_budget_exceeded);
   const auto exact = transaction_environment::capture(trx, 1, 2, 3, {}, bytes.size());
   const auto larger = transaction_environment::capture(trx, 1, 2, 3, {}, bytes.size() + 1);
   BOOST_REQUIRE(exact.get() != larger.get());
   BOOST_CHECK(same_isolated_environment(exact, larger));
   BOOST_CHECK(same_isolated_environment({}, {}));
   BOOST_CHECK(!same_isolated_environment(exact, {}));
   BOOST_CHECK(!same_isolated_environment({}, larger));
   BOOST_CHECK(!same_isolated_environment(exact, transaction_environment::capture(trx, 1, 2, 4)));
   BOOST_CHECK(fc::raw::pack(trx) == bytes);
}

BOOST_AUTO_TEST_CASE(every_transaction_field_is_bound_and_shared_reads_do_not_mutate_owned_storage) {
   const auto original = environment_transaction(1, 2, 128);
   const auto captured = transaction_environment::capture(original, 1, 2, 3);
   // All reflected header/body fields, not just the action currently running.
   for (unsigned field = 0; field < 12; ++field) {
      auto changed = original;
      switch (field) {
         case 0: changed.expiration = fc::time_point_sec{1}; break;
         case 1: changed.ref_block_num = 1; break;
         case 2: changed.ref_block_prefix = 1; break;
         case 3: changed.max_net_usage_words = fc::unsigned_int{1}; break;
         case 4: changed.max_cpu_usage_ms = 1; break;
         case 5: changed.delay_sec = fc::unsigned_int{1}; break;
         case 6: changed.context_free_actions.front().data.push_back('x'); break;
         case 7: changed.actions.back().data.push_back('x'); break;
         case 8: changed.actions.front().authorization.front().permission = "owner"_n; break;
         case 9: changed.actions.front().account = "other"_n; break;
         case 10: changed.actions.front().name = "other"_n; break;
         case 11: changed.transaction_extensions.front().second.push_back('x'); break;
      }
      const auto other = transaction_environment::capture(changed, 1, 2, 3);
      BOOST_CHECK(!captured->matches(changed.id(), 1, 2, 3));
      BOOST_CHECK(captured->fingerprint() != other->fingerprint());
   }
   const auto wire = fc::raw::pack(original);
   const auto action_wire = fc::raw::pack(original.actions.back());
   for (size_t workers : {1u, 2u, 4u}) {
      std::atomic<bool> equal{true};
      std::vector<std::jthread> threads;
      for (size_t worker = 0; worker < workers; ++worker) threads.emplace_back([&, captured] {
         for (size_t iteration = 0; iteration < 100; ++iteration) {
            std::vector<char> tx_output(wire.size()), action_output(action_wire.size());
            if (captured->read_transaction(tx_output) != int32_t(wire.size()) || tx_output != wire ||
                captured->get_action(1, 1, action_output) != int32_t(action_wire.size()) || action_output != action_wire ||
                !captured->matches(original.id(), 1, 2, 3)) equal.store(false);
         }
      });
      threads.clear(); // join before checking; no Boost assertions in worker threads
      BOOST_CHECK(equal.load());
   }
   BOOST_CHECK_THROW(captured->action_count(2), primary_state_unsupported);
   std::array<char, 3> sentinel{'?', '?', '?'};
   BOOST_CHECK_EQUAL(captured->get_action(1, UINT32_MAX, sentinel), -1);
   BOOST_CHECK((sentinel == std::array<char, 3>{'?', '?', '?'}));
}

BOOST_AUTO_TEST_SUITE_END()
