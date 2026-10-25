#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/testing/tester.hpp>
#include "parallel_test_runtime.hpp"
#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <cstring>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
#include "parallel_test_diagnostics.hpp"

namespace {
// Opt-in post-join cost evidence for the actual late-failure/reuse batch.
// These sums overlap (wait includes worker work); they are not TPS or billing.
template<typename F>
auto secondary_test_measure(const char* label, F&& run) {
   const auto* setting=std::getenv("PUREEOS_PARALLEL_TEST_METRICS");
   if(!setting || std::string_view(setting)!="1") return std::invoke(std::forward<F>(run));
   constexpr std::array stages{performance::stage::parallel_capture,performance::stage::parallel_batch_wait,
      performance::stage::parallel_adopt,performance::stage::parallel_fallback};
   auto& metrics=performance::metrics();
   std::array<performance::sample,stages.size()> before;
   for(size_t i=0;i<stages.size();++i) before[i]=metrics.snapshot(stages[i]);
   const auto report=[&] {
      for(size_t i=0;i<stages.size();++i) {
         const auto after=metrics.snapshot(stages[i]);
         std::fprintf(stderr,"SECONDARY_STAGE %s stage=%s count=%llu us=%llu\n",label,
            performance::names[static_cast<size_t>(stages[i])],
            static_cast<unsigned long long>(after.count-before[i].count),
            static_cast<unsigned long long>((after.nanoseconds-before[i].nanoseconds)/1000));
      }
   };
   try {
      auto result=parallel_test_measure(label,std::forward<F>(run)); report(); return result;
   } catch(...) { report(); throw; }
}

constexpr name code_account = "test"_n;
constexpr name table_name = "rows"_n;
const std::array<std::string, 5> families{"idx64", "idx128", "idx256", "idx_double", "idx_long_double"};
std::string i32(uint32_t value) { return "(i32.const " + std::to_string(value) + ")"; }
std::string i64(uint64_t value) { return "(i64.const " + std::to_string(value) + ")"; }
template<class T> void put(std::vector<char>& out, size_t offset, T value) { std::memcpy(out.data() + offset, &value, sizeof(value)); }
std::array<uint64_t, 4> key_bits(unsigned family, bool second = false) {
   switch (family) {
      case 0: case 1: return {second ? 2000u : 1000u, 0, 0, 0};
      case 2: return {second ? 2000u : 1000u, 0, second ? 21u : 11u, 0};
      case 3: return {second ? 0x4000000000000000ULL : 0x3ff0000000000000ULL, 0, 0, 0};
      default: return {0, second ? 0x4000000000000000ULL : 0x3fff000000000000ULL, 0, 0};
   }
}
// All calls are actual production imports. Each action gets a fresh scope,
// seeds three rows (including duplicate keys), then executes the selected host.
// Returning raw memory exposes original aligned/misaligned output aliasing.
std::vector<char> secondary_program(unsigned family, bool zero_keys = false) {
   const bool span = family == 2;
   const auto prefix = "db_" + families.at(family) + "_";
   const auto count_param = span ? " i32" : "";
   std::string imports;
   auto add = [&](const char* operation, const std::string& parameters, bool result = true) {
      imports += "(import \"env\" \"" + prefix + operation + "\" (func $" + operation + " (param " + parameters + ")" +
         (result ? " (result i32)" : "") + "))";
   };
   add("store", "i64 i64 i64 i64 i32" + std::string(count_param));
   add("update", "i32 i64 i32" + std::string(count_param), false); add("remove", "i32", false);
   add("find_primary", "i64 i64 i64 i32" + std::string(count_param) + " i64");
   for (const char* op : {"find_secondary", "lowerbound", "upperbound"})
      add(op, "i64 i64 i64 i32" + std::string(count_param) + " i32");
   add("end", "i64 i64 i64"); add("next", "i32 i32"); add("previous", "i32 i32");
   imports += R"((import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32))))";
   const auto table = i64(table_name.to_uint64_t());
   const std::string owner = "(get_local $receiver)", scope = "(get_local $scope)", key = "(get_local $key)", output = "(get_local $output)";
   const auto count = span ? "(i32.load (i32.const 8204))" : "";
   const auto valid_count = span ? "(i32.const 2)" : "";
   std::string seed;
   for (unsigned j = 0; j < 3; ++j) {
      const auto bits = zero_keys ? std::array<uint64_t, 4>{} : key_bits(family, j == 1);
      for (unsigned w = 0; w < 4; ++w) seed += "(i64.store " + i32(2048 + w * 8) + i64(bits[w]) + ")";
      seed += "(drop (call $store " + scope + table + owner + i64(j == 0 ? 17 : j == 1 ? 7 : 5) + i32(2048) + valid_count + "))";
   }
   std::string body = R"((local $scope i64) (local $key i32) (local $output i32) (local $op i32) (local $result i32)
      (drop (call $read (i32.const 8192) (i32.const 80)))
      (set_local $key (i32.load (i32.const 8192))) (set_local $output (i32.load (i32.const 8196)))
      (set_local $op (i32.load (i32.const 8200))) (set_local $scope (i64.load (i32.const 8208)))
      (if (i32.eqz (i32.load (i32.const 8256))) (then )" + seed + "))";
   std::string initialize;
   for (unsigned w = 0; w < 4; ++w)
      initialize += "(i64.store (i32.add " + key + i32(8 * w) + ") (i64.load " + i32(8224 + 8 * w) + "))";
   body += "(if (i32.le_u " + key + i32(65504) + ") (then " + initialize + "))";
   body += "(if (i32.le_u " + output + i32(65528) + ") (then (i64.store " + output + i64(99) + ")))";
   const auto query = owner + scope + table;
   // An existing-row update/remove first obtains its ORIGINAL iterator. The
   // separate scratch key preserves the requested replacement key at $key.
   body += "(if (i32.and (i32.ne (i32.load (i32.const 8256)) (i32.const 0))"
      "(i32.or (i32.eq (get_local $op) (i32.const 1)) (i32.eq (get_local $op) (i32.const 2))))"
      "(then (drop (call $find_primary " + query + i32(2048) + valid_count + "(i64.load (i32.const 8216))))))";
   const std::array operations{
      "(call $store " + scope + table + owner + i64(19) + key + count + ")",
      "(block (result i32) (call $update " + i32(0) + i64(0) + key + count + ")" + i32(0) + ")",
      "(block (result i32) (call $remove " + i32(0) + ")" + i32(0) + ")",
      "(call $find_primary " + query + key + count + "(i64.load (i32.const 8216)))",
      "(call $find_secondary " + query + key + count + output + ")",
      "(call $lowerbound " + query + key + count + output + ")",
      "(call $upperbound " + query + key + count + output + ")",
      "(call $end " + query + ")",
      "(call $next " + i32(0) + output + ")",
      "(call $previous " + i32(0) + output + ")"};
   for (unsigned op = 0; op < operations.size(); ++op)
      body += "(if (i32.eq (get_local $op) " + i32(op) + ") (then (set_local $result " + operations[op] + ")))";
   body += "(i32.store (i32.const 128) (get_local $result)) (call $return (i32.const 0) (i32.const 256))";
   const auto wasm = wast_to_wasm("(module " + imports + "(memory 1 1) (func (export \"apply\") (param $receiver i64) (param i64 i64) " + body + "))");
   return {wasm.begin(), wasm.end()};
}
struct wasm_oracle_fixture {
   eosio::testing::tester original;
   uint64_t next_scope = 1;
   wasm_oracle_fixture() { original.create_accounts({code_account}); }
   isolated_action_input arguments(unsigned family, unsigned operation, uint32_t key_offset = 64, uint32_t output_offset = 192) {
      isolated_action_input input; input.receiver = input.account = code_account; input.action = "run"_n;
      input.authorizations = {{code_account, "active"_n}}; input.data.resize(80);
      put(input.data, 0, key_offset); put(input.data, 4, output_offset); put(input.data, 8, operation); put(input.data, 12, uint32_t{2});
      put(input.data, 16, next_scope); put(input.data, 72, next_scope++); put(input.data, 24, uint64_t{17});
      const auto bits = key_bits(family); std::memcpy(input.data.data() + 32, bits.data(), 32);
      return input;
   }
   table_key table(const isolated_action_input& input) {
      uint64_t scope; std::memcpy(&scope, input.data.data() + 16, 8); return {code_account, name(scope), table_name};
   }
   std::shared_ptr<const frozen_primary_state> capture(const isolated_action_input& input, std::span<const secondary_query> reads = {}) {
      chainbase_transaction_state_view view(original.control->db());
      const std::array tables{table(input)}; const std::array accounts{code_account};
      return frozen_primary_state::capture_queries_with_secondary(view, tables, {}, reads, {}, {}, accounts);
   }
   transaction_trace_ptr push(const isolated_action_input& input) {
      signed_transaction trx; trx.actions.push_back(action{{{code_account, "active"_n}}, code_account, input.action, input.data});
      original.set_transaction_headers(trx); trx.sign(original.get_private_key(code_account, "active"), original.control->get_chain_id());
      return original.push_transaction(trx);
   }
   void deploy(const std::vector<char>& code) {
      original.set_code(code_account, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
   }
   void compare(unsigned family, isolated_notification_executor& executor,
                const std::vector<isolated_receiver_program>& programs, const isolated_action_input& input) {
      const auto base = capture(input); const std::array roots{input};
      const auto before = original.control->calculate_integrity_hash();
      const auto attempt = executor.execute(base, roots, programs);
      BOOST_REQUIRE(validate_isolated_notification_journal(attempt, roots, programs));
      BOOST_CHECK(!attempt.state.primary_only());
      BOOST_CHECK(original.control->calculate_integrity_hash() == before); // no worker effects on chain
      const auto trace = push(input); BOOST_REQUIRE(trace && !trace->except);
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
      BOOST_REQUIRE_EQUAL(attempt.steps.size(), 1u);
      BOOST_CHECK(attempt.steps[0].effects.return_value == trace->action_traces[0].return_value);
      chainbase_transaction_state_view live(original.control->db()); chainbase_secondary_state_view secondary(live);
      primary_state_overlay expected(base);
      for (const auto& write : attempt.state.writes()) {
         const auto& w = std::get<secondary_mutation>(write);
         if (w.operation == primary_operation::store) expected.store_secondary(w.table, w.primary_key, w.payer, w.key);
         else if (w.operation == primary_operation::update) expected.update_secondary(w.table, w.primary_key, w.payer, w.key);
         else expected.remove_secondary(w.table, w.primary_key, w.key);
      }
      for (uint64_t pk : {5, 7, 17, 19}) {
         secondary_key zero;
         switch (family) { case 0: zero = uint64_t{0}; break; case 1: zero = uint128_t{0}; break;
            case 2: zero = std::array<uint128_t, 2>{}; break; case 3: zero = secondary_float64{}; break; default: zero = secondary_float128{}; }
         const secondary_query q{table(input), secondary_query_kind::find_primary, zero, pk};
         const auto a = expected.read_secondary(q), b = secondary.read_secondary(q);
         BOOST_REQUIRE_EQUAL(bool(a.row), bool(b.row));
         if (a.row) { BOOST_CHECK(a.row->key == b.row->key); BOOST_CHECK(a.row->payer == b.row->payer); }
         BOOST_REQUIRE_EQUAL(bool(a.table), bool(b.table));
         if (a.table) { BOOST_CHECK_EQUAL(a.table->count, b.table->count); BOOST_CHECK(a.table->payer == b.table->payer); }
      }
   }
};
} // namespace

