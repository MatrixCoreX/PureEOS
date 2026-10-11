#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <future>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key tree_rows{"test"_n, "test"_n, "rows"_n};
std::string constant(name n) { return "(i64.const " + std::to_string(n.to_uint64_t()) + ")"; }
std::string notify_tree(name n) { return "(call $notify " + constant(n) + ")"; }
std::string when_action(name n, const std::string& body) {
   return "(if (i64.eq (get_local $action) " + constant(n) + ") (then " + body + "))";
}
std::vector<char> tree_wasm(const std::string& text) {
   const auto bytes = wast_to_wasm(text); return {bytes.begin(), bytes.end()};
}
std::string escaped(std::span<const char> bytes) {
   constexpr char hex[] = "0123456789abcdef";
   std::string result;
   for (unsigned char byte : bytes) { result += '\\'; result += hex[byte >> 4]; result += hex[byte & 15]; }
   return result;
}
std::vector<char> packed_child(name receiver, name method, size_t bytes = 0) {
   action child; child.account = receiver; child.name = method; child.data.resize(bytes, 'x');
   return fc::raw::pack(child);
}

// Uses ONLY real host imports. Every group reads the same original table to
// expose shared notification cursors versus fresh inline/top-level cursors.
std::vector<char> tree_program(uint64_t key, const std::string& body = {},
      const std::vector<std::vector<char>>& sends = {}, bool update = false) {
   std::string data, functions;
   size_t offset = 1024;
   for (size_t i = 0; i < sends.size(); ++i) {
      data += "(data (i32.const " + std::to_string(offset) + ") \"" + escaped(sends[i]) + "\")";
      functions += "(func $send" + std::to_string(i) + " (call $inline (i32.const " + std::to_string(offset) +
         ") (i32.const " + std::to_string(sends[i].size()) + ")))";
      offset += sends[i].size();
   }
   return tree_wasm(R"((module
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "send_inline" (func $inline (param i32 i32)))
      (import "env" "get_sender" (func $sender (result i64)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1) )" + data + functions + R"(
      (func (export "apply") (param $receiver i64) (param $account i64) (param $action i64)
         (local $it i32) (local $count i32)
         (set_local $it (call $find )" + constant(tree_rows.code) + " " + constant(tree_rows.scope) + " " +
         constant(tree_rows.table) + " (i64.const " + std::to_string(key) + R"()))
         (drop (call $get (get_local $it) (i32.const 0) (i32.const 8)))
         (i32.store (i32.const 8) (get_local $it))
         (i64.store (i32.const 16) (get_local $action))
         )" + (update ? R"((i64.store (i32.const 0) (i64.add (i64.load (i32.const 0)) (i64.const 1)))
            (call $update (get_local $it) (i64.const 0) (i32.const 0) (i32.const 8)))" : "") + body + R"(
         (i64.store (i32.const 24) (call $sender))
         (call $return (i32.const 0) (i32.const 32)))))");
}

