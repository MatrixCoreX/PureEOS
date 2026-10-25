#include <eosio/chain/parallel_pure_hash.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include "parallel_test_runtime.hpp"
#include <eosio/vm/exceptions.hpp>
#include <fc/crypto/sha1.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr name hash_account = "test"_n, hash_child = "bob"_n;
constexpr uint32_t hash_chunk = config::hashing_checktime_block_size;
constexpr std::array<const char*, 4> hash_names{"sha1", "sha256", "sha512", "ripemd160"};
constexpr std::array<uint32_t, 4> hash_sizes{20, 32, 64, 20};
std::string hc32(uint32_t n) { return "(i32.const " + std::to_string(n) + ")"; }
template<class T> void hput(std::vector<char>& data, size_t offset, T value) {
   std::memcpy(data.data() + offset, &value, sizeof(value));
}
uint32_t hget(const std::vector<char>& data, size_t offset) {
   uint32_t result; std::memcpy(&result, data.data() + offset, 4); return result;
}
template<class Digest> std::vector<char> digest_bytes(std::span<const char> data) {
   const auto result = Digest::hash(data.data(), data.size());
   return {result.data(), result.data() + result.data_size()};
}
std::vector<char> expected_hash(unsigned family, std::span<const char> data) {
   switch (family) {
      case 0: return digest_bytes<fc::sha1>(data);
      case 1: return digest_bytes<fc::sha256>(data);
      case 2: return digest_bytes<fc::sha512>(data);
      default: return digest_bytes<fc::ripemd160>(data);
   }
}
std::vector<char> hash_wasm(const std::string& text) {
   const auto bytes = wast_to_wasm(text); return {bytes.begin(), bytes.end()};
}
// One immutable program exercises correctly typed calls to all EIGHT hosts.
// Header: family, assert?, input offset, length, digest offset, nonce, route,
// packed CF child length. Route 1 = sync, 2 = CF, 3 = both. sync_call does not
// recurse. Ordinary and CF apply share the original action-data ABI.
std::vector<char> hash_program(bool write_first = false) {
   std::string imports, dispatch;
   for (unsigned family = 0; family < hash_names.size(); ++family) {
      const auto name = std::string(hash_names[family]);
      imports += "(import \"env\" \"" + name + "\" (func $" + name + " (param i32 i32 i32)))";
      imports += "(import \"env\" \"assert_" + name + "\" (func $assert_" + name + " (param i32 i32 i32)))";
      const std::string args = "(i32.load (i32.const 8))(i32.load (i32.const 12))(i32.load (i32.const 16))";
      dispatch += "(if (i32.eq (i32.load (i32.const 0)) " + hc32(family) + ") (then "
         "(if (i32.eq (i32.load (i32.const 4)) (i32.const 2))"
         "(then (call $" + name + args + ")(call $assert_" + name + args + "))"
         "(else (if (i32.load (i32.const 4)) (then (call $assert_" + name + args + "))"
         "(else (call $" + name + args + ")))))(return " + hc32(hash_sizes[family]) + ")))";
   }
   std::string prefix;
   if (write_first) {
      imports += R"((import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32))))";
      prefix = "(drop (call $store (get_local $receiver) (i64.const " + std::to_string("hashrows"_n.to_uint64_t()) +
         ") (get_local $receiver) (i64.extend_u/i32 (i32.load (i32.const 20))) (i32.const 1024) (i32.const 8)))";
   }
   return hash_wasm("(module " + imports + R"(
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "set_action_return_value" (func $out (param i32 i32)))
      (import "env" "get_call_data" (func $get (param i32 i32) (result i32)))
      (import "env" "set_call_return_value" (func $set (param i32 i32)))
      (import "env" "call" (func $call (param i64 i64 i32 i32) (result i64)))
      (import "env" "get_call_return_value" (func $returned (param i32 i32) (result i32)))
      (import "env" "send_context_free_inline" (func $cfi (param i32 i32)))
      (import "env" "eosio_assert_code" (func $check (param i32 i64)))
      (import "env" "memcmp" (func $compare (param i32 i32 i32) (result i32)))
      (memory 1 1)
      (func $digest (result i32) )" + dispatch + R"((unreachable))
      (func (export "apply") (param $receiver i64) (param i64) (param $action i64)
         (local $size i32) (local $length i32)
         (set_local $length (call $read (i32.const 0) (i32.const 65536))) )" + prefix + R"(
         (set_local $size (call $digest))
         )" + "(if (i64.eq (get_local $action) (i64.const " + std::to_string("pure"_n.to_uint64_t()) + R"()) (then
            (call $check (i32.eqz (call $compare (i32.load (i32.const 16)) (i32.const 512) (get_local $size))) (i64.const 903))
            (return)))
         (if (i32.and (i32.load (i32.const 24)) (i32.const 1)) (then
            (call $check (i64.eq (call $call (get_local $receiver) (i64.const 0)
               (i32.const 0) (get_local $length)) (i64.extend_u/i32 (get_local $size))) (i64.const 901))
            (call $check (i32.eq (call $returned (i32.const 48000) (get_local $size))
               (get_local $size)) (i64.const 902))))
         (if (i32.and (i32.load (i32.const 24)) (i32.const 2)) (then
            (call $cfi (i32.const 24000) (i32.load (i32.const 28)))))
         (call $out (i32.load (i32.const 16)) (get_local $size)))
      (func (export "sync_call") (param i64 i64 i32) (result i64) (local $size i32)
         (drop (call $get (i32.const 0) (i32.const 65536)))
         (set_local $size (call $digest))
         (call $set (i32.load (i32.const 16)) (get_local $size)) (i64.const 0))))");
}
std::vector<char> hash_start_program(unsigned family) {
   const std::string name = hash_names[family];
   return hash_wasm("(module (import \"env\" \"" + name + "\" (func $hash (param i32 i32 i32)))"
      "(import \"env\" \"assert_" + name + "\" (func $assert (param i32 i32 i32)))"
      R"((import "env" "set_action_return_value" (func $out (param i32 i32)))
      (memory 1 1)(data (i32.const 1024) "abc")
      (func $start (call $hash (i32.const 1024)(i32.const 3)(i32.const 257))) (start $start)
      (func (export "apply") (param i64 i64 i64)
         (call $assert (i32.const 1024)(i32.const 3)(i32.const 257))
         (call $out (i32.const 257) )" + hc32(hash_sizes[family]) + ") (i32.store (i32.const 257)(i32.const 0))))");
}
isolated_action_input hash_input(unsigned family, uint32_t length = 65, uint32_t source = 1024,
                                 uint32_t output = 256, bool verify = false) {
   isolated_action_input input;
   input.receiver = input.account = hash_account; input.action = "run"_n;
   input.authorizations = {{hash_account, "active"_n}};
   // Invalid pointers are ABI negative cases; do not turn them into enormous
   // native allocations. Unwritten linear memory remains originally zeroed.
   input.data.resize(32);
   if (source <= 65536 && length <= 65536 - source && length) {
      input.data.resize(std::max<size_t>(32, source + length));
      for (uint32_t i = 0; i < length; ++i) input.data[source + i] = char((i * 37u + 19u) & 255u);
   }
   std::fill_n(input.data.begin(), 32, char{0});
   hput(input.data, 0, uint32_t{family}); hput(input.data, 4, uint32_t{verify});
   hput(input.data, 8, source); hput(input.data, 12, length); hput(input.data, 16, output);
   return input;
}
std::vector<char> hash_expected(const isolated_action_input& input) {
   std::vector<char> memory(65536); std::copy(input.data.begin(), input.data.end(), memory.begin());
   return expected_hash(hget(input.data, 0), std::span<const char>(memory).subspan(hget(input.data, 8), hget(input.data, 12)));
}
isolated_action_input hash_assert_input(unsigned family, bool correct = true, uint32_t output = 257) {
   auto input = hash_input(family, 65, 1024, output, true);
   auto expected = hash_expected(input); if (!correct) expected[0] ^= 1;
   input.data.resize(std::max(input.data.size(), size_t(output) + expected.size()));
   std::copy(expected.begin(), expected.end(), input.data.begin() + output); return input;
}
std::vector<isolated_action_input> hash_good_inputs(unsigned family) {
   std::vector<isolated_action_input> result;
   for (uint32_t length : {0u, 1u, 63u, 64u, 65u, 127u, 128u, 129u,
                          hash_chunk - 1, hash_chunk, hash_chunk + 1, 2 * hash_chunk + 1})
      result.push_back(hash_input(family, length, 1025, 257));
   result.push_back(hash_input(family, 0, 0));
   result.push_back(hash_input(family, 32, 0));
   result.push_back(hash_input(family, 0, 65536));
   result.push_back(hash_input(family, 65, 1024, 65536 - hash_sizes[family]));
   for (uint32_t output : {1024u, 1025u, 1023u}) result.push_back(hash_input(family, 65, 1024, output));
   result.push_back(hash_assert_input(family, true, 256));
   result.push_back(hash_assert_input(family));
   return result;
}
std::vector<isolated_action_input> hash_bad_inputs(unsigned family) {
   std::vector<isolated_action_input> result;
   for (bool verify : {false, true}) {
      result.push_back(hash_input(family, 0, 1024, 0, verify));
      result.push_back(hash_input(family, 1, 1024, 65537 - hash_sizes[family], verify));
      result.push_back(hash_input(family, 1, 1024, UINT32_MAX, verify));
      result.push_back(hash_input(family, 0, 65537, 256, verify));
      result.push_back(hash_input(family, 2, 65535, 256, verify));
      result.push_back(hash_input(family, UINT32_MAX, 1024, 256, verify));
      result.push_back(hash_input(family, 0, UINT32_MAX, 256, verify));
   }
   result.push_back(hash_assert_input(family, false));
   result.push_back(hash_input(family, 65, 1024, 1025, true)); // unaligned overlapping mismatched digest
   return result;
}
struct hash_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   const std::vector<char> code = hash_program();
   hash_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const std::array accounts{hash_account, hash_child};
      for (name account : accounts) db.create<account_object>([&](auto& a) { a.name = account; });
      base = frozen_primary_state::capture_queries(view, {}, {}, {}, {}, accounts);
   }
};
template<class Digest> void hash_checkpoint_cases() {
   for (uint32_t length : {0u, 1u, hash_chunk - 1, hash_chunk, hash_chunk + 1, 2 * hash_chunk, 3 * hash_chunk + 1}) {
      const std::string data(length, '\xa5');
      const size_t checks = 2 + (length ? (length - 1) / hash_chunk : 0);
      size_t observed = 0;
      const auto result = isolated_hash_with_checktime<Digest>(data, [&] { ++observed; });
      BOOST_CHECK(result == Digest::hash(data.data(), data.size()));
      BOOST_CHECK_EQUAL(observed, checks);
      for (size_t cancel_at = 1; cancel_at <= checks; ++cancel_at) {
         observed = 0; bool published = false;
         BOOST_CHECK_THROW(([&] {
            (void)isolated_hash_with_checktime<Digest>(data, [&] {
               if (++observed == cancel_at) throw isolated_execution_interrupted(isolated_interrupt_reason::cancelled);
            }); published = true;
         }()), isolated_execution_interrupted);
         BOOST_CHECK_EQUAL(observed, cancel_at); BOOST_CHECK(!published);
      }
   }
}
isolated_action_input hash_graph_input(unsigned family, unsigned route) {
   auto root = hash_input(family);
   hput(root.data, 24, uint32_t{route});
   if (route & 2) {
      auto child = hash_assert_input(family); child.receiver = child.account = hash_child;
      const auto expected = hash_expected(child);
      std::copy(expected.begin(), expected.end(), child.data.begin() + 512);
      hput(child.data, 4, uint32_t{2}); // hash AND assert, checked against separately owned expected bytes
      const auto packed = fc::raw::pack(action{{}, hash_child, "pure"_n, child.data});
      root.data.resize(24000 + packed.size());
      std::copy(packed.begin(), packed.end(), root.data.begin() + 24000);
      hput(root.data, 28, uint32_t(packed.size()));
   }
   return root;
}
transaction_trace_ptr hash_push(eosio::testing::tester& chain, const isolated_action_input& input, uint32_t nonce) {
   auto bytes = input.data; hput(bytes, 20, nonce);
   signed_transaction trx;
   trx.actions.push_back(action{{{hash_account, "active"_n}}, hash_account, input.action, bytes});
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key(hash_account, "active"), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}