namespace {
constexpr table_key graph_table{code_account, code_account, table_name};
constexpr std::array graph_accounts{code_account, "bob"_n, "carol"_n, "empty"_n};
std::string sg_assert(const std::string& condition) { return "(call $assert " + condition + i64(777) + ")"; }
std::string sg_equal(const std::string& expression, uint32_t expected) { return sg_assert("(i32.eq " + expression + i32(expected) + ")"); }
std::string sg_when(name method, const std::string& body) {
   return "(if (i64.eq (get_local $action) " + i64(method.to_uint64_t()) + ") (then " + body + "))";
}
std::string sg_key(unsigned family, bool second = false) {
   const auto bits = key_bits(family, second); std::string body;
   for (unsigned i = 0; i < 4; ++i) body += "(i64.store " + i32(64 + i * 8) + i64(bits[i]) + ")";
   return body;
}
std::string sg_count(unsigned family) { return family == 2 ? i32(2) : ""; }
std::string sg_find(unsigned family, uint64_t pk) {
   return "(call $sfind " + i64(code_account.to_uint64_t()) + i64(graph_table.scope.to_uint64_t()) +
      i64(table_name.to_uint64_t()) + i32(64) + sg_count(family) + i64(pk) + ")";
}
std::string sg_store(unsigned family, uint64_t pk, bool second, uint32_t handle) {
   return sg_key(family, second) + sg_equal("(call $sstore " + i64(graph_table.scope.to_uint64_t()) +
      i64(table_name.to_uint64_t()) + i64(code_account.to_uint64_t()) + i64(pk) + i32(64) + sg_count(family) + ")", handle);
}
std::string sg_update(unsigned family, uint32_t handle, bool second) {
   return sg_key(family, second) + "(call $supdate " + i32(handle) + i64(0) + i32(64) + sg_count(family) + ")";
}
std::string sg_call(const std::string& mode, const std::string& flags, int64_t expected = 32) {
   return "(i32.store (i32.const 256) " + mode + ")" + sg_assert("(i64.eq (call $call " +
      i64(code_account.to_uint64_t()) + flags + i32(256) + i32(4) + ") (i64.const " + std::to_string(expected) + "))");
}
std::vector<char> sg_child(name receiver, name method) {
   action child; child.account = receiver; child.name = method; return fc::raw::pack(child);
}
std::vector<char> sg_program(unsigned family, const std::string& apply, const std::string& sync = "(i64.const 0)",
      const std::vector<std::pair<bool, std::vector<char>>>& sends = {}, bool with_console = false) {
   const auto prefix = "db_" + families[family] + "_"; const auto span = family == 2 ? " i32" : "";
   std::string imports;
   auto add = [&](const char* op, const char* alias, const std::string& params, bool result = true) {
      imports += "(import \"env\" \"" + prefix + op + "\" (func $" + alias + " (param " + params + ")" +
         (result ? " (result i32)" : "") + "))";
   };
   add("store", "sstore", "i64 i64 i64 i64 i32" + std::string(span));
   add("update", "supdate", "i32 i64 i32" + std::string(span), false); add("remove", "sremove", "i32", false);
   add("find_primary", "sfind", "i64 i64 i64 i32" + std::string(span) + " i64");
   add("next", "snext", "i32 i32"); add("previous", "sprevious", "i32 i32"); add("end", "send", "i64 i64 i64");
   imports += R"((import "env" "call" (func $call (param i64 i64 i32 i32) (result i64)))
      (import "env" "get_call_data" (func $data (param i32 i32) (result i32)))
      (import "env" "set_call_return_value" (func $set (param i32 i32)))
      (import "env" "set_action_return_value" (func $out (param i32 i32)))
      (import "env" "eosio_assert_code" (func $assert (param i32 i64)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "send_inline" (func $inline (param i32 i32)))
      (import "env" "send_context_free_inline" (func $cfi (param i32 i32)))
      (import "env" "db_store_i64" (func $pstore (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $pfind (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_remove_i64" (func $premove (param i32))))";
   if (with_console) imports += R"((import "env" "printi" (func $printi (param i64))))";
   std::string data, functions; size_t offset = 1024;
   for (size_t i = 0; i < sends.size(); ++i) {
      constexpr char hex[] = "0123456789abcdef"; std::string encoded;
      for (unsigned char byte : sends[i].second) { encoded += '\\'; encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
      data += "(data " + i32(offset) + " \"" + encoded + "\")";
      functions += "(func $send" + std::to_string(i) + " (call $" + (sends[i].first ? "cfi" : "inline") +
         i32(offset) + i32(sends[i].second.size()) + "))";
      offset += sends[i].second.size();
   }
   const auto bytes = wast_to_wasm("(module " + imports + "(memory 1 1)" + data + functions +
      "(func (export \"apply\") (param $receiver i64) (param $account i64) (param $action i64) " + apply + ")" +
      "(func (export \"sync_call\") (param $sender i64) (param $receiver i64) (param $size i32) (result i64) (local $mode i32) " + sync + "))");
   return {bytes.begin(), bytes.end()};
}
std::vector<isolated_sync_program> secondary_graph(unsigned family, bool sync_only = false, bool late_authority = false,
      bool with_console = false) {
   // An opt-in variant of the SAME graph, not a reduced substitute for the
   // original index/cursor/rollback cases. Default generated WASM is unchanged.
   const auto print = [=](int value) {
      return with_console ? "(call $printi (i64.const " + std::to_string(value) + "))" : std::string{};
   };
   const std::string out = "(call $out (i32.const 64) (i32.const 64))";
   const auto returned = print(299) + "(call $set (i32.const 64) (i32.const 32))(return (i64.const 0))";
   const auto scope = i64(graph_table.scope.to_uint64_t()), table = i64(table_name.to_uint64_t()), owner = i64(code_account.to_uint64_t());
   const auto is_action = [](name method) { return "(i64.eq (get_local $action) " + i64(method.to_uint64_t()) + ")"; };
   const auto root = sg_when("noop"_n, print(12) + out + "(return)") +
      sg_when("read"_n, print(11) + sg_equal(sg_find(family, 17), 0) + out + "(return)") + print(10) +
      sg_equal("(call $pstore " + scope + table + owner + i64(100) + i32(320) + i32(8) + ")", 0) +
      print(20) + sg_store(family, 17, false, 0) + print(30) + sg_store(family, 7, true, 1) + print(40) +
      sg_call("(if (result i32) " + is_action("business"_n) + " (then (i32.const 11)) (else (i32.const 1)))",
         "(if (result i64) " + is_action("readonly"_n) + " (then (i64.const 1)) (else (i64.const 0)))") + print(50) +
      sg_when("stale"_n, "(drop (call $snext (i32.const 1) (i32.const 112)))") +
      sg_equal(sg_find(family, 7), 2) + "(i32.store (i32.const 96)(i32.const 2))" +
      sg_equal("(call $sprevious (i32.const 0) (i32.const 112))", 2) + print(60) +
      (sync_only ? sg_call(i32(4), i64(0)) :
         "(call $notify " + i64("carol"_n.to_uint64_t()) + ")" +
         "(if " + is_action("cfbad"_n) + " (then (call $send2)) (else (call $send0)))" +
         (late_authority ? "(if " + is_action("authfail"_n) + " (then (call $send3)) (else (call $send1)))" : "(call $send1)")) + print(70) + out;
   const auto mode_is = [](uint32_t mode) { return "(i32.eq (get_local $mode) " + i32(mode) + ")"; };
   const auto condition = [&](uint32_t a, uint32_t b) { return "(i32.or " + mode_is(a) + mode_is(b) + ")"; };
   const auto sync = print(200) + "(drop (call $data (i32.const 256)(i32.const 4)))(set_local $mode (i32.load (i32.const 256)))"
      "(if " + condition(1, 11) + " (then " + print(210) + sg_equal(sg_find(family, 17), 0) + sg_update(family, 0, true) + print(211) +
         "(if " + mode_is(11) + " (then " + sg_call(i32(12), i64(0), -10017) + ") (else " + sg_call(i32(2), i64(0)) + "))" +
         print(212) + sg_equal(sg_find(family, 17), 0) + returned + "))" +
      "(if " + condition(2, 12) + " (then " + print(220) + sg_equal(sg_find(family, 7), 0) +
         "(call $sremove (i32.const 0))" + print(221) + sg_store(family, 7, false, 1) + print(222) +
         "(if " + mode_is(12) + " (then " + print(223) + "(return (i64.const -10017))))" + returned + "))" +
      "(if " + mode_is(3) + " (then " + print(230) + sg_equal(sg_find(family, 17), 0) + returned + "))" +
      "(if " + mode_is(4) + " (then " + print(240) + sg_equal(sg_find(family, 17), 0) + sg_update(family, 0, false) + print(241) +
         "(call $premove (call $pfind " + owner + scope + table + i64(100) + "))" + print(242) + returned + ")) (i64.const 0)";
   const auto notification = print(80) + sg_equal(sg_find(family, 7), 2) +
      sg_when("foreign"_n, sg_update(family, 2, true)) + print(81) + sg_call(i32(3), i64(1)) +
      print(82) + sg_equal(sg_find(family, 7), 2) + out;
   const auto inlined = print(100) + sg_equal(sg_find(family, 7), 0) + print(101) + sg_call(i32(4), i64(0)) +
      print(102) + sg_equal(sg_find(family, 17), 1) + out;
   const auto cf = print(90) + sg_when("bad"_n, "(drop (call $send " + owner + scope + table + "))") + print(91);
   std::vector<std::pair<bool, std::vector<char>>> sends{{true, sg_child("empty"_n, "pure"_n)},
      {false, sg_child("bob"_n, "work"_n)}, {true, sg_child("empty"_n, "bad"_n)}};
   if (late_authority) {
      action child; child.account = "bob"_n; child.name = "work"_n;
      child.authorization = {{"bob"_n, "active"_n}};
      sends.emplace_back(false, fc::raw::pack(child));
   }
   if (sync_only) return {{{code_account, sg_program(family, root, sync, sends, with_console), {}}, true}};
   return {{{code_account, sg_program(family, root, sync, sends, with_console), {}}, true},
      {{"carol"_n, sg_program(family, notification, "(i64.const 0)", {}, with_console), {}}, true},
      {{"bob"_n, sg_program(family, inlined, "(i64.const 0)", {}, with_console), {}}, true},
      {{"empty"_n, sg_program(family, cf, "(i64.const 0)", {}, with_console), {}}, true}};
}
isolated_mixed_tree_context graph_context() {
   isolated_mixed_tree_context c; c.tree.context_free_data = isolated_context_free_data::capture({}); return c;
}
std::vector<isolated_action_input> graph_roots(name action = "run"_n) {
   isolated_action_input root; root.account = root.receiver = code_account; root.action = action;
   root.authorizations = {{code_account, "active"_n}};
   auto read = root; read.action = "read"_n; return {root, read};
}
std::shared_ptr<const frozen_primary_state> graph_capture(const chainbase_transaction_state_view& view) {
   const std::array tables{graph_table};
   return frozen_primary_state::capture_queries_with_secondary(view, tables, {}, {}, {}, {}, graph_accounts);
}
struct secondary_graph_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   secondary_graph_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      db.add_index<index64_index>(); db.add_index<index128_index>(); db.add_index<index256_index>();
      db.add_index<index_double_index>(); db.add_index<index_long_double_index>();
      for (auto n : graph_accounts) db.create<account_object>([&](auto& a) { a.name = n; });
      base = graph_capture(view);
   }
};

struct secondary_live_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
std::vector<char> secondary_live_data(unsigned family, uint64_t pk, uint64_t nonce, bool seed = false) {
   std::vector<char> data(80);
   put(data, 0, uint32_t{64}); put(data, 4, uint32_t{192}); put(data, 8, uint32_t{seed ? 7u : 1u});
   put(data, 12, uint32_t{2}); put(data, 16, uint64_t{1}); put(data, 24, pk);
   const auto bits = key_bits(family, !seed); std::memcpy(data.data() + 32, bits.data(), 32);
   put(data, 64, uint32_t{seed ? 0u : 1u}); put(data, 72, nonce); return data;
}
signed_transaction secondary_live_transaction(eosio::testing::tester& chain, const std::vector<isolated_action_input>& roots) {
   signed_transaction trx;
   for (const auto& root : roots)
      trx.actions.emplace_back(std::vector<permission_level>{{code_account, "active"_n}}, root.account, root.action, root.data);
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(code_account, "active"), chain.control->get_chain_id()); return trx;
}
std::vector<char> secondary_live_trace(const transaction_trace_ptr& trace, bool allow_failure = false) {
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->receipt), fc::raw::pack(trace->account_ram_delta), fc::raw::pack(trace->error_code),
      fc::raw::pack(trace->except ? trace->except->code() : int64_t{0})};
   BOOST_REQUIRE((allow_failure || !trace->except) && !trace->failed_dtrx_trace);
   for (const auto& action : trace->action_traces) {
      fields.push_back(fc::raw::pack(action.action_ordinal)); fields.push_back(fc::raw::pack(action.creator_action_ordinal));
      fields.push_back(fc::raw::pack(action.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(action.receiver)); fields.push_back(fc::raw::pack(action.act));
      fields.push_back(fc::raw::pack(action.receipt)); fields.push_back(fc::raw::pack(action.context_free));
      fields.push_back(action.return_value); fields.push_back(fc::raw::pack(action.account_ram_deltas));
      fields.push_back(fc::raw::pack(action.console)); fields.push_back(fc::raw::pack(action.console_markers));
      fields.push_back(fc::raw::pack(action.error_code)); fields.push_back(fc::raw::pack(action.except ? action.except->code() : int64_t{0}));
      BOOST_REQUIRE(allow_failure || (!action.except && !action.error_code));
      for (const auto& call : action.call_traces) {
         fields.push_back(fc::raw::pack(call.call_ordinal)); fields.push_back(fc::raw::pack(call.sender_ordinal));
         fields.push_back(fc::raw::pack(call.receiver)); fields.push_back(fc::raw::pack(call.read_only));
         fields.push_back(call.data); fields.push_back(call.return_value); fields.push_back(fc::raw::pack(call.error_id));
         fields.push_back(fc::raw::pack(call.console)); fields.push_back(fc::raw::pack(call.console_markers));
         fields.push_back(fc::raw::pack(call.error_code)); fields.push_back(fc::raw::pack(call.except ? call.except->code() : int64_t{0}));
         BOOST_REQUIRE(allow_failure || (!call.except && !call.error_code));
      }
   }
   return fc::raw::pack(fields);
}
enum class secondary_live_mode { updates, mixed, sync_only, late_authority };
struct secondary_live_corpus {
   genesis_state genesis = eosio::testing::base_tester::default_genesis();
   flat_set<digest_type> reviewed;
   std::vector<std::vector<char>> blocks;
   bool console_enabled = true;
   secondary_live_corpus(unsigned family, secondary_live_mode mode, bool with_console = false, bool enabled = true)
      : console_enabled(enabled) {
      std::vector<isolated_sync_program> programs;
      if (mode == secondary_live_mode::updates) programs.push_back({{code_account, secondary_program(family), {}}, false});
      else programs = secondary_graph(family, mode == secondary_live_mode::sync_only,
         mode == secondary_live_mode::late_authority, with_console);
      for (const auto& p : programs) reviewed.insert(fc::sha256::hash(p.program.code.data(), p.program.code.size()));
      auto [key, pub, proof] = eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key = pub;
      genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory dir; secondary_live_chain source(configuration(dir, 0), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call, builtin_protocol_feature_t::get_sender_fix});
      seal(); seal(); source.create_accounts({code_account, "bob"_n, "carol"_n, "empty"_n}); seal();
      for (const auto& p : programs) source.set_code(p.program.receiver, std::vector<uint8_t>(p.program.code.begin(), p.program.code.end()));
      seal();
      if (mode == secondary_live_mode::updates) {
         auto root = graph_roots().front(); root.data = secondary_live_data(family, 17, 1, true);
         auto seed = secondary_live_transaction(source, {root});
         BOOST_REQUIRE(!source.push_transaction(seed)->except); seal();
      }
   }
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool production = false) const {
      auto cfg = eosio::testing::base_tester::default_config(dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg); cfg.contracts_console = console_enabled;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      cfg.parallel_code_allowlist = reviewed; return cfg;
   }
   void prefix(eosio::testing::tester& chain) const {
      for (const auto& bytes : blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes))); chain.control->abort_block();
      }
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_secondary_wasm_oracle_tests)

