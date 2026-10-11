#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/vm/exceptions.hpp>
#include <eosio/testing/tester.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <barrier>
#include <cstring>
#include <future>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key rows{"test"_n, "alice"_n, "rows"_n};

std::vector<char> wasm(const std::string& text) {
   const auto bytes = wast_to_wasm(text);
   return {bytes.begin(), bytes.end()};
}

// All imports are real chain intrinsics. No test-only host/barrier is imported.
std::vector<char> increment_wasm() {
   return wasm(R"(
      (module
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

struct wasm_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   wasm_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
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

BOOST_FIXTURE_TEST_CASE(unsupported_write_and_pointer_failure_discard_prior_write, wasm_fixture) {
   isolated_wasm_module machine(increment_wasm());
   for (uint32_t mode : {2u, 3u}) {
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
      isolated_wasm_module local(code);
      entered.set_value();
      try { (void)local.execute(base, input(1, 7, 0, 1), during.get_token()); }
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
            isolated_wasm_module local(code);
            gate.arrive_and_wait();
            auto start = std::chrono::steady_clock::now();
            auto result = local.execute(base, input(i + 1, 7, 150000));
            auto end = std::chrono::steady_clock::now();
            return report{start, end, std::move(result)};
         }));
      }
      std::vector<report> reports;
      for (auto& future : futures) reports.push_back(future.get());
      auto last_start = reports.front().start, first_end = reports.front().end;
      for (unsigned i = 0; i < workers; ++i) {
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
      isolated_wasm_module local(code, limits);
      gate.arrive_and_wait();
      try { (void)local.execute(base, input(1, 7, 0, 1)); }
      catch (const eosio::vm::timeout_exception&) { timeout_observed.store(true); return true; }
      timeout_observed.store(true);
      return false;
   });
   auto good = std::async(std::launch::async, [&] {
      isolated_wasm_module local(code);
      gate.arrive_and_wait();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      unsigned i = 0;
      while (!timeout_observed.load() || i < 10) {
         if (std::chrono::steady_clock::now() >= deadline) return false;
         if (number(local.execute(base, input(2, 3, 50000)).return_value) != 23) return false;
         ++i;
      }
      // Also execute after the other module's timeout/permission restoration.
      return number(local.execute(base, input(2, 3, 50000)).return_value) == 23;
   });
   BOOST_CHECK(bad.get());
   BOOST_CHECK(good.get());
   BOOST_CHECK_EQUAL(canonical(1), 10u);
   BOOST_CHECK_EQUAL(canonical(2), 20u);
}

BOOST_AUTO_TEST_CASE(unknown_import_and_observable_local_memory_limit_are_not_supported) {
   const auto unknown = wasm(R"((module
      (import "env" "db_remove_i64" (func (param i32)))
      (func (export "apply") (param i64 i64 i64))))");
   BOOST_CHECK_THROW(isolated_wasm_module{unknown}, eosio::vm::wasm_link_exception);
   for (auto memory : {"(memory 1)", "(memory 1 17)"}) {
      const auto code = wasm(std::string("(module ") + memory + " (func (export \"apply\") (param i64 i64 i64)))");
      BOOST_CHECK_THROW(isolated_wasm_module{code}, primary_state_unsupported);
   }
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

BOOST_AUTO_TEST_SUITE_END()
