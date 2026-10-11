#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <eosio/chain/block_state.hpp>
#include <eosio/chain/protocol_state_object.hpp>
#include <eosio/chain/block_log_config.hpp>
#include <eosio/chain/rotation_context.hpp>
#include <eosio/chain/rotation_context_cache.hpp>
#include <fc/io/json.hpp>
#include <boost/test/unit_test.hpp>
#include <thread>

#include "snapshot_suites.hpp"

using namespace eosio::chain;
using namespace eosio::testing;

namespace {

genesis_state native_genesis() {
   auto genesis = base_tester::default_genesis();
   auto [priv, pub, pop] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = pub;
   genesis.initial_savanna->proof_of_possession = pop;
   return genesis;
}

void enable_local_finalizer(base_tester& chain) {
   const std::array<account_name, 1> names = {"eosio"_n};
   chain.set_node_finalizers(names);
}

// Independent baseline serializer (the pre-cache wire format), including all
// transition fields. Used while policies and keys advance, not just at genesis.
// Use the existing public reflection metadata instead of redefining controller's
// private block_handle_accessor (which would violate the one-definition rule).
struct savanna_head_reader {
   const block_handle& handle;
   block_state_ptr& result;
   template<typename Member, typename Class, Member Class::*member>
   void operator()(const char*) const { result = std::get<block_state_ptr>(handle.*member); }
};

std::vector<char> uncached_rotation_context(const base_tester& chain) {
   block_state_ptr head;
   fc::reflector<block_handle>::visit(savanna_head_reader{chain.control->head(), head});
      rotation_context_v1 r;
      r.block_num = head->block_num();
      r.slot = head->timestamp().slot;
      r.final_block_num = head->core.last_final_block_num();
      r.final_slot = head->core.last_final_block_timestamp().slot;
      r.proposer_version = head->active_schedule_auth().version;
      r.finalizer_generation = head->active_finalizer_policy->generation;
      r.latest_finalizer_generation = head->finalizer_policy_generation;
      r.transitioning = bool(head->latest_proposed_proposer_policy) || bool(head->latest_pending_proposer_policy)
         || bool(head->pending_finalizer_policy) || !head->proposed_finalizer_policies.empty();
      r.producers = head->active_schedule_auth().producers;
      r.threshold = head->active_finalizer_policy->threshold;
      r.finalizers = head->active_finalizer_policy->finalizers;
      return fc::raw::pack(r);
}

void assert_native_head(const base_tester& chain) {
   BOOST_REQUIRE(chain.control->head().header().is_proper_svnn_block());
   const auto policy = chain.control->head_active_finalizer_policy();
   BOOST_REQUIRE(policy);
   BOOST_REQUIRE_EQUAL(policy->finalizers.size(), 1u);
   BOOST_CHECK_EQUAL(policy->threshold, 1u);
   BOOST_CHECK_EQUAL(policy->finalizers.front().weight, 1u);
   BOOST_CHECK_EQUAL(policy->finalizers.front().description, "eosio");
   BOOST_REQUIRE_EQUAL(chain.control->head_active_producers().producers.size(), 1u);
   BOOST_CHECK(chain.control->head_active_producers().producers.front().producer_name == "eosio"_n);
}

} // namespace

BOOST_AUTO_TEST_SUITE(pureeos_genesis_tests)