BOOST_AUTO_TEST_CASE(selected_secondary_profile_configures_canonical_and_private_engines) {
   secondary_live_corpus corpus(0, secondary_live_mode::mixed);
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

static void check_secondary_console(const transaction_trace_ptr& trace, bool enabled,
      secondary_live_mode mode, bool late_failure = false) {
   const auto frame = [&](const auto& value, const std::string& output, std::initializer_list<uint32_t> markers) {
      BOOST_CHECK_EQUAL(value.console, enabled ? output : std::string{});
      BOOST_REQUIRE_EQUAL(value.console_markers.size(), enabled ? markers.size() : 0u);
      if (enabled) {
         size_t index = 0;
         for (uint32_t position : markers) BOOST_CHECK_EQUAL(value.console_markers[index++].value, position);
      }
   };
   BOOST_REQUIRE(trace && !trace->action_traces.empty());
   const auto& root = trace->action_traces.front();
   if (root.act.name == "noop"_n) {
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
      frame(root, "12", {}); BOOST_CHECK(root.call_traces.empty()); return;
   }
   const bool sync = mode == secondary_live_mode::sync_only;
   const bool authorization_failure = late_failure && mode == secondary_live_mode::late_authority;
   const bool negative_child = root.act.name == "business"_n;
   frame(root, authorization_failure ? "102030405060" : "10203040506070",
      sync ? std::initializer_list<uint32_t>{8, 12} : std::initializer_list<uint32_t>{8});
   BOOST_REQUIRE_EQUAL(root.call_traces.size(), sync ? 3u : 2u);
   frame(root.call_traces[0], "200210211212299", {9});
   frame(root.call_traces[1], negative_child ? "200220221222223" : "200220221222299", {});
   if (negative_child) {
      BOOST_REQUIRE(root.call_traces[1].error_id);
      BOOST_CHECK_EQUAL(*root.call_traces[1].error_id, -10017);
   }
   if (sync) frame(root.call_traces[2], "200240241242299", {});
   // An authority error occurs at send_inline, before queued recipients and
   // inline children execute. Compare their unexecuted traces with the oracle
   // below; do not confuse a trace allocated at send time with an executed one.
   if (authorization_failure) return;
   unsigned reads = 0, notifications = 0, inlines = 0, context_free = 0;
   for (size_t i = 1; i < trace->action_traces.size(); ++i) {
      const auto& action = trace->action_traces[i];
      if (action.receiver == code_account && action.act.name == "read"_n) {
         frame(action, "11", {}); BOOST_CHECK(action.call_traces.empty()); ++reads;
      } else if (action.receiver == "carol"_n) {
         frame(action, "808182", {4}); BOOST_REQUIRE_EQUAL(action.call_traces.size(), 1u);
         frame(action.call_traces[0], "200230299", {}); ++notifications;
      } else if (action.receiver == "bob"_n) {
         frame(action, "100101102", {6}); BOOST_REQUIRE_EQUAL(action.call_traces.size(), 1u);
         frame(action.call_traces[0], "200240241242299", {}); ++inlines;
      } else if (action.receiver == "empty"_n) {
         frame(action, "9091", {}); BOOST_CHECK(action.context_free);
         BOOST_CHECK(action.call_traces.empty()); ++context_free;
      } else BOOST_FAIL("unexpected receiver/action in console secondary graph");
   }
   BOOST_CHECK_EQUAL(reads, 1u); BOOST_CHECK_EQUAL(notifications, sync ? 0u : 1u);
   BOOST_CHECK_EQUAL(inlines, sync ? 0u : 1u); BOOST_CHECK_EQUAL(context_free, sync ? 0u : 1u);
}

static void check_secondary_production(bool with_console, bool enabled = true) {
   for (unsigned family = 0; family < families.size(); ++family)
      for (auto mode : {secondary_live_mode::updates, secondary_live_mode::mixed, secondary_live_mode::sync_only}) {
      if (with_console && mode == secondary_live_mode::updates) continue; // printing variant uses the complete nested graph
      const bool nested = mode != secondary_live_mode::updates;
      secondary_live_corpus corpus(family, mode, with_console, enabled);
      for (uint16_t workers : {1, 2, 4}) BOOST_TEST_CONTEXT("secondary controller family=" << family << " mode=" << unsigned(mode)
         << " workers=" << workers << " prints=" << with_console << " console=" << enabled) {
         fc::temp_directory pd, sd, vd;
         secondary_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis);
         eosio::testing::tester serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         std::vector<transaction_metadata_ptr> batch;
         // First three updates touch different existing primary IDs in the
         // SAME secondary table. Each repeated ID needs current-prefix replay;
         // the stale old secondary key must never be adopted under a new tag.
         const std::array<uint64_t, 5> keys{17, 7, 5, 17, 7};
         for (size_t i = 0; i < (nested ? 1u : keys.size()); ++i) {
            auto roots = graph_roots(nested ? "business"_n : "run"_n);
            if (!nested) { roots.resize(1); roots[0].data = secondary_live_data(family, keys[i], 100 + i); }
            const auto trx = secondary_live_transaction(producer, roots);
            batch.push_back(transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input));
         }
         std::vector<std::vector<char>> expected, original, candidate;
         const auto collect = [&](auto& dest, const auto& event) {
            const auto trace = std::get<0>(event);
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; }))
               dest.push_back(secondary_live_trace(trace));
         };
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) { collect(original, e); });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) { collect(candidate, e); });
         const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
            BOOST_REQUIRE_MESSAGE(trace && !trace->except && trace->receipt,
               (trace && trace->except ? trace->except->to_detail_string() : "missing secondary production receipt"));
            if (with_console) check_secondary_console(trace, enabled, mode);
            if (mode == secondary_live_mode::sync_only) {
               // Only ordinary roots and immediate calls were executed. The
               // controller upgrades tree -> sync only on the actual call;
               // this program never executes a notify/inline/mixed host.
               BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 2u);
               BOOST_REQUIRE_EQUAL(trace->action_traces[0].call_traces.size(), 3u);
               BOOST_CHECK(trace->action_traces[1].call_traces.empty());
               BOOST_REQUIRE(trace->action_traces[0].call_traces[1].error_id);
               BOOST_CHECK_EQUAL(*trace->action_traces[0].call_traces[1].error_id, -10017);
            }
            expected.push_back(secondary_live_trace(trace)); return true;
         }), batch.size());
         const auto after = producer.control->get_parallel_production_stats();
         const auto adopted = batch.size();
         const auto refreshed = nested ? 0u : 2u;
         BOOST_TEST_CONTEXT("attempts=" << after.attempts - before.attempts << " successes=" << after.succeeded - before.succeeded
            << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
            << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines) {
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, adopted);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, batch.size() - adopted);
            BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, refreshed);
            BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, refreshed);
         }
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(original == expected); BOOST_CHECK(candidate == expected);
         BOOST_CHECK_EQUAL(original.size(), batch.size()); BOOST_CHECK_EQUAL(candidate.size(), batch.size());
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, adopted);
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, batch.size() - adopted);
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().prefix_refresh_requests - vb.prefix_refresh_requests, refreshed);
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().prefix_refresh_successes - vb.prefix_refresh_successes, refreshed);
      }
   }
}

