#include <eosio/chain/parallel_console.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/vm/exceptions.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/state_history/serialization.hpp>
#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "parallel_test_runtime.hpp"

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr name console_account = "test"_n, console_child = "bob"_n, console_leaf = "carol"_n;
using console_op = isolated_console_operation;
std::string cn(name account) { return "(i64.const " + std::to_string(account.to_uint64_t()) + ")"; }
std::string console_call(name receiver) {
   return "(drop (call $call " + cn(receiver) + "(i64.const 0)(i32.const 0)(i32.const 0)))";
}
std::string console_bytes(std::span<const char> bytes) {
   std::string out; constexpr char hex[] = "0123456789abcdef";
   for (unsigned char byte : bytes) { out += '\\'; out += hex[byte >> 4]; out += hex[byte & 15]; }
   return out;
}
const std::string all_console_calls = R"(
   (call $prints (i32.const 16))
   (call $prints_l (i32.const 32)(i32.const 3))
   (call $printi (i64.const -7))(call $printui (i64.const -1))
   (call $printi128 (i32.const 49))(call $printui128 (i32.const 80))
   (call $printsf (f32.const -0))(call $printdf (f64.const 1.25))
   (call $printqf (i32.const 113))(call $printn (i64.const 3773036822876127232))
   (call $printhex (i32.const 144)(i32.const 2))
   (i32.store8 (i32.const 16)(i32.const 90))
   (i64.store (i32.const 49)(i64.const 0)))";

std::vector<char> console_program(const std::string& apply = "(call $all)",
      const std::string& child = "(call $all)", bool start = true, const std::string& extra_data = "",
      const std::string& extra_imports = "") {
   const auto code = wast_to_wasm(R"((module
      (import "env" "prints" (func $prints (param i32)))
      (import "env" "prints_l" (func $prints_l (param i32 i32)))
      (import "env" "printi" (func $printi (param i64)))
      (import "env" "printui" (func $printui (param i64)))
      (import "env" "printi128" (func $printi128 (param i32)))
      (import "env" "printui128" (func $printui128 (param i32)))
      (import "env" "printsf" (func $printsf (param f32)))
      (import "env" "printdf" (func $printdf (param f64)))
      (import "env" "printqf" (func $printqf (param i32)))
      (import "env" "printn" (func $printn (param i64)))
      (import "env" "printhex" (func $printhex (param i32 i32)))
      (import "env" "call" (func $call (param i64 i64 i32 i32) (result i64)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "send_inline" (func $inline (param i32 i32)))
      (import "env" "send_context_free_inline" (func $cf (param i32 i32))))" + extra_imports + R"(
      (memory 1 1)
      (data (i32.const 16) "text\00")(data (i32.const 32) "B\00C")
      (data (i32.const 49) "\01\00\00\00\00\00\00\00\00\00\00\00\00\00\00\80")
      (data (i32.const 80) "\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff\ff")
      (data (i32.const 113) "\00\00\00\00\00\00\00\00\00\00\00\00\00\80\ff\3f")
      (data (i32.const 144) "\00\ff"))" + extra_data +
      "(func $all " + all_console_calls + ")" +
      (start ? "(func $start (call $printi (i64.const -9)))(start $start)" : "") +
      "(func (export \"apply\") (param $receiver i64)(param $account i64)(param $action i64)" + apply + ")" +
      "(func (export \"sync_call\") (param i64 i64 i32)(result i64)" + child + "(i64.const 0)))");
   return {code.begin(), code.end()};
}
isolated_action_input console_input(name action_name = "run"_n) {
   isolated_action_input input; input.receiver = input.account = console_account; input.action = action_name;
   input.authorizations = {{console_account, "active"_n}}; return input;
}
isolated_action_tree_context console_tree_context(bool enabled) {
   isolated_action_tree_context c; c.context_free_data = isolated_context_free_data::capture({});
   c.console_enabled = enabled; return c;
}
isolated_sync_context console_sync_context(bool enabled) {
   isolated_sync_context c; c.get_sender_fix = true; c.console_enabled = enabled; return c;
}
struct console_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   console_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const std::array accounts{console_account, console_child, console_leaf, "missing"_n};
      for (name account : {console_account, console_child, console_leaf})
         db.create<account_object>([&](auto& a) { a.name = account; });
      base = frozen_primary_state::capture_queries(view, {}, {}, {}, {}, accounts);
   }
};
template<class Events> const isolated_console_event& console_event(const Events& events, size_t index) {
   return std::get<isolated_console_event>(std::get<isolated_cursor_event>(events.at(index)));
}

// One catalog exercises ordinary, pure sync and mixed execution. CF/normal
// inline use the same code but a different action, so children do not reschedule.
std::vector<isolated_sync_program> console_graph() {
   const auto ordinary = fc::raw::pack(action{{{console_account, "active"_n}}, console_account, "quiet"_n, std::vector<char>{}});
   const auto cf = fc::raw::pack(action{{}, console_account, "quiet"_n, std::vector<char>{}});
   const auto send = [](const char* host, size_t offset, size_t size) {
      return "(call $" + std::string(host) + "(i32.const " + std::to_string(offset) + ")(i32.const " + std::to_string(size) + "))";
   };
   const auto root = console_program("(call $all)"
      "(if (i64.eq (get_local $action)" + cn("sync"_n) + ")(then " + console_call(console_child) + "))"
      "(if (i64.eq (get_local $action)" + cn("mixed"_n) + ")(then " + console_call(console_child) +
      "(call $notify " + cn(console_child) + ")" + send("inline", 1024, ordinary.size()) + send("cf", 2048, cf.size()) + "))"
      "(call $printui (i64.const 42))", "(call $all)", true,
      "(data (i32.const 1024)\"" + console_bytes(ordinary) + "\")(data (i32.const 2048)\"" + console_bytes(cf) + "\")");
   const auto child = console_program("(call $all)", "(call $all)" + console_call(console_leaf) + "(call $printi (i64.const -4))");
   return {{{console_account, root, {}}, true}, {{console_child, child, {}}, true},
           {{console_leaf, console_program(), {}}, true}};
}

struct console_numeric_vectors {
   std::vector<char> code;
   std::vector<isolated_console_event> expected;
   console_numeric_vectors() {
      std::string body, data = "(data (i32.const 256)\"|\")";
      const auto emit = [&](console_op op, const char* host, const std::string& arg, std::array<uint64_t, 2> bits) {
         body += "(call $" + std::string(host) + arg + ")(call $prints_l (i32.const 256)(i32.const 1))";
         expected.push_back({op, bits, {}, 0});
         expected.push_back({console_op::prints_l, {}, {'|'}, 0});
      };
      const auto store = [&](uint32_t offset, std::array<uint64_t, 2> bits, size_t size) {
         std::vector<char> bytes;
         for (size_t i = 0; i < size; ++i) bytes.push_back(char(bits[i / 8] >> (8 * (i % 8))));
         data += "(data (i32.const " + std::to_string(offset) + ")\"" + console_bytes(bytes) + "\")";
      };
      for (uint64_t bits : {uint64_t{0}, uint64_t{1}, UINT64_MAX, uint64_t{1} << 63, (uint64_t{1} << 63) - 1}) {
         const auto arg = "(i64.const " + std::to_string(bits) + ")";
         emit(console_op::printi, "printi", arg, {bits, 0});
         emit(console_op::printui, "printui", arg, {bits, 0});
      }
      const std::array<std::array<uint64_t, 2>, 5> integers{{{0, 0}, {1, 0}, {0, uint64_t{1} << 63},
         {UINT64_MAX, (uint64_t{1} << 63) - 1}, {UINT64_MAX, UINT64_MAX}}};
      for (size_t i = 0; i < integers.size(); ++i) for (uint32_t base : {512u, 2049u}) {
         const uint32_t offset = base + 16 * i; store(offset, integers[i], 16);
         const auto arg = "(i32.const " + std::to_string(offset) + ")";
         emit(console_op::printi128, "printi128", arg, integers[i]);
         emit(console_op::printui128, "printui128", arg, integers[i]);
      }
      const std::array<uint32_t, 13> singles{0, 0x80000000u, 1, 0x007fffffu, 0x00800000u, 0x3f800000u,
         0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00025u, 0x7f800001u, 0xffc00001u};
      for (size_t i = 0; i < singles.size(); ++i) {
         const uint32_t offset = 16384 + 4 * i; store(offset, {singles[i], 0}, 4);
         emit(console_op::printsf, "printsf", "(f32.load (i32.const " + std::to_string(offset) + "))", {singles[i], 0});
      }
      const std::array<uint64_t, 13> doubles{0, 0x8000000000000000ull, 1, 0x000fffffffffffffull,
         0x0010000000000000ull, 0x3ff0000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
         0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000025ull, 0x7ff0000000000001ull, 0xfff8000000000001ull};
      for (size_t i = 0; i < doubles.size(); ++i) {
         const uint32_t offset = 20480 + 8 * i; store(offset, {doubles[i], 0}, 8);
         emit(console_op::printdf, "printdf", "(f64.load (i32.const " + std::to_string(offset) + "))", {doubles[i], 0});
      }
      const std::array<std::array<uint64_t, 2>, 13> quads{{{0, 0}, {0, 0x8000000000000000ull}, {1, 0},
         {UINT64_MAX, 0x0000ffffffffffffull}, {0, 0x0001000000000000ull}, {0, 0x3fff000000000000ull},
         {UINT64_MAX, 0x7ffeffffffffffffull}, {UINT64_MAX, 0xfffeffffffffffffull},
         {0, 0x7fff000000000000ull}, {0, 0xffff000000000000ull}, {37, 0x7fff800000000000ull},
         {1, 0x7fff000000000000ull}, {1, 0xffff800000000000ull}}};
      for (size_t i = 0; i < quads.size(); ++i) for (uint32_t base : {4096u, 8193u}) {
         const uint32_t offset = base + 16 * i; store(offset, quads[i], 16);
         emit(console_op::printqf, "printqf", "(i32.const " + std::to_string(offset) + ")", quads[i]);
      }
      code = console_program(body, "", false, data);
   }
};

std::vector<isolated_sync_program> console_negative_graph() {
   const auto ordinary = fc::raw::pack(action{{{console_account, "active"_n}}, console_account, "quiet"_n, std::vector<char>{}});
   const auto cf = fc::raw::pack(action{{}, console_account, "quiet"_n, std::vector<char>{}});
   const auto extra = "(data (i32.const 1024)\"" + console_bytes(ordinary) + "\")"
      "(data (i32.const 2048)\"" + console_bytes(cf) + "\")";
   const auto root = console_program("(if (i64.eq (get_local $action)" + cn("quiet"_n) + ")(then (call $printi (i64.const 7)))"
      "(else (call $printi (i64.const 11))(call $prints_l (i32.const 65536)(i32.const 0))" + console_call(console_child) +
      "(call $printi (i64.const 222))" + console_call("missing"_n) + "(call $printi (i64.const 3))"
      "(if (i64.eq (get_local $action)" + cn("mixed"_n) + ")(then (call $notify " + cn(console_child) + ")"
      "(call $inline (i32.const 1024)(i32.const " + std::to_string(ordinary.size()) + "))"
      "(call $cf (i32.const 2048)(i32.const " + std::to_string(cf.size()) + "))))))", "", false, extra);
   const auto child = console_program("(call $printi (i64.const -6))", "(call $printi (i64.const -9))" +
      console_call(console_leaf) + "(call $printi (i64.const 7))(return (i64.const -10007))", false);
   const auto leaf = console_program("", "(call $printi (i64.const 8))(return (i64.const -10042))", false);
   return {{{console_account, root, {}}, true}, {{console_child, child, {}}, true}, {{console_leaf, leaf, {}}, true}};
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_console_private_tests)

