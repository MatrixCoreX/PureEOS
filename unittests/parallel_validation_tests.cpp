#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/merkle.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/json.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include "parallel_test_diagnostics.hpp"
#include "parallel_test_runtime.hpp"

using namespace eosio::chain;
using namespace eosio::testing;

namespace {
std::vector<uint8_t> compile(const std::string& wat) { return wast_to_wasm(wat); }

std::vector<uint8_t> cdt_fixture_code() {
   const auto fixture = std::filesystem::path(__FILE__).parent_path() / "test-contracts/parallel_cdt/parallel_cdt.wasm";
   std::ifstream stream(fixture, std::ios::binary);
   BOOST_REQUIRE_MESSAGE(stream.good(), "missing versioned CDT fixture: " << fixture);
   const std::vector<uint8_t> code{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
   BOOST_REQUIRE(!code.empty());
   BOOST_REQUIRE(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()) ==
      fc::sha256{"0365f466b059f6985baa7a1aa1415e59139e58bdb10344a9c457b59909fe8de8"});
   return code;
}

std::vector<uint8_t> token_fixture_code() {
   // Default: the repository's unchanged EOS token. An explicit second run can
   // exercise the unchanged PureEOS reference-contracts artifact too. Pin BOTH
   // hashes; do not silently accept a simplified or substituted token fixture.
   const char* external = std::getenv("PUREEOS_PARALLEL_TOKEN_WASM");
   const auto path = external ? std::filesystem::path(external) :
      std::filesystem::path(__FILE__).parent_path() / "contracts/eosio.token/eosio.token.wasm";
   std::ifstream stream(path, std::ios::binary);
   BOOST_REQUIRE_MESSAGE(stream.good(), "missing token artifact: " << path);
   const std::vector<uint8_t> code{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
   const auto expected = external ? "ea1bd149e28f21420450b6c7b2c5be600ef9922e44c383fa4489b025aac0eb46" :
      "4c76fdcd411cafbf3af1c0987bbb093bac2bfd2e8a669f12feae785ffd43a7c3";
   BOOST_REQUIRE(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()) == fc::sha256{expected});
   return code;
}

signed_transaction token_transaction(tester& chain, name from, name to, int64_t amount, uint64_t nonce) {
   signed_transaction trx;
   trx.actions.push_back(chain.get_action("test"_n, "transfer"_n, {{from, "active"_n}},
      fc::mutable_variant_object()("from", from)("to", to)
         ("quantity", asset{amount, symbol::from_string("4,TKN")}.to_string())("memo", std::to_string(nonce))));
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(from, "active"), chain.control->get_chain_id());
   return trx;
}

signed_transaction combine_actions(tester& chain, std::vector<signed_transaction> inputs) {
   BOOST_REQUIRE(!inputs.empty());
   signed_transaction result;
   result.max_cpu_usage_ms = inputs.front().max_cpu_usage_ms;
   std::set<std::pair<name, name>> permissions;
   for (auto& trx : inputs) for (auto& act : trx.actions) {
      for (const auto& auth : act.authorization) permissions.emplace(auth.actor, auth.permission);
      result.actions.push_back(std::move(act));
   }
   chain.set_transaction_headers(result);
   for (const auto& [actor, permission] : permissions)
      result.sign(chain.get_private_key(actor, permission.to_string()), chain.control->get_chain_id());
   return result;
}

void setup_token(tester& chain, const std::vector<uint8_t>& code) {
   chain.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n, "erin"_n, "frank"_n,
                          "grace"_n, "sponsor"_n, "blank"_n});
   chain.set_code("test"_n, code);
   std::ifstream abi(std::filesystem::path(__FILE__).parent_path() / "contracts/eosio.token/eosio.token.abi");
   BOOST_REQUIRE(abi.good());
   chain.set_abi("test"_n, std::string{std::istreambuf_iterator<char>(abi), std::istreambuf_iterator<char>()});
   BOOST_REQUIRE(!chain.push_action("test"_n, "create"_n, "test"_n, fc::mutable_variant_object()
      ("issuer", "alice")("maximum_supply", "1000000.0000 TKN"))->except);
   BOOST_REQUIRE(!chain.push_action("test"_n, "issue"_n, "alice"_n, fc::mutable_variant_object()
      ("to", "alice")("quantity", "10000.0000 TKN")("memo", "setup"))->except);
   for (const auto owner : {"bob"_n, "carol"_n, "dave"_n, "erin"_n, "frank"_n, "grace"_n, "sponsor"_n}) {
      const auto payer = owner == "sponsor"_n ? "alice"_n : owner;
      BOOST_REQUIRE(!chain.push_action("test"_n, "open"_n, payer, fc::mutable_variant_object()
         ("owner", owner)("symbol", "4,TKN")("ram_payer", payer))->except);
      if (owner != "grace"_n) {
         auto trx = token_transaction(chain, "alice"_n, owner, 1000000, owner.to_uint64_t());
         BOOST_REQUIRE(!chain.push_transaction(trx)->except);
      }
   }
}

int64_t token_balance(tester& chain, name owner) {
   const auto row = chain.get_row_by_account("test"_n, owner, "accounts"_n,
      name{symbol::from_string("4,TKN").to_symbol_code().value});
   BOOST_REQUIRE_EQUAL(row.size(), 16u);
   int64_t result; std::memcpy(&result, row.data(), sizeof(result));
   return result;
}

struct resize_step { uint64_t bytes; name payer; };
using resize_steps = std::array<resize_step, 3>;

std::vector<uint8_t> resize_code() {
   std::string text = R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (memory 1 1)
      (data (i32.const 128) "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!!??")
      (func (export "apply") (param $receiver i64) (param i64 i64) (local $it i32)
         (drop (call $read (i32.const 0) (i32.const 64)))
         (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
            (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (i64.const 1)))
         (i32.store (i32.const 256) (call $get (get_local $it) (i32.const 0) (i32.const 0)))
   )";
   text += "(call $notify (i64.const " + std::to_string("bob"_n.to_uint64_t()) + "))";
   for (unsigned i = 0; i < 3; ++i) {
      text += "(call $update (get_local $it) (i64.load (i32.const " + std::to_string(16 + i * 16) +
         ")) (i32.const 128) (i32.load (i32.const " + std::to_string(8 + i * 16) + ")))";
      text += "(i32.store (i32.const " + std::to_string(260 + i * 4) +
         ") (call $get (get_local $it) (i32.const 0) (i32.const 0)))";
      if (i == 0) text += "(call $notify (i64.const " + std::to_string("alice"_n.to_uint64_t()) + "))";
   }
   text += "(call $notify (i64.const " + std::to_string("bob"_n.to_uint64_t()) + "))"
      "(call $return (i32.const 256) (i32.const 16))))";
   return compile(text);
}

signed_transaction resize_transaction(tester& chain, name scope, const resize_steps& steps, uint64_t nonce,
                                      bool bob_authorizes = false) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = "resize"_n; act.authorization = {{"alice"_n, "active"_n}};
   if (bob_authorizes) act.authorization.push_back({"bob"_n, "active"_n});
   const std::array<uint64_t, 8> data{scope.to_uint64_t(), steps[0].bytes, steps[0].payer.to_uint64_t(),
      steps[1].bytes, steps[1].payer.to_uint64_t(), steps[2].bytes, steps[2].payer.to_uint64_t(), nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("alice"_n, "active"), chain.control->get_chain_id());
   if (bob_authorizes) trx.sign(chain.get_private_key("bob"_n, "active"), chain.control->get_chain_id());
   return trx;
}

std::vector<uint8_t> lifecycle_code() {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_end_i64" (func $end (param i64 i64 i64) (result i32)))
      (import "env" "db_next_i64" (func $next (param i32 i32) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_remove_i64" (func $erase (param i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $scope i64) (local $mode i64) (local $payer i64) (local $table i64)
         (local $old i32) (local $new i32) (local $oldend i32) (local $size i32)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $scope (i64.load (i32.const 0))) (set_local $mode (i64.load (i32.const 8)))
         (set_local $payer (i64.load (i32.const 16)))
         (set_local $table (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"())
         (set_local $old (call $find (get_local $receiver) (get_local $scope) (get_local $table) (i64.const 1)))
         (set_local $oldend (call $end (get_local $receiver) (get_local $scope) (get_local $table)))
         (i64.store (i32.const 64) (i64.const 77))
         (call $notify (i64.const )" + std::to_string("bob"_n.to_uint64_t()) + R"())
         (if (i64.eq (get_local $mode) (i64.const 1)) (then
            (drop (call $store (get_local $scope) (get_local $table) (get_local $payer)
               (i64.const 2) (i32.const 64) (i32.const 0)))))
         (call $erase (get_local $old))
         (call $notify (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"())
         (set_local $new (i32.const -1))
         (if (i64.ne (get_local $mode) (i64.const 2)) (then
            (set_local $size (if (result i32) (i64.eq (get_local $mode) (i64.const 1))
               (then (i32.const 4)) (else (i32.const 8))))
            (set_local $new (call $store (get_local $scope) (get_local $table) (get_local $payer)
               (i64.const 1) (i32.const 64) (get_local $size)))))
         (if (i64.eq (get_local $mode) (i64.const 3))
            (then (drop (call $get (get_local $old) (i32.const 0) (i32.const 0)))))
         (if (i64.eq (get_local $mode) (i64.const 4)) (then (unreachable)))
         (i32.store (i32.const 128) (get_local $old))
         (i32.store (i32.const 132) (get_local $oldend))
         (i32.store (i32.const 136) (get_local $new))
         (i32.store (i32.const 140) (call $end (get_local $receiver) (get_local $scope) (get_local $table)))
         (i32.store (i32.const 144) (get_local $size))
         (i32.store (i32.const 148) (i32.const -1))
         (i64.store (i32.const 152) (i64.const 123))
         (if (i32.ge_s (get_local $new) (i32.const 0)) (then
            (i32.store (i32.const 144) (call $get (get_local $new) (i32.const 0) (i32.const 0)))
            (i32.store (i32.const 148) (call $next (get_local $new) (i32.const 152)))))
         (call $return (i32.const 128) (i32.const 32)))))");
}

signed_transaction lifecycle_transaction(tester& chain, name scope, uint64_t mode, name payer, uint64_t nonce,
                                         bool bob_authorizes = false) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = "life"_n; act.authorization = {{"alice"_n, "active"_n}};
   if (bob_authorizes) act.authorization.push_back({"bob"_n, "active"_n});
   const std::array<uint64_t, 4> data{scope.to_uint64_t(), mode, payer.to_uint64_t(), nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("alice"_n, "active"), chain.control->get_chain_id());
   if (bob_authorizes) trx.sign(chain.get_private_key("bob"_n, "active"), chain.control->get_chain_id());
   return trx;
}

std::vector<uint8_t> initialize_code(bool keyed = false) {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (call $read (i32.const 0) (i32.const )" + std::to_string(keyed ? 40 : 32) + R"()))
         (drop (call $store (i64.load (i32.const 0)) (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"()
            (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + ") " +
            (keyed ? "(i64.load (i32.const 32))" : "(i64.const 1)") + R"( (i32.const 8) (i32.const 8))))))");
}
std::vector<uint8_t> update_code(bool extra = false, bool keyed = false) {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $it i32) (local $work i32)
         (drop (call $read (i32.const 0) (i32.const )" + std::to_string(keyed ? 40 : 32) + R"()))
         (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
            (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + ") " +
            (keyed ? "(i64.load (i32.const 32))" : "(i64.const 1)") + R"())
         (drop (call $get (get_local $it) (i32.const 64) (i32.const 8)))
         (i64.store (i32.const 64) (i64.add (i64.load (i32.const 64)) (i64.load (i32.const 8))))
         )" + (extra ? "(i64.store (i32.const 64) (i64.add (i64.load (i32.const 64)) (i64.const 1)))" : "") + R"(
         (if (i64.lt_s (i64.load (i32.const 64)) (i64.const 0)) (then (unreachable)))
         (set_local $work (i32.load (i32.const 16)))
         (block $end (loop $loop
            (br_if $end (i32.eqz (get_local $work)))
            (set_local $work (i32.sub (get_local $work) (i32.const 1))) (br $loop)))
         (call $update (get_local $it) (i64.const 0) (i32.const 64) (i32.const 8))
         (call $return (i32.const 64) (i32.const 8)))))");
}
void push(tester& chain, name scope, uint64_t delta, uint64_t nonce, uint64_t work = 0,
          std::optional<uint64_t> primary = std::nullopt, name action_name = "run"_n) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = action_name; act.authorization = {{"alice"_n, "active"_n}};
   const std::array<uint64_t, 4> data{scope.to_uint64_t(), delta, work, nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
   if (primary) {
      const auto* bytes = reinterpret_cast<const char*>(&*primary);
      act.data.insert(act.data.end(), bytes, bytes + sizeof(*primary));
   }
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("alice"_n, "active"), chain.control->get_chain_id());
   BOOST_REQUIRE(!chain.push_transaction(trx)->except);
}
std::vector<char> consensus_trace(const transaction_trace_ptr& trace) {
   std::vector<std::vector<char>> fields;
   fields.push_back(fc::raw::pack(trace->id));
   if (trace->receipt) fields.push_back(fc::raw::pack(*trace->receipt));
   fields.push_back(fc::raw::pack(trace->net_usage));
   fields.push_back(fc::raw::pack(trace->except ? trace->except->code() : int64_t{0}));
   for (const auto& action : trace->action_traces) {
      fields.push_back(fc::raw::pack(action.action_ordinal));
      fields.push_back(fc::raw::pack(action.creator_action_ordinal));
      fields.push_back(fc::raw::pack(action.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(action.receiver));
      fields.push_back(fc::raw::pack(action.act));
      fields.push_back(fc::raw::pack(action.context_free));
      if (action.receipt) fields.push_back(fc::raw::pack(*action.receipt));
      fields.push_back(action.return_value);
      fields.push_back(fc::raw::pack(action.account_ram_deltas));
      fields.push_back(fc::raw::pack(action.error_code));
      fields.push_back(fc::raw::pack(action.except ? action.except->code() : int64_t{0}));
      fields.emplace_back(action.console.begin(), action.console.end());
   }
   return fc::raw::pack(fields);
}

std::vector<uint8_t> notification_adoption_code(unsigned role, unsigned extra = 0) {
   const auto constant = [](name value) { return "(i64.const " + std::to_string(value.to_uint64_t()) + ")"; };
   const auto notify = [&](name account) { return "(call $notify " + constant(account) + ")"; };
   const std::string selected_scope = role == 1 ? constant("carol"_n) : role == 2 ? constant("bob"_n) : "(i64.load (i32.const 0))";
   std::string tail;
   if (role == 0) {
      tail = R"((i64.store (i32.const 72) (i64.add (i64.load (i32.const 72)) (i64.load (i32.const 8))))
         (call $update (get_local $it) (i64.const 0) (i32.const 72) (i32.const 8))
         (drop (call $find )" + constant("test"_n) + " " + constant("bob"_n) + " " + constant("rows"_n) + " (i64.const 1)))" +
         notify("alice"_n) + notify("bob"_n) + notify("alice"_n);
   } else if (role == 1) {
      const std::string bad = "(i64.eq (i64.load (i32.const 8)) (i64.const 9))";
      const std::string payer = extra == 2 ? "(select " + constant("bob"_n) + " (i64.const 0) " + bad + ")" : "(i64.const 0)";
      const std::string size = extra == 2 ? "(select (i32.const 16) (i32.const 8) " + bad + ")" : "(i32.const 8)";
      tail = "(set_local $own (call $find (get_local $receiver) (i64.load (i32.const 0)) " + constant("rows"_n) + R"( (i64.const 1)))
         (drop (call $get (get_local $own) (i32.const 80) (i32.const 8)))
         (i64.store (i32.const 80) (i64.add (i64.load (i32.const 80))
            (i64.add (i64.load (i32.const 8)) (i64.const )" + std::to_string(extra == 1 ? 1 : 0) + R"())))
         (call $update (get_local $own) )" + payer + " (i32.const 80) " + size + ")" +
         notify("carol"_n) + notify("test"_n) + notify("bob"_n);
   } else {
      tail = notify(role == 2 ? "alice"_n : "bob"_n);
      if (role == 3 && extra) tail += "(if (i64.eq (i64.load (i32.const 8)) (i64.const 9)) (then unreachable))";
   }
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64) (local $it i32) (local $own i32)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $it (call $find )" + constant("test"_n) + " " + selected_scope + " " + constant("rows"_n) + R"( (i64.const 1)))
         (i32.store (i32.const 64) (get_local $it))
         (drop (call $get (get_local $it) (i32.const 72) (i32.const 8)))
         )" + tail + R"(
         (i64.store (i32.const 88) (get_local $receiver))
         (call $return (i32.const 64) (i32.const 32)))))");
}

signed_transaction notification_transaction(tester& chain, name account, name scope, uint64_t delta, uint64_t nonce,
                                            unsigned root_count = 2) {
   signed_transaction trx;
   for (unsigned i = 0; i < root_count; ++i) {
      action act; act.account = account; act.name = "run"_n; act.authorization = {{"alice"_n, "active"_n}};
      const std::array<uint64_t, 4> data{scope.to_uint64_t(), delta + i, 0, nonce};
      act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
      trx.actions.push_back(std::move(act));
   }
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("alice"_n, "active"), chain.control->get_chain_id());
   return trx;
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_validation_tests)

struct validation_corpus {
   genesis_state genesis = base_tester::default_genesis();
   const std::vector<uint8_t> code = update_code(), replacement = update_code(true);
   const fc::sha256 code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   const fc::sha256 replacement_hash = fc::sha256::hash(reinterpret_cast<const char*>(replacement.data()), replacement.size());
   std::vector<std::vector<char>> corpus;
   fc::temp_directory source_dir;
   controller::config source_config;

   validation_corpus() {
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->version = 2; // release genesis, not legacy governance
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   {
      auto config = base_tester::default_config(source_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      source_config = config;
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { corpus.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n}); seal();
      source.set_code("test"_n, initialize_code()); seal();
      uint64_t nonce = 1;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(source, scope, 10, nonce++);
      seal(); source.set_code("test"_n, code); seal();
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(source, scope, 7, nonce++, 20000);
      seal();
      push(source, "alice"_n, 3, nonce++); push(source, "alice"_n, 5, nonce++);
      push(source, "bob"_n, 9, nonce++); seal();
      // Second private execution traps on the OLD balance (25 - 30), but must
      // succeed canonically after the first has credited 10 (35 - 30 = 5).
      push(source, "alice"_n, 10, nonce++);
      push(source, "alice"_n, uint64_t{0} - 30, nonce++); seal();
      // Entirely hot batch: later results require bounded current-prefix
      // execution; they may not reuse the initial old-balance journals.
      for (unsigned i = 0; i < 6; ++i) push(source, "alice"_n, 1, nonce++);
      seal();
      // A native setcode is a serial barrier; later work must use the new code.
      push(source, "carol"_n, 2, nonce++); source.set_code("test"_n, replacement);
      push(source, "dave"_n, 2, nonce++); seal();
      for (unsigned i = 0; i < 5; ++i) {
         if (i == 1) { // real writes after the snapshot/restart checkpoint
            push(source, "bob"_n, 4, nonce++); push(source, "dave"_n, 3, nonce++);
         }
         seal();
      }
   }
   }

   controller::config configuration(const fc::temp_directory& directory, uint16_t workers) const {
      auto result = base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(result);
      result.parallel_validation_threads = workers;
      result.parallel_code_allowlist = {code_hash, replacement_hash};
      return result;
   }
   void apply(tester& chain, size_t index) const {
      chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus.at(index))));
      chain.control->abort_block();
   }
   void prefix(tester& chain, size_t count) const {
      for (size_t i = 0; i < count; ++i) apply(chain, i);
   }
};

BOOST_FIXTURE_TEST_CASE(native_savanna_parallel_adoption_matches_original_at_every_block_prefix, validation_corpus) {
   // Explicit opt-in export for a DIFFERENT, pre-change nodeos binary. Never
   // overwrite a caller's directory or export signing credentials / databases.
   std::optional<std::filesystem::path> export_directory;
   fc::variants checkpoints;
   if (const char* destination = std::getenv("PUREEOS_PARALLEL_EXPORT_DIR")) {
      export_directory = std::filesystem::path(destination);
      BOOST_REQUIRE(std::filesystem::create_directory(*export_directory));
      BOOST_REQUIRE(fc::json::save_to_file(genesis, *export_directory / "genesis.json"));
      std::filesystem::create_directory(*export_directory / "blocks");
      for (const char* file : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(source_config.blocks_dir / file, *export_directory / "blocks" / file);
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto reference_config = base_tester::default_config(reference_dir).first;
      eosio::testing::configure_parallel_test_runtime(reference_config);
      tester reference(reference_config, genesis);
      auto observed_config = base_tester::default_config(observed_dir).first;
      eosio::testing::configure_parallel_test_runtime(observed_config);
      observed_config.parallel_validation_threads = workers;
      observed_config.parallel_code_allowlist = {code_hash, replacement_hash};
      tester observed(observed_config, genesis);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto expected_connection = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto actual_connection = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      for (const auto& block : corpus) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.control->head().id() == observed.control->head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
         if (export_directory && workers == 0) {
            const auto number = reference.control->head().block_num();
            const auto file = "block-" + std::to_string(number) + ".json";
            BOOST_REQUIRE(fc::json::save_to_file(fc::raw::unpack<signed_block>(block), *export_directory / file));
            checkpoints.emplace_back(fc::mutable_variant_object()
               ("block_num", number)("block_id", reference.control->head().id())
               ("lib", reference.last_irreversible_block_num())
               ("integrity_hash", reference.control->calculate_integrity_hash())("file", file));
         }
      }
      const auto stats = observed.control->get_parallel_execution_stats();
      if (workers) {
         BOOST_CHECK_GE(stats.batches, 4u);
         BOOST_CHECK_GE(stats.adopted, 7u);
         // The failed private result remains a barrier; valid conflicts now
         // rerun against the committed prefix instead of forcing the tail.
         BOOST_CHECK_EQUAL(stats.serial_fallbacks, 1u);
         BOOST_CHECK_GT(stats.prefix_refresh_requests, 0u);
         BOOST_CHECK_EQUAL(stats.prefix_refresh_requests, stats.prefix_refresh_successes);
         BOOST_CHECK_GE(stats.succeeded, stats.adopted);
         BOOST_CHECK_GT(stats.attempts, stats.succeeded); // failed speculation is not final rejection
      } else BOOST_CHECK_EQUAL(stats.attempts, 0u);
      BOOST_TEST_MESSAGE("validation threads=" << workers << " attempts=" << stats.attempts
         << " adopted=" << stats.adopted << " serial=" << stats.serial_fallbacks);
      for (const auto& [scope, expected] : std::array<std::pair<name, uint64_t>, 4>{
         std::pair{"alice"_n, 11}, {"bob"_n, 31}, {"carol"_n, 19}, {"dave"_n, 24}}) {
         const auto data = observed.get_row_by_account("test"_n, scope, "rows"_n, name{1});
         BOOST_REQUIRE_EQUAL(data.size(), 8u);
         uint64_t value; std::memcpy(&value, data.data(), sizeof(value)); BOOST_CHECK_EQUAL(value, expected);
      }
   }
   if (export_directory) {
      // Also archive a reversible competing branch, with hashes calculated by
      // original serial execution. A process test can now test REAL fork undo
      // against the old executable rather than generating a different fork.
      fc::temp_directory first_dir, second_dir, reference_dir;
      tester first(configuration(first_dir, 0), genesis), second(configuration(second_dir, 0), genesis);
      tester reference(configuration(reference_dir, 0), genesis);
      prefix(first, 9); prefix(second, 9); prefix(reference, 9);
      fc::variants forks;
      auto export_fork = [&](const signed_block_ptr& block) {
         reference.push_block(block); reference.control->abort_block();
         BOOST_REQUIRE(reference.head().id() == block->calculate_id());
         const auto file = "fork-" + std::to_string(forks.size()) + ".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block, *export_directory / file));
         forks.emplace_back(fc::mutable_variant_object()
            ("block_num", reference.head().block_num())("block_id", reference.head().id())
            ("lib", reference.last_irreversible_block_num())
            ("integrity_hash", reference.control->calculate_integrity_hash())("file", file));
      };
      push(first, "alice"_n, 100, 10001); push(first, "carol"_n, 100, 10002);
      export_fork(first.produce_block());
      push(second, "alice"_n, 200, 20001); push(second, "bob"_n, 2, 20002);
      export_fork(second.produce_block(fc::milliseconds(1000)));
      push(second, "carol"_n, 3, 20003); export_fork(second.produce_block());
      const fc::variant manifest = fc::mutable_variant_object()
         ("format", 1)("chain_id", genesis.compute_chain_id())
         ("code_hashes", std::vector<fc::sha256>{code_hash, replacement_hash})
         ("checkpoints", checkpoints)("fork_prefix_blocks", 9)("forks", forks);
      BOOST_REQUIRE(fc::json::save_to_file(manifest, *export_directory / "manifest.json"));
      BOOST_TEST_MESSAGE("Exported immutable business corpus to " << export_directory->string());
   }
}