BOOST_AUTO_TEST_CASE(five_families_are_actually_adopted_in_production_validation_and_mixed_call_trees) {
   check_secondary_production(false);
}

BOOST_AUTO_TEST_CASE(console_interleaved_with_all_five_secondary_families_matches_original_production_and_validation) {
   for (bool enabled : {false, true}) check_secondary_production(true, enabled);
}

static void check_secondary_late_failure(bool with_console, bool enabled = true) {
   for (unsigned family = 0; family < families.size(); ++family) for (bool ram_failure : {false, true}) {
      const auto mode = ram_failure ? secondary_live_mode::sync_only : secondary_live_mode::late_authority;
      secondary_live_corpus corpus(family, mode, with_console, enabled);
      for (uint16_t workers : {1, 2, 4}) BOOST_TEST_CONTEXT("secondary late failure family=" << family
         << " ram=" << ram_failure << " workers=" << workers << " prints=" << with_console << " console=" << enabled) {
         fc::temp_directory pd, sd, vd;
         secondary_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis),
            serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         const auto ram_limit = [&](int64_t bytes) {
            BOOST_REQUIRE(!producer.push_action("eosio"_n, "setalimits"_n, "eosio"_n, fc::mutable_variant_object()
               ("account", "test")("ram_bytes", bytes)("net_weight", -1)("cpu_weight", -1))->except);
            const auto setup = producer.produce_block(); producer.control->abort_block();
            serial.push_block(setup); serial.control->abort_block(); parallel.push_block(setup); parallel.control->abort_block();
            producer.start();
         };
         if (ram_failure) ram_limit(producer.control->get_resource_limits_manager().get_account_ram_usage(code_account));
         serial.start();
         const auto metadata = [&](const std::vector<isolated_action_input>& roots) {
            const auto trx = secondary_live_transaction(producer, roots);
            return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
               producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
         };
         const auto push = [](eosio::testing::tester& chain, const transaction_metadata_ptr& trx) {
            return chain.control->push_transaction(trx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
         };
         auto bad_roots = graph_roots(ram_failure ? "business"_n : "authfail"_n);
         bad_roots.front().data = fc::raw::pack(uint64_t{100});
         auto tail_root = graph_roots("noop"_n).front(); tail_root.data = fc::raw::pack(uint64_t{101});
         const auto bad = metadata(bad_roots), tail = metadata({tail_root});
         const auto before_state = producer.control->calculate_integrity_hash(), original_state = serial.control->calculate_integrity_hash();
         std::array<int64_t, graph_accounts.size()> original_ram{};
         for (size_t i = 0; i < graph_accounts.size(); ++i)
            original_ram[i] = producer.control->get_resource_limits_manager().get_account_ram_usage(graph_accounts[i]);
         const auto reference = push(serial, bad);
         BOOST_REQUIRE(reference && reference->except && !reference->receipt);
         if (with_console) check_secondary_console(reference, enabled, mode, true);
         BOOST_CHECK(serial.control->calculate_integrity_hash() == original_state);
         const auto before = producer.control->get_parallel_production_stats();
         size_t callbacks = 0; std::vector<char> accepted_tail;
         const auto cost_label="secondary family="+std::to_string(family)+" ram="+std::to_string(ram_failure)+
            " workers="+std::to_string(workers);
         BOOST_CHECK_EQUAL(secondary_test_measure((cost_label+" phase=late").c_str(),[&] {
            return producer.control->for_each_parallel_production_transaction({bad, tail}, fc::time_point::maximum(), [&](const auto& tx) {
            const auto trace = push(producer, tx);
            if (callbacks++ == 0) {
               BOOST_REQUIRE(trace && trace->except && !trace->receipt);
               const int64_t expected_error = ram_failure ? int64_t{ram_usage_exceeded::code_value} : int64_t{unsatisfied_authorization::code_value};
               BOOST_CHECK_EQUAL(trace->except->code(), expected_error);
               if (with_console) check_secondary_console(trace, enabled, mode, true);
               BOOST_CHECK(secondary_live_trace(trace, true) == secondary_live_trace(reference, true));
               BOOST_REQUIRE(!trace->action_traces.empty());
               // Calls preceding the failure occur exactly once. A post-write
               // error must never cause the entire root to execute a second time.
               BOOST_CHECK_EQUAL(trace->action_traces[0].call_traces.size(), ram_failure ? 3u : 2u);
               BOOST_CHECK(producer.control->calculate_integrity_hash() == before_state);
               BOOST_CHECK(!producer.control->is_known_unexpired_transaction(bad->id()));
               chainbase_transaction_state_view live(producer.control->db());
               BOOST_CHECK(!live.read_table(graph_table));
               for (size_t i = 0; i < graph_accounts.size(); ++i)
                  BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(graph_accounts[i]), original_ram[i]);
            } else {
               BOOST_REQUIRE(trace && !trace->except && trace->receipt);
               if (with_console) check_secondary_console(trace, enabled, mode);
               accepted_tail = secondary_live_trace(trace);
            }
            return true;
         }); }), 2u);
         const auto after = producer.control->get_parallel_production_stats();
         const auto late_diagnostics=parallel_attempt_diagnostics(before,after);
         BOOST_TEST_MESSAGE("SECONDARY_ATTEMPT "<<cost_label<<" phase=late adopted="<<after.adopted-before.adopted
            <<" serial_fallbacks="<<after.serial_fallbacks-before.serial_fallbacks<<" "<<late_diagnostics);
         BOOST_TEST_CONTEXT(late_diagnostics) {
            BOOST_CHECK_GT(after.succeeded - before.succeeded, 0u);
            // adopted counts execution, NOT acceptance. The RAM error is in
            // transaction finalization AFTER full sync replay; no receipt exists.
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, ram_failure ? 1u : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, ram_failure ? 1u : 2u);
         }
         serial.control->abort_block();
         std::vector<char> original_tail, parallel_tail;
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) {
            if (std::get<0>(e)->id == tail->id()) original_tail = secondary_live_trace(std::get<0>(e));
         });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) {
            if (std::get<0>(e)->id == tail->id()) parallel_tail = secondary_live_trace(std::get<0>(e));
         });
         const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(accepted_tail == original_tail); BOOST_CHECK(accepted_tail == parallel_tail);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK(!serial.control->is_known_unexpired_transaction(bad->id()));
         producer.start(); if (ram_failure) ram_limit(-1);
         auto healthy_roots = graph_roots("business"_n); healthy_roots.front().data = fc::raw::pack(uint64_t{102});
         const auto healthy = metadata(healthy_roots);
         const auto restart = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
         transaction_trace_ptr healthy_trace;
         BOOST_CHECK_EQUAL(secondary_test_measure((cost_label+" phase=healthy").c_str(),[&] {
            return producer.control->for_each_parallel_production_transaction({healthy}, fc::time_point::maximum(), [&](const auto& tx) {
               healthy_trace = push(producer, tx); return true;
            }); }), 1u);
         BOOST_REQUIRE(healthy_trace && !healthy_trace->except && healthy_trace->receipt);
         if (with_console) check_secondary_console(healthy_trace, enabled, mode);
         const auto recovered = producer.control->get_parallel_production_stats();
         BOOST_TEST_MESSAGE("SECONDARY_ATTEMPT "<<cost_label<<" phase=healthy adopted="<<recovered.adopted-restart.adopted
            <<" serial_fallbacks="<<recovered.serial_fallbacks-restart.serial_fallbacks
            <<" "<<parallel_attempt_diagnostics(restart,recovered));
         BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(restart, recovered)) {
            BOOST_CHECK_EQUAL(recovered.adopted - restart.adopted, 1u);
         }
         std::vector<char> original_healthy, parallel_healthy;
         boost::signals2::scoped_connection sr = serial.control->applied_transaction().connect([&](const auto& e) {
            if (std::get<0>(e)->id == healthy->id()) original_healthy = secondary_live_trace(std::get<0>(e));
         });
         boost::signals2::scoped_connection vr = parallel.control->applied_transaction().connect([&](const auto& e) {
            if (std::get<0>(e)->id == healthy->id()) parallel_healthy = secondary_live_trace(std::get<0>(e));
         });
         const auto recovered_block = producer.finish_block(); BOOST_REQUIRE_EQUAL(recovered_block->transactions.size(), 1u);
         serial.push_block(recovered_block); serial.control->abort_block(); parallel.push_block(recovered_block); parallel.control->abort_block();
         BOOST_CHECK(secondary_live_trace(healthy_trace) == original_healthy); BOOST_CHECK(original_healthy == parallel_healthy);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, 1u);
      }
   }
}