// Explicit opt-in microbenchmark. Measures two intrinsic-style accesses per
// parent (size probe + payload read), NOT chain TPS or SSD performance. The
// 21-member payload is a synthetic serialization fixture, not a live schedule.
BOOST_AUTO_TEST_CASE(rotation_cache_microbenchmark, *boost::unit_test::disabled()) {
   fc::temp_directory dir;
   tester chain(base_tester::default_config(dir).first, native_genesis());
   const auto seed = fc::raw::unpack<rotation_context_v1>(uncached_rotation_context(chain));
   for (const unsigned members : {1u, 21u}) {
      auto payload = seed;
      payload.producers.resize(members, seed.producers.front());
      payload.finalizers.resize(members, seed.finalizers.front());
      for (unsigned i = 0; i < members; ++i) {
         payload.producers[i].producer_name = account_name(i + 1);
         payload.finalizers[i].description = payload.producers[i].producer_name.to_string();
      }
      constexpr unsigned iterations = 5000;
      std::vector<block_id_type> parents;
      for (unsigned i = 0; i < iterations; ++i) parents.push_back(fc::sha256::hash(std::to_string(i)));
      auto pack = [&] { auto copy = payload; return fc::raw::pack(copy); };
      for (unsigned repeat = 0; repeat < 5; ++repeat) {
         uint64_t old_bytes = 0, new_bytes = 0;
         rotation_context_cache cache;
         auto uncached = [&] {
            for (unsigned i = 0; i < iterations; ++i) {
               auto probe = pack(), data = pack();
               old_bytes += probe.size() + data.size() + uint8_t(data.back());
            }
         };
         auto cached = [&] {
            for (unsigned i = 0; i < iterations; ++i) {
               auto probe = cache.get(parents[i], pack), data = cache.get(parents[i], pack);
               new_bytes += probe->size() + data->size() + uint8_t(data->back());
            }
         };
         auto measure = [](auto&& run) {
            auto start = std::chrono::steady_clock::now();
            run();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
         };
         int64_t old_ns, new_ns;
         if (repeat % 2) { new_ns = measure(cached); old_ns = measure(uncached); }
         else { old_ns = measure(uncached); new_ns = measure(cached); }
         BOOST_REQUIRE_EQUAL(old_bytes, new_bytes);
         BOOST_REQUIRE_EQUAL(cache.snapshot().builds, iterations);
         BOOST_REQUIRE_EQUAL(cache.snapshot().hits, iterations);
         BOOST_TEST_MESSAGE("CACHE_MICRO members=" << members << " repeat=" << repeat << " iterations=" << iterations
                            << " payload_bytes=" << pack().size() << " uncached_ns=" << old_ns << " cached_ns=" << new_ns);
      }
   }
}

BOOST_AUTO_TEST_CASE(rotation_cache_preserves_empty_legacy_context) {
   fc::temp_directory dir;
   tester chain(base_tester::default_config(dir).first, base_tester::default_genesis());
   BOOST_REQUIRE(!chain.head().header().is_proper_svnn_block());
   const auto empty = chain.control->get_rotation_context_v1_cached();
   BOOST_REQUIRE(empty);
   BOOST_CHECK(empty->empty());
   BOOST_CHECK(chain.control->get_rotation_context_v1().empty());
   chain.produce_block();
   BOOST_CHECK(empty == chain.control->get_rotation_context_v1_cached());
   BOOST_CHECK_EQUAL(chain.control->get_rotation_context_cache_metrics().builds, 0u);
}

BOOST_AUTO_TEST_CASE(rotation_cache_lifetime_exception_and_concurrent_reads) {
   rotation_context_cache cache;
   const auto a = fc::sha256::hash("parent a");
   const auto b = fc::sha256::hash("parent b");
   // No arbitrary 64 KiB ceiling. Holding the old result must remain safe after eviction.
   auto large = cache.get(a, [] { return std::vector<char>(131072, 'a'); });
   std::vector<std::thread> readers;
   std::atomic<unsigned> wrong{0};
   for (unsigned i = 0; i < 8; ++i)
      readers.emplace_back([&] {
         for (unsigned j = 0; j < 100; ++j)
            if (cache.get(a, [] { throw std::runtime_error("unexpected rebuild"); return std::vector<char>{}; }) != large)
               ++wrong;
      });
   for (auto& reader : readers) reader.join();
   BOOST_CHECK_EQUAL(wrong.load(), 0u);
   BOOST_CHECK_EQUAL(cache.snapshot().builds, 1u);
   BOOST_CHECK_EQUAL(cache.snapshot().hits, 800u);
   BOOST_CHECK_THROW(cache.get(b, []() -> std::vector<char> { throw std::runtime_error("build failure"); }), std::runtime_error);
   BOOST_CHECK(cache.get(a, [] { return std::vector<char>{}; }) == large);
   auto next = cache.get(b, [] { return std::vector<char>{'b'}; });
   BOOST_CHECK_EQUAL(next->front(), 'b');
   BOOST_CHECK_EQUAL(large->size(), 131072u);
   BOOST_CHECK_EQUAL(large->back(), 'a');
   BOOST_CHECK_EQUAL(cache.snapshot().bytes, 1u);
   BOOST_CHECK_EQUAL(cache.snapshot().builds, 2u);
   BOOST_CHECK(cache.get(a, [] { return std::vector<char>{'c'}; }) != large);
   BOOST_CHECK_EQUAL(cache.snapshot().builds, 3u);
}