std::vector<isolated_receiver_program> tree_graph() {
   auto exact = packed_child("bob"_n, "childone"_n);
   exact.insert(exact.end(), {'\0', char(0xff)}); // original raw bytes must survive
   return {
      {"test"_n, tree_program(1, "(call $send0)" + notify_tree("carol"_n) + "(call $send1)" +
         notify_tree("bob"_n) + notify_tree("test"_n), {exact, packed_child("dave"_n, "childtwo"_n)}, true), {}},
      {"carol"_n, tree_program(2,
         when_action("run"_n, "(call $send0)" + notify_tree("dave"_n)) +
         when_action("childone"_n, "(call $send1)"),
         {packed_child("bob"_n, "childthree"_n), packed_child("bob"_n, "leaf"_n)}), {}},
      {"bob"_n, tree_program(3,
         when_action("run"_n, "(call $send0)" + notify_tree("carol"_n)) +
         when_action("childone"_n, notify_tree("carol"_n) + "(call $send1)"),
         {packed_child("bob"_n, "leaf"_n), packed_child("dave"_n, "grand"_n)}), {}},
      {"dave"_n, tree_program(4), {}}};
}
isolated_action_input tree_root() {
   isolated_action_input result;
   result.receiver = result.account = "test"_n; result.action = "run"_n;
   result.authorizations = {{"test"_n, "active"_n}};
   return result;
}
uint64_t value64(const std::vector<char>& data) {
   if (data.size() < 8) throw std::logic_error("short return value");
   uint64_t result; std::memcpy(&result, data.data(), 8); return result;
}
int32_t returned_cursor(const std::vector<char>& data) {
   if (data.size() < 12) throw std::logic_error("short return cursor");
   int32_t result; std::memcpy(&result, data.data() + 8, 4); return result;
}
name returned_sender(const std::vector<char>& data) {
   if (data.size() < 32) throw std::logic_error("short return sender");
   uint64_t result; std::memcpy(&result, data.data() + 24, 8); return name{result};
}
struct tree_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   tree_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const auto& table = db.create<table_id_object>([](auto& row) {
         row.code = tree_rows.code; row.scope = tree_rows.scope; row.table = tree_rows.table;
         row.payer = tree_rows.code; row.count = 4;
      });
      for (uint64_t i = 1; i <= 4; ++i) db.create<key_value_object>([&](auto& row) {
         row.t_id = table.id; row.primary_key = i; row.payer = tree_rows.code;
         const uint64_t value = i * 10; row.value.assign(reinterpret_cast<const char*>(&value), 8);
      });
      const std::array tables{tree_rows}; base = frozen_primary_state::capture(view, tables);
   }
   uint64_t canonical() { return value64(view.read_primary({tree_rows, primary_query_kind::find, 1}, 8).row->value); }
};
std::string repeat_send(unsigned count) {
   return "(set_local $count (i32.const " + std::to_string(count) + R"())
      (block $done (loop $again (br_if $done (i32.eqz (get_local $count)))
         (call $send0) (set_local $count (i32.sub (get_local $count) (i32.const 1))) (br $again))))";
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_action_tree_tests)