struct hash_live_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
struct hash_live_corpus {
   genesis_state genesis = eosio::testing::base_tester::default_genesis();
   std::vector<char> code = hash_program();
   std::vector<std::vector<char>> blocks;
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool production = false) const {
      auto cfg = eosio::testing::base_tester::default_config(dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg); cfg.contracts_console = true;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      cfg.parallel_code_allowlist = {fc::sha256::hash(code.data(), code.size())}; return cfg;
   }
   hash_live_corpus() {
      auto [key, pub, proof] = eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key = pub;
      genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory dir; hash_live_chain source(configuration(dir, 0), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call, builtin_protocol_feature_t::get_sender_fix});
      seal(); seal(); source.create_accounts({hash_account, hash_child}); seal();
      for (name account : {hash_account, hash_child}) source.set_code(account, std::vector<uint8_t>(code.begin(), code.end()));
      seal();
   }
   void prefix(eosio::testing::tester& chain) const {
      for (const auto& block : blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block))); chain.control->abort_block();
      }
   }
};
std::vector<char> hash_trace(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE_MESSAGE(trace && !trace->except && !trace->failed_dtrx_trace,
      (trace && trace->except ? trace->except->to_detail_string() : "missing or failed hash transaction trace"));
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->receipt), fc::raw::pack(trace->account_ram_delta), fc::raw::pack(trace->error_code)};
   for (const auto& action : trace->action_traces) {
      BOOST_REQUIRE(!action.except && !action.error_code);
      fields.push_back(fc::raw::pack(action.action_ordinal)); fields.push_back(fc::raw::pack(action.creator_action_ordinal));
      fields.push_back(fc::raw::pack(action.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(action.receiver)); fields.push_back(fc::raw::pack(action.act));
      fields.push_back(fc::raw::pack(action.receipt)); fields.push_back(fc::raw::pack(action.context_free));
      fields.push_back(action.return_value); fields.push_back(fc::raw::pack(action.account_ram_deltas));
      fields.push_back(fc::raw::pack(action.console)); fields.push_back(fc::raw::pack(action.console_markers));
      for (const auto& call : action.call_traces) {
         BOOST_REQUIRE(!call.except && !call.error_code);
         fields.push_back(fc::raw::pack(call.call_ordinal)); fields.push_back(fc::raw::pack(call.sender_ordinal));
         fields.push_back(fc::raw::pack(call.receiver)); fields.push_back(fc::raw::pack(call.read_only));
         fields.push_back(call.data); fields.push_back(call.return_value); fields.push_back(fc::raw::pack(call.error_id));
         fields.push_back(fc::raw::pack(call.console)); fields.push_back(fc::raw::pack(call.console_markers));
      }
   }
   return fc::raw::pack(fields); // deliberately excludes local timing only
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_pure_hash_private_tests)