BOOST_AUTO_TEST_CASE(rotation_cache_tracks_parent_not_pending_or_local_votes) {
   fc::temp_directory dir;
   tester chain(base_tester::default_config(dir).first, native_genesis());
   const auto first = chain.control->get_rotation_context_v1_cached();
   BOOST_CHECK(first == chain.control->get_rotation_context_v1_cached());
   BOOST_CHECK(*first == chain.control->get_rotation_context_v1());
   BOOST_CHECK(*first == uncached_rotation_context(chain));
   auto decoded = fc::raw::unpack<rotation_context_v1>(*first);
   BOOST_CHECK_EQUAL(decoded.version, 1u);
   BOOST_CHECK_EQUAL(decoded.block_num, chain.head().block_num());
   BOOST_CHECK_EQUAL(decoded.slot, chain.head().timestamp().slot);
   BOOST_CHECK_EQUAL(decoded.finalizers.size(), 1u);
   chain.control->abort_block();
   enable_local_finalizer(chain);
   BOOST_CHECK(first == chain.control->get_rotation_context_v1_cached());
   chain.produce_block();
   const auto next = chain.control->get_rotation_context_v1_cached();
   BOOST_CHECK(next != first);
   BOOST_CHECK_EQUAL(fc::raw::unpack<rotation_context_v1>(*next).block_num, decoded.block_num + 1);
   BOOST_CHECK(*next == chain.control->get_rotation_context_v1());
   BOOST_CHECK(*next == uncached_rotation_context(chain));
   BOOST_CHECK_EQUAL(fc::raw::unpack<rotation_context_v1>(*first).block_num, decoded.block_num);
}

BOOST_AUTO_TEST_CASE(rotation_cache_does_not_reuse_same_height_fork) {
   fc::temp_directory a_dir, b_dir, observer_dir;
   const auto genesis = native_genesis();
   tester a(base_tester::default_config(a_dir).first, genesis);
   tester b(base_tester::default_config(b_dir).first, genesis);
   tester observer(base_tester::default_config(observer_dir).first, genesis);
   auto a_block = a.produce_block();
   auto b_block = b.produce_block(fc::seconds(1));
   BOOST_REQUIRE_EQUAL(a.head().block_num(), b.head().block_num());
   BOOST_REQUIRE(a.head().id() != b.head().id());
   observer.push_block(a_block);
   const auto old_bytes = observer.control->get_rotation_context_v1_cached();
   observer.push_block(b_block);
   // A later timestamp at the same height is preferred by Savanna fork choice.
   BOOST_REQUIRE(observer.head().id() == b.head().id());
   const auto fork_bytes = observer.control->get_rotation_context_v1_cached();
   BOOST_CHECK(fork_bytes != old_bytes);
   BOOST_CHECK(*fork_bytes == b.control->get_rotation_context_v1());
   BOOST_CHECK(*fork_bytes != *old_bytes);
}