BOOST_AUTO_TEST_CASE(typed_shape_and_transaction_budget_reject_malformed_and_overflowed_events) {
   isolated_console_event event{console_op::prints, {}, {'x', '\0'}, 7};
   isolated_console_budget budget;
   BOOST_CHECK(!budget.accept(event, 7, false)); BOOST_CHECK(!budget.accept(event, 8, true));
   BOOST_CHECK(budget.accept(event, 7, true));
   for (unsigned corruption = 0; corruption < 5; ++corruption) {
      auto bad = event;
      if (corruption == 0) bad.operation = static_cast<console_op>(255);
      if (corruption == 1) bad.bytes.pop_back();
      if (corruption == 2) bad.bytes.insert(bad.bytes.begin(), '\0');
      if (corruption == 3) bad.bits[0] = 1;
      if (corruption == 4) bad.bytes.resize(isolated_console_budget::max_event_bytes + 1);
      BOOST_CHECK(!isolated_console_budget::argument_bytes(bad));
   }
   isolated_console_budget bytes;
   for (unsigned i = 0; i < 4; ++i) BOOST_CHECK(bytes.consume(65536));
   BOOST_CHECK(!bytes.consume(1)); BOOST_CHECK(bytes.consume(0));
   isolated_console_budget count;
   for (size_t i = 0; i < isolated_console_budget::max_events; ++i) BOOST_CHECK(count.consume(0));
   BOOST_CHECK(!count.consume(0));
   const std::array<uint32_t, 2> markers{0, 3}; size_t next = 0;
   BOOST_CHECK(accept_isolated_console_marker(markers, next, 0, true));
   BOOST_CHECK(!accept_isolated_console_marker(markers, next, 2, true));
   BOOST_CHECK(accept_isolated_console_marker(markers, next, 3, true));
   BOOST_CHECK(!accept_isolated_console_marker(markers, next, 3, true));
}

BOOST_FIXTURE_TEST_CASE(all_eleven_hosts_keep_owned_bits_bytes_start_order_and_cold_warm_isolation, console_fixture) {
   const auto code = console_program(); const std::array roots{console_input()};
   const std::array programs{isolated_receiver_program{console_account, code, {}}};
   isolated_action_tree_executor executor;
   for (bool enabled : {false, true, false, true}) for (unsigned round = 0; round < 3; ++round) {
      const auto context = console_tree_context(enabled);
      const auto result = executor.execute(base, roots, programs, context);
      BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, programs, context));
      BOOST_REQUIRE_EQUAL(result.steps.size(), 1u); const auto& events = result.steps[0].events;
      BOOST_REQUIRE_EQUAL(events.size(), enabled ? 12u : 0u);
      BOOST_CHECK(result.state.writes().empty());
      if (!enabled) continue;
      BOOST_CHECK(console_event(events, 0).operation == console_op::printi);
      BOOST_CHECK_EQUAL(console_event(events, 0).bits[0], uint64_t(-9));
      for (unsigned i = 0; i < 11; ++i) BOOST_CHECK(console_event(events, i + 1).operation == static_cast<console_op>(i));
      BOOST_CHECK(console_event(events, 1).bytes == std::vector<char>({'t','e','x','t','\0'}));
      BOOST_CHECK(console_event(events, 2).bytes == std::vector<char>({'B','\0','C'}));
      BOOST_CHECK_EQUAL(console_event(events, 5).bits[0], 1u); // VM overwrites this AFTER capture
      BOOST_CHECK_EQUAL(console_event(events, 5).bits[1], uint64_t{1} << 63);
      BOOST_CHECK_EQUAL(console_event(events, 7).bits[0], 0x80000000u);
      BOOST_CHECK_EQUAL(console_event(events, 8).bits[0], 0x3ff4000000000000ull);
      BOOST_CHECK(console_event(events, 11).bytes == std::vector<char>({'\0', char(255)}));
      auto changed = context; changed.console_enabled = false;
      BOOST_CHECK(!validate_isolated_action_tree_journal(result, roots, programs, changed));
   }
   // Old result types cannot discard console side effects, even for an empty print.
   isolated_wasm_module legacy(code);
   BOOST_CHECK_THROW(legacy.execute(base, roots[0]), primary_state_unsupported);
}

BOOST_FIXTURE_TEST_CASE(numeric_extremes_keep_exact_bits_in_aligned_unaligned_and_reused_modules, console_fixture) {
   const console_numeric_vectors vectors;
   const std::array programs{isolated_receiver_program{console_account, vectors.code, {}}};
   const std::array roots{console_input()}; isolated_action_tree_executor executor;
   for (bool enabled : {false, true, false, true}) for (unsigned round = 0; round < 2; ++round) {
      const auto context = console_tree_context(enabled);
      const auto result = executor.execute(base, roots, programs, context);
      BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, programs, context));
      BOOST_REQUIRE_EQUAL(result.steps.size(), 1u);
      const auto& events = result.steps[0].events;
      BOOST_REQUIRE_EQUAL(events.size(), enabled ? vectors.expected.size() : 0u);
      if (enabled) for (size_t i = 0; i < events.size(); ++i)
         BOOST_TEST_CONTEXT("console numeric event=" << i << " round=" << round) {
            BOOST_CHECK(console_event(events, i) == vectors.expected[i]);
         }
   }
}

BOOST_FIXTURE_TEST_CASE(console_off_still_validates_pointers_and_failed_modules_are_reusable, console_fixture) {
   const std::array roots{console_input()}; isolated_action_tree_executor executor;
   const std::array<std::string, 8> bad{
      "(call $prints (i32.const 65536))", "(call $prints_l (i32.const 65535)(i32.const 2))",
      "(call $printhex (i32.const 0)(i32.const -1))", "(call $printi128 (i32.const 0))",
      "(call $printui128 (i32.const 65521))", "(call $printqf (i32.const 0))",
      "(call $printqf (i32.const 65521))", "(call $prints_l (i32.const -1)(i32.const 0))"};
   for (bool enabled : {false, true}) for (const auto& body : bad) {
      const auto context = console_tree_context(enabled);
      std::array programs{isolated_receiver_program{console_account, console_program(body, "", false), {}}};
      BOOST_CHECK_THROW(executor.execute(base, roots, programs, context), primary_state_unsupported);
      programs[0].code = console_program("(call $prints_l (i32.const 65536)(i32.const 0))"
         "(call $printhex (i32.const 0)(i32.const 0))", "", false);
      const auto result = executor.execute(base, roots, programs, context);
      BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, programs, context));
      BOOST_CHECK_EQUAL(result.steps[0].events.size(), enabled ? 2u : 0u);
   }
}

BOOST_FIXTURE_TEST_CASE(nested_sync_and_mixed_cf_notification_inline_markers_are_frame_local, console_fixture) {
   const auto graph = console_graph(); isolated_sync_executor sync; isolated_mixed_tree_executor mixed;
   for (bool enabled : {false, true}) {
      const auto sync_context = console_sync_context(enabled);
      const auto input = console_input("sync"_n);
      const auto result = sync.execute(base, input, graph, sync_context);
      BOOST_REQUIRE(validate_isolated_sync_journal(result, input, graph, sync_context));
      BOOST_REQUIRE_EQUAL(result.calls.size(), 2u);
      BOOST_CHECK(result.root_console_markers == (enabled ? std::vector<uint32_t>{12} : std::vector<uint32_t>{}));
      BOOST_CHECK(result.calls[0].console_markers == (enabled ? std::vector<uint32_t>{12} : std::vector<uint32_t>{}));
      const isolated_mixed_tree_context context{console_tree_context(enabled), sync_context};
      const std::array roots{console_input("mixed"_n), console_input("mixed"_n)};
      const auto both = mixed.execute(base, roots, graph, context);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(both, roots, graph, context));
      BOOST_REQUIRE_EQUAL(both.steps.size(), 8u);
      BOOST_CHECK(both.groups[both.steps[2].group_index].context_free); // CF queue precedes normal inline
      BOOST_CHECK(!both.groups[both.steps[3].group_index].context_free);
      BOOST_CHECK(both.steps[0].console_markers == result.root_console_markers);
      if (enabled) {
         auto bad = both; bad.steps[0].console_markers[0] = 11;
         BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad, roots, graph, context));
         bad = both;
         std::get<isolated_console_event>(std::get<isolated_cursor_event>(bad.steps[2].events[0])).after_writes = 1;
         BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad, roots, graph, context));
         bad = both;
         bad.steps[2].events[0] = isolated_cursor_event{isolated_cursor_query{}};
         BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad, roots, graph, context));
      }
   }
}

BOOST_FIXTURE_TEST_CASE(console_limits_cover_all_roots_and_do_not_become_contract_visible_errors, console_fixture) {
   isolated_action_tree_executor executor; const auto context = console_tree_context(true);
   std::array programs{isolated_receiver_program{console_account,
      console_program("(call $prints_l (i32.const 0)(i32.const 65536))", "", false), {}}};
   std::vector<isolated_action_input> roots(4, console_input());
   const auto good = executor.execute(base, roots, programs, context);
   BOOST_REQUIRE(validate_isolated_action_tree_journal(good, roots, programs, context));
   roots.push_back(console_input());
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, context), primary_state_budget_exceeded);
   BOOST_CHECK_NO_THROW(executor.execute(base, roots, programs, console_tree_context(false)));
   roots.resize(1);
   std::string many;
   for (size_t i = 0; i < 1025; ++i) many += "(call $printi (i64.const 1))";
   programs[0].code = console_program(many, "", false);
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, context), primary_state_budget_exceeded);
   programs[0].code = console_program();
   BOOST_CHECK_NO_THROW(executor.execute(base, roots, programs, context));
}