BOOST_FIXTURE_TEST_CASE(inline_and_notification_scheduling_uses_original_order_and_fresh_child_cursors, tree_fixture) {
   isolated_action_tree_executor executor;
   const std::array roots{tree_root(), tree_root()};
   const auto programs = tree_graph();
   const auto result = executor.execute(base, roots, programs, {});
   BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, programs, {}));
   const std::array ordinals{1u,4u,6u,8u,3u,10u,11u,12u,5u,7u,9u, 2u,14u,16u,18u,13u,20u,21u,22u,15u,17u,19u};
   const std::array creators{0u,1u,1u,4u,1u,3u,3u,10u,1u,4u,6u};
   const std::array ancestors{0u,1u,1u,1u,1u,3u,3u,3u,1u,1u,1u};
   const std::array depths{0u,0u,0u,0u,1u,1u,2u,2u,1u,1u,1u};
   const std::array cursors{0,1,2,3,0,1,0,0,0,0,0};
   BOOST_REQUIRE_EQUAL(result.steps.size(), ordinals.size());
   BOOST_REQUIRE_EQUAL(result.groups.size(), 14u);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(), 2u);
   for (size_t i = 0; i < result.steps.size(); ++i) {
      const auto& step = result.steps[i]; const auto& group = result.groups.at(step.group_index);
      BOOST_CHECK_EQUAL(step.action_ordinal, ordinals[i]);
      BOOST_CHECK_EQUAL(group.root_index, i / 11);
      BOOST_CHECK_EQUAL(group.depth, depths[i % 11]);
      BOOST_CHECK_EQUAL(returned_cursor(step.effects.return_value), cursors[i % 11]);
      BOOST_CHECK(returned_sender(step.effects.return_value) == step.sender);
      if (i < 11) {
         BOOST_CHECK_EQUAL(step.creator_action_ordinal, creators[i]);
         BOOST_CHECK_EQUAL(step.closest_unnotified_ancestor, ancestors[i]);
      }
      if (step.creator_action_ordinal) {
         const auto parent = std::find_if(result.steps.begin(), result.steps.end(), [&](const auto& s) {
            return s.action_ordinal == step.creator_action_ordinal;
         });
         BOOST_REQUIRE(parent != result.steps.end()); BOOST_CHECK(step.sender == parent->receiver);
      } else BOOST_CHECK(!step.sender);
   }
   BOOST_CHECK_EQUAL(value64(result.state.writes()[0].value), 11u);
   BOOST_CHECK_EQUAL(value64(result.state.writes()[1].value), 12u);
   const auto& sent = std::get<isolated_tree_inline_event>(result.steps[0].events[2]);
   BOOST_CHECK_EQUAL(sent.action_ordinal, 3u); BOOST_CHECK_EQUAL(sent.after_writes, 1u);
   BOOST_CHECK_EQUAL(sent.sent.packed.size(), sent.sent.decoded_bytes + 2);
   BOOST_CHECK(result.state.matches_base_reads(view, [] {})); BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(valid_inline_remains_unavailable_to_all_legacy_execution_entries, tree_fixture) {
   const auto programs = tree_graph(); const std::array roots{tree_root()};
   isolated_wasm_module module(programs[0].code);
   isolated_transaction_executor transaction;
   isolated_notification_executor notification;
   const std::array calls{isolated_action_invocation{programs[0].code, roots[0], {}}};
   BOOST_CHECK_THROW(module.execute(base, roots[0]), primary_state_unsupported);
   BOOST_CHECK_THROW(transaction.execute(base, calls), primary_state_unsupported);
   BOOST_CHECK_THROW(notification.execute(base, roots, programs), primary_state_unsupported);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(sender_host_without_inline_still_requires_the_explicit_private_tree_capability, tree_fixture) {
   const std::array programs{isolated_receiver_program{"test"_n, tree_program(1), {}}};
   const std::array roots{tree_root()};
   isolated_wasm_module module(programs[0].code);
   isolated_transaction_executor transaction;
   isolated_notification_executor notification;
   const std::array calls{isolated_action_invocation{programs[0].code, roots[0], {}}};
   BOOST_CHECK_THROW(module.execute(base, roots[0]), primary_state_unsupported);
   BOOST_CHECK_THROW(transaction.execute(base, calls), primary_state_unsupported);
   BOOST_CHECK_THROW(notification.execute(base, roots, programs), primary_state_unsupported);
   isolated_action_tree_executor tree;
   const auto result = tree.execute(base, roots, programs, {});
   BOOST_REQUIRE_EQUAL(result.steps.size(), 1u);
   BOOST_CHECK(!returned_sender(result.steps[0].effects.return_value));
   BOOST_CHECK(validate_isolated_action_tree_journal(result, roots, programs, {}));
}

BOOST_FIXTURE_TEST_CASE(deep_failure_missing_code_and_cancellation_discard_the_whole_attempt_and_allow_reuse, tree_fixture) {
   isolated_action_tree_executor executor;
   const std::array roots{tree_root()}; const auto good = tree_graph();
   for (unsigned mode = 0; mode < 5; ++mode) {
      auto programs = good;
      if (mode == 0) programs.pop_back();
      if (mode == 1) programs.back().serial_only = true;
      if (mode == 2) programs.back().code = tree_program(4, when_action("grand"_n, "unreachable"));
      if (mode == 3) programs.back().code = {'b','a','d'};
      if (mode == 4) programs.back().code = tree_program(4, "(loop $forever (br $forever))");
      isolated_cancel cancel;
      if (mode == 4) cancel.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
      BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}, {}, {}, cancel), std::exception);
      const auto result = executor.execute(base, roots, good, {});
      BOOST_CHECK_EQUAL(result.steps.size(), 11u); BOOST_CHECK_EQUAL(canonical(), 10u);
   }
   std::stop_source stopped; stopped.request_stop();
   BOOST_CHECK_THROW(executor.execute(base, roots, good, {}, stopped.get_token()), primary_state_invalid_attempt);
   auto generation = std::make_shared<std::atomic<uint64_t>>(1);
   BOOST_CHECK_THROW(executor.execute(base, roots, good, {}, {}, {}, {generation, 0}), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(executor.execute(base, roots, good, {}).steps.size(), 11u);
}