BOOST_AUTO_TEST_CASE(genesis_identity_and_validation) {
   auto old = base_tester::default_genesis();
   BOOST_CHECK(old.compute_chain_id() == chain_id_type{fc::sha256::hash(old.legacy_state())});
   const auto genesis = native_genesis();
   BOOST_REQUIRE_NO_THROW(genesis.validate());
   BOOST_CHECK(genesis.compute_chain_id() != old.compute_chain_id());
   BOOST_CHECK(genesis != old);
   BOOST_CHECK(fc::raw::unpack<genesis_state>(fc::raw::pack(genesis)) == genesis);
   BOOST_CHECK(fc::json::from_string(fc::json::to_string(genesis, fc::time_point::maximum())).as<genesis_state>() == genesis);
   // JSON may omit the canonical manifest, but never the finalizer credentials.
   auto minimal = fc::mutable_variant_object(fc::variant(genesis).get_object());
   minimal["initial_savanna"] = fc::mutable_variant_object()
      ("version", 1)("finalizer_key", genesis.initial_savanna->finalizer_key)
      ("proof_of_possession", genesis.initial_savanna->proof_of_possession);
   BOOST_CHECK(fc::variant(minimal).as<genesis_state>() == genesis);

   auto bad = genesis;
   bad.initial_savanna->version = 3;
   BOOST_CHECK_THROW(bad.validate(), fc::exception);
   bad = genesis;
   bad.initial_savanna->protocol_features.pop_back();
   BOOST_CHECK_THROW(bad.validate(), fc::exception);
   bad = genesis;
   auto [other_priv, other_pub, other_pop] = get_bls_key("other"_n);
   bad.initial_savanna->proof_of_possession = other_pop;
   BOOST_CHECK_THROW(bad.validate(), fc::exception);
   bad.initial_savanna->finalizer_key = other_pub;
   BOOST_REQUIRE_NO_THROW(bad.validate());
   BOOST_CHECK(bad.compute_chain_id() != genesis.compute_chain_id());
   bad = genesis;
   bad.initial_savanna->finalizer_key = {};
   BOOST_CHECK_THROW(bad.validate(), fc::exception);
   bad.initial_savanna->proof_of_possession = {};
   BOOST_CHECK_THROW(bad.validate(), fc::exception);
}

BOOST_AUTO_TEST_CASE(single_node_starts_native_and_finalizes_without_activation_transactions) {
   fc::temp_directory dir;
   auto cfg = base_tester::default_config(dir).first;
   auto genesis = native_genesis();
   tester chain(cfg, genesis);
   BOOST_CHECK_EQUAL(chain.head().block_num(), 1u); // The existing log format numbers the genesis anchor as 1.
   assert_native_head(chain);
   const auto& ps = chain.control->db().get<protocol_state_object>();
   BOOST_CHECK_EQUAL(ps.activated_protocol_features.size(), genesis.initial_savanna->protocol_features.size());
   BOOST_CHECK(chain.control->get_protocol_feature_manager().is_builtin_activated(builtin_protocol_feature_t::savanna, 1));
   enable_local_finalizer(chain);
   auto first = chain.produce_block();
   BOOST_CHECK(first->is_proper_svnn_block());
   chain.produce_blocks(10);
   BOOST_CHECK_GT(chain.last_irreversible_block_num(), 1u);
   assert_native_head(chain);
}

BOOST_AUTO_TEST_CASE(disabled_required_feature_cannot_silently_downgrade_genesis) {
   fc::temp_directory dir;
   auto pfs = make_protocol_feature_set({{builtin_protocol_feature_t::savanna,
                                         protocol_feature_subjective_restrictions{time_point{}, true, false}}});
   BOOST_CHECK_THROW(tester(base_tester::default_config(dir).first, std::move(pfs), native_genesis()), fc::exception);
}

BOOST_AUTO_TEST_CASE(no_finalizer_key_does_not_fake_finality) {
   fc::temp_directory dir;
   tester chain(base_tester::default_config(dir).first, native_genesis());
   chain.produce_blocks(6);
   BOOST_CHECK_EQUAL(chain.last_irreversible_block_num(), 1u);
   enable_local_finalizer(chain);
   chain.produce_blocks(10);
   BOOST_CHECK_GT(chain.last_irreversible_block_num(), 1u);
}

BOOST_AUTO_TEST_CASE(observer_validates_native_blocks_and_rejects_bad_signature) {
   fc::temp_directory producer_dir, observer_dir;
   const auto genesis = native_genesis();
   tester producer(base_tester::default_config(producer_dir).first, genesis);
   tester observer(base_tester::default_config(observer_dir).first, genesis);
   enable_local_finalizer(producer);
   const auto first = producer.produce_block();
   auto tampered = first->clone();
   tampered->producer_signature = {};
   BOOST_CHECK_THROW(observer.push_block(signed_block::create_signed_block(std::move(tampered))), fc::exception);
   observer.push_block(first);
   for (unsigned i = 0; i < 10; ++i)
      observer.push_block(producer.produce_block());
   BOOST_CHECK(observer.head().id() == producer.head().id());
   BOOST_CHECK_GT(observer.last_irreversible_block_num(), 1u);
}