BOOST_FIXTURE_TEST_CASE(one_two_four_workers_reuse_console_modules_across_tree_sync_and_mixed_modes, console_fixture) {
   const auto graph = console_graph(); std::vector<isolated_receiver_program> ordinary;
   for (const auto& p : graph) ordinary.push_back(p.program);
   const auto code_hash = fc::sha256::hash(ordinary[0].code.data(), ordinary[0].code.size());
   for (size_t threads : {1u, 2u, 4u}) {
      isolated_worker_pool pool(threads);
      for (bool enabled : {true, false, true}) {
         const auto tree = console_tree_context(enabled); const auto sync = console_sync_context(enabled);
         const isolated_mixed_tree_context mixed{tree, sync};
         const std::vector<isolated_action_input> tr{console_input()}, sr{console_input("sync"_n)}, mr{console_input("mixed"_n)};
         const std::array root_hash{isolated_execution_fingerprint(code_hash, {}, tr[0])};
         const std::array fingerprints{isolated_action_tree_fingerprint(root_hash, tree),
            isolated_sync_transaction_fingerprint(sr, graph, sync), isolated_mixed_tree_fingerprint(mr, graph, mixed)};
         std::vector<ordered_attempt_batch::request> requests;
         for (size_t i = 0; i < 6; ++i) requests.push_back({fc::sha256::hash(std::to_string(i)), fingerprints[i % 3]});
         ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < 6; ++i) {
            if (i % 3 == 0) jobs.emplace_back(batch.get_ticket(i), tr, ordinary, tree);
            else if (i % 3 == 1) jobs.emplace_back(batch.get_ticket(i), sr, graph, sync);
            else jobs.emplace_back(batch.get_ticket(i), isolated_mixed_tree_job{mr, graph, mixed});
         }
         const auto results = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(results.size(), requests.size());
         for (size_t i = 0; i < results.size(); ++i) BOOST_TEST_CONTEXT("threads=" << threads << " console=" << enabled
            << " job=" << i << " failure=" << isolated_worker_failure_name(results[i].failure)
            << " attempt_wall_us=" << results[i].attempt_wall.count()) {
            const auto& r = results[i]; BOOST_REQUIRE(r.failure == isolated_worker_failure::none);
            if (i % 3 == 0) {
               BOOST_REQUIRE(r.tree_result);
               BOOST_CHECK(validate_isolated_action_tree_journal(*r.tree_result, tr, ordinary, tree));
               BOOST_CHECK_EQUAL(r.tree_result->steps[0].events.size(), enabled ? 13u : 0u);
            } else if (i % 3 == 1) {
               BOOST_REQUIRE(r.sync_transaction_result);
               BOOST_CHECK(validate_isolated_sync_transaction_journal(*r.sync_transaction_result, sr, graph, sync));
            } else {
               BOOST_REQUIRE(r.mixed_result);
               BOOST_CHECK(validate_isolated_mixed_tree_journal(*r.mixed_result, mr, graph, mixed));
            }
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
struct console_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
struct console_corpus {
   genesis_state genesis = eosio::testing::base_tester::default_genesis();
   std::vector<isolated_sync_program> programs;
   std::vector<std::vector<char>> blocks;
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool enabled,
                                    bool production = false) const {
      auto cfg = eosio::testing::base_tester::default_config(dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg); cfg.contracts_console = enabled;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      for (const auto& p : programs) cfg.parallel_code_allowlist.insert(fc::sha256::hash(p.program.code.data(), p.program.code.size()));
      return cfg;
   }
   explicit console_corpus(std::vector<isolated_sync_program> captured = console_graph()) : programs(std::move(captured)) {
      auto [key, pub, proof] = eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key = pub;
      genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory dir; console_chain source(configuration(dir, 0, true), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call, builtin_protocol_feature_t::get_sender_fix});
      seal(); seal(); source.create_accounts({console_account, console_child, console_leaf}); seal();
      for (const auto& p : programs)
         source.set_code(p.program.receiver, std::vector<uint8_t>(p.program.code.begin(), p.program.code.end()));
      seal();
   }
   void prefix(eosio::testing::tester& chain) const {
      for (const auto& block : blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         chain.control->abort_block();
      }
   }
};
std::vector<char> console_ship_trace(const transaction_trace_ptr& trace, bool debug) {
   // Use the ORIGINAL SHiP serializer; normalize timing only in a deep value
   // copy. Its format does not contain sync traces/markers, which remain in
   // the explicit comparison below. No live trace or protocol format changes.
   BOOST_REQUIRE(trace && !trace->failed_dtrx_trace);
   auto copy = std::make_shared<transaction_trace>(*trace);
   copy->elapsed = fc::microseconds{};
   for (auto& a : copy->action_traces) {
      a.elapsed = fc::microseconds{};
      for (auto& c : a.call_traces) c.elapsed = fc::microseconds{};
   }
   const eosio::state_history::augmented_transaction_trace augmented{copy};
   return fc::raw::pack(make_history_context_wrapper(debug, augmented));
}
std::vector<char> console_trace(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE_MESSAGE(trace && !trace->except && !trace->failed_dtrx_trace,
      (trace && trace->except ? trace->except->to_detail_string() : "missing console trace"));
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->receipt), fc::raw::pack(trace->account_ram_delta), fc::raw::pack(trace->error_code)};
   for (const auto& a : trace->action_traces) {
      BOOST_REQUIRE(!a.except && !a.error_code);
      fields.push_back(fc::raw::pack(a.action_ordinal)); fields.push_back(fc::raw::pack(a.creator_action_ordinal));
      fields.push_back(fc::raw::pack(a.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(a.receiver)); fields.push_back(fc::raw::pack(a.act));
      fields.push_back(fc::raw::pack(a.receipt)); fields.push_back(fc::raw::pack(a.context_free));
      fields.push_back(a.return_value); fields.push_back(fc::raw::pack(a.account_ram_deltas));
      fields.push_back(fc::raw::pack(a.console)); fields.push_back(fc::raw::pack(a.console_markers));
      for (const auto& c : a.call_traces) {
         BOOST_REQUIRE(!c.except && !c.error_code);
         fields.push_back(fc::raw::pack(c.call_ordinal)); fields.push_back(fc::raw::pack(c.sender_ordinal));
         fields.push_back(fc::raw::pack(c.receiver)); fields.push_back(fc::raw::pack(c.read_only));
         fields.push_back(c.data); fields.push_back(c.return_value); fields.push_back(fc::raw::pack(c.error_id));
         fields.push_back(fc::raw::pack(c.console)); fields.push_back(fc::raw::pack(c.console_markers));
      }
   }
   fields.push_back(console_ship_trace(trace, false));
   fields.push_back(console_ship_trace(trace, true));
   return fc::raw::pack(fields); // timing excluded, console/markers/receipt/RAM and original SHiP retained
}
void compare_console_production(console_corpus& corpus, const std::vector<name>& routes,
      const std::function<void(const transaction_trace_ptr&, bool, name)>& inspect, uint32_t batch_width = 2,
      uint32_t root_count = 1, packed_transaction::compression_type compression = packed_transaction::compression_type::none) {
   BOOST_REQUIRE(root_count >= 1 && root_count <= 16);
   for (bool enabled : {false, true}) for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory pd, sd, vd;
      console_chain producer(corpus.configuration(pd, workers, enabled, true), corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd, 0, enabled), corpus.genesis),
         parallel(corpus.configuration(vd, workers, enabled), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      // One FIXED cold batch checks the original result even if compilation or
      // discovery exhausts the unchanged production budget. Keep the original
      // TWO mandatory-adoption batches afterwards; never retry until success.
      for (name route : routes) for (unsigned round = 0; round < 3; ++round)
         BOOST_TEST_CONTEXT("console=" << enabled << " workers=" << workers << " route=" << route
                            << " round=" << round << " warm=" << (round != 0)) {
         producer.start(); std::vector<transaction_metadata_ptr> batch;
         for (uint32_t i = 0; i < batch_width; ++i) {
            signed_transaction trx;
            for (uint32_t root = 0; root < root_count; ++root)
               trx.actions.push_back(action{{{console_account, "active"_n}}, console_account, route,
                                            fc::raw::pack(uint32_t{round * batch_width + i})});
            producer.set_transaction_headers(trx);
            trx.sign(producer.get_private_key(console_account, "active"), producer.control->get_chain_id());
            batch.push_back(transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, compression),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input));
         }
         std::vector<std::vector<char>> expected, original, candidate;
         const auto collect = [&](auto& dest, const auto& event) {
            const auto trace = std::get<0>(event);
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; }))
               dest.push_back(console_trace(trace));
         };
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) { collect(original, event); });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) { collect(candidate, event); });
         const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
            expected.push_back(console_trace(trace));
            inspect(trace, enabled, route);
            return true;
         }), batch.size());
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT("attempts=" << after.attempts - before.attempts << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
            << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
            << " vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts
            << " worker_other=" << after.worker_other_failures - before.worker_other_failures
            << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
            << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks) {
            BOOST_TEST_MESSAGE("phase=" << (round ? "warm" : "cold") << " adopted=" << after.adopted - before.adopted
               << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
               << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines);
            if (round) {
               BOOST_CHECK_EQUAL(after.adopted - before.adopted, batch.size());
               BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
            }
         }
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(original == expected); BOOST_CHECK(candidate == expected); BOOST_CHECK_EQUAL(original.size(), batch.size());
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         if (round) {
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, batch.size());
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, 0u);
         }
      }
   }
}

constexpr table_key console_owner_rows{console_account, console_account, "rows"_n};
std::vector<isolated_sync_program> console_owner_graph() {
   const auto ordinary = fc::raw::pack(action{{{console_account, "active"_n}}, console_account, "quiet"_n, std::vector<char>{}});
   const auto cf = fc::raw::pack(action{{}, console_account, "quiet"_n, std::vector<char>{}});
   const auto when = [](name method, const std::string& body) {
      return "(if (i64.eq (get_local $action)" + cn(method) + ")(then " + body + "))";
   };
   const auto root = console_program(when("quiet"_n, "(call $printi (i64.const 7))(return)") +
      "(call $printi (i64.const 11))(drop (call $store " + cn(console_account) + cn("rows"_n) + cn(console_account) +
      "(i64.const 7)(i32.const 512)(i32.const 8)))(call $printi (i64.const 22))" +
      when("sync"_n, console_call(console_child)) + when("mixed"_n, console_call(console_child) +
         "(call $notify " + cn(console_child) + ")(call $inline (i32.const 1024)(i32.const " + std::to_string(ordinary.size()) +
         "))(call $cf (i32.const 2048)(i32.const " + std::to_string(cf.size()) + "))") +
      "(call $printi (i64.const 33))", "", false,
      "(data (i32.const 512)\"\\01\\00\\00\\00\\00\\00\\00\\00\")"
      "(data (i32.const 1024)\"" + console_bytes(ordinary) + "\")(data (i32.const 2048)\"" + console_bytes(cf) + "\")",
      "(import \"env\" \"db_store_i64\" (func $store (param i64 i64 i64 i64 i32 i32)(result i32)))");
   return {{{console_account, root, {}}, true},
      {{console_child, console_program("(call $printi (i64.const 55))", "(call $printi (i64.const 66))" +
         console_call(console_leaf) + "(call $printi (i64.const 77))"), {}}, true},
      {{console_leaf, console_program("", "(call $printi (i64.const 88))"), {}}, true}};
}

// Read-only owner admission and direct receiver replay, not a substitute for
// the signed production/validation tests above. The result contains a real
// prospective write, so a premature replay cannot hide behind a read-only graph.
struct console_owner_fixture {
   console_corpus corpus{console_owner_graph()};
   fc::temp_directory directory;
   console_chain chain;
   std::optional<packed_transaction> packed;
   std::vector<isolated_action_input> roots;
   std::vector<isolated_sync_program> programs;
   isolated_mixed_tree_context protocol;
   std::shared_ptr<const frozen_primary_state> base;
   console_owner_fixture(bool enabled, name route) : chain(corpus.configuration(directory, 0, enabled), corpus.genesis) {
      corpus.prefix(chain); chain.start();
      signed_transaction trx;
      trx.actions.push_back(action{{{console_account, "active"_n}}, console_account, route, std::vector<char>{}});
      chain.set_transaction_headers(trx);
      packed.emplace(trx, packed_transaction::compression_type::none);
      const auto captured = prepare_isolated_transaction(*chain.control, *packed); BOOST_REQUIRE(captured);
      for (const auto& a : *captured) roots.push_back(a.input);
      for (const auto& program : corpus.programs) {
         const auto p = prepare_isolated_sync_program(*chain.control, program.program.receiver); BOOST_REQUIRE(p);
         programs.push_back(*p);
      }
      protocol = {current_action_tree_context(*chain.control), current_sync_context(*chain.control)};
      protocol.tree.context_free_data = isolated_context_free_data::capture(packed->get_context_free_data());
      chainbase_transaction_state_view live(chain.control->db());
      const std::array tables{console_owner_rows};
      const std::array queries{primary_query{console_owner_rows, primary_query_kind::find, 7}};
      const std::array accounts{console_account, console_child, console_leaf};
      base = frozen_primary_state::capture_queries(live, tables, queries, {}, {}, accounts);
   }
   std::vector<isolated_receiver_program> tree_programs() const {
      std::vector<isolated_receiver_program> result;
      for (const auto& p : programs) result.push_back(p.program);
      return result;
   }
   void replay_tree(const isolated_action_tree_result& result, bool expected, int64_t publication_shift_us = 0) {
      const auto original = chain.control->calculate_integrity_hash();
      {
         platform_timer timer; const auto start = fc::time_point::now();
         transaction_context tx(*chain.control, *packed, packed->id(), transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
         tx.block_deadline = start + fc::seconds(5); tx.explicit_billed_cpu_time = true; tx.billed_cpu_time_us = 100;
         tx.init_for_input_trx(packed->get_unprunable_size(), packed->get_prunable_size());
         tx.published += fc::microseconds{publication_shift_us};
         tx.trace->action_traces.emplace_back(*tx.trace, packed->get_transaction().actions[0], console_account, false, 1, 0, 0);
         apply_context context(*chain.control, tx, 1);
         const auto before = chain.control->calculate_integrity_hash();
         const auto prepared = prepare_isolated_transaction(*chain.control, *packed); BOOST_REQUIRE(prepared);
         BOOST_CHECK_EQUAL(try_adopt_action_tree_result(context, prepared->front().code_hash, 0, 0,
            prepared->front().limits, result, 0), expected);
         BOOST_CHECK_EQUAL(tx.trace->action_traces[0].console,
            expected && chain.control->contracts_console() ? "112233" : "");
         chainbase_transaction_state_view live(chain.control->db());
         const auto row = live.read_primary({console_owner_rows, primary_query_kind::find, 7}, 16);
         BOOST_CHECK_EQUAL(bool(row.row), expected);
         if (expected) { BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == result.state.primary_write(0).value); }
         else BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
         tx.undo();
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash() == original);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_console_oracle_tests)

BOOST_AUTO_TEST_CASE(captured_console_configuration_cannot_replay_under_a_different_owner_configuration) {
   for (bool enabled : {false, true}) for (name route : {"run"_n, "sync"_n, "mixed"_n})
      BOOST_TEST_CONTEXT("owner console=" << enabled << " route=" << route) {
      console_owner_fixture f(enabled, route);
      const auto before = f.chain.control->calculate_integrity_hash();
      auto old = f.protocol; old.tree.console_enabled = old.sync.console_enabled = !enabled;
      // The controller's setting has no runtime setter. Simulate a previously
      // captured configuration, without modifying the live controller or DB.
      if (route == "run"_n) {
         isolated_action_tree_executor executor; const auto programs = f.tree_programs();
         const auto stale = executor.execute(f.base, f.roots, programs, old.tree);
         BOOST_REQUIRE(validate_isolated_action_tree_journal(stale, f.roots, programs, old.tree));
         BOOST_REQUIRE_EQUAL(stale.state.writes().size(), 1u); f.replay_tree(stale, false);
         const auto fresh = executor.execute(f.base, f.roots, programs, f.protocol.tree);
         BOOST_REQUIRE(validate_isolated_action_tree_journal(fresh, f.roots, programs, f.protocol.tree)); f.replay_tree(fresh, true);
      } else if (route == "sync"_n) {
         isolated_sync_executor executor;
         const auto stale = executor.execute_transaction(f.base, f.roots, f.programs, old.sync);
         BOOST_REQUIRE(validate_isolated_sync_transaction_journal(stale, f.roots, f.programs, old.sync));
         BOOST_REQUIRE_EQUAL(stale.state.writes().size(), 1u);
         BOOST_CHECK(!validate_isolated_sync_transaction_current_state(*f.chain.control, *f.packed, f.programs, stale));
         const auto fresh = executor.execute_transaction(f.base, f.roots, f.programs, f.protocol.sync);
         BOOST_REQUIRE(validate_isolated_sync_transaction_journal(fresh, f.roots, f.programs, f.protocol.sync));
         BOOST_CHECK(validate_isolated_sync_transaction_current_state(*f.chain.control, *f.packed, f.programs, fresh));
      } else {
         isolated_mixed_tree_executor executor;
         const auto stale = executor.execute(f.base, f.roots, f.programs, old);
         BOOST_REQUIRE(validate_isolated_mixed_tree_journal(stale, f.roots, f.programs, old));
         BOOST_REQUIRE_EQUAL(stale.state.writes().size(), 1u);
         BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*f.chain.control, *f.packed, f.programs, stale));
         const auto fresh = executor.execute(f.base, f.roots, f.programs, f.protocol);
         BOOST_REQUIRE(validate_isolated_mixed_tree_journal(fresh, f.roots, f.programs, f.protocol));
         BOOST_CHECK(validate_isolated_mixed_tree_current_state(*f.chain.control, *f.packed, f.programs, fresh));
      }
      BOOST_CHECK(f.chain.control->calculate_integrity_hash() == before);
   }
}