BOOST_AUTO_TEST_CASE(secondary_prefix_undo_on_late_inline_authority_and_sync_final_ram_failure_keeps_tail_and_reuse_safe) {
   check_secondary_late_failure(false);
}

BOOST_AUTO_TEST_CASE(console_prefix_secondary_undo_tail_and_reuse_match_original_after_late_authority_and_final_ram_failure) {
   for (bool enabled : {false, true}) check_secondary_late_failure(true, enabled);
}

BOOST_FIXTURE_TEST_CASE(all_fifty_hosts_replay_through_original_notification_and_tree_adopters, wasm_oracle_fixture) {
   isolated_notification_executor notification_executor; isolated_action_tree_executor tree_executor;
   for (unsigned family = 0; family < families.size(); ++family) {
      deploy(secondary_program(family));
      for (unsigned operation = 0; operation < 10; ++operation)
         BOOST_TEST_CONTEXT("canonical family=" << family << " operation=" << operation) {
            const auto input = arguments(family, operation);
            signed_transaction trx;
            trx.actions.emplace_back(std::vector<permission_level>{{code_account, "active"_n}}, code_account, input.action, input.data);
            original.set_transaction_headers(trx);
            const packed_transaction packed(trx, packed_transaction::compression_type::none);
            const auto prepared = prepare_isolated_transaction(*original.control, packed); BOOST_REQUIRE(prepared);
            const std::array roots{prepared->front().input};
            const auto program = prepare_isolated_receiver_program(*original.control, code_account, roots); BOOST_REQUIRE(program);
            const std::array programs{*program}; const auto base = capture(input);
            const auto notification = notification_executor.execute(base, roots, programs);
            const auto tree = tree_executor.execute(base, roots, programs, current_action_tree_context(*original.control));
            BOOST_REQUIRE(!notification.state.primary_only()); BOOST_REQUIRE(!tree.state.primary_only());
            const auto before = original.control->calculate_integrity_hash();
            digest_type expected_state; std::vector<char> expected_memory;
            for (unsigned mode = 0; mode < 8; ++mode) {
               {
                  platform_timer timer; const auto start = fc::time_point::now();
                  transaction_context transaction(*original.control, packed, packed.id(), transaction_checktime_timer(timer),
                     action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
                  transaction.block_deadline = start + fc::seconds(5);
                  transaction.explicit_billed_cpu_time = true; transaction.billed_cpu_time_us = 100;
                  transaction.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
                  transaction.trace->action_traces.emplace_back(*transaction.trace, trx.actions[0], code_account, false, 1, 0, 0);
                  apply_context context(*original.control, transaction, 1);
                  const auto before_events = original.control->calculate_integrity_hash();
                  auto& resources = original.control->get_mutable_resource_limits_manager();
                  const auto ram_before = resources.get_account_ram_usage(code_account);
                  if (mode >= 5) {
                     int64_t ram, net, cpu; resources.get_account_limits(code_account, ram, net, cpu);
                     // Owner-side test setup in the SAME undo session: allow no
                     // additional RAM. Private success must not bypass original
                     // transaction finalization after all index writes occurred.
                     resources.set_account_limits(code_account, ram_before, net, cpu, false);
                  }
                  if (mode == 0 || mode == 5) original.control->get_wasm_interface().execute(prepared->front().code_hash, 0, 0, context);
                  if (mode == 1 || mode == 6) BOOST_REQUIRE(try_adopt_notification_result(context, prepared->front().code_hash, 0, 0,
                     program->limits, notification, 0));
                  if (mode == 2 || mode == 7) BOOST_REQUIRE(try_adopt_action_tree_result(context, prepared->front().code_hash, 0, 0,
                     program->limits, tree, 0));
                  if (mode == 3) {
                     auto forged = notification; forged.steps.front().cursor_events.erase(forged.steps.front().cursor_events.begin());
                     BOOST_CHECK(!try_adopt_notification_result(context, prepared->front().code_hash, 0, 0, program->limits, forged, 0));
                  }
                  if (mode == 4) {
                     auto forged = tree; forged.steps.front().events.erase(forged.steps.front().events.begin());
                     BOOST_CHECK(!try_adopt_action_tree_result(context, prepared->front().code_hash, 0, 0, program->limits, forged, 0));
                  }
                  if (mode == 0) {
                     expected_state = original.control->calculate_integrity_hash(); expected_memory = context.action_return_value;
                  } else if (mode < 3) {
                     BOOST_CHECK(original.control->calculate_integrity_hash() == expected_state);
                     BOOST_CHECK(context.action_return_value == expected_memory);
                  } else if (mode < 5) {
                     BOOST_CHECK(context.action_return_value.empty());
                     BOOST_CHECK(original.control->calculate_integrity_hash() == before_events);
                  } else {
                     BOOST_CHECK(context.action_return_value == expected_memory);
                     BOOST_CHECK_GT(resources.get_account_ram_usage(code_account), ram_before);
                     BOOST_CHECK_THROW(transaction.finalize(), ram_usage_exceeded);
                  }
               }
               BOOST_CHECK(original.control->calculate_integrity_hash() == before);
            }
         }
   }
}

BOOST_FIXTURE_TEST_CASE(all_fifty_hosts_match_original_wasm_state_handles_and_raw_memory, wasm_oracle_fixture) {
   for (unsigned family = 0; family < families.size(); ++family) {
      const auto code = secondary_program(family); deploy(code);
      const std::vector<isolated_receiver_program> programs{{code_account, code, {}}};
      isolated_notification_executor executor;
      for (unsigned op = 0; op < 10; ++op) BOOST_TEST_CONTEXT(families[family] << " operation=" << op) {
         compare(family, executor, programs, arguments(family, op));
      }
   }
}

BOOST_FIXTURE_TEST_CASE(bound_outputs_preserve_aligned_misaligned_aliases_and_misses, wasm_oracle_fixture) {
   const std::array<std::pair<uint32_t, uint32_t>, 9> pointers{{{64, 192}, {65, 193}, {64, 64},
      {65, 65}, {64, 65}, {65, 64}, {64, 72}, {72, 64}, {65, 72}}};
   for (unsigned family = 0; family < families.size(); ++family) {
      const auto code = secondary_program(family); deploy(code);
      const std::vector<isolated_receiver_program> programs{{code_account, code, {}}}; isolated_notification_executor executor;
      for (unsigned op : {3, 4, 5, 6}) for (bool missing : {false, true}) for (const auto& [key, output] : pointers)
         BOOST_TEST_CONTEXT(families[family] << " operation=" << op << " missing=" << missing << " pointers=" << key << "," << output) {
            auto input = arguments(family, op, key, output); put(input.data, 64, uint32_t(missing));
            compare(family, executor, programs, input);
         }
   }
}

BOOST_FIXTURE_TEST_CASE(invalid_pointers_counts_and_nan_discard_private_prefix_and_allow_reuse, wasm_oracle_fixture) {
   for (unsigned family = 0; family < families.size(); ++family) {
      const auto code = secondary_program(family); deploy(code);
      const std::vector<isolated_receiver_program> programs{{code_account, code, {}}}; isolated_notification_executor executor;
      std::vector<isolated_action_input> invalid;
      if (family != 2) invalid.push_back(arguments(family, 5, 0));
      for (unsigned op : {0, 1, 3, 4, 5, 6}) invalid.push_back(arguments(family, op, 65535));
      for (unsigned op : {4, 5, 6, 8, 9}) {
         invalid.push_back(arguments(family, op, 64, 0)); invalid.push_back(arguments(family, op, 64, 65535));
      }
      if (family == 2) for (uint32_t count : {0u, 1u, 3u, 0xffffffffu}) {
         auto in = arguments(family, 5); put(in.data, 12, count); invalid.push_back(in);
      }
      if (family >= 3) for (unsigned op : {0, 1, 4, 5, 6}) for (bool missing : {false, true}) {
         // Read NaN before looking up even an absent table. No host-native FP.
         if (op <= 1 && missing) continue;
         auto in = arguments(family, op); put(in.data, family == 3 ? 32 : 40,
            family == 3 ? uint64_t{0x7ff8000000000000} : uint64_t{0x7fff800000000000});
         put(in.data, 64, uint32_t(missing)); invalid.push_back(in);
      }
      for (const auto& input : invalid) BOOST_TEST_CONTEXT(families[family] << " invalid scope=" << table(input).scope) {
         const auto before = original.control->calculate_integrity_hash();
         const std::array roots{input};
         BOOST_CHECK_THROW(executor.execute(capture(input), roots, programs), primary_state_unsupported);
         BOOST_CHECK_THROW(push(input), fc::exception);
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
      }
      if (family == 2) compare(family, executor, programs, arguments(family, 5, 0)); // valid span at zero
      if (family >= 3) {
         auto out_nan = arguments(family, 3);
         put(out_nan.data, family == 3 ? 32 : 40, family == 3 ? uint64_t{0x7ff8000000000000} : uint64_t{0x7fff800000000000});
         compare(family, executor, programs, out_nan); // output is not an input NaN check
      }
      compare(family, executor, programs, arguments(family, 5));
   }
}

BOOST_FIXTURE_TEST_CASE(existing_rows_use_owned_discovery_and_actual_hosts_in_all_private_proofs, wasm_oracle_fixture) {
   for (unsigned family = 0; family < families.size(); ++family) {
      const auto code = secondary_program(family); deploy(code);
      const std::vector<isolated_receiver_program> programs{{code_account, code, {}}};
      const std::vector<isolated_sync_program> sync_programs{{programs[0], false}};
      const auto seed = arguments(family, 7); BOOST_REQUIRE(push(seed)); const auto saved_scope = table(seed).scope.to_uint64_t();
      isolated_notification_executor ne; isolated_action_tree_executor te; isolated_sync_executor se; isolated_mixed_tree_executor me;
      for (unsigned op : {3, 4, 5, 6}) for (bool miss : {false, true})
         BOOST_TEST_CONTEXT(families[family] << " operation=" << op << " missing row=" << miss) {
            auto input = arguments(family, op); put(input.data, 16, saved_scope); put(input.data, 64, uint32_t{1});
            if (miss) {
               put(input.data, 24, uint64_t{99});
               // A value above every stored key, with exact finite/infinite
               // bits as appropriate. Existing-table misses also cache end.
               const auto bits = family < 3 ? std::array<uint64_t, 4>{~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0}} :
                  family == 3 ? std::array<uint64_t, 4>{0x7ff0000000000000, 0, 0, 0} :
                                std::array<uint64_t, 4>{0, 0x7fff000000000000, 0, 0};
               std::memcpy(input.data.data() + 32, bits.data(), 32);
            }
            const std::array roots{input}; std::vector<secondary_query> reads;
            std::optional<isolated_notification_result> result; std::shared_ptr<const frozen_primary_state> base;
            for (unsigned round = 0; round < 8 && !result; ++round) {
               base = capture(input, reads);
               try { result = ne.execute(base, roots, programs); }
               catch (const primary_state_missing& missing) {
                  const auto* request = std::get_if<secondary_capture_request>(&missing.request);
                  BOOST_REQUIRE(request); BOOST_CHECK(request->query.table == table(input));
                  BOOST_CHECK(secondary_kind(request->query.key) == static_cast<secondary_key_kind>(family));
                  reads.push_back(request->query);
               }
            }
            BOOST_REQUIRE(result); BOOST_REQUIRE_EQUAL(reads.size(), 1u);
            BOOST_CHECK(validate_isolated_notification_journal(*result, roots, programs));
            BOOST_CHECK(result->state.writes().empty()); BOOST_CHECK(!result->state.primary_only());
            const auto tree = te.execute(base, roots, programs, {});
            BOOST_CHECK(validate_isolated_action_tree_journal(tree, roots, programs, {}));
            isolated_sync_context readonly; readonly.root_read_only = true;
            const auto single = se.execute(base, input, sync_programs, readonly);
            BOOST_CHECK(validate_isolated_sync_journal(single, input, sync_programs, readonly));
            const std::array twice{input, input};
            const auto multi = se.execute_transaction(base, twice, sync_programs, readonly);
            BOOST_CHECK(validate_isolated_sync_transaction_journal(multi, twice, sync_programs, readonly));
            const auto mixed = me.execute(base, roots, sync_programs, {});
            BOOST_CHECK(validate_isolated_mixed_tree_journal(mixed, roots, sync_programs, {}));
            const auto trace = push(input); BOOST_REQUIRE(trace && !trace->except);
            const auto& expected = trace->action_traces.at(0).return_value;
            BOOST_CHECK(result->steps.at(0).effects.return_value == expected);
            BOOST_CHECK(tree.steps.at(0).effects.return_value == expected);
            BOOST_CHECK(single.root.return_value == expected);
            for (const auto& root : multi.roots) BOOST_CHECK(root.root.return_value == expected);
            BOOST_CHECK(mixed.steps.at(0).effects.return_value == expected);
            // Actual imported store must not pass a readonly frame, even if
            // its key/table was already captured. Failed attempt is reusable.
            auto write = input; put(write.data, 8, uint32_t{0});
            BOOST_CHECK_THROW(se.execute(base, write, sync_programs, readonly), primary_state_unsupported);
            BOOST_CHECK(validate_isolated_sync_journal(se.execute(base, input, sync_programs, readonly), input, sync_programs, readonly));
         }
   }
}