BOOST_AUTO_TEST_CASE(original_encoders_and_exact_chunk_cancellation_publish_no_partial_digest) {
   hash_checkpoint_cases<fc::sha1>(); hash_checkpoint_cases<fc::sha256>();
   hash_checkpoint_cases<fc::sha512>(); hash_checkpoint_cases<fc::ripemd160>();
}

BOOST_FIXTURE_TEST_CASE(eight_typed_hash_hosts_preserve_boundaries_aliases_and_failed_module_reuse, hash_fixture) {
   isolated_wasm_module module(code);
   for (unsigned family = 0; family < 4; ++family) BOOST_TEST_CONTEXT(hash_names[family]) {
      for (const auto& input : hash_good_inputs(family)) {
         const auto result = module.execute(base, input);
         BOOST_CHECK(result.return_value == hash_expected(input)); BOOST_CHECK(result.state.writes().empty());
         BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
      }
      for (const auto& bad : hash_bad_inputs(family)) {
         BOOST_CHECK_THROW(module.execute(base, bad), primary_state_unsupported);
         const auto good = hash_input(family);
         BOOST_CHECK(module.execute(base, good).return_value == hash_expected(good));
      }
   }
   auto stop = std::stop_source{}; stop.request_stop();
   BOOST_CHECK_THROW(module.execute(base, hash_input(0), stop.get_token()), isolated_execution_interrupted);
   isolated_cancel expired; expired.deadline = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(module.execute(base, hash_input(1), {}, {}, expired), isolated_execution_interrupted);
   BOOST_CHECK(module.execute(base, hash_input(2)).return_value == hash_expected(hash_input(2)));
}