BOOST_AUTO_TEST_CASE(printed_sync_and_mixed_owner_checkpoint_exceptions_propagate_without_writes_and_allow_reuse) {
   for (bool enabled : {false, true}) for (name route : {"sync"_n, "mixed"_n})
      BOOST_TEST_CONTEXT("owner checkpoint console=" << enabled << " route=" << route) {
      console_owner_fixture f(enabled, route); const auto before = f.chain.control->calculate_integrity_hash();
      const auto inspect = [&](const auto& valid) {
         size_t checkpoints = 0; BOOST_REQUIRE(valid([&] { ++checkpoints; })); BOOST_REQUIRE_GT(checkpoints, 20u);
         for (size_t cutoff = 1; cutoff <= checkpoints; ++cutoff) {
            size_t seen = 0;
            BOOST_CHECK_THROW(valid([&] { if (++seen == cutoff) throw std::runtime_error("console owner cancelled"); }), std::runtime_error);
            BOOST_CHECK_EQUAL(seen, cutoff); seen = 0;
            BOOST_CHECK_THROW(valid([&] { if (++seen == cutoff) throw primary_state_budget_exceeded(); }), primary_state_budget_exceeded);
            BOOST_CHECK_EQUAL(seen, cutoff); BOOST_CHECK(f.chain.control->calculate_integrity_hash() == before);
         }
         BOOST_CHECK(valid({}));
      };
      if (route == "sync"_n) {
         isolated_sync_executor executor;
         const auto result = executor.execute_transaction(f.base, f.roots, f.programs, f.protocol.sync);
         BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
         inspect([&](const std::function<void()>& checkpoint) {
            return validate_isolated_sync_transaction_current_state(*f.chain.control, *f.packed, f.programs, result, checkpoint);
         });
         const auto fresh = executor.execute_transaction(f.base, f.roots, f.programs, f.protocol.sync);
         BOOST_CHECK(validate_isolated_sync_transaction_current_state(*f.chain.control, *f.packed, f.programs, fresh));
      } else {
         isolated_mixed_tree_executor executor;
         const auto result = executor.execute(f.base, f.roots, f.programs, f.protocol);
         BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
         inspect([&](const std::function<void()>& checkpoint) {
            return validate_isolated_mixed_tree_current_state(*f.chain.control, *f.packed, f.programs, result, checkpoint);
         });
         const auto fresh = executor.execute(f.base, f.roots, f.programs, f.protocol);
         BOOST_CHECK(validate_isolated_mixed_tree_current_state(*f.chain.control, *f.packed, f.programs, fresh));
      }
      BOOST_CHECK(f.chain.control->calculate_integrity_hash() == before);
   }
}

BOOST_AUTO_TEST_CASE(original_wasm_console_matches_actual_parallel_production_and_block_validation) {
   console_corpus corpus;
   compare_console_production(corpus, {"run"_n, "sync"_n, "mixed"_n}, [](const auto& trace, bool enabled, name route) {
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route == "mixed"_n ? 4u : 1u);
      const auto& a = trace->action_traces[0];
      BOOST_CHECK_EQUAL(a.console.empty(), !enabled);
      BOOST_REQUIRE_EQUAL(a.call_traces.size(), route == "run"_n ? 0u : 2u);
      if (route != "run"_n) {
         BOOST_REQUIRE_EQUAL(a.console_markers.size(), enabled ? 1u : 0u);
         if (enabled) {
            BOOST_CHECK_GT(a.console_markers[0].value, 12u); // actual bytes, NOT the private event count
            BOOST_CHECK_EQUAL(a.console_markers[0].value + 2, a.console.size()); // final printui(42)
            BOOST_REQUIRE_EQUAL(a.call_traces[0].console_markers.size(), 1u);
            BOOST_CHECK_EQUAL(a.call_traces[0].console_markers[0].value + 2, a.call_traces[0].console.size()); // -4
         }
      }
   });
}

BOOST_AUTO_TEST_CASE(ship_control_proves_debug_console_visibility_and_never_changes_live_elapsed) {
   auto trace = std::make_shared<transaction_trace>();
   trace->elapsed = fc::microseconds{17}; trace->action_traces.emplace_back();
   trace->action_traces[0].elapsed = fc::microseconds{19};
   trace->action_traces[0].console = "first";
   auto different = std::make_shared<transaction_trace>(*trace);
   different->elapsed = fc::microseconds{101}; different->action_traces[0].elapsed = fc::microseconds{103};
   BOOST_CHECK(console_ship_trace(trace, false) == console_ship_trace(different, false));
   BOOST_CHECK(console_ship_trace(trace, true) == console_ship_trace(different, true));
   different->action_traces[0].console = "second";
   BOOST_CHECK(console_ship_trace(trace, false) == console_ship_trace(different, false));
   BOOST_CHECK(console_ship_trace(trace, true) != console_ship_trace(different, true));
   BOOST_CHECK_EQUAL(trace->elapsed.count(), 17);
   BOOST_CHECK_EQUAL(trace->action_traces[0].elapsed.count(), 19);
   BOOST_CHECK_EQUAL(trace->action_traces[0].console, "first");
}

BOOST_AUTO_TEST_CASE(numeric_extremes_match_original_vm_and_ship_in_production_and_validation) {
   const console_numeric_vectors vectors;
   console_corpus corpus({{{console_account, vectors.code, {}}, true}});
   compare_console_production(corpus, {"numbers"_n}, [](const auto& trace, bool enabled, name) {
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
      const auto& a = trace->action_traces[0];
      BOOST_CHECK(a.call_traces.empty()); BOOST_CHECK(a.console_markers.empty());
      BOOST_CHECK_EQUAL(a.console.empty(), !enabled);
      if (enabled) {
         BOOST_CHECK(a.console.find("-9223372036854775808|9223372036854775808|") != std::string::npos);
         BOOST_CHECK(a.console.find("-170141183460469231731687303715884105728|") != std::string::npos);
         BOOST_CHECK(a.console.find("340282366920938463463374607431768211455|") != std::string::npos);
         // Floating text is compared with ORIGINAL VM formatting by the shared
         // oracle, not a test reimplementation of NaN/infinity/quad formatting.
      }
   });
}

BOOST_AUTO_TEST_CASE(negative_sync_and_missing_target_keep_console_prefixes_and_exact_byte_markers) {
   console_corpus corpus(console_negative_graph());
   compare_console_production(corpus, {"sync"_n, "mixed"_n}, [](const auto& trace, bool enabled, name route) {
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route == "mixed"_n ? 4u : 1u);
      const auto& a = trace->action_traces[0];
      BOOST_CHECK_EQUAL(a.console, enabled ? "112223" : "");
      BOOST_REQUIRE_EQUAL(a.call_traces.size(), 3u);
      const std::array<int64_t, 3> codes{-10007, -10042, -1};
      const std::array<const char*, 3> text{"-97", "8", ""};
      for (size_t i = 0; i < codes.size(); ++i) {
         BOOST_REQUIRE(a.call_traces[i].error_id);
         BOOST_CHECK_EQUAL(*a.call_traces[i].error_id, codes[i]);
         BOOST_CHECK_EQUAL(a.call_traces[i].console, enabled ? text[i] : "");
      }
      BOOST_REQUIRE_EQUAL(a.console_markers.size(), enabled ? 2u : 0u);
      BOOST_REQUIRE_EQUAL(a.call_traces[0].console_markers.size(), enabled ? 1u : 0u);
      if (enabled) {
         BOOST_CHECK_EQUAL(a.console_markers[0].value, 2u); // empty print is NOT an extra output byte
         BOOST_CHECK_EQUAL(a.console_markers[1].value, 5u); // missing target still creates a marker
         BOOST_CHECK_EQUAL(a.call_traces[0].console_markers[0].value, 2u);
      }
      if (route == "mixed"_n) {
         BOOST_CHECK_EQUAL(trace->action_traces[1].console, enabled ? "-6" : "");
         BOOST_CHECK_EQUAL(trace->action_traces[2].console, enabled ? "7" : "");
         BOOST_CHECK_EQUAL(trace->action_traces[3].console, enabled ? "7" : "");
         // Trace slots are allocated when inline actions are SENT (ordinary
         // first here), not in execution order. The original executor runs
         // CF before ordinary; receipt global_sequence proves that ordering.
         BOOST_CHECK(!trace->action_traces[2].context_free);
         BOOST_CHECK(trace->action_traces[3].context_free);
         BOOST_REQUIRE(trace->action_traces[2].receipt);
         BOOST_REQUIRE(trace->action_traces[3].receipt);
         BOOST_CHECK_LT(trace->action_traces[3].receipt->global_sequence,
                        trace->action_traces[2].receipt->global_sequence);
      }
   });
}

BOOST_AUTO_TEST_CASE(selected_console_profile_matches_both_canonical_and_private_engines) {
   console_corpus corpus;
   const bool jit = eosio::testing::base_tester::arguments_contains("--eos-vm-jit");
   for (uint16_t workers : {0, 1, 4}) for (bool enabled : {false, true}) {
      fc::temp_directory dir;
      const auto cfg = corpus.configuration(dir, workers, enabled, true);
      BOOST_CHECK(cfg.wasm_runtime == (jit ? wasm_interface::vm_type::eos_vm_jit : wasm_interface::vm_type::eos_vm));
      BOOST_CHECK(cfg.parallel_engine == (jit ? isolated_vm_engine::jit : isolated_vm_engine::interpreter));
      BOOST_CHECK_EQUAL(cfg.parallel_validation_threads, workers);
      BOOST_CHECK_EQUAL(cfg.parallel_production_enabled, workers != 0);
      BOOST_CHECK_EQUAL(cfg.contracts_console, enabled);
   }
}