BOOST_FIXTURE_TEST_CASE(floating_signed_zero_and_infinities_keep_original_order_and_exact_output_bits, wasm_oracle_fixture) {
   for (unsigned family : {3, 4}) {
      const auto code = secondary_program(family, true); deploy(code);
      const std::vector<isolated_receiver_program> programs{{code_account, code, {}}}; isolated_notification_executor executor;
      for (bool negative : {false, true}) for (bool infinity : {false, true}) for (unsigned op : {3, 4, 5, 6})
         BOOST_TEST_CONTEXT(families[family] << " negative=" << negative << " infinity=" << infinity << " operation=" << op) {
            auto input = arguments(family, op); std::array<uint64_t, 4> bits{};
            bits[family == 3 ? 0 : 1] = (negative ? 0x8000000000000000ULL : 0) |
               (infinity ? (family == 3 ? 0x7ff0000000000000ULL : 0x7fff000000000000ULL) : 0);
            std::memcpy(input.data.data() + 32, bits.data(), 32);
            compare(family, executor, programs, input);
         }
   }
}

BOOST_AUTO_TEST_CASE(nested_secondary_writes_notifications_inline_and_negative_sync_match_original) {
   for (unsigned family = 0; family < families.size(); ++family) for (name action_name : {"run"_n, "business"_n})
      BOOST_TEST_CONTEXT(families[family] << " root=" << action_name) {
         eosio::testing::tester chain; chain.create_accounts({code_account, "bob"_n, "carol"_n, "empty"_n});
         auto programs = secondary_graph(family); auto context = graph_context();
         for (auto& p : programs) {
            chain.set_code(p.program.receiver, std::vector<uint8_t>(p.program.code.begin(), p.program.code.end()));
            const auto& account = chain.control->db().get<account_metadata_object, by_name>(p.program.receiver);
            p.sync_call_supported = chain.control->db().get<code_object, by_code_hash>(
               boost::make_tuple(account.code_hash, account.vm_type, account.vm_version)).sync_call_supported;
         }
         chain.produce_block();
         const auto& config = chain.control->get_global_properties().configuration;
         context.tree.max_inline_action_size = config.max_inline_action_size;
         context.tree.max_inline_action_depth = config.max_inline_action_depth;
         context.sync = {config.max_sync_call_depth, config.max_sync_call_data_size, false,
            chain.control->is_builtin_activated(builtin_protocol_feature_t::get_sender_fix), chain.control->contracts_console()};
         context.tree.console_enabled = context.sync.console_enabled;
         const auto push = [&](const std::vector<isolated_action_input>& roots) {
            signed_transaction trx;
            for (const auto& r : roots) trx.actions.emplace_back(std::vector<permission_level>{{code_account, "active"_n}}, r.account, r.action, r.data);
            chain.set_transaction_headers(trx); trx.sign(chain.get_private_key(code_account, "active"), chain.control->get_chain_id());
            return chain.push_transaction(trx, fc::time_point::maximum(), eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US);
         };
         chainbase_transaction_state_view live(chain.control->db()); const auto base = graph_capture(live);
         isolated_mixed_tree_executor executor;
         if (action_name == "run"_n) for (name failure : {"stale"_n, "foreign"_n, "cfbad"_n, "readonly"_n}) {
            const auto bad = graph_roots(failure), healthy = graph_roots();
            const auto before = chain.control->calculate_integrity_hash();
            const auto ram_before = chain.control->get_resource_limits_manager().get_account_ram_usage(code_account);
            BOOST_CHECK_THROW(push(bad), fc::exception);
            if (failure == "foreign"_n)
               BOOST_CHECK_THROW(executor.execute(base, bad, programs, context), table_access_violation);
            else BOOST_CHECK_THROW(executor.execute(base, bad, programs, context), primary_state_unsupported);
            BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
            BOOST_CHECK_EQUAL(chain.control->get_resource_limits_manager().get_account_ram_usage(code_account), ram_before);
            BOOST_CHECK(validate_isolated_mixed_tree_journal(executor.execute(base, healthy, programs, context), healthy, programs, context));
         }
         const auto roots = graph_roots(action_name);
         // Preserve ORIGINAL behavior first, even if private execution fails.
         const auto actual = push(roots); BOOST_REQUIRE(actual && !actual->except);
         const auto after = chain.control->calculate_integrity_hash();
         const auto result = executor.execute(base, roots, programs, context);
         BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result, roots, programs, context));
         BOOST_CHECK(!result.state.primary_only()); BOOST_REQUIRE_EQUAL(result.state.writes().size(), 8u);
         BOOST_REQUIRE_EQUAL(result.steps.size(), 5u); BOOST_REQUIRE_EQUAL(actual->action_traces.size(), result.steps.size());
         size_t calls = 0, negative_calls = 0;
         for (const auto& step : result.steps) {
            const auto& trace = actual->action_traces.at(step.action_ordinal - 1);
            BOOST_CHECK(trace.receiver == step.receiver); BOOST_CHECK(trace.return_value == step.effects.return_value);
            BOOST_CHECK_EQUAL(trace.creator_action_ordinal.value, step.creator_action_ordinal);
            BOOST_CHECK_EQUAL(trace.closest_unnotified_ancestor_action_ordinal.value, step.closest_unnotified_ancestor);
            BOOST_REQUIRE_EQUAL(trace.call_traces.size(), step.calls.size());
            for (size_t i = 0; i < step.calls.size(); ++i) {
               const auto& a = trace.call_traces[i]; const auto& b = step.calls[i]; ++calls;
               BOOST_CHECK_EQUAL(a.call_ordinal.value, b.call_ordinal); BOOST_CHECK_EQUAL(a.sender_ordinal.value, b.sender_ordinal);
               BOOST_CHECK(a.receiver == b.receiver); BOOST_CHECK(a.data == b.data); BOOST_CHECK(a.return_value == b.return_value);
               BOOST_CHECK_EQUAL(a.read_only, b.read_only); BOOST_CHECK(!a.except && !a.error_code);
               BOOST_CHECK_EQUAL(bool(a.error_id), b.status < 0);
               if (b.status < 0) { ++negative_calls; BOOST_REQUIRE(a.error_id); BOOST_CHECK_EQUAL(*a.error_id, b.status); }
            }
         }
         BOOST_CHECK_EQUAL(calls, 4u); BOOST_CHECK_EQUAL(negative_calls, action_name == "business"_n ? 1u : 0u);
         primary_state_overlay expected(base);
         for (const auto& write : result.state.writes()) {
            if (const auto* w = std::get_if<primary_mutation>(&write)) {
               if (w->operation == primary_operation::store) expected.store(w->table, w->primary_key, w->payer, w->value);
               else if (w->operation == primary_operation::update) expected.update_existing(w->table, w->primary_key, w->payer, w->value);
               else expected.remove(w->table, w->primary_key);
            } else {
               const auto& sw = std::get<secondary_mutation>(write);
               if (sw.operation == primary_operation::store) expected.store_secondary(sw.table, sw.primary_key, sw.payer, sw.key);
               else if (sw.operation == primary_operation::update) expected.update_secondary(sw.table, sw.primary_key, sw.payer, sw.key);
               else expected.remove_secondary(sw.table, sw.primary_key, sw.key);
            }
         }
         chainbase_secondary_state_view secondary(live);
         const auto key = std::get<secondary_mutation>(result.state.writes().at(1)).key;
         for (uint64_t pk : {7, 17}) {
            const secondary_query q{graph_table, secondary_query_kind::find_primary, key, pk};
            const auto a = expected.read_secondary(q), b = secondary.read_secondary(q);
            BOOST_REQUIRE(a.row && b.row && a.table && b.table);
            BOOST_CHECK(a.row->key == b.row->key); BOOST_CHECK(a.row->payer == b.row->payer);
            BOOST_CHECK(a.table->payer == b.table->payer); BOOST_CHECK_EQUAL(a.table->count, 2u); BOOST_CHECK_EQUAL(b.table->count, 2u);
         }
         BOOST_CHECK(!live.read_primary({graph_table, primary_query_kind::find, 100}, 8).row);
         BOOST_CHECK(chain.control->calculate_integrity_hash() == after);
      }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_secondary_wasm_private_tests)

