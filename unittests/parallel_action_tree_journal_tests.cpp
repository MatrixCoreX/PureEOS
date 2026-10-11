#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/raw.hpp>
#include <array>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key journal_rows{"test"_n, "test"_n, "rows"_n};
std::string i64(name n) { return "(i64.const " + std::to_string(n.to_uint64_t()) + ")"; }
std::vector<char> journal_wasm(const std::string& text) {
   const auto wasm = wast_to_wasm(text); return {wasm.begin(), wasm.end()};
}
struct journal_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   std::array<isolated_action_input, 2> roots;
   std::vector<isolated_receiver_program> programs;
   journal_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const auto& table = db.create<table_id_object>([](auto& row) {
         row.code = journal_rows.code; row.scope = journal_rows.scope; row.table = journal_rows.table;
         row.payer = journal_rows.code; row.count = 2;
      });
      for (uint64_t i : {1u, 2u}) db.create<key_value_object>([&](auto& row) {
         row.t_id = table.id; row.primary_key = i; row.payer = journal_rows.code;
         const uint64_t value = i * 10; row.value.assign(reinterpret_cast<const char*>(&value), 8);
      });
      const std::array tables{journal_rows}; base = frozen_primary_state::capture(view, tables);
      for (auto& root : roots) {
         root.receiver = root.account = "test"_n; root.action = "run"_n;
         root.authorizations = {{"test"_n, "active"_n}};
      }
      action child; child.account = "bob"_n; child.name = "leaf"_n;
      child.authorization = {{"bob"_n, "active"_n}, {"carol"_n, "owner"_n}}; child.data = {9};
      auto raw = fc::raw::pack(child); raw.push_back('x');
      constexpr char hex[] = "0123456789abcdef"; std::string data;
      for (unsigned char byte : raw) { data += '\\'; data += hex[byte >> 4]; data += hex[byte & 15]; }
      auto root = journal_wasm(R"((module
         (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
         (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
         (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
         (import "env" "send_inline" (func $send (param i32 i32)))
         (import "env" "require_recipient" (func $notify (param i64)))
         (memory 1 1) (data (i32.const 1024) ")" + data + R"(")
         (func (export "apply") (param i64 i64 i64) (local $it i32)
            (set_local $it (call $find )" + i64(journal_rows.code) + " " + i64(journal_rows.scope) + " " +
            i64(journal_rows.table) + R"( (i64.const 1)))
            (drop (call $get (get_local $it) (i32.const 0) (i32.const 8)))
            (i64.store (i32.const 0) (i64.add (i64.load (i32.const 0)) (i64.const 1)))
            (call $update (get_local $it) (i64.const 0) (i32.const 0) (i32.const 8))
            (call $send (i32.const 1024) (i32.const )" + std::to_string(raw.size()) + R"())
            (call $notify )" + i64("carol"_n) + ")))");
      auto leaf = journal_wasm(R"((module
         (import "env" "require_auth2" (func $auth (param i64 i64)))
         (func (export "apply") (param i64 i64 i64) (call $auth )" + i64("bob"_n) + " " + i64("active"_n) + ")))");
      programs = {{"test"_n, std::move(root), {}}, {"bob"_n, std::move(leaf), {}}, {"carol"_n, {}, {}}};
   }
   isolated_action_tree_result execute() {
      isolated_action_tree_executor executor; return executor.execute(base, roots, programs, {});
   }
};
isolated_tree_inline_event& first_send(isolated_action_tree_result& result) {
   return std::get<isolated_tree_inline_event>(result.steps[0].events[2]);
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_action_tree_journal_tests)

BOOST_FIXTURE_TEST_CASE(reconstruction_accepts_exact_tree_but_is_not_authorization_or_current_state_proof, journal_fixture) {
   const auto result = execute();
   for (unsigned repeat = 0; repeat < 3; ++repeat)
      BOOST_CHECK(validate_isolated_action_tree_journal(result, roots, programs, {}));
   BOOST_REQUIRE_EQUAL(result.groups.size(), 4u);
   BOOST_REQUIRE_EQUAL(result.groups[2].input.authorizations.size(), 2u);
   BOOST_CHECK(result.groups[2].input.authorizations[0].first == "bob"_n);
   BOOST_CHECK(result.groups[2].input.authorizations[1].second == "owner"_n);
   BOOST_CHECK(result.state.matches_base_reads(view, [] {}));
   const auto* table = db.find<table_id_object, by_code_scope_table>(boost::make_tuple(journal_rows.code, journal_rows.scope, journal_rows.table));
   BOOST_REQUIRE(table);
   const auto* row = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, uint64_t(1)));
   BOOST_REQUIRE(row);
   db.modify(*row, [](auto& value) { const uint64_t changed = 99; value.value.assign(reinterpret_cast<const char*>(&changed), 8); });
   BOOST_CHECK(validate_isolated_action_tree_journal(result, roots, programs, {})); // frozen proof only
   BOOST_CHECK(!result.state.matches_base_reads(view, [] {}));
   // This fixture has NO permission objects at all. Declared inline authorities
   // are carried faithfully, but successful private execution is NOT authority.
}

