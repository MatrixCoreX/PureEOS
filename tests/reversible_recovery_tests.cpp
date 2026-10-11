#include <boost/test/unit_test.hpp>
#include <eosio/chain/reversible_recovery.hpp>
#include <eosio/chain/genesis_state.hpp>
#include <eosio/chain/exceptions.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <fstream>
#include <thread>
#include <tuple>

using namespace eosio::chain;

namespace {
struct recovery_fixture {
   fc::temp_directory temp;
   std::filesystem::path directory = temp.path() / "recovery";
   chain_id_type chain_id = genesis_state{}.compute_chain_id();

   signed_block_ptr block(const signed_block_ptr& previous = {}, uint32_t slot = 1, size_t payload_size = 0) {
      auto result = signed_block::create_mutable_block({});
      if (previous) result->previous = previous->calculate_id();
      result->timestamp = block_timestamp_type(slot);
      // Opaque storage payload, not a claim that this extension is protocol-valid.
      if (payload_size) result->block_extensions.emplace_back(0xf00d, bytes(payload_size, 'x'));
      return signed_block::create_signed_block(std::move(result));
   }
   auto path(const signed_block_ptr& block) { return directory / (block->calculate_id().str() + ".block"); }
   void write(const std::filesystem::path& path, const std::string& value) {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(value.data(), value.size());
      BOOST_REQUIRE(output.good());
   }
   std::string read(const std::filesystem::path& path) {
      std::ifstream input(path, std::ios::binary);
      BOOST_REQUIRE(input.good());
      std::string value{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      BOOST_REQUIRE(!input.bad());
      return value;
   }
   static void append_checksum(std::string& value) {
      const auto checksum = fc::sha256::hash(value.data(), value.size());
      value.append(checksum.data(), checksum.data_size());
   }
   std::string legacy_record(const signed_block_ptr& block) {
      // Independent reference using the pre-optimization v1 serialization order.
      const auto magic = fc::raw::pack(uint64_t{0x3152564345524550});
      const auto packed_chain = fc::raw::pack(chain_id);
      std::string value(magic.begin(), magic.end());
      value.append(packed_chain.data(), packed_chain.size());
      const auto& packed_block = block->packed_signed_block();
      value.append(packed_block.data(), packed_block.size());
      append_checksum(value);
      return value;
   }
};
}

BOOST_FIXTURE_TEST_SUITE(reversible_recovery_tests, recovery_fixture)

BOOST_AUTO_TEST_CASE(stored_bytes_match_v1_for_small_and_large_blocks) {
   reversible_recovery store(directory, chain_id);
   std::vector<signed_block_ptr> expected;
   for (const size_t payload_size : {0u, 1u, 4096u, 1024u * 1024u}) {
      auto item = block({}, expected.size() + 1, payload_size);
      store.store(item);
      // A single assertion per record, rather than millions of byte assertions.
      BOOST_CHECK(read(path(item)) == legacy_record(item));
      expected.push_back(std::move(item));
   }
   reversible_recovery reopened(directory, chain_id);
   auto loaded = reopened.load();
   BOOST_REQUIRE_EQUAL(loaded.size(), expected.size());
   std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
      return a->calculate_id() < b->calculate_id();
   });
   for (size_t i = 0; i < expected.size(); ++i) {
      BOOST_CHECK(loaded[i]->calculate_id() == expected[i]->calculate_id());
      BOOST_CHECK(fc::raw::pack(*loaded[i]) == expected[i]->packed_signed_block());
   }
}

