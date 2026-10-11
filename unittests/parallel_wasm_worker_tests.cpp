#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/vm/exceptions.hpp>
#include <eosio/testing/tester.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <barrier>
#include <cstring>
#include <future>
#include "parallel_audited_intrinsics.hpp"

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key rows{"test"_n, "alice"_n, "rows"_n};

std::vector<char> wasm(const std::string& text) {
   const auto bytes = wast_to_wasm(text);
   return {bytes.begin(), bytes.end()};
}

// All imports are real chain intrinsics. No test-only host/barrier is imported.
std::vector<char> increment_wasm(const std::string& extra_imports = {}, const std::string& tail = {},
                                 const std::string& before_update = {}) {
   return wasm(R"(
      (module )" + extra_imports + R"(
         (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
         (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
         (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
         (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
         (import "env" "set_action_return_value" (func $return (param i32 i32)))
         (memory 1 1)
         (func (export "apply") (param $receiver i64) (param i64) (param i64)
            (local $iterator i32) (local $count i32)
            (drop (call $read (i32.const 0) (i32.const 40)))
            (set_local $iterator (call $find (get_local $receiver) (i64.load (i32.const 0))
               (i64.load (i32.const 32)) (i64.load (i32.const 8))))
            (drop (call $get (get_local $iterator) (i32.const 64) (i32.const 8)))
            (i64.store (i32.const 64) (i64.add (i64.load (i32.const 64)) (i64.load (i32.const 16))))
            )" + before_update + R"(
            (call $update (get_local $iterator) (i64.const 0) (i32.const 64) (i32.const 8))
            (if (i32.eq (i32.load (i32.const 28)) (i32.const 1))
               (then (loop $forever (br $forever))))
            (if (i32.eq (i32.load (i32.const 28)) (i32.const 2))
               (then (call $update (get_local $iterator) (i64.const 0) (i32.const 64) (i32.const 7))))
            (if (i32.eq (i32.load (i32.const 28)) (i32.const 3))
               (then (drop (call $get (get_local $iterator) (i32.const 65535) (i32.const 8)))))
            (set_local $count (i32.load (i32.const 24)))
            (block $done (loop $work
               (br_if $done (i32.eqz (get_local $count)))
               (set_local $count (i32.sub (get_local $count) (i32.const 1)))
               (br $work)))
            )" + tail + R"(
            (call $return (i32.const 64) (i32.const 8))))
   )");
}

isolated_action_input input(uint64_t primary = 1, uint64_t delta = 7, uint32_t work = 0, uint32_t mode = 0) {
   isolated_action_input result;
   result.receiver = result.account = rows.code;
   result.action = "inc"_n;
   result.authorizations = {{"alice"_n, "active"_n}};
   result.data.resize(40);
   const auto scope = rows.scope.to_uint64_t(), table = rows.table.to_uint64_t();
   std::memcpy(result.data.data(), &scope, 8);
   std::memcpy(result.data.data() + 8, &primary, 8);
   std::memcpy(result.data.data() + 16, &delta, 8);
   std::memcpy(result.data.data() + 24, &work, 4);
   std::memcpy(result.data.data() + 28, &mode, 4);
   std::memcpy(result.data.data() + 32, &table, 8);
   return result;
}

uint64_t number(const std::vector<char>& bytes) {
   if (bytes.size() != 8) throw std::logic_error("expected uint64 payload");
   uint64_t result;
   std::memcpy(&result, bytes.data(), 8);
   return result;
}

fc::sha256 single_transaction_fingerprint(const fc::sha256& hash, const isolated_wasm_limits& limits,
                                         const isolated_action_input& input) {
   const std::array fingerprints{isolated_execution_fingerprint(hash, limits, input)};
   return isolated_transaction_fingerprint(fingerprints);
}

struct fallback_import { const char* name; const char* signature; const char* invocation; };
const std::array fallback_imports{
   fallback_import{"prints_l", "(param i32 i32)", "(call $stub (i32.const 0) (i32.const 0))"},
   fallback_import{"send_inline", "(param i32 i32)", "(call $stub (i32.const 0) (i32.const 0))"},
   fallback_import{"set_blockchain_parameters_packed", "(param i32 i32)", "(call $stub (i32.const 0) (i32.const 0))"},
   fallback_import{"get_blockchain_parameters_packed", "(param i32 i32) (result i32)", "(drop (call $stub (i32.const 0) (i32.const 0)))"},
   fallback_import{"set_proposed_producers", "(param i32 i32) (result i64)", "(drop (call $stub (i32.const 0) (i32.const 0)))"},
   fallback_import{"current_time", "(result i64)", "(drop (call $stub))"},
   fallback_import{"get_active_producers", "(param i32 i32) (result i32)", "(drop (call $stub (i32.const 0) (i32.const 0)))"}
};

// Compare actual chain host conversion + implementation with the isolated
// path. Each result contains the return offset/sign AND modified memory bytes.
struct host_case { const char* label; const char* body; bool success; };
const std::array primitive_cases{
   host_case{"memcpy_unaligned", "(i32.store (i32.const 128) (call $copy (i32.const 161) (i32.const 257) (i32.const 3)))", true},
   host_case{"memcpy_zero_end_source", "(i32.store (i32.const 128) (call $copy (i32.const 160) (i32.const 65536) (i32.const 0)))", true},
   host_case{"memcpy_overlap", "(drop (call $copy (i32.const 257) (i32.const 256) (i32.const 3)))", false},
   host_case{"memcpy_self", "(drop (call $copy (i32.const 256) (i32.const 256) (i32.const 1)))", false},
   host_case{"memcpy_oob", "(drop (call $copy (i32.const 65535) (i32.const 256) (i32.const 2)))", false},
   host_case{"memcpy_zero_end_destination", "(drop (call $copy (i32.const 65536) (i32.const 256) (i32.const 0)))", false},
   host_case{"memmove_overlap_right", "(drop (call $move (i32.const 257) (i32.const 256) (i32.const 3))) (drop (call $copy (i32.const 160) (i32.const 256) (i32.const 4)))", true},
   host_case{"memmove_overlap_left", "(drop (call $move (i32.const 256) (i32.const 257) (i32.const 3))) (drop (call $copy (i32.const 160) (i32.const 256) (i32.const 4)))", true},
   host_case{"memmove_self", "(i32.store (i32.const 128) (call $move (i32.const 160) (i32.const 160) (i32.const 4)))", true},
   host_case{"memmove_zero_end_destination", "(drop (call $move (i32.const 65536) (i32.const 256) (i32.const 0)))", false},
   host_case{"memcmp_unsigned_positive", "(i32.store (i32.const 128) (call $compare (i32.const 256) (i32.const 258) (i32.const 1)))", true},
   host_case{"memcmp_unsigned_negative", "(i32.store (i32.const 128) (call $compare (i32.const 258) (i32.const 256) (i32.const 1)))", true},
   host_case{"memcmp_equal", "(i32.store (i32.const 128) (call $compare (i32.const 256) (i32.const 256) (i32.const 4)))", true},
   host_case{"memcmp_zero_end", "(i32.store (i32.const 128) (call $compare (i32.const 65536) (i32.const 65536) (i32.const 0)))", true},
   host_case{"memcmp_overflow", "(drop (call $compare (i32.const 256) (i32.const 257) (i32.const -1)))", false},
   host_case{"memset_low_byte", "(i32.store (i32.const 128) (call $fill (i32.const 161) (i32.const 511) (i32.const 4)))", true},
   host_case{"memset_negative_byte", "(i32.store (i32.const 128) (call $fill (i32.const 161) (i32.const -1) (i32.const 4)))", true},
   host_case{"memset_zero_null", "(i32.store (i32.const 128) (call $fill (i32.const 0) (i32.const 1) (i32.const 0)))", true},
   host_case{"memset_zero_end", "(drop (call $fill (i32.const 65536) (i32.const 1) (i32.const 0)))", false},
   host_case{"memset_overflow", "(drop (call $fill (i32.const -1) (i32.const 1) (i32.const 4)))", false},
   host_case{"assert_noncanonical_true", "(call $assert (i32.const -1) (i32.const 256))", true},
   host_case{"assert_false", "(call $assert (i32.const 0) (i32.const 256))", false},
   host_case{"assert_true_bad_pointer", "(call $assert (i32.const 1) (i32.const 65536))", false},
   host_case{"assert_true_unterminated", "(i32.store8 (i32.const 65535) (i32.const 65)) (call $assert (i32.const 1) (i32.const 65535))", false},
   host_case{"assert_message_noncanonical_true", "(call $message (i32.const 2) (i32.const 256) (i32.const 4))", true},
   host_case{"assert_message_false", "(call $message (i32.const 0) (i32.const 256) (i32.const 4))", false},
   host_case{"assert_message_true_oob", "(call $message (i32.const 1) (i32.const 65535) (i32.const 2))", false},
   host_case{"assert_message_zero_end", "(call $message (i32.const 1) (i32.const 65536) (i32.const 0))", true},
   host_case{"assert_code_true_reserved", "(call $code (i32.const 256) (i64.const -1))", true},
   host_case{"assert_code_false_user", "(call $code (i32.const 0) (i64.const 42))", false},
   host_case{"assert_code_false_reserved", "(call $code (i32.const 0) (i64.const -1))", false},
   host_case{"abort", "(call $abort)", false}
};

std::vector<char> primitive_wasm(const host_case& item) {
   return wasm(R"((module
      (import "env" "memcpy" (func $copy (param i32 i32 i32) (result i32)))
      (import "env" "memmove" (func $move (param i32 i32 i32) (result i32)))
      (import "env" "memcmp" (func $compare (param i32 i32 i32) (result i32)))
      (import "env" "memset" (func $fill (param i32 i32 i32) (result i32)))
      (import "env" "eosio_assert" (func $assert (param i32 i32)))
      (import "env" "eosio_assert_message" (func $message (param i32 i32 i32)))
      (import "env" "eosio_assert_code" (func $code (param i32 i64)))
      (import "env" "abort" (func $abort))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1) (data (i32.const 256) "\ff\7f\01\00")
      (func (export "apply") (param i64 i64 i64) )" + std::string(item.body) + R"(
         (call $return (i32.const 128) (i32.const 64)))))");
}

std::vector<char> growth_wasm(const std::string& declaration, uint32_t growth, bool at_start = false) {
   const std::string body = "(i32.store (i32.const 0) (grow_memory (i32.const " + std::to_string(growth) +
      "))) (i32.store (i32.const 4) (current_memory))";
   return wasm(R"((module
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      )" + declaration + (at_start ? " (func $start " + body + ") (start $start)" : "") + R"(
      (func (export "apply") (param i64 i64 i64) )" + (at_start ? "" : body) + R"(
         (call $return (i32.const 0) (i32.const 8)))))");
}

std::unique_ptr<isolated_wasm_module> prepare_for_gate(const std::vector<char>& code,
      std::barrier<>& gate, isolated_wasm_limits limits = {}) {
   try { return std::make_unique<isolated_wasm_module>(code, limits); }
   catch (...) { gate.arrive_and_drop(); throw; } // do not strand peers on constructor/allocation failure
}

transaction_trace_ptr push_raw(eosio::testing::tester& chain, const isolated_action_input& arguments) {
   action act;
   act.account = arguments.account; act.name = arguments.action; act.data = arguments.data;
   for (const auto& [actor, permission] : arguments.authorizations) act.authorization.push_back({actor, permission});
   signed_transaction trx;
   trx.actions.push_back(std::move(act));
   chain.set_transaction_headers(trx);
   for (const auto& [actor, permission] : arguments.authorizations)
      trx.sign(chain.get_private_key(actor, permission.to_string()), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}

// create/remove/read/update, with a fresh action-local iterator and a no-code
// notification. Inputs share increment_wasm's fixed layout; no test-only hosts.
std::vector<char> lifecycle_steps_wasm() {
   return wasm(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_remove_i64" (func $remove (param i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_end_i64" (func $end (param i64 i64 i64) (result i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64) (param i64)
         (local $iterator i32) (local $mode i32)
         (drop (call $read (i32.const 0) (i32.const 40)))
         (set_local $mode (i32.load (i32.const 28)))
         (call $notify (i64.const )" + std::to_string("bob"_n.to_uint64_t()) + R"())
         (if (i32.eqz (get_local $mode))
            (then (set_local $iterator (call $store (i64.load (i32.const 0)) (i64.load (i32.const 32))
               (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"()
               (i64.load (i32.const 8)) (i32.const 16) (i32.const 8))))
            (else
               (set_local $iterator (call $find (get_local $receiver) (i64.load (i32.const 0))
                  (i64.load (i32.const 32)) (i64.load (i32.const 8))))
               (if (i32.eq (get_local $mode) (i32.const 1))
                  (then (call $remove (get_local $iterator))))
               (if (i32.eq (get_local $mode) (i32.const 3))
                  (then (call $update (get_local $iterator) (i64.const 0) (i32.const 16) (i32.const 8))))))
         (i32.store (i32.const 64) (get_local $iterator))
         (i32.store (i32.const 68) (call $end (get_local $receiver) (i64.load (i32.const 0)) (i64.load (i32.const 32))))
         (if (i32.ne (get_local $mode) (i32.const 1))
            (then (drop (call $get (get_local $iterator) (i32.const 72) (i32.const 8)))))
         (call $return (i32.const 64) (i32.const 16)))))");
}

isolated_action_input scoped_input(name scope, uint64_t primary, uint64_t value, uint32_t mode) {
   auto result = input(primary, value, 0, mode);
   const auto raw = scope.to_uint64_t();
   std::memcpy(result.data.data(), &raw, 8);
   return result;
}

transaction_trace_ptr push_raw_actions(eosio::testing::tester& chain,
      std::span<const isolated_action_invocation> calls) {
   signed_transaction trx;
   std::set<std::pair<name, name>> permissions;
   for (const auto& call : calls) {
      action act;
      act.account = call.input.account; act.name = call.input.action; act.data = call.input.data;
      for (const auto& auth : call.input.authorizations) {
         act.authorization.push_back({auth.first, auth.second}); permissions.insert(auth);
      }
      trx.actions.push_back(std::move(act));
   }
   chain.set_transaction_headers(trx);
   for (const auto& [actor, permission] : permissions)
      trx.sign(chain.get_private_key(actor, permission.to_string()), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}

const std::string notify_import = R"((import "env" "require_recipient" (func $notify (param i64))))";
std::string notify(name account) {
   return "(call $notify (i64.const " + std::to_string(account.to_uint64_t()) + "))";
}

// Notification code reads the root contract's rows, not its own table. Return
// the iterator, value, current receiver and root data to expose group semantics.
std::vector<char> notification_reader(uint64_t primary, const std::string& tail = {},
                                       const std::string& extra_imports = {}) {
   return wasm("(module " + notify_import + extra_imports + R"(
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "current_receiver" (func $receiver (result i64)))
      (import "env" "require_auth" (func $auth (param i64)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func $start (i64.store (i32.const 128) (i64.const 9))) (start $start)
      (func (export "apply") (param $current i64) (param $code i64) (param $action i64)
         (local $iterator i32)
         (if (i64.ne (get_local $code) (i64.const )" + std::to_string(rows.code.to_uint64_t()) + R"()) (then unreachable))
         (if (i64.ne (get_local $action) (i64.const )" + std::to_string("inc"_n.to_uint64_t()) + R"()) (then unreachable))
         (if (i64.ne (get_local $current) (call $receiver)) (then unreachable))
         (if (i64.ne (i64.load (i32.const 128)) (i64.const 9)) (then unreachable))
         (i64.store (i32.const 128) (i64.const 100))
         (call $auth (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"())
         (drop (call $read (i32.const 64) (i32.const 40)))
         (set_local $iterator (call $find (get_local $code) (i64.load (i32.const 64))
            (i64.load (i32.const 96)) (i64.const )" + std::to_string(primary) + R"()))
         (i32.store (i32.const 0) (get_local $iterator))
         (drop (call $get (get_local $iterator) (i32.const 8) (i32.const 8)))
         (i64.store (i32.const 16) (call $receiver))
         (i64.store (i32.const 24) (i64.load (i32.const 80)))
         )" + tail + R"(
         (call $return (i32.const 0) (i32.const 32)))))");
}

std::vector<isolated_receiver_program> notification_graph(bool empty_carol = false) {
   return {{rows.code, increment_wasm(notify_import, notify("bob"_n) + notify("carol"_n) + notify("bob"_n)), {}},
           {"bob"_n, notification_reader(2, notify(rows.code) + notify("carol"_n) + notify("dave"_n)), {}},
           {"carol"_n, empty_carol ? std::vector<char>{} : notification_reader(3, notify("bob"_n)), {}},
           {"dave"_n, notification_reader(1), {}}};
}

// Test-only generation of a host replay contract. This is NOT the production
// adopter or code proof; the original controller/hosts execute these operations
// so a later ORIGINAL notification observes the reconstructed iterator cache.
std::vector<char> cursor_replay_wasm(const isolated_notification_result& result,
                                    const isolated_receiver_effects& step, bool include_queries = true) {
   auto i64 = [](uint64_t n) { return "(i64.const " + std::to_string(n) + ")"; };
   auto i32 = [](int32_t n) { return "(i32.const " + std::to_string(n) + ")"; };
   auto bytes = [](const std::vector<char>& value) {
      constexpr char hex[] = "0123456789abcdef";
      std::string text;
      for (unsigned char c : value) { text += '\\'; text += hex[c >> 4]; text += hex[c & 15]; }
      return text;
   };
   auto check = [&](const std::string& call, int32_t expected) {
      return "(if (i32.ne " + call + " " + i32(expected) + ") (then unreachable))";
   };
   std::string data = "(data (i32.const 0) \"" + bytes(step.effects.return_value) + "\")";
   std::string body;
   int32_t offset = 1024;
   for (const auto& event : step.cursor_events) {
      if (const auto* query = std::get_if<isolated_cursor_query>(&event)) {
         if (!include_queries) continue;
         std::string call;
         if (query->kind == isolated_cursor_query_kind::next || query->kind == isolated_cursor_query_kind::previous) {
            body += "(i64.store (i32.const 512) " + i64(query->primary_before) + ")";
            call = std::string("(call ") + (query->kind == isolated_cursor_query_kind::next ? "$next " : "$previous ") +
                   i32(query->iterator) + " (i32.const 512))";
            body += check(call, query->result);
            body += "(if (i64.ne (i64.load (i32.const 512)) " + i64(query->primary_after) + ") (then unreachable))";
         } else {
            const char* function = query->kind == isolated_cursor_query_kind::find ? "$find" :
               query->kind == isolated_cursor_query_kind::lower_bound ? "$lower" :
               query->kind == isolated_cursor_query_kind::upper_bound ? "$upper" : "$end";
            call = std::string("(call ") + function + " " + i64(query->table.code.to_uint64_t()) +
                   i64(query->table.scope.to_uint64_t()) + i64(query->table.table.to_uint64_t());
            if (query->kind != isolated_cursor_query_kind::end) call += i64(query->key);
            body += check(call + ")", query->result);
         }
      } else if (const auto* write = std::get_if<isolated_cursor_write>(&event)) {
         const auto& mutation = result.state.writes().at(write->write_index);
         if (mutation.operation == primary_operation::remove) {
            body += "(call $remove " + i32(write->iterator) + ")";
         } else {
            if (mutation.value.size() > 65536u - offset) throw std::logic_error("test replay memory limit");
            data += "(data " + i32(offset) + " \"" + bytes(mutation.value) + "\")";
            const auto args = i64(mutation.payer.to_uint64_t()) + i32(offset) + i32(mutation.value.size());
            if (mutation.operation == primary_operation::store)
               body += check("(call $store " + i64(mutation.table.scope.to_uint64_t()) +
                  i64(mutation.table.table.to_uint64_t()) + i64(mutation.payer.to_uint64_t()) +
                  i64(mutation.primary_key) + i32(offset) + i32(mutation.value.size()) + ")", write->iterator);
            else body += "(call $update " + i32(write->iterator) + args + ")";
            offset += mutation.value.size();
         }
      } else {
         body += notify(step.effects.recipients.at(std::get<isolated_cursor_notify>(event).notification_index).recipient);
      }
   }
   return wasm("(module " + notify_import + R"(
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_lowerbound_i64" (func $lower (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_upperbound_i64" (func $upper (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_end_i64" (func $end (param i64 i64 i64) (result i32)))
      (import "env" "db_next_i64" (func $next (param i32 i32) (result i32)))
      (import "env" "db_previous_i64" (func $previous (param i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "db_remove_i64" (func $remove (param i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1) )" + data + "(func (export \"apply\") (param i64 i64 i64) " + body +
      "(call $return (i32.const 0) " + i32(step.effects.return_value.size()) + ")))");
}

std::vector<char> cursor_operations_wasm() {
   const auto code = std::to_string(rows.code.to_uint64_t());
   const auto scope = std::to_string(rows.scope.to_uint64_t());
   const auto table = std::to_string(rows.table.to_uint64_t());
   const auto args = "(i64.const " + code + ")(i64.const " + scope + ")(i64.const " + table + ")";
   const auto absent = "(i64.const " + code + ")(i64.const " + std::to_string("fresh"_n.to_uint64_t()) +
                       ")(i64.const " + table + ")";
   return wasm("(module " + notify_import + R"(
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_lowerbound_i64" (func $lower (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_upperbound_i64" (func $upper (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_end_i64" (func $end (param i64 i64 i64) (result i32)))
      (import "env" "db_next_i64" (func $next (param i32 i32) (result i32)))
      (import "env" "db_previous_i64" (func $previous (param i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "db_remove_i64" (func $remove (param i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         )" + "(drop (call $find " + args + "(i64.const 1)))" +
         "(drop (call $lower " + args + "(i64.const 2)))" +
         "(drop (call $upper " + args + "(i64.const 2)))" +
         "(drop (call $end " + args + "))" + R"(
         (i64.store (i32.const 64) (i64.const 100))
         (drop (call $next (i32.const 2) (i32.const 64)))
         (drop (call $next (i32.const 3) (i32.const 64)))
         (drop (call $previous (i32.const -2) (i32.const 64)))
         (drop (call $previous (i32.const 0) (i32.const 64)))
         )" + notify("bob"_n) + R"(
         (i64.store (i32.const 128) (i64.const 22))
         (call $update (i32.const 1) (i64.const 0) (i32.const 128) (i32.const 8))
         (call $remove (i32.const 2))
         (i64.store (i32.const 128) (i64.const 55))
         (drop (call $store (i64.const )" + scope + ")(i64.const " + table + ")(i64.const " +
         std::to_string("alice"_n.to_uint64_t()) + R"()(i64.const 5)(i32.const 128)(i32.const 8)))
         )" + "(drop (call $end " + args + "))" + "(drop (call $lower " + args + "(i64.const 3)))" +
         notify("carol"_n) + "(drop (call $upper " + args + "(i64.const 5)))" +
         "(drop (call $find " + args + "(i64.const 999)))" +
         "(drop (call $find " + absent + "(i64.const 1)))" + "(drop (call $end " + absent + "))" + R"(
         (drop (call $get (i32.const 1) (i32.const 0) (i32.const 8)))
         (call $return (i32.const 0) (i32.const 8)))))");
}

struct wasm_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   wasm_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      db.add_index<account_index>();
      const auto& table = db.create<table_id_object>([](auto& row) {
         row.code = rows.code; row.scope = rows.scope; row.table = rows.table;
         row.payer = "alice"_n; row.count = 4;
      });
      for (uint64_t primary = 1; primary <= 4; ++primary) {
         db.create<key_value_object>([&](auto& row) {
            row.t_id = table.id; row.primary_key = primary; row.payer = "alice"_n;
            const uint64_t value = primary * 10;
            row.value.assign(reinterpret_cast<const char*>(&value), sizeof(value));
         });
      }
      const std::array keys{rows};
      base = frozen_primary_state::capture(view, keys);
   }
   uint64_t canonical(uint64_t primary = 1) {
      return number(view.read_primary({rows, primary_query_kind::find, primary}, 8).row->value);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_wasm_worker_tests)

BOOST_FIXTURE_TEST_CASE(fixed_input_and_authorization_hosts_match_original_wasm_execution, wasm_fixture) {
   const auto actor = std::to_string("alice"_n.to_uint64_t());
   const auto permission = std::to_string("active"_n.to_uint64_t());
   const auto code = wasm(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "action_data_size" (func $size (result i32)))
      (import "env" "current_receiver" (func $receiver (result i64)))
      (import "env" "require_auth" (func $require (param i64)))
      (import "env" "require_auth2" (func $require2 (param i64 i64)))
      (import "env" "has_auth" (func $has (param i64) (result i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (call $read (i32.const 0) (i32.const 40)))
         (i64.store (i32.const 64) (call $receiver))
         (i32.store (i32.const 72) (call $size))
         (i32.store (i32.const 76) (call $read (i32.const 0) (i32.const 0)))
         (i32.store (i32.const 80) (call $has (i64.const )" + actor + R"()))
         (call $require (i64.const )" + actor + R"())
         (call $require2 (i64.const )" + actor + ") (i64.const " + permission + R"())
         (call $return (i32.const 64) (i32.const 24)))))");
   isolated_wasm_module isolated(code);
   const auto arguments = input();
   const auto attempt = isolated.execute(base, arguments);

   eosio::testing::tester original;
   original.create_accounts({rows.code, "alice"_n});
   original.set_code(rows.code, std::vector<uint8_t>(code.begin(), code.end()));
   const auto trace = push_raw(original, arguments);
   BOOST_REQUIRE(trace);
   BOOST_REQUIRE(!trace->except);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
   BOOST_CHECK(attempt.return_value == trace->action_traces.front().return_value);
   BOOST_CHECK_EQUAL(attempt.return_value.size(), 24u);
   BOOST_CHECK(attempt.state.writes().empty());
   auto unauthorized = arguments;
   unauthorized.authorizations.clear();
   BOOST_CHECK_THROW(isolated.execute(base, unauthorized), primary_state_unsupported);
}

BOOST_AUTO_TEST_CASE(private_wasm_write_matches_original_chain_execution) {
   eosio::testing::tester original;
   original.create_accounts({rows.code, "alice"_n});
   const auto initializer = wasm(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (call $read (i32.const 0) (i32.const 40)))
         (i64.store (i32.const 64) (i64.const 10))
         (drop (call $store (i64.load (i32.const 0)) (i64.load (i32.const 32))
            (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"()
            (i64.load (i32.const 8)) (i32.const 64) (i32.const 8))))))");
   original.set_code(rows.code, std::vector<uint8_t>(initializer.begin(), initializer.end()));
   const auto arguments = input();
   BOOST_REQUIRE(!push_raw(original, arguments)->except);
   original.produce_block();
   const auto code = increment_wasm();
   original.set_code(rows.code, std::vector<uint8_t>(code.begin(), code.end()));
   original.produce_block();
   chainbase_transaction_state_view live(original.control->db());
   const std::array keys{rows};
   auto base = frozen_primary_state::capture(live, keys);
   const auto before = original.control->calculate_integrity_hash();
   isolated_wasm_module machine(code);
   const auto attempt = machine.execute(base, arguments);
   BOOST_CHECK(original.control->calculate_integrity_hash() == before);
   const auto trace = push_raw(original, arguments);
   BOOST_REQUIRE(!trace->except);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
   BOOST_CHECK(attempt.return_value == trace->action_traces.front().return_value);
   BOOST_REQUIRE_EQUAL(attempt.state.writes().size(), 1u);
   const auto row = live.read_primary({rows, primary_query_kind::find, 1}, 8);
   BOOST_REQUIRE(row.row);
   BOOST_CHECK(attempt.state.writes()[0].value == row.row->value);
   BOOST_CHECK_EQUAL(number(row.row->value), 17u);
   BOOST_CHECK(trace->action_traces.front().account_ram_deltas.empty());
}

BOOST_FIXTURE_TEST_CASE(real_wasm_updates_only_private_state_and_returns_original_bytes, wasm_fixture) {
   const auto code = increment_wasm();
   isolated_wasm_module machine(code);
   for (unsigned i = 0; i < 5; ++i) {
      const auto result = machine.execute(base, input());
      BOOST_CHECK_EQUAL(number(result.return_value), 17u);
      BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
      BOOST_CHECK_EQUAL(number(result.state.writes()[0].value), 17u);
      BOOST_CHECK_EQUAL(result.state.read_tables().size(), 1u);
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(resize_is_logged_but_pointer_failure_discards_prior_write, wasm_fixture) {
   isolated_wasm_module machine(increment_wasm());
   const auto resized = machine.execute(base, input(1, 7, 0, 2));
   BOOST_REQUIRE_EQUAL(resized.state.writes().size(), 2u);
   BOOST_CHECK_EQUAL(resized.state.writes().back().value.size(), 7u);
   BOOST_CHECK_EQUAL(number(resized.return_value), 17u);
   for (uint32_t mode : {3u}) {
      BOOST_CHECK_THROW(machine.execute(base, input(1, 7, 0, mode)), primary_state_unsupported);
      BOOST_CHECK_EQUAL(canonical(), 10u);
      BOOST_CHECK_EQUAL(number(machine.execute(base, input()).return_value), 17u);
   }
   auto unknown = input();
   const auto scope = "uncaptured"_n.to_uint64_t();
   std::memcpy(unknown.data.data(), &scope, sizeof(scope));
   BOOST_CHECK_THROW(machine.execute(base, unknown), primary_state_unsupported);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(wasm_resize_reads_current_size_and_same_payer_tracks_private_switch, wasm_fixture) {
   const auto code = increment_wasm({},
      "(call $update (get_local $iterator) (i64.const " + std::to_string("bob"_n.to_uint64_t()) + ") (i32.const 64) (i32.const 12))"
      "(i32.store (i32.const 128) (call $get (get_local $iterator) (i32.const 0) (i32.const 0)))"
      "(call $update (get_local $iterator) (i64.const 0) (i32.const 64) (i32.const 0))"
      "(i32.store (i32.const 132) (call $get (get_local $iterator) (i32.const 0) (i32.const 0)))"
      "(i64.store (i32.const 64) (i64.load (i32.const 128)))");
   isolated_wasm_module machine(code);
   for (unsigned i = 0; i < 3; ++i) {
      const auto result = machine.execute(base, input());
      BOOST_CHECK_EQUAL(number(result.return_value), 12u);
      BOOST_REQUIRE_EQUAL(result.state.writes().size(), 3u);
      BOOST_CHECK_EQUAL(result.state.writes()[0].value.size(), 8u);
      BOOST_CHECK_EQUAL(result.state.writes()[1].value.size(), 12u);
      BOOST_CHECK(result.state.writes()[2].value.empty());
      BOOST_CHECK(result.state.writes()[1].payer == "bob"_n);
      BOOST_CHECK(result.state.writes()[2].payer == "bob"_n);
      BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(timeout_interrupts_infinite_wasm_and_module_can_be_reused, wasm_fixture) {
   isolated_wasm_limits limits;
   limits.max_attempt_time = std::chrono::milliseconds(20);
   isolated_wasm_module machine(increment_wasm(), limits);
   const auto start = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(machine.execute(base, input(1, 7, 0, 1)), eosio::vm::timeout_exception);
   BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
   BOOST_CHECK_EQUAL(canonical(), 10u);
   BOOST_CHECK_EQUAL(number(machine.execute(base, input()).return_value), 17u);
}

BOOST_FIXTURE_TEST_CASE(cancel_before_and_during_execution_never_publishes_result, wasm_fixture) {
   const auto code = increment_wasm();
   isolated_wasm_module machine(code);
   std::stop_source before;
   before.request_stop();
   BOOST_CHECK_THROW(machine.execute(base, input(), before.get_token()), primary_state_invalid_attempt);
   std::stop_source during;
   std::promise<void> entered;
   auto ready = entered.get_future();
   auto worker = std::async(std::launch::async, [&] {
      std::unique_ptr<isolated_wasm_module> local;
      try { local = std::make_unique<isolated_wasm_module>(code); }
      catch (...) { entered.set_exception(std::current_exception()); throw; }
      entered.set_value();
      try { (void)local->execute(base, input(1, 7, 0, 1), during.get_token()); }
      catch (const eosio::vm::timeout_exception&) { return true; }
      catch (const primary_state_invalid_attempt&) { return true; }
      return false;
   });
   ready.get();
   // Permit entry into the actual loop; the stop also remains valid if the
   // worker is descheduled before execution. A separate timeout test covers it.
   std::this_thread::sleep_for(std::chrono::milliseconds(5));
   during.request_stop();
   BOOST_CHECK(worker.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
   BOOST_CHECK(worker.get());
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(one_two_four_workers_execute_wasm_on_independent_modules, wasm_fixture) {
   const auto code = increment_wasm();
   for (unsigned workers : {1u, 2u, 4u}) {
      std::barrier gate(workers);
      struct report {
         std::chrono::steady_clock::time_point start, end;
         isolated_action_result result;
      };
      std::vector<std::future<report>> futures;
      for (unsigned i = 0; i < workers; ++i) {
         futures.push_back(std::async(std::launch::async, [&, i] {
            // This is an overlap/correctness test, not a 500ms performance
            // gate. ASan+UBSan considerably slows the interpreted work loop.
            // Use the already-supported maximum LOCAL test budget; production
            // defaults and the dedicated short-deadline tests stay unchanged.
            isolated_wasm_limits limits;
            limits.max_attempt_time = std::chrono::seconds(1);
            auto local = prepare_for_gate(code, gate, limits);
            gate.arrive_and_wait();
            auto start = std::chrono::steady_clock::now();
            auto result = local->execute(base, input(i + 1, 7, 150000));
            auto end = std::chrono::steady_clock::now();
            return report{start, end, std::move(result)};
         }));
      }
      std::vector<report> reports;
      for (auto& future : futures) reports.push_back(future.get());
      auto last_start = reports.front().start, first_end = reports.front().end;
      for (unsigned i = 0; i < workers; ++i) {
         BOOST_TEST_MESSAGE("overlap workers=" << workers << " worker=" << i << " elapsed_us=" <<
            std::chrono::duration_cast<std::chrono::microseconds>(reports[i].end - reports[i].start).count());
         BOOST_CHECK_EQUAL(number(reports[i].result.return_value), (i + 1) * 10 + 7);
         BOOST_REQUIRE_EQUAL(reports[i].result.state.writes().size(), 1u);
         BOOST_CHECK_EQUAL(number(reports[i].result.state.writes()[0].value), (i + 1) * 10 + 7);
         BOOST_CHECK_EQUAL(canonical(i + 1), (i + 1) * 10);
         last_start = std::max(last_start, reports[i].start);
         first_end = std::min(first_end, reports[i].end);
      }
      // Overlapping execution calls, NOT a TPS/speedup benchmark or evidence
      // of canonical adoption. Modules include a real interpreted WASM loop.
      BOOST_CHECK(last_start < first_end);
   }
}

BOOST_FIXTURE_TEST_CASE(timeout_of_one_module_does_not_disable_other_modules, wasm_fixture) {
   const auto code = increment_wasm();
   std::barrier gate(2);
   std::atomic<bool> timeout_observed{false};
   auto bad = std::async(std::launch::async, [&] {
      isolated_wasm_limits limits; limits.max_attempt_time = std::chrono::milliseconds(10);
      auto local = prepare_for_gate(code, gate, limits);
      gate.arrive_and_wait();
      try { (void)local->execute(base, input(1, 7, 0, 1)); }
      catch (const eosio::vm::timeout_exception&) { timeout_observed.store(true); return true; }
      timeout_observed.store(true);
      return false;
   });
   auto good = std::async(std::launch::async, [&] {
      auto local = prepare_for_gate(code, gate);
      gate.arrive_and_wait();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      unsigned i = 0;
      while (!timeout_observed.load() || i < 10) {
         if (std::chrono::steady_clock::now() >= deadline) return false;
         if (number(local->execute(base, input(2, 3, 50000)).return_value) != 23) return false;
         ++i;
      }
      // Also execute after the other module's timeout/permission restoration.
      return number(local->execute(base, input(2, 3, 50000)).return_value) == 23;
   });
   BOOST_CHECK(bad.get());
   BOOST_CHECK(good.get());
   BOOST_CHECK_EQUAL(canonical(1), 10u);
   BOOST_CHECK_EQUAL(canonical(2), 20u);
}

BOOST_FIXTURE_TEST_CASE(unknown_imports_fail_but_unbounded_memory_can_execute_within_local_cap, wasm_fixture) {
   const auto unknown = wasm(R"((module
      (import "env" "send_deferred" (func (param i32 i64 i32 i32 i32)))
      (func (export "apply") (param i64 i64 i64))))");
   BOOST_CHECK_THROW(isolated_wasm_module{unknown}, eosio::vm::wasm_link_exception);
   for (auto memory : {"(memory 1)", "(memory 1 17)"}) {
      const auto code = wasm(std::string("(module ") + memory + " (func (export \"apply\") (param i64 i64 i64)))");
      isolated_wasm_module machine(code);
      BOOST_CHECK(machine.execute(base, input()).state.writes().empty());
   }
   const auto too_large = wasm(R"((module (memory 17) (func (export "apply") (param i64 i64 i64))))");
   BOOST_CHECK_THROW(isolated_wasm_module{too_large}, eosio::vm::wasm_parse_exception);
}

BOOST_AUTO_TEST_CASE(every_other_production_intrinsic_is_rejected_before_execution) {
   const std::set<std::string_view> supported{
      "current_receiver", "action_data_size", "read_action_data", "has_auth", "require_auth", "require_auth2",
      "db_find_i64", "db_lowerbound_i64", "db_upperbound_i64", "db_end_i64", "db_get_i64", "db_update_i64",
      "db_next_i64", "db_previous_i64", "set_action_return_value",
      "memcpy", "memmove", "memcmp", "memset", "eosio_assert", "eosio_assert_message", "eosio_assert_code", "abort",
      "is_account", "require_recipient", "db_store_i64", "db_remove_i64"};
   const std::set<std::string_view> fallback_only{
      "prints_l", "send_inline",
      "set_blockchain_parameters_packed", "get_blockchain_parameters_packed", "set_proposed_producers",
      "current_time", "get_active_producers"};
   size_t rejected = 0;
   for (auto name : audited_production_intrinsics) {
      if (supported.contains(name) || fallback_only.contains(name)) continue;
      BOOST_TEST_CONTEXT("denied production import=" << name) {
         // Even an UNUSED import must fail, so no unsupported call can reach
         // canonical state after earlier private writes or control flow.
         const auto code = wasm("(module (import \"env\" \"" + std::string(name) +
            "\" (func)) (func (export \"apply\") (param i64 i64 i64)))");
         BOOST_CHECK_THROW(isolated_wasm_module{code}, eosio::vm::wasm_link_exception);
         ++rejected;
      }
   }
   BOOST_CHECK_EQUAL(rejected, 228u);
}

BOOST_FIXTURE_TEST_CASE(link_only_imports_never_publish_partial_results, wasm_fixture) {
   for (const auto& item : fallback_imports) {
      BOOST_TEST_CONTEXT("fallback-only host=" << item.name) {
         const std::string declaration = "(import \"env\" \"" + std::string(item.name) + "\" (func $stub " + item.signature + "))";
         isolated_wasm_module unused(increment_wasm(declaration));
         BOOST_CHECK_EQUAL(number(unused.execute(base, input()).return_value), 17u);
         isolated_wasm_module called(increment_wasm(declaration, item.invocation));
         for (unsigned repeat = 0; repeat < 3; ++repeat) {
            BOOST_CHECK_THROW(called.execute(base, input()), primary_state_unsupported);
            BOOST_CHECK_EQUAL(canonical(), 10u);
         }
         const auto start_code = wasm("(module " + declaration + " (func $start " + item.invocation +
            ") (start $start) (func (export \"apply\") (param i64 i64 i64)))");
         isolated_wasm_module start(start_code);
         BOOST_CHECK_THROW(start.execute(base, input()), primary_state_unsupported);
         const auto wrong_signature = wasm("(module (import \"env\" \"" + std::string(item.name) +
            "\" (func (param f64))) (func (export \"apply\") (param i64 i64 i64)))");
         BOOST_CHECK_THROW(isolated_wasm_module{wrong_signature}, eosio::vm::wasm_link_exception);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(primary_lifecycle_hosts_keep_iterator_generations_and_interleaved_notifications, wasm_fixture) {
   const std::string imports = R"(
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_remove_i64" (func $erase (param i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
   )";
   auto notify = [](name who) { return "(call $notify (i64.const " + std::to_string(who.to_uint64_t()) + "))"; };
   const auto store = "(call $store (i64.load (i32.const 0)) (i64.load (i32.const 32)) (i64.const " +
      std::to_string("alice"_n.to_uint64_t()) + ") (i64.load (i32.const 8)) (i32.const 64) (i32.const 8))";
   const auto code = increment_wasm(imports, notify("alice"_n) +
      "(i32.store (i32.const 128) (get_local $iterator)) (call $erase (get_local $iterator))" + notify("carol"_n) +
      "(set_local $iterator " + store + ") (i32.store (i32.const 132) (get_local $iterator))" + notify("dave"_n) +
      "(i64.store (i32.const 64) (i64.load (i32.const 128)))", notify("bob"_n));
   isolated_wasm_module module(code);
   for (unsigned repeat = 0; repeat < 3; ++repeat) {
      BOOST_CHECK_THROW(module.execute(base, input(1, 7, 0, 3)), primary_state_unsupported);
      const auto result = module.execute(base, input());
      BOOST_REQUIRE_EQUAL(result.state.writes().size(), 3u);
      BOOST_CHECK(result.state.writes()[0].operation == primary_operation::update);
      BOOST_CHECK(result.state.writes()[1].operation == primary_operation::remove);
      BOOST_CHECK(result.state.writes()[2].operation == primary_operation::store);
      BOOST_CHECK_LT(result.state.writes()[2].object_id, 0);
      BOOST_CHECK_EQUAL(number(result.state.writes()[2].value), 17u);
      BOOST_CHECK_EQUAL(number(result.return_value), uint64_t{1} << 32); // old handle 0, recreated row handle 1
      BOOST_CHECK((result.recipients == std::vector<isolated_notification>{
         {"bob"_n, 0}, {"alice"_n, 1}, {"carol"_n, 2}, {"dave"_n, 3}}));
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
   isolated_wasm_module duplicate(increment_wasm(imports, "(drop " + store + ")"));
   BOOST_CHECK_THROW(duplicate.execute(base, input()), primary_state_unsupported);
   isolated_wasm_module stale(increment_wasm(imports,
      "(call $erase (get_local $iterator)) (drop " + store + ") (drop (call $get (get_local $iterator) (i32.const 0) (i32.const 0)))"));
   BOOST_CHECK_THROW(stale.execute(base, input()), primary_state_unsupported);
   for (auto host : {"db_store_i64", "db_remove_i64"}) {
      const auto bad = wasm(std::string("(module (import \"env\" \"") + host +
         "\" (func (param f64))) (func (export \"apply\") (param i64 i64 i64)))");
      BOOST_CHECK_THROW(isolated_wasm_module{bad}, eosio::vm::wasm_link_exception);
   }
}

BOOST_FIXTURE_TEST_CASE(memory_and_assertion_hosts_obey_bounded_local_semantics, wasm_fixture) {
   for (const auto& item : primitive_cases) {
      BOOST_TEST_CONTEXT(item.label) {
         isolated_wasm_module machine(primitive_wasm(item));
         for (unsigned repeat = 0; repeat < 3; ++repeat) {
            if (item.success) {
               const auto result = machine.execute(base, input());
               BOOST_CHECK_EQUAL(result.return_value.size(), 64u);
               BOOST_CHECK(result.state.writes().empty());
               if (std::string_view(item.label).starts_with("memcmp_")) {
                  int32_t observed;
                  std::memcpy(&observed, result.return_value.data(), sizeof(observed));
                  const int32_t expected = std::string_view(item.label).ends_with("positive") ? 1 :
                     std::string_view(item.label).ends_with("negative") ? -1 : 0;
                  BOOST_CHECK_EQUAL(observed, expected);
               }
            } else BOOST_CHECK_THROW(machine.execute(base, input()), primary_state_unsupported);
            BOOST_CHECK_EQUAL(canonical(), 10u);
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(growth_is_guarded_in_start_and_apply_and_resets_between_attempts, wasm_fixture) {
   for (bool at_start : {false, true}) {
      for (const auto declaration : {"(memory 1)", "(memory 1 17)", "(memory 1 2)"}) {
         for (uint32_t growth : {0u, 1u, 15u, 16u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            BOOST_TEST_CONTEXT(declaration << " growth=" << growth << " start=" << at_start) {
               isolated_wasm_module machine(growth_wasm(declaration, growth, at_start));
               for (unsigned repeat = 0; repeat < 3; ++repeat) {
                  if (growth <= 15) {
                     const auto result = machine.execute(base, input());
                     const bool module_full = std::string_view(declaration) == "(memory 1 2)" && growth > 1;
                     const uint64_t expected = (uint64_t(module_full ? 1 : 1 + growth) << 32) |
                        uint32_t(module_full ? -1 : 1);
                     BOOST_CHECK_EQUAL(number(result.return_value), expected);
                  } else BOOST_CHECK_THROW(machine.execute(base, input()), primary_state_unsupported);
               }
            }
         }
      }
   }
   isolated_wasm_limits small; small.max_pages = 2;
   isolated_wasm_module capped(growth_wasm("(memory 1)", 2), small);
   BOOST_CHECK_THROW(capped.execute(base, input()), primary_state_unsupported);
   isolated_wasm_module written(increment_wasm({}, "(drop (grow_memory (i32.const 16)))"));
   BOOST_CHECK_THROW(written.execute(base, input()), primary_state_unsupported);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(wasm_memory_globals_and_start_function_are_attempt_local, wasm_fixture) {
   const auto code = wasm(R"((module
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1) (global $counter (mut i64) (i64.const 4))
      (func $start (set_global $counter (i64.add (get_global $counter) (i64.const 1))))
      (start $start)
      (func (export "apply") (param i64 i64 i64)
         (i64.store (i32.const 0) (i64.add (i64.load (i32.const 0)) (get_global $counter)))
         (set_global $counter (i64.const 99))
         (call $return (i32.const 0) (i32.const 8)))))");
   isolated_wasm_module machine(code);
   for (unsigned i = 0; i < 5; ++i) {
      const auto result = machine.execute(base, input());
      BOOST_CHECK_EQUAL(number(result.return_value), 5u);
      BOOST_CHECK(result.state.writes().empty());
   }
}

BOOST_FIXTURE_TEST_CASE(affinity_input_bounds_and_return_budget_are_enforced, wasm_fixture) {
   isolated_wasm_module machine(increment_wasm());
   auto wrong_thread = std::async(std::launch::async, [&] {
      try { (void)machine.execute(base, input()); }
      catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(wrong_thread.get());
   auto limited = input(); limited.max_return_bytes = 7;
   BOOST_CHECK_THROW(machine.execute(base, limited), primary_state_unsupported);
   limited = input(); limited.receiver = "notify"_n;
   BOOST_CHECK_THROW(machine.execute(base, limited), primary_state_unsupported);
   limited = input(); limited.data.resize(64 * 1024 + 1);
   BOOST_CHECK_THROW(machine.execute(base, limited), primary_state_unsupported);
   limited = input(); limited.authorizations.resize(65);
   BOOST_CHECK_THROW(machine.execute(base, limited), primary_state_unsupported);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(execution_fingerprint_binds_code_inputs_and_observable_vm_limits, wasm_fixture) {
   const auto code = increment_wasm();
   const auto hash = fc::sha256::hash(code.data(), code.size());
   isolated_wasm_limits limits;
   auto args = input();
   const auto expected = isolated_execution_fingerprint(hash, limits, args);
   isolated_wasm_module machine(code, limits);
   BOOST_CHECK(machine.execute(base, args).execution_fingerprint == expected);
   const auto check_changed = [&](const isolated_action_input& changed) {
      BOOST_CHECK(isolated_execution_fingerprint(hash, limits, changed) != expected);
   };
   auto changed = args; changed.receiver = "other"_n; check_changed(changed);
   changed = args; changed.account = "other"_n; check_changed(changed);
   changed = args; changed.action = "other"_n; check_changed(changed);
   changed = args; changed.authorizations[0].first = "bob"_n; check_changed(changed);
   changed = args; changed.authorizations[0].second = "owner"_n; check_changed(changed);
   changed = args; changed.data.push_back('x'); check_changed(changed);
   changed = args; ++changed.max_return_bytes; check_changed(changed);
   BOOST_CHECK(isolated_execution_fingerprint(fc::sha256::hash("other"), limits, args) != expected);
   auto changed_limits = limits; ++changed_limits.max_pages;
   BOOST_CHECK(isolated_execution_fingerprint(hash, changed_limits, args) != expected);
   changed_limits = limits; ++changed_limits.max_call_depth;
   BOOST_CHECK(isolated_execution_fingerprint(hash, changed_limits, args) != expected);
   // Wall time and copy budgets select fallback only, not successful WASM semantics.
   changed_limits = limits; changed_limits.max_attempt_time *= 2;
   BOOST_CHECK(isolated_execution_fingerprint(hash, changed_limits, args) == expected);
}

BOOST_FIXTURE_TEST_CASE(infinite_start_function_obeys_attempt_deadline, wasm_fixture) {
   const auto code = wasm(R"((module (memory 1 1)
      (func $start (loop $forever (br $forever))) (start $start)
      (func (export "apply") (param i64 i64 i64))))");
   isolated_wasm_limits limits; limits.max_attempt_time = std::chrono::milliseconds(10);
   isolated_wasm_module machine(code, limits);
   for (unsigned i = 0; i < 3; ++i)
      BOOST_CHECK_THROW(machine.execute(base, input()), eosio::vm::timeout_exception);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(absent_and_zero_page_memory_never_become_unbounded_host_spans, wasm_fixture) {
   for (const auto memory : {"", "(memory 0 1)"}) {
      const auto code = wasm(std::string(R"((module
         (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
         )") + memory + R"(
         (func (export "apply") (param i64 i64 i64)
            (drop (call $read (i32.const 0) (i32.const 8))))))");
      isolated_wasm_module machine(code);
      BOOST_CHECK_THROW(machine.execute(base, input()), primary_state_unsupported);
   }
   const auto no_memory_needed = wasm(R"((module
      (import "env" "current_receiver" (func $receiver (result i64)))
      (func (export "apply") (param i64 i64 i64) (drop (call $receiver)))))");
   isolated_wasm_module machine(no_memory_needed);
   BOOST_CHECK(machine.execute(base, input()).state.writes().empty());
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(real_wasm_memory_fault_discards_attempt_and_preserves_other_modules, wasm_fixture) {
   const auto code = wasm(R"((module (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (i64.load (i32.const 65536))))))");
   isolated_wasm_module faulty(code), healthy(increment_wasm());
   for (unsigned i = 0; i < 3; ++i) {
      BOOST_CHECK_THROW(faulty.execute(base, input()), eosio::vm::wasm_memory_exception);
      BOOST_CHECK_EQUAL(number(healthy.execute(base, input()).return_value), 17u);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_transaction_executor_tests)

BOOST_FIXTURE_TEST_CASE(actions_share_private_state_but_reset_memory_and_iterators, wasm_fixture) {
   const auto code = increment_wasm({}, {}, R"(
      (if (i32.ne (get_local $iterator) (i32.const 0)) (then unreachable))
      (if (i64.ne (i64.load (i32.const 128)) (i64.const 0)) (then unreachable))
      (i64.store (i32.const 128) (i64.const 99)))");
   const std::array calls{isolated_action_invocation{code, input(1, 7), {}},
                          isolated_action_invocation{code, input(1, 9), {}},
                          isolated_action_invocation{code, input(2, 1), {}}};
   isolated_transaction_executor executor;
   for (unsigned repeat = 0; repeat < 3; ++repeat) {
      const auto result = executor.execute(base, calls);
      BOOST_REQUIRE_EQUAL(result.actions.size(), 3u);
      BOOST_REQUIRE_EQUAL(result.state.writes().size(), 3u);
      for (size_t i = 0; i < calls.size(); ++i) {
         BOOST_CHECK_EQUAL(result.actions[i].writes_begin, i);
         BOOST_CHECK_EQUAL(result.actions[i].writes_end, i + 1);
         BOOST_CHECK_EQUAL(number(result.actions[i].return_value), (std::array{17u, 26u, 21u})[i]);
         BOOST_CHECK(result.actions[i].elapsed.count() >= 0);
      }
      BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
      BOOST_CHECK_EQUAL(canonical(), 10u);
      BOOST_CHECK_EQUAL(canonical(2), 20u);
   }
}

BOOST_FIXTURE_TEST_CASE(lifecycle_and_module_switches_share_one_transaction_journal, wasm_fixture) {
   const table_key created{rows.code, "fresh"_n, rows.table};
   const std::array tables{rows, created};
   const auto captured = frozen_primary_state::capture(view, tables);
   const auto code = lifecycle_steps_wasm();
   const std::array calls{
      isolated_action_invocation{code, scoped_input(created.scope, 7, 40, 0), {}},
      isolated_action_invocation{increment_wasm(), scoped_input(created.scope, 7, 2, 0), {}},
      isolated_action_invocation{code, scoped_input(created.scope, 7, 0, 1), {}},
      isolated_action_invocation{code, scoped_input(created.scope, 7, 99, 0), {}},
      isolated_action_invocation{code, scoped_input(created.scope, 7, 0, 2), {}}};
   isolated_transaction_executor executor;
   for (unsigned repeat = 0; repeat < 3; ++repeat) {
      const auto result = executor.execute(captured, calls);
      BOOST_REQUIRE_EQUAL(result.actions.size(), 5u);
      BOOST_REQUIRE_EQUAL(result.state.writes().size(), 4u);
      BOOST_CHECK(result.state.writes()[0].operation == primary_operation::store);
      BOOST_CHECK(result.state.writes()[1].operation == primary_operation::update);
      BOOST_CHECK(result.state.writes()[2].operation == primary_operation::remove);
      BOOST_CHECK(result.state.writes()[3].operation == primary_operation::store);
      BOOST_CHECK(result.state.writes()[0].object_id != result.state.writes()[3].object_id);
      BOOST_CHECK(result.state.writes()[0].table_object_id != result.state.writes()[3].table_object_id);
      BOOST_CHECK_EQUAL(number(result.actions[1].return_value), 42u);
      BOOST_CHECK(result.actions[3].return_value == result.actions[4].return_value);
      for (size_t i : {0u, 3u, 4u}) {
         BOOST_REQUIRE_EQUAL(result.actions[i].return_value.size(), 16u);
         int32_t iterator, end;
         std::memcpy(&iterator, result.actions[i].return_value.data(), 4);
         std::memcpy(&end, result.actions[i].return_value.data() + 4, 4);
         BOOST_CHECK_EQUAL(iterator, 0); BOOST_CHECK_EQUAL(end, -2);
      }
      BOOST_REQUIRE_EQUAL(result.actions[4].recipients.size(), 1u);
      BOOST_CHECK_EQUAL(result.actions[4].recipients[0].after_writes, 4u);
      BOOST_CHECK_EQUAL(result.actions[4].writes_begin, 4u);
      BOOST_CHECK_EQUAL(result.actions[4].writes_end, 4u);
      BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
      BOOST_CHECK(!view.read_table(created));
   }
}

BOOST_FIXTURE_TEST_CASE(notification_deduplication_and_authorization_are_action_local, wasm_fixture) {
   const auto notify_bob = "(call $notify (i64.const " + std::to_string("bob"_n.to_uint64_t()) + "))";
   const auto notify_alice = "(call $notify (i64.const " + std::to_string("alice"_n.to_uint64_t()) + "))";
   const auto code = increment_wasm(R"(
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "require_auth" (func $auth (param i64))))",
      notify_bob + notify_alice,
      "(call $auth (i64.const " + std::to_string("alice"_n.to_uint64_t()) + "))" + notify_bob);
   std::array calls{isolated_action_invocation{code, input(1, 1), {}},
                    isolated_action_invocation{code, input(1, 2), {}}};
   isolated_transaction_executor executor;
   const auto result = executor.execute(base, calls);
   for (size_t i = 0; i < calls.size(); ++i) {
      BOOST_REQUIRE_EQUAL(result.actions[i].recipients.size(), 2u);
      BOOST_CHECK((result.actions[i].recipients[0] == isolated_notification{"bob"_n, i}));
      BOOST_CHECK((result.actions[i].recipients[1] == isolated_notification{"alice"_n, i + 1}));
   }
   calls[1].input.authorizations = {{"bob"_n, "active"_n}};
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_unsupported);
   calls[1].input.authorizations = calls[0].input.authorizations;
   calls[1].input.max_return_bytes = 7;
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_unsupported);
   calls[1].input.max_return_bytes = 256;
   BOOST_CHECK_EQUAL(number(executor.execute(base, calls).actions[1].return_value), 13u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(failed_later_action_never_returns_successful_prefix_or_poisoned_cache, wasm_fixture) {
   const auto code = increment_wasm();
   isolated_transaction_executor executor;
   const auto stub = increment_wasm(R"((import "env" "prints_l" (func $print (param i32 i32))))",
                                    "(call $print (i32.const 0) (i32.const 0))");
   const std::array failures{code, increment_wasm({}, "unreachable"), stub};
   for (size_t i = 0; i < failures.size(); ++i) {
      std::array calls{isolated_action_invocation{code, input(1, 1), {}},
                       isolated_action_invocation{failures[i], input(1, 2, 0, i == 0 ? 3 : 0), {}}};
      BOOST_CHECK_THROW(executor.execute(base, calls), std::exception);
      BOOST_CHECK_EQUAL(canonical(), 10u);
      calls[1] = {code, input(1, 2), {}};
      BOOST_CHECK_EQUAL(number(executor.execute(base, calls).actions[1].return_value), 13u);
   }
   isolated_wasm_limits short_time; short_time.max_attempt_time = std::chrono::milliseconds(10);
   const std::array timed{isolated_action_invocation{code, input(), {}},
                          isolated_action_invocation{code, input(1, 1, 0, 1), short_time}};
   BOOST_CHECK_THROW(executor.execute(base, timed), eosio::vm::timeout_exception);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(whole_transaction_budgets_are_not_reset_between_actions, wasm_fixture) {
   const auto code = increment_wasm();
   std::vector<isolated_action_invocation> calls(3, {code, input(), {}});
   isolated_transaction_executor executor;
   primary_overlay_limits limits; limits.max_write_operations = 2;
   BOOST_CHECK_THROW(executor.execute(base, calls, {}, limits), primary_state_budget_exceeded);
   limits = {}; limits.max_write_value_bytes = 16;
   BOOST_CHECK_THROW(executor.execute(base, calls, {}, limits), primary_state_budget_exceeded);
   limits = {}; limits.max_read_queries = 1; calls[1].input = input(2);
   BOOST_CHECK_THROW(executor.execute(base, calls, {}, limits), primary_state_budget_exceeded);
   calls.assign(17, {code, input(), {}});
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_budget_exceeded);
   calls.clear(); BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_budget_exceeded);
   calls.assign(16, {code, input(1, 1), {}});
   BOOST_CHECK_EQUAL(number(executor.execute(base, calls).actions.back().return_value), 26u);
   calls.assign(5, {code, input(), {}});
   for (auto& call : calls) call.input.data.resize(64 * 1024);
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_budget_exceeded);
   calls.assign(5, {std::vector<char>(64 * 1024), input(), {}});
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_budget_exceeded);
   calls.assign(1, {code, input(), {}}); calls[0].input.authorizations.resize(65);
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(missing_state_after_private_prefix_requires_fresh_whole_transaction, wasm_fixture) {
   const std::array tables{rows};
   const std::array first_query{primary_query{rows, primary_query_kind::find, 1}};
   const auto sparse = frozen_primary_state::capture_queries(view, tables, first_query);
   const auto code = increment_wasm();
   const std::array calls{isolated_action_invocation{code, input(1, 1), {}},
                          isolated_action_invocation{code, input(2, 2), {}}};
   isolated_transaction_executor executor;
   bool requested = false;
   try { (void)executor.execute(sparse, calls); }
   catch (const primary_state_missing& missing) {
      const auto* request = std::get_if<primary_capture_request>(&missing.request);
      BOOST_REQUIRE(request && request->query);
      BOOST_CHECK(request->query->table == rows);
      BOOST_CHECK_EQUAL(request->query->primary_key, 2u); requested = true;
   }
   BOOST_CHECK(requested);
   const auto result = executor.execute(base, calls);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(), 2u);
   BOOST_CHECK_EQUAL(number(result.actions[0].return_value), 11u);
   BOOST_CHECK_EQUAL(number(result.actions[1].return_value), 22u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(fingerprint_binds_action_count_order_and_each_input, wasm_fixture) {
   const auto code = increment_wasm();
   std::array calls{isolated_action_invocation{code, input(1, 1), {}},
                    isolated_action_invocation{code, input(1, 2), {}}};
   isolated_transaction_executor executor;
   const auto result = executor.execute(base, calls);
   const std::array fingerprints{result.actions[0].execution_fingerprint, result.actions[1].execution_fingerprint};
   BOOST_CHECK(result.execution_fingerprint == isolated_transaction_fingerprint(fingerprints));
   BOOST_CHECK(result.execution_fingerprint != isolated_transaction_fingerprint(std::span(fingerprints).first(1)));
   std::swap(calls[0], calls[1]);
   const auto reversed = executor.execute(base, calls);
   BOOST_CHECK(reversed.execution_fingerprint != result.execution_fingerprint);
   BOOST_CHECK_EQUAL(number(reversed.actions.back().return_value), 13u);
   calls[0].input.action = "other"_n;
   BOOST_CHECK(executor.execute(base, calls).execution_fingerprint != reversed.execution_fingerprint);
   BOOST_CHECK_THROW(isolated_transaction_fingerprint({}), primary_state_budget_exceeded);
}

BOOST_FIXTURE_TEST_CASE(affinity_and_transaction_cancellation_preserve_owned_state, wasm_fixture) {
   isolated_transaction_executor executor;
   const std::array calls{isolated_action_invocation{increment_wasm(), input(), {}}};
   auto foreign = std::async(std::launch::async, [&] {
      try { (void)executor.execute(base, calls); }
      catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(foreign.get());
   std::stop_source stop; stop.request_stop();
   BOOST_CHECK_THROW(executor.execute(base, calls, stop.get_token()), primary_state_invalid_attempt);
   isolated_cancel expired; expired.deadline = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(executor.execute(base, calls, {}, {}, expired), primary_state_invalid_attempt);
   auto generation = std::make_shared<std::atomic<uint64_t>>(2);
   BOOST_CHECK_THROW(executor.execute(base, calls, {}, {}, {generation, 1}), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(number(executor.execute(base, calls).actions[0].return_value), 17u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(independent_transaction_executors_do_not_share_overlay_or_vm, wasm_fixture) {
   const auto code = increment_wasm();
   for (size_t count : {1u, 2u, 4u}) {
      std::barrier gate(static_cast<std::ptrdiff_t>(count));
      std::vector<std::future<std::vector<uint64_t>>> workers;
      for (size_t worker = 0; worker < count; ++worker) workers.push_back(std::async(std::launch::async, [&, worker] {
         std::unique_ptr<isolated_transaction_executor> executor;
         try { executor = std::make_unique<isolated_transaction_executor>(); }
         catch (...) { gate.arrive_and_drop(); throw; }
         gate.arrive_and_wait();
         std::vector<uint64_t> values;
         for (unsigned repeat = 0; repeat < 5; ++repeat) {
            const std::array calls{isolated_action_invocation{code, input(1, worker + 1, 10000), {}},
                                   isolated_action_invocation{code, input(1, repeat + 1, 10000), {}}};
            const auto result = executor->execute(base, calls);
            values.push_back(number(result.actions[0].return_value));
            values.push_back(number(result.actions[1].return_value));
         }
         return values;
      }));
      for (size_t worker = 0; worker < count; ++worker) {
         const auto values = workers[worker].get();
         BOOST_REQUIRE_EQUAL(values.size(), 10u);
         for (size_t repeat = 0; repeat < 5; ++repeat) {
            BOOST_CHECK_EQUAL(values[repeat * 2], 11 + worker);
            BOOST_CHECK_EQUAL(values[repeat * 2 + 1], 12 + worker + repeat);
         }
      }
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(cross_contract_reads_see_prefix_but_cannot_write_foreign_rows, wasm_fixture) {
   const auto reader_code = [&](bool write) {
      return wasm(R"((module
         (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
         (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
         (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
         (import "env" "set_action_return_value" (func $return (param i32 i32)))
         (memory 1 1)
         (func (export "apply") (param i64 i64 i64) (local $it i32)
            (set_local $it (call $find (i64.const )" + std::to_string(rows.code.to_uint64_t()) +
            ") (i64.const " + std::to_string(rows.scope.to_uint64_t()) + ") (i64.const " +
            std::to_string(rows.table.to_uint64_t()) + R"() (i64.const 1)))
            (drop (call $get (get_local $it) (i32.const 0) (i32.const 8)))
            )" + (write ? "(call $update (get_local $it) (i64.const 0) (i32.const 0) (i32.const 8))" : "") +
            "(call $return (i32.const 0) (i32.const 8))))");
   };
   auto reader_input = input(); reader_input.receiver = reader_input.account = "reader"_n;
   std::array calls{isolated_action_invocation{increment_wasm(), input(), {}},
                    isolated_action_invocation{reader_code(false), reader_input, {}}};
   isolated_transaction_executor executor;
   const auto result = executor.execute(base, calls);
   BOOST_CHECK_EQUAL(number(result.actions[1].return_value), 17u);
   BOOST_CHECK_EQUAL(result.actions[1].writes_begin, 1u);
   BOOST_CHECK_EQUAL(result.actions[1].writes_end, 1u);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
   BOOST_CHECK(result.state.read_queries().contains({rows, primary_query_kind::find, 1}));
   calls[1].code = reader_code(true);
   BOOST_CHECK_THROW(executor.execute(base, calls), table_access_violation);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(transaction_deadline_and_inflight_cancel_cover_later_actions, wasm_fixture) {
   const auto code = increment_wasm();
   isolated_wasm_limits long_step; long_step.max_attempt_time = std::chrono::seconds(1);
   const std::array calls{isolated_action_invocation{code, input(), long_step},
                          isolated_action_invocation{code, input(1, 1, 0, 1), long_step}};
   isolated_transaction_executor executor;
   (void)executor.execute(base, std::span(calls).first(1)); // warm module, not private state
   isolated_cancel short_deadline;
   short_deadline.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
   BOOST_CHECK_THROW(executor.execute(base, calls, {}, {}, short_deadline), primary_state_invalid_attempt);
   // Each step allows one second, but the shared half-second cap wins.
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_invalid_attempt);
   std::stop_source stop;
   std::promise<void> warmed;
   auto ready = warmed.get_future();
   auto worker = std::async(std::launch::async, [&] {
      std::unique_ptr<isolated_transaction_executor> local;
      try {
         local = std::make_unique<isolated_transaction_executor>();
         (void)local->execute(base, std::span(calls).first(1));
      }
      catch (...) { warmed.set_exception(std::current_exception()); throw; }
      warmed.set_value();
      try { (void)local->execute(base, calls, stop.get_token()); }
      catch (const primary_state_invalid_attempt&) { return true; }
      return false;
   });
   ready.get(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); stop.request_stop();
   BOOST_CHECK(worker.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
   BOOST_CHECK(worker.get());
   BOOST_CHECK_EQUAL(number(executor.execute(base, std::span(calls).first(1)).actions[0].return_value), 17u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_notification_executor_tests)

BOOST_FIXTURE_TEST_CASE(cursor_events_preserve_query_write_and_notification_interleaving, wasm_fixture) {
   const table_key absent{rows.code, "fresh"_n, rows.table};
   const std::array tables{rows, absent};
   const auto captured = frozen_primary_state::capture(view, tables);
   const std::array roots{input()};
   const std::array programs{isolated_receiver_program{rows.code, cursor_operations_wasm(), {}},
      isolated_receiver_program{"bob"_n, notification_reader(2), {}},
      isolated_receiver_program{"carol"_n, notification_reader(4), {}}};
   isolated_notification_executor executor;
   const auto result = executor.execute(captured, roots, programs);
   BOOST_REQUIRE_EQUAL(result.steps.size(), 3u);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(), 3u);
   using kind = isolated_cursor_query_kind;
   const std::array<isolated_cursor_event, 19> expected{
      isolated_cursor_query{kind::find, rows, 1, -1, 0},
      isolated_cursor_query{kind::lower_bound, rows, 2, -1, 1},
      isolated_cursor_query{kind::upper_bound, rows, 2, -1, 2},
      isolated_cursor_query{kind::end, rows, 0, -1, -2},
      isolated_cursor_query{kind::next, {}, 0, 2, 3, 100, 4},
      isolated_cursor_query{kind::next, {}, 0, 3, -2, 4, 4},
      isolated_cursor_query{kind::previous, {}, 0, -2, 3, 4, 4},
      isolated_cursor_query{kind::previous, {}, 0, 0, -1, 4, 4},
      isolated_cursor_notify{0}, isolated_cursor_write{0, 1}, isolated_cursor_write{1, 2}, isolated_cursor_write{2, 4},
      isolated_cursor_query{kind::end, rows, 0, -1, -2},
      isolated_cursor_query{kind::lower_bound, rows, 3, -1, 3},
      isolated_cursor_notify{1},
      isolated_cursor_query{kind::upper_bound, rows, 5, -1, -2},
      isolated_cursor_query{kind::find, rows, 999, -1, -2},
      isolated_cursor_query{kind::find, absent, 1, -1, -1},
      isolated_cursor_query{kind::end, absent, 0, -1, -1}};
   BOOST_REQUIRE_EQUAL(result.steps[0].cursor_events.size(), expected.size());
   for (size_t i = 0; i < expected.size(); ++i) BOOST_CHECK(result.steps[0].cursor_events[i] == expected[i]);
   for (size_t i : {1u, 2u}) BOOST_REQUIRE_EQUAL(result.steps[i].cursor_events.size(), 1u);
   BOOST_CHECK((result.steps[1].cursor_events[0] == isolated_cursor_event{isolated_cursor_query{kind::find, rows, 2, -1, 1}}));
   BOOST_CHECK((result.steps[2].cursor_events[0] == isolated_cursor_event{isolated_cursor_query{kind::find, rows, 4, -1, 3}}));
   BOOST_CHECK_EQUAL(number(result.steps[0].effects.return_value), 22u);
   BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
   BOOST_CHECK_EQUAL(canonical(2), 20u); BOOST_CHECK_EQUAL(canonical(3), 30u);
}

BOOST_FIXTURE_TEST_CASE(cursor_events_span_roots_without_recording_duplicate_recipients, wasm_fixture) {
   const std::array roots{input(1, 7), input(1, 3)};
   isolated_notification_executor executor;
   for (bool empty_carol : {false, true}) {
      const auto result = executor.execute(base, roots, notification_graph(empty_carol));
      size_t events = 0;
      for (const auto& step : result.steps) events += step.cursor_events.size();
      BOOST_CHECK_EQUAL(events, empty_carol ? 14u : 16u);
      for (size_t group : {0u, 1u}) {
         const auto& root = result.steps[group * 4];
         BOOST_REQUIRE_EQUAL(root.cursor_events.size(), 4u);
         BOOST_CHECK((root.cursor_events[1] == isolated_cursor_event{isolated_cursor_write{group, 0}}));
         BOOST_CHECK((root.cursor_events[2] == isolated_cursor_event{isolated_cursor_notify{0}}));
         BOOST_CHECK((root.cursor_events[3] == isolated_cursor_event{isolated_cursor_notify{1}}));
         BOOST_CHECK((result.steps[group * 4 + 1].cursor_events.back() == isolated_cursor_event{isolated_cursor_notify{0}}));
      }
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(cursor_event_budget_counts_repeated_reads_across_the_whole_attempt, wasm_fixture) {
   const auto code = wasm(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64) (local $count i32)
         (drop (call $read (i32.const 0) (i32.const 4)))
         (set_local $count (i32.load (i32.const 0)))
         (block $done (loop $again
            (br_if $done (i32.eqz (get_local $count)))
            (drop (call $find (i64.const )" + std::to_string(rows.code.to_uint64_t()) + ")(i64.const " +
            std::to_string(rows.scope.to_uint64_t()) + ")(i64.const " + std::to_string(rows.table.to_uint64_t()) + R"()(i64.const 1)))
            (set_local $count (i32.sub (get_local $count) (i32.const 1))) (br $again))))))");
   const std::array programs{isolated_receiver_program{rows.code, code, {}}};
   auto counted = [](uint32_t count) {
      auto root = input(); root.data.resize(4); std::memcpy(root.data.data(), &count, 4); return root;
   };
   std::vector roots{counted(isolated_notification_executor::max_cursor_events)};
   isolated_notification_executor executor;
   BOOST_CHECK_EQUAL(executor.execute(base, roots, programs).steps[0].cursor_events.size(),
                     isolated_notification_executor::max_cursor_events);
   roots.push_back(counted(1));
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   roots[0] = counted(isolated_notification_executor::max_cursor_events + 1); roots.resize(1);
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   roots = {counted(0), counted(1)};
   const auto fresh = executor.execute(base, roots, programs);
   BOOST_CHECK(fresh.steps[0].cursor_events.empty());
   BOOST_CHECK_EQUAL(fresh.steps[1].cursor_events.size(), 1u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(group_queue_deduplicates_cycles_and_preserves_shared_iterators, wasm_fixture) {
   const std::array roots{input(1, 7), input(1, 3)};
   isolated_notification_executor executor;
   for (bool empty_carol : {false, true}) {
      const auto programs = notification_graph(empty_carol);
      for (unsigned repeat = 0; repeat < 3; ++repeat) {
         const auto result = executor.execute(base, roots, programs);
         BOOST_REQUIRE_EQUAL(result.steps.size(), 8u);
         BOOST_REQUIRE_EQUAL(result.state.writes().size(), 2u);
         const std::array ordinals{1u, 3u, 4u, 5u, 2u, 6u, 7u, 8u};
         const std::array creators{0u, 1u, 1u, 3u, 0u, 2u, 2u, 6u};
         for (size_t i = 0; i < result.steps.size(); ++i) {
            const auto& step = result.steps[i];
            const size_t group = i / 4, member = i % 4;
            BOOST_CHECK_EQUAL(step.root_index, group);
            BOOST_CHECK_EQUAL(step.action_ordinal, ordinals[i]);
            BOOST_CHECK_EQUAL(step.creator_action_ordinal, creators[i]);
            BOOST_CHECK(step.receiver == programs[member].receiver);
            BOOST_CHECK_EQUAL(step.effects.writes_begin, group + (member != 0));
            BOOST_CHECK_EQUAL(step.effects.writes_end, group + 1);
            BOOST_CHECK_EQUAL(step.has_code, !(empty_carol && member == 2));
            if (member == 0) {
               BOOST_CHECK_EQUAL(number(step.effects.return_value), group ? 20u : 17u);
               BOOST_REQUIRE_EQUAL(step.effects.recipients.size(), 2u);
               BOOST_CHECK((step.effects.recipients[0] == isolated_notification{"bob"_n, group + 1}));
               BOOST_CHECK((step.effects.recipients[1] == isolated_notification{"carol"_n, group + 1}));
            } else if (!step.has_code) {
               BOOST_CHECK(step.effects.return_value.empty());
               BOOST_CHECK(step.code_hash == fc::sha256{});
               BOOST_CHECK_EQUAL(step.effects.elapsed.count(), 0);
            } else {
               BOOST_REQUIRE_EQUAL(step.effects.return_value.size(), 32u);
               int32_t iterator; std::array<uint64_t, 3> values;
               std::memcpy(&iterator, step.effects.return_value.data(), 4);
               std::memcpy(values.data(), step.effects.return_value.data() + 8, 24);
               BOOST_CHECK_EQUAL(iterator, member == 3 ? 0 : static_cast<int32_t>(member));
               BOOST_CHECK_EQUAL(values[0], member == 3 ? (group ? 20u : 17u) : (member + 1) * 10);
               BOOST_CHECK_EQUAL(values[1], step.receiver.to_uint64_t());
               BOOST_CHECK_EQUAL(values[2], group ? 3u : 7u);
               if (member == 1) {
                  BOOST_REQUIRE_EQUAL(step.effects.recipients.size(), 1u);
                  BOOST_CHECK((step.effects.recipients[0] == isolated_notification{"dave"_n, group + 1}));
               } else BOOST_CHECK(step.effects.recipients.empty());
            }
         }
         BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
         BOOST_CHECK_EQUAL(canonical(), 10u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(missing_code_and_state_require_a_fresh_whole_attempt, wasm_fixture) {
   const std::array roots{input()};
   auto programs = notification_graph();
   isolated_notification_executor executor;
   programs.pop_back();
   bool requested = false;
   try { (void)executor.execute(base, roots, programs); }
   catch (const isolated_receiver_missing& missing) {
      BOOST_CHECK(missing.receiver == "dave"_n); requested = true;
   }
   BOOST_CHECK(requested);
   BOOST_CHECK_EQUAL(canonical(), 10u);
   programs = notification_graph();
   const std::array tables{rows};
   const std::array queries{primary_query{rows, primary_query_kind::find, 1}};
   const auto sparse = frozen_primary_state::capture_queries(view, tables, queries);
   requested = false;
   try { (void)executor.execute(sparse, roots, programs); }
   catch (const primary_state_missing& missing) {
      const auto* request = std::get_if<primary_capture_request>(&missing.request);
      BOOST_REQUIRE(request && request->query);
      BOOST_CHECK_EQUAL(request->query->primary_key, 2u); requested = true;
   }
   BOOST_CHECK(requested);
   const auto result = executor.execute(base, roots, programs);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
   BOOST_CHECK_EQUAL(number(result.state.writes()[0].value), 17u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(receiver_switch_keeps_foreign_write_protection_and_allows_own_rows, wasm_fixture) {
   const std::array roots{input()};
   auto programs = notification_graph();
   isolated_notification_executor executor;
   programs[1].code = notification_reader(1,
      "(call $update (get_local $iterator) (i64.const 0) (i32.const 8) (i32.const 8))",
      R"((import "env" "db_update_i64" (func $update (param i32 i64 i32 i32))))");
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), table_access_violation);
   programs[1].code = notification_reader(1, "(call $remove (get_local $iterator))",
      R"((import "env" "db_remove_i64" (func $remove (param i32))))");
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), table_access_violation);
   const table_key child_rows{"bob"_n, rows.scope, rows.table};
   const std::array tables{rows, child_rows};
   const auto captured = frozen_primary_state::capture(view, tables);
   programs[1].code = lifecycle_steps_wasm();
   const auto result = executor.execute(captured, roots, programs);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(), 2u);
   BOOST_CHECK(result.state.writes()[1].table == child_rows);
   BOOST_CHECK(result.state.writes()[1].operation == primary_operation::store);
   BOOST_CHECK_EQUAL(number(result.state.writes()[1].value), 7u);
   BOOST_REQUIRE_EQUAL(result.steps[1].effects.return_value.size(), 16u);
   int32_t iterator, end;
   std::memcpy(&iterator, result.steps[1].effects.return_value.data(), 4);
   std::memcpy(&end, result.steps[1].effects.return_value.data() + 4, 4);
   BOOST_CHECK_EQUAL(iterator, 1); BOOST_CHECK_EQUAL(end, -3);
   BOOST_CHECK(!view.read_table(child_rows));
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(deep_failure_cancellation_and_cache_recovery_discard_prefix, wasm_fixture) {
   const std::array roots{input()};
   isolated_notification_executor executor;
   auto programs = notification_graph();
   programs[3].serial_only = true;
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_unsupported);
   programs[3].serial_only = false;
   const auto good_child = programs[3].code;
   const auto inline_child = notification_reader(1, "(call $inline (i32.const 0) (i32.const 0))",
      R"((import "env" "send_inline" (func $inline (param i32 i32))))");
   for (const auto& failed : {notification_reader(1, "unreachable"), inline_child, std::vector<char>{'b', 'a', 'd'}}) {
      programs[3].code = failed;
      BOOST_CHECK_THROW(executor.execute(base, roots, programs), std::exception);
      programs[3].code = good_child;
      BOOST_CHECK_EQUAL(number(executor.execute(base, roots, programs).steps[0].effects.return_value), 17u);
   }
   programs[3].code = notification_reader(1, "(loop $forever (br $forever))");
   programs[3].limits.max_attempt_time = std::chrono::milliseconds(10);
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), eosio::vm::timeout_exception);
   programs[3].limits = {};
   std::stop_source stop;
   auto generation = std::make_shared<std::atomic<uint64_t>>(0);
   std::jthread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(40)); generation->store(1); });
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}, {}, {generation, 0}), primary_state_invalid_attempt);
   cancel.join();
   stop.request_stop();
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, stop.get_token()), primary_state_invalid_attempt);
   isolated_cancel deadline; deadline.deadline = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}, {}, deadline), primary_state_invalid_attempt);
   programs[3].code = good_child;
   BOOST_CHECK_EQUAL(executor.execute(base, roots, programs).steps.size(), 4u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(catalog_input_and_whole_overlay_limits_are_bounded, wasm_fixture) {
   auto programs = notification_graph();
   std::vector roots{input(), input()};
   isolated_notification_executor executor;
   primary_overlay_limits limits; limits.max_write_operations = 1;
   BOOST_CHECK_THROW(executor.execute(base, roots, programs, {}, limits), primary_state_budget_exceeded);
   auto invalid = programs; invalid.push_back(programs.front());
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_unsupported);
   invalid = programs; invalid[1].receiver = name{};
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_unsupported);
   invalid = programs; invalid[0].code.clear();
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_unsupported);
   invalid = programs; invalid.resize(257);
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_budget_exceeded);
   invalid.clear();
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_budget_exceeded);
   invalid = programs; invalid[0].code.resize(64 * 1024 + 1);
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_budget_exceeded);
   invalid = programs; invalid.push_back({"unused"_n, {}, {}, true});
   for (auto& program : invalid) program.code.resize(64 * 1024);
   BOOST_CHECK_THROW(executor.execute(base, roots, invalid), primary_state_budget_exceeded);
   invalid = programs; invalid.push_back({"unused"_n, {}, {}, true});
   BOOST_CHECK_EQUAL(executor.execute(base, roots, invalid).steps.size(), 8u);
   roots[0].receiver = "bob"_n;
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_unsupported);
   roots.assign(17, input());
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   roots.clear(); BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   roots.assign(5, input()); for (auto& root : roots) root.data.resize(64 * 1024);
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   roots.assign(1, input()); roots[0].authorizations.resize(65);
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   roots.assign(1, input()); roots[0].max_return_bytes = 4097;
   BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(group_and_transaction_step_limits_include_empty_receivers, wasm_fixture) {
   isolated_notification_executor executor;
   for (size_t notifications : {63u, 64u, 65u}) {
      std::string calls;
      std::vector<isolated_receiver_program> programs{{rows.code, {}, {}}};
      for (size_t i = 0; i < notifications; ++i) {
         const name recipient{uint64_t(i + 1) << 4};
         calls += notify(recipient); programs.push_back({recipient, {}, {}});
      }
      programs[0].code = increment_wasm(notify_import, calls);
      std::vector roots{input()};
      if (notifications > 64) {
         BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
         continue;
      }
      BOOST_CHECK_EQUAL(executor.execute(base, roots, programs).steps.size(), notifications + 1);
      roots.assign(4, input());
      if (notifications == 63) {
         const auto result = executor.execute(base, roots, programs);
         BOOST_REQUIRE_EQUAL(result.steps.size(), 256u);
         BOOST_CHECK_EQUAL(result.steps.back().action_ordinal, 256u);
         BOOST_CHECK_EQUAL(number(result.state.writes().back().value), 38u);
      } else BOOST_CHECK_THROW(executor.execute(base, roots, programs), primary_state_budget_exceeded);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(thread_affinity_and_independent_notification_executors, wasm_fixture) {
   const auto programs = notification_graph();
   const std::array roots{input(1, 7), input(1, 3)};
   isolated_notification_executor owner;
   auto wrong = std::async(std::launch::async, [&] {
      try { (void)owner.execute(base, roots, programs); }
      catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(wrong.get());
   BOOST_CHECK_EQUAL(owner.execute(base, roots, programs).steps.size(), 8u);
   for (unsigned threads : {1u, 2u, 4u}) {
      std::vector<std::future<bool>> futures;
      for (unsigned i = 0; i < threads; ++i) futures.push_back(std::async(std::launch::async, [&] {
         isolated_notification_executor executor;
         for (unsigned repeat = 0; repeat < 3; ++repeat) {
            const auto result = executor.execute(base, roots, programs);
            if (result.steps.size() != 8 || result.state.writes().size() != 2 ||
                number(result.state.writes().back().value) != 20) return false;
         }
         return true;
      }));
      for (auto& future : futures) BOOST_CHECK(future.get());
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_notification_oracle_tests)

BOOST_AUTO_TEST_CASE(cursor_host_replay_preserves_original_notification_fallback_and_undo) {
   for (bool writes : {false, true}) for (unsigned mode : {0u, 1u, 2u}) {
      eosio::testing::tester original;
      original.create_accounts({rows.code, "alice"_n, "bob"_n, "carol"_n});
      const auto initializer = lifecycle_steps_wasm();
      original.set_code(rows.code, std::vector<uint8_t>(initializer.begin(), initializer.end()));
      original.produce_block();
      for (uint64_t primary = 1; primary <= 4; ++primary)
         push_raw(original, scoped_input(rows.scope, primary, primary * 10, 0));
      const std::array programs{
         isolated_receiver_program{rows.code, writes ? cursor_operations_wasm() : notification_reader(2, notify("bob"_n)), {}},
         isolated_receiver_program{"bob"_n, writes ? notification_reader(2) : notification_reader(1, notify("carol"_n)), {}},
         isolated_receiver_program{"carol"_n, notification_reader(writes ? 4 : 3), {}}};
      for (const auto& program : programs)
         original.set_code(program.receiver, std::vector<uint8_t>(program.code.begin(), program.code.end()));
      original.produce_block();
      chainbase_transaction_state_view live(original.control->db());
      const std::array tables{rows, table_key{rows.code, "fresh"_n, rows.table}};
      const auto base = frozen_primary_state::capture(live, tables);
      const std::array roots{input()};
      isolated_notification_executor executor;
      auto result = executor.execute(base, roots, programs);
      BOOST_REQUIRE_EQUAL(result.steps.size(), 3u);
      if (mode != 0) {
         if (writes && mode == 2) {
            // Deliberately corrupt a query AFTER all three writes. Original
            // host replay checks must fail and original transaction undo wins.
            auto& end = std::get<isolated_cursor_query>(result.steps[0].cursor_events.at(12));
            BOOST_CHECK(end.kind == isolated_cursor_query_kind::end); --end.result;
         }
         const auto replay = cursor_replay_wasm(result, result.steps[0], writes || mode != 2);
         original.set_code(rows.code, std::vector<uint8_t>(replay.begin(), replay.end()));
         original.produce_block();
      }
      const auto before = original.control->calculate_integrity_hash();
      BOOST_REQUIRE(original.control->is_builtin_activated(builtin_protocol_feature_t::action_return_value));
      if (writes && mode == 2) {
         BOOST_CHECK_THROW(push_raw(original, roots[0]), fc::exception);
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
         BOOST_CHECK_EQUAL(number(live.read_primary({rows, primary_query_kind::find, 2}, 8).row->value), 20u);
         BOOST_CHECK(live.read_primary({rows, primary_query_kind::find, 3}, 8).row);
         BOOST_CHECK(!live.read_primary({rows, primary_query_kind::find, 5}, 8).row);
         continue;
      }
      const auto trace = push_raw(original, roots[0]);
      BOOST_REQUIRE(trace && !trace->except);
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), result.steps.size());
      for (const auto& step : result.steps) {
         const auto& actual = trace->action_traces.at(step.action_ordinal - 1);
         BOOST_CHECK(actual.receiver == step.receiver);
         BOOST_CHECK_EQUAL(actual.creator_action_ordinal.value, step.creator_action_ordinal);
         BOOST_CHECK_EQUAL(actual.closest_unnotified_ancestor_action_ordinal.value, step.creator_action_ordinal ? 1u : 0u);
         BOOST_REQUIRE(actual.receipt);
         const auto expected_digest = generate_action_digest(actual.act, step.effects.return_value);
         if (!writes && mode == 2 && step.creator_action_ordinal) {
            // Counterexample: balances still match, but omitting pure reads
            // changes later original notifications' shared iterator numbers.
            BOOST_REQUIRE_EQUAL(actual.return_value.size(), 32u);
            BOOST_CHECK(actual.return_value != step.effects.return_value);
            BOOST_CHECK(std::equal(actual.return_value.begin() + 4, actual.return_value.end(), step.effects.return_value.begin() + 4));
            BOOST_CHECK(actual.receipt->act_digest != expected_digest);
         } else {
            BOOST_CHECK(actual.return_value == step.effects.return_value);
            BOOST_CHECK(actual.receipt->act_digest == expected_digest);
         }
      }
      BOOST_CHECK_EQUAL(number(live.read_primary({rows, primary_query_kind::find, 2}, 8).row->value), writes ? 22u : 20u);
      BOOST_CHECK_EQUAL(bool(live.read_primary({rows, primary_query_kind::find, 3}, 8).row), !writes);
      BOOST_CHECK_EQUAL(bool(live.read_primary({rows, primary_query_kind::find, 5}, 8).row), writes);
   }
}

BOOST_AUTO_TEST_CASE(notification_graph_matches_original_receipts_and_failed_tail_undo) {
   for (bool empty_carol : {false, true}) {
      eosio::testing::tester original;
      original.create_accounts({rows.code, "alice"_n, "bob"_n, "carol"_n, "dave"_n});
      const auto initializer = lifecycle_steps_wasm();
      original.set_code(rows.code, std::vector<uint8_t>(initializer.begin(), initializer.end()));
      original.produce_block();
      for (uint64_t primary = 1; primary <= 4; ++primary)
         push_raw(original, scoped_input(rows.scope, primary, primary * 10, 0));
      auto programs = notification_graph(empty_carol);
      for (const auto& program : programs)
         if (!program.code.empty()) original.set_code(program.receiver, std::vector<uint8_t>(program.code.begin(), program.code.end()));
      original.produce_block();
      chainbase_transaction_state_view live(original.control->db());
      const std::array tables{rows};
      auto base = frozen_primary_state::capture(live, tables);
      const std::array roots{input(1, 7), input(1, 3)};
      isolated_notification_executor executor;
      const auto before = original.control->calculate_integrity_hash();
      const auto result = executor.execute(base, roots, programs);
      BOOST_CHECK(original.control->calculate_integrity_hash() == before);
      const std::array calls{isolated_action_invocation{programs[0].code, roots[0], {}},
                             isolated_action_invocation{programs[0].code, roots[1], {}}};
      const auto trace = push_raw_actions(original, calls);
      BOOST_REQUIRE(trace && !trace->except);
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(), result.steps.size());
      uint64_t previous_sequence = 0;
      for (const auto& step : result.steps) {
         const auto& actual = trace->action_traces.at(step.action_ordinal - 1);
         BOOST_CHECK_EQUAL(actual.action_ordinal.value, step.action_ordinal);
         BOOST_CHECK_EQUAL(actual.creator_action_ordinal.value, step.creator_action_ordinal);
         BOOST_CHECK(actual.receiver == step.receiver);
         BOOST_CHECK(actual.return_value == step.effects.return_value);
         BOOST_CHECK(actual.act.account == roots[step.root_index].account);
         BOOST_CHECK(actual.act.data == roots[step.root_index].data);
         BOOST_REQUIRE(actual.receipt);
         BOOST_CHECK_GT(actual.receipt->global_sequence, previous_sequence);
         previous_sequence = actual.receipt->global_sequence;
      }
      BOOST_CHECK_EQUAL(number(live.read_primary({rows, primary_query_kind::find, 1}, 8).row->value), 20u);
      original.produce_block();
      programs[3].code = notification_reader(1, "unreachable");
      original.set_code("dave"_n, std::vector<uint8_t>(programs[3].code.begin(), programs[3].code.end()));
      original.produce_block();
      base = frozen_primary_state::capture(live, tables);
      const auto before_failure = original.control->calculate_integrity_hash();
      BOOST_CHECK_THROW(executor.execute(base, roots, programs), std::exception);
      BOOST_CHECK_THROW(push_raw_actions(original, calls), fc::exception);
      BOOST_CHECK(original.control->calculate_integrity_hash() == before_failure);
      BOOST_CHECK_EQUAL(number(live.read_primary({rows, primary_query_kind::find, 1}, 8).row->value), 20u);
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_transaction_oracle_tests)

BOOST_AUTO_TEST_CASE(lifecycle_actions_match_original_transaction_and_failed_tail_undo) {
   eosio::testing::tester original;
   original.create_accounts({rows.code, "alice"_n, "bob"_n});
   const auto code = lifecycle_steps_wasm();
   original.set_code(rows.code, std::vector<uint8_t>(code.begin(), code.end()));
   original.produce_block();
   chainbase_transaction_state_view live(original.control->db());
   const std::array tables{rows};
   auto base = frozen_primary_state::capture(live, tables);
   std::vector<isolated_action_invocation> calls;
   for (const auto [mode, value] : {std::pair{0u, 40u}, {3u, 42u}, {2u, 0u}, {1u, 0u}, {0u, 99u}, {2u, 0u}})
      calls.push_back({code, scoped_input(rows.scope, 7, value, mode), {}});
   isolated_transaction_executor executor;
   const auto before = original.control->calculate_integrity_hash();
   const auto result = executor.execute(base, calls);
   BOOST_CHECK(original.control->calculate_integrity_hash() == before);
   const auto trace = push_raw_actions(original, calls);
   BOOST_REQUIRE(trace && !trace->except);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(), calls.size() * 2);
   for (size_t i = 0; i < calls.size(); ++i) {
      const auto& actual = trace->action_traces[i];
      BOOST_CHECK_EQUAL(actual.action_ordinal.value, i + 1);
      BOOST_CHECK_EQUAL(actual.creator_action_ordinal.value, 0u);
      BOOST_CHECK(actual.receiver == rows.code);
      BOOST_CHECK(actual.return_value == result.actions[i].return_value);
      BOOST_REQUIRE_EQUAL(result.actions[i].recipients.size(), 1u);
      const auto& notification = trace->action_traces[calls.size() + i];
      BOOST_CHECK(notification.receiver == "bob"_n);
      BOOST_CHECK_EQUAL(notification.creator_action_ordinal.value, i + 1);
   }
   const auto final = live.read_primary({rows, primary_query_kind::find, 7}, 8);
   BOOST_REQUIRE(final.row && final.table);
   BOOST_CHECK_EQUAL(final.table->count, 1u);
   BOOST_CHECK(final.row->value == result.state.writes().back().value);
   BOOST_CHECK_EQUAL(number(final.row->value), 99u);
   BOOST_CHECK(final.row->object_id >= 0 && result.state.writes().back().object_id < 0);
   original.produce_block();
   base = frozen_primary_state::capture(live, tables);
   calls = {{code, scoped_input(rows.scope, 7, 0, 1), {}},
            {code, scoped_input(rows.scope, 7, 123, 0), {}},
            {code, scoped_input(rows.scope, 7, 456, 0), {}}}; // duplicate create AFTER a valid prefix
   const auto before_failure = original.control->calculate_integrity_hash();
   BOOST_CHECK_THROW(executor.execute(base, calls), primary_state_unsupported);
   BOOST_CHECK_THROW(push_raw_actions(original, calls), fc::exception);
   BOOST_CHECK(original.control->calculate_integrity_hash() == before_failure);
   BOOST_CHECK_EQUAL(number(live.read_primary({rows, primary_query_kind::find, 7}, 8).row->value), 99u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_cdt_host_oracle_tests)

BOOST_FIXTURE_TEST_CASE(memory_and_assertion_results_match_original_host_converters, wasm_fixture) {
   eosio::testing::tester original;
   original.create_accounts({rows.code, "alice"_n});
   uint32_t nonce = 0;
   for (const auto& item : primitive_cases) {
      BOOST_TEST_CONTEXT(item.label) {
         const auto code = primitive_wasm(item);
         original.set_code(rows.code, std::vector<uint8_t>(code.begin(), code.end()));
         original.produce_block();
         isolated_wasm_module isolated(code);
         auto arguments = input(1, 7, ++nonce);
         const auto before = original.control->calculate_integrity_hash();
         if (item.success) {
            const auto attempt = isolated.execute(base, arguments);
            const auto trace = push_raw(original, arguments);
            BOOST_REQUIRE(trace && !trace->except);
            BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
            BOOST_CHECK(attempt.return_value == trace->action_traces.front().return_value);
            BOOST_CHECK(trace->action_traces.front().account_ram_deltas.empty());
         } else {
            BOOST_CHECK_THROW(isolated.execute(base, arguments), primary_state_unsupported);
            BOOST_CHECK_THROW(push_raw(original, arguments), fc::exception);
            BOOST_CHECK(original.control->calculate_integrity_hash() == before);
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(unbounded_memory_growth_matches_original_or_requires_fallback, wasm_fixture) {
   eosio::testing::tester original;
   original.create_accounts({rows.code, "alice"_n});
   uint32_t nonce = 0;
   for (bool at_start : {false, true}) {
      for (const auto declaration : {"(memory 1)", "(memory 1 17)", "(memory 1 2)"}) {
         for (uint32_t growth : {0u, 1u, 15u, 16u}) {
            BOOST_TEST_CONTEXT(declaration << " growth=" << growth << " start=" << at_start) {
               const auto code = growth_wasm(declaration, growth, at_start);
               original.set_code(rows.code, std::vector<uint8_t>(code.begin(), code.end()));
               original.produce_block();
               isolated_wasm_module isolated(code);
               const auto arguments = input(1, 7, ++nonce);
               const auto trace = push_raw(original, arguments);
               BOOST_REQUIRE(trace && !trace->except);
               BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
               if (growth <= 15) {
                  const auto result = isolated.execute(base, arguments);
                  BOOST_CHECK(result.return_value == trace->action_traces.front().return_value);
               } else {
                  BOOST_CHECK_THROW(isolated.execute(base, arguments), primary_state_unsupported);
                  const bool module_full = std::string_view(declaration) == "(memory 1 2)";
                  BOOST_CHECK_EQUAL(number(trace->action_traces.front().return_value),
                     (uint64_t(module_full ? 1 : 17) << 32) | uint32_t(module_full ? -1 : 1));
               }
            }
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_worker_pool_tests)

BOOST_FIXTURE_TEST_CASE(cdt_primitive_hosts_and_growth_are_isolated_across_four_workers, wasm_fixture) {
   isolated_worker_pool pool(4);
   const auto success = primitive_wasm(primitive_cases[0]);
   const auto grow = growth_wasm("(memory 1)", 15);
   const auto capped = growth_wasm("(memory 1)", 16);
   const auto failure = primitive_wasm(primitive_cases.back());
   const std::array codes{success, grow, capped, failure};
   for (unsigned round = 0; round < 4; ++round) {
      std::vector<ordered_attempt_batch::request> requests;
      for (unsigned i = 0; i < 16; ++i) {
         const auto& code = codes[i % codes.size()];
         const auto hash = fc::sha256::hash(code.data(), code.size());
         requests.push_back({fc::sha256::hash(std::to_string(i)), single_transaction_fingerprint(hash, {}, input())});
      }
      ordered_attempt_batch batch(base, requests);
      std::vector<isolated_worker_job> jobs;
      for (unsigned i = 0; i < requests.size(); ++i)
         jobs.push_back({batch.get_ticket(i), codes[i % codes.size()], input(), {}});
      const auto outputs = pool.run(std::move(jobs));
      BOOST_REQUIRE_EQUAL(outputs.size(), requests.size());
      for (unsigned i = 0; i < outputs.size(); ++i) {
         const auto& output = outputs[i];
         BOOST_CHECK(!output.request);
         if (i % codes.size() < 2) {
            BOOST_REQUIRE(output.result);
            BOOST_CHECK(output.result->state.writes().empty());
            if (i % codes.size() == 1) BOOST_CHECK_EQUAL(number(output.result->actions.front().return_value), (uint64_t{16} << 32) | 1);
            else BOOST_CHECK_EQUAL(output.result->actions.front().return_value.size(), 64u);
         } else BOOST_CHECK(!output.result);
      }
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(persistent_one_two_four_threads_run_multiple_bounded_batches, wasm_fixture) {
   const auto code = increment_wasm();
   const auto hash = fc::sha256::hash(code.data(), code.size());
   for (size_t count : {1u, 2u, 4u}) {
      isolated_worker_pool pool(count);
      bool overlapping = false;
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<ordered_attempt_batch::request> requests;
         std::vector<isolated_action_input> args;
         for (size_t i = 0; i < 8; ++i) {
            args.push_back(input(i % 4 + 1, 7 + round, 150000));
            requests.push_back({fc::sha256::hash(std::to_string(i)), single_transaction_fingerprint(hash, {}, args.back())});
         }
         ordered_attempt_batch batch(base, requests);
         std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < args.size(); ++i) jobs.push_back({batch.get_ticket(i), code, args[i], {}});
         const auto results = pool.run(std::move(jobs));
         BOOST_REQUIRE_EQUAL(results.size(), args.size());
         for (size_t i = 0; i < results.size(); ++i) {
            BOOST_REQUIRE(results[i].result);
            BOOST_CHECK_EQUAL(number(results[i].result->actions.front().return_value), (i % 4 + 1) * 10 + 7 + round);
            BOOST_CHECK(results[i].result->execution_fingerprint == requests[i].execution_fingerprint);
            BOOST_CHECK(results[i].elapsed.count() > 0);
            for (size_t j = 0; j < i; ++j)
               overlapping |= std::max(results[i].began, results[j].began) < std::min(results[i].ended, results[j].ended);
         }
      }
      BOOST_CHECK_EQUAL(overlapping, count > 1);
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(transaction_jobs_share_prefixes_but_not_workers_and_track_late_conflicts, wasm_fixture) {
   const auto code = increment_wasm();
   const auto hash = fc::sha256::hash(code.data(), code.size());
   for (size_t threads : {1u, 2u, 4u}) {
      isolated_worker_pool pool(threads);
      bool overlap = false;
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<std::vector<isolated_action_invocation>> calls;
         std::vector<ordered_attempt_batch::request> requests;
         for (size_t i = 0; i < 8; ++i) {
            // Split the single-action fixture's total work between two steps.
            // Doubling it can legitimately hit the unchanged TRANSACTION-wide
            // 500 ms cap under sanitizers; this checks overlap, not capacity.
            calls.push_back({{code, input(i % 4 + 1, 7, 75000), {}},
                             {code, input(i % 4 + 1, 3, 75000), {}}});
            const std::array fingerprints{
               isolated_execution_fingerprint(hash, {}, calls.back()[0].input),
               isolated_execution_fingerprint(hash, {}, calls.back()[1].input)};
            requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_transaction_fingerprint(fingerprints)});
         }
         ordered_attempt_batch batch(base, requests);
         std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < calls.size(); ++i) jobs.emplace_back(batch.get_ticket(i), calls[i]);
         const auto results = pool.run(std::move(jobs));
         for (size_t i = 0; i < results.size(); ++i) {
            BOOST_REQUIRE_MESSAGE(results[i].result, "multi workers=" << threads << " round=" << round << " job=" << i);
            const auto& output = *results[i].result;
            BOOST_REQUIRE_EQUAL(output.actions.size(), 2u);
            BOOST_CHECK_EQUAL(number(output.actions[0].return_value), (i % 4 + 1) * 10 + 7);
            BOOST_CHECK_EQUAL(number(output.actions[1].return_value), (i % 4 + 1) * 10 + 10);
            BOOST_CHECK(output.execution_fingerprint == requests[i].execution_fingerprint);
            BOOST_CHECK(results[i].elapsed == output.actions[0].elapsed + output.actions[1].elapsed);
            BOOST_CHECK(output.actions[0].ended <= output.actions[1].began);
            for (size_t j = 0; j < i; ++j) for (const auto& a : output.actions)
               for (const auto& b : results[j].result->actions)
                  overlap |= std::max(a.began, b.began) < std::min(a.ended, b.ended);
         }
      }
      BOOST_CHECK_EQUAL(overlap, threads > 1); // actual WASM step intervals, not compilation gaps

      // The conflict is in action TWO of the first transaction. A root-only
      // read/write set would incorrectly accept the second transaction.
      const std::vector<std::vector<isolated_action_invocation>> calls{
         {{code, input(2), {}}, {code, input(1), {}}},
         {{code, input(1), {}}, {code, input(3), {}}}};
      std::vector<ordered_attempt_batch::request> requests;
      for (size_t i = 0; i < calls.size(); ++i) {
         std::vector<fc::sha256> fingerprints;
         for (const auto& action : calls[i]) fingerprints.push_back(isolated_execution_fingerprint(hash, {}, action.input));
         requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_transaction_fingerprint(fingerprints)});
      }
      ordered_attempt_batch batch(base, requests);
      const auto results = pool.run({{batch.get_ticket(0), calls[0]}, {batch.get_ticket(1), calls[1]}});
      size_t adopted = 0, fallback = 0;
      for (size_t i = 0; i < 2; ++i) {
         BOOST_REQUIRE(results[i].result);
         batch.run_next(batch.get_ticket(i), requests[i].transaction_id, results[i].result,
            [&](const auto&) { ++adopted; }, [&] { ++fallback; });
      }
      BOOST_CHECK_EQUAL(adopted, 1u); BOOST_CHECK_EQUAL(fallback, 1u);
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(transaction_job_failure_and_owner_bounds_expose_no_adoptable_prefix, wasm_fixture) {
   isolated_worker_pool pool(2);
   const auto code = increment_wasm();
   const std::array<ordered_attempt_batch::request, 1> requests{{{fc::sha256::hash("multi"), fc::sha256::hash("multi")}}};
   ordered_attempt_batch batch(base, requests);
   isolated_worker_job job{batch.get_ticket(0), {{code, input(), {}}, {code, input(1, 7, 0, 3), {}}}};
   const auto failed = pool.run({job});
   BOOST_CHECK(!failed[0].result); BOOST_CHECK(!failed[0].request);
   job.actions[1].input = input();
   const auto retry = pool.run({job});
   BOOST_REQUIRE(retry[0].result);
   BOOST_CHECK_EQUAL(number(retry[0].result->actions[1].return_value), 24u);
   const auto good = job;
   job.actions.resize(17, good.actions[0]);
   BOOST_CHECK_THROW(pool.run({job}), primary_state_budget_exceeded);
   job.actions.clear();
   BOOST_CHECK_THROW(pool.run({job}), primary_state_budget_exceeded);
   job.actions.resize(5, good.actions[0]);
   for (auto& action : job.actions) action.code.resize(64 * 1024);
   BOOST_CHECK_THROW(pool.run({job}), primary_state_budget_exceeded);
   job.actions.assign(5, good.actions[0]);
   for (auto& action : job.actions) action.input.data.resize(64 * 1024);
   BOOST_CHECK_THROW(pool.run({job}), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(failed_canceled_and_changed_code_jobs_do_not_poison_reused_pool, wasm_fixture) {
   isolated_worker_pool pool(2);
   const auto good = increment_wasm();
   const auto bad = wasm(R"((module (import "env" "unsupported" (func))
      (func (export "apply") (param i64 i64 i64))))");
   const std::array<ordered_attempt_batch::request, 2> requests{{
      {fc::sha256::hash("a"), fc::sha256::hash("a")}, {fc::sha256::hash("b"), fc::sha256::hash("b")}}};
   for (unsigned phase = 0; phase < 4; ++phase) {
      ordered_attempt_batch batch(base, requests);
      std::vector<isolated_worker_job> jobs;
      isolated_wasm_limits limits; limits.max_attempt_time = std::chrono::milliseconds(20);
      jobs.push_back({batch.get_ticket(0), phase == 0 ? bad : good, input(1, 7, 0, phase == 1 ? 1 : 0), limits});
      jobs.push_back({batch.get_ticket(1), good, input(), limits});
      if (phase == 2) batch.close();
      const auto results = pool.run(std::move(jobs));
      BOOST_CHECK_EQUAL(bool(results[0].result), phase == 3);
      BOOST_CHECK_EQUAL(bool(results[1].result), phase != 2);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(real_wasm_results_pass_ordered_coordinator_for_independent_scopes, wasm_fixture) {
   const table_key other{rows.code, "bob"_n, rows.table};
   const auto& table = db.create<table_id_object>([&](auto& row) {
      row.code = other.code; row.scope = other.scope; row.table = other.table; row.payer = "alice"_n; row.count = 1;
   });
   db.create<key_value_object>([&](auto& row) {
      row.t_id = table.id; row.primary_key = 1; row.payer = "alice"_n;
      const uint64_t value = 10; row.value.assign(reinterpret_cast<const char*>(&value), sizeof(value));
   });
   const std::array keys{rows, other};
   base = frozen_primary_state::capture(view, keys);
   auto second = input(); const auto scope = other.scope.to_uint64_t();
   std::memcpy(second.data.data(), &scope, sizeof(scope));
   const auto code = increment_wasm(); const auto hash = fc::sha256::hash(code.data(), code.size());
   const std::array<ordered_attempt_batch::request, 2> requests{{
      {fc::sha256::hash("a"), single_transaction_fingerprint(hash, {}, input())},
      {fc::sha256::hash("b"), single_transaction_fingerprint(hash, {}, second)}}};
   ordered_attempt_batch batch(base, requests);
   isolated_worker_pool pool(2);
   auto results = pool.run({{batch.get_ticket(0), code, input(), {}}, {batch.get_ticket(1), code, second, {}}});
   size_t adopted = 0;
   for (size_t i = 0; i < 2; ++i)
      batch.run_next(batch.get_ticket(i), requests[i].transaction_id, results[i].result,
         [&](const auto& result) { BOOST_CHECK_EQUAL(number(result.actions.front().return_value), 17u); ++adopted; },
         [] { BOOST_FAIL("independent table must be usable"); });
   BOOST_CHECK_EQUAL(adopted, 2u);
   BOOST_CHECK_EQUAL(canonical(), 10u); // callbacks above intentionally do NOT adopt into canonical DB
}

BOOST_FIXTURE_TEST_CASE(pool_bounds_and_thread_affinity, wasm_fixture) {
   BOOST_CHECK_THROW(isolated_worker_pool{0}, primary_state_budget_exceeded);
   BOOST_CHECK_THROW(isolated_worker_pool{17}, primary_state_budget_exceeded);
   isolated_worker_pool pool(1);
   BOOST_CHECK_THROW(pool.run({}), primary_state_budget_exceeded);
   const std::array<ordered_attempt_batch::request, 1> requests{{{fc::sha256::hash("a"), fc::sha256::hash("b")}}};
   ordered_attempt_batch batch(base, requests);
   isolated_worker_job job{batch.get_ticket(0), increment_wasm(), input(), {}};
   BOOST_CHECK_THROW(pool.run(std::vector<isolated_worker_job>(65, job)), primary_state_budget_exceeded);
   job.actions.front().code.resize(64 * 1024 + 1);
   BOOST_CHECK_THROW(pool.run({job}), primary_state_budget_exceeded);
   auto worker = std::async(std::launch::async, [&] {
      try { (void)pool.run({}); } catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(worker.get());
}

BOOST_FIXTURE_TEST_CASE(account_host_requests_owned_evidence_and_tracks_negative_reads, wasm_fixture) {
   db.create<account_object>([](auto& obj) { obj.name = "alice"_n; });
   const auto code = increment_wasm(
      R"((import "env" "is_account" (func $account (param i64) (result i32))))",
      "(i32.store (i32.const 64) (call $account (i64.const " + std::to_string("alice"_n.to_uint64_t()) + ")))" +
      "(i32.store (i32.const 68) (call $account (i64.const " + std::to_string("bob"_n.to_uint64_t()) + ")))");
   isolated_wasm_module module(code);
   BOOST_CHECK_THROW(module.execute(base, input()), primary_state_missing); // after private write
   const auto hash = fc::sha256::hash(code.data(), code.size());
   const std::array<ordered_attempt_batch::request, 1> requests{{
      {fc::sha256::hash("account"), single_transaction_fingerprint(hash, {}, input())}}};
   ordered_attempt_batch batch(base, requests);
   isolated_worker_pool pool(2);
   const auto discovery = pool.run({{batch.get_ticket(0), code, input(), {}}});
   BOOST_REQUIRE(discovery[0].request);
   BOOST_CHECK(!discovery[0].result);
   BOOST_REQUIRE(std::holds_alternative<account_capture_request>(*discovery[0].request));
   BOOST_CHECK(std::get<account_capture_request>(*discovery[0].request).account == "alice"_n);
   const std::array names{"alice"_n, "bob"_n};
   const std::array queries{primary_query{rows, primary_query_kind::find, 1}};
   auto captured = frozen_primary_state::capture_queries(view, {}, queries, {}, {}, names);
   const auto result = module.execute(captured, input());
   BOOST_CHECK_EQUAL(number(result.return_value), 1u);
   BOOST_CHECK_EQUAL(result.state.read_accounts().size(), 2u);
   BOOST_CHECK_EQUAL(result.state.writes().size(), 1u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(notification_journal_is_ordered_deduplicated_bounded_and_reset_after_failure, wasm_fixture) {
   const std::string imported = R"((import "env" "require_recipient" (func $notify (param i64))))";
   auto notify = [](name account) { return "(call $notify (i64.const " + std::to_string(account.to_uint64_t()) + "))"; };
   const auto code = increment_wasm(imported, notify("test"_n) + notify("bob"_n) + notify("alice"_n) + notify("bob"_n));
   isolated_wasm_module module(code);
   for (unsigned repeat = 0; repeat < 3; ++repeat) {
      BOOST_CHECK_THROW(module.execute(base, input(1, 7, 0, 3)), primary_state_unsupported);
      const auto result = module.execute(base, input());
      BOOST_CHECK((result.recipients == std::vector<isolated_notification>{{"bob"_n, 1}, {"alice"_n, 1}}));
      BOOST_CHECK_EQUAL(result.state.writes().size(), 1u);
   }
   std::string tail;
   for (uint64_t i = 1; i <= isolated_action_result::max_recipients; ++i) tail += notify(name{i});
   isolated_wasm_module limit(increment_wasm(imported, tail));
   BOOST_CHECK_EQUAL(limit.execute(base, input()).recipients.size(), isolated_action_result::max_recipients);
   isolated_wasm_module excessive(increment_wasm(imported, tail + notify(name{1000})));
   BOOST_CHECK_THROW(excessive.execute(base, input()), primary_state_budget_exceeded);
   isolated_wasm_module late_failure(increment_wasm(imported, notify("alice"_n) + "(unreachable)"));
   BOOST_CHECK_THROW(late_failure.execute(base, input()), eosio::vm::exception);
   const auto start_code = wasm("(module " + imported +
      "(func $start " + notify("bob"_n) + ") (start $start)" +
      "(func (export \"apply\") (param i64 i64 i64) " + notify("alice"_n) + notify("bob"_n) + "))");
   isolated_wasm_module start_module(start_code);
   BOOST_CHECK((start_module.execute(base, input()).recipients ==
      std::vector<isolated_notification>{{"bob"_n, 0}, {"alice"_n, 0}}));
   const auto interleaved = increment_wasm(imported,
      notify("alice"_n) + "(call $update (get_local $iterator) (i64.const 0) (i32.const 64) (i32.const 3))" +
      notify("carol"_n) + notify("bob"_n) + notify("alice"_n), notify("bob"_n));
   isolated_wasm_module ordered_module(interleaved);
   const auto ordered = ordered_module.execute(base, input());
   BOOST_CHECK_EQUAL(ordered.state.writes().size(), 2u);
   BOOST_CHECK((ordered.recipients ==
      std::vector<isolated_notification>{{"bob"_n, 0}, {"alice"_n, 1}, {"carol"_n, 2}}));
   for (auto host : {"is_account", "require_recipient"}) {
      const auto wrong_signature = wasm(std::string("(module (import \"env\" \"") + host +
         "\" (func (param i32))) (func (export \"apply\") (param i64 i64 i64)))");
      BOOST_CHECK_THROW(isolated_wasm_module{wrong_signature}, eosio::vm::wasm_link_exception);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(pool_reports_owned_missing_queries_then_restarts_on_fresh_sparse_base, wasm_fixture) {
   const auto code = increment_wasm();
   const auto hash = fc::sha256::hash(code.data(), code.size());
   const std::array<ordered_attempt_batch::request, 2> requests{{
      {fc::sha256::hash("first"), single_transaction_fingerprint(hash, {}, input(1))},
      {fc::sha256::hash("second"), single_transaction_fingerprint(hash, {}, input(2))}}};
   isolated_worker_pool pool(2);
   auto empty = frozen_primary_state::capture_queries(view, {}, {});
   ordered_attempt_batch discovery(empty, requests);
   const auto missing = pool.run({{discovery.get_ticket(0), code, input(1), {}},
                                  {discovery.get_ticket(1), code, input(2), {}}});
   std::vector<primary_query> queries;
   for (size_t i = 0; i < missing.size(); ++i) {
      BOOST_CHECK(!missing[i].result);
      BOOST_REQUIRE(missing[i].request);
      BOOST_REQUIRE(std::holds_alternative<primary_capture_request>(*missing[i].request));
      const auto& request = std::get<primary_capture_request>(*missing[i].request);
      BOOST_CHECK(request.table == rows);
      BOOST_REQUIRE(request.query);
      BOOST_CHECK_EQUAL(request.query->primary_key, i + 1);
      queries.push_back(*request.query);
   }
   discovery.close();
   auto sparse = frozen_primary_state::capture_queries(view, {}, queries);
   BOOST_CHECK_EQUAL(sparse->row_count(), 2u);
   ordered_attempt_batch ready(sparse, requests);
   auto results = pool.run({{ready.get_ticket(0), code, input(1), {}},
                            {ready.get_ticket(1), code, input(2), {}}});
   for (size_t i = 0; i < results.size(); ++i) {
      BOOST_REQUIRE(results[i].result);
      BOOST_CHECK(!results[i].request);
      ready.run_next(ready.get_ticket(i), requests[i].transaction_id, results[i].result,
         [&](const auto& output) { BOOST_CHECK_EQUAL(number(output.actions.front().return_value), (i + 1) * 10 + 7); },
         [] { BOOST_FAIL("sparse independent rows must be usable"); });
   }
   BOOST_CHECK_EQUAL(canonical(), 10u); // discovery and private updates never changed DB
}

BOOST_FIXTURE_TEST_CASE(external_interrupt_cancels_running_and_queued_attempts_without_controller_refs, wasm_fixture) {
   isolated_worker_pool pool(1);
   auto generation = std::make_shared<std::atomic<uint64_t>>(0);
   isolated_wasm_limits limits; limits.max_attempt_time = std::chrono::seconds(1);
   const auto code = increment_wasm();
   std::vector<ordered_attempt_batch::request> requests;
   for (size_t i = 0; i < 8; ++i) requests.push_back({fc::sha256::hash(std::to_string(i)), fc::sha256::hash("probe")});
   ordered_attempt_batch batch(base, requests);
   std::vector<isolated_worker_job> jobs;
   for (size_t i = 0; i < requests.size(); ++i)
      jobs.push_back({batch.get_ticket(i), code, input(1, 7, 0, 1), limits, {generation, 0}});
   auto interrupter = std::async(std::launch::async, [generation] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      generation->fetch_add(1, std::memory_order_relaxed);
   });
   const auto start = std::chrono::steady_clock::now();
   const auto results = pool.run(std::move(jobs));
   interrupter.get();
   BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
   for (const auto& result : results) BOOST_CHECK(!result.result);
   const auto retry = pool.run({{batch.get_ticket(0), code, input(), limits, {generation, 1}}});
   BOOST_REQUIRE(retry[0].result); BOOST_CHECK_EQUAL(number(retry[0].result->actions.front().return_value), 17u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(one_absolute_deadline_bounds_running_and_queued_work_then_pool_recovers, wasm_fixture) {
   isolated_worker_pool pool(1);
   isolated_wasm_limits limits; limits.max_attempt_time = std::chrono::seconds(1);
   const auto code = increment_wasm();
   std::vector<ordered_attempt_batch::request> requests;
   for (size_t i = 0; i < 8; ++i) requests.push_back({fc::sha256::hash(std::to_string(i)), fc::sha256::hash("deadline")});
   ordered_attempt_batch batch(base, requests);
   isolated_cancel cancel;
   cancel.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
   std::vector<isolated_worker_job> jobs;
   for (size_t i = 0; i < requests.size(); ++i)
      jobs.push_back({batch.get_ticket(i), code, input(1, 7, 0, 1), limits, cancel});
   const auto start = std::chrono::steady_clock::now();
   const auto results = pool.run(std::move(jobs));
   BOOST_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
   for (const auto& result : results) BOOST_CHECK(!result.result);
   const auto retry = pool.run({{batch.get_ticket(0), code, input(), limits, {}}});
   BOOST_REQUIRE(retry[0].result); BOOST_CHECK_EQUAL(number(retry[0].result->actions.front().return_value), 17u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()
