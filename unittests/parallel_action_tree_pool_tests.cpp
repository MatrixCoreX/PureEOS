#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include "parallel_test_diagnostics.hpp"

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key pool_rows{"test"_n, "test"_n, "rows"_n};
std::string pool_name(name n) { return "(i64.const " + std::to_string(n.to_uint64_t()) + ")"; }
std::vector<char> pool_code(unsigned mode, bool trap = false) {
   action act; act.account = "bob"_n; act.name = "child"_n;
   const auto packed = fc::raw::pack(act);
   const char hex[] = "0123456789abcdef"; std::string bytes;
   for (unsigned char c : packed) { bytes += '\\'; bytes += hex[c >> 4]; bytes += hex[c & 15]; }
   std::string tail;
   if (mode != 3) tail += R"((i64.store (i32.const 0) (i64.add (i64.load (i32.const 0)) (i64.const 1)))
      (call $update (get_local $it) (i64.const 0) (i32.const 0) (i32.const 8)))";
   if (mode == 1 || mode == 2) tail += "(call $notify " + pool_name("bob"_n) + ")";
   if (mode == 2) tail += "(call $inline (i32.const 128) (i32.const " + std::to_string(packed.size()) + "))";
   if (mode == 1 || mode == 2) tail += "(call $notify " + pool_name("blank"_n) + ")";
   if (trap) tail += "unreachable";
   const auto wasm = wast_to_wasm(R"((module
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "send_inline" (func $inline (param i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1) (data (i32.const 128) ")" + bytes + R"(")
      (func (export "apply") (param i64 i64 i64) (local $it i32) (local $counter i32)
         (set_local $it (call $find )" + pool_name(pool_rows.code) + " " + pool_name(pool_rows.scope) + " " +
         pool_name(pool_rows.table) + R"( (i64.const 1)))
         (drop (call $get (get_local $it) (i32.const 0) (i32.const 8)))
         (set_local $counter (i32.const 50000))
         (block $done (loop $again (br_if $done (i32.eqz (get_local $counter)))
            (set_local $counter (i32.sub (get_local $counter) (i32.const 1))) (br $again)))
         )" + tail + R"((call $return (i32.const 0) (i32.const 8)))))");
   return {wasm.begin(), wasm.end()};
}
struct tree_pool_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   const std::vector<char> legacy = pool_code(0), notification = pool_code(1), tree_code = pool_code(2), child = pool_code(3);
   isolated_action_input root;
   const isolated_action_tree_context tree_context{};
   tree_pool_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      const auto& table = db.create<table_id_object>([](auto& row) {
         row.code = pool_rows.code; row.scope = pool_rows.scope; row.table = pool_rows.table;
         row.payer = pool_rows.code; row.count = 1;
      });
      db.create<key_value_object>([&](auto& row) {
         row.t_id = table.id; row.primary_key = 1; row.payer = pool_rows.code;
         const uint64_t value = 10; row.value.assign(reinterpret_cast<const char*>(&value), 8);
      });
      const std::array tables{pool_rows}; base = frozen_primary_state::capture(view, tables);
      root.receiver = root.account = "test"_n; root.action = "run"_n;
   }
   std::vector<isolated_receiver_program> programs(bool tree) const {
      return {{"test"_n, tree ? tree_code : notification, {}}, {"bob"_n, child, {}}, {"blank"_n, {}, {}}};
   }
   uint64_t canonical() const {
      const auto& row = db.get<key_value_object, by_scope_primary>(boost::make_tuple(table_id_object::id_type{0}, uint64_t{1}));
      uint64_t value; std::memcpy(&value, row.value.data(), 8); return value;
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_action_tree_pool_tests)