BOOST_FIXTURE_TEST_CASE(coded_notification_validation_matches_full_original_receipts_and_child_writes, validation_corpus) {
   const std::array codes{notification_adoption_code(0), notification_adoption_code(1),
                         notification_adoption_code(2), notification_adoption_code(3)};
   const auto unreviewed = notification_adoption_code(1, 1);
   flat_set<digest_type> reviewed;
   for (const auto& bytes : codes) reviewed.insert(fc::sha256::hash(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
   struct point { std::vector<char> block; uint64_t adopted, fallbacks; };
   std::vector<point> blocks;
   std::optional<std::filesystem::path> export_directory;
   fc::variants checkpoints;
   if (const char* destination = std::getenv("PUREEOS_PARALLEL_NOTIFICATION_EXPORT_DIR")) {
      export_directory = std::filesystem::path(destination);
      BOOST_REQUIRE(std::filesystem::create_directory(*export_directory));
      BOOST_REQUIRE(fc::json::save_to_file(genesis, *export_directory / "genesis.json"));
   }
   auto export_point = [&](tester& chain, const signed_block& block) {
      if (!export_directory) return;
      const auto file = "block-" + std::to_string(block.block_num()) + ".json";
      BOOST_REQUIRE(fc::json::save_to_file(block, *export_directory / file));
      checkpoints.emplace_back(fc::mutable_variant_object()
         ("block_num", block.block_num())("block_id", chain.head().id())("lib", chain.last_irreversible_block_num())
         ("integrity_hash", chain.control->calculate_integrity_hash())("file", file));
   };
   fc::temp_directory source_directory;
   const auto source_config = configuration(source_directory, 0);
   {
      tester source(source_config, genesis);
      for (size_t i = 0; i < 4; ++i) {
         apply(source, i);
         export_point(source, fc::raw::unpack<signed_block>(corpus[i]));
      }
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&](uint64_t adopted, uint64_t fallbacks) {
         const auto block = source.produce_block(); source.control->abort_block();
         blocks.push_back({fc::raw::pack(*block), adopted, fallbacks});
         export_point(source, *block);
      };
      source.set_code("alice"_n, initialize_code()); seal(0, 0);
      uint64_t nonce = 270000;
      for (const auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) {
         auto trx = notification_transaction(source, "alice"_n, scope, 100, nonce++, 1);
         BOOST_REQUIRE(!source.push_transaction(trx)->except);
      }
      seal(0, 0);
      const std::array receivers{"test"_n, "alice"_n, "bob"_n, "carol"_n};
      for (size_t i = 0; i < receivers.size(); ++i) source.set_code(receivers[i], codes[i]);
      seal(0, 0);
      auto run = [&](name scope) {
         auto trx = notification_transaction(source, "test"_n, scope, 2, nonce++);
         BOOST_REQUIRE(!source.push_transaction(trx)->except);
      };
      run("alice"_n); run("dave"_n); seal(2, 0);
      run("alice"_n); run("alice"_n); run("dave"_n); seal(3, 0); // refresh conflict; keep independent tail
      source.set_code("alice"_n, unreviewed); seal(0, 0);
      run("alice"_n); run("dave"_n); seal(0, 2); // child code is outside the local allowlist
      source.set_code("alice"_n, codes[1]); seal(0, 0);
      run("alice"_n); run("dave"_n); seal(2, 0);
      for (unsigned i = 0; i < 5; ++i) seal(0, 0);
      // Both reversible tail blocks execute real coded notifications, so a
      // snapshot restore must exercise the new path, not just empty blocks.
      run("alice"_n); run("dave"_n); seal(2, 0);
      run("alice"_n); run("dave"_n); seal(2, 0);
   }
   if (export_directory) {
      std::filesystem::create_directory(*export_directory / "blocks");
      for (const char* file : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(source_config.blocks_dir / file, *export_directory / "blocks" / file);
      fc::temp_directory first_dir, second_dir, reference_dir;
      tester first(configuration(first_dir, 0), genesis), second(configuration(second_dir, 0), genesis);
      tester reference(configuration(reference_dir, 0), genesis);
      for (auto* chain : {&first, &second, &reference}) {
         prefix(*chain, 4);
         for (size_t i = 0; i < 3; ++i) {
            chain->push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[i].block)));
            chain->control->abort_block();
         }
      }
      fc::variants forks;
      auto export_fork = [&](const signed_block_ptr& block) {
         reference.push_block(block); reference.control->abort_block();
         BOOST_REQUIRE(reference.head().id() == block->calculate_id());
         const auto file = "fork-" + std::to_string(forks.size()) + ".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block, *export_directory / file));
         forks.emplace_back(fc::mutable_variant_object()
            ("block_num", block->block_num())("block_id", reference.head().id())("lib", reference.last_irreversible_block_num())
            ("integrity_hash", reference.control->calculate_integrity_hash())("file", file));
      };
      uint64_t nonce = 275000;
      auto run = [&](tester& chain, name scope, uint64_t delta) {
         auto trx = notification_transaction(chain, "test"_n, scope, delta, nonce++);
         BOOST_REQUIRE(!chain.push_transaction(trx)->except);
      };
      run(first, "alice"_n, 100); run(first, "dave"_n, 100); export_fork(first.produce_block());
      run(second, "alice"_n, 200); run(second, "dave"_n, 2); export_fork(second.produce_block(fc::milliseconds(1000)));
      run(second, "alice"_n, 3); run(second, "dave"_n, 4); export_fork(second.produce_block());
      BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
         ("fixture_kind", "coded-notification-recovery")("chain_id", genesis.compute_chain_id())
         ("code_hashes", std::vector<digest_type>(reviewed.begin(), reviewed.end()))
         ("checkpoints", checkpoints)("fork_prefix_blocks", 7)("forks", forks), *export_directory / "manifest.json"));
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory serial_dir, parallel_dir;
      auto config = configuration(parallel_dir, workers); config.parallel_code_allowlist = reviewed;
      tester serial(configuration(serial_dir, 0), genesis), parallel(config, genesis);
      prefix(serial, 4); prefix(parallel, 4);
      BOOST_REQUIRE(serial.control->is_builtin_activated(builtin_protocol_feature_t::action_return_value));
      std::vector<std::vector<char>> expected, actual;
      boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& event) {
         expected.push_back(consensus_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& event) {
         actual.push_back(consensus_trace(std::get<0>(event)));
      });
      for (size_t index = 0; index < blocks.size(); ++index) {
         BOOST_TEST_CONTEXT("notification validation workers=" << workers << " block=" << index) {
            const auto& point = blocks[index]; expected.clear(); actual.clear();
            const auto before = parallel.control->get_parallel_execution_stats();
            serial.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(point.block)));
            parallel.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(point.block)));
            serial.control->abort_block(); parallel.control->abort_block();
            BOOST_CHECK(expected == actual);
            BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
            BOOST_CHECK(serial.head().id() == parallel.head().id());
            BOOST_CHECK_EQUAL(serial.last_irreversible_block_num(), parallel.last_irreversible_block_num());
            const auto after = parallel.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? point.adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? point.fallbacks : 0u);
            BOOST_CHECK_EQUAL(serial.control->get_resource_limits_manager().get_account_ram_usage("alice"_n),
               parallel.control->get_resource_limits_manager().get_account_ram_usage("alice"_n));
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(invalid_receipts_authorization_duplicate_and_wasm_failure_undo_adopted_prefix, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) for (unsigned mutation = 0; mutation < 7; ++mutation) {
      BOOST_TEST_CONTEXT("workers=" << workers << " mutation=" << mutation) {
         fc::temp_directory reference_dir, observed_dir;
         tester reference(configuration(reference_dir, 0), genesis);
         tester observed(configuration(observed_dir, workers), genesis);
         prefix(reference, 4); prefix(observed, 4);
         const auto parent = observed.control->head().id();
         const auto before = observed.control->calculate_integrity_hash();
         auto bad = std::make_unique<signed_block>(fc::raw::unpack<signed_block>(corpus[4]));
         BOOST_REQUIRE_EQUAL(bad->transactions.size(), 4u);
         auto& last = bad->transactions.back();
         switch (mutation) {
            case 0: last.status = transaction_receipt_header::hard_fail; break;
            case 1: ++last.net_usage_words.value; break;
            case 2: last.cpu_usage_us = 0; break;
            case 3: {
               auto trx = std::get<packed_transaction>(last.trx).get_signed_transaction();
               trx.signatures.clear();
               last.trx = packed_transaction(trx, packed_transaction::compression_type::none);
               break;
            }
            case 4: {
               auto trx = std::get<packed_transaction>(last.trx).get_signed_transaction();
               const uint64_t negative = uint64_t{0} - 100;
               std::memcpy(trx.actions[0].data.data() + 8, &negative, sizeof(negative));
               trx.signatures.clear();
               trx.sign(reference.get_private_key("alice"_n, "active"), genesis.compute_chain_id());
               last.trx = packed_transaction(trx, packed_transaction::compression_type::none);
               break;
            }
            case 5: bad->transactions[1] = transaction_receipt(bad->transactions.front()); break;
            case 6: {
               // Both private withdrawals see balance 10 and independently
               // succeed (10-7). In canonical order the SECOND must fail; do
               // not adopt its stale result and spend the same balance twice.
               for (unsigned i = 0; i < 2; ++i) {
                  auto trx = std::get<packed_transaction>(bad->transactions[i].trx).get_signed_transaction();
                  const uint64_t scope = "alice"_n.to_uint64_t(), negative = uint64_t{0} - 7;
                  std::memcpy(trx.actions[0].data.data(), &scope, sizeof(scope));
                  std::memcpy(trx.actions[0].data.data() + 8, &negative, sizeof(negative));
                  trx.signatures.clear();
                  trx.sign(reference.get_private_key("alice"_n, "active"), genesis.compute_chain_id());
                  bad->transactions[i].trx = packed_transaction(trx, packed_transaction::compression_type::none);
               }
               break;
            }
         }
         std::vector<digest_type> digests;
         for (const auto& receipt : bad->transactions) digests.push_back(receipt.digest());
         bad->transaction_mroot = calculate_merkle(std::move(digests));
         // Savanna signs block ID. Re-sign the altered body commitment, so
         // rejection must happen during execution, not a trivial bad signature.
         bad->producer_signature = reference.get_private_key("eosio"_n, "active").sign(bad->calculate_id());
         const auto invalid = signed_block::create_signed_block(std::move(bad));
         auto reject = [&](tester& chain) {
            int64_t error = 0;
            try { chain.push_block(invalid); }
            catch (const fc::exception& e) { error = e.code(); }
            chain.control->abort_block();
            return error;
         };
         const auto expected = reject(reference), actual = reject(observed);
         BOOST_CHECK_NE(expected, 0);
         BOOST_CHECK_EQUAL(actual, expected);
         BOOST_CHECK(observed.control->head().id() == parent);
         BOOST_CHECK(observed.control->calculate_integrity_hash() == before);
         BOOST_CHECK(reference.control->calculate_integrity_hash() == before);
         BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, 0u);
         if (mutation == 6) BOOST_CHECK_EQUAL(observed.control->get_parallel_execution_stats().succeeded, 4u);
         // A rejected batch must not leave IDs, writes, receipts or tickets
         // behind. The original valid block still works at this exact prefix.
         apply(reference, 4); apply(observed, 4);
         BOOST_CHECK(reference.control->head().id() == observed.control->head().id());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
      }
   }
}

BOOST_FIXTURE_TEST_CASE(snapshot_restart_and_checked_replay_keep_parallel_state_reconstructible, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory directory, restore_dir, replay_dir;
      auto config = configuration(directory, workers);
      tester chain(config, genesis);
      prefix(chain, 9);
      BOOST_CHECK_GT(chain.control->get_parallel_execution_stats().adopted, 0u);
      const auto snapshot_id = chain.control->head().id();
      const auto snapshot_hash = chain.control->calculate_integrity_hash();
      std::ostringstream bytes;
      auto writer = std::make_shared<ostream_snapshot_writer>(bytes);
      chain.control->write_snapshot(writer); writer->finalize();
      std::istringstream input(bytes.str());
      auto reader = std::make_shared<istream_snapshot_reader>(input);
      auto restore_config = configuration(restore_dir, workers);
      tester restored(restore_config, genesis, call_startup_t::no);
      restored.close(); restored.init(restore_config, reader);
      restored.control->abort_block();
      BOOST_CHECK(restored.control->head().id() == snapshot_id);
      BOOST_CHECK(restored.control->calculate_integrity_hash() == snapshot_hash);
      chain.close(); chain.open(); chain.control->abort_block();
      BOOST_CHECK(chain.control->head().id() == snapshot_id);
      BOOST_CHECK(chain.control->calculate_integrity_hash() == snapshot_hash);
      for (size_t i = 9; i < corpus.size(); ++i) {
         apply(chain, i); apply(restored, i);
         BOOST_CHECK(chain.control->head().id() == restored.control->head().id());
         BOOST_CHECK(chain.control->calculate_integrity_hash() == restored.control->calculate_integrity_hash());
      }
      BOOST_CHECK_GT(chain.control->get_parallel_execution_stats().adopted, 0u);
      BOOST_CHECK_GT(restored.control->get_parallel_execution_stats().adopted, 0u);
      const auto lib = chain.last_irreversible_block_num();
      const auto lib_id = chain.last_irreversible_block_id();
      chain.close();
      auto replay_config = configuration(replay_dir, workers);
      replay_config.disable_replay_opts = true;
      replay_config.force_all_checks = true;
      std::filesystem::create_directories(replay_config.blocks_dir);
      for (const char* filename : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(config.blocks_dir / filename, replay_config.blocks_dir / filename);
      tester replay(replay_config, genesis);
      replay.control->abort_block();
      BOOST_CHECK_EQUAL(replay.head().block_num(), lib);
      BOOST_CHECK(replay.head().id() == lib_id);
      BOOST_CHECK_GT(replay.control->get_parallel_execution_stats().adopted, 0u);
      fc::temp_directory reference_dir;
      tester reference(configuration(reference_dir, 0), genesis);
      prefix(reference, lib - 1); // native genesis is block 1
      BOOST_CHECK(reference.control->calculate_integrity_hash() == replay.control->calculate_integrity_hash());
   }
}

BOOST_FIXTURE_TEST_CASE(fork_switch_discards_adopted_old_branch_and_reexecutes_new_prefix, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory first_dir, second_dir, reference_dir, observed_dir;
      tester first(configuration(first_dir, 0), genesis), second(configuration(second_dir, 0), genesis);
      tester reference(configuration(reference_dir, 0), genesis), observed(configuration(observed_dir, workers), genesis);
      // No local finalizers on the branch builders: keep the split reversible.
      prefix(first, 9); prefix(second, 9); prefix(reference, 9); prefix(observed, 9);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto c1 = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto c2 = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto receive = [&](const signed_block_ptr& block) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(block); observed.push_block(block);
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
      };
      const auto before = observed.control->get_parallel_execution_stats().adopted;
      push(first, "alice"_n, 100, 10001); push(first, "carol"_n, 100, 10002);
      const auto old_branch = first.produce_block(); receive(old_branch);
      BOOST_CHECK(observed.head().id() == old_branch->calculate_id());
      BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, before);
      push(second, "alice"_n, 200, 20001); push(second, "bob"_n, 2, 20002);
      // With identical QC progress, a later slot selects the competing branch.
      const auto new_branch = second.produce_block(fc::milliseconds(1000)); receive(new_branch);
      push(second, "carol"_n, 3, 20003); receive(second.produce_block());
      BOOST_CHECK(observed.head().id() == second.head().id());
      BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, before + 2);
      const auto old_id = std::get<packed_transaction>(old_branch->transactions.front().trx).id();
      BOOST_CHECK(!observed.control->is_known_unexpired_transaction(old_id));
      for (const auto& [scope, expected] : std::array<std::pair<name, uint64_t>, 4>{
         std::pair{"alice"_n, 212}, {"bob"_n, 29}, {"carol"_n, 23}, {"dave"_n, 20}}) {
         const auto data = observed.get_row_by_account("test"_n, scope, "rows"_n, name{1});
         BOOST_REQUIRE_EQUAL(data.size(), 8u);
         uint64_t value; std::memcpy(&value, data.data(), 8); BOOST_CHECK_EQUAL(value, expected);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(local_allowlist_light_validation_and_batch_bounds_never_change_state, validation_corpus) {
   fc::temp_directory reference_dir;
   tester reference(configuration(reference_dir, 0), genesis);
   prefix(reference, corpus.size());
   const auto expected = reference.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 5; ++mode) {
      fc::temp_directory directory;
      auto config = configuration(directory, 4);
      if (mode == 0) config.parallel_code_allowlist.clear();
      if (mode == 1) config.parallel_code_allowlist = {fc::sha256::hash("not deployed")};
      if (mode == 2) config.block_validation_mode = validation_mode::LIGHT;
      if (mode == 3) config.parallel_validation_batch_size = 1;
      if (mode == 4) config.parallel_validation_batch_size = 64;
      tester chain(config, genesis);
      prefix(chain, corpus.size());
      BOOST_CHECK(chain.control->calculate_integrity_hash() == expected);
      const auto stats = chain.control->get_parallel_execution_stats();
      if (mode < 3) BOOST_CHECK_EQUAL(stats.attempts, 0u);
      else BOOST_CHECK_GT(stats.adopted, 0u);
   }
}

BOOST_FIXTURE_TEST_CASE(json_blocks_finalized_before_publication_remain_restartable, validation_corpus) {
   fc::temp_directory reference_dir, observed_dir;
   tester reference(configuration(reference_dir, 0), genesis);
   tester observed(configuration(observed_dir, 2), genesis);
   for (const auto& bytes : corpus) {
      const auto original = fc::raw::unpack<signed_block>(bytes);
      // Same representation arriving at chain_api push_block. Reflection does
      // not initialize the private serialized cache; the factory MUST do it.
      auto json_block = fc::json::from_string(fc::json::to_string(original, fc::time_point::maximum())).as<signed_block>();
      auto published = signed_block::create_signed_block(std::make_unique<signed_block>(std::move(json_block)));
      BOOST_CHECK(published->packed_signed_block() == bytes);
      reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
      observed.push_block(published);
      reference.control->abort_block(); observed.control->abort_block();
      BOOST_CHECK(reference.head().id() == observed.head().id());
      BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
   }
   const auto id = observed.head().id(), hash = observed.control->calculate_integrity_hash();
   BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, 0u);
   observed.close(); observed.open(); observed.control->abort_block();
   BOOST_CHECK(observed.head().id() == id);
   BOOST_CHECK(observed.control->calculate_integrity_hash() == hash);
}

BOOST_FIXTURE_TEST_CASE(parallel_diagnostics_do_not_change_consensus_and_count_real_conflicts, validation_corpus) {
   const auto saved = performance::metrics().enabled.exchange(false);
   auto restore = fc::make_scoped_exit([&] { performance::metrics().enabled = saved; });
   fc::temp_directory off_dir, on_dir;
   tester off(configuration(off_dir, 4), genesis), on(configuration(on_dir, 4), genesis);
   const auto disabled_before = performance::metrics().snapshot(performance::stage::parallel_capture).count;
   prefix(off, corpus.size());
   BOOST_CHECK_EQUAL(performance::metrics().snapshot(performance::stage::parallel_capture).count, disabled_before);
   const auto conflicts_before = performance::metrics().snapshot(performance::stage::parallel_conflict).count;
   performance::metrics().enabled = true;
   prefix(on, corpus.size());
   BOOST_CHECK(off.control->calculate_integrity_hash() == on.control->calculate_integrity_hash());
   BOOST_CHECK_EQUAL(off.control->get_parallel_execution_stats().adopted, on.control->get_parallel_execution_stats().adopted);
   for (auto stage : {performance::stage::parallel_capture, performance::stage::parallel_batch_wait,
          performance::stage::parallel_compile, performance::stage::parallel_execute, performance::stage::parallel_adopt,
          performance::stage::parallel_fallback})
      BOOST_CHECK_GT(performance::metrics().snapshot(stage).count, 0u);
   BOOST_CHECK_GT(performance::metrics().snapshot(performance::stage::parallel_conflict).count, conflicts_before);
}

