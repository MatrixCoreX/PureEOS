#include <eosio/chain/parallel_pure_integer.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include "parallel_test_runtime.hpp"
#include <eosio/testing/bls_utils.hpp>
#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
using bits128 = unsigned __int128;
using integer_op = isolated_integer_operation;
constexpr name integer_account = "test"_n, integer_child = "bob"_n;
constexpr std::array<const char*, 9> integer_names{
   "__ashlti3", "__ashrti3", "__lshlti3", "__lshrti3", "__divti3", "__udivti3", "__multi3", "__modti3", "__umodti3"};
constexpr bits128 integer_top = bits128{1} << 127, integer_all = ~bits128{0};
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(bits128) == 16 && alignof(bits128) == alignof(__int128));

template<class T> void iput(std::vector<char>& data, size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
template<class T> T iget(const std::vector<char>& data, size_t offset) { T value; std::memcpy(&value, data.data() + offset, sizeof(value)); return value; }
std::string ic32(uint32_t value) { return "(i32.const " + std::to_string(value) + ")"; }
std::vector<char> integer_wasm(const std::string& text) { const auto code = wast_to_wasm(text); return {code.begin(), code.end()}; }
std::vector<char> integer_bytes(bits128 value) { const auto* p = reinterpret_cast<const char*>(&value); return {p, p + 16}; }

// Exact unbounded integer reference, independent of the private native helper.
// Avoid shifting negative signed magnitudes: arithmetic >> is floor division.
std::optional<bits128> integer_reference(unsigned operation, bits128 a, bits128 b, uint32_t shift) {
   using boost::multiprecision::cpp_int;
   const cpp_int modulus = cpp_int{1} << 128, top = modulus >> 1, mask64 = (cpp_int{1} << 64) - 1;
   const auto expand = [](bits128 value) -> cpp_int { return (cpp_int{uint64_t(value >> 64)} << 64) + uint64_t(value); };
   const cpp_int lhs = expand(a), rhs = expand(b);
   cpp_int result;
   if (operation < 4) {
      if (shift >= 128) return operation == 1 ? std::nullopt : std::optional<bits128>{0};
      const cpp_int scale = cpp_int{1} << shift;
      if (operation == 0 || operation == 2) result = lhs * scale;
      else if (operation == 3 || lhs < top) result = lhs / scale;
      else result = -((modulus - lhs + scale - 1) / scale);
   } else if (operation == 6) result = lhs * rhs;
   else {
      if (!b) return {};
      if (operation == 4 || operation == 7) {
         const cpp_int sl = lhs >= top ? lhs - modulus : lhs, sr = rhs >= top ? rhs - modulus : rhs;
         if (operation == 4) result = sl / sr; else result = sl % sr;
      } else if (operation == 5) result = lhs / rhs;
      else if (operation == 8) result = lhs % rhs;
      else return {};
   }
   result %= modulus; if (result < 0) result += modulus;
   const cpp_int low = result & mask64, high = result >> 64;
   return isolated_integer_words(low.convert_to<uint64_t>(), high.convert_to<uint64_t>());
}

// Header: opcode/out; 128-bit a,b; shift/route/nonce/packed child size.
// Root and sync write 16 bytes; CF checks independent bytes and returns void.
std::vector<char> integer_program(bool write_first = false) {
   std::string imports, dispatch, prefix;
   for (unsigned op = 0; op < integer_names.size(); ++op) {
      const std::string symbol = integer_names[op];
      imports += "(import \"env\" \"" + symbol + "\" (func $" + symbol +
         (op < 4 ? " (param i32 i64 i64 i32)))" : " (param i32 i64 i64 i64 i64)))");
      const std::string args = "(i32.load (i32.const 4))(i64.load (i32.const 8))(i64.load (i32.const 16))" +
         std::string(op < 4 ? "(i32.load (i32.const 40))" : "(i64.load (i32.const 24))(i64.load (i32.const 32))");
      dispatch += "(if (i32.eq (i32.load (i32.const 0)) " + ic32(op) + ") (then (call $" + symbol + args + ")(return)))";
   }
   if (write_first) {
      imports += R"((import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32))))";
      prefix = "(drop (call $store (get_local $receiver) (i64.const " + std::to_string("introws"_n.to_uint64_t()) +
         ") (get_local $receiver) (i64.extend_u/i32 (i32.load (i32.const 48))) (i32.const 8) (i32.const 16)))";
   }
   return integer_wasm("(module " + imports + R"(
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
      (func $compute )" + dispatch + R"((unreachable))
      (func (export "apply") (param $receiver i64) (param i64) (param $action i64) (local $length i32)
         (set_local $length (call $read (i32.const 0) (i32.const 65536))) )" + prefix + R"(
         (call $compute)
         )" + "(if (i64.eq (get_local $action) (i64.const " + std::to_string("pure"_n.to_uint64_t()) + R"()) (then
            (call $check (i32.eqz (call $compare (i32.load (i32.const 4)) (i32.const 128) (i32.const 16))) (i64.const 913))
            (return)))
         (if (i32.and (i32.load (i32.const 44)) (i32.const 1)) (then
            (call $check (i64.eq (call $call (get_local $receiver) (i64.const 0)
               (i32.const 0) (get_local $length)) (i64.const 16)) (i64.const 911))
            (call $check (i32.eq (call $returned (i32.const 48000) (i32.const 16)) (i32.const 16)) (i64.const 912))
            (call $check (i32.eqz (call $compare (i32.load (i32.const 4)) (i32.const 48000) (i32.const 16))) (i64.const 914))))
         (if (i32.and (i32.load (i32.const 44)) (i32.const 2)) (then
            (call $cfi (i32.const 1024) (i32.load (i32.const 52)))))
         (call $out (i32.load (i32.const 4)) (i32.const 16)))
      (func (export "sync_call") (param i64 i64 i32) (result i64)
         (drop (call $get (i32.const 0) (i32.const 65536))) (call $compute)
         (call $set (i32.load (i32.const 4)) (i32.const 16)) (i64.const 0))))");
}
isolated_action_input integer_input(unsigned operation, bits128 a = integer_top + 123, bits128 b = integer_all - 6,
                                    uint32_t shift = 65, uint32_t output = 64) {
   isolated_action_input input;
   input.receiver = input.account = integer_account; input.action = "run"_n;
   input.authorizations = {{integer_account, "active"_n}}; input.data.resize(256);
   iput(input.data, 0, uint32_t{operation}); iput(input.data, 4, output);
   iput(input.data, 8, a); iput(input.data, 24, b); iput(input.data, 40, shift);
   return input;
}
std::optional<bits128> integer_expected(const isolated_action_input& input) {
   return integer_reference(iget<uint32_t>(input.data, 0), iget<bits128>(input.data, 8),
      iget<bits128>(input.data, 24), iget<uint32_t>(input.data, 40));
}
std::vector<isolated_action_input> integer_good_inputs(unsigned op) {
   const std::array<bits128, 8> values{0, 1, UINT64_MAX, bits128{1} << 64, integer_top - 1, integer_top, integer_all - 6, integer_all};
   std::vector<isolated_action_input> inputs;
   if (op < 4) {
      for (auto a : values) for (uint32_t shift : {0u, 1u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, UINT32_MAX})
         if (op != 1 || shift < 128) inputs.push_back(integer_input(op, a, 0, shift));
   } else {
      for (auto a : values) for (auto b : values) if (op == 6 || b) inputs.push_back(integer_input(op, a, b));
   }
   for (uint32_t output : {65u, 8u, 9u, 16u, 17u, 24u, 65519u, 65520u}) inputs.push_back(integer_input(op, integer_top + 123, integer_all - 6, 65, output));
   return inputs;
}
std::vector<isolated_action_input> integer_bad_inputs(unsigned op) {
   std::vector<isolated_action_input> inputs;
   for (uint32_t output : {0u, 65521u, UINT32_MAX - 8, UINT32_MAX}) inputs.push_back(integer_input(op, 7, 2, 1, output));
   if (op >= 4 && op != 6) inputs.push_back(integer_input(op, 7, 0));
   return inputs;
}
isolated_action_input integer_graph_input(unsigned operation, unsigned route) {
   auto root = integer_input(operation); iput(root.data, 44, uint32_t{route});
   if (route & 2) {
      auto child = integer_input(operation); child.receiver = child.account = integer_child;
      const auto expected = integer_expected(child); BOOST_REQUIRE(expected);
      iput(child.data, 128, *expected);
      const auto packed = fc::raw::pack(action{{}, integer_child, "pure"_n, child.data});
      root.data.resize(1024 + packed.size()); std::copy(packed.begin(), packed.end(), root.data.begin() + 1024);
      iput(root.data, 52, uint32_t(packed.size()));
   }
   return root;
}

