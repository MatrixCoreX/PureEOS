// Standalone PRIVATE runtime integration: no canonical adoption or TPS claim.
#ifdef PUREEOS_PARALLEL_RUNTIME_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Parallel_Runtime_Engines
#include <boost/test/included/unit_test.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_deadline_service.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/vm/exceptions.hpp>
#include <fc/filesystem.hpp>
#include <IR/Module.h>
#include <Inline/Serialization.h>
#include <WAST/WAST.h>
#include <WASM/WASM.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <future>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
using namespace std::chrono_literals;
namespace {
std::vector<char> compile(const std::string& text) {
   IR::Module module;
   std::vector<WAST::Error> errors;
   WAST::parseModule(text.data(), text.size(), module, errors);
   if (!errors.empty()) throw std::runtime_error(errors.front().message);
   const auto names = std::find_if(module.userSections.begin(), module.userSections.end(),
      [](const auto& section) { return section.name == "name"; });
   if (names != module.userSections.end()) module.userSections.erase(names);
   Serialization::ArrayOutputStream output;
   WASM::serialize(output, module);
   const auto bytes = output.getBytes();
   return {bytes.begin(), bytes.end()};
}
uint64_t number(const std::vector<char>& bytes) {
   if (bytes.size() != 8) throw std::runtime_error("wrong return size");
   uint64_t value; std::memcpy(&value, bytes.data(), 8); return value;
}
constexpr table_key table{"test"_n, "test"_n, "rows"_n};
struct fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const auto& t = db.create<table_id_object>([](auto& row) {
         row.code = table.code; row.scope = table.scope; row.table = table.table; row.payer = table.code; row.count = 4;
      });
      for (uint64_t key = 1; key <= 4; ++key) db.create<key_value_object>([&](auto& row) {
         row.t_id = t.id; row.primary_key = key; row.payer = table.code;
         const uint64_t value = key * 10; row.value.assign(reinterpret_cast<const char*>(&value), 8);
      });
      const std::array tables{table};
      base = frozen_primary_state::capture(view, tables);
   }
   isolated_action_input input(uint64_t key = 1, uint64_t increment = 7) const {
      isolated_action_input result;
      result.receiver = result.account = table.code; result.action = "run"_n;
      result.authorizations = {{table.code,"active"_n}};
      result.data.resize(16); std::memcpy(result.data.data(), &key, 8); std::memcpy(result.data.data()+8, &increment, 8);
      return result;
   }
   std::vector<char> program(const std::string& tail = {}) const {
      return compile(R"((module
         (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
         (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
         (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
         (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
         (import "env" "set_action_return_value" (func $return (param i32 i32)))
         (memory 1 4)
         (func (export "apply") (param $receiver i64) (param i64 i64) (local $i i32)
            (drop (call $read (i32.const 0) (i32.const 16)))
            (set_local $i (call $find (get_local $receiver) (get_local $receiver)
               (i64.const )" + std::to_string(table.table.to_uint64_t()) + R"() (i64.load (i32.const 0))))
            (drop (call $get (get_local $i) (i32.const 32) (i32.const 8)))
            (i64.store (i32.const 32) (i64.add (i64.load (i32.const 32)) (i64.load (i32.const 8))))
            (call $update (get_local $i) (i64.const 0) (i32.const 32) (i32.const 8))
            )" + tail + R"((call $return (i32.const 32) (i32.const 8)))))");
   }
};
auto runtime(isolated_vm_engine engine, size_t capacity = 9) {
   return std::make_shared<isolated_execution_runtime>(engine, capacity);
}
}

BOOST_FIXTURE_TEST_CASE(both_engines_execute_identical_owned_writes_and_reuse, fixture) {
   const auto code = program();
   isolated_wasm_module oracle(code);
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      isolated_wasm_module module(code, {}, runtime(engine));
      for (unsigned round = 0; round < 3; ++round) {
         const auto arguments = input(1 + round, 7 + round);
         const auto expected = oracle.execute(base, arguments);
         const auto actual = module.execute(base, arguments);
         BOOST_CHECK(actual.return_value == expected.return_value);
         BOOST_CHECK(actual.execution_fingerprint == expected.execution_fingerprint);
         BOOST_REQUIRE_EQUAL(actual.state.writes().size(), 1u);
         BOOST_CHECK_EQUAL(number(actual.return_value), 10 * (1 + round) + 7 + round);
      }
      BOOST_CHECK_EQUAL(number(view.read_primary({table, primary_query_kind::find, 1}, 8).row->value), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(growth_in_start_and_apply_keeps_local_failures_private, fixture) {
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      auto rt = runtime(engine);
      isolated_wasm_limits limits; limits.max_pages = 1;
      isolated_wasm_module grow(program("(drop (grow_memory (i32.const 1)))"), limits, rt);
      BOOST_CHECK_THROW(grow.execute(base, input()), primary_state_unsupported);
      const auto start_code = compile(R"((module (memory 1 4)
         (func $start (drop (grow_memory (i32.const 1)))) (start $start)
         (func (export "apply") (param i64 i64 i64))))");
      isolated_wasm_module start(start_code, limits, rt);
      BOOST_CHECK_THROW(start.execute(base, input()), primary_state_unsupported);
      isolated_wasm_module healthy(program(), {}, rt);
      BOOST_CHECK_EQUAL(number(healthy.execute(base, input()).return_value), 17u);
   }
}

BOOST_FIXTURE_TEST_CASE(deadline_and_external_cancel_interrupt_then_release_the_timer_slot, fixture) {
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      auto rt = runtime(engine, 1);
      isolated_wasm_limits limits; limits.max_attempt_time = 30ms;
      // The same module can take an infinite or healthy branch; the failed
      // write must never mutate canonical state or poison a later execution.
      isolated_wasm_module module(program(R"((if (i64.eqz (i64.load (i32.const 8)))
         (then (loop $forever (br $forever)))))"), limits, rt);
      BOOST_CHECK_THROW(module.execute(base, input(1, 0)), eosio::vm::timeout_exception);
      BOOST_CHECK_EQUAL(number(module.execute(base, input()).return_value), 17u);
      auto generation = std::make_shared<std::atomic<uint64_t>>(4);
      isolated_cancel cancel{generation, 4, std::chrono::steady_clock::now() - 1ms};
      BOOST_CHECK_THROW(module.execute(base, input(), {}, {}, cancel), isolated_execution_interrupted);
      cancel.deadline = std::chrono::steady_clock::time_point::max();
      generation->store(5);
      BOOST_CHECK_THROW(module.execute(base, input(), {}, {}, cancel), isolated_execution_interrupted);
      BOOST_CHECK_EQUAL(number(module.execute(base, input()).return_value), 17u);
      BOOST_CHECK_EQUAL(number(view.read_primary({table, primary_query_kind::find, 1}, 8).row->value), 10u);
   }
}