BOOST_AUTO_TEST_CASE(original_pointer_failures_preserve_console_prefix_undo_writes_and_allow_healthy_reuse) {
   const std::array<std::string, 9> bad{
      "(call $prints (i32.const 65536))", "(call $prints_l (i32.const 65535)(i32.const 2))",
      "(call $printhex (i32.const 0)(i32.const -1))", "(call $printi128 (i32.const 0))",
      "(call $printui128 (i32.const 65521))", "(call $printqf (i32.const 0))",
      "(call $printqf (i32.const 65521))", "(call $prints_l (i32.const -1)(i32.const 0))",
      "(i32.store8 (i32.const 65535)(i32.const 65))(call $prints (i32.const 65535))"};
   const table_key rows{console_account, console_account, "conrows"_n}; const std::array tables{rows};
   const std::array accounts{console_account}; console_corpus corpus;
   for (bool enabled : {false, true}) {
      fc::temp_directory dir;
      console_chain original(corpus.configuration(dir, 0, enabled), corpus.genesis);
      corpus.prefix(original); original.set_node_finalizers(std::array<name, 1>{"eosio"_n}); original.start();
      chainbase_transaction_state_view live(original.control->db()); isolated_action_tree_executor executor;
      uint32_t nonce = 0;
      for (const auto& body : bad) BOOST_TEST_CONTEXT("console=" << enabled << " bad=" << body) {
         const auto store = "(drop (call $store " + cn(console_account) + cn(rows.table) + cn(console_account) +
            "(i64.const " + std::to_string(nonce + 1) + ")(i32.const 16)(i32.const 4)))";
         const auto code = console_program("(if (i64.eq (get_local $action)" + cn("bad"_n) + ")"
            "(then (call $printi (i64.const 55))" + store + body + ")"
            "(else (call $printi (i64.const 77))" + store +
            "(call $prints_l (i32.const 65536)(i32.const 0))(call $printhex (i32.const 0)(i32.const 0))))",
            "", false, "", "(import \"env\" \"db_store_i64\" (func $store (param i64 i64 i64 i64 i32 i32)(result i32)))");
         original.set_code(console_account, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
         const std::array queries{primary_query{rows, primary_query_kind::find, nonce + 1}};
         const auto base = frozen_primary_state::capture_queries(live, tables, queries, {}, {}, accounts);
         const auto before = original.control->calculate_integrity_hash();
         const auto ram_before = original.control->get_resource_limits_manager().get_account_ram_usage(console_account);
         const std::array programs{isolated_receiver_program{console_account, code, {}}};
         const auto context = console_tree_context(enabled);
         std::array roots{console_input("bad"_n)}; roots[0].data = fc::raw::pack(++nonce);
         const auto push = [&](const isolated_action_input& input) {
            signed_transaction trx;
            trx.actions.push_back(action{{{console_account, "active"_n}}, console_account, input.action, input.data});
            original.set_transaction_headers(trx);
            trx.sign(original.get_private_key(console_account, "active"), original.control->get_chain_id());
            return original.push_transaction(trx, fc::time_point::maximum(),
               eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US, true);
         };
         BOOST_CHECK_EXCEPTION(executor.execute(base, roots, programs, context), primary_state_unsupported,
            [](const auto& e) { return dynamic_cast<const primary_state_missing*>(&e) == nullptr; });
         const auto failure = push(roots[0]); BOOST_REQUIRE(failure && failure->except);
         BOOST_REQUIRE_EQUAL(failure->action_traces.size(), 1u);
         BOOST_CHECK_EQUAL(failure->action_traces[0].console, enabled ? "55" : "");
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
         BOOST_CHECK_EQUAL(original.control->get_resource_limits_manager().get_account_ram_usage(console_account), ram_before);
         BOOST_CHECK(!live.read_primary({rows, primary_query_kind::find, nonce}, 16).row);
         roots[0].action = "good"_n;
         const auto candidate = executor.execute(base, roots, programs, context);
         BOOST_REQUIRE(validate_isolated_action_tree_journal(candidate, roots, programs, context));
         BOOST_REQUIRE_EQUAL(candidate.state.writes().size(), 1u);
         const auto healthy = push(roots[0]); BOOST_REQUIRE(healthy && !healthy->except);
         BOOST_REQUIRE_EQUAL(healthy->action_traces.size(), 1u);
         BOOST_CHECK_EQUAL(healthy->action_traces[0].console, enabled ? "77" : "");
         const auto row = live.read_primary({rows, primary_query_kind::find, nonce}, 16);
         BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == candidate.state.primary_write(0).value);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
const std::string exit_import = R"((import "env" "eosio_exit" (func $exit (param i32))))";
const std::string exit_out_import = R"((import "env" "set_action_return_value" (func $out (param i32 i32))))";
const std::string exit_set_import = R"((import "env" "set_call_return_value" (func $set (param i32 i32))))";
const std::string exit_assert_import = R"((import "env" "eosio_assert_code" (func $assert (param i32 i64))))";
const std::vector<char> exit_bytes = fc::raw::pack(uint64_t{0x1122334455667788});
const std::string exit_data = "(data (i32.const 512)\"" + console_bytes(exit_bytes) + "\")";
constexpr table_key exit_rows{console_account, console_account, "exitrows"_n};
std::string exit_then_unreachable(int32_t code) {
   return "(call $exit (i32.const " + std::to_string(code) + "))(unreachable)";
}
std::vector<char> exit_primitive_program(int32_t code, bool in_start, bool store_first = false) {
   std::string imports = exit_import + exit_out_import;
   std::string body = "(call $printi (i64.const 11))";
   if (store_first) {
      imports += R"((import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
         (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32))))";
      body += "(drop (call $read (i32.const 0)(i32.const 4)))"
         "(drop (call $store " + cn(console_account) + cn(exit_rows.table) + cn(console_account) +
         "(i64.extend_u/i32 (i32.load (i32.const 0)))(i32.const 512)(i32.const 8)))";
   }
   body += "(call $out (i32.const 512)(i32.const 8))" + exit_then_unreachable(code);
   return console_program(in_start ? "(unreachable)" : body, "(unreachable)", false,
      exit_data + (in_start ? "(func $early " + body + ")(start $early)" : ""), imports);
}
std::vector<isolated_sync_program> exit_sync_graph(int32_t code, bool in_start) {
   const auto root = console_program("(call $printi (i64.const 11))"
      "(call $assert (i64.eq (call $call " + cn(console_child) +
      "(i64.const 0)(i32.const 0)(i32.const 0))(i64.const 8))(i64.const 8101))"
      "(call $assert (i32.eq (call $getreturn (i32.const 512)(i32.const 8))(i32.const 8))(i64.const 8102))"
      "(call $out (i32.const 512)(i32.const 8))(call $printi (i64.const 42))", "(unreachable)", false, "",
      exit_out_import + exit_assert_import +
      R"((import "env" "get_call_return_value" (func $getreturn (param i32 i32) (result i32))))");
   const auto leave = "(call $printi (i64.const 5))(call $set (i32.const 512)(i32.const 8))" + exit_then_unreachable(code);
   const auto child = console_program("(unreachable)", in_start ? "(unreachable)" : leave, false,
      exit_data + (in_start ? "(func $early " + leave + ")(start $early)" : ""), exit_import + exit_set_import);
   return {{{console_account, root, {}}, true}, {{console_child, child, {}}, true}};
}
std::vector<isolated_sync_program> exit_queued_graph() {
   const auto normal = fc::raw::pack(action{{{console_account, "active"_n}}, console_account, "quiet"_n, std::vector<char>{}});
   const auto pure = fc::raw::pack(action{{}, console_account, "quiet"_n, std::vector<char>{}});
   const auto root = console_program(
      "(if (i64.eq (get_local $action)" + cn("quiet"_n) + ")(then (call $printi (i64.const 7))" + exit_then_unreachable(0) + "))"
      "(call $printi (i64.const 11))"
      "(if (i64.eq (get_local $action)" + cn("mixed"_n) + ")(then"
      "(call $assert (i64.eq (call $call " + cn(console_child) +
      "(i64.const 0)(i32.const 0)(i32.const 0))(i64.const 8))(i64.const 8103))"
      "(call $printi (i64.const 22))))"
      "(call $notify " + cn(console_child) + ")"
      "(call $inline (i32.const 1024)(i32.const " + std::to_string(normal.size()) + "))"
      "(call $cf (i32.const 2048)(i32.const " + std::to_string(pure.size()) + "))"
      "(call $out (i32.const 512)(i32.const 8))" + exit_then_unreachable(-3), "(unreachable)", false,
      exit_data + "(data (i32.const 1024)\"" + console_bytes(normal) + "\")"
      "(data (i32.const 2048)\"" + console_bytes(pure) + "\")", exit_import + exit_out_import + exit_assert_import);
   const auto child = console_program("(call $printi (i64.const 3))" + exit_then_unreachable(7),
      "(call $printi (i64.const 5))(call $set (i32.const 512)(i32.const 8))" + exit_then_unreachable(-7),
      false, exit_data, exit_import + exit_set_import);
   return {{{console_account, root, {}}, true}, {{console_child, child, {}}, true}};
}

std::vector<isolated_sync_program> exit_recursive_graph(bool read_only) {
   const auto invoke = "(call $assert (i64.eq (call $call " + cn(console_account) +
      "(i64.const " + std::to_string(read_only ? 1 : 0) + ")(i32.const 1024)(i32.const 4))(i64.const 8))(i64.const 8201))";
   const auto code = console_program(invoke + "(call $out (i32.const 512)(i32.const 8))(call $printi (i64.const 99))" +
      exit_then_unreachable(-1),
      "(local $depth i32)(drop (call $getdata (i32.const 1024)(i32.const 4)))"
      "(set_local $depth (i32.load (i32.const 1024)))"
      "(if (i32.lt_u (get_local $depth)(i32.const 3))(then"
      "(i32.store (i32.const 1024)(i32.add (get_local $depth)(i32.const 1)))" + invoke + "))"
      "(call $printi (i64.extend_u/i32 (get_local $depth)))(call $set (i32.const 512)(i32.const 8))" +
      exit_then_unreachable(INT32_MIN), false, exit_data,
      exit_import + exit_out_import + exit_set_import + exit_assert_import +
      R"((import "env" "get_call_data" (func $getdata (param i32 i32)(result i32))))");
   return {{{console_account, code, {}}, true}};
}

// A successful child exit must NOT swallow a later parent trap, a forbidden
// queued CF operation, or final RAM failure. A read-only child must also reject
// its forbidden write BEFORE reaching exit. All failed transactions have a
// real parent write, so the undo assertions cannot pass vacuously.
std::vector<isolated_sync_program> exit_failure_graph(bool read_only_write) {
   const auto cf = fc::raw::pack(action{{}, console_account, "forbidden"_n, std::vector<char>{}});
   const std::string store_import = R"((import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32)(result i32))))";
   const auto store = [](name receiver) {
      return "(drop (call $store " + cn(receiver) + cn(exit_rows.table) + cn(receiver) +
         "(i64.const 7)(i32.const 512)(i32.const 8)))";
   };
   const auto root = console_program(
      "(if (i64.eq (get_local $action)" + cn("forbidden"_n) + ")(then"
      "(call $out (i32.const 512)(i32.const 8))" + exit_then_unreachable(0) + "))"
      "(call $printi (i64.const 11))" + store(console_account) +
      "(call $assert (i64.eq (call $call " + cn(console_child) +
      (read_only_write ? "(select (i64.const 1)(i64.const 0)(i64.eq (get_local $action)" + cn("readonly"_n) + "))" : "(i64.const 0)") +
      "(i32.const 0)(i32.const 0))(i64.const 8))(i64.const 8202))"
      "(call $printi (i64.const 22))"
      "(if (i64.eq (get_local $action)" + cn("trap"_n) + ")(then (unreachable)))"
      "(if (i64.eq (get_local $action)" + cn("badcf"_n) + ")(then"
      "(call $cf (i32.const 2048)(i32.const " + std::to_string(cf.size()) + "))))"
      "(call $out (i32.const 512)(i32.const 8))" + exit_then_unreachable(-1), "", false,
      exit_data + "(data (i32.const 2048)\"" + console_bytes(cf) + "\")", exit_import + exit_out_import + exit_assert_import + store_import);
   const auto child = console_program("", "(call $printi (i64.const 5))" +
      (read_only_write ? store(console_child) : "") +
      "(call $set (i32.const 512)(i32.const 8))" + exit_then_unreachable(3), false, exit_data,
      exit_import + exit_set_import + store_import);
   return {{{console_account, root, {}}, true}, {{console_child, child, {}}, true}};
}