BOOST_AUTO_TEST_CASE(same_table_disjoint_rows_adopt_and_shared_balances_fall_back_in_real_savanna_blocks) {
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   const auto code = update_code(false, true);
   const auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   std::vector<std::vector<char>> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(config);
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n, "alice"_n}); seal();
      source.set_code("test"_n, initialize_code(true)); seal();
      uint64_t nonce = 1;
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 10, nonce++, 0, key);
      seal(); source.set_code("test"_n, code); seal();
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 7, nonce++, 20000, key);
      seal();
      push(source, "alice"_n, 3, nonce++, 0, 1);
      push(source, "alice"_n, 5, nonce++, 0, 1);
      push(source, "alice"_n, 9, nonce++, 0, 3); seal();
      // Old private balance 25 cannot pay 30. Canonical preceding credit makes
      // it possible; a worker failure MUST NOT be a final rejection.
      push(source, "alice"_n, 10, nonce++, 0, 1);
      push(source, "alice"_n, uint64_t{0} - 30, nonce++, 0, 1); seal();
      for (unsigned i = 0; i < 5; ++i) seal();
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto reference_config = base_tester::default_config(reference_dir).first;
      eosio::testing::configure_parallel_test_runtime(reference_config);
      auto observed_config = base_tester::default_config(observed_dir).first;
      eosio::testing::configure_parallel_test_runtime(observed_config);
      observed_config.parallel_validation_threads = workers;
      observed_config.parallel_code_allowlist = {code_hash};
      tester reference(reference_config, genesis), observed(observed_config, genesis);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto expected_connection = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto actual_connection = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      for (size_t index = 0; index < blocks.size(); ++index) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index])));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index])));
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
         if (index == 4 && workers) {
            const auto stats = observed.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(stats.adopted, 3u);
            BOOST_CHECK_EQUAL(stats.serial_fallbacks, 0u);
         }
      }
      const auto stats = observed.control->get_parallel_execution_stats();
      // Three cold discovery/execution pairs (6), three warm roots plus one
      // current-prefix refresh (4), then two warm credit/debit attempts (2).
      // The failed speculative debit still takes the original serial barrier.
      BOOST_CHECK_EQUAL(stats.attempts, workers ? 12u : 0u);
      BOOST_CHECK_EQUAL(stats.adopted, workers ? 7u : 0u);
      BOOST_CHECK_EQUAL(stats.serial_fallbacks, workers ? 1u : 0u); // failed private debit is still a barrier
      BOOST_CHECK_EQUAL(stats.prefix_refresh_requests, workers ? 1u : 0u);
      BOOST_CHECK_EQUAL(stats.prefix_refresh_successes, workers ? 1u : 0u);
      for (const auto& [key, expected] : std::array<std::pair<uint64_t, uint64_t>, 3>{{{1, 5}, {2, 17}, {3, 26}}}) {
         const auto value = observed.get_row_by_account("test"_n, "alice"_n, "rows"_n, name{key});
         BOOST_REQUIRE_EQUAL(value.size(), 8u);
         uint64_t actual; std::memcpy(&actual, value.data(), sizeof(actual));
         BOOST_CHECK_EQUAL(actual, expected);
      }
   }
}

BOOST_AUTO_TEST_CASE(sparse_discovery_ignores_unrelated_scopes_and_exhaustion_preserves_serial_acceptance) {
   // Keep the original exhaustion/irrelevant-scope scenario at the real global
   // cap. The separate production/validation boundary case retains nine-query
   // coverage and requires its adoption rather than this historical fallback.
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   const auto code = update_code(false, true);
   const auto probe = compile(R"((module
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (func (export "apply") (param $receiver i64) (param i64 i64) (local $key i64)
         (set_local $key (i64.const 1))
         (loop $next
            (drop (call $find (get_local $receiver) (i64.const )" + std::to_string("alice"_n.to_uint64_t()) +
            R"() (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (get_local $key)))
            (set_local $key (i64.add (get_local $key) (i64.const 1)))
            (br_if $next (i64.le_u (get_local $key) (i64.const 64)))))))");
   const auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   const auto probe_hash = fc::sha256::hash(reinterpret_cast<const char*>(probe.data()), probe.size());
   std::vector<std::vector<char>> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(config);
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n, "alice"_n}); seal();
      source.set_code("test"_n, initialize_code(true)); seal();
      uint64_t nonce = 1;
      for (uint64_t scope = 200; scope < 280; ++scope) {
         push(source, name{scope}, 10, nonce++, 0, 1);
         if (scope % 10 == 9) seal();
      }
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 10, nonce++, 0, key);
      seal(); source.set_code("test"_n, code); seal();
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 7, nonce++, 0, key);
      seal(); source.set_code("test"_n, probe); seal();
      push(source, "alice"_n, 0, nonce++); seal();
      for (unsigned i = 0; i < 5; ++i) seal();
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto reference_config = base_tester::default_config(reference_dir).first;
      eosio::testing::configure_parallel_test_runtime(reference_config);
      auto config = base_tester::default_config(observed_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      config.parallel_validation_threads = workers;
      config.parallel_code_allowlist = {code_hash, probe_hash};
      tester reference(reference_config, genesis), observed(config, genesis);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto first = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto second = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      for (const auto& block : blocks) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
      }
      const auto stats = observed.control->get_parallel_execution_stats();
      BOOST_CHECK_EQUAL(stats.adopted, workers ? 3u : 0u);
      BOOST_CHECK_EQUAL(stats.succeeded, workers ? 3u : 0u);
      BOOST_CHECK_EQUAL(stats.attempts, workers ? 70u : 0u); // 3*(discovery+execution) + 64 bounded probe passes
   }
}

BOOST_FIXTURE_TEST_CASE(primary_creation_allocates_original_ids_and_ram_in_canonical_order, validation_corpus) {
   const auto initializer = initialize_code();
   const auto initializer_hash = fc::sha256::hash(reinterpret_cast<const char*>(initializer.data()), initializer.size());
   fc::temp_directory reference_dir;
   tester reference(configuration(reference_dir, 0), genesis);
   prefix(reference, corpus.size());
   const auto expected = reference.control->calculate_integrity_hash();
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory directory;
      auto config = configuration(directory, workers);
      // Four independent stores now use private temporary identities. Original
      // hosts allocate table/row IDs and RAM in the same canonical order.
      config.parallel_code_allowlist.insert(initializer_hash);
      tester chain(config, genesis);
      prefix(chain, corpus.size());
      BOOST_CHECK(chain.control->calculate_integrity_hash() == expected);
      const auto stats = chain.control->get_parallel_execution_stats();
      // This fixed corpus reuses code/action/actor shapes across blocks.
      // Path hints save 19 discovery attempts from the former 58; the six
      // actual prefix conflicts, original IDs/RAM and exact adoption stay checked.
      BOOST_CHECK_EQUAL(stats.attempts, 39u);
      BOOST_CHECK_EQUAL(stats.succeeded, 28u);
      BOOST_CHECK_EQUAL(stats.adopted, 22u);
      BOOST_CHECK_EQUAL(stats.serial_fallbacks, 1u); // only the old private assertion remains unavailable
      BOOST_CHECK_EQUAL(stats.prefix_refresh_requests, 6u);
      BOOST_CHECK_EQUAL(stats.prefix_refresh_successes, 6u);
   }
}

BOOST_AUTO_TEST_CASE(real_cdt_dispatcher_multi_index_and_allocator_adopt_or_fall_back_without_semantic_changes) {
   const auto code = cdt_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   struct checkpoint { std::vector<char> block; uint64_t adopted; uint64_t fallbacks; };
   std::vector<checkpoint> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(config);
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&](uint64_t adopted = 0, uint64_t fallbacks = 0) {
         blocks.push_back({fc::raw::pack(*source.produce_block()), adopted, fallbacks});
      };
      source.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n}); seal();
      source.set_code("test"_n, code); seal();
      uint64_t nonce = 100;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n})
         push(source, scope, 10, nonce++, 0, std::nullopt, "init"_n);
      seal(3, 0); // private stores; owner still allocates original IDs and RAM
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) push(source, scope, 7, nonce++, 4096);
      seal(3, 0); // actual CDT multi_index + allocation, not hand-written WAT
      push(source, "alice"_n, 3, nonce++, 64); push(source, "alice"_n, 5, nonce++, 64); seal(2, 0);
      push(source, "alice"_n, 10, nonce++, 64);
      push(source, "alice"_n, uint64_t{0} - 30, nonce++, 64); seal(1, 1); // private assertion fails, serial succeeds
      push(source, "alice"_n, 2, nonce++, 2 * 1024 * 1024);
      push(source, "bob"_n, 3, nonce++, 64); push(source, "carol"_n, 4, nonce++, 64);
      seal(0, 3); // local memory exhaustion AFTER private write; discard whole batch tail
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) push(source, scope, 1, nonce++, 4096);
      seal(3, 0); // cached machines reset even after allocation failure
      for (unsigned i = 0; i < 5; ++i) seal();
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      BOOST_TEST_CONTEXT("CDT workers=" << workers) {
         fc::temp_directory reference_dir, observed_dir;
         auto reference_config = base_tester::default_config(reference_dir).first;
         eosio::testing::configure_parallel_test_runtime(reference_config);
         auto config = base_tester::default_config(observed_dir).first;
         eosio::testing::configure_parallel_test_runtime(config);
         config.parallel_validation_threads = workers;
         config.parallel_code_allowlist = {hash};
         tester reference(reference_config, genesis), observed(config, genesis);
         std::vector<std::vector<char>> expected_traces, actual_traces;
         auto first = reference.control->applied_transaction().connect([&](const auto& event) {
            expected_traces.push_back(consensus_trace(std::get<0>(event)));
         });
         auto second = observed.control->applied_transaction().connect([&](const auto& event) {
            actual_traces.push_back(consensus_trace(std::get<0>(event)));
         });
         for (const auto& item : blocks) {
            expected_traces.clear(); actual_traces.clear();
            const auto before = observed.control->get_parallel_execution_stats();
            reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(item.block)));
            observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(item.block)));
            reference.control->abort_block(); observed.control->abort_block();
            BOOST_CHECK(reference.head().id() == observed.head().id());
            BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
            BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
            BOOST_CHECK(expected_traces == actual_traces);
            const auto after = observed.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? item.adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? item.fallbacks : 0u);
         }
         for (const auto& [scope, expected] : std::array<std::pair<name, int64_t>, 3>{{
               {"alice"_n, 8}, {"bob"_n, 21}, {"carol"_n, 22}}}) {
            const auto value = observed.get_row_by_account("test"_n, scope, "rows"_n, name{1});
            BOOST_REQUIRE_EQUAL(value.size(), 16u);
            int64_t actual; std::memcpy(&actual, value.data() + 8, sizeof(actual));
            BOOST_CHECK_EQUAL(actual, expected);
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(existing_row_resize_and_payer_journals_match_serial_ram_and_notifications, validation_corpus) {
   const auto resize = resize_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(resize.data()), resize.size());
   struct checkpoint { std::vector<char> block; unsigned adopted, fallbacks; };
   std::vector<checkpoint> blocks;
   {
      fc::temp_directory directory;
      tester source(configuration(directory, 0), genesis); prefix(source, 4);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&](unsigned adopted, unsigned fallback) {
         blocks.push_back({fc::raw::pack(*source.produce_block()), adopted, fallback});
      };
      source.set_code("test"_n, resize); seal(0, 0);
      uint64_t nonce = 70000;
      auto update = [&](name scope, const resize_steps& steps, bool bob = false) {
         auto trx = resize_transaction(source, scope, steps, nonce++, bob);
         const auto trace = source.push_transaction(trx);
         BOOST_REQUIRE(!trace->except);
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
         BOOST_REQUIRE_EQUAL(trace->action_traces.front().return_value.size(), 16u);
      };
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n})
         update(scope, {{{16, {}}, {3, {}}, {8, {}}}});
      seal(3, 0);
      for (auto scope : {"alice"_n, "carol"_n})
         update(scope, {{{0, {}}, {32, "bob"_n}, {16, {}}}}, true);
      seal(2, 0);
      for (auto scope : {"alice"_n, "bob"_n})
         update(scope, {{{1, "alice"_n}, {8, {}}, {0, {}}}});
      seal(2, 0);
      update("alice"_n, {{{1, {}}, {2, {}}, {3, {}}}});
      update("alice"_n, {{{1, {}}, {2, {}}, {8, {}}}});
      seal(2, 0); // refresh uses the latest row size/payer; original RAM replay still applies
      for (unsigned i = 0; i < 5; ++i) seal(0, 0);
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto config = configuration(observed_dir, workers); config.parallel_code_allowlist = {hash};
      tester reference(configuration(reference_dir, 0), genesis), observed(config, genesis);
      prefix(reference, 4); prefix(observed, 4);
      std::vector<std::vector<char>> expected, actual;
      boost::signals2::scoped_connection a = reference.control->applied_transaction().connect([&](const auto& event) {
         expected.push_back(consensus_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b = observed.control->applied_transaction().connect([&](const auto& event) {
         actual.push_back(consensus_trace(std::get<0>(event)));
      });
      for (size_t index = 0; index < blocks.size(); ++index) {
         BOOST_TEST_CONTEXT("RAM validator workers=" << workers << " checkpoint=" << index) {
            expected.clear(); actual.clear();
            const auto before = observed.control->get_parallel_execution_stats();
            reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index].block)));
            observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index].block)));
            reference.control->abort_block(); observed.control->abort_block();
            BOOST_CHECK(expected == actual);
            BOOST_CHECK(reference.head().id() == observed.head().id());
            BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
            const auto after = observed.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? blocks[index].adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? blocks[index].fallbacks : 0u);
            for (auto owner : {"alice"_n, "bob"_n})
               BOOST_CHECK_EQUAL(reference.control->get_resource_limits_manager().get_account_ram_usage(owner),
                  observed.control->get_resource_limits_manager().get_account_ram_usage(owner));
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(lifecycle_validation_keeps_canonical_ids_after_same_action_delete_recreate_and_aba, validation_corpus) {
   const auto code = lifecycle_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   struct checkpoint { std::vector<char> block; uint64_t adopted, fallbacks; };
   std::vector<checkpoint> blocks;
   {
      fc::temp_directory directory;
      tester source(configuration(directory, 0), genesis);
      prefix(source, 4); source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&](uint64_t adopted, uint64_t fallbacks) {
         blocks.push_back({fc::raw::pack(*source.produce_block()), adopted, fallbacks});
      };
      source.set_code("test"_n, code); seal(0, 0);
      uint64_t nonce = 120000;
      auto run = [&](name scope, uint64_t mode) {
         auto trx = lifecycle_transaction(source, scope, mode, "alice"_n, nonce++);
         BOOST_REQUIRE(!source.push_transaction(trx)->except);
      };
      run("alice"_n, 0); run("bob"_n, 1); run("carol"_n, 2); seal(3, 0);
      run("alice"_n, 0); run("alice"_n, 0); seal(2, 0); // refresh must capture new object IDs, not just equal values
      run("bob"_n, 2); run("dave"_n, 0); seal(2, 0); // bob retains key 2; dave replaces its whole table
      for (unsigned i = 0; i < 5; ++i) seal(0, 0);
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory serial_dir, parallel_dir;
      auto config = configuration(parallel_dir, workers); config.parallel_code_allowlist = {hash};
      tester serial(configuration(serial_dir, 0), genesis), parallel(config, genesis);
      prefix(serial, 4); prefix(parallel, 4);
      std::vector<std::vector<char>> expected, actual;
      boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& event) {
         expected.push_back(consensus_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& event) {
         actual.push_back(consensus_trace(std::get<0>(event)));
      });
      for (const auto& point : blocks) {
         expected.clear(); actual.clear();
         const auto before = parallel.control->get_parallel_execution_stats();
         serial.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(point.block)));
         parallel.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(point.block)));
         serial.control->abort_block(); parallel.control->abort_block();
         BOOST_CHECK(expected == actual);
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK(serial.head().id() == parallel.head().id());
         BOOST_CHECK_EQUAL(serial.last_irreversible_block_num(), parallel.last_irreversible_block_num());
         const auto after = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? point.adopted : 0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? point.fallbacks : 0u);
         BOOST_CHECK_EQUAL(serial.control->get_resource_limits_manager().get_account_ram_usage("alice"_n),
            parallel.control->get_resource_limits_manager().get_account_ram_usage("alice"_n));
      }
   }
}

BOOST_FIXTURE_TEST_CASE(lifecycle_fork_undo_snapshot_and_reopen_keep_fresh_canonical_id_allocation, validation_corpus) {
   const auto code = lifecycle_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   for (uint16_t workers : {1, 2, 4}) for (bool multiple : {false, true}) {
      fc::temp_directory first_dir, second_dir, reference_dir, observed_dir, restore_dir;
      auto observed_config = configuration(observed_dir, workers); observed_config.parallel_code_allowlist = {hash};
      tester first(configuration(first_dir, 0), genesis), second(configuration(second_dir, 0), genesis);
      tester reference(configuration(reference_dir, 0), genesis), observed(observed_config, genesis);
      prefix(first, 4); prefix(second, 4); prefix(reference, 4); prefix(observed, 4);
      // No local finalizer keys on either builder: both branches stay reversible.
      first.set_code("test"_n, code);
      const auto common = first.produce_block();
      for (tester* chain : {&second, &reference, &observed}) { chain->push_block(common); chain->control->abort_block(); }
      std::vector<std::vector<char>> expected, actual;
      boost::signals2::scoped_connection a = reference.control->applied_transaction().connect([&](const auto& event) {
         expected.push_back(consensus_trace(std::get<0>(event)));
      });
      auto observe = [&] { return observed.control->applied_transaction().connect([&](const auto& event) {
         actual.push_back(consensus_trace(std::get<0>(event)));
      }); };
      boost::signals2::scoped_connection b = observe();
      auto receive = [&](const signed_block_ptr& block) {
         expected.clear(); actual.clear();
         reference.push_block(block); observed.push_block(block);
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(expected == actual);
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
      };
      auto run = [&](tester& chain, name scope, uint64_t mode, uint64_t nonce) {
         auto trx = lifecycle_transaction(chain, scope, mode, "alice"_n, nonce);
         if (multiple) trx = combine_actions(chain, {
            lifecycle_transaction(chain, scope, 0, "alice"_n, nonce + 1000000), trx});
         BOOST_REQUIRE(!chain.push_transaction(trx)->except);
      };
      run(first, "alice"_n, 0, 130001); run(first, "carol"_n, 0, 130002);
      const auto old_branch = first.produce_block(); receive(old_branch);
      BOOST_CHECK_EQUAL(observed.control->get_parallel_execution_stats().adopted, 2u);
      run(second, "alice"_n, 1, 131001); run(second, "bob"_n, 2, 131002);
      receive(second.produce_block(fc::milliseconds(1000)));
      run(second, "carol"_n, 0, 131003); receive(second.produce_block());
      BOOST_CHECK(observed.head().id() == second.head().id());
      BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, 2u);
      BOOST_CHECK(!observed.control->is_known_unexpired_transaction(
         std::get<packed_transaction>(old_branch->transactions.front().trx).id()));

      const auto snapshot_id = observed.head().id();
      const auto snapshot_hash = observed.control->calculate_integrity_hash();
      std::ostringstream bytes;
      auto writer = std::make_shared<ostream_snapshot_writer>(bytes);
      observed.control->write_snapshot(writer); writer->finalize();
      std::istringstream input(bytes.str());
      auto reader = std::make_shared<istream_snapshot_reader>(input);
      auto restore_config = configuration(restore_dir, workers); restore_config.parallel_code_allowlist = {hash};
      tester restored(restore_config, genesis, call_startup_t::no);
      restored.close(); restored.init(restore_config, reader); restored.control->abort_block();
      BOOST_CHECK(restored.head().id() == snapshot_id);
      BOOST_CHECK(restored.control->calculate_integrity_hash() == snapshot_hash);
      observed.close(); observed.open(); observed.control->abort_block(); b = observe();
      BOOST_CHECK(observed.head().id() == snapshot_id);
      BOOST_CHECK(observed.control->calculate_integrity_hash() == snapshot_hash);
      std::vector<std::vector<char>> restored_traces;
      boost::signals2::scoped_connection c = restored.control->applied_transaction().connect([&](const auto& event) {
         restored_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      run(second, "alice"_n, 0, 131004); run(second, "dave"_n, 0, 131005);
      const auto continuation = second.produce_block(); receive(continuation);
      restored.push_block(continuation); restored.control->abort_block();
      BOOST_CHECK(restored_traces == expected);
      BOOST_CHECK(restored.control->calculate_integrity_hash() == reference.control->calculate_integrity_hash());
      BOOST_CHECK_GT(restored.control->get_parallel_execution_stats().adopted, 0u);
      BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, 0u);
   }
}