BOOST_AUTO_TEST_CASE(native_genesis_allows_normal_policy_growth) {
   fc::temp_directory dir;
   tester chain(base_tester::default_config(dir).first, native_genesis());
   chain.set_bios_contract();
   chain.create_accounts({"alice"_n});
   const std::vector<account_name> members{"eosio"_n, "alice"_n};
   chain.set_producers(members);
   // This helper installs both local keys once, then submits the policy action.
   // It simulates two signers, not physical-node fault tolerance.
   chain.set_finalizers(members);
   for (unsigned i = 0; i < 60; ++i) {
      chain.produce_block();
      BOOST_CHECK(chain.control->get_rotation_context_v1() == uncached_rotation_context(chain));
   }
   const auto finalizers = chain.control->head_active_finalizer_policy();
   BOOST_REQUIRE(finalizers);
   BOOST_REQUIRE_EQUAL(finalizers->finalizers.size(), 2u);
   BOOST_CHECK_EQUAL(finalizers->threshold, 2u);
   BOOST_CHECK_GT(finalizers->generation, 1u);
   BOOST_CHECK_EQUAL(chain.control->head_active_producers().producers.size(), 2u);
   const auto before = chain.last_irreversible_block_num();
   chain.produce_blocks(6);
   BOOST_CHECK_GT(chain.last_irreversible_block_num(), before);
}

BOOST_AUTO_TEST_CASE(native_block_log_replays_from_genesis) {
   fc::temp_directory source_dir, replay_dir;
   auto cfg = base_tester::default_config(source_dir).first;
   const auto genesis = native_genesis();
   tester chain(cfg, genesis);
   enable_local_finalizer(chain);
   chain.produce_blocks(15);
   const auto lib = chain.last_irreversible_block_num();
   const auto lib_id = chain.last_irreversible_block_id();
   chain.close();
   auto extracted = block_log::extract_genesis_state(cfg.blocks_dir);
   BOOST_REQUIRE(extracted);
   BOOST_CHECK(*extracted == genesis);
   auto replay_cfg = base_tester::default_config(replay_dir).first;
   std::filesystem::create_directories(replay_cfg.blocks_dir);
   for (const char* filename : {"blocks.log", "blocks.index"})
      std::filesystem::copy_file(cfg.blocks_dir / filename, replay_cfg.blocks_dir / filename);
   tester replay(replay_cfg, *extracted);
   assert_native_head(replay);
   BOOST_CHECK_EQUAL(replay.head().block_num(), lib);
   BOOST_CHECK(replay.head().id() == lib_id);
   BOOST_CHECK_EQUAL(fc::raw::unpack<rotation_context_v1>(replay.control->get_rotation_context_v1()).block_num, lib);
   enable_local_finalizer(replay);
   replay.produce_blocks(6);
   BOOST_CHECK_GT(replay.last_irreversible_block_num(), lib);
}

BOOST_AUTO_TEST_CASE(native_snapshot_and_existing_state_restart) {
   fc::temp_directory source_dir, restored_dir;
   const auto genesis = native_genesis();
   auto cfg = base_tester::default_config(source_dir).first;
   tester chain(cfg, genesis);
   enable_local_finalizer(chain);
   chain.produce_blocks(12);
   chain.control->abort_block();
   const auto head_id = chain.head().id();
   const auto context_bytes = chain.control->get_rotation_context_v1_cached();
   auto writer = buffered_snapshot_suite::get_writer();
   chain.control->write_snapshot(writer);
   auto snapshot = buffered_snapshot_suite::finalize(writer);
   auto reader = buffered_snapshot_suite::get_reader(snapshot);
   auto restored_cfg = base_tester::default_config(restored_dir).first;
   tester restored(restored_cfg, genesis, call_startup_t::no);
   restored.close();
   restored.init(restored_cfg, reader);
   assert_native_head(restored);
   BOOST_CHECK(restored.head().id() == head_id);
   BOOST_CHECK(*context_bytes == restored.control->get_rotation_context_v1());
   BOOST_CHECK(context_bytes != restored.control->get_rotation_context_v1_cached());
   enable_local_finalizer(restored);
   const auto before = restored.last_irreversible_block_num();
   restored.produce_blocks(6);
   BOOST_CHECK_GT(restored.last_irreversible_block_num(), before);
   chain.close();
   chain.open();
   assert_native_head(chain);
   BOOST_CHECK(chain.head().id() == head_id);
   BOOST_CHECK(*context_bytes == chain.control->get_rotation_context_v1());
   enable_local_finalizer(chain);
   chain.produce_blocks(6);
}