std::vector<char> exit_failed_trace(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE(trace && trace->except && !trace->receipt);
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->except->code()),
      fc::raw::pack(trace->error_code), fc::raw::pack(trace->account_ram_delta)};
   for (const auto& a : trace->action_traces) {
      fields.push_back(fc::raw::pack(a.action_ordinal)); fields.push_back(fc::raw::pack(a.creator_action_ordinal));
      fields.push_back(fc::raw::pack(a.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(a.receiver)); fields.push_back(fc::raw::pack(a.act));
      fields.push_back(fc::raw::pack(a.receipt)); fields.push_back(fc::raw::pack(a.context_free));
      fields.push_back(fc::raw::pack(a.error_code)); fields.push_back(fc::raw::pack(a.except ? a.except->code() : 0));
      fields.push_back(a.return_value); fields.push_back(fc::raw::pack(a.account_ram_deltas));
      fields.push_back(fc::raw::pack(a.console)); fields.push_back(fc::raw::pack(a.console_markers));
      for (const auto& c : a.call_traces) {
         fields.push_back(fc::raw::pack(c.call_ordinal)); fields.push_back(fc::raw::pack(c.sender_ordinal));
         fields.push_back(fc::raw::pack(c.receiver)); fields.push_back(fc::raw::pack(c.read_only));
         fields.push_back(c.data); fields.push_back(c.return_value); fields.push_back(fc::raw::pack(c.error_id));
         fields.push_back(fc::raw::pack(c.error_code)); fields.push_back(fc::raw::pack(c.except ? c.except->code() : 0));
         fields.push_back(fc::raw::pack(c.console)); fields.push_back(fc::raw::pack(c.console_markers));
      }
   }
   fields.push_back(console_ship_trace(trace, false)); fields.push_back(console_ship_trace(trace, true));
   return fc::raw::pack(fields);
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_exit_private_tests)
BOOST_FIXTURE_TEST_CASE(no_linear_memory_is_needed_and_incompatible_import_types_remain_rejected, console_fixture) {
   const auto compile = [](const std::string& text) {
      const auto bytes = wast_to_wasm(text);
      return std::vector<char>(bytes.begin(), bytes.end());
   };
   const auto good = compile("(module " + exit_import +
      "(func (export \"apply\") (param i64 i64 i64)" + exit_then_unreachable(-1) + "))");
   isolated_wasm_module machine(good);
   for (const std::string signature : {"", "(param f64)", "(param i32)(result i32)"}) {
      const auto wrong = compile("(module (import \"env\" \"eosio_exit\" (func " + signature +
         "))(func (export \"apply\") (param i64 i64 i64)))");
      BOOST_CHECK_THROW(isolated_wasm_module{wrong}, eosio::vm::wasm_link_exception);
      BOOST_CHECK(machine.execute(base, console_input()).state.writes().empty());
   }
}
BOOST_FIXTURE_TEST_CASE(normal_and_start_exit_preserve_prefix_and_skip_unreachable_entry_or_tail, console_fixture) {
   const std::array tables{exit_rows};
   const std::array accounts{console_account, console_child};
   const auto captured = frozen_primary_state::capture_queries(view, tables, {}, {}, {}, accounts);
   isolated_action_tree_executor executor;
   auto input = console_input(); input.data = fc::raw::pack(uint32_t{1});
   const std::array roots{input};
   for (int32_t code : {0, 1, -1, INT32_MIN, INT32_MAX}) for (bool start : {false, true})
      for (bool enabled : {false, true}) for (unsigned repeat = 0; repeat < 2; ++repeat) {
         BOOST_TEST_CONTEXT("code=" << code << " start=" << start << " console=" << enabled << " repeat=" << repeat) {
            const std::array programs{isolated_receiver_program{console_account, exit_primitive_program(code, start, true), {}}};
            const auto context = console_tree_context(enabled);
            const auto result = executor.execute(captured, roots, programs, context);
            BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, programs, context));
            BOOST_REQUIRE_EQUAL(result.steps.size(), 1u);
            BOOST_CHECK(result.steps[0].effects.return_value == exit_bytes);
            if (enabled) {
               BOOST_REQUIRE(!result.steps[0].events.empty());
               BOOST_CHECK(console_event(result.steps[0].events, 0).operation == console_op::printi);
               BOOST_CHECK_EQUAL(console_event(result.steps[0].events, 0).bits[0], 11u);
            }
            BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
            BOOST_CHECK(result.state.primary_write(0).value == exit_bytes);
            BOOST_CHECK(!view.read_table(exit_rows)); // private prefix never commits here
         }
      }
}
BOOST_FIXTURE_TEST_CASE(sync_exit_retains_child_return_and_parent_continues_including_start_exit, console_fixture) {
   isolated_sync_executor executor;
   const auto input = console_input();
   for (int32_t code : {0, 1, -1, INT32_MIN, INT32_MAX}) for (bool start : {false, true})
      for (bool enabled : {false, true}) for (unsigned repeat = 0; repeat < 2; ++repeat) {
         const auto graph = exit_sync_graph(code, start);
         const auto context = console_sync_context(enabled);
         const auto result = executor.execute(base, input, graph, context);
         BOOST_REQUIRE(validate_isolated_sync_journal(result, input, graph, context));
         BOOST_REQUIRE_EQUAL(result.calls.size(), 1u);
         BOOST_CHECK(result.calls[0].return_value == exit_bytes);
         BOOST_CHECK(result.root.return_value == exit_bytes);
         BOOST_CHECK(result.state.writes().empty());
      }
}
BOOST_FIXTURE_TEST_CASE(root_exit_keeps_notification_cf_inline_and_later_root_queues, console_fixture) {
   isolated_mixed_tree_executor executor;
   const auto graph = exit_queued_graph();
   const std::array roots{console_input("mixed"_n), console_input("tree"_n)};
   for (bool enabled : {false, true}) for (unsigned repeat = 0; repeat < 3; ++repeat) {
      const isolated_mixed_tree_context context{console_tree_context(enabled), console_sync_context(enabled)};
      const auto result = executor.execute(base, roots, graph, context);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result, roots, graph, context));
      BOOST_REQUIRE_EQUAL(result.steps.size(), 8u);
      BOOST_REQUIRE_EQUAL(result.steps[0].calls.size(), 1u);
      BOOST_CHECK(result.steps[0].calls[0].return_value == exit_bytes);
      BOOST_CHECK(result.steps[4].calls.empty());
      for (size_t root : {0u, 4u}) {
         BOOST_CHECK(result.steps[root].effects.return_value == exit_bytes);
         BOOST_CHECK(result.steps[root + 1].receiver == console_child);
         BOOST_CHECK(result.groups[result.steps[root + 2].group_index].context_free);
         BOOST_CHECK(!result.groups[result.steps[root + 3].group_index].context_free);
      }
      BOOST_CHECK(result.state.writes().empty());
   }
}

BOOST_FIXTURE_TEST_CASE(recursive_exits_preserve_independent_frames_and_read_only_inheritance, console_fixture) {
   isolated_sync_executor executor;
   for (bool read_only : {false, true}) for (bool enabled : {false, true}) for (unsigned repeat = 0; repeat < 3; ++repeat) {
      const auto graph = exit_recursive_graph(read_only); const auto context = console_sync_context(enabled);
      const auto input = console_input();
      const auto result = executor.execute(base, input, graph, context);
      BOOST_REQUIRE(validate_isolated_sync_journal(result, input, graph, context));
      BOOST_REQUIRE_EQUAL(result.calls.size(), 4u);
      BOOST_CHECK(result.root.return_value == exit_bytes);
      for (const auto& call : result.calls) {
         BOOST_CHECK_EQUAL(call.read_only, read_only);
         BOOST_CHECK(call.return_value == exit_bytes);
      }
      BOOST_CHECK(result.state.writes().empty());
   }
}

BOOST_FIXTURE_TEST_CASE(traps_and_cancellation_remain_failures_and_do_not_poison_exit_module_reuse, console_fixture) {
   const auto bytes = wast_to_wasm("(module " + exit_import +
      "(func (export \"apply\") (param i64 i64)(param $action i64)"
      "(if (i64.eq (get_local $action)" + cn("trap"_n) + ")(then (unreachable)))"
      "(if (i64.eq (get_local $action)" + cn("loop"_n) + ")(then (loop $forever (br $forever))))" +
      exit_then_unreachable(0) + "))");
   isolated_wasm_module module(std::vector<char>{bytes.begin(), bytes.end()});
   const auto healthy = [&] { BOOST_CHECK(module.execute(base, console_input()).state.writes().empty()); };
   std::stop_source stop; stop.request_stop();
   BOOST_CHECK_THROW(module.execute(base, console_input(), stop.get_token()), isolated_execution_interrupted); healthy();
   auto generation = std::make_shared<std::atomic<uint64_t>>(1);
   isolated_cancel cancelled; cancelled.generation = generation;
   BOOST_CHECK_THROW(module.execute(base, console_input(), {}, {}, cancelled), isolated_execution_interrupted); healthy();
   isolated_cancel expired; expired.deadline = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(module.execute(base, console_input(), {}, {}, expired), isolated_execution_interrupted); healthy();
   BOOST_CHECK_THROW(module.execute(base, console_input("trap"_n)), eosio::vm::exception); healthy();
   // The original 500 ms watchdog, not a timing assertion or a relaxed budget.
   BOOST_CHECK_THROW(module.execute(base, console_input("loop"_n)), eosio::vm::timeout_exception); healthy();
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_exit_oracle_tests)
BOOST_AUTO_TEST_CASE(ordinary_and_start_exit_codes_match_original_vm_and_actual_adoption) {
   for (int32_t code : {0, 1, -1, INT32_MIN, INT32_MAX}) for (bool start : {false, true}) {
      console_corpus corpus(std::vector<isolated_sync_program>{{{console_account, exit_primitive_program(code, start), {}}, true}});
      compare_console_production(corpus, {"run"_n}, [&](const auto& trace, bool enabled, name) {
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         BOOST_CHECK(trace->action_traces[0].return_value == exit_bytes);
         BOOST_CHECK_EQUAL(trace->action_traces[0].console, enabled ? "11" : "");
      });
   }
}
BOOST_AUTO_TEST_CASE(sync_exit_codes_and_start_keep_parent_continuation_in_original_vm_and_adoption) {
   for (int32_t code : {0, 1, -1, INT32_MIN, INT32_MAX}) for (bool start : {false, true}) {
      console_corpus corpus(exit_sync_graph(code, start));
      compare_console_production(corpus, {"run"_n}, [&](const auto& trace, bool enabled, name) {
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         const auto& root = trace->action_traces[0];
         BOOST_CHECK(root.return_value == exit_bytes);
         BOOST_CHECK_EQUAL(root.console, enabled ? "1142" : "");
         BOOST_REQUIRE_EQUAL(root.call_traces.size(), 1u);
         BOOST_CHECK(root.call_traces[0].return_value == exit_bytes);
         BOOST_CHECK_EQUAL(root.call_traces[0].console, enabled ? "5" : "");
      });
   }
}
BOOST_AUTO_TEST_CASE(queued_notifications_cf_inline_survive_root_exit_in_original_vm_and_adoption) {
   console_corpus corpus(exit_queued_graph());
   compare_console_production(corpus, {"tree"_n, "mixed"_n}, [&](const auto& trace, bool enabled, name route) {
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 4u);
      BOOST_CHECK(trace->action_traces[0].return_value == exit_bytes);
      BOOST_CHECK_EQUAL(trace->action_traces[0].console, enabled ? (route == "tree"_n ? "11" : "1122") : "");
      // Do not assume vector indices are execution order; find CF by its flag.
      const auto pure = std::find_if(trace->action_traces.begin(), trace->action_traces.end(), [](const auto& a) { return a.context_free; });
      BOOST_REQUIRE(pure != trace->action_traces.end());
      BOOST_CHECK_EQUAL(pure->console, enabled ? "7" : "");
      for (const auto& a : trace->action_traces) {
         BOOST_CHECK(!a.except && !a.error_code);
         if (a.receiver == console_child) BOOST_CHECK_EQUAL(a.console, enabled ? "3" : "");
      }
   });
}

BOOST_AUTO_TEST_CASE(writes_before_ordinary_and_start_exit_commit_with_original_ram_receipts_and_ledger) {
   for (bool start : {false, true}) {
      console_corpus corpus(std::vector<isolated_sync_program>{{{console_account, exit_primitive_program(-1, start, true), {}}, true}});
      compare_console_production(corpus, {"run"_n}, [](const auto& trace, bool enabled, name) {
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         const auto& root = trace->action_traces[0];
         BOOST_CHECK(root.return_value == exit_bytes); BOOST_CHECK_EQUAL(root.console, enabled ? "11" : "");
         BOOST_REQUIRE(!root.account_ram_deltas.empty());
      }, 1); // Each new row changes table count; do not require two old snapshots to remain independent.
   }
}

BOOST_AUTO_TEST_CASE(recursive_and_read_only_exit_match_original_calls_and_actual_adoption) {
   for (bool read_only : {false, true}) {
      console_corpus corpus(exit_recursive_graph(read_only));
      compare_console_production(corpus, {"run"_n}, [&](const auto& trace, bool enabled, name) {
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
         const auto& root = trace->action_traces[0];
         BOOST_CHECK(root.return_value == exit_bytes); BOOST_CHECK_EQUAL(root.console, enabled ? "99" : "");
         BOOST_REQUIRE_EQUAL(root.call_traces.size(), 4u);
         for (size_t i = 0; i < root.call_traces.size(); ++i) {
            BOOST_CHECK_EQUAL(root.call_traces[i].read_only, read_only);
            BOOST_CHECK(root.call_traces[i].return_value == exit_bytes);
            BOOST_CHECK_EQUAL(root.call_traces[i].console, enabled ? std::to_string(i) : "");
         }
      });
   }
}

