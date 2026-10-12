#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/vm/exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/filesystem.hpp>
#include <array>
#include <cstring>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key cache_rows{"test"_n, "test"_n, "rows"_n};
std::string cache_name(name value) { return "(i64.const " + std::to_string(value.to_uint64_t()) + ")"; }
std::vector<char> cache_program(unsigned variant) {
   const auto bytes = wast_to_wasm(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32)(result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64)(result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32)(result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1) (global $used (mut i64) (i64.const 0))
      (func (export "apply") (param i64 i64 i64) (local $it i32)
         (if (i64.ne (get_global $used) (i64.const 0)) (then unreachable))
         (if (i64.ne (i64.load (i32.const 32)) (i64.const 0)) (then unreachable))
         (set_global $used (i64.const 1)) (i64.store (i32.const 32) (i64.const 1))
         (drop (call $read (i32.const 0)(i32.const 1)))
         (set_local $it (call $find )" + cache_name(cache_rows.code) + cache_name(cache_rows.scope) +
         cache_name(cache_rows.table) + R"((i64.const 1)))
         (drop (call $get (get_local $it)(i32.const 64)(i32.const 8)))
         (i64.store (i32.const 64) (i64.add (i64.load (i32.const 64))(i64.const )" +
         std::to_string(variant + 1) + R"()))
         (call $update (get_local $it)(i64.const 0)(i32.const 64)(i32.const 8))
         (if (i32.load8_u (i32.const 0)) (then unreachable))
         (call $return (i32.const 64)(i32.const 8)))))");
   return {bytes.begin(), bytes.end()};
}
uint64_t cache_value(std::span<const char> bytes) {
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   uint64_t value; std::memcpy(&value, bytes.data(), 8); return value;
}
struct cache_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   std::array<std::vector<char>, 5> programs;
   isolated_action_input root;
   cache_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      const auto& table = db.create<table_id_object>([](auto& row) {
         row.code = cache_rows.code; row.scope = cache_rows.scope; row.table = cache_rows.table;
         row.payer = cache_rows.code; row.count = 1;
      });
      db.create<key_value_object>([&](auto& row) {
         row.t_id = table.id; row.primary_key = 1; row.payer = cache_rows.code;
         const uint64_t value = 10; row.value.assign(reinterpret_cast<const char*>(&value), 8);
      });
      const std::array keys{cache_rows}; base = frozen_primary_state::capture(view, keys);
      for (unsigned i = 0; i < programs.size(); ++i) programs[i] = cache_program(i);
      root.receiver = root.account = cache_rows.code; root.action = "run"_n; root.data = {0};
   }
   void check(const primary_overlay_result& state, std::span<const char> returned, unsigned variant) {
      BOOST_REQUIRE_EQUAL(state.writes().size(), 1u);
      BOOST_CHECK_EQUAL(cache_value(state.writes().front().value), 11u + variant);
      BOOST_CHECK_EQUAL(cache_value(returned), 11u + variant);
      BOOST_CHECK(state.matches_base_reads(view, [] {}));
      const auto original = view.read_primary({cache_rows, primary_query_kind::find, 1}, 8);
      BOOST_REQUIRE(original.row);
      BOOST_CHECK_EQUAL(cache_value(original.row->value), 10u);
   }
};
struct cache_metrics {
   const bool previous = performance::metrics().enabled.exchange(true, std::memory_order_relaxed);
   const uint64_t initial = performance::metrics().snapshot(performance::stage::parallel_compile).count;
   ~cache_metrics() { performance::metrics().enabled.store(previous, std::memory_order_relaxed); }
   uint64_t compilations() const { return performance::metrics().snapshot(performance::stage::parallel_compile).count - initial; }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_module_cache_tests)

BOOST_FIXTURE_TEST_CASE(four_entry_lru_is_code_and_limits_bound_and_never_reuses_state, cache_fixture) {
   for (unsigned mode = 0; mode < 3; ++mode) {
      BOOST_TEST_CONTEXT("executor mode=" << mode) {
         isolated_transaction_executor transaction;
         isolated_notification_executor notification;
         isolated_action_tree_executor tree;
         cache_metrics metrics;
         const auto run = [&](unsigned index, isolated_wasm_limits limits = {}) {
            const std::array calls{isolated_action_invocation{programs[index], root, limits}};
            const std::array roots{root}; const std::array catalog{isolated_receiver_program{root.receiver, programs[index], limits}};
            if (mode == 0) {
               const auto result = transaction.execute(base, calls);
               check(result.state, result.actions.front().return_value, index);
            } else if (mode == 1) {
               const auto result = notification.execute(base, roots, catalog);
               check(result.state, result.steps.front().effects.return_value, index);
            } else {
               const auto result = tree.execute(base, roots, catalog, {});
               check(result.state, result.steps.front().effects.return_value, index);
            }
         };
         // Touch A before E: B must be evicted, while A remains usable. A cache
         // larger than four or a cache keyed only by receiver also fails here.
         const std::array sequence{0u, 1u, 2u, 3u, 0u, 4u, 1u};
         const std::array expected{1u, 2u, 3u, 4u, 4u, 5u, 6u};
         for (size_t i = 0; i < sequence.size(); ++i) {
            run(sequence[i]); BOOST_CHECK_EQUAL(metrics.compilations(), expected[i]);
         }
         isolated_wasm_limits changed; changed.max_call_depth = 32;
         run(0, changed); BOOST_CHECK_EQUAL(metrics.compilations(), 7u);
         run(0); BOOST_CHECK_EQUAL(metrics.compilations(), 7u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(cached_failures_and_invalid_limits_do_not_escape_or_poison_reuse, cache_fixture) {
   for (unsigned mode = 0; mode < 3; ++mode) {
      BOOST_TEST_CONTEXT("failed cached executor mode=" << mode) {
         isolated_transaction_executor transaction;
         isolated_notification_executor notification;
         isolated_action_tree_executor tree;
         const auto run = [&](unsigned index, bool fail, isolated_wasm_limits limits = {}) {
            auto arguments = root; arguments.data = {static_cast<char>(fail)};
            const std::array calls{isolated_action_invocation{programs[index], arguments, limits}};
            const std::array roots{arguments}; const std::array catalog{isolated_receiver_program{root.receiver, programs[index], limits}};
            if (mode == 0) {
               const auto result = transaction.execute(base, calls);
               check(result.state, result.actions.front().return_value, index);
            } else if (mode == 1) {
               const auto result = notification.execute(base, roots, catalog);
               check(result.state, result.steps.front().effects.return_value, index);
            } else {
               const auto result = tree.execute(base, roots, catalog, {});
               check(result.state, result.steps.front().effects.return_value, index);
            }
         };
         for (unsigned round = 0; round < 3; ++round) {
            run(0, false);
            BOOST_CHECK_THROW(run(0, true), eosio::vm::exception); // after a private write
            run(1, false); run(0, false); // memory/globals/cursors reset even after another module
            isolated_wasm_limits invalid; invalid.max_pages = 0;
            BOOST_CHECK_THROW(run(0, false, invalid), primary_state_budget_exceeded);
            run(0, false);
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