BOOST_AUTO_TEST_CASE(empty_code_notifications_keep_original_order_dedup_and_receipt_sequences) {
   const auto code = compile(std::string(R"((module
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "is_account" (func $account (param i64) (result i32)))
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (drop (call $account (i64.const )") + std::to_string("bob"_n.to_uint64_t()) + R"()))
         (call $notify (get_local $receiver))
         (call $notify (i64.const )" + std::to_string("bob"_n.to_uint64_t()) + R"())
         (call $notify (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"())
         (call $notify (i64.const )" + std::to_string("bob"_n.to_uint64_t()) + R"()))))");
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   const auto genesis = base_tester::default_genesis();
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory original_dir, parallel_dir;
      auto original_config = base_tester::default_config(original_dir).first;
      eosio::testing::configure_parallel_test_runtime(original_config);
      auto config = base_tester::default_config(parallel_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      config.parallel_validation_threads = workers; config.parallel_code_allowlist = {hash};
      tester original(original_config, genesis), observed(config, genesis);
      original.create_accounts({"test"_n, "alice"_n, "bob"_n});
      original.set_code("test"_n, code);
      observed.push_block(original.produce_block());
      observed.control->abort_block();
      std::vector<std::vector<char>> expected, actual;
      boost::signals2::scoped_connection a = original.control->applied_transaction().connect([&](const auto& event) {
         if (std::get<0>(event)->action_traces.front().act.account == "test"_n)
            expected.push_back(consensus_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b = observed.control->applied_transaction().connect([&](const auto& event) {
         const auto& trace = std::get<0>(event);
         if (trace->action_traces.front().act.account != "test"_n) return;
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
         const std::array receivers{"test"_n, "bob"_n, "alice"_n};
         for (unsigned i = 0; i < receivers.size(); ++i) {
            const auto& act = trace->action_traces[i];
            BOOST_REQUIRE(act.receipt);
            BOOST_CHECK(act.receiver == receivers[i]);
            BOOST_CHECK_EQUAL(act.action_ordinal.value, i + 1);
            BOOST_CHECK_EQUAL(act.creator_action_ordinal.value, i ? 1u : 0u);
            BOOST_CHECK(act.account_ram_deltas.empty());
         }
         actual.push_back(consensus_trace(trace));
      });
      push(original, "alice"_n, 1, 1); push(original, "alice"_n, 1, 2);
      const auto block = original.produce_block();
      observed.push_block(block); original.control->abort_block(); observed.control->abort_block();
      BOOST_REQUIRE_EQUAL(expected.size(), 2u);
      BOOST_CHECK(expected == actual);
      BOOST_CHECK(original.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
      BOOST_CHECK_EQUAL(observed.control->get_parallel_execution_stats().adopted, workers ? 2u : 0u);
   }
}

BOOST_AUTO_TEST_CASE(real_token_transfers_preserve_notifications_conflicts_and_lifecycle_fallbacks) {
   const auto code = token_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   struct checkpoint { std::vector<char> block; int adopted; int fallbacks; };
   std::vector<checkpoint> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(config);
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&](int adopted = -1, int fallbacks = -1) {
         blocks.push_back({fc::raw::pack(*source.produce_block()), adopted, fallbacks});
      };
      setup_token(source, code); seal();
      uint64_t nonce = 1;
      auto transfer = [&](name from, name to, int64_t amount = 10000) {
         auto trx = token_transaction(source, from, to, amount, nonce++);
         const auto trace = source.push_transaction(trx);
         BOOST_REQUIRE(!trace->except);
         BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
         BOOST_CHECK(trace->action_traces[0].receiver == "test"_n);
         BOOST_CHECK(trace->action_traces[1].receiver == from);
         BOOST_CHECK(trace->action_traces[2].receiver == to);
      };
      transfer("alice"_n, "bob"_n); transfer("carol"_n, "dave"_n); transfer("erin"_n, "frank"_n); seal(3, 0);
      transfer("alice"_n, "bob"_n); transfer("alice"_n, "bob"_n); transfer("carol"_n, "dave"_n); seal(3, 0);
      transfer("bob"_n, "grace"_n); transfer("grace"_n, "bob"_n); seal(1, 1); // old grace balance is zero
      transfer("alice"_n, "blank"_n); transfer("carol"_n, "dave"_n); seal(2, 0); // G4b: first balance/table
      transfer("sponsor"_n, "bob"_n); transfer("carol"_n, "dave"_n); seal(2, 0); // G4a: original RAM payer replay
      transfer("sponsor"_n, "bob"_n); transfer("carol"_n, "dave"_n); seal(2, 0); // now owner-paid
      auto close_grace = [&] {
         BOOST_REQUIRE(!source.push_action("test"_n, "close"_n, "grace"_n,
            fc::mutable_variant_object()("owner", "grace")("symbol", "4,TKN"))->except);
      };
      auto open_grace = [&] {
         BOOST_REQUIRE(!source.push_action("test"_n, "open"_n, "alice"_n,
            fc::mutable_variant_object()("owner", "grace")("symbol", "4,TKN")("ram_payer", "alice"))->except);
      };
      close_grace(); transfer("carol"_n, "dave"_n); seal(2, 0); // last row/table refund
      open_grace(); transfer("carol"_n, "dave"_n); seal(2, 0); // new table and row paid by alice
      close_grace(); open_grace(); seal(2, 0); // refresh observes the removal, then recreates canonical IDs/RAM
      open_grace(); transfer("carol"_n, "dave"_n); seal(2, 0); // existing open is an adopted no-op
      source.set_code("frank"_n, compile("(module (func (export \"apply\") (param i64 i64 i64)))")); seal();
      transfer("erin"_n, "frank"_n); transfer("carol"_n, "dave"_n); seal(0, 2); // child code, even no-op
      source.set_code("frank"_n, std::vector<uint8_t>{}); seal();
      transfer("erin"_n, "frank"_n); transfer("carol"_n, "dave"_n); seal(2, 0);
      for (unsigned i = 0; i < 5; ++i) seal(0, 0);
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto config = base_tester::default_config(observed_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      config.parallel_validation_threads = workers; config.parallel_code_allowlist = {hash};
      auto original_config = base_tester::default_config(reference_dir).first;
      eosio::testing::configure_parallel_test_runtime(original_config);
      tester original(original_config, genesis), observed(config, genesis);
      std::vector<std::vector<char>> expected, actual;
      boost::signals2::scoped_connection a = original.control->applied_transaction().connect([&](const auto& event) {
         expected.push_back(consensus_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b = observed.control->applied_transaction().connect([&](const auto& event) {
         actual.push_back(consensus_trace(std::get<0>(event)));
      });
      for (size_t index = 0; index < blocks.size(); ++index) {
         BOOST_TEST_CONTEXT("token workers=" << workers << " checkpoint=" << index) {
            expected.clear(); actual.clear();
            const auto before = observed.control->get_parallel_execution_stats();
            original.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index].block)));
            observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index].block)));
            original.control->abort_block(); observed.control->abort_block();
            BOOST_CHECK(expected == actual);
            BOOST_CHECK(original.head().id() == observed.head().id());
            BOOST_CHECK_EQUAL(original.last_irreversible_block_num(), observed.last_irreversible_block_num());
            BOOST_CHECK(original.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
            const auto after = observed.control->get_parallel_execution_stats();
            if (blocks[index].adopted >= 0) {
               BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? uint64_t(blocks[index].adopted) : 0u);
               BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? uint64_t(blocks[index].fallbacks) : 0u);
            }
         }
      }
      for (const auto owner : {"alice"_n, "bob"_n, "carol"_n, "dave"_n, "erin"_n, "frank"_n, "grace"_n, "sponsor"_n, "blank"_n})
         BOOST_CHECK_EQUAL(token_balance(original, owner), token_balance(observed, owner));
   }
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(parallel_production_tests)
using parallel_validation_tests::validation_corpus;

struct production_chain : tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};

signed_transaction make_transaction(tester& chain, name scope, int64_t delta, uint64_t nonce,
                                    uint64_t work = 0, name signer = "alice"_n, uint8_t cpu_ms = 0) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = "run"_n; act.authorization = {{"alice"_n, "active"_n}};
   const std::array<uint64_t, 4> data{scope.to_uint64_t(), uint64_t(delta), work, nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.max_cpu_usage_ms = cpu_ms;
   trx.sign(chain.get_private_key(signer, "active"), chain.control->get_chain_id());
   return trx;
}
transaction_metadata_ptr metadata(tester& chain, const signed_transaction& trx) {
   return transaction_metadata::recover_keys(
      std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
      chain.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
}
transaction_trace_ptr production_push(tester& chain, const transaction_metadata_ptr& trx,
      fc::microseconds max_time = fc::seconds(1), fc::time_point deadline = fc::time_point::maximum()) {
   return chain.control->push_transaction(trx, deadline, max_time, 0, false, 0);
}
void check_row(tester& chain, name scope, uint64_t expected) {
   auto bytes = chain.get_row_by_account("test"_n, scope, "rows"_n, name{1});
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   uint64_t value; std::memcpy(&value, bytes.data(), 8); BOOST_CHECK_EQUAL(value, expected);
}

BOOST_FIXTURE_TEST_CASE(large_disjoint_table_batches_use_aggregate_capture_limits, validation_corpus) {
   constexpr size_t count = 96;
   fc::temp_directory producer_dir;
   production_chain producer(configuration(producer_dir, 0), genesis);
   prefix(producer, 2);
   producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   std::vector<name> scopes;
   producer.start();
   for (size_t i = 0; i < count; ++i) {
      const name scope{uint64_t(1000 + i) << 4};
      scopes.push_back(scope);
      push(producer, scope, 10, 800000 + i);
   }
   const auto seeded = producer.produce_block();
   producer.control->abort_block(); producer.start();
   producer.set_code("test"_n, code);
   const auto deployed = producer.produce_block();
   producer.control->abort_block(); producer.start();
   for (size_t i = 0; i < count; ++i) push(producer, scopes[i], 7, 810000 + i);
   const auto business = producer.produce_block();
   producer.control->abort_block();
   BOOST_REQUIRE_EQUAL(business->transactions.size(), count);
   for (uint16_t workers : {32, 64, 128}) BOOST_TEST_CONTEXT("workers=" << workers) {
      fc::temp_directory directory;
      auto cfg = configuration(directory, workers);
      cfg.parallel_validation_batch_size = count;
      tester observed(cfg, genesis);
      prefix(observed, 2);
      observed.push_block(seeded); observed.control->abort_block();
      observed.push_block(deployed); observed.control->abort_block();
      const auto before = observed.control->get_parallel_execution_stats();
      observed.push_block(business); observed.control->abort_block();
      const auto after = observed.control->get_parallel_execution_stats();
      BOOST_TEST_MESSAGE(experimental::parallel_attempt_diagnostics(before, after));
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, count);
      BOOST_CHECK_EQUAL(after.preparation_budget_fallbacks - before.preparation_budget_fallbacks, 0u);
      BOOST_CHECK_EQUAL(after.preparation_deadlines - before.preparation_deadlines, 0u);
      BOOST_CHECK(observed.control->calculate_integrity_hash() == producer.control->calculate_integrity_hash());
      for (auto scope : scopes) check_row(observed, scope, 17);
   }
}

BOOST_AUTO_TEST_CASE(sparse_discovery_uses_global_cap_and_preserves_exact_exhaustion_and_reuse) {
   // One new exact query per attempt, followed by one successful execution.
   // The table exists: unknown keys cannot be inferred absent from copied rows.
   // Thus 63 queries complete on attempt 64; 64 queries need attempt 65 and
   // MUST fall back. Give each probe a distinct action selector, so a learned
   // path from the previous probe cannot make this COLD boundary warm. This
   // WAT intentionally ignores the action selector; execution is otherwise
   // unchanged. Separate prefix-refresh tests require actual warm-path gains.
   // The last short transaction checks pool reuse after exhaustion.
   const auto code = compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $key i64) (local $it i32) (local $first i32)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $key (i64.const 1))
         (loop $next
            (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
               (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (get_local $key)))
            (if (i64.eq (get_local $key) (i64.const 1)) (then (set_local $first (get_local $it))))
            (set_local $key (i64.add (get_local $key) (i64.const 1)))
            (br_if $next (i64.le_u (get_local $key) (i64.load (i32.const 8)))))
         (call $update (get_local $first) (i64.const 0) (i32.const 8) (i32.const 8))
         (call $return (i32.const 8) (i32.const 8)))))");
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory producer_dir, serial_dir, parallel_dir;
      auto config = base_tester::default_config(producer_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      config.parallel_validation_threads = workers;
      config.parallel_production_enabled = workers != 0;
      config.parallel_code_allowlist = {hash};
      auto serial_config = base_tester::default_config(serial_dir).first;
      eosio::testing::configure_parallel_test_runtime(serial_config);
      auto parallel_config = base_tester::default_config(parallel_dir).first;
      eosio::testing::configure_parallel_test_runtime(parallel_config);
      parallel_config.parallel_validation_threads = workers;
      parallel_config.parallel_code_allowlist = {hash};
      production_chain producer(config, genesis);
      tester serial(serial_config, genesis), parallel(parallel_config, genesis);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal_setup = [&] {
         const auto block = producer.produce_block();
         serial.push_block(block); parallel.push_block(block);
         producer.control->abort_block(); serial.control->abort_block(); parallel.control->abort_block();
      };
      producer.create_accounts({"test"_n, "alice"_n});
      producer.set_code("test"_n, initialize_code(true)); seal_setup();
      uint64_t nonce = 490000;
      for (uint64_t key : {1, 2, 3}) push(producer, "alice"_n, 10, nonce++, 0, key);
      seal_setup(); producer.set_code("test"_n, code); seal_setup();
      for (const uint64_t queries : {7, 8, 9, 63, 64, 65, 1}) {
         BOOST_TEST_CONTEXT("sparse discovery queries=" << queries << " workers=" << workers) {
            producer.start();
            auto raw = make_transaction(producer, "alice"_n, queries, nonce++);
            raw.actions.front().name = name{queries};
            raw.signatures.clear();
            raw.sign(producer.get_private_key("alice"_n, "active"), genesis.compute_chain_id());
            const auto trx = metadata(producer, raw);
            transaction_trace_ptr produced, original, observed;
            boost::signals2::scoped_connection first = serial.control->applied_transaction().connect([&](const auto& event) {
               if (std::get<0>(event)->id == trx->id()) original = std::get<0>(event);
            });
            boost::signals2::scoped_connection second = parallel.control->applied_transaction().connect([&](const auto& event) {
               if (std::get<0>(event)->id == trx->id()) observed = std::get<0>(event);
            });
            const auto check_stats = [&](const auto& before, const auto& after, const char* path) {
               const bool adopted = workers && queries < 64;
               const auto diagnostic = experimental::parallel_attempt_diagnostics(before, after);
               BOOST_TEST_MESSAGE(path << " " << diagnostic);
               BOOST_TEST_CONTEXT(path << " " << diagnostic) {
                  BOOST_CHECK_EQUAL(after.attempts - before.attempts, workers ? std::min<uint64_t>(64, queries + 1) : 0u);
                  BOOST_CHECK_EQUAL(after.adopted - before.adopted, adopted ? 1u : 0u);
                  BOOST_CHECK_EQUAL(after.prepared_successes - before.prepared_successes, adopted ? 1u : 0u);
                  BOOST_CHECK_EQUAL(after.discovery_exhaustions - before.discovery_exhaustions, workers && queries >= 64 ? 1u : 0u);
                  BOOST_CHECK_EQUAL(after.preparation_deadlines - before.preparation_deadlines, 0u);
                  BOOST_CHECK_EQUAL(after.worker_deadlines - before.worker_deadlines, 0u);
                  BOOST_CHECK_EQUAL(after.worker_vm_timeouts - before.worker_vm_timeouts, 0u);
                  BOOST_CHECK_EQUAL(after.preparation_budget_fallbacks - before.preparation_budget_fallbacks, 0u);
                  BOOST_CHECK_EQUAL(after.preparation_other_fallbacks - before.preparation_other_fallbacks, 0u);
               }
            };
            const auto before_production = producer.control->get_parallel_production_stats();
            size_t callbacks = 0;
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({trx}, fc::time_point::maximum(),
               [&](const auto& input) {
                  ++callbacks; BOOST_CHECK(input == trx); produced = production_push(producer, input); return true;
               }), 1u);
            BOOST_CHECK_EQUAL(callbacks, 1u);
            BOOST_REQUIRE(produced && !produced->except && produced->receipt);
            check_stats(before_production, producer.control->get_parallel_production_stats(), "production");
            const auto before_validation = parallel.control->get_parallel_execution_stats();
            const auto block = producer.finish_block();
            BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
            serial.push_block(block); parallel.push_block(block);
            serial.control->abort_block(); parallel.control->abort_block();
            BOOST_REQUIRE(original && observed && !original->except && !observed->except);
            check_stats(before_validation, parallel.control->get_parallel_execution_stats(), "validation");
            BOOST_CHECK(consensus_trace(produced) == consensus_trace(original));
            BOOST_CHECK(consensus_trace(observed) == consensus_trace(original));
            BOOST_CHECK(producer.head().id() == serial.head().id());
            BOOST_CHECK(parallel.head().id() == serial.head().id());
            BOOST_CHECK_EQUAL(parallel.last_irreversible_block_num(), serial.last_irreversible_block_num());
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            BOOST_CHECK(parallel.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            check_row(producer, "alice"_n, queries); check_row(serial, "alice"_n, queries); check_row(parallel, "alice"_n, queries);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(uneven_discovery_executes_only_unfinished_transactions_on_fresh_chain) {
   // Four independent transactions need 0, 1, 4 and 12 sparse queries.
   // A whole-batch retry executes 4*13=52 jobs; the new frontier must execute
   // exactly 1+2+5+13=21, without changing receipts or canonical state.
   const auto code = compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64) (local $key i64)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (if (i64.gt_u (i64.load (i32.const 8)) (i64.const 0)) (then
            (set_local $key (i64.const 1))
            (loop $next
               (drop (call $find (get_local $receiver) (i64.load (i32.const 0))
                  (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (get_local $key)))
               (set_local $key (i64.add (get_local $key) (i64.const 1)))
               (br_if $next (i64.le_u (get_local $key) (i64.load (i32.const 8)))))))
         (call $return (i32.const 8) (i32.const 8)))))");
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace(); genesis.initial_savanna->version = 2;
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   for (uint16_t workers : {1, 2, 4, 8, 12, 16, 32, 64, 128}) {
      fc::temp_directory producer_dir, serial_dir, parallel_dir;
      auto cfg = base_tester::default_config(producer_dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg);
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = true;
      cfg.parallel_code_allowlist = {hash};
      auto serial_cfg = base_tester::default_config(serial_dir).first;
      eosio::testing::configure_parallel_test_runtime(serial_cfg);
      auto parallel_cfg = base_tester::default_config(parallel_dir).first;
      eosio::testing::configure_parallel_test_runtime(parallel_cfg);
      parallel_cfg.parallel_validation_threads = workers; parallel_cfg.parallel_code_allowlist = {hash};
      production_chain producer(cfg, genesis);
      tester serial(serial_cfg, genesis), parallel(parallel_cfg, genesis);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] {
         const auto block = producer.produce_block(); serial.push_block(block); parallel.push_block(block);
         producer.control->abort_block(); serial.control->abort_block(); parallel.control->abort_block();
      };
      producer.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n});
      producer.set_code("test"_n, initialize_code(true)); seal();
      uint64_t nonce = 510000;
      const std::array scopes{"alice"_n, "bob"_n, "carol"_n, "dave"_n};
      for (const auto scope : scopes) push(producer, scope, 10, nonce++, 0, 1);
      seal(); producer.set_code("test"_n, code); seal();
      producer.start();
      const std::array<uint64_t, 4> query_counts{0, 1, 4, 12};
      std::vector<transaction_metadata_ptr> batch;
      for (size_t i = 0; i < scopes.size(); ++i)
         batch.push_back(metadata(producer, make_transaction(producer, scopes[i], query_counts[i], nonce++)));
      std::vector<std::vector<char>> produced, expected, observed;
      boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& event) {
         if (!is_onblock(*std::get<0>(event))) expected.push_back(consensus_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& event) {
         if (!is_onblock(*std::get<0>(event))) observed.push_back(consensus_trace(std::get<0>(event)));
      });
      const auto production_before = producer.control->get_parallel_production_stats();
      BOOST_REQUIRE_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
         [&](const auto& input) {
            const auto trace = production_push(producer, input);
            BOOST_REQUIRE(trace && !trace->except && trace->receipt);
            produced.push_back(consensus_trace(trace)); return true;
         }), 4u);
      const auto check_stats = [](const auto& before, const auto& after) {
         BOOST_TEST_CONTEXT(experimental::parallel_attempt_diagnostics(before, after)) {
            BOOST_CHECK_EQUAL(after.attempts - before.attempts, 21u);
            BOOST_CHECK_EQUAL(after.succeeded - before.succeeded, 4u);
            BOOST_CHECK_EQUAL(after.prepared_successes - before.prepared_successes, 4u);
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, 4u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
         }
      };
      check_stats(production_before, producer.control->get_parallel_production_stats());
      const auto validation_before = parallel.control->get_parallel_execution_stats();
      const auto block = producer.finish_block();
      BOOST_REQUIRE_EQUAL(block->transactions.size(), 4u);
      serial.push_block(block); parallel.push_block(block);
      serial.control->abort_block(); parallel.control->abort_block();
      check_stats(validation_before, parallel.control->get_parallel_execution_stats());
      BOOST_REQUIRE_EQUAL(expected.size(), 4u);
      BOOST_CHECK(produced == expected); BOOST_CHECK(observed == expected);
      BOOST_CHECK(producer.head().id() == serial.head().id());
      BOOST_CHECK(parallel.head().id() == serial.head().id());
      BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
      BOOST_CHECK(parallel.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
      for (const auto scope : scopes) {
         check_row(producer, scope, 10); check_row(serial, scope, 10); check_row(parallel, scope, 10);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(nonexplicit_production_preserves_order_receipts_and_serial_validation, validation_corpus) {
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory producer_dir, validator_dir;
      auto config = configuration(producer_dir, workers);
      config.parallel_production_enabled = workers != 0;
      production_chain producer(config, genesis);
      tester validator(configuration(validator_dir, 0), genesis);
      prefix(producer, 4); prefix(validator, 4);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      producer.start();
      std::vector<transaction_metadata_ptr> transactions;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n})
         transactions.push_back(metadata(producer, make_transaction(producer, scope, 7, 9000 + transactions.size(), 20000)));
      size_t callbacks = 0;
      uint64_t billed = 0;
      std::vector<std::vector<char>> produced_traces, verified_traces;
      auto connection = validator.control->applied_transaction().connect([&](const auto& event) {
         if (std::any_of(transactions.begin(), transactions.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
            verified_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(), workers ? 8 : 0);
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(transactions, fc::time_point::maximum(), [&](const auto& trx) {
         BOOST_CHECK(trx == transactions.at(callbacks++));
         auto trace = production_push(producer, trx);
         BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
         BOOST_REQUIRE(trace->receipt);
         BOOST_CHECK_GE(trace->receipt->cpu_usage_us, producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
         billed += trace->receipt->cpu_usage_us;
         produced_traces.push_back(consensus_trace(trace));
         return true;
      }), 4u);
      const auto stats = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(stats.adopted, workers ? 4u : 0u);
      BOOST_CHECK_EQUAL(stats.serial_fallbacks, 0u);
      BOOST_CHECK_GT(billed, 0u);
      auto block = producer.finish_block();
      BOOST_REQUIRE_EQUAL(block->transactions.size(), 4u);
      for (size_t i = 0; i < 4; ++i)
         BOOST_CHECK(std::get<packed_transaction>(block->transactions[i].trx).id() == transactions[i]->id());
      validator.push_block(block); validator.control->abort_block();
      BOOST_CHECK(produced_traces == verified_traces);
      BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) check_row(producer, scope, 17);
   }
}

BOOST_FIXTURE_TEST_CASE(adaptive_production_queue_keeps_unconsumed_tail_and_serial_window, validation_corpus) {
   for (uint16_t workers : {1, 2, 4, 8, 16, 32, 64, 128}) {
      fc::temp_directory producer_dir, validator_dir;
      auto cfg = configuration(producer_dir, workers); cfg.parallel_production_enabled = true;
      production_chain producer(cfg, genesis);
      tester validator(configuration(validator_dir, 0), genesis);
      prefix(producer, 4); prefix(validator, 4);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      producer.start();
      std::vector<transaction_metadata_ptr> queue;
      const std::array scopes{"alice"_n, "bob"_n, "carol"_n, "dave"_n};
      for (size_t i = 0; i < 20; ++i)
         queue.push_back(metadata(producer, make_transaction(producer, scopes[i % scopes.size()], 1, 68000 + i)));
      BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(queue.front(), fc::time_point::maximum()),
         std::min<uint16_t>(workers, cfg.parallel_validation_batch_size));
      BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(queue.front(), fc::time_point::now()), 0u);
      BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(queue.front(), fc::time_point::now() + fc::milliseconds(1)), 0u);
      size_t consumed = 0, rounds = 0;
      while (consumed < queue.size()) {
         const size_t selected = producer.control->get_parallel_production_batch_size(queue[consumed], fc::time_point::maximum());
         BOOST_REQUIRE_GT(selected, 0u);
         BOOST_REQUIRE_LE(selected, cfg.parallel_validation_batch_size);
         const auto end = std::min(queue.size(), consumed + selected);
         const std::vector<transaction_metadata_ptr> batch(queue.begin() + consumed, queue.begin() + end);
         const size_t begin = consumed;
         // Same ownership pattern as both producer queues: process a prefix,
         // stop once, then re-query using the untouched head (no tail erasure).
         const auto callbacks = producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
            [&](const auto& trx) {
               BOOST_REQUIRE(trx == queue[consumed]);
               BOOST_REQUIRE(!production_push(producer, trx)->except);
               ++consumed;
               return rounds != 0;
            });
         BOOST_CHECK_EQUAL(callbacks, consumed - begin);
         if (rounds++ == 0) BOOST_CHECK_EQUAL(callbacks, 1u);
      }
      const auto block = producer.finish_block();
      BOOST_REQUIRE_EQUAL(block->transactions.size(), queue.size());
      for (size_t i = 0; i < queue.size(); ++i)
         BOOST_CHECK(std::get<packed_transaction>(block->transactions[i].trx).id() == queue[i]->id());
      validator.push_block(block); validator.control->abort_block();
      BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
      for (const auto scope : scopes) check_row(producer, scope, 15);
      BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(queue.front(), fc::time_point::maximum()), 0u);
   }
}