BOOST_AUTO_TEST_CASE(child_and_root_exit_do_not_hide_late_trap_cf_read_only_or_final_ram_failures) {
   for (unsigned fault = 0; fault < 4; ++fault) {
      console_corpus corpus(exit_failure_graph(fault == 3));
      for (bool enabled : {false, true}) for (uint16_t workers : {0, 1, 2, 4})
         BOOST_TEST_CONTEXT("exit fault=" << fault << " console=" << enabled << " workers=" << workers) {
         fc::temp_directory pd, sd, vd;
         console_chain producer(corpus.configuration(pd, workers, enabled, true), corpus.genesis),
            serial(corpus.configuration(sd, 0, enabled), corpus.genesis),
            parallel(corpus.configuration(vd, workers, enabled), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         const auto set_ram = [&](int64_t bytes) {
            BOOST_REQUIRE(!producer.push_action("eosio"_n, "setalimits"_n, "eosio"_n, fc::mutable_variant_object()
               ("account", "test")("ram_bytes", bytes)("net_weight", -1)("cpu_weight", -1))->except);
            const auto block = producer.produce_block(); producer.control->abort_block();
            serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
            producer.start();
         };
         if (fault == 2) set_ram(producer.control->get_resource_limits_manager().get_account_ram_usage(console_account));
         serial.start();
         const auto metadata = [&](name route) {
            signed_transaction trx;
            trx.actions.emplace_back(std::vector<permission_level>{{console_account, "active"_n}}, console_account, route, std::vector<char>{});
            producer.set_transaction_headers(trx);
            trx.sign(producer.get_private_key(console_account, "active"), producer.control->get_chain_id());
            return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
         };
         const auto push = [](auto& chain, const auto& tx) {
            return chain.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
         };
         const std::array routes{"trap"_n, "badcf"_n, "run"_n, "readonly"_n};
         const auto bad = metadata(routes[fault]);
         const auto before = producer.control->calculate_integrity_hash(), sb = serial.control->calculate_integrity_hash();
         const std::array accounts{console_account, console_child}; std::array<int64_t, 2> ram{};
         for (size_t i = 0; i < accounts.size(); ++i) ram[i] = producer.control->get_resource_limits_manager().get_account_ram_usage(accounts[i]);
         const auto reference = push(serial, bad); BOOST_REQUIRE(reference && reference->except);
         const auto stats = producer.control->get_parallel_production_stats(); transaction_trace_ptr failed;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({bad}, fc::time_point::maximum(), [&](const auto& tx) {
            failed = push(producer, tx); return true;
         }), 1u);
         BOOST_REQUIRE(failed && failed->except && !failed->receipt);
         BOOST_CHECK(exit_failed_trace(failed) == exit_failed_trace(reference));
         BOOST_CHECK(producer.control->calculate_integrity_hash() == before);
         BOOST_CHECK(serial.control->calculate_integrity_hash() == sb);
         BOOST_CHECK(!producer.control->is_known_unexpired_transaction(bad->id()));
         chainbase_transaction_state_view live(producer.control->db());
         BOOST_CHECK(!live.read_table(exit_rows));
         for (size_t i = 0; i < accounts.size(); ++i)
            BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(accounts[i]), ram[i]);
         BOOST_REQUIRE(!failed->action_traces.empty()); BOOST_CHECK_EQUAL(failed->action_traces[0].call_traces.size(), 1u);
         if (fault == 2) {
            BOOST_CHECK_EQUAL(failed->except->code(), ram_usage_exceeded::code_value);
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_CHECK_EQUAL(after.adopted - stats.adopted, workers ? 1u : 0u); // actual replay before final RAM rejection
         }
         serial.control->abort_block();
         if (fault == 2) set_ram(-1);
         const auto good = metadata("healthy"_n);
         const auto ready = producer.control->get_parallel_production_stats(); transaction_trace_ptr accepted;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({good}, fc::time_point::maximum(), [&](const auto& tx) {
            accepted = push(producer, tx); return true;
         }), 1u);
         BOOST_REQUIRE(accepted && !accepted->except && accepted->receipt);
         BOOST_CHECK_EQUAL(producer.control->get_parallel_production_stats().adopted - ready.adopted, workers ? 1u : 0u);
         const auto row = live.read_primary({exit_rows, primary_query_kind::find, 7}, 16);
         BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == exit_bytes);
         std::vector<char> original, candidate;
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == good->id()) original = console_trace(std::get<0>(event));
         });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == good->id()) candidate = console_trace(std::get<0>(event));
         });
         const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(original == console_trace(accepted)); BOOST_CHECK(candidate == original);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
      }
   }
}
BOOST_AUTO_TEST_SUITE_END()

// Fixed transaction/chain environment hosts: original VM and private comparisons.
namespace {
const std::string environment_imports = R"(
   (import "env" "current_time" (func $now (result i64)))
   (import "env" "publication_time" (func $published (result i64)))
   (import "env" "transaction_size" (func $size (result i32)))
   (import "env" "expiration" (func $expiration (result i32)))
   (import "env" "tapos_block_num" (func $tapos (result i32)))
   (import "env" "tapos_block_prefix" (func $prefix (result i32)))
   (import "env" "get_block_num" (func $block (result i32)))
   (import "env" "read_transaction" (func $transaction (param i32 i32)(result i32)))
   (import "env" "get_action" (func $action (param i32 i32 i32 i32)(result i32)))
   (import "env" "memset" (func $fill (param i32 i32 i32)(result i32)))
   (import "env" "set_action_return_value" (func $return (param i32 i32)))
   (import "env" "set_call_return_value" (func $call_return (param i32 i32))))";

// One bounded observation contains all scalar values, original transaction
// bytes and the ORIGINAL TOP-LEVEL action, even from a notification/inline.
// Synchronous children cannot call get_action; CF cannot read now/published
// or set_action_return_value. The disabled bytes retain their sentinels.
std::string environment_observe(bool synchronous, bool cf, bool print) {
   std::string body = "(drop (call $fill (i32.const 8192)(i32.const 63)(i32.const 256)))";
   if (!cf) body += "(i64.store (i32.const 8192)(call $now))(i64.store (i32.const 8200)(call $published))";
   const std::array<const char*, 5> scalars{"size", "expiration", "tapos", "prefix", "block"};
   for (size_t i = 0; i < scalars.size(); ++i)
      body += "(i32.store (i32.const " + std::to_string(8208 + i * 4) + ")(call $" + scalars[i] + "))";
   body += "(i32.store (i32.const 8228)(call $transaction (i32.const 8256)(i32.const 128)))";
   if (!synchronous) body += "(i32.store (i32.const 8232)(call $action (i32.const 1)(i32.const 0)(i32.const 8384)(i32.const 64)))";
   if (print) body += "(call $printhex (i32.const 8192)(i32.const 256))";
   if (!cf) body += std::string("(call $") + (synchronous ? "call_return" : "return") + "(i32.const 8192)(i32.const 256))";
   return body;
}
std::vector<char> expected_environment_observation(const isolated_transaction_environment& environment,
      bool synchronous = false, bool cf = false) {
   std::vector<char> result(256, '?');
   const auto put = [&](size_t offset, uint64_t bits, size_t size) {
      for (size_t byte = 0; byte < size; ++byte) result[offset + byte] = char(bits >> (byte * 8));
   };
   if (!cf) { put(0, environment.current_time(), 8); put(8, environment.publication_time(), 8); }
   put(16, environment.transaction_size(), 4); put(20, environment.expiration(), 4);
   put(24, environment.tapos_block_num(), 4); put(28, environment.tapos_block_prefix(), 4);
   put(32, environment.block_num(), 4);
   put(36, environment.read_transaction(std::span<char>(result).subspan(64, 128)), 4);
   if (!synchronous) put(40, environment.get_action(1, 0, std::span<char>(result).subspan(192, 64)), 4);
   return result;
}
std::shared_ptr<const isolated_transaction_environment> environment_for(const isolated_action_input& input,
      size_t action_data_bytes = 0, uint64_t number = 1) {
   transaction trx;
   trx.expiration = fc::time_point_sec{UINT32_MAX}; trx.ref_block_num = UINT16_MAX; trx.ref_block_prefix = UINT32_MAX;
   action act; act.account = input.account; act.name = input.action;
   for (const auto& [actor, permission] : input.authorizations) act.authorization.push_back({actor, permission});
   act.data = input.data; act.data.resize(action_data_bytes, 'x'); trx.actions.push_back(std::move(act));
   trx.context_free_actions.push_back(action{{}, console_leaf, "pure"_n, std::vector<char>{'c'}});
   return isolated_transaction_environment::capture(trx, UINT64_MAX - number, number + 7, uint32_t(number));
}
std::vector<isolated_sync_program> environment_graph() {
   const auto normal = fc::raw::pack(action{{{console_account, "active"_n}}, console_account, "nested"_n, std::vector<char>{}});
   const auto cf = fc::raw::pack(action{{}, console_account, "pure"_n, std::vector<char>{}});
   const auto when = [](name method, const std::string& body) {
      return "(if (i64.eq (get_local $action)" + cn(method) + ")(then " + body + "))";
   };
   const auto root = console_program(
      "(if (i64.eq (get_local $action)" + cn("pure"_n) + ")(then " + environment_observe(false, true, true) +
      ")(else " + environment_observe(false, false, true) + "))" +
      when("sync"_n, console_call(console_child)) + when("nested"_n, console_call(console_leaf)) +
      when("mixed"_n, console_call(console_child) + "(call $notify " + cn(console_child) +
         ")(call $inline (i32.const 1024)(i32.const " + std::to_string(normal.size()) +
         "))(call $cf (i32.const 2048)(i32.const " + std::to_string(cf.size()) + "))"),
      environment_observe(true, false, true), false,
      "(data (i32.const 1024)\"" + console_bytes(normal) + "\")(data (i32.const 2048)\"" + console_bytes(cf) + "\")",
      environment_imports);
   return {{{console_account, root, {}}, true},
      {{console_child, console_program(environment_observe(false, false, true) + console_call(console_leaf),
         environment_observe(true, false, true) + console_call(console_leaf), false, "", environment_imports), {}}, true},
      {{console_leaf, console_program(environment_observe(false, false, true),
         environment_observe(true, false, true), false, "", environment_imports), {}}, true}};
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_environment_private_tests)

BOOST_FIXTURE_TEST_CASE(all_nine_hosts_use_owned_bytes_and_missing_context_never_returns_a_prefix, console_fixture) {
   for (bool start : {false, true}) {
      const auto observation = environment_observe(false, false, false);
      const auto code = console_program(start ? "" : observation, "", false,
         start ? "(func $environment_start " + observation + ")(start $environment_start)" : "", environment_imports);
      isolated_wasm_module module(code);
      auto root = console_input();
      BOOST_CHECK_THROW(module.execute(base, root), isolated_transaction_environment_missing);
      for (size_t bytes : {0u, 1u, 127u, 128u}) for (uint64_t number : {1u, 7u}) {
         root.environment = environment_for(root, bytes, number);
         const auto result = module.execute(base, root);
         BOOST_CHECK(result.return_value == expected_environment_observation(*root.environment));
         BOOST_CHECK(result.state.writes().empty());
         BOOST_CHECK(result.execution_fingerprint == isolated_execution_fingerprint(fc::sha256::hash(code.data(), code.size()), {}, root));
      }
      root.environment.reset();
      BOOST_CHECK_THROW(module.execute(base, root), isolated_transaction_environment_missing);
      root.environment = environment_for(root);
      BOOST_CHECK(module.execute(base, root).return_value == expected_environment_observation(*root.environment));
   }
}