struct integer_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   const std::vector<char> code = integer_program();
   integer_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const std::array accounts{integer_account, integer_child};
      for (name account : accounts) db.create<account_object>([&](auto& a) { a.name = account; });
      base = frozen_primary_state::capture_queries(view, {}, {}, {}, {}, accounts);
   }
};
transaction_trace_ptr integer_push(eosio::testing::tester& chain, const isolated_action_input& input, uint32_t nonce) {
   auto bytes = input.data; iput(bytes, 48, nonce);
   signed_transaction trx;
   trx.actions.push_back(action{{{integer_account, "active"_n}}, integer_account, input.action, bytes});
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key(integer_account, "active"), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}

struct integer_live_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
struct integer_live_corpus {
   genesis_state genesis = eosio::testing::base_tester::default_genesis();
   std::vector<char> code = integer_program();
   std::vector<std::vector<char>> blocks;
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool production = false) const {
      auto cfg = eosio::testing::base_tester::default_config(dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg); cfg.contracts_console = true;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      cfg.parallel_code_allowlist = {fc::sha256::hash(code.data(), code.size())}; return cfg;
   }
   integer_live_corpus() {
      auto [key, pub, proof] = eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key = pub; genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory dir; integer_live_chain source(configuration(dir, 0), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call, builtin_protocol_feature_t::get_sender_fix});
      seal(); seal(); source.create_accounts({integer_account, integer_child}); seal();
      for (name account : {integer_account, integer_child}) source.set_code(account, std::vector<uint8_t>(code.begin(), code.end()));
      seal();
   }
   void prefix(eosio::testing::tester& chain) const {
      for (const auto& block : blocks) { chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block))); chain.control->abort_block(); }
   }
};
std::vector<char> integer_trace(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE_MESSAGE(trace && !trace->except && !trace->failed_dtrx_trace,
      (trace && trace->except ? trace->except->to_detail_string() : "missing integer trace"));
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->net_usage), fc::raw::pack(trace->receipt),
      fc::raw::pack(trace->account_ram_delta), fc::raw::pack(trace->error_code)};
   for (const auto& action : trace->action_traces) {
      BOOST_REQUIRE(!action.except && !action.error_code);
      fields.push_back(fc::raw::pack(action.action_ordinal)); fields.push_back(fc::raw::pack(action.creator_action_ordinal));
      fields.push_back(fc::raw::pack(action.closest_unnotified_ancestor_action_ordinal)); fields.push_back(fc::raw::pack(action.receiver));
      fields.push_back(fc::raw::pack(action.act)); fields.push_back(fc::raw::pack(action.receipt)); fields.push_back(fc::raw::pack(action.context_free));
      fields.push_back(action.return_value); fields.push_back(fc::raw::pack(action.account_ram_deltas));
      fields.push_back(fc::raw::pack(action.console)); fields.push_back(fc::raw::pack(action.console_markers));
      for (const auto& call : action.call_traces) {
         BOOST_REQUIRE(!call.except && !call.error_code);
         fields.push_back(fc::raw::pack(call.call_ordinal)); fields.push_back(fc::raw::pack(call.sender_ordinal));
         fields.push_back(fc::raw::pack(call.receiver)); fields.push_back(fc::raw::pack(call.read_only)); fields.push_back(call.data);
         fields.push_back(call.return_value); fields.push_back(fc::raw::pack(call.error_id)); fields.push_back(fc::raw::pack(call.console));
         fields.push_back(fc::raw::pack(call.console_markers));
      }
   }
   return fc::raw::pack(fields); // exclude only local timing, not receipts/resources/ordinals
}
} // namespace