BOOST_FIXTURE_TEST_CASE(inline_wire_checks_and_memory_bounds_select_fallback_without_losing_exact_bytes, tree_fixture) {
   isolated_action_tree_executor executor; const std::array roots{tree_root()};
   auto raw = packed_child("bob"_n, "leaf"_n); raw.push_back('z');
   std::array programs{isolated_receiver_program{"test"_n, tree_program(1, "(call $send0)", {raw}, true), {}},
      isolated_receiver_program{"bob"_n, {}, {}}};
   for (uint32_t limit : {0u, uint32_t(raw.size() - 1), uint32_t(raw.size())})
      BOOST_CHECK_THROW(executor.execute(base, roots, programs, {limit, 4}), primary_state_unsupported);
   const auto result = executor.execute(base, roots, programs, {uint32_t(raw.size() + 1), 4});
   BOOST_CHECK(validate_isolated_action_tree_journal(result, roots, programs, {uint32_t(raw.size() + 1), 4}));
   BOOST_REQUIRE_EQUAL(result.steps.size(), 2u); BOOST_CHECK(!result.steps.back().has_code);
   const auto& event = std::get<isolated_tree_inline_event>(result.steps[0].events.back());
   BOOST_CHECK(event.sent.packed == raw); BOOST_CHECK_EQUAL(event.sent.decoded_bytes + 1, raw.size());
   for (const auto& invalid : {std::vector<char>{}, std::vector<char>(17, 0), std::vector<char>(19, char(0xff))}) {
      programs[0].code = tree_program(1, "(call $send0)", {invalid}, true);
      BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}), primary_state_unsupported);
   }
   programs[0].code = tree_program(1, "(call $inline (i32.const 65536) (i32.const 1))", {}, true);
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}), primary_state_unsupported);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(protocol_depth_local_depth_steps_bytes_and_events_are_whole_tree_budgets, tree_fixture) {
   isolated_action_tree_executor executor; const std::array roots{tree_root()};
   const auto self = packed_child("test"_n, "run"_n);
   std::array recursive{isolated_receiver_program{"test"_n, tree_program(1, "(call $send0)", {self}, true), {}}};
   for (uint32_t depth : {0u, 1u, 4u, 100u})
      BOOST_CHECK_THROW(executor.execute(base, roots, recursive, {512 * 1024, depth}), primary_state_unsupported);
   for (unsigned count : {255u, 256u}) {
      std::array programs{isolated_receiver_program{"test"_n,
         tree_program(1, repeat_send(count), {packed_child("bob"_n, "leaf"_n)}, true), {}},
         isolated_receiver_program{"bob"_n, {}, {}}};
      if (count == 255) {
         const auto result = executor.execute(base, roots, programs, {});
         BOOST_CHECK_EQUAL(result.steps.size(), 256u);
         BOOST_CHECK(validate_isolated_action_tree_journal(result, roots, programs, {}));
      }
      else BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}), primary_state_budget_exceeded);
   }
   std::array big{isolated_receiver_program{"test"_n,
      tree_program(1, repeat_send(64), {packed_child("bob"_n, "leaf"_n, 4096)}, true), {}},
      isolated_receiver_program{"bob"_n, {}, {}}};
   BOOST_CHECK_THROW(executor.execute(base, roots, big, {}), primary_state_budget_exceeded);
   std::string repeat_reads = R"((set_local $count (i32.const 4096))
      (block $done (loop $again (br_if $done (i32.eqz (get_local $count)))
         (drop (call $find )" + constant(tree_rows.code) + " " + constant(tree_rows.scope) + " " +
         constant(tree_rows.table) + R"( (i64.const 1)))
         (set_local $count (i32.sub (get_local $count) (i32.const 1))) (br $again))))";
   recursive[0].code = tree_program(1, repeat_reads);
   BOOST_CHECK_THROW(executor.execute(base, roots, recursive, {}), primary_state_budget_exceeded);
   const auto graph = tree_graph();
   BOOST_CHECK_THROW(executor.execute(base, roots, graph, {512 * 1024, 1}), primary_state_unsupported);
   BOOST_CHECK_EQUAL(executor.execute(base, roots, graph, {512 * 1024, 2}).steps.size(), 11u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(inputs_tree_context_and_creator_relationships_have_separate_fingerprints, tree_fixture) {
   isolated_action_tree_executor executor; const std::array roots{tree_root()}; const auto programs = tree_graph();
   const auto first = executor.execute(base, roots, programs, {});
   const auto different_limit = executor.execute(base, roots, programs, {512 * 1024 - 1, 4});
   BOOST_CHECK(first.root_fingerprint != different_limit.root_fingerprint);
   BOOST_CHECK(first.steps[0].effects.execution_fingerprint != different_limit.steps[0].effects.execution_fingerprint);
   const auto different_depth = executor.execute(base, roots, programs, {512 * 1024, 3});
   BOOST_CHECK(first.root_fingerprint != different_depth.root_fingerprint);
   auto changed_roots = roots; changed_roots[0].data = {'x'};
   BOOST_CHECK(first.root_fingerprint != executor.execute(base, changed_roots, programs, {}).root_fingerprint);
   const auto& group = first.groups[first.steps[4].group_index]; const auto step = first.steps[4];
   auto args = group.input; args.receiver = step.receiver;
   const auto base_fp = isolated_execution_fingerprint(step.code_hash, {}, args);
   BOOST_CHECK(step.effects.execution_fingerprint == isolated_action_tree_step_fingerprint(base_fp, {}, group, step));
   for (unsigned mode = 0; mode < 5; ++mode) {
      auto altered = step;
      if (mode == 0) ++altered.action_ordinal;
      if (mode == 1) ++altered.creator_action_ordinal;
      if (mode == 2) ++altered.closest_unnotified_ancestor;
      if (mode == 3) altered.sender = "wrong"_n;
      if (mode == 4) ++altered.group_index;
      BOOST_CHECK(step.effects.execution_fingerprint != isolated_action_tree_step_fingerprint(base_fp, {}, group, altered));
   }
}

BOOST_FIXTURE_TEST_CASE(one_two_four_thread_affine_executors_own_independent_call_trees_and_vm_memory, tree_fixture) {
   const auto programs = tree_graph(); const std::array roots{tree_root(), tree_root()};
   isolated_action_tree_executor owner;
   auto wrong = std::async(std::launch::async, [&] {
      try { (void)owner.execute(base, roots, programs, {}); }
      catch (const state_view_wrong_thread&) { return true; } return false;
   });
   BOOST_CHECK(wrong.get());
   for (unsigned count : {1u, 2u, 4u}) {
      std::vector<std::future<bool>> jobs;
      for (unsigned i = 0; i < count; ++i) jobs.push_back(std::async(std::launch::async, [&] {
         isolated_action_tree_executor local;
         for (unsigned repeat = 0; repeat < 3; ++repeat) {
            const auto result = local.execute(base, roots, programs, {});
            if (result.steps.size() != 22 || result.state.writes().size() != 2 ||
                value64(result.state.writes()[1].value) != 12 ||
                !validate_isolated_action_tree_journal(result, roots, programs, {})) return false;
         }
         return true;
      }));
      for (auto& job : jobs) BOOST_CHECK(job.get());
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_action_tree_oracle_tests)

BOOST_AUTO_TEST_CASE(private_tree_matches_original_inline_notification_trace_and_receipt_order) {
   eosio::testing::tester original;
   original.create_accounts({"test"_n, "bob"_n, "carol"_n, "dave"_n});
   std::string create;
   for (unsigned key = 1; key <= 4; ++key)
      create += "(i64.store (i32.const 0) (i64.const " + std::to_string(key * 10) + ")) (drop (call $store " +
         constant(tree_rows.scope) + " " + constant(tree_rows.table) + " " + constant(tree_rows.code) +
         " (i64.const " + std::to_string(key) + ") (i32.const 0) (i32.const 8)))";
   const auto init = tree_wasm(R"((module
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (memory 1 1) (func (export "apply") (param i64 i64 i64) )" + create + "))");
   original.set_code("test"_n, std::vector<uint8_t>(init.begin(), init.end())); original.produce_block();
   auto push = [&](unsigned count, bool no_throw = false) {
      signed_transaction trx;
      for (unsigned i = 0; i < count; ++i) {
         action act; act.account = "test"_n; act.name = "run"_n; act.authorization = {{"test"_n, "active"_n}};
         act.data = {static_cast<char>(i)}; trx.actions.push_back(std::move(act));
      }
      original.set_transaction_headers(trx);
      trx.sign(original.get_private_key("test"_n, "active"), original.control->get_chain_id());
      return original.push_transaction(trx, fc::time_point::maximum(),
         eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US, no_throw);
   };
   push(1); original.produce_block();
   const auto programs = tree_graph();
   for (const auto& p : programs) original.set_code(p.receiver, std::vector<uint8_t>(p.code.begin(), p.code.end()));
   original.produce_block();
   chainbase_transaction_state_view live(original.control->db()); const std::array tables{tree_rows};
   const auto base = frozen_primary_state::capture(live, tables);
   std::array roots{tree_root(), tree_root()}; roots[0].data = {0}; roots[1].data = {1};
   const auto& config = original.control->get_global_properties().configuration;
   const isolated_action_tree_context limits{config.max_inline_action_size, config.max_inline_action_depth};
   isolated_action_tree_executor executor;
   const auto before = original.control->calculate_integrity_hash();
   const auto result = executor.execute(base, roots, programs, limits);
   BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, programs, limits));
   BOOST_CHECK(original.control->calculate_integrity_hash() == before);
   const auto trace = push(2); BOOST_REQUIRE(trace && !trace->except);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), result.steps.size());
   uint64_t sequence = 0;
   for (const auto& step : result.steps) {
      const auto& actual = trace->action_traces.at(step.action_ordinal - 1);
      const auto& group = result.groups.at(step.group_index);
      BOOST_CHECK_EQUAL(actual.action_ordinal.value, step.action_ordinal);
      BOOST_CHECK_EQUAL(actual.creator_action_ordinal.value, step.creator_action_ordinal);
      BOOST_CHECK_EQUAL(actual.closest_unnotified_ancestor_action_ordinal.value, step.closest_unnotified_ancestor);
      BOOST_CHECK(actual.receiver == step.receiver); BOOST_CHECK(actual.return_value == step.effects.return_value);
      BOOST_CHECK(actual.act.account == group.input.account); BOOST_CHECK(actual.act.name == group.input.action);
      BOOST_CHECK(actual.act.data == group.input.data);
      std::vector<std::pair<name,name>> permissions;
      for (const auto& p : actual.act.authorization) permissions.emplace_back(p.actor, p.permission);
      BOOST_CHECK(permissions == group.input.authorizations);
      BOOST_REQUIRE(actual.receipt); BOOST_CHECK_GT(actual.receipt->global_sequence, sequence);
      sequence = actual.receipt->global_sequence;
      if (step.creator_action_ordinal)
         BOOST_CHECK(step.sender == trace->action_traces.at(step.creator_action_ordinal - 1).receiver);
   }
   BOOST_CHECK_EQUAL(value64(live.read_primary({tree_rows, primary_query_kind::find, 1}, 8).row->value), 12u);

   // Private scheduling/structural validation is deliberately NOT authority.
   // Original send-time checks reject absent eosio.code authority and a missing
   // permission AFTER the write and earlier notification, BEFORE child traces.
   auto permission_programs = programs;
   for (unsigned mode : {0u, 1u}) {
      original.produce_block();
      action child; child.account = "bob"_n; child.name = "leaf"_n;
      child.authorization = {{"bob"_n, mode ? "missing"_n : "active"_n}};
      permission_programs[0].code = tree_program(1, notify_tree("carol"_n) + "(call $send0)",
         {fc::raw::pack(child)}, true);
      original.set_code("test"_n, std::vector<uint8_t>(permission_programs[0].code.begin(), permission_programs[0].code.end()));
      original.produce_block();
      const auto frozen = frozen_primary_state::capture(live, tables);
      const auto hypothetical = executor.execute(frozen, std::span(roots).first(1), permission_programs, limits);
      BOOST_CHECK(validate_isolated_action_tree_journal(hypothetical, std::span(roots).first(1), permission_programs, limits));
      const auto before_failure = original.control->calculate_integrity_hash();
      const auto failed = push(1, true);
      BOOST_REQUIRE(failed && failed->except);
      BOOST_REQUIRE_EQUAL(failed->action_traces.size(), 2u);
      BOOST_CHECK(failed->action_traces[0].except); BOOST_CHECK(!failed->action_traces[0].receipt);
      BOOST_CHECK(failed->action_traces[1].receiver == "carol"_n);
      BOOST_CHECK(!failed->action_traces[1].receipt);
      BOOST_CHECK(original.control->calculate_integrity_hash() == before_failure);
      BOOST_CHECK_EQUAL(value64(live.read_primary({tree_rows, primary_query_kind::find, 1}, 8).row->value), 12u);
   }
   // Grant original permission explicitly in this disposable test chain; now
   // the same sender can authorize bob@active without inheriting root signatures.
   authority allowed{1, {{original.get_public_key("bob"_n, "active"), 1}},
      {{{"test"_n, config::eosio_code_name}, 1}}};
   original.set_authority("bob"_n, "active"_n, allowed);
   action authorized_child; authorized_child.account = "bob"_n; authorized_child.name = "leaf"_n;
   authorized_child.authorization = {{"bob"_n, "active"_n}};
   permission_programs[0].code = tree_program(1, notify_tree("carol"_n) + "(call $send0)",
      {fc::raw::pack(authorized_child)}, true);
   original.set_code("test"_n, std::vector<uint8_t>(permission_programs[0].code.begin(), permission_programs[0].code.end()));
   original.produce_block();
   const auto authorized = executor.execute(frozen_primary_state::capture(live, tables), std::span(roots).first(1), permission_programs, limits);
   const auto accepted = push(1); BOOST_REQUIRE(accepted && !accepted->except);
   BOOST_REQUIRE_EQUAL(accepted->action_traces.size(), authorized.steps.size());
   for (const auto& step : authorized.steps) {
      const auto& actual = accepted->action_traces.at(step.action_ordinal - 1);
      BOOST_CHECK(actual.return_value == step.effects.return_value);
      BOOST_CHECK_EQUAL(actual.creator_action_ordinal.value, step.creator_action_ordinal);
   }
   BOOST_CHECK_EQUAL(value64(live.read_primary({tree_rows, primary_query_kind::find, 1}, 8).row->value), 13u);

   // A valid prefix followed by a grandchild trap must leave neither an owned
   // private prefix nor a canonical balance/receipt sequence mutation behind.
   original.produce_block();
   auto failed_programs = programs;
   failed_programs.back().code = tree_program(4, when_action("grand"_n, "unreachable"));
   // Only these two programs changed; original setcode correctly rejects
   // deploying an identical code hash to the unchanged notification receivers.
   for (const size_t index : {0u, 3u}) {
      const auto& p = failed_programs[index];
      original.set_code(p.receiver, std::vector<uint8_t>(p.code.begin(), p.code.end()));
   }
   original.produce_block();
   const auto before_deep_failure = original.control->calculate_integrity_hash();
   BOOST_CHECK_THROW(executor.execute(frozen_primary_state::capture(live, tables), roots, failed_programs, limits), std::exception);
   const auto deep = push(2, true); BOOST_REQUIRE(deep && deep->except);
   BOOST_CHECK(std::any_of(deep->action_traces.begin(), deep->action_traces.end(), [](const auto& action) {
      return action.receiver == "dave"_n && action.act.name == "grand"_n && bool(action.except);
   }));
   BOOST_CHECK(original.control->calculate_integrity_hash() == before_deep_failure);
   BOOST_CHECK_EQUAL(value64(live.read_primary({tree_rows, primary_query_kind::find, 1}, 8).row->value), 13u);
}

BOOST_AUTO_TEST_SUITE_END()