BOOST_AUTO_TEST_CASE(legacy_records_keep_height_and_id_order_across_branches) {
   reversible_recovery store(directory, chain_id);
   auto root = block();
   std::vector<signed_block_ptr> expected{root};
   for (uint32_t slot = 2; slot != 18; ++slot) {
      auto fork = block(root, slot);
      expected.push_back(fork);
      expected.push_back(block(fork, slot + 20));
   }
   // Commit old-format bytes in reverse order, independently of store().
   for (auto it = expected.rbegin(); it != expected.rend(); ++it) write(path(*it), legacy_record(*it));
   std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
      return std::make_tuple(a->block_num(), a->calculate_id()) <
             std::make_tuple(b->block_num(), b->calculate_id());
   });
   auto loaded = store.load();
   BOOST_REQUIRE_EQUAL(loaded.size(), expected.size());
   for (size_t i = 0; i < expected.size(); ++i)
      BOOST_CHECK(loaded[i]->calculate_id() == expected[i]->calculate_id());
   store.prune(2);
   reversible_recovery reopened(directory, chain_id);
   loaded = reopened.load();
   BOOST_REQUIRE_EQUAL(loaded.size(), 16u);
   for (const auto& item : loaded) BOOST_CHECK_EQUAL(item->block_num(), 3u);
}

BOOST_AUTO_TEST_CASE(valid_checksum_does_not_hide_trailing_content_or_bad_magic) {
   auto item = block();
   reversible_recovery store(directory, chain_id);
   auto trailing = legacy_record(item);
   trailing.resize(trailing.size() - sizeof(fc::sha256));
   trailing.push_back('x');
   append_checksum(trailing);
   write(path(item), trailing);
   BOOST_CHECK_THROW(store.load(), fc::exception);

   auto bad_magic = legacy_record(item);
   bad_magic.resize(bad_magic.size() - sizeof(fc::sha256));
   bad_magic[0] ^= 1;
   append_checksum(bad_magic);
   write(path(item), bad_magic);
   BOOST_CHECK_THROW(store.load(), fc::exception);
}

BOOST_AUTO_TEST_CASE(empty_and_header_only_records_are_rejected) {
   auto item = block();
   reversible_recovery store(directory, chain_id);
   for (const size_t size : {0u, 7u, 40u, 71u, 72u}) {
      write(path(item), std::string(size, '\0'));
      BOOST_CHECK_THROW(store.load(), fc::exception);
   }
}

BOOST_AUTO_TEST_CASE(rename_failure_remains_retryable) {
   auto item = block();
   reversible_recovery store(directory, chain_id);
   const auto temporary = directory / (item->calculate_id().str() + ".tmp");
   std::filesystem::create_directory(path(item));
   BOOST_CHECK_THROW(store.store(item), std::filesystem::filesystem_error);
   BOOST_CHECK(std::filesystem::is_regular_file(temporary));
   BOOST_CHECK(read(temporary) == legacy_record(item));
   std::filesystem::remove(path(item)); // only the empty directory created by this test
   BOOST_CHECK_NO_THROW(store.store(item));
   BOOST_CHECK(!std::filesystem::exists(temporary));
   BOOST_CHECK(read(path(item)) == legacy_record(item));
   reversible_recovery reopened(directory, chain_id);
   auto loaded = reopened.load();
   BOOST_REQUIRE_EQUAL(loaded.size(), 1u);
   BOOST_CHECK(loaded.front()->calculate_id() == item->calculate_id());
}

BOOST_AUTO_TEST_CASE(reopen_all_branches_and_deduplicate) {
   auto a = block(), b = block(a, 2), c = block(a, 3);
   {
      reversible_recovery store(directory, chain_id);
      BOOST_CHECK(store.load().empty());
      store.store(c); store.store(a); store.store(b); store.store(a);
   }
   reversible_recovery reopened(directory, chain_id);
   auto blocks = reopened.load();
   BOOST_REQUIRE_EQUAL(blocks.size(), 3u);
   BOOST_CHECK(blocks.front()->calculate_id() == a->calculate_id());
   BOOST_CHECK(blocks[1]->previous == a->calculate_id());
   BOOST_CHECK(blocks[2]->previous == a->calculate_id());
}

BOOST_AUTO_TEST_CASE(prune_only_durable_heights_and_keep_both_live_branches) {
   auto a = block(), b = block(a, 2), c = block(a, 3), d = block(b, 4);
   reversible_recovery store(directory, chain_id);
   for (const auto& item : {a, b, c, d}) store.store(item);
   store.prune(1);
   store.store(a); // a delayed network thread must not resurrect retired records
   BOOST_CHECK(!std::filesystem::exists(path(a)));
   reversible_recovery reopened(directory, chain_id);
   BOOST_REQUIRE_EQUAL(reopened.load().size(), 3u);
   reopened.prune(2);
   reversible_recovery again(directory, chain_id);
   auto blocks = again.load();
   BOOST_REQUIRE_EQUAL(blocks.size(), 1u);
   BOOST_CHECK(blocks.front()->calculate_id() == d->calculate_id());
}