namespace {
std::vector<char> integer_start_program(unsigned op) {
   const auto input = integer_input(op);
   const auto word = [&](size_t offset) { return "(i64.const " + std::to_string(std::bit_cast<int64_t>(iget<uint64_t>(input.data, offset))) + ")"; };
   return integer_wasm("(module (import \"env\" \"" + std::string(integer_names[op]) + "\" (func $calculate " +
      (op < 4 ? "(param i32 i64 i64 i32)))" : "(param i32 i64 i64 i64 i64)))") +
      R"((import "env" "set_action_return_value" (func $out (param i32 i32))) (memory 1 1)
      (func $start (call $calculate (i32.const 257) )" + word(8) + word(16) +
      (op < 4 ? ic32(iget<uint32_t>(input.data, 40)) : word(24) + word(32)) + R"()) (start $start)
      (func (export "apply") (param i64 i64 i64)
         (call $out (i32.const 257) (i32.const 16)) (i64.store (i32.const 257) (i64.const 0)))))");
}
}

BOOST_AUTO_TEST_SUITE(parallel_integer_private_tests)

BOOST_AUTO_TEST_CASE(pure_integer_arithmetic_matches_unbounded_reference_without_native_undefined_behavior) {
   for (unsigned op = 0; op < 9; ++op) BOOST_TEST_CONTEXT(integer_names[op]) {
      for (const auto& input : integer_good_inputs(op)) {
         const auto expected = integer_expected(input); BOOST_REQUIRE(expected);
         const auto actual = isolated_integer128(static_cast<integer_op>(op), iget<bits128>(input.data, 8),
            iget<bits128>(input.data, 24), iget<uint32_t>(input.data, 40));
         BOOST_REQUIRE(actual); BOOST_CHECK(*actual == *expected);
      }
   }
   for (auto op : {integer_op::signed_divide, integer_op::unsigned_divide, integer_op::signed_modulo, integer_op::unsigned_modulo})
      BOOST_CHECK(!isolated_integer128(op, integer_top, 0));
   BOOST_CHECK(isolated_integer128(integer_op::signed_divide, integer_top, integer_all) == integer_top);
   BOOST_CHECK(isolated_integer128(integer_op::signed_modulo, integer_top, integer_all) == 0);
   for (uint32_t shift : {128u, 129u, 255u, UINT32_MAX})
      BOOST_CHECK(!isolated_integer128(integer_op::arithmetic_right, integer_all, 0, shift));
   BOOST_CHECK(!isolated_integer128(static_cast<integer_op>(255), 1, 1));
}

