#include <eosio/chain/parallel_inline_action.hpp>
#include <fc/io/raw.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <future>
#include <limits>
#include <random>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
action inline_sample(size_t authorizations = 1, size_t bytes = 7) {
   action result;
   result.account = "target"_n; result.name = "transfer"_n;
   for (size_t i = 0; i < authorizations; ++i)
      result.authorization.push_back({name{uint64_t{100} + i}, name{uint64_t{200} + i}});
   for (size_t i = 0; i < bytes; ++i) result.data.push_back(static_cast<char>(i));
   return result;
}
bool same_action(const action& a, const action& b) {
   return a.account == b.account && a.name == b.name && a.authorization == b.authorization && a.data == b.data;
}
constexpr uint32_t unlimited_wire_limit = std::numeric_limits<uint32_t>::max();
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_inline_action_tests)

BOOST_AUTO_TEST_CASE(owned_decode_matches_original_action_and_preserves_declared_authorizations) {
   for (const size_t permissions : {0u, 1u, 2u, 64u}) for (const size_t bytes : {0u, 1u, 127u, 128u, 4096u}) {
      auto input = inline_sample(permissions, bytes);
      if (permissions == 2) input.authorization[1] = input.authorization[0]; // no invented deduplication
      if (!permissions) { input.account = {}; input.name = {}; } // codec is NOT account eligibility
      const auto packed = fc::raw::pack(input);
      const auto decoded = decode_isolated_inline_action(packed, unlimited_wire_limit);
      BOOST_REQUIRE(decoded);
      BOOST_CHECK(same_action(decoded->decoded, input));
      BOOST_CHECK(same_action(decoded->decoded, fc::raw::unpack<action>(packed)));
      BOOST_CHECK(decoded->packed == packed);
      BOOST_CHECK_EQUAL(decoded->decoded_bytes, packed.size());
   }
}

BOOST_AUTO_TEST_CASE(original_strict_wire_limit_and_local_caps_do_not_conflate) {
   const auto packed = fc::raw::pack(inline_sample());
   BOOST_CHECK(!decode_isolated_inline_action(packed, 0));
   BOOST_CHECK(!decode_isolated_inline_action(packed, packed.size() - 1));
   BOOST_CHECK(!decode_isolated_inline_action(packed, packed.size()));
   BOOST_REQUIRE(decode_isolated_inline_action(packed, packed.size() + 1));
   // 16 name bytes + two vector lengths (1 and 3 bytes) + payload.
   const auto at_cap = fc::raw::pack(inline_sample(0, isolated_inline_action::max_packed_bytes - 20));
   BOOST_REQUIRE_EQUAL(at_cap.size(), isolated_inline_action::max_packed_bytes);
   BOOST_CHECK(!decode_isolated_inline_action(at_cap, at_cap.size()));
   BOOST_REQUIRE(decode_isolated_inline_action(at_cap, at_cap.size() + 1));
   auto over_cap = at_cap; over_cap.push_back('x');
   BOOST_CHECK(!decode_isolated_inline_action(over_cap, unlimited_wire_limit));
   const auto too_many = fc::raw::pack(inline_sample(65));
   BOOST_CHECK(!decode_isolated_inline_action(too_many, unlimited_wire_limit));
   BOOST_CHECK(same_action(fc::raw::unpack<action>(too_many), inline_sample(65))); // canonical may accept more
}

BOOST_AUTO_TEST_CASE(truncated_fields_and_untrusted_counts_never_allocate_from_the_wire) {
   const auto packed = fc::raw::pack(inline_sample(4, 128));
   for (size_t bytes = 0; bytes < packed.size(); ++bytes)
      BOOST_CHECK(!decode_isolated_inline_action(std::span(packed).first(bytes), unlimited_wire_limit));
   // Auth/data counts are independently bounded before vector allocation.
   for (const size_t position : {16u, 17u}) for (const std::vector<char>& length : {
         std::vector<char>{char(0xff), char(0xff), char(0xff), char(0xff), char(0x0f)},
         std::vector<char>{char(0x80), char(0x80), char(0x80), char(0x80), char(0x10)},
         std::vector<char>{char(0x80), char(0x80), char(0x80), char(0x80), char(0x80), 0},
         std::vector<char>{char(0x80)}}) {
      auto invalid = fc::raw::pack(inline_sample(0, 0));
      invalid.resize(position); invalid.insert(invalid.end(), length.begin(), length.end());
      BOOST_CHECK(!decode_isolated_inline_action(invalid, unlimited_wire_limit));
   }
}