BOOST_FIXTURE_TEST_CASE(nested_secondary_graph_is_worker_owned_on_one_two_four_threads_and_recovers, secondary_graph_fixture) {
   for (unsigned family = 0; family < families.size(); ++family) {
      const auto programs = secondary_graph(family); const auto context = graph_context();
      for (size_t threads : {1, 2, 4}) BOOST_TEST_CONTEXT(families[family] << " workers=" << threads) {
         isolated_worker_pool pool(threads);
         for (unsigned round = 0; round < 3; ++round) {
            std::vector<isolated_mixed_tree_job> inputs;
            std::vector<ordered_attempt_batch::request> requests;
            for (size_t i = 0; i < threads * 2; ++i) {
               const auto action = (i == 0 && round == 1) ? "stale"_n : i % 2 ? "business"_n : "run"_n;
               inputs.push_back({graph_roots(action), programs, context});
               requests.push_back({fc::sha256::hash(std::to_string(i)),
                  isolated_mixed_tree_fingerprint(inputs.back().roots, programs, context)});
            }
            ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
            for (size_t i = 0; i < inputs.size(); ++i) jobs.emplace_back(batch.get_ticket(i), inputs[i]);
            const auto outputs = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(outputs.size(), inputs.size());
            for (size_t i = 0; i < outputs.size(); ++i) {
               if (i == 0 && round == 1) {
                  BOOST_CHECK(!outputs[i].mixed_result); BOOST_CHECK(outputs[i].failure == isolated_worker_failure::unsupported); continue;
               }
               BOOST_REQUIRE_MESSAGE(outputs[i].mixed_result, "round=" << round << " item=" << i << " failure=" << isolated_worker_failure_name(outputs[i].failure));
               const auto& result = *outputs[i].mixed_result;
               BOOST_CHECK(validate_isolated_mixed_tree_journal(result, inputs[i].roots, programs, context));
               BOOST_CHECK(!result.state.primary_only()); BOOST_CHECK_EQUAL(result.state.writes().size(), 8u);
               BOOST_CHECK_EQUAL(result.steps.size(), 5u);
               // The coordinator still MUST refuse secondary canonical replay.
               // No callback/final chain mutation is invoked by this test.
            }
         }
      }
   }
   BOOST_CHECK(!view.read_table(graph_table));
}

BOOST_AUTO_TEST_SUITE_END()