BOOST_FIXTURE_TEST_CASE(hash_start_functions_reset_and_reuse_original_memory, hash_fixture) {
   for (unsigned family = 0; family < 4; ++family) {
      isolated_wasm_module module(hash_start_program(family));
      for (unsigned round = 0; round < 3; ++round)
         BOOST_CHECK(module.execute(base, hash_input(family)).return_value == expected_hash(family, std::string("abc")));
   }
}

BOOST_FIXTURE_TEST_CASE(hash_jobs_are_private_on_one_two_four_workers_and_survive_mismatch, hash_fixture) {
   const auto code_hash = fc::sha256::hash(code.data(), code.size());
   for (unsigned workers : {1u, 2u, 4u}) {
      isolated_worker_pool pool(workers);
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<ordered_attempt_batch::request> requests;
         std::vector<isolated_action_input> inputs;
         for (unsigned i = 0; i < 16; ++i) {
            inputs.push_back(i % 3 == 0 ? hash_assert_input(i % 4, false) : hash_input(i % 4, hash_chunk + 1));
            const std::array hashes{isolated_execution_fingerprint(code_hash, {}, inputs.back())};
            requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_transaction_fingerprint(hashes)});
         }
         ordered_attempt_batch batch(base, requests);
         std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < inputs.size(); ++i) jobs.emplace_back(batch.get_ticket(i), code, inputs[i], isolated_wasm_limits{});
         const auto results = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(results.size(), inputs.size());
         for (size_t i = 0; i < results.size(); ++i) {
            BOOST_CHECK(!results[i].request);
            if (i % 3 == 0) { BOOST_CHECK(!results[i].result); BOOST_CHECK(results[i].failure == isolated_worker_failure::unsupported); }
            else {
               BOOST_REQUIRE(results[i].result); BOOST_REQUIRE_EQUAL(results[i].result->actions.size(), 1u);
               BOOST_CHECK(results[i].result->state.writes().empty());
               BOOST_CHECK(results[i].result->actions[0].return_value == hash_expected(inputs[i]));
            }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(hash_hosts_work_in_cf_sync_and_mixed_without_ledger_capabilities, hash_fixture) {
   const std::vector<isolated_receiver_program> ordinary{{hash_account, code, {}}, {hash_child, code, {}}};
   const std::vector<isolated_sync_program> programs{{ordinary[0], true}, {ordinary[1], true}};
   isolated_action_tree_context tree; tree.context_free_data = isolated_context_free_data::capture({});
   isolated_sync_context sync; sync.get_sender_fix = true;
   isolated_action_tree_executor tree_executor; isolated_sync_executor sync_executor; isolated_mixed_tree_executor mixed;
   for (unsigned family = 0; family < 4; ++family) {
      const std::array roots{hash_graph_input(family, 2)};
      const auto cf = tree_executor.execute(base, roots, ordinary, tree);
      BOOST_REQUIRE(validate_isolated_action_tree_journal(cf, roots, ordinary, tree));
      BOOST_REQUIRE_EQUAL(cf.steps.size(), 2u); BOOST_CHECK(cf.groups[cf.steps[1].group_index].context_free);
      BOOST_CHECK(cf.steps[0].effects.return_value == hash_expected(roots[0]));
      // ORIGINAL set_action_return_value is NOT a CF host. The CF WASM checks
      // all digest bytes itself against its independent input, and returns none.
      BOOST_CHECK(cf.steps[1].effects.return_value.empty());
      const auto sync_root = hash_graph_input(family, 1);
      const auto called = sync_executor.execute(base, sync_root, programs, sync);
      BOOST_REQUIRE(validate_isolated_sync_journal(called, sync_root, programs, sync));
      BOOST_REQUIRE_EQUAL(called.calls.size(), 1u); BOOST_CHECK_EQUAL(called.calls[0].status, 0);
      BOOST_CHECK(called.calls[0].return_value == hash_expected(sync_root));
      const std::array mixed_roots{hash_graph_input(family, 3)};
      const isolated_mixed_tree_context context{tree, sync};
      const auto both = mixed.execute(base, mixed_roots, programs, context);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(both, mixed_roots, programs, context));
      BOOST_REQUIRE_EQUAL(both.steps.size(), 2u); BOOST_REQUIRE_EQUAL(both.steps[0].calls.size(), 1u);
      BOOST_CHECK(both.steps[0].calls[0].return_value == hash_expected(mixed_roots[0]));
      BOOST_CHECK(both.steps[1].effects.return_value.empty());
      BOOST_CHECK(both.state.writes().empty());
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_pure_hash_oracle_tests)

BOOST_AUTO_TEST_CASE(selected_hash_profile_configures_canonical_and_private_engines) {
   hash_live_corpus corpus;
   const bool jit = eosio::testing::base_tester::arguments_contains("--eos-vm-jit");
   for (uint16_t workers : {0, 1, 4}) for (bool production : {false, true}) {
      fc::temp_directory directory;
      const auto cfg = corpus.configuration(directory, workers, production);
      BOOST_CHECK(cfg.wasm_runtime == (jit ? wasm_interface::vm_type::eos_vm_jit : wasm_interface::vm_type::eos_vm));
      BOOST_CHECK(cfg.parallel_engine == (jit ? isolated_vm_engine::jit : isolated_vm_engine::interpreter));
      BOOST_CHECK_EQUAL(cfg.parallel_validation_threads, workers);
      BOOST_CHECK_EQUAL(cfg.parallel_production_enabled, production && workers != 0);
   }
}

BOOST_AUTO_TEST_CASE(original_module_start_hashing_matches_and_resets_after_apply_mutates_digest) {
   eosio::testing::tester original; original.create_accounts({hash_account});
   chainbase_transaction_state_view live(original.control->db());
   const auto base = frozen_primary_state::capture(live, {});
   uint32_t nonce = 0;
   for (unsigned family = 0; family < 4; ++family) {
      const auto code = hash_start_program(family);
      original.set_code(hash_account, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
      isolated_wasm_module module(code);
      for (unsigned round = 0; round < 3; ++round) {
         const auto input = hash_input(family); const auto attempt = module.execute(base, input);
         const auto trace = hash_push(original, input, ++nonce); BOOST_REQUIRE(trace && !trace->except);
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         BOOST_CHECK(trace->action_traces[0].return_value == attempt.return_value);
         BOOST_CHECK(attempt.return_value == expected_hash(family, std::string("abc")));
      }
   }
}

BOOST_AUTO_TEST_CASE(actual_original_wasm_agrees_on_all_hash_pointer_and_alias_cases) {
   eosio::testing::tester original; original.create_accounts({hash_account});
   const auto code = hash_program(); original.set_code(hash_account, std::vector<uint8_t>(code.begin(), code.end()));
   original.produce_block();
   chainbase_transaction_state_view live(original.control->db());
   const auto base = frozen_primary_state::capture(live, {});
   isolated_wasm_module module(code); uint32_t nonce = 0;
   for (unsigned family = 0; family < 4; ++family) BOOST_TEST_CONTEXT(hash_names[family]) {
      for (auto input : hash_good_inputs(family)) {
         hput(input.data, 20, ++nonce);
         const auto expected = hash_expected(input);
         const auto attempt = module.execute(base, input);
         const auto trace = hash_push(original, input, nonce);
         BOOST_REQUIRE(trace && !trace->except); BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         BOOST_CHECK(trace->action_traces[0].return_value == expected);
         BOOST_CHECK(attempt.return_value == trace->action_traces[0].return_value);
      }
      for (auto input : hash_bad_inputs(family)) {
         hput(input.data, 20, ++nonce);
         const auto before = original.control->calculate_integrity_hash();
         BOOST_CHECK_THROW(module.execute(base, input), primary_state_unsupported);
         BOOST_CHECK_THROW(hash_push(original, input, nonce), fc::exception);
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(late_digest_mismatch_undoes_original_and_private_write_prefix_and_allows_reuse) {
   eosio::testing::tester original; original.create_accounts({hash_account});
   const auto code = hash_program(true); original.set_code(hash_account, std::vector<uint8_t>(code.begin(), code.end()));
   original.produce_block();
   chainbase_transaction_state_view live(original.control->db());
   const table_key rows{hash_account, hash_account, "hashrows"_n}; const std::array tables{rows};
   isolated_wasm_module module(code);
   for (unsigned family = 0; family < 4; ++family) {
      auto bad = hash_assert_input(family, false); hput(bad.data, 20, uint32_t{family + 1});
      const auto base = frozen_primary_state::capture(live, tables);
      const auto before = original.control->calculate_integrity_hash();
      BOOST_CHECK_THROW(module.execute(base, bad), primary_state_unsupported);
      BOOST_CHECK_THROW(hash_push(original, bad, family + 1), crypto_api_exception);
      BOOST_CHECK(original.control->calculate_integrity_hash() == before);
      BOOST_CHECK(!live.read_primary({rows, primary_query_kind::find, family + 1}, 8).row);
      auto good = hash_assert_input(family); hput(good.data, 20, uint32_t{family + 1});
      const auto attempt = module.execute(base, good);
      BOOST_REQUIRE_EQUAL(attempt.state.writes().size(), 1u);
      const auto trace = hash_push(original, good, family + 1); BOOST_REQUIRE(trace && !trace->except);
      BOOST_CHECK(trace->action_traces[0].return_value == attempt.return_value);
      const auto row = live.read_primary({rows, primary_query_kind::find, family + 1}, 8);
      BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == attempt.state.primary_write(0).value);
   }
}

BOOST_AUTO_TEST_CASE(hash_results_are_actually_adopted_in_production_and_validation_with_nested_cf_sync) {
   hash_live_corpus corpus;
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory pd, sd, vd;
      hash_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      for (unsigned route : {0u, 1u, 2u, 3u}) BOOST_TEST_CONTEXT("hash workers=" << workers << " route=" << route) {
         producer.start(); std::vector<transaction_metadata_ptr> batch;
         for (unsigned family = 0; family < 4; ++family) {
            auto input = hash_graph_input(family, route);
            signed_transaction trx;
            trx.actions.push_back(action{{{hash_account, "active"_n}}, hash_account, input.action, input.data});
            producer.set_transaction_headers(trx);
            trx.sign(producer.get_private_key(hash_account, "active"), producer.control->get_chain_id());
            batch.push_back(transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input));
         }
         std::vector<std::vector<char>> expected, original, candidate;
         const auto collect = [&](auto& dest, const auto& event) {
            const auto trace = std::get<0>(event);
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; })) dest.push_back(hash_trace(trace));
         };
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) { collect(original, e); });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) { collect(candidate, e); });
         const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
            expected.push_back(hash_trace(trace));
            BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route & 2 ? 2u : 1u);
            BOOST_REQUIRE_EQUAL(trace->action_traces[0].call_traces.size(), route & 1 ? 1u : 0u);
            return true;
         }), batch.size());
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT("attempts=" << after.attempts - before.attempts << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
            << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
            << " succeeded=" << after.succeeded - before.succeeded
            << " vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts
            << " worker_other=" << after.worker_other_failures - before.worker_other_failures
            << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
            << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks) {
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, batch.size());
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
         }
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(original == expected); BOOST_CHECK(candidate == expected); BOOST_CHECK_EQUAL(original.size(), batch.size());
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, batch.size());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, 0u);
      }
   }
}