BOOST_AUTO_TEST_CASE(noncanonical_lengths_select_fallback_without_changing_original_acceptance) {
   for (size_t position : {16u, 17u}) {
      const auto original = inline_sample(0, 0);
      auto packed = fc::raw::pack(original);
      packed[position] = char(0x80); packed.insert(packed.begin() + position + 1, 0);
      BOOST_CHECK(same_action(fc::raw::unpack<action>(packed), original));
      BOOST_CHECK(!decode_isolated_inline_action(packed, unlimited_wire_limit));
   }
}

BOOST_AUTO_TEST_CASE(trailing_bytes_are_preserved_and_owned_separately_from_decoded_fields) {
   const auto input = inline_sample();
   auto packed = fc::raw::pack(input);
   const auto content_size = packed.size();
   packed.insert(packed.end(), {'\0', char(0xff), 'x'});
   const auto expected = packed;
   auto decoded = decode_isolated_inline_action(packed, unlimited_wire_limit);
   BOOST_REQUIRE(decoded);
   BOOST_CHECK(same_action(decoded->decoded, fc::raw::unpack<action>(packed)));
   BOOST_CHECK_EQUAL(decoded->decoded_bytes, content_size);
   BOOST_CHECK(decoded->packed == expected);
   BOOST_CHECK(!decode_isolated_inline_action(packed, content_size + 1)); // repacking would incorrectly pass
   std::fill(packed.begin(), packed.end(), 'z'); packed.clear(); packed.shrink_to_fit();
   BOOST_CHECK(decoded->packed == expected); BOOST_CHECK(same_action(decoded->decoded, input));
   std::fill(decoded->packed.begin(), decoded->packed.end(), 'y');
   BOOST_CHECK(same_action(decoded->decoded, input));
}

BOOST_AUTO_TEST_CASE(deterministic_malformed_bytes_and_valid_corpus_match_original_codec) {
   std::mt19937 random(0x1a11ce);
   for (unsigned round = 0; round < 4096; ++round) {
      std::vector<char> bytes(random() % 192);
      for (auto& byte : bytes) byte = static_cast<char>(random());
      const auto decoded = decode_isolated_inline_action(bytes, unlimited_wire_limit);
      if (decoded) {
         BOOST_CHECK(same_action(decoded->decoded, fc::raw::unpack<action>(bytes)));
         BOOST_CHECK(decoded->packed == bytes);
      }
   }
   for (unsigned round = 0; round < 512; ++round) {
      auto input = inline_sample(random() % 65, random() % 2048);
      input.account = name{uint64_t(random()) << 32 | random()};
      input.name = name{uint64_t(random()) << 32 | random()};
      auto bytes = fc::raw::pack(input);
      for (unsigned i = 0, count = random() % 7; i < count; ++i) bytes.push_back(static_cast<char>(random()));
      const auto decoded = decode_isolated_inline_action(bytes, unlimited_wire_limit);
      BOOST_REQUIRE(decoded);
      BOOST_CHECK(same_action(decoded->decoded, fc::raw::unpack<action>(bytes)));
      BOOST_CHECK(decoded->packed == bytes);
   }
}

BOOST_AUTO_TEST_CASE(one_two_four_threads_decode_owned_actions_without_shared_mutable_state) {
   std::vector<std::vector<char>> packed;
   for (unsigned i = 0; i < 64; ++i) packed.push_back(fc::raw::pack(inline_sample(i, i * 31)));
   for (size_t workers : {1u, 2u, 4u}) {
      std::vector<std::future<bool>> tasks;
      for (size_t thread = 0; thread < workers; ++thread) tasks.push_back(std::async(std::launch::async, [&] {
         for (unsigned repeat = 0; repeat < 8; ++repeat) for (const auto& bytes : packed) {
            auto decoded = decode_isolated_inline_action(bytes, unlimited_wire_limit);
            if (!decoded || !same_action(decoded->decoded, fc::raw::unpack<action>(bytes)) || decoded->packed != bytes) return false;
            decoded->packed.clear(); decoded->decoded.data.clear();
         }
         return true;
      }));
      for (auto& task : tasks) BOOST_CHECK(task.get());
   }
}

BOOST_AUTO_TEST_SUITE_END()