BOOST_FIXTURE_TEST_CASE(receipt_capacity_avoids_block_tail_preparation_and_recovers_after_abort, validation_corpus) {
   fc::temp_directory producer_dir, validator_dir;
   auto cfg = configuration(producer_dir, 4); cfg.parallel_production_enabled = true;
   production_chain producer(cfg, genesis);
   tester validator(configuration(validator_dir, 0), genesis);
   prefix(producer, 4); prefix(validator, 4);
   producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   producer.start();
   auto first = metadata(producer, make_transaction(producer, "alice"_n, 1, 69500));
   auto next = metadata(producer, make_transaction(producer, "bob"_n, 1, 69501));
   uint32_t billed = 0;
   BOOST_REQUIRE_EQUAL(producer.control->for_each_parallel_production_transaction({first}, fc::time_point::maximum(),
      [&](const auto& trx) {
         auto trace = production_push(producer, trx);
         BOOST_REQUIRE(trace && !trace->except && trace->receipt);
         billed = trace->receipt->cpu_usage_us; return true;
      }), 1u);
   BOOST_REQUIRE_GT(billed, 0u);
   auto& limits = producer.control->get_mutable_resource_limits_manager();
   const uint64_t small_remaining = uint64_t(billed) + (billed + 3u) / 4u;
   BOOST_REQUIRE_GT(limits.get_block_cpu_limit(), small_remaining);
   // Synthetic occupancy ONLY inside an aborted block. Never publish fake
   // resource usage as a valid block or use it for a performance measurement.
   limits.add_transaction_usage({}, limits.get_block_cpu_limit() - small_remaining, 0,
      block_timestamp_type(producer.control->pending_block_time()).slot);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(next, fc::time_point::maximum()), 1u);
   const auto before = producer.control->get_parallel_production_stats();
   limits.add_transaction_usage({}, limits.get_block_cpu_limit(), 0, block_timestamp_type(producer.control->pending_block_time()).slot);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(next, fc::time_point::maximum()), 0u);
   auto failure = production_push(producer, next);
   BOOST_REQUIRE(failure && failure->except);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_stats().attempts, before.attempts);
   check_row(producer, "bob"_n, 10);
   producer.control->abort_block(); producer.start();
   check_row(producer, "alice"_n, 10);
   BOOST_CHECK_GT(producer.control->get_parallel_production_batch_size(next, fc::time_point::maximum()), 0u);
   BOOST_REQUIRE_EQUAL(producer.control->for_each_parallel_production_transaction({first, next}, fc::time_point::maximum(),
      [&](const auto& trx) { BOOST_REQUIRE(!production_push(producer, trx)->except); return true; }), 2u);
   auto block = producer.finish_block();
   BOOST_REQUIRE_EQUAL(block->transactions.size(), 2u);
   validator.push_block(block); validator.control->abort_block();
   BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
   check_row(producer, "alice"_n, 11); check_row(producer, "bob"_n, 11);
}

BOOST_FIXTURE_TEST_CASE(noop_callback_and_stale_metadata_are_not_receipt_observations, validation_corpus) {
   fc::temp_directory dir;
   auto cfg = configuration(dir, 4); cfg.parallel_production_enabled = true;
   production_chain producer(cfg, genesis); prefix(producer, 4); producer.start();
   auto stale = metadata(producer, make_transaction(producer, "alice"_n, 1, 69600));
   auto next = metadata(producer, make_transaction(producer, "bob"_n, 1, 69601));
   stale->billed_cpu_time_us = 100000; // NOT a new canonical receipt
   BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({stale}, fc::time_point::maximum(),
      [&](const auto&) { return true; }), 1u);
   auto bad = metadata(producer, make_transaction(producer, "alice"_n, 1, 69602, 0, "bob"_n));
   bad->billed_cpu_time_us = 100000;
   BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({bad}, fc::time_point::maximum(),
      [&](const auto& trx) { BOOST_REQUIRE(production_push(producer, trx)->except); return true; }), 1u);
   auto& limits = producer.control->get_mutable_resource_limits_manager();
   limits.add_transaction_usage({}, limits.get_block_cpu_limit() - 5000, 0,
      block_timestamp_type(producer.control->pending_block_time()).slot);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(stale, fc::time_point::maximum()), 0u);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(next, fc::time_point::maximum()), 4u);
   check_row(producer, "alice"_n, 10);
   producer.control->abort_block();
}

BOOST_FIXTURE_TEST_CASE(serial_tail_after_ineligible_prefix_cannot_teach_another_shapes_cost, validation_corpus) {
   fc::temp_directory dir;
   auto cfg = configuration(dir, 4); cfg.parallel_production_enabled = true;
   production_chain producer(cfg, genesis); prefix(producer, 4); producer.start();
   auto head = metadata(producer, make_transaction(producer, "alice"_n, 1, 69700));
   auto tail = metadata(producer, combine_actions(producer, {
      make_transaction(producer, "bob"_n, 1, 69701), make_transaction(producer, "carol"_n, 1, 69702)}));
   // Only head belongs to the inspected prefix; nullptr stops preparation.
   // The tail still follows the ORIGINAL callback semantics and can commit.
   BOOST_REQUIRE_EQUAL(producer.control->for_each_parallel_production_transaction({head, nullptr, tail}, fc::time_point::maximum(),
      [&](const auto& trx) {
         if (trx == tail) {
            const auto trace = producer.control->push_transaction(trx, fc::time_point::maximum(), fc::seconds(1), 40000, true, 0);
            BOOST_REQUIRE(trace && !trace->except && trace->receipt);
            BOOST_CHECK_EQUAL(trace->receipt->cpu_usage_us, 40000u);
         }
         return true;
      }), 3u);
   auto& limits = producer.control->get_mutable_resource_limits_manager();
   limits.add_transaction_usage({}, limits.get_block_cpu_limit() - 5000, 0,
      block_timestamp_type(producer.control->pending_block_time()).slot);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(head, fc::time_point::maximum()), 4u);
   check_row(producer, "alice"_n, 10); check_row(producer, "bob"_n, 11);
   producer.control->abort_block();
}

BOOST_FIXTURE_TEST_CASE(single_preparation_deadline_backs_off_only_same_shape_and_keeps_serial_progress, validation_corpus) {
   fc::temp_directory producer_dir, validator_dir;
   auto cfg = configuration(producer_dir, 2); cfg.parallel_production_enabled = true;
   production_chain producer(cfg, genesis);
   tester validator(configuration(validator_dir, 0), genesis);
   prefix(producer, 4); prefix(validator, 4);
   producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   producer.start();
   const auto heavy = metadata(producer, make_transaction(producer, "alice"_n, 1, 69000,
      std::numeric_limits<uint64_t>::max()));
   const auto healthy = metadata(producer, make_transaction(producer, "alice"_n, 1, 69001));
   const auto other_shape = metadata(producer, combine_actions(producer, {
      make_transaction(producer, "bob"_n, 1, 69002), make_transaction(producer, "carol"_n, 1, 69003)}));
   const auto before = producer.control->get_parallel_production_stats();
   // Exercise a REAL deadline, but do not canonically run the intentionally
   // enormous loop. The existing CPU-limit tests cover its original rejection.
   BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({heavy}, fc::time_point::maximum(),
      [&](const auto& trx) { BOOST_CHECK(trx == heavy); return false; }), 1u);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(healthy, fc::time_point::maximum()), 0u);
   BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(other_shape, fc::time_point::maximum()), 2u);
   const auto after = producer.control->get_parallel_production_stats();
   BOOST_CHECK_GT(after.preparation_deadlines + after.worker_deadlines,
                  before.preparation_deadlines + before.worker_deadlines);
   BOOST_CHECK_EQUAL(after.adopted, before.adopted);
   check_row(producer, "alice"_n, 10);
   // A zero recommendation means SERIAL, not "wait" or "reject this shape".
   BOOST_REQUIRE(!production_push(producer, healthy)->except);
   check_row(producer, "alice"_n, 11);
   const auto block = producer.finish_block();
   BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
   validator.push_block(block); validator.control->abort_block();
   BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
}

BOOST_FIXTURE_TEST_CASE(coded_notification_production_bills_noncontiguous_steps_and_matches_both_validators, validation_corpus) {
   const std::array codes{notification_adoption_code(0), notification_adoption_code(1),
                         notification_adoption_code(2), notification_adoption_code(3)};
   flat_set<digest_type> reviewed;
   for (const auto& code : codes) reviewed.insert(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()));
   for (uint16_t workers : {0, 1, 2, 4}) {
      BOOST_TEST_CONTEXT("notification production workers=" << workers) {
         fc::temp_directory producer_dir, serial_dir, parallel_dir;
         auto cfg = configuration(producer_dir, workers); cfg.parallel_production_enabled = workers != 0;
         cfg.parallel_code_allowlist = reviewed;
         auto validator_cfg = configuration(parallel_dir, workers); validator_cfg.parallel_code_allowlist = reviewed;
         production_chain producer(cfg, genesis);
         tester serial(configuration(serial_dir, 0), genesis), parallel(validator_cfg, genesis);
         prefix(producer, 4); prefix(serial, 4); prefix(parallel, 4);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         auto sync_setup = [&] {
            const auto block = producer.produce_block();
            serial.push_block(block); serial.control->abort_block();
            parallel.push_block(block); parallel.control->abort_block();
            producer.control->abort_block(); producer.start();
         };
         uint64_t nonce = 280000;
         producer.set_code("alice"_n, initialize_code());
         for (const auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) {
            auto trx = notification_transaction(producer, "alice"_n, scope, 100, nonce++, 1);
            BOOST_REQUIRE(!producer.push_transaction(trx)->except);
         }
         const std::array receivers{"test"_n, "alice"_n, "bob"_n, "carol"_n};
         for (size_t i = 0; i < receivers.size(); ++i) producer.set_code(receivers[i], codes[i]);
         sync_setup();
         auto run = [&](std::vector<name> scopes, size_t expected_adopted, size_t expected_fallbacks) {
            std::vector<transaction_metadata_ptr> batch;
            for (const auto scope : scopes)
               batch.push_back(metadata(producer, notification_transaction(producer, "test"_n, scope, 2, nonce++)));
            std::vector<std::vector<char>> produced, verified, concurrent;
            auto collect = [&](auto& output, const auto& event) {
               if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
                  output.push_back(consensus_trace(std::get<0>(event)));
            };
            boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& e) { collect(verified, e); });
            boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& e) { collect(concurrent, e); });
            const auto before = producer.control->get_parallel_production_stats();
            const auto validation_before = parallel.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
               const auto trace = production_push(producer, trx);
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               BOOST_REQUIRE(trace->receipt);
               BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 8u);
               int64_t action_elapsed = 0;
               uint64_t sequence = 0;
               for (size_t index : {0u, 2u, 3u, 4u, 1u, 5u, 6u, 7u}) {
                  const auto& action = trace->action_traces[index];
                  BOOST_REQUIRE(action.receipt);
                  BOOST_CHECK_GT(action.receipt->global_sequence, sequence);
                  sequence = action.receipt->global_sequence;
                  action_elapsed += action.elapsed.count();
               }
               BOOST_CHECK_LE(action_elapsed, trace->elapsed.count());
               BOOST_CHECK_GE(trace->receipt->cpu_usage_us, producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
               produced.push_back(consensus_trace(trace)); return true;
            }), batch.size());
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? expected_adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? expected_fallbacks : 0u);
            const auto block = producer.finish_block();
            serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
            BOOST_CHECK(produced == verified); BOOST_CHECK(produced == concurrent);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - validation_before.adopted,
                              workers ? expected_adopted : 0u);
            producer.start();
         };
         run({"alice"_n, "dave"_n}, 2, 0);
         run({"alice"_n, "alice"_n, "dave"_n}, 3, 0);
         producer.set_code("carol"_n, std::vector<uint8_t>{}); sync_setup();
         run({"alice"_n, "dave"_n}, 2, 0); // empty tail still consumes exactly one step
         check_row(producer, "alice"_n, 30); check_row(producer, "dave"_n, 25);
         auto bytes = producer.get_row_by_account("alice"_n, "alice"_n, "rows"_n, name{1});
         BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
         uint64_t own_value; std::memcpy(&own_value, bytes.data(), 8); BOOST_CHECK_EQUAL(own_value, 120u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(coded_notification_deep_traps_and_original_ram_failures_undo_the_whole_prefix, validation_corpus) {
   for (uint16_t workers : {0, 1, 2, 4}) for (unsigned failure = 0; failure < 2; ++failure) {
      BOOST_TEST_CONTEXT("notification rollback workers=" << workers << " failure=" << failure) {
         const std::array codes{notification_adoption_code(0), notification_adoption_code(1, failure ? 2 : 0),
                               notification_adoption_code(2), notification_adoption_code(3, failure ? 0 : 1)};
         flat_set<digest_type> reviewed;
         for (const auto& code : codes) reviewed.insert(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()));
         fc::temp_directory producer_dir, serial_dir;
         auto cfg = configuration(producer_dir, workers); cfg.parallel_production_enabled = workers != 0;
         cfg.parallel_code_allowlist = reviewed;
         production_chain producer(cfg, genesis), serial(configuration(serial_dir, 0), genesis);
         prefix(producer, 4); prefix(serial, 4);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         BOOST_REQUIRE(producer.control->is_builtin_activated(builtin_protocol_feature_t::ram_restrictions));
         producer.set_code("alice"_n, initialize_code());
         uint64_t nonce = 290000;
         for (const auto scope : {"alice"_n, "dave"_n}) {
            auto trx = notification_transaction(producer, "alice"_n, scope, 100, nonce++, 1);
            BOOST_REQUIRE(!producer.push_transaction(trx)->except);
         }
         const std::array receivers{"test"_n, "alice"_n, "bob"_n, "carol"_n};
         for (size_t i = 0; i < receivers.size(); ++i) producer.set_code(receivers[i], codes[i]);
         serial.push_block(producer.produce_block()); producer.control->abort_block(); serial.control->abort_block();
         producer.start(); serial.start();
         const auto producer_before = producer.control->calculate_integrity_hash();
         const auto serial_before = serial.control->calculate_integrity_hash();
         auto bad = notification_transaction(producer, "test"_n, "alice"_n, 9, nonce++);
         const auto original_failure = production_push(serial, metadata(serial, bad));
         BOOST_REQUIRE(original_failure->except);
         BOOST_CHECK(serial.control->calculate_integrity_hash() == serial_before);
         const std::vector<transaction_metadata_ptr> batch{metadata(producer, bad),
            metadata(producer, notification_transaction(producer, "test"_n, "dave"_n, 2, nonce++))};
         std::vector<char> accepted;
         size_t index = 0;
         const auto before = producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
            const auto trace = production_push(producer, trx);
            if (index++ == 0) {
               BOOST_REQUIRE(trace->except);
               BOOST_CHECK(consensus_trace(trace) == consensus_trace(original_failure));
               BOOST_CHECK(producer.control->calculate_integrity_hash() == producer_before);
               check_row(producer, "alice"_n, 10);
            } else {
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               accepted = consensus_trace(trace);
            }
            return true;
         }), 2u);
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, 0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? 2u : 0u);
         if (workers && failure) BOOST_CHECK_GT(after.succeeded - before.succeeded, 0u); // RAM is checked on the owner
         serial.control->abort_block();
         std::vector<char> verified;
         boost::signals2::scoped_connection connection = serial.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == batch[1]->id()) verified = consensus_trace(std::get<0>(event));
         });
         const auto block = producer.finish_block();
         BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
         serial.push_block(block); serial.control->abort_block();
         BOOST_CHECK(accepted == verified);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         check_row(producer, "alice"_n, 10); check_row(producer, "dave"_n, 15);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(notification_receiver_capture_is_owned_and_rejects_invalid_capabilities, validation_corpus) {
   using namespace experimental;
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   const packed_transaction packed(notification_transaction(chain, "test"_n, "alice"_n, 2, 295000),
                                   packed_transaction::compression_type::none);
   const auto prepared = prepare_isolated_transaction(*chain.control, packed);
   BOOST_REQUIRE(prepared);
   std::vector<isolated_action_input> roots;
   for (const auto& act : *prepared) roots.push_back(act.input);
   const auto owned = prepare_isolated_receiver_program(*chain.control, "test"_n, roots);
   BOOST_REQUIRE(owned); BOOST_CHECK(owned->code == prepared->front().code);
   const auto empty = prepare_isolated_receiver_program(*chain.control, "alice"_n, roots);
   BOOST_REQUIRE(empty); BOOST_CHECK(empty->code.empty()); BOOST_CHECK(!empty->serial_only);
   for (const auto account : {name{}, "missing"_n, "eosio"_n, "eosio.msig"_n, "eosio.wrap"_n})
      BOOST_CHECK(!prepare_isolated_receiver_program(*chain.control, account, roots));
   BOOST_CHECK(!prepare_isolated_receiver_program(*chain.control, "test"_n, {}));
   auto oversized = roots; oversized.resize(17, roots.front());
   BOOST_CHECK(!prepare_isolated_receiver_program(*chain.control, "test"_n, oversized));
   const auto original = chain.control->calculate_integrity_hash();
   // Test-only metadata fault injection, inside a nested undo session. The
   // production controller's mutable database remains private.
   auto& db = const_cast<chainbase::database&>(chain.control->db());
   for (unsigned fault = 0; fault < 5; ++fault) {
      {
         auto undo = db.start_undo_session(true);
         db.modify(db.get<account_metadata_object, by_name>("test"_n), [&](auto& metadata) {
            if (fault == 0) metadata.set_privileged(true);
            if (fault == 1) metadata.vm_type = 1;
            if (fault == 2) metadata.vm_version = 1;
            if (fault == 3) metadata.code_hash = fc::sha256::hash("missing code");
            if (fault == 4) metadata.code_hash = {};
         });
         const auto changed = prepare_isolated_receiver_program(*chain.control, "test"_n, roots);
         if (fault == 4) { BOOST_REQUIRE(changed); BOOST_CHECK(changed->code.empty()); }
         else BOOST_CHECK(!changed);
         BOOST_CHECK(owned->code == prepared->front().code); // no live metadata/code alias
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash() == original);
   }
}