BOOST_FIXTURE_TEST_CASE(sync_inline_notification_and_cf_inherit_the_same_environment_and_proof_rejects_substitution, console_fixture) {
   const auto programs = environment_graph();
   isolated_sync_executor sync_executor; isolated_mixed_tree_executor mixed_executor;
   for (bool enabled : {false, true}) {
      std::array roots{console_input("sync"_n)}; roots[0].environment = environment_for(roots[0]);
      const auto sync = console_sync_context(enabled);
      const auto result = sync_executor.execute_transaction(base, roots, programs, sync);
      BOOST_REQUIRE(validate_isolated_sync_transaction_journal(result, roots, programs, sync));
      BOOST_REQUIRE_EQUAL(result.roots.size(), 1u); BOOST_REQUIRE_EQUAL(result.roots[0].calls.size(), 2u);
      BOOST_CHECK(result.roots[0].root.return_value == expected_environment_observation(*roots[0].environment));
      for (const auto& child : result.roots[0].calls)
         BOOST_CHECK(child.return_value == expected_environment_observation(*roots[0].environment, true));
      auto forged = result; forged.environment = environment_for(roots[0], 0, 2);
      BOOST_CHECK(!validate_isolated_sync_transaction_journal(forged, roots, programs, sync));
      roots[0] = console_input("mixed"_n); roots[0].environment = environment_for(roots[0]);
      const isolated_mixed_tree_context context{console_tree_context(enabled), sync};
      const auto mixed = mixed_executor.execute(base, roots, programs, context);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(mixed, roots, programs, context));
      BOOST_REQUIRE_EQUAL(mixed.steps.size(), 4u);
      for (const auto& group : mixed.groups) BOOST_CHECK(same_isolated_environment(group.input.environment, roots[0].environment));
      for (const auto& step : mixed.steps) {
         if (!mixed.groups[step.group_index].context_free)
            BOOST_CHECK(step.effects.return_value == expected_environment_observation(*roots[0].environment));
         else BOOST_CHECK(step.effects.return_value.empty());
         for (const auto& child : step.calls)
            BOOST_CHECK(child.return_value == expected_environment_observation(*roots[0].environment, true));
      }
      for (size_t group = 0; group < mixed.groups.size(); ++group) {
         auto wrong = mixed; wrong.groups[group].input.environment = environment_for(roots[0], 0, 2);
         BOOST_CHECK(!validate_isolated_mixed_tree_journal(wrong, roots, programs, context));
      }
      auto other_roots = roots; other_roots[0].environment = environment_for(roots[0], 0, 2);
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(mixed, other_roots, programs, context));
      BOOST_CHECK(mixed_executor.execute(base, roots, programs, context).steps[0].effects.return_value ==
         expected_environment_observation(*roots[0].environment));
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_environment_oracle_tests)
BOOST_AUTO_TEST_CASE(current_owner_rejects_stale_transaction_block_or_publication_before_any_write) {
   for (bool enabled : {false, true}) for (name route : {"run"_n, "sync"_n, "mixed"_n}) {
      console_owner_fixture fixture(enabled, route);
      const auto& control = *fixture.chain.control;
      const auto now = uint64_t(control.pending_block_time().time_since_epoch().count());
      const auto number = control.pending_block_num();
      for (unsigned mutation = 0; mutation < 9; ++mutation) {
         auto trx = fixture.packed->get_transaction();
         if (mutation == 1) trx.ref_block_prefix ^= 1;
         if (mutation == 2) trx.expiration = fc::time_point_sec{trx.expiration.sec_since_epoch() - 1};
         if (mutation == 6) trx.actions.front().data.push_back('x');
         if (mutation == 7) trx.transaction_extensions.emplace_back(60000, std::vector<char>{'x'});
         auto roots = fixture.roots;
         roots.front().environment = isolated_transaction_environment::capture(trx,
            now + (mutation == 3), now + (mutation == 4), number + (mutation == 5));
         const bool expected = mutation == 0 || mutation == 8;
         if (mutation == 8) { // equivalent content with a DIFFERENT owned allocation
            const auto same = isolated_transaction_environment::capture(trx, now, now, number);
            BOOST_CHECK(same.get() != roots.front().environment.get());
            BOOST_CHECK(same_isolated_environment(same, roots.front().environment));
            roots.front().environment = same;
         }
         if (route == "run"_n) {
            isolated_action_tree_executor executor;
            const auto result = executor.execute(fixture.base, roots, fixture.tree_programs(), fixture.protocol.tree);
            BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, fixture.tree_programs(), fixture.protocol.tree));
            fixture.replay_tree(result, expected);
            if (expected) fixture.replay_tree(result, false, 1); // actual tx_context publication changed AFTER capture
         } else if (route == "sync"_n) {
            isolated_sync_executor executor;
            const auto result = executor.execute_transaction(fixture.base, roots, fixture.programs, fixture.protocol.sync);
            BOOST_REQUIRE(validate_isolated_sync_transaction_journal(result, roots, fixture.programs, fixture.protocol.sync));
            BOOST_CHECK_EQUAL(validate_isolated_sync_transaction_current_state(control, *fixture.packed,
               fixture.programs, result), expected);
         } else {
            isolated_mixed_tree_executor executor;
            const auto result = executor.execute(fixture.base, roots, fixture.programs, fixture.protocol);
            BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result, roots, fixture.programs, fixture.protocol));
            BOOST_CHECK_EQUAL(validate_isolated_mixed_tree_current_state(control, *fixture.packed,
               fixture.programs, result), expected);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(original_production_and_block_validation_observe_the_same_nine_hosts_in_all_call_modes) {
   console_corpus corpus{environment_graph()};
   compare_console_production(corpus, {"run"_n, "sync"_n, "mixed"_n}, [](const auto& trace, bool enabled, name route) {
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route == "mixed"_n ? 4u : 1u);
      for (const auto& action : trace->action_traces) {
         if (action.context_free) BOOST_CHECK(action.return_value.empty());
         else BOOST_CHECK_EQUAL(action.return_value.size(), 256u);
         BOOST_CHECK_EQUAL(action.console.size(), enabled ? 512u : 0u);
         for (const auto& call : action.call_traces) {
            BOOST_CHECK_EQUAL(call.return_value.size(), 256u);
            BOOST_CHECK_EQUAL(call.console.size(), enabled ? 512u : 0u);
         }
      }
   });
}

BOOST_AUTO_TEST_CASE(multiple_roots_and_both_envelopes_keep_original_transaction_bytes_in_every_frame) {
   console_corpus corpus{environment_graph()};
   for (auto compression : {packed_transaction::compression_type::none, packed_transaction::compression_type::zlib})
      compare_console_production(corpus, {"run"_n, "sync"_n, "mixed"_n}, [](const auto& trace, bool enabled, name route) {
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route == "mixed"_n ? 8u : 2u);
         for (const auto& action : trace->action_traces) {
            if (action.context_free) BOOST_CHECK(action.return_value.empty());
            else BOOST_CHECK_EQUAL(action.return_value.size(), 256u);
            BOOST_CHECK_EQUAL(action.console.size(), enabled ? 512u : 0u);
            for (const auto& child : action.call_traces) BOOST_CHECK_EQUAL(child.return_value.size(), 256u);
         }
      }, 2, 2, compression);
}
BOOST_AUTO_TEST_SUITE_END()
// Append after the environment fixtures; private rejection is never canonical success.
BOOST_AUTO_TEST_SUITE(parallel_environment_private_tests)
BOOST_FIXTURE_TEST_CASE(environment_host_import_types_buffer_bounds_and_cancellation_remain_enforced, console_fixture) {
   const auto compile = [](const std::string& text) {
      const auto bytes = wast_to_wasm(text); return std::vector<char>(bytes.begin(), bytes.end());
   };
   for (const char* host : {"current_time", "publication_time", "transaction_size", "expiration",
         "tapos_block_num", "tapos_block_prefix", "get_block_num", "read_transaction", "get_action"}) {
      const auto wrong = compile(std::string("(module (import \"env\" \"") + host +
         "\" (func (param i64)))(func (export \"apply\") (param i64 i64 i64)))");
      BOOST_CHECK_THROW(isolated_wasm_module{wrong}, eosio::vm::wasm_link_exception);
   }
   auto root = console_input(); root.environment = environment_for(root);
   const auto code = console_program(environment_observe(false, false, false), "", false, "", environment_imports);
   isolated_wasm_module healthy(code);
   const auto reused = [&] {
      BOOST_CHECK(healthy.execute(base, root).return_value == expected_environment_observation(*root.environment));
   };
   for (const std::string invalid : {
         "(drop (call $transaction (i32.const -1)(i32.const 0)))",
         "(drop (call $transaction (i32.const 65535)(i32.const 2)))",
         "(drop (call $action (i32.const 1)(i32.const 0)(i32.const 65535)(i32.const 2)))",
         "(drop (call $action (i32.const 2)(i32.const 0)(i32.const 0)(i32.const 0)))",
         "(drop (call $action (i32.const -1)(i32.const 0)(i32.const 0)(i32.const 0)))"}) {
      isolated_wasm_module bad(console_program(invalid, "", false, "", environment_imports));
      BOOST_CHECK_THROW(bad.execute(base, root), primary_state_unsupported);
      reused();
   }
   std::stop_source stop; stop.request_stop();
   BOOST_CHECK_THROW(healthy.execute(base, root, stop.get_token()), isolated_execution_interrupted); reused();
   isolated_cancel expired; expired.deadline = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(healthy.execute(base, root, {}, {}, expired), isolated_execution_interrupted); reused();
   root.environment.reset();
   BOOST_CHECK_THROW(healthy.execute(base, root), isolated_transaction_environment_missing);
   root.environment = environment_for(root); reused();
}

BOOST_FIXTURE_TEST_CASE(cf_time_and_sync_action_queries_are_not_made_legal_by_a_captured_environment, console_fixture) {
   auto root = console_input(); root.environment = environment_for(root);
   const std::array roots{root};
   const auto pure = fc::raw::pack(action{{}, console_account, "pure"_n, std::vector<char>{}});
   isolated_action_tree_executor tree;
   for (const char* host : {"now", "published"}) {
      const auto code = console_program("(if (i64.eq (get_local $action)" + cn("pure"_n) +
         ")(then (drop (call $" + host + ")))(else (call $cf (i32.const 1024)(i32.const " +
         std::to_string(pure.size()) + "))))", "", false,
         "(data (i32.const 1024)\"" + console_bytes(pure) + "\")", environment_imports);
      const std::array programs{isolated_receiver_program{console_account, code, {}}};
      BOOST_CHECK_EXCEPTION(tree.execute(base, roots, programs, console_tree_context(true)), primary_state_unsupported,
         [](const auto& e) { return dynamic_cast<const isolated_transaction_environment_missing*>(&e) == nullptr; });
   }
   const std::vector<isolated_sync_program> sync_programs{
      {{console_account, console_program(console_call(console_child), "", false), {}}, true},
      {{console_child, console_program("", "(drop (call $action (i32.const 1)(i32.const 0)(i32.const 0)(i32.const 0)))",
         false, "", environment_imports), {}}, true}};
   isolated_sync_executor sync;
   BOOST_CHECK_EXCEPTION(sync.execute_transaction(base, roots, sync_programs, console_sync_context(true)), primary_state_unsupported,
      [](const auto& e) { return dynamic_cast<const isolated_transaction_environment_missing*>(&e) == nullptr; });
   const auto good = environment_graph();
   BOOST_CHECK(sync.execute_transaction(base, roots, good, console_sync_context(true)).roots[0].root.return_value ==
      expected_environment_observation(*root.environment));
}

BOOST_FIXTURE_TEST_CASE(all_roots_share_equal_context_content_and_mismatched_roots_are_rejected, console_fixture) {
   auto first = console_input(); first.environment = environment_for(first);
   auto second = first; second.environment = environment_for(first);
   BOOST_REQUIRE(first.environment.get() != second.environment.get());
   std::array roots{first, second};
   const auto programs = environment_graph();
   std::vector<isolated_receiver_program> ordinary;
   for (const auto& p : programs) ordinary.push_back(p.program);
   isolated_action_tree_executor tree; isolated_sync_executor sync; isolated_mixed_tree_executor mixed;
   const auto tc = console_tree_context(true); const auto sc = console_sync_context(true);
   const isolated_mixed_tree_context mc{tc, sc};
   BOOST_CHECK(validate_isolated_action_tree_journal(tree.execute(base, roots, ordinary, tc), roots, ordinary, tc));
   BOOST_CHECK(validate_isolated_sync_transaction_journal(sync.execute_transaction(base, roots, programs, sc), roots, programs, sc));
   BOOST_CHECK(validate_isolated_mixed_tree_journal(mixed.execute(base, roots, programs, mc), roots, programs, mc));
   for (bool missing : {false, true}) {
      roots[1].environment = missing ? nullptr : environment_for(first, 0, 2);
      BOOST_CHECK_THROW(tree.execute(base, roots, ordinary, tc), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(sync.execute_transaction(base, roots, programs, sc), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(mixed.execute(base, roots, programs, mc), primary_state_invalid_attempt);
   }
   roots[1].environment = environment_for(first);
   BOOST_CHECK(validate_isolated_mixed_tree_journal(mixed.execute(base, roots, programs, mc), roots, programs, mc));
}
BOOST_AUTO_TEST_SUITE_END()