BOOST_FIXTURE_TEST_CASE(nine_typed_hosts_cover_alignment_aliases_limits_cancellation_and_failed_reuse, integer_fixture) {
   isolated_wasm_module module(code);
   for (unsigned op = 0; op < 9; ++op) BOOST_TEST_CONTEXT(integer_names[op]) {
      for (const auto& input : integer_good_inputs(op)) {
         const auto expected = integer_expected(input); BOOST_REQUIRE(expected);
         const auto result = module.execute(base, input);
         BOOST_CHECK(result.return_value == integer_bytes(*expected)); BOOST_CHECK(result.state.writes().empty());
         BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
      }
      for (const auto& bad : integer_bad_inputs(op)) {
         BOOST_CHECK_THROW(module.execute(base, bad), primary_state_unsupported);
         const auto good = integer_input(op);
         BOOST_CHECK(module.execute(base, good).return_value == integer_bytes(*integer_expected(good)));
      }
   }
   for (uint32_t shift : {128u, 129u, 255u, UINT32_MAX}) {
      BOOST_CHECK_THROW(module.execute(base, integer_input(1, integer_all, 0, shift)), primary_state_unsupported);
      BOOST_CHECK(module.execute(base, integer_input(1)).return_value == integer_bytes(*integer_expected(integer_input(1))));
   }
   std::stop_source stop; stop.request_stop();
   BOOST_CHECK_THROW(module.execute(base, integer_input(4), stop.get_token()), isolated_execution_interrupted);
   isolated_cancel expired; expired.deadline = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(module.execute(base, integer_input(8), {}, {}, expired), isolated_execution_interrupted);
   BOOST_CHECK(module.execute(base, integer_input(6)).return_value == integer_bytes(*integer_expected(integer_input(6))));
}

