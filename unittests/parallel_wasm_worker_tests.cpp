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

// Persistent workers are tested across batches, not just one-shot async tasks.
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
         requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_execution_fingerprint(hash, {}, input())});
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
            if (i % codes.size() == 1) BOOST_CHECK_EQUAL(number(output.result->return_value), (uint64_t{16} << 32) | 1);
            else BOOST_CHECK_EQUAL(output.result->return_value.size(), 64u);
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
            requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_execution_fingerprint(hash, {}, args.back())});
         }
         ordered_attempt_batch batch(base, requests);
         std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < args.size(); ++i) jobs.push_back({batch.get_ticket(i), code, args[i], {}});
         const auto results = pool.run(std::move(jobs));
         BOOST_REQUIRE_EQUAL(results.size(), args.size());
         for (size_t i = 0; i < results.size(); ++i) {
            BOOST_REQUIRE(results[i].result);
            BOOST_CHECK_EQUAL(number(results[i].result->return_value), (i % 4 + 1) * 10 + 7 + round);
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
      {fc::sha256::hash("a"), isolated_execution_fingerprint(hash, {}, input())},
      {fc::sha256::hash("b"), isolated_execution_fingerprint(hash, {}, second)}}};
   ordered_attempt_batch batch(base, requests);
   isolated_worker_pool pool(2);
   auto results = pool.run({{batch.get_ticket(0), code, input(), {}}, {batch.get_ticket(1), code, second, {}}});
   size_t adopted = 0;
   for (size_t i = 0; i < 2; ++i)
      batch.run_next(batch.get_ticket(i), requests[i].transaction_id, results[i].result,
         [&](const auto& result) { BOOST_CHECK_EQUAL(number(result.return_value), 17u); ++adopted; },
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
   job.code.resize(64 * 1024 + 1);
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
      {fc::sha256::hash("account"), isolated_execution_fingerprint(hash, {}, input())}}};
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
      {fc::sha256::hash("first"), isolated_execution_fingerprint(hash, {}, input(1))},
      {fc::sha256::hash("second"), isolated_execution_fingerprint(hash, {}, input(2))}}};
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
         [&](const auto& output) { BOOST_CHECK_EQUAL(number(output.return_value), (i + 1) * 10 + 7); },
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
   BOOST_REQUIRE(retry[0].result); BOOST_CHECK_EQUAL(number(retry[0].result->return_value), 17u);
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
   BOOST_REQUIRE(retry[0].result); BOOST_CHECK_EQUAL(number(retry[0].result->return_value), 17u);
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()
