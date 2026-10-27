#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_preparation_budget.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/vm/exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/filesystem.hpp>
#include <fc/scoped_exit.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <latch>
#include <limits>

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
      BOOST_CHECK_EQUAL(cache_value(state.primary_write(0).value), 11u + variant);
      BOOST_CHECK_EQUAL(cache_value(returned), 11u + variant);
      BOOST_CHECK(state.matches_base_reads(view, [] {}));
      const auto original = view.read_primary({cache_rows, primary_query_kind::find, 1}, 8);
      BOOST_REQUIRE(original.row);
      BOOST_CHECK_EQUAL(cache_value(original.row->value), 10u);
   }
   void shared_run(unsigned mode, const std::shared_ptr<isolated_module_cache>& cache,
                   const std::vector<char>& code, unsigned variant, isolated_wasm_limits limits = {}, bool fail = false) {
      auto input = root; input.data = {static_cast<char>(fail)};
      const std::array roots{input};
      const std::array catalog{isolated_receiver_program{input.receiver, code, limits}};
      const std::array sync_catalog{isolated_sync_program{catalog[0], false}};
      // Each call destroys the executor. Only the common thread-owned module
      // cache survives: no overlay, cursor, result or input may survive in it.
      switch (mode) {
         case 0: {
            isolated_transaction_executor executor(cache);
            const std::array actions{isolated_action_invocation{code, input, limits}};
            const auto result = executor.execute(base, actions);
            check(result.state, result.actions.front().return_value, variant); break;
         }
         case 1: {
            isolated_notification_executor executor(cache);
            const auto result = executor.execute(base, roots, catalog);
            BOOST_REQUIRE(validate_isolated_notification_journal(result, roots, catalog));
            check(result.state, result.steps.front().effects.return_value, variant); break;
         }
         case 2: {
            isolated_action_tree_executor executor(cache);
            const auto result = executor.execute(base, roots, catalog, {});
            BOOST_REQUIRE(validate_isolated_action_tree_journal(result, roots, catalog, {}));
            check(result.state, result.steps.front().effects.return_value, variant); break;
         }
         case 3: {
            isolated_sync_executor executor(cache);
            const auto result = executor.execute(base, input, sync_catalog, {});
            BOOST_REQUIRE(validate_isolated_sync_journal(result, input, sync_catalog, {}));
            check(result.state, result.root.return_value, variant); break;
         }
         case 4: {
            isolated_sync_executor executor(cache);
            const auto result = executor.execute_transaction(base, roots, sync_catalog, {});
            BOOST_REQUIRE(validate_isolated_sync_transaction_journal(result, roots, sync_catalog, {}));
            check(result.state, result.roots.front().root.return_value, variant); break;
         }
         default: {
            isolated_mixed_tree_executor executor(cache);
            const auto result = executor.execute(base, roots, sync_catalog, {});
            BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result, roots, sync_catalog, {}));
            check(result.state, result.steps.front().effects.return_value, variant); break;
         }
      }
   }
};
struct cache_metrics {
   const bool previous = performance::metrics().enabled.exchange(true, std::memory_order_relaxed);
   const uint64_t initial = performance::metrics().snapshot(performance::stage::parallel_compile).count;
   ~cache_metrics() { performance::metrics().enabled.store(previous, std::memory_order_relaxed); }
   uint64_t compilations() const { return performance::metrics().snapshot(performance::stage::parallel_compile).count - initial; }
};
// Legacy tests intentionally capture only primary tables. Sync/mixed execution
// additionally requires an OWNER-CAPTURED account observation, even when apply
// does not call is_account. Keep the old fixture unchanged and own both bases.
struct shared_cache_fixture : cache_fixture {
   const std::shared_ptr<const frozen_primary_state> uncaptured_account = base;
   shared_cache_fixture() {
      db.add_index<account_index>();
      db.create<account_object>([&](auto& account) { account.name = root.receiver; });
      const std::array tables{cache_rows}; const std::array accounts{root.receiver};
      // Sparse capture does NOT copy every row in a requested table. Own the
      // actual find request too, rather than mistaking table metadata for data.
      const std::array reads{primary_query{cache_rows, primary_query_kind::find, 1}};
      base = frozen_primary_state::capture_queries(view, tables, reads, {}, {}, accounts);
      BOOST_REQUIRE_EQUAL(base->account_count(), 1u);
      BOOST_REQUIRE_EQUAL(base->row_count(), 1u);
      BOOST_REQUIRE(base->read_account(root.receiver));
      const auto row = base->read_primary(reads[0], 8);
      BOOST_REQUIRE(row.row);
      BOOST_REQUIRE_EQUAL(cache_value(row.row->value), 10u);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_module_cache_tests)

BOOST_AUTO_TEST_CASE(preparation_budget_keeps_one_absolute_deadline_and_one_total_round_allowance) {
   static_assert(!std::is_copy_constructible_v<parallel_preparation_budget>);
   static_assert(!std::is_move_constructible_v<parallel_preparation_budget>);
   BOOST_CHECK_THROW(parallel_preparation_budget(isolated_cancel{}), primary_state_invalid_attempt);
   auto generation = std::make_shared<std::atomic<uint64_t>>(7);
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
   parallel_preparation_budget budget({generation, 7, deadline});
   auto use_phase = [&](size_t count) {
      for (size_t i = 0; i < count; ++i) {
         const auto before = budget.remaining_rounds();
         BOOST_REQUIRE(budget.try_begin_round());
         BOOST_CHECK_EQUAL(budget.remaining_rounds(), before - 1);
         BOOST_CHECK(budget.cancellation().deadline == deadline);
         BOOST_CHECK(budget.cancellation().generation == generation);
         BOOST_CHECK_EQUAL(budget.cancellation().observed, 7u);
      }
   };
   use_phase(3); // initial discovery
   use_phase(5); // first hypothetical current-prefix retry
   use_phase(parallel_runtime_limits::max_discovery_rounds - 8); // all remaining retries
   BOOST_CHECK_EQUAL(budget.remaining_rounds(), 0u);
   for (unsigned i = 0; i < 3; ++i) BOOST_CHECK(!budget.try_begin_round());
   BOOST_CHECK(budget.cancellation().deadline == deadline);
   BOOST_CHECK_NO_THROW(budget.cancellation().check());
   generation->fetch_add(1);
   try { budget.cancellation().check(); BOOST_FAIL("generation cancellation was lost"); }
   catch (const isolated_execution_interrupted& error) { BOOST_CHECK(error.reason == isolated_interrupt_reason::cancelled); }
   const auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
   parallel_preparation_budget past({{}, 0, expired});
   try { past.cancellation().check(); BOOST_FAIL("expired deadline was renewed"); }
   catch (const isolated_execution_interrupted& error) { BOOST_CHECK(error.reason == isolated_interrupt_reason::deadline); }
}

BOOST_AUTO_TEST_CASE(preparation_environment_budget_is_shared_subtractive_and_atomic_on_overflow) {
   parallel_preparation_budget budget({{}, 0, std::chrono::steady_clock::now() + std::chrono::hours(1)});
   constexpr size_t cap = parallel_runtime_limits::max_environment_wire_bytes;
   BOOST_CHECK_EQUAL(budget.remaining_environment_wire_bytes(), cap);
   budget.consume_environment_wire_bytes(0);
   budget.consume_environment_wire_bytes(3);
   BOOST_CHECK_EQUAL(budget.remaining_environment_wire_bytes(), cap - 3);
   BOOST_CHECK_THROW(budget.consume_environment_wire_bytes(std::numeric_limits<size_t>::max()), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(budget.remaining_environment_wire_bytes(), cap - 3);
   budget.consume_environment_wire_bytes(cap - 3);
   BOOST_CHECK_EQUAL(budget.remaining_environment_wire_bytes(), 0u);
   BOOST_CHECK_NO_THROW(budget.consume_environment_wire_bytes(0));
   BOOST_CHECK_THROW(budget.consume_environment_wire_bytes(1), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(budget.remaining_environment_wire_bytes(), 0u);
   BOOST_CHECK_EQUAL(budget.remaining_rounds(), parallel_runtime_limits::max_discovery_rounds);
}

BOOST_AUTO_TEST_CASE(preparation_budget_cannot_be_debited_from_a_worker) {
   parallel_preparation_budget budget({{}, 0, std::chrono::steady_clock::now() + std::chrono::hours(1)});
   const auto worker_cancel = budget.cancellation(); // owned copy, no budget pointer
   auto result = std::async(std::launch::async, [&] {
      unsigned rejected = 0;
      try { (void)budget.cancellation(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { (void)budget.remaining_rounds(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { (void)budget.try_begin_round(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { (void)budget.remaining_environment_wire_bytes(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { budget.consume_environment_wire_bytes(1); } catch (const state_view_wrong_thread&) { ++rejected; }
      worker_cancel.check();
      return rejected;
   });
   BOOST_CHECK_EQUAL(result.get(), 5u);
   BOOST_CHECK_EQUAL(budget.remaining_rounds(), parallel_runtime_limits::max_discovery_rounds);
   BOOST_CHECK_EQUAL(budget.remaining_environment_wire_bytes(), parallel_runtime_limits::max_environment_wire_bytes);
}

BOOST_AUTO_TEST_CASE(local_runtime_limits_keep_disabled_mode_and_reject_unsupported_shapes) {
   using bounds = parallel_runtime_limits;
   BOOST_CHECK_EQUAL(bounds::max_workers, 128u);
   for (size_t workers = 0; workers <= bounds::max_workers + 1; ++workers)
      for (size_t batch = 0; batch <= bounds::max_batch_transactions + 1; ++batch)
         BOOST_CHECK_EQUAL(bounds::valid_configuration(workers, batch),
            workers <= bounds::max_workers && batch > 0 && batch <= bounds::max_batch_transactions);
   BOOST_CHECK(!bounds::valid_workers(0));
   BOOST_CHECK(!bounds::valid_workers(size_t(-1)));
   BOOST_CHECK(!bounds::valid_batch(size_t(-1)));
   BOOST_CHECK_THROW(isolated_worker_pool(0), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(isolated_worker_pool(bounds::max_workers + 1), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(isolated_worker_pool(size_t(-1)), primary_state_budget_exceeded);
}

BOOST_FIXTURE_TEST_CASE(shared_cache_reuses_all_modes_and_resets_after_private_write_failure, shared_cache_fixture) {
   for (unsigned first = 0; first < 6; ++first) {
      auto cache = std::make_shared<isolated_module_cache>();
      cache_metrics metrics;
      for (unsigned i = 0; i < 18; ++i) BOOST_TEST_CONTEXT("first=" << first << " transition=" << i) {
         const auto mode = (first + i) % 6;
         shared_run(mode, cache, programs[0], 0);
         BOOST_CHECK_THROW(shared_run(mode, cache, programs[0], 0, {}, true), eosio::vm::exception);
         shared_run((mode + 1) % 6, cache, programs[0], 0);
         BOOST_CHECK_EQUAL(metrics.compilations(), 1u);
      }
      // A cached backend carries NO account capability from previous calls.
      // Removing the observation must still request owner capture, not infer
      // absence or reuse the previous successful account check.
      const auto captured = base;
      for (unsigned mode : {3u, 4u, 5u}) {
         base = uncaptured_account;
         BOOST_CHECK_EXCEPTION(shared_run(mode, cache, programs[0], 0), primary_state_missing,
            [&](const auto& error) {
               const auto* request = std::get_if<account_capture_request>(&error.request);
               return request && request->account == root.receiver;
            });
         base = captured;
         shared_run(mode, cache, programs[0], 0);
         BOOST_CHECK_EQUAL(metrics.compilations(), 1u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(shared_cache_has_one_nine_slot_lru_across_mode_changes_and_failed_compiles, shared_cache_fixture) {
   std::array<std::vector<char>, 10> variants;
   for (unsigned i = 0; i < variants.size(); ++i) variants[i] = cache_program(i);
   auto cache = std::make_shared<isolated_module_cache>(); cache_metrics metrics;
   unsigned mode = 0;
   const auto run = [&](unsigned i) { shared_run(mode++ % 6, cache, variants[i], i); };
   for (unsigned i = 0; i < 9; ++i) { run(i); BOOST_CHECK_EQUAL(metrics.compilations(), i + 1); }
   run(0); BOOST_CHECK_EQUAL(metrics.compilations(), 9u);
   run(9); BOOST_CHECK_EQUAL(metrics.compilations(), 10u);
   run(0); BOOST_CHECK_EQUAL(metrics.compilations(), 10u);
   run(1); BOOST_CHECK_EQUAL(metrics.compilations(), 11u);
   const std::vector<char> invalid{'x', 'a', 's', 'm', 1, 0, 0, 0};
   for (unsigned i = 0; i < 2; ++i) {
      BOOST_CHECK_THROW(shared_run(mode++ % 6, cache, invalid, 0), eosio::vm::wasm_parse_exception);
      BOOST_CHECK_EQUAL(metrics.compilations(), 12u + i);
   }
   run(1); run(0); BOOST_CHECK_EQUAL(metrics.compilations(), 13u);
   run(2); BOOST_CHECK_EQUAL(metrics.compilations(), 14u);
}

BOOST_FIXTURE_TEST_CASE(shared_cache_keys_include_every_limit_across_modes, shared_cache_fixture) {
   for (unsigned field = 0; field < 7; ++field) {
      auto cache = std::make_shared<isolated_module_cache>(); cache_metrics metrics;
      isolated_wasm_limits changed;
      switch (field) {
         case 0: changed.max_pages = 8; break;
         case 1: changed.max_call_depth = 32; break;
         case 2: changed.max_code_bytes = 32 * 1024; break;
         case 3: changed.max_action_bytes = 32 * 1024; break;
         case 4: changed.max_authorizations = 32; break;
         case 5: changed.max_return_bytes = 2048; break;
         case 6: changed.max_attempt_time = std::chrono::milliseconds(400); break;
      }
      for (unsigned mode = 0; mode < 6; ++mode) {
         shared_run(mode, cache, programs[0], 0);
         shared_run((mode + 1) % 6, cache, programs[0], 0, changed);
         BOOST_CHECK_EQUAL(metrics.compilations(), 2u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(deterministic_link_failures_are_cached_across_modes_but_not_code_or_limits, shared_cache_fixture) {
   const auto missing_bytes = wast_to_wasm(R"((module
      (import "env" "parallel_missing_link_probe" (func))
      (func (export "apply") (param i64 i64 i64))))");
   const auto mistyped_bytes = wast_to_wasm(R"((module
      (import "env" "current_receiver" (func (result i32)))
      (func (export "apply") (param i64 i64 i64))))");
   const std::array bad_links{std::vector<char>(missing_bytes.begin(), missing_bytes.end()),
                             std::vector<char>(mistyped_bytes.begin(), mistyped_bytes.end())};
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      if (!isolated_vm_engine_supported(engine)) continue;
      for (unsigned field = 0; field < 7; ++field) BOOST_TEST_CONTEXT("engine=" << isolated_vm_engine_name(engine) << " limit=" << field) {
         auto runtime = std::make_shared<isolated_execution_runtime>(engine, 9);
         auto cache = std::make_shared<isolated_module_cache>(runtime);
         cache_metrics metrics;
         for (unsigned i = 0; i < 18; ++i) {
            for (const auto& code : bad_links)
               BOOST_CHECK_THROW(shared_run(i % 6, cache, code, 0), eosio::vm::wasm_link_exception);
            BOOST_CHECK_EQUAL(metrics.compilations(), 2u);
         }
         // A new code version at the SAME receiver is eligible immediately.
         shared_run(0, cache, programs[0], 0);
         BOOST_CHECK_EQUAL(metrics.compilations(), 3u);
         isolated_wasm_limits changed;
         switch (field) {
            case 0: changed.max_pages = 8; break;
            case 1: changed.max_call_depth = 32; break;
            case 2: changed.max_code_bytes = 32 * 1024; break;
            case 3: changed.max_action_bytes = 32 * 1024; break;
            case 4: changed.max_authorizations = 32; break;
            case 5: changed.max_return_bytes = 2048; break;
            case 6: changed.max_attempt_time = std::chrono::milliseconds(400); break;
         }
         for (unsigned mode = 0; mode < 6; ++mode) {
            BOOST_CHECK_THROW(shared_run(mode, cache, bad_links[0], 0, changed), eosio::vm::wasm_link_exception);
            BOOST_CHECK_THROW(shared_run(mode, cache, bad_links[0], 0), eosio::vm::wasm_link_exception);
            shared_run(mode, cache, programs[0], 0);
            BOOST_CHECK_EQUAL(metrics.compilations(), 4u);
         }
         // Neither a parse failure nor invalid local limits publishes a
         // negative entry. Existing healthy modules still reset fresh state.
         const std::vector<char> malformed{'x', 'a', 's', 'm', 1, 0, 0, 0};
         isolated_wasm_limits invalid; invalid.max_pages = 0;
         for (unsigned i = 0; i < 2; ++i) {
            BOOST_CHECK_THROW(shared_run(i, cache, malformed, 0), eosio::vm::wasm_parse_exception);
            BOOST_CHECK_THROW(shared_run(i, cache, programs[0], 0, invalid), primary_state_budget_exceeded);
            BOOST_CHECK_EQUAL(metrics.compilations(), 6u + 2 * i);
         }
         shared_run(5, cache, programs[0], 0);
         BOOST_CHECK_EQUAL(metrics.compilations(), 8u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(negative_entries_share_the_bounded_nine_slot_lru_and_do_not_poison_execution, shared_cache_fixture) {
   std::array<std::vector<char>, 10> bad_links;
   for (unsigned i = 0; i < bad_links.size(); ++i) {
      const auto bytes = wast_to_wasm("(module (import \"env\" \"parallel_missing_" + std::to_string(i) +
         "\" (func)) (func (export \"apply\") (param i64 i64 i64)))");
      bad_links[i].assign(bytes.begin(), bytes.end());
   }
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      if (!isolated_vm_engine_supported(engine)) continue;
      auto runtime = std::make_shared<isolated_execution_runtime>(engine, 9);
      auto cache = std::make_shared<isolated_module_cache>(runtime);
      cache_metrics metrics;
      unsigned mode = 0;
      const auto reject = [&](unsigned i) {
         BOOST_CHECK_THROW(shared_run(mode++ % 6, cache, bad_links[i], 0), eosio::vm::wasm_link_exception);
      };
      for (unsigned i = 0; i < 9; ++i) reject(i);
      BOOST_CHECK_EQUAL(metrics.compilations(), 9u);
      reject(0); reject(9); reject(0);
      BOOST_CHECK_EQUAL(metrics.compilations(), 10u);
      reject(1); // evicted, NOT an unbounded negative cache
      BOOST_CHECK_EQUAL(metrics.compilations(), 11u);
      shared_run(0, cache, programs[0], 0);
      // A runtime trap is not a module capability failure. The same module
      // remains usable for all modes with new inputs and fresh private state.
      for (unsigned i = 0; i < 6; ++i) {
         BOOST_CHECK_THROW(shared_run(i, cache, programs[0], 0, {}, true), eosio::vm::exception);
         shared_run(i, cache, programs[0], 0);
         reject(1);
         BOOST_CHECK_EQUAL(metrics.compilations(), 12u);
      }
   }
}

BOOST_AUTO_TEST_CASE(shared_cache_rejects_null_and_foreign_thread_before_module_access) {
   const auto construct = [](unsigned mode, const std::shared_ptr<isolated_module_cache>& cache) {
      switch (mode) {
         case 0: { isolated_transaction_executor executor(cache); break; }
         case 1: { isolated_notification_executor executor(cache); break; }
         case 2: { isolated_action_tree_executor executor(cache); break; }
         case 3: { isolated_sync_executor executor(cache); break; }
         default: { isolated_mixed_tree_executor executor(cache); break; }
      }
   };
   auto cache = std::make_shared<isolated_module_cache>();
   for (unsigned mode = 0; mode < 5; ++mode) {
      BOOST_CHECK_THROW(construct(mode, {}), primary_state_invalid_attempt);
      const auto rejected = std::async(std::launch::async, [&, mode] {
         try { construct(mode, cache); } catch (const state_view_wrong_thread&) { return true; }
         return false;
      }).get();
      BOOST_CHECK(rejected);
      BOOST_CHECK_NO_THROW(construct(mode, cache));
   }
}

BOOST_FIXTURE_TEST_CASE(worker_pool_reuses_one_cache_across_all_six_modes_and_failed_batches, shared_cache_fixture) {
   for (size_t workers : {1, 2, 4}) BOOST_TEST_CONTEXT("workers=" << workers) {
      isolated_worker_pool pool(workers); cache_metrics metrics;
      for (unsigned round = 0; round < 12; ++round) {
         const bool fail = round % 3 == 1;
         auto input = root; input.data = {static_cast<char>(fail)};
         const std::vector roots{input};
         const std::vector catalog{isolated_receiver_program{root.receiver, programs[0], {}}};
         const std::vector sync_catalog{isolated_sync_program{catalog[0], false}};
         const std::array action_hashes{isolated_execution_fingerprint(
            fc::sha256::hash(programs[0].data(), programs[0].size()), {}, input)};
         const auto ordinary = isolated_transaction_fingerprint(action_hashes);
         const std::array fingerprints{ordinary, ordinary, isolated_action_tree_fingerprint(action_hashes, {}),
            isolated_sync_fingerprint(input, sync_catalog, {}),
            isolated_sync_transaction_fingerprint(roots, sync_catalog, {}),
            isolated_mixed_tree_fingerprint(roots, sync_catalog, {})};
         std::vector<ordered_attempt_batch::request> requests;
         for (unsigned i = 0; i < 6; ++i)
            requests.push_back({fc::sha256::hash(std::to_string(round) + ":" + std::to_string(i)), fingerprints[(round + i) % 6]});
         ordered_attempt_batch order(base, requests); std::vector<isolated_worker_job> jobs;
         for (unsigned i = 0; i < 6; ++i) switch ((round + i) % 6) {
            case 0: jobs.emplace_back(order.get_ticket(i), programs[0], input, isolated_wasm_limits{}); break;
            case 1: jobs.emplace_back(order.get_ticket(i), roots, catalog); break;
            case 2: jobs.emplace_back(order.get_ticket(i), roots, catalog, isolated_action_tree_context{}); break;
            case 3: jobs.emplace_back(order.get_ticket(i), input, sync_catalog, isolated_sync_context{}); break;
            case 4: jobs.emplace_back(order.get_ticket(i), roots, sync_catalog, isolated_sync_context{}); break;
            case 5: jobs.emplace_back(order.get_ticket(i), isolated_mixed_tree_job{roots, sync_catalog, {}}); break;
         }
         const auto results = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(results.size(), 6u);
         for (unsigned i = 0; i < 6; ++i) {
            const auto& result = results[i];
            if (fail) {
               BOOST_CHECK_MESSAGE(result.failure == isolated_worker_failure::vm_error, isolated_worker_failure_name(result.failure));
               BOOST_CHECK(!result.result && !result.notification_result && !result.tree_result && !result.sync_result &&
                           !result.sync_transaction_result && !result.mixed_result);
               BOOST_CHECK_EQUAL(result.elapsed.count(), 0); continue;
            }
            BOOST_REQUIRE_MESSAGE(result.failure == isolated_worker_failure::none, isolated_worker_failure_name(result.failure));
            switch ((round + i) % 6) {
               case 0: BOOST_REQUIRE(result.result); check(result.result->state, result.result->actions[0].return_value, 0); break;
               case 1: BOOST_REQUIRE(result.notification_result); check(result.notification_result->state, result.notification_result->steps[0].effects.return_value, 0); break;
               case 2: BOOST_REQUIRE(result.tree_result); check(result.tree_result->state, result.tree_result->steps[0].effects.return_value, 0); break;
               case 3: BOOST_REQUIRE(result.sync_result); check(result.sync_result->state, result.sync_result->root.return_value, 0); break;
               case 4: BOOST_REQUIRE(result.sync_transaction_result); check(result.sync_transaction_result->state, result.sync_transaction_result->roots[0].root.return_value, 0); break;
               case 5: BOOST_REQUIRE(result.mixed_result); check(result.mixed_result->state, result.mixed_result->steps[0].effects.return_value, 0); break;
            }
         }
         BOOST_CHECK_GE(metrics.compilations(), 1u); BOOST_CHECK_LE(metrics.compilations(), workers);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(single_job_batches_keep_one_warm_cache_and_fresh_owned_state_across_all_modes, shared_cache_fixture) {
   // A joined one-job discovery round has no inter-transaction parallelism to
   // gain from migration. Count real constructions, not wall time or TPS.
   // First run this on the original scheduler as an explicit negative control.
   for (size_t workers : {1u, 2u, 4u}) BOOST_TEST_CONTEXT("single-job workers=" << workers) {
      isolated_worker_pool pool(workers); cache_metrics metrics;
      for (unsigned round = 0; round < 60; ++round) BOOST_TEST_CONTEXT("round=" << round) {
         const unsigned phase = round < 48 ? (round / 6) % 4 : 0;
         const size_t count = round >= 48 && round % 2 == 0 ? 6 : 1;
         const uint64_t original_value = 1000 + round;
         const auto& row = db.get<key_value_object, by_scope_primary>(
            boost::make_tuple(table_id_object::id_type{0}, uint64_t{1}));
         db.modify(row, [&](auto& changed) {
            changed.value.assign(reinterpret_cast<const char*>(&original_value), sizeof(original_value));
         });
         const std::array tables{cache_rows}; const std::array accounts{root.receiver};
         const std::vector<primary_query> reads = phase == 3 ? std::vector<primary_query>{} :
            std::vector<primary_query>{{cache_rows, primary_query_kind::find, 1}};
         // New snapshot and new ticket EVERY round, including missing-query
         // requests. A warm module never supplies data from an earlier base.
         const auto owned_base = frozen_primary_state::capture_queries(view, tables, reads, {}, {}, accounts);
         auto input = root; input.data = {static_cast<char>(phase == 1)};
         const std::vector roots{input};
         const std::vector catalog{isolated_receiver_program{root.receiver, programs[0], {}}};
         const std::vector sync_catalog{isolated_sync_program{catalog[0], false}};
         const std::array action_hashes{isolated_execution_fingerprint(
            fc::sha256::hash(programs[0].data(), programs[0].size()), {}, input)};
         const auto ordinary = isolated_transaction_fingerprint(action_hashes);
         const std::array fingerprints{ordinary, ordinary, isolated_action_tree_fingerprint(action_hashes, {}),
            isolated_sync_fingerprint(input, sync_catalog, {}),
            isolated_sync_transaction_fingerprint(roots, sync_catalog, {}),
            isolated_mixed_tree_fingerprint(roots, sync_catalog, {})};
         std::vector<ordered_attempt_batch::request> requests;
         for (size_t i = 0; i < count; ++i)
            requests.push_back({fc::sha256::hash(std::to_string(round) + ":" + std::to_string(i)),
                                fingerprints[(round + i) % 6]});
         ordered_attempt_batch order(owned_base, requests); std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < count; ++i) {
            switch ((round + i) % 6) {
               case 0: jobs.emplace_back(order.get_ticket(i), programs[0], input, isolated_wasm_limits{}); break;
               case 1: jobs.emplace_back(order.get_ticket(i), roots, catalog); break;
               case 2: jobs.emplace_back(order.get_ticket(i), roots, catalog, isolated_action_tree_context{}); break;
               case 3: jobs.emplace_back(order.get_ticket(i), input, sync_catalog, isolated_sync_context{}); break;
               case 4: jobs.emplace_back(order.get_ticket(i), roots, sync_catalog, isolated_sync_context{}); break;
               case 5: jobs.emplace_back(order.get_ticket(i), isolated_mixed_tree_job{roots, sync_catalog, {}}); break;
            }
            if (phase == 2) jobs.back().cancel.deadline = std::chrono::steady_clock::now();
         }
         const auto outcomes = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(outcomes.size(), count);
         const auto check_owned = [&](const primary_overlay_result& state, std::span<const char> returned) {
            BOOST_CHECK(state.base().get() == owned_base.get());
            BOOST_REQUIRE_EQUAL(state.writes().size(), 1u);
            BOOST_CHECK_EQUAL(cache_value(state.primary_write(0).value), original_value + 1);
            BOOST_CHECK_EQUAL(cache_value(returned), original_value + 1);
            BOOST_CHECK(state.matches_base_reads(view, [] {}));
         };
         for (size_t i = 0; i < count; ++i) {
            const auto& result = outcomes[i];
            if (phase) {
               const auto expected = phase == 1 ? isolated_worker_failure::vm_error :
                  phase == 2 ? isolated_worker_failure::deadline : isolated_worker_failure::missing_state;
               BOOST_CHECK_MESSAGE(result.failure == expected, isolated_worker_failure_name(result.failure));
               BOOST_CHECK(!result.result && !result.notification_result && !result.tree_result && !result.sync_result &&
                           !result.sync_transaction_result && !result.mixed_result);
               BOOST_CHECK_EQUAL(result.elapsed.count(), 0);
               BOOST_CHECK_EQUAL(bool(result.request), phase == 3);
               BOOST_CHECK(!result.receiver_request);
               if (phase == 3) {
                  BOOST_REQUIRE(result.request);
                  BOOST_REQUIRE(std::holds_alternative<primary_capture_request>(*result.request));
                  BOOST_CHECK(std::get<primary_capture_request>(*result.request).table == cache_rows);
               }
            } else {
               BOOST_REQUIRE_MESSAGE(result.failure == isolated_worker_failure::none, isolated_worker_failure_name(result.failure));
               BOOST_CHECK_EQUAL(unsigned(bool(result.result)) + unsigned(bool(result.notification_result)) +
                  unsigned(bool(result.tree_result)) + unsigned(bool(result.sync_result)) +
                  unsigned(bool(result.sync_transaction_result)) + unsigned(bool(result.mixed_result)), 1u);
               switch ((round + i) % 6) {
                  case 0: BOOST_REQUIRE(result.result); check_owned(result.result->state, result.result->actions[0].return_value); break;
                  case 1: BOOST_REQUIRE(result.notification_result); check_owned(result.notification_result->state, result.notification_result->steps[0].effects.return_value); break;
                  case 2: BOOST_REQUIRE(result.tree_result); check_owned(result.tree_result->state, result.tree_result->steps[0].effects.return_value); break;
                  case 3: BOOST_REQUIRE(result.sync_result); check_owned(result.sync_result->state, result.sync_result->root.return_value); break;
                  case 4: BOOST_REQUIRE(result.sync_transaction_result); check_owned(result.sync_transaction_result->state, result.sync_transaction_result->roots[0].root.return_value); break;
                  case 5: BOOST_REQUIRE(result.mixed_result); check_owned(result.mixed_result->state, result.mixed_result->steps[0].effects.return_value); break;
               }
            }
         }
         const auto original = view.read_primary({cache_rows, primary_query_kind::find, 1}, 8);
         BOOST_REQUIRE(original.row); BOOST_CHECK_EQUAL(cache_value(original.row->value), original_value);
         if (round < 48) BOOST_CHECK_EQUAL(metrics.compilations(), 1u);
         else { BOOST_CHECK_GE(metrics.compilations(), 1u); BOOST_CHECK_LE(metrics.compilations(), workers); }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(sparse_discovery_waves_keep_original_slot_cache_owners, cache_fixture) {
   // Logical pool sizes exercise scheduling even on a smaller test host.
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
   if (!isolated_vm_engine_supported(engine)) continue;
   for (size_t workers : {1u, 2u, 4u, 8u, 12u, 16u, 32u, 64u, 128u}) BOOST_TEST_CONTEXT("engine=" << isolated_vm_engine_name(engine) << " workers=" << workers) {
      isolated_worker_pool pool(workers, engine);
      cache_metrics metrics;
      isolated_wasm_limits limits;
      std::vector<ordered_attempt_batch::request> requests;
      std::vector<size_t> full, sparse;
      for (size_t i = 0; i < workers; ++i) {
         const auto& code = programs[i % programs.size()];
         const std::array hashes{isolated_execution_fingerprint(fc::sha256::hash(code.data(), code.size()), limits, root)};
         const auto id = std::to_string(i);
         requests.push_back({fc::sha256::hash(id.data(), id.size()), isolated_transaction_fingerprint(hashes)});
         full.push_back(i);
         if (i % 2 == 0) sparse.push_back(i);
      }
      ordered_attempt_batch batch(base, requests);
      auto reversed = full; std::reverse(reversed.begin(), reversed.end());
      const std::vector<std::vector<size_t>> waves{full, {workers - 1}, sparse, reversed, full};
      for (const auto& indices : waves) {
         std::vector<isolated_worker_job> jobs;
         for (const auto index : indices)
            jobs.emplace_back(batch.get_ticket(index), programs[index % programs.size()], root, limits);
         const auto outcomes = pool.run(std::move(jobs));
         BOOST_REQUIRE_EQUAL(outcomes.size(), indices.size());
         for (size_t i = 0; i < outcomes.size(); ++i) {
            BOOST_REQUIRE(outcomes[i].result);
            BOOST_CHECK(outcomes[i].failure == isolated_worker_failure::none);
            check(outcomes[i].result->state, outcomes[i].result->actions[0].return_value, indices[i] % programs.size());
         }
         BOOST_CHECK_EQUAL(metrics.compilations(), workers);
      }
   }
   }
}

BOOST_FIXTURE_TEST_CASE(scalable_workers_preserve_all_modes_at_batch_limits_and_after_failures, shared_cache_fixture) {
   using bounds = parallel_runtime_limits;
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
   if (!isolated_vm_engine_supported(engine)) continue;
   for (size_t workers : {4u, 8u, 12u, 16u, 32u, 64u, 128u}) BOOST_TEST_CONTEXT("engine=" << isolated_vm_engine_name(engine) << " workers=" << workers) {
      isolated_worker_pool pool(workers, engine);
      BOOST_CHECK_THROW(pool.run({}), primary_state_budget_exceeded);
      for (size_t count : {size_t{1}, workers, workers + 1, bounds::max_batch_transactions})
         for (unsigned phase = 0; phase < 4; ++phase) BOOST_TEST_CONTEXT("jobs=" << count << " phase=" << phase) {
            auto input = root; input.data = {static_cast<char>(phase == 1)};
            const std::vector roots{input};
            std::vector<ordered_attempt_batch::request> requests;
            for (size_t i = 0; i < count; ++i) {
               const auto& code = programs[i % programs.size()];
               const std::vector catalog{isolated_receiver_program{input.receiver, code, {}}};
               const std::vector sync_catalog{isolated_sync_program{catalog[0], false}};
               const std::array hashes{isolated_execution_fingerprint(fc::sha256::hash(code.data(), code.size()), {}, input)};
               const auto ordinary = isolated_transaction_fingerprint(hashes);
               const std::array fingerprints{ordinary, ordinary, isolated_action_tree_fingerprint(hashes, {}),
                  isolated_sync_fingerprint(input, sync_catalog, {}),
                  isolated_sync_transaction_fingerprint(roots, sync_catalog, {}),
                  isolated_mixed_tree_fingerprint(roots, sync_catalog, {})};
               requests.push_back({fc::sha256::hash(std::to_string(i) + ":" + std::to_string(phase)), fingerprints[i % 6]});
            }
            ordered_attempt_batch order(base, requests);
            if (count == bounds::max_batch_transactions && phase == 0) {
               std::vector<isolated_worker_job> excessive;
               for (size_t i = 0; i <= bounds::max_batch_transactions; ++i)
                  excessive.emplace_back(order.get_ticket(0), programs[0], input, isolated_wasm_limits{});
               BOOST_CHECK_THROW(pool.run(std::move(excessive)), primary_state_budget_exceeded);
            }
            std::vector<isolated_worker_job> jobs;
            for (size_t i = 0; i < count; ++i) {
               const auto& code = programs[i % programs.size()];
               const std::vector catalog{isolated_receiver_program{input.receiver, code, {}}};
               const std::vector sync_catalog{isolated_sync_program{catalog[0], false}};
               switch (i % 6) {
                  case 0: jobs.emplace_back(order.get_ticket(i), code, input, isolated_wasm_limits{}); break;
                  case 1: jobs.emplace_back(order.get_ticket(i), roots, catalog); break;
                  case 2: jobs.emplace_back(order.get_ticket(i), roots, catalog, isolated_action_tree_context{}); break;
                  case 3: jobs.emplace_back(order.get_ticket(i), input, sync_catalog, isolated_sync_context{}); break;
                  case 4: jobs.emplace_back(order.get_ticket(i), roots, sync_catalog, isolated_sync_context{}); break;
                  case 5: jobs.emplace_back(order.get_ticket(i), isolated_mixed_tree_job{roots, sync_catalog, {}}); break;
               }
               if (phase == 2 && i % 2 == 0) jobs.back().cancel.deadline = std::chrono::steady_clock::now();
            }
            const auto outcomes = pool.run(std::move(jobs));
            BOOST_REQUIRE_EQUAL(outcomes.size(), count);
            for (size_t i = 0; i < count; ++i) BOOST_TEST_CONTEXT("slot=" << i) {
               const auto& result = outcomes[i];
               if (phase == 1 || (phase == 2 && i % 2 == 0)) {
                  const auto failure = phase == 1 ? isolated_worker_failure::vm_error : isolated_worker_failure::deadline;
                  BOOST_CHECK_MESSAGE(result.failure == failure, isolated_worker_failure_name(result.failure));
                  BOOST_CHECK(!result.result && !result.notification_result && !result.tree_result && !result.sync_result &&
                     !result.sync_transaction_result && !result.mixed_result);
                  BOOST_CHECK_EQUAL(result.elapsed.count(), 0);
               } else {
                  BOOST_REQUIRE_MESSAGE(result.failure == isolated_worker_failure::none, isolated_worker_failure_name(result.failure));
                  const auto variant = i % programs.size();
                  switch (i % 6) {
                     case 0: BOOST_REQUIRE(result.result); check(result.result->state, result.result->actions[0].return_value, variant); break;
                     case 1: BOOST_REQUIRE(result.notification_result); check(result.notification_result->state, result.notification_result->steps[0].effects.return_value, variant); break;
                     case 2: BOOST_REQUIRE(result.tree_result); check(result.tree_result->state, result.tree_result->steps[0].effects.return_value, variant); break;
                     case 3: BOOST_REQUIRE(result.sync_result); check(result.sync_result->state, result.sync_result->root.return_value, variant); break;
                     case 4: BOOST_REQUIRE(result.sync_transaction_result); check(result.sync_transaction_result->state, result.sync_transaction_result->roots[0].root.return_value, variant); break;
                     case 5: BOOST_REQUIRE(result.mixed_result); check(result.mixed_result->state, result.mixed_result->steps[0].effects.return_value, variant); break;
                  }
               }
            }
            const auto original = view.read_primary({cache_rows, primary_query_kind::find, 1}, 8);
            BOOST_REQUIRE(original.row); BOOST_CHECK_EQUAL(cache_value(original.row->value), 10u);
         }
   }
   }
}

BOOST_FIXTURE_TEST_CASE(small_batches_keep_per_job_caches_and_fresh_state_across_modes_and_queue_sizes, shared_cache_fixture) {
   // A batch no larger than the pool can give EVERY job its own stable worker.
   // Count actual module constructions; never infer TPS from these counters.
   // First run on the old shared queue, preserving all non-cache checks.
   for(size_t workers:{1u,2u,4u}) for(size_t small_count:{2u,3u,4u}) {
      BOOST_TEST_CONTEXT("small cache workers="<<workers<<" jobs="<<small_count) {
         isolated_worker_pool pool(workers); cache_metrics metrics;
         uint64_t returned_warm_count=0;
         for(unsigned round=0;round<54;++round) BOOST_TEST_CONTEXT("round="<<round) {
            const unsigned phase=round/6;
            const size_t count=phase==7?6:small_count;
            const uint64_t original_value=2000+round;
            const auto& row=db.get<key_value_object,by_scope_primary>(
               boost::make_tuple(table_id_object::id_type{0},uint64_t{1}));
            db.modify(row,[&](auto& changed) {
               changed.value.assign(reinterpret_cast<const char*>(&original_value),sizeof(original_value));
            });
            const std::array tables{cache_rows}; const std::array accounts{root.receiver};
            const std::vector<primary_query> reads=phase==3?std::vector<primary_query>{}:
               std::vector<primary_query>{{cache_rows,primary_query_kind::find,1}};
            const auto owned_base=frozen_primary_state::capture_queries(view,tables,reads,{},{},accounts);
            auto input=root; input.data={static_cast<char>(phase==1)};
            isolated_wasm_limits limits;
            if(phase==5) limits.max_call_depth=32;
            const auto variant_for=[&](size_t i) { return unsigned((i+(phase==4 || phase==5?1:0))%programs.size()); };
            const std::vector roots{input};
            std::vector<ordered_attempt_batch::request> requests;
            for(size_t i=0;i<count;++i) {
               const auto& code=programs[variant_for(i)];
               const std::vector catalog{isolated_receiver_program{root.receiver,code,limits}};
               const std::vector sync_catalog{isolated_sync_program{catalog[0],false}};
               const std::array action_hashes{isolated_execution_fingerprint(fc::sha256::hash(code.data(),code.size()),limits,input)};
               const auto ordinary=isolated_transaction_fingerprint(action_hashes);
               const std::array fingerprints{ordinary,ordinary,isolated_action_tree_fingerprint(action_hashes,{}),
                  isolated_sync_fingerprint(input,sync_catalog,{}),
                  isolated_sync_transaction_fingerprint(roots,sync_catalog,{}),
                  isolated_mixed_tree_fingerprint(roots,sync_catalog,{})};
               requests.push_back({fc::sha256::hash(std::to_string(round)+":"+std::to_string(i)),fingerprints[(round+i)%6]});
            }
            ordered_attempt_batch order(owned_base,requests); std::vector<isolated_worker_job> jobs;
            for(size_t i=0;i<count;++i) {
               const auto& code=programs[variant_for(i)];
               const std::vector catalog{isolated_receiver_program{root.receiver,code,limits}};
               const std::vector sync_catalog{isolated_sync_program{catalog[0],false}};
               switch((round+i)%6) {
                  case 0: jobs.emplace_back(order.get_ticket(i),code,input,limits); break;
                  case 1: jobs.emplace_back(order.get_ticket(i),roots,catalog); break;
                  case 2: jobs.emplace_back(order.get_ticket(i),roots,catalog,isolated_action_tree_context{}); break;
                  case 3: jobs.emplace_back(order.get_ticket(i),input,sync_catalog,isolated_sync_context{}); break;
                  case 4: jobs.emplace_back(order.get_ticket(i),roots,sync_catalog,isolated_sync_context{}); break;
                  case 5: jobs.emplace_back(order.get_ticket(i),isolated_mixed_tree_job{roots,sync_catalog,{}}); break;
               }
               if(phase==2) jobs.back().cancel.deadline=std::chrono::steady_clock::now();
            }
            const auto outcomes=pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(outcomes.size(),count);
            for(size_t i=0;i<count;++i) BOOST_TEST_CONTEXT("job="<<i) {
               const auto& result=outcomes[i];
               if(phase>=1 && phase<=3) {
                  const auto expected=phase==1?isolated_worker_failure::vm_error:
                     phase==2?isolated_worker_failure::deadline:isolated_worker_failure::missing_state;
                  BOOST_CHECK_MESSAGE(result.failure==expected,isolated_worker_failure_name(result.failure));
                  BOOST_CHECK(!result.result && !result.notification_result && !result.tree_result && !result.sync_result &&
                              !result.sync_transaction_result && !result.mixed_result);
                  BOOST_CHECK_EQUAL(result.elapsed.count(),0);
                  BOOST_CHECK_EQUAL(bool(result.request),phase==3);
                  BOOST_CHECK(!result.receiver_request);
                  if(phase==3) {
                     BOOST_REQUIRE(result.request);
                     BOOST_REQUIRE(std::holds_alternative<primary_capture_request>(*result.request));
                     BOOST_CHECK(std::get<primary_capture_request>(*result.request).table==cache_rows);
                  }
               } else {
                  BOOST_REQUIRE_MESSAGE(result.failure==isolated_worker_failure::none,isolated_worker_failure_name(result.failure));
                  BOOST_CHECK_EQUAL(unsigned(bool(result.result))+unsigned(bool(result.notification_result))+
                     unsigned(bool(result.tree_result))+unsigned(bool(result.sync_result))+
                     unsigned(bool(result.sync_transaction_result))+unsigned(bool(result.mixed_result)),1u);
                  const auto check_owned=[&](const primary_overlay_result& state,std::span<const char> returned) {
                     BOOST_CHECK(state.base().get()==owned_base.get());
                     BOOST_REQUIRE_EQUAL(state.writes().size(),1u);
                     BOOST_CHECK_EQUAL(cache_value(state.primary_write(0).value),original_value+variant_for(i)+1);
                     BOOST_CHECK_EQUAL(cache_value(returned),original_value+variant_for(i)+1);
                     BOOST_CHECK(state.matches_base_reads(view,[]{}));
                  };
                  switch((round+i)%6) {
                     case 0: BOOST_REQUIRE(result.result); check_owned(result.result->state,result.result->actions[0].return_value); break;
                     case 1: BOOST_REQUIRE(result.notification_result); check_owned(result.notification_result->state,result.notification_result->steps[0].effects.return_value); break;
                     case 2: BOOST_REQUIRE(result.tree_result); check_owned(result.tree_result->state,result.tree_result->steps[0].effects.return_value); break;
                     case 3: BOOST_REQUIRE(result.sync_result); check_owned(result.sync_result->state,result.sync_result->root.return_value); break;
                     case 4: BOOST_REQUIRE(result.sync_transaction_result); check_owned(result.sync_transaction_result->state,result.sync_transaction_result->roots[0].root.return_value); break;
                     case 5: BOOST_REQUIRE(result.mixed_result); check_owned(result.mixed_result->state,result.mixed_result->steps[0].effects.return_value); break;
                  }
               }
            }
            const auto original=view.read_primary({cache_rows,primary_query_kind::find,1},8);
            BOOST_REQUIRE(original.row); BOOST_CHECK_EQUAL(cache_value(original.row->value),original_value);
            const auto compiled=metrics.compilations();
            BOOST_TEST_MESSAGE("SMALL_BATCH_CACHE workers="<<workers<<" jobs="<<small_count<<" active="<<count
               <<" round="<<round<<" compiled="<<compiled);
            if(small_count<=workers && phase<7) {
               // Distinct code per job, then new code, then new limits. A stable
               // owner needs one construction per job at each of these steps.
               BOOST_CHECK_EQUAL(compiled,small_count*(phase<4?1u:phase==4?2u:3u));
            } else if(small_count<=workers && phase==8) {
               // A large shared-queue batch may have changed cache contents.
               // Warm the returning small batch once, then require reuse again.
               if(round%6==0) returned_warm_count=compiled;
               BOOST_CHECK_EQUAL(compiled,returned_warm_count);
            } else BOOST_CHECK_GE(compiled,1u);
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(concurrent_cold_linking_preserves_types_failures_and_private_ownership, cache_fixture) {
   const auto compile = [](const char* source) {
      const auto wasm = wast_to_wasm(source);
      return std::vector<char>(wasm.begin(), wasm.end());
   };
   // Produce bytes on the owner: WAST conversion is NOT what is being made
   // concurrent. Run this case alone in a fresh process to exercise the first
   // private registry registration with four simultaneous callers as well.
   const std::array bad_links{
      compile(R"((module (import "env" "parallel_missing_link_probe" (func))
         (func (export "apply") (param i64 i64 i64))))"),
      compile(R"((module (import "env" "current_receiver" (func (result i32)))
         (func (export "apply") (param i64 i64 i64))))")};
   const std::vector<char> truncated{'\0', 'a', 's', 'm'};
   const std::vector<char> invalid_magic{'x', 'a', 's', 'm', 1, 0, 0, 0};
   struct observation {
      unsigned variant;
      isolated_action_result result;
   };
   struct thread_result {
      unsigned rejected_links = 0, rejected_parses = 0, rejected_bounds = 0;
      unsigned trapped_executions = 0, accepted_invalid = 0;
      std::vector<observation> successes;
      std::exception_ptr unexpected;
   };
   // Four FIRST, so the standalone test includes concurrent call_once entry.
   for (unsigned threads : {4u, 2u, 1u}) BOOST_TEST_CONTEXT("cold compile workers=" << threads) {
      std::latch start{1};
      std::vector<std::future<thread_result>> pending;
      pending.reserve(threads); // push_back must not throw while a temporary future waits at start
      bool released = false;
      // If launching a thread throws, unblock already-created futures BEFORE
      // their destructors join. Never strand a worker at a reusable barrier.
      auto release_on_failure = fc::make_scoped_exit([&] { if (!released) start.count_down(); });
      for (unsigned worker = 0; worker < threads; ++worker)
         pending.push_back(std::async(std::launch::async, [&, worker] {
            thread_result result;
            start.wait();
            try {
               for (unsigned round = 0; round < 3; ++round) {
                  for (const auto& bytes : bad_links) {
                     try { isolated_wasm_module rejected(bytes); ++result.accepted_invalid; }
                     catch (const eosio::vm::wasm_link_exception&) { ++result.rejected_links; }
                  }
                  try { isolated_wasm_module rejected(truncated); ++result.accepted_invalid; }
                  catch (const eosio::vm::guarded_ptr_exception&) { ++result.rejected_bounds; }
                  try { isolated_wasm_module rejected(invalid_magic); ++result.accepted_invalid; }
                  catch (const eosio::vm::wasm_parse_exception&) { ++result.rejected_parses; }
                  // Same code in round zero, different code in later rounds;
                  // each module is created, used and destroyed on its owner.
                  const unsigned variant = round ? (worker + round) % programs.size() : 0;
                  isolated_wasm_module module(programs[variant]);
                  result.successes.push_back({variant, module.execute(base, root)});
                  auto failing = root; failing.data = {1};
                  try { (void)module.execute(base, failing); ++result.accepted_invalid; }
                  catch (const eosio::vm::exception&) { ++result.trapped_executions; }
                  result.successes.push_back({variant, module.execute(base, root)});
               }
            } catch (...) { result.unexpected = std::current_exception(); }
            return result;
         }));
      released = true;
      start.count_down();
      // All Boost assertions and original database reads remain on the owner.
      // This is a concurrency/correctness test, NOT a timing or TPS assertion.
      for (auto& future : pending) {
         const auto result = future.get();
         if (result.unexpected) std::rethrow_exception(result.unexpected);
         BOOST_CHECK_EQUAL(result.rejected_links, 6u);
         BOOST_CHECK_EQUAL(result.rejected_parses, 3u);
         BOOST_CHECK_EQUAL(result.rejected_bounds, 3u);
         BOOST_CHECK_EQUAL(result.trapped_executions, 3u);
         BOOST_CHECK_EQUAL(result.accepted_invalid, 0u);
         BOOST_REQUIRE_EQUAL(result.successes.size(), 6u);
         for (const auto& success : result.successes)
            check(success.result.state, success.result.return_value, success.variant);
      }
   }
}

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


BOOST_AUTO_TEST_SUITE(owner_pipeline_tests)
BOOST_FIXTURE_TEST_CASE(proof_is_immutable_and_bound_to_input_result_and_preparation,shared_cache_fixture) {
   static_assert(!std::is_copy_constructible_v<isolated_prevalidated_tree>);
   static_assert(!std::is_default_constructible_v<isolated_prevalidated_tree>);
   for(auto engine:{isolated_vm_engine::interpreter,isolated_vm_engine::jit}) {
      if(!isolated_vm_engine_supported(engine))continue;
      isolated_worker_pool pool(2,engine);
      const std::vector roots{root};
      const std::vector catalog{isolated_receiver_program{root.receiver,programs[0],{}}};
      const std::array hashes{isolated_execution_fingerprint(fc::sha256::hash(programs[0].data(),programs[0].size()),{},root)};
      const std::array requests{ordered_attempt_batch::request{fc::sha256::hash("proof"),isolated_action_tree_fingerprint(hashes,{})}};
      ordered_attempt_batch order(base,requests),foreign(base,requests);
      auto tag=order.get_ticket(0);
      auto job=isolated_worker_job(tag,roots,catalog,isolated_action_tree_context{});job.prevalidate_tree=true;
      auto results=pool.run({std::move(job)});
      BOOST_REQUIRE_EQUAL(results.size(),1u);const auto& outcome=results[0];
      BOOST_REQUIRE(outcome.failure==isolated_worker_failure::none);
      BOOST_REQUIRE(outcome.prevalidated_tree);BOOST_CHECK(!outcome.tree_result);
      BOOST_REQUIRE(outcome.tree_attempt());const auto& result=*outcome.tree_attempt();
      const auto& proof=*outcome.prevalidated_tree;
      BOOST_CHECK(proof.belongs_to(tag));BOOST_CHECK(!proof.belongs_to(foreign.get_ticket(0)));
      BOOST_CHECK(proof.cpu().count()>=0);
      BOOST_CHECK(validate_isolated_action_tree_journal(result,roots,catalog,{}));
      BOOST_CHECK(proof.matches(result,roots,catalog,{},{}));
      auto copy=result;BOOST_CHECK(!proof.matches(copy,roots,catalog,{},{}));
      for(unsigned mutation=0;mutation<6;++mutation) {
         auto altered=roots;
         switch(mutation) {
            case 0:altered[0].data.push_back(1);break;
            case 1:altered[0].authorizations.emplace_back("test"_n,"active"_n);break;
            case 2:++altered[0].max_return_bytes;break;
            case 3:altered[0].action="other"_n;break;
            case 4:altered[0].receiver="other"_n;break;
            default:altered[0].account="other"_n;break;
         }
         BOOST_CHECK(!proof.matches(result,altered,catalog,{},{}));
      }
      for(unsigned mutation=0;mutation<4;++mutation) {
         auto altered=catalog;
         switch(mutation) {
            case 0:altered[0].code=programs[1];break;
            case 1:++altered[0].limits.max_pages;break;
            case 2:altered[0].serial_only=true;break;
            default:altered.push_back(altered[0]);break;
         }
         BOOST_CHECK(!proof.matches(result,roots,altered,{},{}));
      }
      for(unsigned mutation=0;mutation<4;++mutation) {
         isolated_action_tree_context context;
         if(mutation==0)++context.max_inline_action_size;
         if(mutation==1)++context.max_inline_action_depth;
         if(mutation==2)context.console_enabled=!context.console_enabled;
         if(mutation==3)context.context_free_data=isolated_context_free_data::capture(std::vector<std::vector<char>>{{'x'}});
         BOOST_CHECK(!proof.matches(result,roots,catalog,context,{}));
      }
      check(result.state,result.steps.front().effects.return_value,0);
      const std::array changes{ordered_attempt_batch::retry_request{0,requests[0]}};
      order.prepare_retries(base,changes);
      BOOST_CHECK(!proof.belongs_to(order.get_ticket(0)));BOOST_CHECK(proof.belongs_to(tag));
   }
}

BOOST_FIXTURE_TEST_CASE(streamed_publication_overlaps_owned_capture_and_joins_factory_failures,shared_cache_fixture) {
   for(auto engine:{isolated_vm_engine::interpreter,isolated_vm_engine::jit}) {
      if(!isolated_vm_engine_supported(engine))continue;
      isolated_worker_pool pool(8,engine);
      const std::vector roots{root};
      const std::vector catalog{isolated_receiver_program{root.receiver,programs[0],{}}};
      const std::array hashes{isolated_execution_fingerprint(fc::sha256::hash(programs[0].data(),programs[0].size()),{},root)};
      std::vector<ordered_attempt_batch::request> requests;
      for(size_t i=0;i<24;++i)requests.push_back({fc::sha256::hash(std::to_string(i)),isolated_action_tree_fingerprint(hashes,{})});
      ordered_attempt_batch order(base,requests),foreign(base,requests);
      std::vector<ordered_attempt_batch::ticket> tags;for(size_t i=0;i<24;++i)tags.push_back(order.get_ticket(i));
      const auto owner=std::this_thread::get_id();bool overlapped=false,reentrant_rejected=false;
      auto make=[&](size_t i) {
         BOOST_REQUIRE(std::this_thread::get_id()==owner);
         if(i==8) {
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(pool.active_started_jobs()<8 && std::chrono::steady_clock::now()<until)std::this_thread::yield();
            overlapped=pool.active_started_jobs()>=8;BOOST_REQUIRE(overlapped);
            // Owner may capture canonical state while old workers hold copies.
            const std::array keys{cache_rows};const auto next=frozen_primary_state::capture(view,keys);
            BOOST_REQUIRE(next);BOOST_CHECK_EQUAL(cache_value(view.read_primary({cache_rows,primary_query_kind::find,1},8).row->value),10u);
            std::vector<ordered_attempt_batch::retry_request> pending;
            for(size_t k=i;k<tags.size();++k)pending.push_back({k,requests[k]});
            order.prepare_retries(next,pending);
            for(size_t k=i;k<tags.size();++k)tags[k]=order.get_ticket(k);
            try {pool.run({isolated_worker_job(tags[0],roots,catalog,isolated_action_tree_context{})});}
            catch(const primary_state_invalid_attempt&){reentrant_rejected=true;}
         }
         auto job=isolated_worker_job(tags[i],roots,catalog,isolated_action_tree_context{});job.prevalidate_tree=true;return job;
      };
      const auto results=pool.run_streamed(tags,make);
      BOOST_CHECK(overlapped);BOOST_CHECK(reentrant_rejected);BOOST_CHECK_EQUAL(pool.active_started_jobs(),0u);
      BOOST_REQUIRE_EQUAL(results.size(),24u);
      for(size_t i=0;i<results.size();++i) {
         BOOST_REQUIRE(results[i].prevalidated_tree);BOOST_REQUIRE(results[i].tree_attempt());
         BOOST_CHECK(results[i].prevalidated_tree->belongs_to(tags[i]));
         BOOST_CHECK(validate_isolated_action_tree_journal(*results[i].tree_attempt(),roots,catalog,{}));
         check(results[i].tree_attempt()->state,results[i].tree_attempt()->steps.front().effects.return_value,0);
      }
      bool original_failure=false;
      try {pool.run_streamed(tags,[&](size_t i) {
         if(i==8)throw std::runtime_error("factory-test-owned-failure");
         return isolated_worker_job(tags[i],roots,catalog,isolated_action_tree_context{});
      });}catch(const std::runtime_error& error){original_failure=std::string(error.what())=="factory-test-owned-failure";}
      BOOST_CHECK(original_failure);BOOST_CHECK_EQUAL(pool.active_started_jobs(),0u);
      BOOST_CHECK_THROW(pool.run_streamed(tags,[&](size_t){return isolated_worker_job(foreign.get_ticket(0),roots,catalog,isolated_action_tree_context{});}),primary_state_invalid_attempt);
      auto again=pool.run({isolated_worker_job(tags[0],roots,catalog,isolated_action_tree_context{})});
      BOOST_REQUIRE(again.front().tree_result);BOOST_CHECK(!again.front().prevalidated_tree);
   }
}

BOOST_FIXTURE_TEST_CASE(streamed_tail_sizes_cancellation_and_deadline_clear_proofs,shared_cache_fixture) {
   const std::vector roots{root};const std::vector catalog{isolated_receiver_program{root.receiver,programs[0],{}}};
   const std::array hashes{isolated_execution_fingerprint(fc::sha256::hash(programs[0].data(),programs[0].size()),{},root)};
   for(size_t workers:{1u,2u,8u})for(size_t count:{1u,2u,8u,9u,24u}) {
      isolated_worker_pool pool(workers,isolated_vm_engine::interpreter);
      std::vector<ordered_attempt_batch::request> requests;
      for(size_t i=0;i<count;++i)requests.push_back({fc::sha256::hash(std::to_string(i)),isolated_action_tree_fingerprint(hashes,{})});
      ordered_attempt_batch order(base,requests);std::vector<ordered_attempt_batch::ticket> tags;
      for(size_t i=0;i<count;++i)tags.push_back(order.get_ticket(i));
      for(unsigned phase=0;phase<3;++phase) {
         auto generation=std::make_shared<std::atomic<uint64_t>>(1);
         isolated_cancel cancel;
         if(phase==1)cancel={generation,0};
         if(phase==2)cancel={{},0,std::chrono::steady_clock::now()-std::chrono::seconds(1)};
         const auto results=pool.run_streamed(tags,[&](size_t i){
            auto job=isolated_worker_job(tags[i],roots,catalog,isolated_action_tree_context{},cancel);job.prevalidate_tree=true;return job;
         });
         BOOST_REQUIRE_EQUAL(results.size(),count);
         for(const auto& outcome:results) {
            if(phase==0){BOOST_REQUIRE(outcome.prevalidated_tree);BOOST_REQUIRE(outcome.tree_attempt());}
            else {BOOST_CHECK(!outcome.prevalidated_tree);BOOST_CHECK(!outcome.tree_attempt());
               BOOST_CHECK(outcome.failure==(phase==1?isolated_worker_failure::cancelled:isolated_worker_failure::deadline));}
         }
      }
   }
}
BOOST_AUTO_TEST_SUITE_END()