BOOST_FIXTURE_TEST_CASE(integer_start_functions_reinitialize_mutated_memory, integer_fixture) {
   for (unsigned op = 0; op < 9; ++op) {
      isolated_wasm_module module(integer_start_program(op));
      for (unsigned round = 0; round < 3; ++round)
         BOOST_CHECK(module.execute(base, integer_input(op)).return_value == integer_bytes(*integer_expected(integer_input(op))));
   }
}

BOOST_FIXTURE_TEST_CASE(integer_jobs_are_isolated_on_persistent_one_two_four_worker_pools, integer_fixture) {
   const auto code_hash = fc::sha256::hash(code.data(), code.size());
   for (unsigned workers : {1u, 2u, 4u}) {
      isolated_worker_pool pool(workers);
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<ordered_attempt_batch::request> requests;
         std::vector<isolated_action_input> inputs;
         for (unsigned i = 0; i < 27; ++i) {
            const auto op = i % 9;
            inputs.push_back(i >= 18 && op >= 4 && op != 6 ? integer_input(op, 7, 0) : integer_input(op));
            const std::array hashes{isolated_execution_fingerprint(code_hash, {}, inputs.back())};
            requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_transaction_fingerprint(hashes)});
         }
         ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < inputs.size(); ++i) jobs.emplace_back(batch.get_ticket(i), code, inputs[i], isolated_wasm_limits{});
         const auto results = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(results.size(), inputs.size());
         for (size_t i = 0; i < results.size(); ++i) {
            const auto expected = integer_expected(inputs[i]); BOOST_CHECK(!results[i].request);
            if (!expected) { BOOST_CHECK(!results[i].result); BOOST_CHECK(results[i].failure == isolated_worker_failure::unsupported); }
            else {
               BOOST_REQUIRE(results[i].result); BOOST_REQUIRE_EQUAL(results[i].result->actions.size(), 1u);
               BOOST_CHECK(results[i].result->state.writes().empty());
               BOOST_CHECK(results[i].result->actions[0].return_value == integer_bytes(*expected));
            }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(integer_hosts_execute_in_context_free_sync_and_mixed_trees, integer_fixture) {
   const std::vector<isolated_receiver_program> ordinary{{integer_account, code, {}}, {integer_child, code, {}}};
   const std::vector<isolated_sync_program> programs{{ordinary[0], true}, {ordinary[1], true}};
   isolated_action_tree_context tree; tree.context_free_data = isolated_context_free_data::capture({});
   isolated_sync_context sync; sync.get_sender_fix = true;
   isolated_action_tree_executor tree_executor; isolated_sync_executor sync_executor; isolated_mixed_tree_executor mixed;
   for (unsigned op = 0; op < 9; ++op) {
      const auto expected = integer_bytes(*integer_expected(integer_input(op)));
      const std::array roots{integer_graph_input(op, 2)};
      const auto cf = tree_executor.execute(base, roots, ordinary, tree);
      BOOST_REQUIRE(validate_isolated_action_tree_journal(cf, roots, ordinary, tree));
      BOOST_REQUIRE_EQUAL(cf.steps.size(), 2u); BOOST_CHECK(cf.groups[cf.steps[1].group_index].context_free);
      BOOST_CHECK(cf.steps[0].effects.return_value == expected); BOOST_CHECK(cf.steps[1].effects.return_value.empty());
      const auto sync_root = integer_graph_input(op, 1);
      const auto called = sync_executor.execute(base, sync_root, programs, sync);
      BOOST_REQUIRE(validate_isolated_sync_journal(called, sync_root, programs, sync));
      BOOST_REQUIRE_EQUAL(called.calls.size(), 1u); BOOST_CHECK_EQUAL(called.calls[0].status, 0);
      BOOST_CHECK(called.calls[0].return_value == expected);
      const std::array mixed_roots{integer_graph_input(op, 3)};
      const isolated_mixed_tree_context context{tree, sync};
      const auto both = mixed.execute(base, mixed_roots, programs, context);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(both, mixed_roots, programs, context));
      BOOST_REQUIRE_EQUAL(both.steps.size(), 2u); BOOST_REQUIRE_EQUAL(both.steps[0].calls.size(), 1u);
      BOOST_CHECK(both.steps[0].calls[0].return_value == expected); BOOST_CHECK(both.steps[1].effects.return_value.empty());
      BOOST_CHECK(both.state.writes().empty());
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_integer_oracle_tests)

BOOST_AUTO_TEST_CASE(selected_integer_profile_configures_canonical_and_private_engines) {
   integer_live_corpus corpus;
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

BOOST_AUTO_TEST_CASE(actual_original_wasm_matches_integer_extremes_aliases_and_pointer_failures) {
   eosio::testing::tester original; original.create_accounts({integer_account});
   const auto code = integer_program(); original.set_code(integer_account, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
   chainbase_transaction_state_view live(original.control->db()); const auto base = frozen_primary_state::capture(live, {});
   isolated_wasm_module module(code); uint32_t nonce = 0;
   for (unsigned op = 0; op < 9; ++op) BOOST_TEST_CONTEXT(integer_names[op]) {
      for (auto input : integer_good_inputs(op)) {
         // The original tester bills 2,000 us per transaction. Keep every
         // vector, but seal bounded batches instead of exhausting one block's
         // original CPU allowance; do not increase chain resource limits.
         if (nonce && nonce % 32 == 0) original.produce_block();
         iput(input.data, 48, ++nonce);
         const auto expected = integer_expected(input); BOOST_REQUIRE(expected);
         const auto candidate = module.execute(base, input);
         const auto trace = integer_push(original, input, nonce); BOOST_REQUIRE(trace && !trace->except);
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         BOOST_CHECK(trace->action_traces[0].return_value == integer_bytes(*expected));
         BOOST_CHECK(candidate.return_value == trace->action_traces[0].return_value);
      }
      for (auto input : integer_bad_inputs(op)) {
         if (nonce && nonce % 32 == 0) original.produce_block();
         iput(input.data, 48, ++nonce); const auto before = original.control->calculate_integrity_hash();
         BOOST_CHECK_THROW(module.execute(base, input), primary_state_unsupported);
         BOOST_CHECK_THROW(integer_push(original, input, nonce), fc::exception);
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(original_integer_start_functions_agree_and_reset_on_reuse) {
   eosio::testing::tester original; original.create_accounts({integer_account});
   chainbase_transaction_state_view live(original.control->db()); const auto base = frozen_primary_state::capture(live, {});
   uint32_t nonce = 0;
   for (unsigned op = 0; op < 9; ++op) {
      const auto code = integer_start_program(op); original.set_code(integer_account, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
      isolated_wasm_module module(code);
      for (unsigned round = 0; round < 3; ++round) {
         const auto input = integer_input(op);
         const auto candidate = module.execute(base, input);
         const auto trace = integer_push(original, input, ++nonce); BOOST_REQUIRE(trace && !trace->except);
         BOOST_CHECK(trace->action_traces[0].return_value == candidate.return_value);
         BOOST_CHECK(candidate.return_value == integer_bytes(*integer_expected(input)));
      }
   }
}

BOOST_AUTO_TEST_CASE(division_failure_discards_private_write_prefix_and_preserves_original_undo) {
   eosio::testing::tester original; original.create_accounts({integer_account});
   const auto code = integer_program(true); original.set_code(integer_account, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
   chainbase_transaction_state_view live(original.control->db()); const table_key rows{integer_account, integer_account, "introws"_n};
   const std::array tables{rows}; isolated_wasm_module module(code);
   for (unsigned op : {4u, 5u, 7u, 8u}) {
      const auto base = frozen_primary_state::capture(live, tables); const auto before = original.control->calculate_integrity_hash();
      auto bad = integer_input(op, integer_top, 0); iput(bad.data, 48, uint32_t{op + 1});
      BOOST_CHECK_THROW(module.execute(base, bad), primary_state_unsupported);
      BOOST_CHECK_THROW(integer_push(original, bad, op + 1), arithmetic_exception);
      BOOST_CHECK(original.control->calculate_integrity_hash() == before);
      BOOST_CHECK(!live.read_primary({rows, primary_query_kind::find, op + 1}, 16).row);
      auto good = integer_input(op); iput(good.data, 48, uint32_t{op + 1});
      const auto candidate = module.execute(base, good); BOOST_REQUIRE_EQUAL(candidate.state.writes().size(), 1u);
      const auto trace = integer_push(original, good, op + 1); BOOST_REQUIRE(trace && !trace->except);
      BOOST_CHECK(trace->action_traces[0].return_value == candidate.return_value);
      const auto row = live.read_primary({rows, primary_query_kind::find, op + 1}, 16);
      BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == candidate.state.primary_write(0).value);
   }
}

BOOST_AUTO_TEST_CASE(integer_results_are_adopted_in_production_and_block_validation_and_invalid_cases_fall_back) {
   integer_live_corpus corpus;
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory pd, sd, vd;
      integer_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); uint32_t nonce = 30000;
      // First twelve batches: every host in every execution mode. Last two:
      // original divide-by-zero and oversized ASHR, with whole serial tails.
      for (unsigned scenario = 0; scenario < 14; ++scenario) BOOST_TEST_CONTEXT("integer workers=" << workers << " scenario=" << scenario) {
         const unsigned route = scenario < 12 ? scenario / 3 : 0;
         producer.start(); std::vector<transaction_metadata_ptr> batch;
         for (unsigned i = 0; i < 3; ++i) {
            auto input = integer_graph_input(scenario < 12 ? (scenario % 3) * 3 + i : i + 4, route);
            if (i == 0 && scenario == 12) input = integer_input(4, 7, 0);
            if (i == 0 && scenario == 13) input = integer_input(1, integer_all, 0, 128);
            iput(input.data, 48, nonce++);
            signed_transaction trx;
            trx.actions.push_back(action{{{integer_account, "active"_n}}, integer_account, input.action, input.data});
            producer.set_transaction_headers(trx); trx.sign(producer.get_private_key(integer_account, "active"), producer.control->get_chain_id());
            batch.push_back(transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input));
         }
         std::vector<std::vector<char>> produced, original, candidate;
         const auto collect = [&](auto& dest, const auto& event) {
            const auto trace = std::get<0>(event);
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; })) dest.push_back(integer_trace(trace));
         };
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) { collect(original, e); });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) { collect(candidate, e); });
         const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
         size_t callbacks = 0;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            const auto state_before = producer.control->calculate_integrity_hash();
            const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
            if (scenario == 12 && callbacks == 0) {
               BOOST_REQUIRE(trace && trace->except); BOOST_CHECK_EQUAL(trace->except->code(), arithmetic_exception::code_value);
               BOOST_CHECK(!trace->receipt); BOOST_CHECK(producer.control->calculate_integrity_hash() == state_before);
               BOOST_CHECK(!producer.control->is_known_unexpired_transaction(tx->id()));
            } else {
               produced.push_back(integer_trace(trace));
               BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route & 2 ? 2u : 1u);
               BOOST_REQUIRE_EQUAL(trace->action_traces[0].call_traces.size(), route & 1 ? 1u : 0u);
            }
            ++callbacks; return true;
         }), batch.size());
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT("attempts=" << after.attempts - before.attempts << " succeeded=" << after.succeeded - before.succeeded
            << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
            << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
            << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks
            << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
            << " vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts
            << " worker_other=" << after.worker_other_failures - before.worker_other_failures) {
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, scenario < 12 ? batch.size() : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, scenario < 12 ? 0u : batch.size());
         }
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(original == produced); BOOST_CHECK(candidate == produced);
         BOOST_CHECK_EQUAL(original.size(), scenario == 12 ? 2u : 3u);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, scenario == 13 ? 0u : original.size());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, scenario == 13 ? original.size() : 0u);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