BOOST_FIXTURE_TEST_CASE(notification_canonical_cursor_prefix_survives_later_defensive_fallback, validation_corpus) {
   using namespace experimental;
   auto constant = [](name value) { return "(i64.const " + std::to_string(value.to_uint64_t()) + ")"; };
   auto program = [&](bool child) {
      const auto find = [&](name scope) { return "(call $find " + constant("test"_n) + " " + constant(scope) +
         " " + constant("rows"_n) + " (i64.const 1))"; };
      const auto effects = child ? std::string{} : R"(
         (i64.store (i32.const 8) (i64.add (i64.load (i32.const 8)) (i64.const 2)))
         (call $update (get_local $it) (i64.const 0) (i32.const 8) (i32.const 8))
         (drop )" + find("bob"_n) + ")(call $notify " + constant("alice"_n) + ")";
      return compile(R"((module
         (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
         (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
         (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
         (import "env" "require_recipient" (func $notify (param i64)))
         (import "env" "set_action_return_value" (func $return (param i32 i32)))
         (memory 1 1)
         (func (export "apply") (param i64 i64 i64) (local $it i32)
            (set_local $it )" + find(child ? "carol"_n : "alice"_n) + R"()
            (i32.store (i32.const 0) (get_local $it))
            (drop (call $get (get_local $it) (i32.const 8) (i32.const 8)))
            )" + effects + R"((call $return (i32.const 0) (i32.const 16)))))");
   };
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4);
   chain.set_code("test"_n, program(false)); chain.set_code("alice"_n, program(true));
   chain.produce_block(); chain.control->abort_block(); chain.start();
   const packed_transaction packed(notification_transaction(chain, "test"_n, "alice"_n, 2, 296000, 1),
                                   packed_transaction::compression_type::none);
   const auto prepared = prepare_isolated_transaction(*chain.control, packed);
   BOOST_REQUIRE(prepared); BOOST_REQUIRE_EQUAL(prepared->size(), 1u);
   const std::vector roots{prepared->front().input};
   std::vector<isolated_receiver_program> programs;
   for (const auto receiver : {"test"_n, "alice"_n}) {
      const auto captured = prepare_isolated_receiver_program(*chain.control, receiver, roots);
      BOOST_REQUIRE(captured); programs.push_back(*captured);
   }
   const std::array<table_key, 3> keys{{{"test"_n, "alice"_n, "rows"_n},
                                      {"test"_n, "bob"_n, "rows"_n}, {"test"_n, "carol"_n, "rows"_n}}};
   chainbase_transaction_state_view view(chain.control->db());
   isolated_notification_executor executor;
   const auto result = executor.execute(frozen_primary_state::capture(view, keys), roots, programs);
   BOOST_REQUIRE_EQUAL(result.steps.size(), 2u);
   const auto before = chain.control->calculate_integrity_hash();
   std::vector<char> original_root, original_child;
   digest_type original_state;
   for (unsigned mode = 0; mode < 6; ++mode) {
      BOOST_TEST_CONTEXT("direct original-host cursor boundary mode=" << mode) {
         {
            platform_timer timer;
            const auto start = fc::time_point::now();
            transaction_context transaction(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
               action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
            transaction.block_deadline = start + fc::seconds(5);
            transaction.explicit_billed_cpu_time = true; transaction.billed_cpu_time_us = 100;
            transaction.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
            transaction.trace->action_traces.reserve(16);
            transaction.trace->action_traces.emplace_back(*transaction.trace, packed.get_transaction().actions.front(),
                                                          "test"_n, false, 1, 0, 0);
            apply_context context(*chain.control, transaction, 1);
            if (mode == 0) chain.control->get_wasm_interface().execute(result.steps[0].code_hash, 0, 0, context);
            else BOOST_REQUIRE(try_adopt_notification_result(context, result.steps[0].code_hash, 0, 0,
                                                             programs[0].limits, result, 0));
            if (mode == 0) original_root = context.action_return_value;
            else BOOST_CHECK(context.action_return_value == original_root);
            BOOST_REQUIRE_EQUAL(transaction.trace->action_traces.size(), 2u);
            // Direct host-boundary unit: retain the ORIGINAL shared cache while
            // modelling exec()'s receiver transition. This does not stand in
            // for full controller receipt/CPU tests, which run separately.
            context.receiver = "alice"_n;
            auto& current = context.get_current_action_trace();
            current.receiver = "alice"_n; current.action_ordinal = 2;
            current.creator_action_ordinal = 1; current.closest_unnotified_ancestor_action_ordinal = 1;
            context.action_return_value.clear();
            if (mode == 1) {
               BOOST_REQUIRE(try_adopt_notification_result(context, result.steps[1].code_hash, 0, 0,
                                                           programs[1].limits, result, 1));
            } else {
               if (mode > 1) {
                  auto rejected = result;
                  auto limits = programs[1].limits;
                  if (mode == 2) rejected.steps[1].effects.execution_fingerprint = {};
                  if (mode == 3) rejected.steps[1].receiver = "bob"_n;
                  if (mode == 4) --limits.max_pages;
                  const auto prefix_state = chain.control->calculate_integrity_hash();
                  BOOST_CHECK(!try_adopt_notification_result(context, result.steps[1].code_hash, 0, mode == 5 ? 1 : 0,
                                                             limits, rejected, 1));
                  BOOST_CHECK(chain.control->calculate_integrity_hash() == prefix_state);
                  BOOST_CHECK(context.action_return_value.empty());
                  BOOST_CHECK_EQUAL(transaction.trace->action_traces.size(), 2u);
               }
               chain.control->get_wasm_interface().execute(result.steps[1].code_hash, 0, 0, context);
            }
            BOOST_REQUIRE_EQUAL(context.action_return_value.size(), 16u);
            int32_t iterator; std::memcpy(&iterator, context.action_return_value.data(), sizeof(iterator));
            BOOST_CHECK_EQUAL(iterator, 2); // root's non-writing Bob query consumed handle 1
            check_row(chain, "alice"_n, 12);
            if (mode == 0) { original_child = context.action_return_value; original_state = chain.control->calculate_integrity_hash(); }
            else { BOOST_CHECK(context.action_return_value == original_child); BOOST_CHECK(chain.control->calculate_integrity_hash() == original_state); }
         }
         BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(multiple_actions_adopt_shared_prefix_and_preserve_transaction_conflicts, validation_corpus) {
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory producer_dir, serial_dir, parallel_dir;
      auto cfg = configuration(producer_dir, workers); cfg.parallel_production_enabled = workers != 0;
      production_chain producer(cfg, genesis);
      tester serial(configuration(serial_dir, 0), genesis), parallel(configuration(parallel_dir, workers), genesis);
      prefix(producer, 4); prefix(serial, 4); prefix(parallel, 4);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
      uint64_t nonce = 120000;
      auto pair = [&](name a, int64_t x, name b, int64_t y) {
         return combine_actions(producer, {make_transaction(producer, a, x, nonce++),
                                            make_transaction(producer, b, y, nonce++)});
      };
      std::vector<signed_transaction> signed_inputs{
         pair("alice"_n, 5, "alice"_n, -12), // second action needs the first action's credit
         pair("bob"_n, 2, "bob"_n, 4),
         pair("alice"_n, 1, "carol"_n, 7), // whole transaction conflicts with the first
         pair("dave"_n, 1, "dave"_n, 1)};
      auto run = [&](const std::vector<signed_transaction>& inputs, size_t expected_adopted, size_t expected_fallbacks) {
         std::vector<transaction_metadata_ptr> batch;
         for (const auto& input : inputs) batch.push_back(metadata(producer, input));
         std::vector<std::vector<char>> produced, verified, concurrent;
         auto collect = [&](auto& output, const auto& event) {
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
               output.push_back(consensus_trace(std::get<0>(event)));
         };
         boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& e) { collect(verified, e); });
         boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& e) { collect(concurrent, e); });
         const auto before = producer.control->get_parallel_production_stats();
         const auto validation_before = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
            const auto trace = production_push(producer, trx);
            BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
            BOOST_REQUIRE(trace->receipt);
            BOOST_REQUIRE_EQUAL(trace->action_traces.size(), trx->packed_trx()->get_transaction().actions.size());
            int64_t actions_elapsed = 0;
            for (size_t i = 0; i < trace->action_traces.size(); ++i) {
               BOOST_CHECK_EQUAL(trace->action_traces[i].action_ordinal.value, i + 1);
               BOOST_CHECK_EQUAL(trace->action_traces[i].creator_action_ordinal.value, 0u);
               actions_elapsed += trace->action_traces[i].elapsed.count();
            }
            BOOST_CHECK_LE(actions_elapsed, trace->elapsed.count());
            BOOST_CHECK_GE(trace->receipt->cpu_usage_us, producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
            produced.push_back(consensus_trace(trace)); return true;
         }), inputs.size());
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? expected_adopted : 0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? expected_fallbacks : 0u);
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block();
         parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(produced == verified); BOOST_CHECK(produced == concurrent);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - validation_before.adopted,
                           workers ? expected_adopted : 0u);
         producer.start();
      };
      run(signed_inputs, 4, 0); // third transaction refreshes; fourth keeps its independent initial result
      check_row(producer, "alice"_n, 4); check_row(producer, "bob"_n, 16);
      check_row(producer, "carol"_n, 17); check_row(producer, "dave"_n, 12);
      for (size_t count : {16u, 17u}) {
         std::vector<signed_transaction> actions;
         for (size_t i = 0; i < count; ++i) actions.push_back(make_transaction(producer, "alice"_n, 1, nonce++));
         run({combine_actions(producer, std::move(actions))}, count == 16 ? 1 : 0, 0);
      }
      check_row(producer, "alice"_n, 37);
   }
}

BOOST_FIXTURE_TEST_CASE(multiple_lifecycle_actions_keep_notifications_and_failed_tail_atomic, validation_corpus) {
   const auto code = lifecycle_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   for (uint16_t workers : {0, 1, 2, 4}) for (unsigned scenario = 0; scenario < 6; ++scenario) {
      BOOST_TEST_CONTEXT("multi lifecycle workers=" << workers << " case=" << scenario) {
         fc::temp_directory producer_dir, validator_dir;
         auto cfg = configuration(producer_dir, workers); cfg.parallel_production_enabled = workers != 0;
         cfg.parallel_code_allowlist = {hash};
         production_chain producer(cfg, genesis), validator(configuration(validator_dir, 0), genesis);
         prefix(producer, 4); prefix(validator, 4);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.set_code("test"_n, code);
         if (scenario == 5) {
            producer.set_bios_contract();
            const auto usage = producer.control->get_resource_limits_manager().get_account_ram_usage("alice"_n);
            BOOST_REQUIRE(!producer.push_action("eosio"_n, "setalimits"_n, "eosio"_n, fc::mutable_variant_object()
               ("account", "alice")("ram_bytes", usage)("net_weight", -1)("cpu_weight", -1))->except);
         }
         validator.push_block(producer.produce_block()); producer.control->abort_block(); validator.control->abort_block();
         producer.start();
         const auto payer = scenario == 2 ? "missing"_n : (scenario == 3 || scenario == 4 ? "bob"_n : "alice"_n);
         const auto mode = scenario == 1 ? 3u : (scenario == 5 ? 1u : 0u);
         const bool fails = scenario != 0 && scenario != 4;
         const auto first = combine_actions(producer, {
            lifecycle_transaction(producer, "alice"_n, 0, "alice"_n, 121000),
            lifecycle_transaction(producer, "alice"_n, mode, payer, 121001, scenario == 4)});
         transaction_trace_ptr expected_failure;
         if (fails) {
            validator.start(); expected_failure = production_push(validator, metadata(validator, first));
            BOOST_REQUIRE(expected_failure->except); validator.control->abort_block();
         }
         const std::vector<transaction_metadata_ptr> batch{
            metadata(producer, first), metadata(producer, lifecycle_transaction(producer, "carol"_n, 0, "alice"_n, 121002))};
         std::vector<std::vector<char>> produced, verified;
         boost::signals2::scoped_connection connection = validator.control->applied_transaction().connect([&](const auto& event) {
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
               verified.push_back(consensus_trace(std::get<0>(event)));
         });
         const auto before = producer.control->get_parallel_production_stats();
         size_t index = 0;
         producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
            const auto state_before = producer.control->calculate_integrity_hash();
            const auto trace = production_push(producer, trx);
            if (index++ == 0 && fails) {
               BOOST_REQUIRE(trace->except);
               BOOST_CHECK(consensus_trace(trace) == consensus_trace(expected_failure));
               BOOST_CHECK(producer.control->calculate_integrity_hash() == state_before);
               BOOST_CHECK(!producer.control->is_known_unexpired_transaction(trx->id()));
            } else {
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               produced.push_back(consensus_trace(trace));
            }
            return true;
         });
         const auto after = producer.control->get_parallel_production_stats();
         const size_t adopted = fails ? (scenario == 5 ? 1u : 0u) : 2u;
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? adopted : 0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? 2 - adopted : 0u);
         const auto block = producer.finish_block();
         validator.push_block(block); validator.control->abort_block();
         BOOST_CHECK(produced == verified);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
      }
   }
}

BOOST_FIXTURE_TEST_CASE(multiple_real_token_actions_spend_first_credit_and_close_new_balance, validation_corpus) {
   const auto code = token_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory producer_dir, validator_dir, parallel_dir;
      auto cfg = configuration(producer_dir, workers); cfg.parallel_production_enabled = workers != 0;
      cfg.parallel_code_allowlist = {hash};
      production_chain producer(cfg, genesis), validator(configuration(validator_dir, 0), genesis);
      auto parallel_config = configuration(parallel_dir, workers); parallel_config.parallel_code_allowlist = {hash};
      tester parallel(parallel_config, genesis);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); setup_token(producer, code);
      const auto setup = producer.produce_block();
      validator.push_block(setup); parallel.push_block(setup);
      producer.control->abort_block(); validator.control->abort_block(); parallel.control->abort_block(); producer.start();
      signed_transaction close;
      close.actions.push_back(producer.get_action("test"_n, "close"_n, {{"blank"_n, "active"_n}},
         fc::mutable_variant_object()("owner", "blank")("symbol", "4,TKN")));
      const auto first = combine_actions(producer, {token_transaction(producer, "alice"_n, "blank"_n, 100, 122000),
         token_transaction(producer, "blank"_n, "bob"_n, 100, 122001), close});
      const auto second = combine_actions(producer, {token_transaction(producer, "carol"_n, "dave"_n, 1, 122002),
         token_transaction(producer, "dave"_n, "carol"_n, 1, 122003)});
      const std::vector<transaction_metadata_ptr> batch{metadata(producer, first), metadata(producer, second)};
      const auto alice = token_balance(producer, "alice"_n), bob = token_balance(producer, "bob"_n);
      const auto ram = producer.control->get_resource_limits_manager().get_account_ram_usage("alice"_n);
      std::vector<std::vector<char>> produced, verified, concurrent;
      auto collect = [&](auto& out, const auto& event) {
         if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
            out.push_back(consensus_trace(std::get<0>(event)));
      };
      boost::signals2::scoped_connection a = validator.control->applied_transaction().connect([&](const auto& e) { collect(verified, e); });
      boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& e) { collect(concurrent, e); });
      const auto before = producer.control->get_parallel_production_stats();
      const auto validation_before = parallel.control->get_parallel_execution_stats();
      producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
         const auto trace = production_push(producer, trx);
         BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
         produced.push_back(consensus_trace(trace)); return true;
      });
      const auto production_after = producer.control->get_parallel_production_stats();
      // Diagnostics only: the batch has already joined. Keep the original
      // workload, budgets, expected adoption and consensus checks unchanged.
      const auto production_diagnostics = experimental::parallel_attempt_diagnostics(before, production_after);
      BOOST_TEST_MESSAGE("TOKEN_MULTI production workers=" << workers
         << " adopted=" << production_after.adopted - before.adopted
         << " serial_fallbacks=" << production_after.serial_fallbacks - before.serial_fallbacks
         << " " << production_diagnostics);
      BOOST_TEST_CONTEXT("multi real token production workers=" << workers << " " << production_diagnostics) {
         BOOST_CHECK_EQUAL(production_after.adopted - before.adopted, workers ? 2u : 0u);
      }
      BOOST_CHECK_EQUAL(token_balance(producer, "alice"_n), alice - 100);
      BOOST_CHECK_EQUAL(token_balance(producer, "bob"_n), bob + 100);
      BOOST_CHECK(producer.get_row_by_account("test"_n, "blank"_n, "accounts"_n,
         name{symbol::from_string("4,TKN").to_symbol_code().value}).empty());
      BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage("alice"_n), ram);
      const auto block = producer.finish_block();
      validator.push_block(block); validator.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
      BOOST_CHECK(produced == verified); BOOST_CHECK(produced == concurrent);
      BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
      BOOST_CHECK(validator.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
      const auto validation_after = parallel.control->get_parallel_execution_stats();
      const auto validation_diagnostics = experimental::parallel_attempt_diagnostics(validation_before, validation_after);
      BOOST_TEST_MESSAGE("TOKEN_MULTI validation workers=" << workers
         << " adopted=" << validation_after.adopted - validation_before.adopted
         << " serial_fallbacks=" << validation_after.serial_fallbacks - validation_before.serial_fallbacks
         << " " << validation_diagnostics);
      BOOST_TEST_CONTEXT("multi real token validation workers=" << workers << " " << validation_diagnostics) {
         BOOST_CHECK_EQUAL(validation_after.adopted - validation_before.adopted, workers ? 2u : 0u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(ram_replay_preserves_payers_limits_intermediate_failures_and_transaction_undo, validation_corpus) {
   const auto code = resize_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   for (uint16_t workers : {0, 1, 2, 4}) for (unsigned mode = 0; mode < 9; ++mode) {
      BOOST_TEST_CONTEXT("RAM producer workers=" << workers << " mode=" << mode) {
         fc::temp_directory producer_dir, validator_dir;
         auto config = configuration(producer_dir, workers);
         config.parallel_production_enabled = workers != 0; config.parallel_code_allowlist = {hash};
         production_chain producer(config, genesis), validator(configuration(validator_dir, 0), genesis);
         prefix(producer, 4); prefix(validator, 4);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         producer.set_code("test"_n, code);
         if (mode == 5 || mode == 6) {
            // Explicit-genesis testers only have the native ABI. Install the
            // real BIOS so limits are changed on-chain and replayed by validator.
            producer.set_bios_contract();
            const auto account = mode == 5 ? "alice"_n : "bob"_n;
            const auto usage = producer.control->get_resource_limits_manager().get_account_ram_usage(account);
            BOOST_REQUIRE(!producer.push_action("eosio"_n, "setalimits"_n, "eosio"_n, fc::mutable_variant_object()
               ("account", account)("ram_bytes", usage + (mode == 5 ? 8 : 0))
               ("net_weight", -1)("cpu_weight", -1))->except);
         }
         validator.push_block(producer.produce_block());
         producer.control->abort_block(); validator.control->abort_block();
         producer.start();
         const resize_steps ordinary{{{16, {}}, {3, {}}, {8, {}}}};
         auto steps = ordinary;
         if (mode == 1 || mode == 3) steps = {{{0, {}}, {32, "bob"_n}, {16, {}}}};
         if (mode == 2) steps[2].bytes = 0;
         if (mode == 4) steps = {{{16, "missing"_n}, {3, "alice"_n}, {8, {}}}};
         if (mode == 5) steps = {{{16, {}}, {32, {}}, {64, {}}}};
         if (mode == 6) steps = {{{16, "bob"_n}, {3, "alice"_n}, {8, {}}}};
         // Fail after one/two canonical mutations as well as before any. Both
         // earlier notifications and partial RAM/value changes need original undo.
         if (mode == 7) steps[1].payer = "missing"_n;
         if (mode == 8) steps[2].payer = "missing"_n;
         auto first = resize_transaction(producer, "alice"_n, steps, 81000 + mode * 2, mode == 1 || mode == 6);
         const bool fails = (mode >= 3 && mode <= 5) || mode >= 7;
         transaction_trace_ptr expected_failure;
         if (fails) {
            validator.start();
            expected_failure = production_push(validator, metadata(validator, first));
            BOOST_REQUIRE(expected_failure->except);
            if (mode == 5) BOOST_CHECK_EQUAL(expected_failure->except->code(), ram_usage_exceeded::code_value);
            validator.control->abort_block();
         }
         std::vector<transaction_metadata_ptr> batch{
            metadata(producer, first), metadata(producer, resize_transaction(producer, "bob"_n, ordinary, 81001 + mode * 2))};
         std::vector<std::vector<char>> produced, verified;
         boost::signals2::scoped_connection connection = validator.control->applied_transaction().connect([&](const auto& event) {
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) {
                  return trx->id() == std::get<0>(event)->id;
               })) verified.push_back(consensus_trace(std::get<0>(event)));
         });
         const auto before = producer.control->get_parallel_production_stats();
         size_t callbacks = 0;
         const auto diagnostic_label = "RAM production workers=" + std::to_string(workers) + " mode=" + std::to_string(mode);
         const auto run_batch = [&] { return producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
            const auto index = callbacks++;
            const auto state_before = producer.control->calculate_integrity_hash();
            const auto trace = production_push(producer, trx);
            if (index == 0 && fails) {
               BOOST_REQUIRE(trace->except);
               BOOST_CHECK_EQUAL(trace->except->code(), expected_failure->except->code());
               BOOST_CHECK(consensus_trace(trace) == consensus_trace(expected_failure));
               BOOST_CHECK(producer.control->calculate_integrity_hash() == state_before);
               BOOST_CHECK(!producer.control->is_known_unexpired_transaction(trx->id()));
               BOOST_CHECK(!trace->receipt);
            } else {
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               BOOST_REQUIRE(trace->receipt);
               BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
               BOOST_REQUIRE_EQUAL(trace->action_traces.front().return_value.size(), 16u);
               std::array<uint32_t, 4> sizes;
               std::memcpy(sizes.data(), trace->action_traces.front().return_value.data(), sizeof(sizes));
               const auto& applied_steps = index == 0 ? steps : ordinary;
               BOOST_CHECK_EQUAL(sizes[0], 8u);
               for (unsigned i = 0; i < 3; ++i) BOOST_CHECK_EQUAL(sizes[i + 1], applied_steps[i].bytes);
               produced.push_back(consensus_trace(trace));
            }
            return true;
         }); };
         BOOST_CHECK_EQUAL(experimental::parallel_test_measure(diagnostic_label.c_str(), run_batch), 2u);
         BOOST_CHECK_EQUAL(callbacks, 2u);
         const auto after = producer.control->get_parallel_production_stats();
         // In mode 5 the root WASM was adopted, but ORIGINAL transaction-final
         // RAM checks reject it afterwards. "adopted" is not an acceptance count.
         const unsigned adopted = fails ? (mode == 5 ? 1u : 0u) : 2u;
         // Read counters only after all workers/callbacks finished. Preserve
         // the original expectations: a wholly failed preparation is NOT
         // evidence that the RAM adopter/replay path was exercised.
         const auto diagnostics = experimental::parallel_attempt_diagnostics(before, after);
         BOOST_TEST_MESSAGE(diagnostic_label
            << " adopted=" << after.adopted - before.adopted
            << " serial_fallbacks=" << after.serial_fallbacks - before.serial_fallbacks
            << " " << diagnostics);
         BOOST_TEST_CONTEXT(diagnostics) {
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? 2 - adopted : 0u);
         }
         const auto block = producer.finish_block();
         BOOST_REQUIRE_EQUAL(block->transactions.size(), fails ? 1u : 2u);
         validator.push_block(block); validator.control->abort_block();
         BOOST_CHECK(produced == verified);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
         for (auto owner : {"alice"_n, "bob"_n})
            BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(owner),
               validator.control->get_resource_limits_manager().get_account_ram_usage(owner));
      }
   }
}