BOOST_FIXTURE_TEST_CASE(worker_counts_use_the_selected_engine_and_keep_failures_atomic, fixture) {
   const auto good = program(), bad = program("(unreachable)");
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      for (size_t threads : {1u, 2u, 4u, 8u, 12u, 16u}) {
         isolated_worker_pool pool(threads, engine);
         for (unsigned round = 0; round < 3; ++round) {
            std::vector<ordered_attempt_batch::request> requests;
            for (size_t i = 0; i < 16; ++i) {
               const auto& code = round == 1 && i % 2 ? bad : good;
               const std::array fingerprints{isolated_execution_fingerprint(
                  fc::sha256::hash(code.data(), code.size()), {}, input(1 + i % 4))};
               requests.push_back({fc::sha256::hash(std::to_string(i)), isolated_transaction_fingerprint(fingerprints)});
            }
            ordered_attempt_batch batch(base, requests);
            std::vector<isolated_worker_job> jobs;
            for (size_t i = 0; i < requests.size(); ++i)
               jobs.emplace_back(batch.get_ticket(i), round == 1 && i % 2 ? bad : good, input(1 + i % 4), isolated_wasm_limits{});
            const auto outcomes = pool.run(std::move(jobs));
            BOOST_REQUIRE_EQUAL(outcomes.size(), requests.size());
            for (size_t i = 0; i < outcomes.size(); ++i) {
               const auto& out = outcomes[i];
               if (round == 1 && i % 2) {
                  BOOST_CHECK(!out.result);
                  BOOST_CHECK(out.failure == isolated_worker_failure::vm_error);
               } else {
                  BOOST_REQUIRE(out.result);
                  BOOST_CHECK(out.failure == isolated_worker_failure::none);
                  BOOST_CHECK(out.result->execution_fingerprint == requests[i].execution_fingerprint);
                  BOOST_CHECK_EQUAL(number(out.result->actions[0].return_value), 10 * (1 + i % 4) + 7);
               }
            }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(sync_export_reads_owned_metadata_and_nested_frames_have_distinct_timers, fixture) {
   for (name account : {"test"_n, "bob"_n}) db.create<account_object>([&](auto& obj) { obj.name = account; });
   const std::array tables{table};
   const std::array accounts{"test"_n, "bob"_n};
   base = frozen_primary_state::capture_queries(view, tables, {}, {}, {}, accounts);
   const auto root = compile(R"((module
      (import "env" "call" (func $call (param i64 i64 i32 i32) (result i64)))
      (import "env" "get_call_return_value" (func $get (param i32 i32) (result i32)))
      (import "env" "set_action_return_value" (func $out (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (call $call (i64.const )" + std::to_string("bob"_n.to_uint64_t()) + R"()
            (i64.const 0) (i32.const 0) (i32.const 0)))
         (drop (call $get (i32.const 0) (i32.const 8)))
         (call $out (i32.const 0) (i32.const 8)))))");
   const auto child = compile(R"((module
      (import "env" "set_call_return_value" (func $out (param i32 i32)))
      (memory 1 1)
      (func (export "sync_call") (param i64 i64 i32) (result i64)
         (i64.store (i32.const 0) (i64.const 77))
         (call $out (i32.const 0) (i32.const 8))
         (i64.const 0))))");
   const std::vector<isolated_sync_program> programs{{{table.code, root, {}, false}, false},
                                                   {{"bob"_n, child, {}, false}, true}};
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit}) {
      BOOST_TEST_CONTEXT("nested runtime engine=" << isolated_vm_engine_name(engine)) {
      auto modules = std::make_shared<isolated_module_cache>(runtime(engine));
      isolated_sync_executor executor(modules);
      for (unsigned round = 0; round < 3; ++round) {
         const auto result = executor.execute(base, input(), programs, {});
         BOOST_CHECK_EQUAL(number(result.root.return_value), 77u);
         BOOST_REQUIRE_EQUAL(result.calls.size(), 1u);
         BOOST_CHECK(result.calls.front().executed);
         BOOST_CHECK_EQUAL(result.calls.front().status, 0);
         BOOST_CHECK_EQUAL(number(result.calls.front().return_value), 77u);
         BOOST_CHECK(validate_isolated_sync_journal(result, input(), programs, {}));
      }
      }
   }
}

BOOST_AUTO_TEST_CASE(runtime_policy_rejects_invalid_engines_and_frame_capacities) {
   BOOST_CHECK_THROW(isolated_execution_runtime(static_cast<isolated_vm_engine>(99), 1), primary_state_unsupported);
   BOOST_CHECK_THROW(isolated_execution_runtime(isolated_vm_engine::jit, 0), primary_state_unsupported);
   BOOST_CHECK_THROW(isolated_execution_runtime(isolated_vm_engine::jit,
      parallel_runtime_limits::max_workers * (isolated_sync_executor::max_depth + 1) + 1), primary_state_unsupported);
   BOOST_CHECK_THROW(isolated_worker_pool(parallel_runtime_limits::max_workers + 1, isolated_vm_engine::jit), primary_state_budget_exceeded);
   isolated_execution_runtime interpreted(isolated_vm_engine::interpreter, 1);
   BOOST_CHECK_THROW(interpreted.deadlines(), primary_state_invalid_attempt);
}
#endif