BOOST_AUTO_TEST_CASE(completed_private_batches_survive_delayed_ordered_adoption) {
   hash_live_corpus corpus;
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory pd, sd, vd;
      hash_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      uint32_t nonce = 10000;
      for (unsigned route : {0u, 1u, 2u, 3u}) for (size_t pause_at : {0u, 1u})
         BOOST_TEST_CONTEXT("completed phase workers=" << workers << " route=" << route << " pause_at=" << pause_at) {
         producer.start(); std::vector<transaction_metadata_ptr> batch;
         for (unsigned family = 0; family < 3; ++family) {
            auto input = hash_graph_input(family, route); hput(input.data, 20, nonce++);
            signed_transaction trx;
            trx.actions.push_back(action{{{hash_account, "active"_n}}, hash_account, input.action, input.data});
            producer.set_transaction_headers(trx);
            trx.sign(producer.get_private_key(hash_account, "active"), producer.control->get_chain_id());
            batch.push_back(transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input));
         }
         std::vector<std::vector<char>> produced, original, candidate;
         const auto collect = [&](auto& dest, const auto& event) {
            const auto trace = std::get<0>(event);
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; })) dest.push_back(hash_trace(trace));
         };
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) { collect(original, e); });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) { collect(candidate, e); });
         const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
         size_t callbacks = 0;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            // First prove that private preparation finished. The intentional
            // pause is OWNER work, after the bounded worker phase has joined.
            const auto ready = producer.control->get_parallel_production_stats();
            BOOST_TEST_CONTEXT("ready attempts=" << ready.attempts - before.attempts
               << " succeeded=" << ready.succeeded - before.succeeded
               << " preparation_deadlines=" << ready.preparation_deadlines - before.preparation_deadlines
               << " preparation_budget=" << ready.preparation_budget_fallbacks - before.preparation_budget_fallbacks
               << " preparation_other=" << ready.preparation_other_fallbacks - before.preparation_other_fallbacks
               << " worker_deadlines=" << ready.worker_deadlines - before.worker_deadlines
               << " vm_timeouts=" << ready.worker_vm_timeouts - before.worker_vm_timeouts
               << " worker_other=" << ready.worker_other_failures - before.worker_other_failures) {
               BOOST_REQUIRE_GE(ready.succeeded - before.succeeded, batch.size());
            }
            BOOST_REQUIRE_EQUAL(ready.preparation_deadlines - before.preparation_deadlines, 0u);
            BOOST_REQUIRE_EQUAL(ready.worker_deadlines - before.worker_deadlines, 0u);
            BOOST_REQUIRE_EQUAL(ready.worker_vm_timeouts - before.worker_vm_timeouts, 0u);
            if (callbacks == pause_at) std::this_thread::sleep_for(std::chrono::milliseconds(60));
            BOOST_CHECK(tx == batch.at(callbacks++));
            const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
            produced.push_back(hash_trace(trace));
            BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route & 2 ? 2u : 1u);
            BOOST_REQUIRE_EQUAL(trace->action_traces[0].call_traces.size(), route & 1 ? 1u : 0u);
            return true;
         }), batch.size());
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(callbacks, batch.size());
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, batch.size());
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(original == produced); BOOST_CHECK(candidate == produced); BOOST_CHECK_EQUAL(original.size(), batch.size());
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, batch.size());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, 0u);
      }
   }
}