BOOST_FIXTURE_TEST_CASE(lifecycle_production_preserves_ids_iterators_table_fees_and_partial_failure_undo, validation_corpus) {
   const auto code = lifecycle_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   struct scenario { uint64_t mode; name payer; bool bob_auth; bool fails; };
   const std::array<scenario, 9> scenarios{{
      {0, "alice"_n, false, false}, {1, "alice"_n, false, false}, {2, "alice"_n, false, false},
      {3, "alice"_n, false, true}, {4, "alice"_n, false, true}, {0, "bob"_n, false, true},
      {0, "bob"_n, true, false}, {0, "missing"_n, false, true}, {1, "alice"_n, false, true}}};
   for (uint16_t workers : {0, 1, 2, 4}) for (unsigned index = 0; index < scenarios.size(); ++index) {
      BOOST_TEST_CONTEXT("lifecycle workers=" << workers << " scenario=" << index) {
         const auto& scenario = scenarios[index];
         fc::temp_directory producer_dir, validator_dir;
         auto config = configuration(producer_dir, workers);
         config.parallel_production_enabled = workers != 0; config.parallel_code_allowlist = {hash};
         production_chain producer(config, genesis), validator(configuration(validator_dir, 0), genesis);
         prefix(producer, 4); prefix(validator, 4);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         producer.set_code("test"_n, code);
         if (index == 8) {
            producer.set_bios_contract();
            const auto usage = producer.control->get_resource_limits_manager().get_account_ram_usage("alice"_n);
            BOOST_REQUIRE(!producer.push_action("eosio"_n, "setalimits"_n, "eosio"_n, fc::mutable_variant_object()
               ("account", "alice")("ram_bytes", usage + 4)("net_weight", -1)("cpu_weight", -1))->except);
         }
         validator.push_block(producer.produce_block());
         validator.control->abort_block(); producer.control->abort_block(); producer.start();
         const auto first = lifecycle_transaction(producer, "alice"_n, scenario.mode, scenario.payer,
            91000 + index * 2, scenario.bob_auth);
         transaction_trace_ptr failure;
         if (scenario.fails) {
            validator.start(); failure = production_push(validator, metadata(validator, first));
            BOOST_REQUIRE(failure->except);
            if (index == 8) BOOST_CHECK_EQUAL(failure->except->code(), ram_usage_exceeded::code_value);
            validator.control->abort_block();
         }
         std::vector<transaction_metadata_ptr> batch{metadata(producer, first),
            metadata(producer, lifecycle_transaction(producer, "bob"_n, 0, "alice"_n, 91001 + index * 2))};
         std::vector<std::vector<char>> produced, verified;
         boost::signals2::scoped_connection connection = validator.control->applied_transaction().connect([&](const auto& event) {
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
               verified.push_back(consensus_trace(std::get<0>(event)));
         });
         const auto before = producer.control->get_parallel_production_stats();
         unsigned callbacks = 0;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
            const auto position = callbacks++;
            const auto state_before = producer.control->calculate_integrity_hash();
            const auto trace = production_push(producer, trx);
            if (position == 0 && scenario.fails) {
               BOOST_REQUIRE(trace->except);
               BOOST_CHECK(consensus_trace(trace) == consensus_trace(failure));
               BOOST_CHECK(producer.control->calculate_integrity_hash() == state_before);
               BOOST_CHECK(!trace->receipt);
               BOOST_CHECK(!producer.control->is_known_unexpired_transaction(trx->id()));
            } else {
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               BOOST_REQUIRE(trace->receipt);
               BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
               BOOST_REQUIRE_EQUAL(trace->action_traces[0].return_value.size(), 32u);
               std::array<int32_t, 6> handles;
               uint64_t next_primary;
               std::memcpy(handles.data(), trace->action_traces[0].return_value.data(), sizeof(handles));
               std::memcpy(&next_primary, trace->action_traces[0].return_value.data() + sizeof(handles), sizeof(next_primary));
               const auto mode = position == 0 ? scenario.mode : 0;
               const std::array<int32_t, 6> expected = mode == 0 ? std::array<int32_t, 6>{0, -2, 1, -3, 8, -3} :
                  mode == 1 ? std::array<int32_t, 6>{0, -2, 2, -2, 4, 1} : std::array<int32_t, 6>{0, -2, -1, -1, 0, -1};
               BOOST_CHECK(handles == expected);
               BOOST_CHECK_EQUAL(next_primary, mode == 1 ? 2u : 123u);
               produced.push_back(consensus_trace(trace));
            }
            return true;
         }), 2u);
         BOOST_CHECK_EQUAL(callbacks, 2u);
         const auto after = producer.control->get_parallel_production_stats();
         const unsigned adopted = scenario.fails ? (index == 8 ? 1 : 0) : 2;
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? adopted : 0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? 2 - adopted : 0u);
         const auto block = producer.finish_block();
         BOOST_REQUIRE_EQUAL(block->transactions.size(), scenario.fails ? 1u : 2u);
         validator.push_block(block); validator.control->abort_block();
         BOOST_CHECK(produced == verified);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
         for (auto owner : {"alice"_n, "bob"_n}) BOOST_CHECK_EQUAL(
            producer.control->get_resource_limits_manager().get_account_ram_usage(owner),
            validator.control->get_resource_limits_manager().get_account_ram_usage(owner));
      }
   }
}

BOOST_AUTO_TEST_CASE(real_token_production_notifications_and_rejections_preserve_atomicity) {
   const auto code = token_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory producer_dir, validator_dir;
      auto config = base_tester::default_config(producer_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      config.parallel_validation_threads = workers; config.parallel_production_enabled = workers != 0;
      config.parallel_code_allowlist = {hash};
      auto serial_config = base_tester::default_config(validator_dir).first;
      eosio::testing::configure_parallel_test_runtime(serial_config);
      production_chain producer(config, genesis), validator(serial_config, genesis);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      setup_token(producer, code);
      validator.push_block(producer.produce_block());
      validator.control->abort_block(); producer.control->abort_block();
      uint64_t nonce = 100000;
      for (unsigned mode = 0; mode < 6; ++mode) {
         BOOST_TEST_CONTEXT("token producer workers=" << workers << " mode=" << mode) {
            if (mode == 4) {
               producer.set_code("bob"_n, compile("(module (func (export \"apply\") (param i64 i64 i64) (unreachable)))"));
               validator.push_block(producer.produce_block());
               validator.control->abort_block(); producer.control->abort_block();
            }
            if (mode == 5) {
               producer.set_code("bob"_n, std::vector<uint8_t>{});
               validator.push_block(producer.produce_block());
               validator.control->abort_block(); producer.control->abort_block();
            }
            producer.start();
            std::vector<transaction_metadata_ptr> transactions;
            const bool fails = mode >= 2 && mode <= 4;
            auto first = token_transaction(producer, "alice"_n, mode == 3 ? "missing"_n : "bob"_n,
               mode == 2 ? 10000000000ll : 10000, nonce++);
            transactions.push_back(metadata(producer, first));
            transactions.push_back(metadata(producer, token_transaction(producer,
               mode == 1 ? "alice"_n : "carol"_n, mode == 1 ? "bob"_n : "dave"_n, 10000, nonce++)));
            transaction_trace_ptr expected_failure;
            if (fails) {
               validator.start();
               expected_failure = production_push(validator, metadata(validator, first));
               BOOST_REQUIRE(expected_failure->except);
               validator.control->abort_block();
            }
            std::vector<std::vector<char>> produced, verified;
            boost::signals2::scoped_connection connection = validator.control->applied_transaction().connect([&](const auto& event) {
               if (std::any_of(transactions.begin(), transactions.end(), [&](const auto& trx) {
                     return trx->id() == std::get<0>(event)->id;
                  })) verified.push_back(consensus_trace(std::get<0>(event)));
            });
            const auto before = producer.control->get_parallel_production_stats();
            size_t callbacks = 0;
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(transactions,
               fc::time_point::maximum(), [&](const auto& trx) {
                  const auto before_state = producer.control->calculate_integrity_hash();
                  const auto index = callbacks++;
                  BOOST_CHECK(trx == transactions[index]);
                  const auto trace = production_push(producer, trx);
                  if (fails && index == 0) {
                     BOOST_REQUIRE(trace->except);
                     BOOST_REQUIRE(!trace->receipt);
                     BOOST_CHECK_EQUAL(trace->except->code(), expected_failure->except->code());
                     BOOST_CHECK(consensus_trace(trace) == consensus_trace(expected_failure));
                     BOOST_CHECK(producer.control->calculate_integrity_hash() == before_state);
                     BOOST_CHECK(!producer.control->is_known_unexpired_transaction(trx->id()));
                  } else {
                     BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
                     BOOST_REQUIRE(trace->receipt);
                     BOOST_CHECK_GE(trace->receipt->cpu_usage_us,
                        producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
                     BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 3u);
                     for (const auto& act : trace->action_traces) BOOST_CHECK(act.account_ram_deltas.empty());
                     produced.push_back(consensus_trace(trace));
                  }
                  return true;
               }), 2u);
            BOOST_CHECK_EQUAL(callbacks, 2u);
            const auto after = producer.control->get_parallel_production_stats();
            const auto adopted = fails ? 0u : 2u;
            BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests,
                              workers && mode == 1 ? 1u : 0u);
            BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes,
                              workers && mode == 1 ? 1u : 0u);
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? 2 - adopted : 0u);
            const auto block = producer.finish_block();
            BOOST_REQUIRE_EQUAL(block->transactions.size(), fails ? 1u : 2u);
            validator.push_block(block); validator.control->abort_block();
            BOOST_CHECK(produced == verified);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(real_cdt_production_charges_cpu_and_serial_validator_replays_results_and_fallbacks, validation_corpus) {
   const auto cdt_code = cdt_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(cdt_code.data()), cdt_code.size());
   for (uint16_t workers : {0, 1, 2, 4}) {
      BOOST_TEST_CONTEXT("CDT producer workers=" << workers) {
         fc::temp_directory producer_dir, validator_dir;
         auto config = configuration(producer_dir, workers);
         config.parallel_production_enabled = workers != 0;
         config.parallel_code_allowlist = {hash};
         production_chain producer(config, genesis);
         tester validator(configuration(validator_dir, 0), genesis);
         prefix(producer, 1); prefix(validator, 1);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         producer.set_code("test"_n, cdt_code);
         uint64_t nonce = 50000;
         for (auto scope : {"alice"_n, "bob"_n, "carol"_n})
            push(producer, scope, 10, nonce++, 0, std::nullopt, "init"_n);
         auto setup = producer.produce_block();
         validator.push_block(setup); validator.control->abort_block(); producer.control->abort_block();
         for (unsigned round = 0; round < 3; ++round) {
            producer.start();
            std::vector<transaction_metadata_ptr> transactions;
            for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) {
               const auto allocation = round == 1 && scope == "alice"_n ? 2 * 1024 * 1024 : 4096;
               transactions.push_back(metadata(producer, make_transaction(producer, scope, 1, nonce++, allocation)));
            }
            std::vector<std::vector<char>> produced_traces, verified_traces;
            // This subscription is per round; a plain connection does NOT
            // disconnect on destruction and would retain dead local refs.
            boost::signals2::scoped_connection connection = validator.control->applied_transaction().connect([&](const auto& event) {
               if (std::any_of(transactions.begin(), transactions.end(), [&](const auto& trx) {
                     return trx->id() == std::get<0>(event)->id;
                  })) verified_traces.push_back(consensus_trace(std::get<0>(event)));
            });
            const auto before = producer.control->get_parallel_production_stats();
            size_t callbacks = 0;
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(transactions,
               fc::time_point::maximum(), [&](const auto& trx) {
                  BOOST_CHECK(trx == transactions.at(callbacks++));
                  auto trace = production_push(producer, trx);
                  BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
                  BOOST_REQUIRE(trace->receipt);
                  BOOST_CHECK_GE(trace->receipt->cpu_usage_us,
                     producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
                  BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
                  BOOST_CHECK(trace->action_traces.front().account_ram_deltas.empty());
                  produced_traces.push_back(consensus_trace(trace));
                  return true;
               }), 3u);
            BOOST_CHECK_EQUAL(callbacks, 3u);
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers && round != 1 ? 3u : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers && round == 1 ? 3u : 0u);
            auto block = producer.finish_block();
            BOOST_REQUIRE_EQUAL(block->transactions.size(), 3u);
            validator.push_block(block); validator.control->abort_block();
            BOOST_CHECK_EQUAL(produced_traces.size(), 3u);
            BOOST_CHECK_EQUAL(verified_traces.size(), 3u);
            BOOST_CHECK(produced_traces == verified_traces);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
            for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) {
               const auto value = producer.get_row_by_account("test"_n, scope, "rows"_n, name{1});
               BOOST_REQUIRE_EQUAL(value.size(), 16u);
               int64_t actual; std::memcpy(&actual, value.data() + 8, sizeof(actual));
               BOOST_CHECK_EQUAL(actual, 11 + round);
            }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(conflict_failed_private_and_auth_rejection_use_original_atomic_path, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory directory;
      auto config = configuration(directory, workers); config.parallel_production_enabled = true;
      production_chain chain(config, genesis); prefix(chain, 4); chain.start();
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain, make_transaction(chain, "alice"_n, 10, 9100)),
         metadata(chain, make_transaction(chain, "alice"_n, -15, 9101)), // private 10-15 fails; canonical 20-15 succeeds
         metadata(chain, make_transaction(chain, "bob"_n, 2, 9102))};
      size_t called = 0;
      chain.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
         BOOST_CHECK(trx == batch.at(called++)); BOOST_REQUIRE(!production_push(chain, trx)->except); return true;
      });
      BOOST_CHECK_EQUAL(called, 3u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted, 1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().serial_fallbacks, 2u);
      check_row(chain, "alice"_n, 5); check_row(chain, "bob"_n, 12);
      batch = {metadata(chain, make_transaction(chain, "carol"_n, 1, 9200, 0, "bob"_n)),
               metadata(chain, make_transaction(chain, "dave"_n, 1, 9201))};
      called = 0;
      chain.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
         auto trace = production_push(chain, trx);
         if (called++ == 0) { BOOST_REQUIRE(trace->except); BOOST_CHECK(!trace->receipt); }
         else BOOST_REQUIRE(!trace->except);
         return true;
      });
      BOOST_CHECK_EQUAL(called, 2u);
      check_row(chain, "carol"_n, 10); check_row(chain, "dave"_n, 11);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted, 1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().serial_fallbacks, 4u);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(batch.front()->id()));
      // Ordinary duplicate rejection cannot replay a previous accepted result.
      auto duplicate = production_push(chain, batch.back());
      BOOST_REQUIRE(duplicate->except); BOOST_CHECK_EQUAL(duplicate->except->code(), tx_duplicate::code_value);
      check_row(chain, "dave"_n, 11);
   }
}

BOOST_FIXTURE_TEST_CASE(batch_stop_abort_exception_deadline_and_identity_never_reuse_private_results, validation_corpus) {
   for (unsigned mode = 0; mode < 7; ++mode) {
      fc::temp_directory directory;
      auto config = configuration(directory, 2); config.parallel_production_enabled = true;
      production_chain chain(config, genesis); prefix(chain, 4); chain.start();
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain, make_transaction(chain, "alice"_n, 1, 9300)),
         metadata(chain, make_transaction(chain, "bob"_n, 1, 9301))};
      const auto before = chain.control->calculate_integrity_hash();
      size_t callbacks = 0;
      const auto deadline = mode == 3 ? fc::time_point::now() - fc::seconds(1) : fc::time_point::maximum();
      auto run = [&] {
         return chain.control->for_each_parallel_production_transaction(batch, deadline, [&](const auto& trx) {
            ++callbacks;
            if (mode == 0) return false; // queue declines before push
            if (mode == 1) { chain.control->abort_block(); chain.start(); return true; }
            if (mode == 2) throw std::runtime_error("queue callback failed");
            if (mode == 4) {
               BOOST_CHECK_THROW(chain.control->for_each_parallel_production_transaction(batch, deadline, [](const auto&) { return true; }), transaction_exception);
               return false;
            }
            if (mode == 5) {
               // Same signed bytes but a different queue metadata capability.
               auto other = metadata(chain, trx->packed_trx()->get_signed_transaction());
               BOOST_REQUIRE(!production_push(chain, other)->except);
               return true;
            }
            BOOST_REQUIRE(!production_push(chain, trx)->except);
            chain.control->interrupt_transaction(controller::interrupt_t::speculative_block_trx);
            return true;
         });
      };
      if (mode == 2) BOOST_CHECK_THROW(run(), std::runtime_error);
      else BOOST_CHECK_EQUAL(run(), mode == 3 ? 0u : mode == 5 ? 2u : 1u);
      BOOST_CHECK_EQUAL(callbacks, mode == 3 ? 0u : mode == 5 ? 2u : 1u);
      if (mode <= 4) {
         check_row(chain, "alice"_n, 10); check_row(chain, "bob"_n, 10);
         if (mode != 1) BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
      }
      if (mode == 5) {
         BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted, 0u);
         BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().serial_fallbacks, 2u);
      }
      // Close/abort that pending context and process fresh work. No old slot or
      // reentrancy flag survives any of the above outcomes.
      chain.control->abort_block(); chain.start();
      auto fresh = metadata(chain, make_transaction(chain, "carol"_n, 3, 9400));
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({fresh}, fc::time_point::maximum(), [&](const auto& trx) {
         BOOST_REQUIRE(!production_push(chain, trx)->except); return true;
      }), 1u);
      check_row(chain, "carol"_n, 13);
   }
}

BOOST_FIXTURE_TEST_CASE(private_cpu_is_billed_once_and_obeys_original_deadlines, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   const auto snapshot = chain.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 7; ++mode) {
      auto trx = make_transaction(chain, "alice"_n, 1, 9500, 0, "alice"_n, mode == 2 ? 20 : 0);
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      {
         platform_timer timer;
         const auto start = fc::time_point::now();
         const auto owner_cpu_start = thread_cpu_clock::now();
         transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
         context.block_deadline = start + fc::seconds(5);
         if (mode == 1) context.block_deadline = start + fc::milliseconds(20);
         context.explicit_billed_cpu_time = mode == 3;
         if (mode == 3) context.billed_cpu_time_us = 100;
         context.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
         if (mode == 0 || mode == 1) {
            const int64_t private_us = mode == 1 ? 30000 : 10000;
            BOOST_CHECK_NO_THROW(context.charge_isolated_cpu(fc::microseconds(private_us)));
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), private_us);
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(1)), transaction_exception);
            BOOST_CHECK_NO_THROW(context.checktime());
            context.pause_billing_timer();
            const auto owner_upper_us = std::chrono::duration_cast<std::chrono::microseconds>(
               thread_cpu_clock::now() - owner_cpu_start).count();
            const auto bill = context.update_billed_cpu_time(start);
            BOOST_CHECK_GE(bill, uint64_t(private_us));
            BOOST_CHECK_LE(bill, uint64_t(private_us + owner_upper_us));
            // Advancing a caller's wall instant never invents twelve ms of CPU.
            BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start + fc::milliseconds(12)), bill);
         } else if (mode == 2) {
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(21)), tx_cpu_usage_exceeded);
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 21000);
         } else if (mode == 3) {
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1)), transaction_exception);
            BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start + fc::seconds(1)), 100u);
         } else if (mode == 4) {
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(-1)), transaction_exception);
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::seconds(2)), transaction_exception);
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 0);
         } else if (mode == 5) {
            timer.interrupt_timer();
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1)), interrupt_exception);
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 0);
         } else {
            // The original wall deadline must remain unchanged: private CPU
            // does not make the remaining real wall window negative twice.
            context.block_deadline = start - fc::microseconds(1);
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1)), deadline_exception);
         }
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash() == snapshot); // init + charge all undone
   }
}

BOOST_FIXTURE_TEST_CASE(multiple_private_cpu_charges_are_incremental_ordered_and_preserve_billing_pauses, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   const auto snapshot = chain.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 6; ++mode) {
      BOOST_TEST_CONTEXT("multi CPU mode=" << mode) {
         auto trx = combine_actions(chain, {make_transaction(chain, "alice"_n, 1, 123000 + mode * 3),
            make_transaction(chain, "alice"_n, 1, 123001 + mode * 3),
            make_transaction(chain, "alice"_n, 1, 123002 + mode * 3)});
         if (mode == 1 || mode == 4) trx.max_cpu_usage_ms = 20;
         packed_transaction packed(trx, packed_transaction::compression_type::none);
         {
            platform_timer timer;
            const auto start = fc::time_point::now();
            const auto owner_cpu_start = thread_cpu_clock::now();
            transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
               action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
            context.block_deadline = start + (mode == 2 ? fc::milliseconds(20) : fc::seconds(5));
            context.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
            if (mode == 0) {
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1), 0), transaction_exception);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1), 2), transaction_exception);
               context.charge_isolated_cpu(fc::milliseconds(4), 1);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1), 1), transaction_exception);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1), 3), transaction_exception);
               context.charge_isolated_cpu(fc::milliseconds(5), 2);
               context.charge_isolated_cpu(fc::milliseconds(6), 3);
               BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 15000);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1), 4), transaction_exception);
               context.pause_billing_timer();
               const auto owner_upper_us = std::chrono::duration_cast<std::chrono::microseconds>(
                  thread_cpu_clock::now() - owner_cpu_start).count();
               const auto bill = context.update_billed_cpu_time(start);
               BOOST_CHECK_GE(bill, 15000u);
               BOOST_CHECK_LE(bill, uint64_t(15000 + owner_upper_us));
               BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start + fc::milliseconds(12)), bill);
            } else if (mode == 1) {
               context.charge_isolated_cpu(fc::milliseconds(12), 1);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(9), 2), tx_cpu_usage_exceeded);
               BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 21000);
            } else if (mode == 2) {
               context.charge_isolated_cpu(fc::milliseconds(15), 1);
               BOOST_CHECK_NO_THROW(context.charge_isolated_cpu(fc::milliseconds(15), 2));
               BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 30000);
               BOOST_CHECK_NO_THROW(context.checktime()); // private CPU is not deducted from block wall time
            } else if (mode == 3) {
               context.charge_isolated_cpu(fc::milliseconds(1), 1);
               timer.interrupt_timer();
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1), 2), interrupt_exception);
               BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 1000);
            } else if (mode == 4) {
               context.charge_isolated_cpu(fc::milliseconds(5), 1);
               context.pause_billing_timer();
               std::this_thread::sleep_for(std::chrono::milliseconds(30));
               context.resume_billing_timer();
               BOOST_CHECK_NO_THROW(context.charge_isolated_cpu(fc::milliseconds(5), 2));
               BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 10000);
            } else {
               context.charge_isolated_cpu(fc::milliseconds(1), 1);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(-1), 2), transaction_exception);
               BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::seconds(1), 2), transaction_exception);
               BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 1000);
               BOOST_CHECK_NO_THROW(context.charge_isolated_cpu(fc::microseconds(0), 2));
            }
         }
         BOOST_CHECK(chain.control->calculate_integrity_hash() == snapshot);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(owner_private_and_signature_cpu_are_charged_once_without_waits, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   auto trx = make_transaction(chain, "alice"_n, 1, 95101);
   packed_transaction packed(trx, packed_transaction::compression_type::none);
   platform_timer timer;
   const auto start = fc::time_point::now();
   const auto owner_start = thread_cpu_clock::now();
   transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
      action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input, fc::milliseconds(5));
   context.block_deadline = start + fc::seconds(5);
   context.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
   context.charge_isolated_cpu(fc::milliseconds(10));
   const auto until = thread_cpu_clock::now() + std::chrono::milliseconds(2);
   while (thread_cpu_clock::now() < until) {}
   context.pause_billing_timer();
   const auto owner_upper = std::chrono::duration_cast<std::chrono::microseconds>(thread_cpu_clock::now() - owner_start).count();
   const auto bill = context.update_billed_cpu_time(start);
   BOOST_CHECK_GE(bill, 17000u);
   BOOST_CHECK_LE(bill, uint64_t(15000 + owner_upper));
   BOOST_CHECK_EQUAL(context.trace->cpu_usage.count(), bill);
   std::this_thread::sleep_for(std::chrono::milliseconds(30));
   BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start + fc::seconds(1)), bill);
   context.resume_billing_timer();
   const auto resumed_until = thread_cpu_clock::now() + std::chrono::milliseconds(1);
   while (thread_cpu_clock::now() < resumed_until) {}
   context.pause_billing_timer();
   BOOST_CHECK_GE(context.update_billed_cpu_time(start), bill + 1000u);
   BOOST_CHECK_LT(context.update_billed_cpu_time(start), bill + 10000u);
}