BOOST_AUTO_TEST_CASE(incomplete_temporary_write_does_not_hide_committed_block) {
   auto a = block();
   reversible_recovery store(directory, chain_id);
   store.store(a);
   write(directory / "interrupted.tmp", "torn write");
   reversible_recovery reopened(directory, chain_id);
   BOOST_REQUIRE_EQUAL(reopened.load().size(), 1u);
}

BOOST_AUTO_TEST_CASE(committed_truncation_is_fatal) {
   auto a = block();
   reversible_recovery store(directory, chain_id);
   store.store(a);
   std::filesystem::resize_file(path(a), 7);
   reversible_recovery reopened(directory, chain_id);
   BOOST_CHECK_THROW(reopened.load(), fc::exception);
}

BOOST_AUTO_TEST_CASE(committed_corruption_is_fatal) {
   auto a = block();
   reversible_recovery store(directory, chain_id);
   store.store(a);
   std::fstream file(path(a), std::ios::in | std::ios::out | std::ios::binary);
   file.seekp(48); file.put('\x7f'); file.close();
   reversible_recovery reopened(directory, chain_id);
   BOOST_CHECK_THROW(reopened.load(), fc::exception);
}

BOOST_AUTO_TEST_CASE(wrong_chain_and_wrong_filename_are_fatal) {
   auto a = block(), b = block(a, 2);
   reversible_recovery store(directory, chain_id);
   store.store(a);
   auto other = chain_id; other._hash[0] ^= 1;
   reversible_recovery wrong_chain(directory, other);
   BOOST_CHECK_THROW(wrong_chain.load(), fc::exception);
   std::filesystem::rename(path(a), path(b));
   reversible_recovery wrong_id(directory, chain_id);
   BOOST_CHECK_THROW(wrong_id.load(), fc::exception);
}

BOOST_AUTO_TEST_CASE(write_error_is_propagated_and_retry_is_not_deduplicated) {
   auto a = block();
   reversible_recovery store(directory, chain_id);
   auto temporary = directory / (a->calculate_id().str() + ".tmp");
   std::filesystem::create_directory(temporary);
   BOOST_CHECK_THROW(store.store(a), std::exception);
   BOOST_CHECK(!std::filesystem::exists(path(a)));
   std::filesystem::remove(temporary); // only the empty directory created by this test
   BOOST_CHECK_NO_THROW(store.store(a));
   reversible_recovery reopened(directory, chain_id);
   BOOST_REQUIRE_EQUAL(reopened.load().size(), 1u);
}

BOOST_AUTO_TEST_CASE(concurrent_duplicate_writers) {
   auto a = block(), b = block(a, 2);
   reversible_recovery store(directory, chain_id);
   auto write = [&] { for (int i = 0; i != 20; ++i) { store.store(a); store.store(b); } };
   std::thread first(write), second(write), third(write);
   first.join(); second.join(); third.join();
   reversible_recovery reopened(directory, chain_id);
   BOOST_REQUIRE_EQUAL(reopened.load().size(), 2u);
}

BOOST_AUTO_TEST_CASE(concurrent_pruning_and_late_writers) {
   auto a = block(), b = block(a, 2), c = block(b, 3);
   reversible_recovery store(directory, chain_id);
   std::thread writer([&] {
      for (int i = 0; i != 30; ++i) { store.store(a); store.store(b); store.store(c); }
   });
   std::thread collector([&] { store.prune(2); });
   writer.join(); collector.join();
   reversible_recovery reopened(directory, chain_id);
   auto blocks = reopened.load();
   BOOST_REQUIRE_EQUAL(blocks.size(), 1u);
   BOOST_CHECK(blocks.front()->calculate_id() == c->calculate_id());
}

BOOST_AUTO_TEST_SUITE_END()