BOOST_AUTO_TEST_CASE(delayed_adoption_keeps_interrupt_identity_generation_and_real_deadline_guards) {
   hash_live_corpus corpus;
   for (uint16_t workers : {1, 2, 4}) for (unsigned mode = 0; mode < 4; ++mode)
      BOOST_TEST_CONTEXT("delayed safety workers=" << workers << " mode=" << mode) {
      fc::temp_directory dir;
      hash_live_chain chain(corpus.configuration(dir, workers, true), corpus.genesis);
      corpus.prefix(chain); chain.start();
      const auto make = [&](unsigned family, uint32_t nonce) {
         auto input = hash_graph_input(family, 3); hput(input.data, 20, nonce);
         signed_transaction trx;
         trx.actions.push_back(action{{{hash_account, "active"_n}}, hash_account, input.action, input.data});
         chain.set_transaction_headers(trx);
         trx.sign(chain.get_private_key(hash_account, "active"), chain.control->get_chain_id());
         return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
            chain.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
      };
      const std::vector<transaction_metadata_ptr> batch{make(0, 20000), make(1, 20001)};
      const auto before = chain.control->get_parallel_production_stats();
      const auto state_before = chain.control->calculate_integrity_hash();
      const auto deadline = mode == 3 ? fc::time_point::now() + fc::milliseconds(500) : fc::time_point::maximum();
      size_t callbacks = 0;
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction(batch, deadline, [&](const auto& tx) {
         ++callbacks;
         const auto ready = chain.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT("ready attempts=" << ready.attempts - before.attempts
            << " succeeded=" << ready.succeeded - before.succeeded
            << " preparation_deadlines=" << ready.preparation_deadlines - before.preparation_deadlines
            << " preparation_budget=" << ready.preparation_budget_fallbacks - before.preparation_budget_fallbacks
            << " preparation_other=" << ready.preparation_other_fallbacks - before.preparation_other_fallbacks
            << " worker_deadlines=" << ready.worker_deadlines - before.worker_deadlines
            << " vm_timeouts=" << ready.worker_vm_timeouts - before.worker_vm_timeouts
            << " worker_other=" << ready.worker_other_failures - before.worker_other_failures) {
            BOOST_REQUIRE_GE(ready.succeeded - before.succeeded, batch.size());
         }
         BOOST_REQUIRE_EQUAL(ready.preparation_deadlines - before.preparation_deadlines, 0u);
         std::this_thread::sleep_for(std::chrono::milliseconds(mode == 3 ? 550 : 60));
         if (mode == 0) chain.control->interrupt_transaction(controller::interrupt_t::speculative_block_trx);
         if (mode == 1) { chain.control->abort_block(); chain.start(); }
         auto submitted = tx;
         if (mode == 2) {
            submitted = transaction_metadata::recover_keys(tx->packed_trx(), chain.control->get_chain_id(),
               fc::seconds(5), transaction_metadata::trx_type::input);
            BOOST_REQUIRE(submitted != tx); BOOST_REQUIRE(submitted->id() == tx->id());
         }
         const auto trace = chain.control->push_transaction(submitted, deadline, fc::seconds(1), 0, false, 0);
         BOOST_REQUIRE(trace);
         if (mode == 3) {
            BOOST_REQUIRE(trace->except); BOOST_CHECK_EQUAL(trace->except->code(), deadline_exception::code_value);
            BOOST_CHECK(!trace->receipt); BOOST_CHECK(chain.control->calculate_integrity_hash() == state_before);
            BOOST_CHECK(!chain.control->is_known_unexpired_transaction(tx->id()));
         } else {
            BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
            BOOST_REQUIRE(trace->receipt); BOOST_CHECK(chain.control->is_known_unexpired_transaction(tx->id()));
         }
         return false; // do not enter another queue item after the injected event
      }), 1u);
      BOOST_CHECK_EQUAL(callbacks, 1u);
      const auto after = chain.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, 0u);
      BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 1u);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(batch.back()->id()));
      chain.control->abort_block(); chain.start();
      const auto fresh = make(2, 20002);
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({fresh}, fc::time_point::maximum(), [&](const auto& tx) {
         BOOST_CHECK(tx == fresh);
         const auto trace = chain.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
         BOOST_REQUIRE(trace && !trace->except && trace->receipt); return true;
      }), 1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted - after.adopted, 1u);
   }
}

BOOST_AUTO_TEST_SUITE_END()