BOOST_FIXTURE_TEST_CASE(cpu_receipt_overflow_and_explicit_charge_are_checked, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   auto trx = make_transaction(chain, "alice"_n, 1, 95102);
   packed_transaction packed(trx, packed_transaction::compression_type::none);
   for (unsigned mode = 0; mode < 4; ++mode) {
      platform_timer timer;
      transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
         action_digests_t::store_which_t::savanna, fc::time_point::now(), transaction_metadata::trx_type::input,
         mode < 2 ? fc::microseconds(mode ? std::numeric_limits<uint32_t>::max() : std::numeric_limits<int64_t>::max()) : fc::microseconds{});
      if (mode < 2) {
         const auto until = thread_cpu_clock::now() + std::chrono::microseconds(10);
         while (thread_cpu_clock::now() < until) {}
         BOOST_CHECK_THROW(context.update_billed_cpu_time(fc::time_point::now()), transaction_exception);
      } else {
         context.explicit_billed_cpu_time = true;
         context.billed_cpu_time_us = mode == 2 ? -1 : int64_t(std::numeric_limits<uint32_t>::max()) + 1;
         BOOST_CHECK_THROW(context.update_billed_cpu_time(fc::time_point::now()), transaction_exception);
         context.billed_cpu_time_us = 123;
         BOOST_CHECK_EQUAL(context.update_billed_cpu_time(fc::time_point::now() + fc::seconds(1)), 123u);
         BOOST_CHECK_EQUAL(context.trace->cpu_usage.count(), 123);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(success_and_failure_traces_supply_cpu_to_subjective_billing, validation_corpus) {
   for (uint32_t pct : {0u, 50u, 100u}) {
      fc::temp_directory directory;
      auto cfg = configuration(directory, 0); cfg.sig_cpu_bill_pct = pct * config::percent_1;
      production_chain chain(cfg, genesis); prefix(chain, 4); chain.start();
      for (unsigned mode = 0; mode < 2; ++mode) {
         auto trx = metadata(chain, make_transaction(chain, "alice"_n, 1, 95110 + mode, 0,
            mode ? "bob"_n : "alice"_n));
         const auto owner_start = thread_cpu_clock::now();
         const auto trace = production_push(chain, trx);
         const auto owner_upper = std::chrono::duration_cast<std::chrono::microseconds>(thread_cpu_clock::now() - owner_start).count();
         const auto signature = EOS_PERCENT(trx->signature_cpu_usage().count(), cfg.sig_cpu_bill_pct);
         BOOST_CHECK_EQUAL(bool(trace->except), mode != 0);
         BOOST_CHECK_GE(trace->cpu_usage.count(), int64_t(signature));
         BOOST_CHECK_GE(trace->cpu_usage.count(), int64_t(chain.control->get_global_properties().configuration.min_transaction_cpu_usage));
         BOOST_CHECK_LE(trace->cpu_usage.count(), std::max<int64_t>(100, owner_upper + signature));
         if (!mode) {
            BOOST_REQUIRE(trace->receipt);
            BOOST_CHECK_EQUAL(trace->receipt->cpu_usage_us, trace->cpu_usage.count());
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(signature_worker_cpu_reaches_the_controller_receipt, validation_corpus) {
   fc::temp_directory directory;
   auto cfg = configuration(directory, 0); cfg.sig_cpu_bill_pct = config::percent_100;
   production_chain chain(cfg, genesis); prefix(chain, 4); chain.start();
   // Compilation is deliberately paused out of account billing. Warm it before
   // the independent outer CPU bound, which also observes those paused costs.
   BOOST_REQUIRE(!production_push(chain, metadata(chain,
      make_transaction(chain, "alice"_n, 0, 95108)))->except);
   authority auth; auth.threshold = 64;
   std::vector<private_key_type> keys;
   for (unsigned i = 0; i < auth.threshold; ++i) {
      keys.push_back(private_key_type::regenerate<fc::ecc::private_key_shim>(
         fc::sha256::hash("CPU recovery fixture " + std::to_string(i))));
      auth.keys.push_back({keys.back().get_public_key(), 1});
   }
   std::sort(auth.keys.begin(), auth.keys.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
   chain.set_authority("alice"_n, "active"_n, auth);
   auto trx = make_transaction(chain, "alice"_n, 1, 95109);
   trx.signatures.clear();
   for (const auto& key : keys) trx.sign(key, chain.control->get_chain_id());
   auto meta = metadata(chain, trx);
   const auto signature_cpu = meta->signature_cpu_usage().count();
   const auto owner_start = thread_cpu_clock::now();
   const auto trace = production_push(chain, meta);
   const auto owner_upper = std::chrono::duration_cast<std::chrono::microseconds>(thread_cpu_clock::now() - owner_start).count();
   BOOST_REQUIRE(!trace->except);
   BOOST_REQUIRE(trace->receipt);
   BOOST_TEST_MESSAGE("CPU_SIGNATURE_BRIDGE signature_us=" << signature_cpu << " owner_upper_us=" << owner_upper
      << " billed_us=" << trace->receipt->cpu_usage_us);
   // Establish that omitting signature CPU cannot be hidden by owner work or
   // the minimum fee. Recovery is genuine; no forged metadata/test capability.
   BOOST_REQUIRE_GT(signature_cpu, std::max<int64_t>(owner_upper, 100));
   BOOST_CHECK_GE(trace->receipt->cpu_usage_us, signature_cpu);
   BOOST_CHECK_LE(trace->receipt->cpu_usage_us, uint64_t(signature_cpu + owner_upper));
}

BOOST_FIXTURE_TEST_CASE(cpu_minimum_block_and_subjective_account_budgets_are_not_bypassed, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   const auto before = chain.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 3; ++mode) {
      auto& limits = chain.control->get_mutable_resource_limits_manager();
      auto trx = make_transaction(chain, "alice"_n, 1, 9600);
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      platform_timer timer;
      const auto start = fc::time_point::now();
      const auto owner_cpu_start = thread_cpu_clock::now();
      transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
         action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
      // These fixture-only resource changes are covered by the context's
      // original undo session; no mutable controller DB capability is exposed.
      if (mode == 1) limits.add_transaction_usage({}, limits.get_block_cpu_limit() - 5000, 0,
         block_timestamp_type(chain.control->pending_block_time()).slot);
      if (mode == 2) {
         limits.set_account_limits("alice"_n, -1, -1, 1, false);
         limits.set_account_limits("bob"_n, -1, -1, 100, false);
         limits.process_account_limit_updates();
      }
      context.block_deadline = start + fc::seconds(5); context.leeway = fc::microseconds(0);
      if (mode == 2) {
         auto [available, ignored] = limits.get_account_cpu_limit("alice"_n);
         BOOST_REQUIRE_GT(available, 10000);
         context.subjective_cpu_bill_us = available - 10000;
      }
      context.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
      if (mode == 0) {
         context.charge_isolated_cpu(fc::microseconds(0));
         context.pause_billing_timer();
         const auto minimum = chain.control->get_global_properties().configuration.min_transaction_cpu_usage;
         const auto upper = std::chrono::duration_cast<std::chrono::microseconds>(thread_cpu_clock::now() - owner_cpu_start).count();
         const auto bill = context.update_billed_cpu_time(start);
         BOOST_CHECK_GE(bill, minimum);
         BOOST_CHECK_LE(bill, std::max<int64_t>(minimum, upper));
      } else if (mode == 1) {
         BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(6000)), block_cpu_usage_exceeded);
      } else {
         BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(11000)), tx_cpu_usage_exceeded);
      }
   }
   BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
}

BOOST_FIXTURE_TEST_CASE(slow_private_queue_head_falls_back_and_does_not_starve_following_transaction, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) for (auto wall_ms : {100, 500}) {
      fc::temp_directory directory;
      auto config = configuration(directory, workers); config.parallel_production_enabled = true;
      production_chain chain(config, genesis); prefix(chain, 4); chain.start();
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain, make_transaction(chain, "alice"_n, 1, 9700, 1'000'000'000)),
         metadata(chain, make_transaction(chain, "bob"_n, 1, 9701))};
      const auto reserved_cpu = std::min<uint64_t>(
         chain.control->get_global_properties().configuration.max_transaction_cpu_usage,
         chain.control->get_resource_limits_manager().get_block_cpu_limit()) + 5'000;
      // Pin the fixture: 100ms bypasses speculation; 500ms admits it with the
      // unchanged 50ms cap. A local cap must not disappear as "external cancel".
      BOOST_REQUIRE_GT(reserved_cpu, 100'000u);
      BOOST_REQUIRE_LT(reserved_cpu, 500'000u);
      const auto before = chain.control->get_parallel_production_stats();
      const auto deadline = fc::time_point::now() + fc::milliseconds(wall_ms);
      size_t callbacks = 0;
      const auto count = chain.control->for_each_parallel_production_transaction(batch, deadline, [&](const auto& trx) {
         auto trace = production_push(chain, trx, fc::milliseconds(20), deadline);
         if (callbacks++ == 0) {
            BOOST_REQUIRE(trace->except);
            BOOST_CHECK_EQUAL(trace->except->code(), tx_cpu_usage_exceeded::code_value);
            BOOST_CHECK(!trace->receipt);
         } else BOOST_REQUIRE(!trace->except);
         return true;
      });
      BOOST_CHECK_EQUAL(count, 2u); BOOST_CHECK_EQUAL(callbacks, 2u);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(batch.front()->id()));
      BOOST_CHECK(chain.control->is_known_unexpired_transaction(batch.back()->id()));
      check_row(chain, "alice"_n, 10); check_row(chain, "bob"_n, 11);
      const auto after = chain.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(after.preparation_deadlines - before.preparation_deadlines, wall_ms == 500 ? 1u : 0u);
      BOOST_CHECK_EQUAL(after.preparation_budget_fallbacks - before.preparation_budget_fallbacks, wall_ms == 500 ? 1u : 0u);
      BOOST_CHECK_EQUAL(after.preparation_other_fallbacks - before.preparation_other_fallbacks, 0u);
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, 0u);
      if (wall_ms == 100) BOOST_CHECK_EQUAL(after.attempts - before.attempts, 0u);
   }
}

BOOST_AUTO_TEST_CASE(export_fresh_live_producer_fixture) {
   const char* destination = std::getenv("PUREEOS_PARALLEL_PRODUCER_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Live process fixture export not requested"); return; }
   const auto directory = std::filesystem::path(destination);
   const bool token_fixture = std::getenv("PUREEOS_PARALLEL_PRODUCER_TOKEN") != nullptr;
   const bool ram_fixture = std::getenv("PUREEOS_PARALLEL_PRODUCER_RAM") != nullptr;
   const bool lifecycle_fixture = std::getenv("PUREEOS_PARALLEL_PRODUCER_LIFECYCLE") != nullptr;
   const bool multi_fixture = std::getenv("PUREEOS_PARALLEL_PRODUCER_MULTI_ACTION") != nullptr;
   const bool notification_fixture = std::getenv("PUREEOS_PARALLEL_PRODUCER_CODED_NOTIFICATIONS") != nullptr;
   BOOST_REQUIRE(!ram_fixture || token_fixture);
   BOOST_REQUIRE(!lifecycle_fixture || (token_fixture && !ram_fixture));
   BOOST_REQUIRE(!multi_fixture || (token_fixture && !ram_fixture && !lifecycle_fixture));
   BOOST_REQUIRE(!notification_fixture || (!token_fixture && !ram_fixture && !lifecycle_fixture && !multi_fixture));
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
   auto genesis = base_tester::default_genesis();
   // FC JSON timestamps retain milliseconds, not arbitrary microseconds.
   // Normalize BEFORE chain-ID calculation, otherwise the exported genesis
   // round trip creates a different chain than the in-memory signed blocks.
   genesis.initial_timestamp = block_timestamp_type(fc::time_point::now() - fc::seconds(5)).to_time_point();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   BOOST_REQUIRE(fc::json::from_string(fc::json::to_string(genesis, fc::time_point::maximum()))
                    .as<genesis_state>().compute_chain_id() == genesis.compute_chain_id());
   fc::temp_directory source_dir;
   auto config = base_tester::default_config(source_dir).first;
   eosio::testing::configure_parallel_test_runtime(config);
   production_chain source(config, genesis);
   source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   fc::variants checkpoints;
   auto seal = [&] {
      auto block = source.produce_block(); source.control->abort_block();
      auto file = "block-" + std::to_string(block->block_num()) + ".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
      checkpoints.emplace_back(fc::mutable_variant_object()
         ("block_num", block->block_num())("block_id", source.head().id())("lib", source.last_irreversible_block_num())
         ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
   };
   auto code = token_fixture ? token_fixture_code() : notification_fixture ? notification_adoption_code(0) : update_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   std::vector<fc::sha256> code_hashes{hash};
   if (token_fixture) {
      setup_token(source, code); seal();
   } else {
      source.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n}); seal();
      source.set_code("test"_n, initialize_code()); seal();
      unsigned nonce = 1;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(source, scope, 10, nonce++);
      seal();
      if (notification_fixture) {
         source.set_code("alice"_n, initialize_code()); seal();
         for (const auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) {
            auto trx = notification_transaction(source, "alice"_n, scope, 100, nonce++, 1);
            BOOST_REQUIRE(!source.push_transaction(trx)->except);
         }
         seal();
         const std::array receivers{"alice"_n, "bob"_n, "carol"_n};
         for (size_t i = 0; i < receivers.size(); ++i) {
            const auto child = notification_adoption_code(i + 1);
            code_hashes.push_back(fc::sha256::hash(reinterpret_cast<const char*>(child.data()), child.size()));
            source.set_code(receivers[i], child);
         }
      }
      source.set_code("test"_n, code); seal();
   }
   source.start();
   fc::variants transactions;
   unsigned transaction_count = 16;
   if (const char* count_text = std::getenv("PUREEOS_PARALLEL_PRODUCER_TRANSACTION_COUNT")) {
      char* end = nullptr;
      const auto count = std::strtoul(count_text, &end, 10);
      BOOST_REQUIRE(end && *end == '\0' && count >= 16 && count <= 4096);
      transaction_count = count;
   }
   for (unsigned i = 0; i < transaction_count; ++i) {
      const std::array scopes{"alice"_n, "bob"_n, "carol"_n, "dave"_n};
      const std::array senders{"alice"_n, "carol"_n, "erin"_n};
      const std::array recipients{"bob"_n, "dave"_n, "frank"_n};
      // sponsor's existing row was funded by alice. The first payment must
      // transfer its RAM bill to sponsor through the unchanged token contract.
      const auto sender = token_fixture ? (ram_fixture && i == 0 ? "sponsor"_n : senders[i % 3]) : "alice"_n;
      const auto recipient = lifecycle_fixture && i == 0 ? "blank"_n : recipients[i % 3];
      auto trx = token_fixture ? token_transaction(source, sender, recipient, 1, 10000 + i) :
         make_transaction(source, scopes[i % 4], 1, 10000 + i, 20000);
      if (notification_fixture)
         trx = notification_transaction(source, "test"_n, i % 2 ? "dave"_n : "alice"_n, 1, 10000 + i);
      if (multi_fixture) {
         if (i == 0) {
            signed_transaction close;
            close.actions.push_back(source.get_action("test"_n, "close"_n, {{"blank"_n, "active"_n}},
               fc::mutable_variant_object()("owner", "blank")("symbol", "4,TKN")));
            trx = combine_actions(source, {token_transaction(source, sender, "blank"_n, 1, 20000 + i * 2),
               token_transaction(source, "blank"_n, recipient, 1, 20001 + i * 2), close});
         } else {
            trx = combine_actions(source, {trx, token_transaction(source, recipient, sender, 1, 20001 + i * 2)});
         }
      }
      trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(1800));
      trx.signatures.clear();
      std::set<std::pair<name, name>> permissions;
      for (const auto& action : trx.actions) for (const auto& auth : action.authorization)
         permissions.emplace(auth.actor, auth.permission);
      for (const auto& [actor, permission] : permissions)
         trx.sign(source.get_private_key(actor, permission.to_string()), source.control->get_chain_id());
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      transactions.emplace_back(fc::mutable_variant_object()("id", packed.id())("packed", packed)("actions", trx.actions.size()));
   }
   BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions, directory / "transactions.json"));
   // Public, deterministic TEST credentials only. Confined to the new private
   // fixture directory, never printed, installed or used on an existing chain.
   std::vector<std::string> providers{
      base_tester::get_public_key("eosio"_n, "active").to_string({}) + "=KEY:" +
         base_tester::get_private_key("eosio"_n, "active").to_string({}),
      public_key.to_string() + "=KEY:" + private_key.to_string()};
   BOOST_REQUIRE(fc::json::save_to_file(providers, directory / "TEST-ONLY-keys.json"));
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
      ("fixture_kind", notification_fixture ? "coded-notification-multiple-actions" : multi_fixture ? "real-token-multiple-actions" : lifecycle_fixture ? "real-token-first-balance" :
         ram_fixture ? "real-token-payer-change" : token_fixture ? "real-token" : "existing-row-wat")
      ("chain_id", source.control->get_chain_id())("code_hashes", code_hashes)("checkpoints", checkpoints), directory / "manifest.json"));
}

BOOST_AUTO_TEST_SUITE_END()

// Explicit, bounded offline measurement fixture. Registered separately so an
// ordinary correctness run does not silently claim this opt-in export ran.
BOOST_AUTO_TEST_SUITE(parallel_benchmark_fixtures)
BOOST_AUTO_TEST_CASE(export_fixed_replay_workloads) {
   using namespace parallel_production_tests;
   const char* destination = std::getenv("PUREEOS_PARALLEL_BENCHMARK_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Benchmark fixture export not requested"); return; }
   const auto root = std::filesystem::path(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(root));
   std::filesystem::permissions(root, std::filesystem::perms::owner_all);
   const auto code = update_code(), replacement = update_code(true);
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   for (const std::string workload : {"independent", "hot", "mixed", "serial"}) {
      const auto directory = root / workload;
      BOOST_REQUIRE(std::filesystem::create_directory(directory));
      auto genesis = base_tester::default_genesis();
      auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace();
      genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory source_dir;
      auto config = base_tester::default_config(source_dir).first;
      eosio::testing::configure_parallel_test_runtime(config);
      production_chain source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      fc::variants checkpoints;
      auto seal = [&] {
         auto block = source.produce_block(); source.control->abort_block();
         auto file = "block-" + std::to_string(block->block_num()) + ".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
         checkpoints.emplace_back(fc::mutable_variant_object()
            ("block_num", block->block_num())("block_id", source.head().id())("lib", source.last_irreversible_block_num())
            ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
      };
      source.create_accounts({"test"_n, "alice"_n}); seal();
      source.set_code("test"_n, initialize_code()); seal();
      unsigned nonce = 1;
      for (unsigned i = 0; i < 16; ++i) push(source, name{uint64_t{100} + i}, 10, nonce++);
      seal(); source.set_code("test"_n, workload == "serial" ? replacement : code); seal();
      for (unsigned block = 0; block < 32; ++block) {
         source.start();
         for (unsigned i = 0; i < 16; ++i) {
            // Native code changes are true serial barriers, not worker hints.
            if (workload == "mixed" && i == 4) source.set_code("test"_n, replacement);
            if (workload == "mixed" && i == 12) source.set_code("test"_n, code);
            auto trx = metadata(source, make_transaction(source, name{uint64_t{100} + (workload == "hot" ? 0 : i)},
                                                          1, nonce++, 80000));
            auto trace = production_push(source, trx);
            BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
            BOOST_REQUIRE(trace->receipt);
         }
         seal();
      }
      for (unsigned i = 0; i < 5; ++i) seal();
      const auto replay_height = source.last_irreversible_block_num();
      BOOST_REQUIRE_GE(replay_height, 37u); // every measured business block is finalized
      const auto replay_point = checkpoints.at(replay_height - 2);
      source.close();
      std::filesystem::create_directory(directory / "blocks");
      for (const char* file : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(config.blocks_dir / file, directory / "blocks" / file);
      BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
      BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
         ("chain_id", genesis.compute_chain_id())("code_hashes", std::vector{hash})("checkpoints", checkpoints)
         ("replay_checkpoint", replay_point)("workload", workload)("business_transactions", 512)
         ("business_blocks", 32)("loop_iterations", 80000), directory / "manifest.json"));
   }
}
BOOST_AUTO_TEST_SUITE_END()

#include "parallel_prefix_refresh_cases.hpp"
#include "parallel_release_fixture.hpp"
#include "parallel_mixed_stress_fixture.hpp"

#include "parallel_multitoken_cases.hpp"

#include "parallel_pipeline_optimization_cases.hpp"

#include "parallel_early_adoption_cases.hpp"

#include "parallel_input_fingerprint_cases.hpp"