BOOST_AUTO_TEST_CASE(legacy_genesis_logs_remain_readable) {
   auto reset_version = fc::make_scoped_exit([] { block_log::set_initial_version(3); });
   for (uint32_t version : {1u, 2u, 3u}) {
      block_log::set_initial_version(version);
      fc::temp_directory dir;
      auto [cfg, genesis] = base_tester::default_config(dir);
      tester chain(cfg, genesis);
      chain.produce_blocks(4);
      chain.close();
      auto extracted = block_log::extract_genesis_state(cfg.blocks_dir);
      BOOST_REQUIRE(extracted);
      BOOST_CHECK(*extracted == genesis);
      BOOST_CHECK(extracted->compute_chain_id() == genesis.compute_chain_id());
      chain.open();
      BOOST_CHECK(!chain.head().header().is_proper_svnn_block());
   }
}

BOOST_AUTO_TEST_CASE(legacy_v2_binary_snapshot_still_restores) {
   const auto snapshot = buffered_snapshot_suite::load_from_file("snap_v2");
   auto reader = buffered_snapshot_suite::get_reader(snapshot);
   const auto info = snapshot_info(*reader);
   fc::temp_directory dir;
   const auto cfg = base_tester::default_config(dir).first;
   tester chain(cfg, base_tester::default_genesis(), call_startup_t::no);
   chain.close();
   chain.init(cfg, buffered_snapshot_suite::get_reader(snapshot));
   BOOST_CHECK(!chain.head().header().is_proper_svnn_block());
   BOOST_CHECK(info.get_object()["chain_id"].as<chain_id_type>() == chain.control->get_chain_id());
   chain.produce_blocks(2);
}

BOOST_AUTO_TEST_CASE(native_pruned_log_preserves_genesis_manifest) {
   fc::temp_directory dir;
   auto cfg = base_tester::default_config(dir).first;
   cfg.blog = prune_blocklog_config{.prune_blocks = 5, .prune_threshold = 1, .vacuum_on_close = std::nullopt};
   const auto genesis = native_genesis();
   tester chain(cfg, genesis);
   enable_local_finalizer(chain);
   chain.produce_blocks(20);
   const auto id = chain.head().id();
   chain.close();
   auto extracted = block_log::extract_genesis_state(cfg.blocks_dir);
   BOOST_REQUIRE(extracted);
   BOOST_CHECK(*extracted == genesis);
   chain.open();
   BOOST_CHECK(chain.head().id() == id);
   enable_local_finalizer(chain);
   chain.produce_blocks(5);
}

BOOST_AUTO_TEST_CASE(native_log_vacuum_preserves_chain_context) {
   // Cover both keeping block 1 (v4 manifest) and dropping it (chain ID only).
   for (uint32_t retained : {100u, 5u}) {
      fc::temp_directory dir;
      auto cfg = base_tester::default_config(dir).first;
      cfg.blog = prune_blocklog_config{.prune_blocks = retained, .prune_threshold = 1,
                                      .vacuum_on_close = std::numeric_limits<size_t>::max()};
      const auto genesis = native_genesis();
      tester chain(cfg, genesis);
      enable_local_finalizer(chain);
      chain.produce_blocks(20);
      const auto id = chain.head().id();
      chain.close();
      BOOST_CHECK(!block_log::is_pruned_log(cfg.blocks_dir));
      const auto chain_id = block_log::extract_chain_id(cfg.blocks_dir);
      BOOST_REQUIRE(chain_id);
      BOOST_CHECK(*chain_id == genesis.compute_chain_id());
      const auto extracted = block_log::extract_genesis_state(cfg.blocks_dir);
      BOOST_CHECK_EQUAL(bool(extracted), retained == 100u);
      if (extracted)
         BOOST_CHECK(*extracted == genesis);
      chain.open();
      BOOST_CHECK(chain.head().id() == id);
      enable_local_finalizer(chain);
      chain.produce_blocks(5);
   }
}

BOOST_AUTO_TEST_SUITE_END()