BOOST_FIXTURE_TEST_CASE(reconstruction_rejects_corrupt_send_context_child_inputs_and_execution_tree, journal_fixture) {
   const auto good = execute();
   const std::vector<std::function<void(isolated_action_tree_result&)>> corruptions{
      [](auto& r) { r.groups.pop_back(); }, [](auto& r) { r.steps.pop_back(); },
      [](auto& r) { r.groups.push_back(r.groups.back()); }, [](auto& r) { r.steps.push_back(r.steps.back()); },
      [](auto& r) { std::swap(r.steps[1], r.steps[2]); },
      [](auto& r) { ++r.groups[0].root_index; }, [](auto& r) { ++r.groups[0].depth; },
      [](auto& r) { ++r.groups[2].creator_action_ordinal; },
      [](auto& r) { ++r.groups[2].closest_unnotified_ancestor; }, [](auto& r) { ++r.groups[2].action_ordinal; },
      [](auto& r) { r.groups[2].sender = "wrong"_n; }, [](auto& r) { ++r.groups[2].depth; },
      [](auto& r) { r.groups[2].input.account = "wrong"_n; },
      [](auto& r) { r.groups[2].input.receiver = "wrong"_n; },
      [](auto& r) { r.groups[2].input.action = "wrong"_n; },
      [](auto& r) { r.groups[2].input.data[0] += 1; },
      [](auto& r) { r.groups[2].input.authorizations.pop_back(); },
      [](auto& r) { r.groups[2].input.authorizations[0].second = "owner"_n; },
      [](auto& r) { ++r.groups[2].input.max_return_bytes; },
      [](auto& r) { ++r.steps[2].group_index; }, [](auto& r) { ++r.steps[2].action_ordinal; },
      [](auto& r) { ++r.steps[2].creator_action_ordinal; },
      [](auto& r) { ++r.steps[2].closest_unnotified_ancestor; },
      [](auto& r) { r.steps[2].sender = "wrong"_n; },
      [](auto& r) { r.steps[2].code_hash = {}; },
      [](auto& r) { r.steps[2].effects.execution_fingerprint = {}; },
      [](auto& r) { r.steps[0].effects.elapsed = std::chrono::seconds(1); },
      [](auto& r) { r.steps[1].effects.elapsed = std::chrono::microseconds(1); },
      [](auto& r) { r.steps[1].effects.return_value = {'x'}; },
      [](auto& r) { r.steps[0].effects.writes_begin = 1; },
      [](auto& r) { r.steps[0].effects.writes_end = 0; },
      [](auto& r) { r.root_fingerprint = {}; },
      [](auto& r) { ++first_send(r).after_writes; },
      [](auto& r) { ++first_send(r).action_ordinal; },
      [](auto& r) { ++first_send(r).sent.decoded_bytes; },
      [](auto& r) { first_send(r).sent.decoded.data[0] += 1; },
      [](auto& r) { first_send(r).sent.decoded.authorization.clear(); },
      [](auto& r) { first_send(r).sent.packed[0] ^= 1; },
      [](auto& r) { first_send(r).sent.packed.resize(17); },
      [](auto& r) { first_send(r).sent.packed.resize(65537); },
      [](auto& r) { std::swap(r.steps[0].events[1], r.steps[0].events[2]); },
      [](auto& r) { std::swap(r.steps[0].events[2], r.steps[0].events[3]); },
      [](auto& r) { r.steps[0].events.erase(r.steps[0].events.begin()); },
   };
   BOOST_REQUIRE(validate_isolated_action_tree_journal(good, roots, programs, {}));
   for (size_t i = 0; i < corruptions.size(); ++i) BOOST_TEST_CONTEXT("tree corruption " << i) {
      auto invalid = good; corruptions[i](invalid);
      BOOST_CHECK(!validate_isolated_action_tree_journal(invalid, roots, programs, {}));
   }
   BOOST_CHECK(!validate_isolated_action_tree_journal(good, roots, programs, {512 * 1024 - 1, 4}));
   BOOST_CHECK(!validate_isolated_action_tree_journal(good, roots, programs, {512 * 1024, 3}));
   BOOST_CHECK(!validate_isolated_action_tree_journal(good, {}, programs, {}));
   BOOST_CHECK(!validate_isolated_action_tree_journal(good, roots, {}, {}));
   for (size_t i = 0; i < programs.size(); ++i) {
      auto changed = programs; changed[i].serial_only = true;
      BOOST_CHECK(!validate_isolated_action_tree_journal(good, roots, changed, {}));
      changed = programs; changed.erase(changed.begin() + i);
      BOOST_CHECK(!validate_isolated_action_tree_journal(good, roots, changed, {}));
   }
}

BOOST_FIXTURE_TEST_CASE(reconstruction_rejects_forged_write_and_unproved_query_but_propagates_owner_interrupts, journal_fixture) {
   const auto good = execute();
   auto invalid = good;
   primary_state_overlay other(base);
   (void)other.locate_primary({journal_rows, primary_query_kind::find, 1});
   for (const auto& write : good.state.writes()) other.update_existing(journal_rows, 2, write.payer, write.value);
   invalid.state = other.seal();
   BOOST_CHECK(!validate_isolated_action_tree_journal(invalid, roots, programs, {}));
   invalid = good;
   invalid.steps[0].events.emplace_back(isolated_cursor_event{
      isolated_cursor_query{isolated_cursor_query_kind::find, journal_rows, 2, -1, 1}});
   BOOST_CHECK(!validate_isolated_action_tree_journal(invalid, roots, programs, {}));
   invalid = good;
   invalid.steps[0].events.emplace_back(isolated_cursor_event{
      isolated_cursor_query{isolated_cursor_query_kind::find, journal_rows, 1, -1, 0}});
   BOOST_CHECK(validate_isolated_action_tree_journal(invalid, roots, programs, {})); // not a second WASM execution
   for (size_t stop_at : {1u, 10u, 18u, 28u}) {
      size_t calls = 0;
      BOOST_CHECK_THROW(validate_isolated_action_tree_journal(good, roots, programs, {}, [&] {
         if (++calls == stop_at) throw primary_state_invalid_attempt();
      }), primary_state_invalid_attempt);
      BOOST_CHECK_EQUAL(calls, stop_at);
   }
   BOOST_CHECK(good.state.matches_base_reads(view, [] {}));
}

BOOST_AUTO_TEST_SUITE_END()