BOOST_FIXTURE_TEST_CASE(three_exclusive_modes_keep_owned_results_and_one_thread_local_cache, tree_pool_fixture) {
   const std::vector roots{root, root};
   const auto tree_programs = programs(true), notification_programs = programs(false);
   isolated_action_tree_executor tree_reference; isolated_notification_executor notify_reference;
   isolated_transaction_executor legacy_reference;
   const auto expected_tree = tree_reference.execute(base, roots, tree_programs, tree_context);
   const auto expected_notify = notify_reference.execute(base, roots, notification_programs);
   const std::vector<isolated_action_invocation> calls{{legacy, root, {}}};
   const auto expected_legacy = legacy_reference.execute(base, calls);
   for (size_t threads : {1u, 2u, 4u}) {
      isolated_worker_pool pool(threads);
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<ordered_attempt_batch::request> requests;
         for (size_t i = 0; i < 9; ++i) requests.push_back({fc::sha256::hash(std::to_string(i)),
            i % 3 == 0 ? expected_legacy.execution_fingerprint : i % 3 == 1 ? expected_notify.root_fingerprint : expected_tree.root_fingerprint});
         ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < requests.size(); ++i) {
            if (i % 3 == 0) jobs.emplace_back(batch.get_ticket(i), calls);
            else if (i % 3 == 1) jobs.emplace_back(batch.get_ticket(i), roots, notification_programs);
            else jobs.emplace_back(batch.get_ticket(i), roots, tree_programs, tree_context);
         }
         const auto outputs = parallel_test_measure("tree three modes", [&] { return pool.run(std::move(jobs)); });
         BOOST_REQUIRE_EQUAL(outputs.size(), requests.size());
         for (size_t i = 0; i < outputs.size(); ++i) {
            const auto& out = outputs[i];
            BOOST_CHECK_EQUAL(bool(out.result), i % 3 == 0); BOOST_CHECK_EQUAL(bool(out.notification_result), i % 3 == 1);
            BOOST_CHECK_MESSAGE(bool(out.tree_result) == (i % 3 == 2),
               "threads=" << threads << " round=" << round << " job=" << i << " failure=" <<
               isolated_worker_failure_name(out.failure) << " attempt_wall_us=" << out.attempt_wall.count());
            BOOST_CHECK(!out.request); BOOST_CHECK(!out.receiver_request);
            if (i % 3 != 2) continue;
            BOOST_REQUIRE_MESSAGE(out.tree_result, isolated_worker_failure_name(out.failure) <<
               " attempt_wall_us=" << out.attempt_wall.count());
            BOOST_CHECK(out.failure == isolated_worker_failure::none);
            BOOST_CHECK(out.attempt_wall >= out.elapsed);
            BOOST_CHECK(validate_isolated_action_tree_journal(*out.tree_result, roots, tree_programs, tree_context));
            BOOST_CHECK(out.tree_result->root_fingerprint == requests[i].execution_fingerprint);
            std::chrono::microseconds elapsed{}; std::chrono::steady_clock::time_point last;
            for (const auto& step : out.tree_result->steps) if (step.has_code) { elapsed += step.effects.elapsed; last = step.effects.ended; }
            BOOST_CHECK(out.elapsed == elapsed); BOOST_CHECK(out.ended == last);
            BOOST_CHECK(out.began == out.tree_result->steps.front().effects.began);
            BOOST_CHECK(out.elapsed <= out.ended - out.began);
         }
         BOOST_CHECK_EQUAL(canonical(), 10u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(tree_requests_cancellation_and_deep_failures_never_publish_a_prefix, tree_pool_fixture) {
   isolated_worker_pool pool(2); const std::vector roots{root}; const auto catalog = programs(true);
   isolated_action_tree_executor reference;
   const auto expected = reference.execute(base, roots, catalog, tree_context);
   const std::array<ordered_attempt_batch::request, 1> requests{{{fc::sha256::hash("tree"), expected.root_fingerprint}}};
   ordered_attempt_batch batch(base, requests); const isolated_worker_job good{batch.get_ticket(0), roots, catalog, tree_context};
   for (unsigned failure = 0; failure < 5; ++failure) {
      auto bad = good;
      if (failure == 0) bad.tree->programs.erase(bad.tree->programs.begin() + 1);
      if (failure == 1) bad.tree->programs[1].serial_only = true;
      if (failure == 2) bad.tree->programs[1].code = pool_code(3, true);
      if (failure == 3) bad.tree->context.max_inline_action_depth = 0;
      if (failure == 4) bad.cancel.deadline = std::chrono::steady_clock::now();
      const auto outcomes = pool.run({bad, good});
      BOOST_CHECK(!outcomes[0].result); BOOST_CHECK(!outcomes[0].notification_result); BOOST_CHECK(!outcomes[0].tree_result);
      BOOST_CHECK(!outcomes[0].request); BOOST_CHECK_EQUAL(bool(outcomes[0].receiver_request), failure == 0);
      if (failure == 0) BOOST_CHECK(*outcomes[0].receiver_request == "bob"_n);
      BOOST_CHECK_EQUAL(outcomes[0].elapsed.count(), 0);
      const std::array reasons{isolated_worker_failure::missing_receiver, isolated_worker_failure::unsupported,
         isolated_worker_failure::vm_error, isolated_worker_failure::unsupported, isolated_worker_failure::deadline};
      BOOST_CHECK_MESSAGE(outcomes[0].failure == reasons[failure], isolated_worker_failure_name(outcomes[0].failure));
      BOOST_CHECK(outcomes[1].failure == isolated_worker_failure::none);
      BOOST_REQUIRE(outcomes[1].tree_result); BOOST_CHECK(validate_isolated_action_tree_journal(*outcomes[1].tree_result, roots, catalog, tree_context));
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
   const auto sparse = frozen_primary_state::capture_queries(view, {}, {});
   ordered_attempt_batch missing_batch(sparse, requests);
   const auto missing = pool.run({{missing_batch.get_ticket(0), roots, catalog, tree_context}});
   BOOST_CHECK(!missing[0].tree_result); BOOST_CHECK(!missing[0].result); BOOST_CHECK(!missing[0].notification_result);
   BOOST_REQUIRE(missing[0].request); BOOST_CHECK(!missing[0].receiver_request);
   BOOST_CHECK(missing[0].failure == isolated_worker_failure::missing_state);
   BOOST_REQUIRE(std::holds_alternative<primary_capture_request>(*missing[0].request));
   BOOST_CHECK(std::get<primary_capture_request>(*missing[0].request).table == pool_rows);
}

BOOST_FIXTURE_TEST_CASE(tree_job_input_bounds_reject_ambiguous_modes_and_keep_pool_reusable, tree_pool_fixture) {
   isolated_worker_pool pool(1); const std::vector roots{root}; const auto catalog = programs(true);
   const std::array<ordered_attempt_batch::request, 1> requests{{{fc::sha256::hash("tree"), fc::sha256::hash("tree")}}};
   ordered_attempt_batch batch(base, requests); const isolated_worker_job good{batch.get_ticket(0), roots, catalog, tree_context};
   for (unsigned failure = 0; failure < 11; ++failure) {
      auto bad = good; auto& input = *bad.tree;
      switch (failure) {
         case 0: bad.actions.push_back({legacy, root, {}}); break;
         case 1: bad.notifications = isolated_notification_job{roots, programs(false)}; break;
         case 2: input.roots.clear(); break;
         case 3: input.roots.resize(17, root); break;
         case 4: input.programs.clear(); break;
         case 5: input.programs.resize(257, catalog.front()); break;
         case 6: input.programs[0].code.resize(65537); break;
         case 7: input.roots[0].data.resize(65537); break;
         case 8: input.roots[0].authorizations.resize(65); break;
         case 9: input.roots[0].max_return_bytes = 4097; break;
         case 10: input.roots.resize(5, root); for (auto& action : input.roots) action.data.resize(65536); break;
      }
      BOOST_CHECK_THROW(pool.run({bad}), primary_state_budget_exceeded);
      BOOST_REQUIRE(pool.run({good})[0].tree_result);
   }
   // The coordinator accepts the domain-separated tree fingerprint, tracks its
   // whole write set, and sends a conflicting second attempt through serial.
   isolated_action_tree_executor executor;
   const auto result = executor.execute(base, roots, catalog, tree_context);
   const std::array<ordered_attempt_batch::request, 2> ordered{{
      {fc::sha256::hash("first"), result.root_fingerprint}, {fc::sha256::hash("second"), result.root_fingerprint}}};
   ordered_attempt_batch order(base, ordered);
   const std::optional attempt{result}; unsigned adopted = 0, fallback = 0;
   for (size_t i = 0; i < ordered.size(); ++i)
      order.run_next(order.get_ticket(i), ordered[i].transaction_id, attempt,
         [&](const isolated_action_tree_result&) { ++adopted; }, [&] { ++fallback; });
   BOOST_CHECK_EQUAL(adopted, 1u); BOOST_CHECK_EQUAL(fallback, 1u); BOOST_CHECK(order.complete());
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(tree_workers_overlap_real_wasm_intervals_without_sharing_writes, tree_pool_fixture) {
   const std::vector roots{root, root}; const auto catalog = programs(true);
   isolated_action_tree_executor executor;
   const auto expected = executor.execute(base, roots, catalog, tree_context);
   for (size_t threads : {1u, 2u, 4u}) {
      isolated_worker_pool pool(threads); std::vector<ordered_attempt_batch::request> requests;
      for (size_t i = 0; i < 4; ++i) requests.push_back({fc::sha256::hash(std::to_string(i)), expected.root_fingerprint});
      ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
      for (size_t i = 0; i < requests.size(); ++i) jobs.emplace_back(batch.get_ticket(i), roots, catalog, tree_context);
      const auto outcomes = parallel_test_measure("tree overlap", [&] { return pool.run(std::move(jobs)); });
      bool overlap = false;
      for (size_t i = 0; i < outcomes.size(); ++i) {
         BOOST_REQUIRE_MESSAGE(outcomes[i].tree_result, "threads=" << threads << " job=" << i << " failure=" <<
            isolated_worker_failure_name(outcomes[i].failure) << " attempt_wall_us=" << outcomes[i].attempt_wall.count());
         const auto& result = *outcomes[i].tree_result;
         BOOST_CHECK(validate_isolated_action_tree_journal(result, roots, catalog, tree_context));
         BOOST_REQUIRE_EQUAL(result.state.writes().size(), 2u);
         for (size_t j = 0; j < 2; ++j) BOOST_CHECK(result.state.writes()[j].value == expected.state.writes()[j].value);
         for (size_t j = 0; j < i; ++j) for (const auto& a : result.steps)
            for (const auto& b : outcomes[j].tree_result->steps)
               if (a.has_code && b.has_code && std::max(a.effects.began, b.effects.began) < std::min(a.effects.ended, b.effects.ended))
                  overlap = true;
      }
      BOOST_CHECK_EQUAL(overlap, threads > 1); // actual WASM, not compilation/queue intervals
      BOOST_CHECK_EQUAL(canonical(), 10u);
   }
}

BOOST_AUTO_TEST_SUITE_END()
