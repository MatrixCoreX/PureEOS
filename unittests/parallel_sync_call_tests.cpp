#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <fc/io/raw.hpp>
#include <eosio/vm/exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <barrier>
#include <cstring>
#include <future>
#include <limits>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
#include "parallel_test_diagnostics.hpp"
namespace {
std::string sc_i64(uint64_t n) { return "(i64.const " + std::to_string(n) + ")"; }
std::string sc_name(name n) { return sc_i64(n.to_uint64_t()); }
std::string sc_call(name receiver, uint64_t flags = 0, uint32_t size = 8) {
   return "(call $call " + sc_name(receiver) + sc_i64(flags) + "(i32.const 0)(i32.const " + std::to_string(size) + "))";
}
std::string sc_assert64(const std::string& value, int64_t expected) {
   return "(call $assert (i64.eq " + value + "(i64.const " + std::to_string(expected) + "))(i64.const 1))";
}
std::string sc_assert32(const std::string& value, uint32_t expected) {
   return "(call $assert (i32.eq " + value + "(i32.const " + std::to_string(expected) + "))(i64.const 2))";
}
std::vector<char> sc_program(const std::string& root = {}, const std::string& child = "(i64.const 0)",
      const std::string& imports = {}, const std::string& start = {}, bool has_sync = true) {
   const auto bytes = wast_to_wasm(R"((module
      (import "env" "call" (func $call (param i64 i64 i32 i32)(result i64)))
      (import "env" "get_call_return_value" (func $ret (param i32 i32)(result i32)))
      (import "env" "get_call_data" (func $data (param i32 i32)(result i32)))
      (import "env" "set_call_return_value" (func $set (param i32 i32)))
      (import "env" "set_action_return_value" (func $out (param i32 i32)))
      (import "env" "eosio_assert_code" (func $assert (param i32 i64)))
      (import "env" "current_receiver" (func $receiver (result i64)))
      (import "env" "get_sender" (func $sender (result i64)))
      (import "env" "read_action_data" (func $read (param i32 i32)(result i32)))
      )" + imports + R"(
      (memory 1 2) (global $counter (mut i32) (i32.const 0))
      (func $start )" + start + R"() (start $start)
      (func (export "apply") (param i64 i64 i64) (local $n i32) )" + root + ")" +
      (has_sync ? R"((func (export "sync_call") (param $sender i64)(param $receiver i64)(param $size i32)(result i64)
         (local $n i32) )" + child + ")" : "") + ")");
   return {bytes.begin(), bytes.end()};
}
isolated_action_input sc_root(uint64_t value = 5) {
   isolated_action_input input; input.receiver = input.account = "test"_n; input.action = "run"_n;
   input.authorizations = {{"test"_n,"active"_n}}; input.data.resize(8); std::memcpy(input.data.data(), &value, 8);
   return input;
}
uint64_t sc_number(const std::vector<char>& bytes) {
   if (bytes.size() != 8) throw std::logic_error("expected eight owned bytes");
   uint64_t n; std::memcpy(&n, bytes.data(), 8); return n;
}
std::string sc_root_body(name target = "bob"_n, uint64_t flags = 0) {
   return "(drop (call $read (i32.const 0)(i32.const 8)))" + sc_assert64(sc_call(target,flags),8) +
      sc_assert32("(call $ret (i32.const 64)(i32.const 8))",8) + "(call $out (i32.const 64)(i32.const 8))";
}
std::string sc_echo() {
   return sc_assert32("(get_local $size)",8) +
      "(call $assert (i64.eq (call $receiver)(get_local $receiver))(i64.const 3))" +
      sc_assert32("(call $data (i32.const 65536)(i32.const 0))",8) +
      sc_assert32("(call $data (i32.const 0)(i32.const 2))",8) +
      sc_assert32("(call $data (i32.const 0)(i32.const 8))",8) +
      "(call $set (i32.const 0)(i32.const 8))(i64.const 0)";
}
std::vector<isolated_sync_program> sc_pair(const std::string& child = sc_echo(), uint64_t flags = 0) {
   return {{{"test"_n, sc_program(sc_root_body("bob"_n,flags)), {}}, true},
           {{"bob"_n, sc_program({},child), {}}, true}};
}

std::vector<isolated_sync_program> sc_preserved_return() {
   auto programs=sc_pair(
      "(i64.store (i32.const 0)(i64.const 42))(call $set (i32.const 0)(i32.const 8))"+
      sc_assert64(sc_call("carol"_n),8)+
      sc_assert32("(call $ret (i32.const 16)(i32.const 2))",8)+
      sc_assert32("(i32.load16_u (i32.const 16))",99)+"(i64.const 0)");
   programs.push_back({{"carol"_n,sc_program({},
      "(i64.store (i32.const 0)(i64.const 99))(call $set (i32.const 0)(i32.const 8))(i64.const 0)"),{}},true});
   return programs;
}

struct sc_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   sc_fixture() {
      db.add_index<account_index>(); db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      for (name n : {"test"_n,"bob"_n,"carol"_n,"empty"_n})
         db.create<account_object>([&](auto& a) { a.name = n; });
      const std::array names{"test"_n,"bob"_n,"carol"_n,"empty"_n,"absent"_n};
      base = frozen_primary_state::capture_queries(view, {}, {}, {}, {}, names);
   }
};
std::vector<isolated_sync_program> sc_recursive() {
   const auto body = R"(
      (call $assert (i32.eqz (get_global $counter))(i64.const 4))
      (set_global $counter (i32.const 1))
      (drop (call $data (i32.const 0)(i32.const 8)))
      (set_local $n (i32.load (i32.const 0)))
      (i32.store (i32.const 32)(i32.add (get_local $n)(i32.const 100)))
      (i64.store (i32.const 64)(i64.const 1))
      (if (get_local $n) (then
         (i32.store (i32.const 0)(i32.sub (get_local $n)(i32.const 1)))
         )" + sc_assert64(sc_call("test"_n),8) + R"(
         (drop (call $ret (i32.const 64)(i32.const 8)))
         (i64.store (i32.const 64)(i64.add (i64.load (i32.const 64))(i64.const 1)))
      ))
      (call $assert (i32.eq (i32.load (i32.const 32))(i32.add (get_local $n)(i32.const 100)))(i64.const 5))
      (call $assert (i32.eq (get_global $counter)(i32.const 1))(i64.const 6))
      (call $set (i32.const 64)(i32.const 8))(i64.const 0))";
   return {{{"test"_n, sc_program(sc_root_body("test"_n),body), {}}, true}};
}
const std::string sc_db_imports=R"(
   (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64)(result i32)))
   (import "env" "db_get_i64" (func $get (param i32 i32 i32)(result i32)))
   (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
   (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32)(result i32)))
   (import "env" "db_remove_i64" (func $remove (param i32)))
   (import "env" "db_next_i64" (func $next (param i32 i32)(result i32)))
   (import "env" "db_previous_i64" (func $prev (param i32 i32)(result i32))))";
const std::array sc_tables{table_key{"test"_n,"scope"_n,"rows"_n},table_key{"bob"_n,"scope"_n,"rows"_n}};
std::string sc_find(name code,uint64_t key=7) {
   return "(call $find "+sc_name(code)+sc_name("scope"_n)+sc_name("rows"_n)+sc_i64(key)+")";
}
std::string sc_update(const std::string& iterator,uint64_t value,name payer={}) {
   return "(i64.store (i32.const 64)"+sc_i64(value)+")(call $update "+iterator+sc_name(payer)+"(i32.const 64)(i32.const 8))";
}
std::string sc_check_row(const std::string& iterator,uint64_t value) {
   return sc_assert32("(call $get "+iterator+"(i32.const 64)(i32.const 8))",8)+sc_assert64("(i64.load (i32.const 64))",value);
}
std::string sc_store(uint64_t key,uint64_t value,name payer="bob"_n) {
   return "(i64.store (i32.const 64)"+sc_i64(value)+")(drop (call $store "+
      sc_name("scope"_n)+sc_name("rows"_n)+sc_name(payer)+sc_i64(key)+"(i32.const 64)(i32.const 8)))";
}
void sc_seed(chainbase::database& db,std::span<const table_key> tables) {
   for(const auto& key:tables) {
      const auto& table=db.create<table_id_object>([&](auto& t){
         t.code=key.code; t.scope=key.scope; t.table=key.table; t.payer=key.code; t.count=2;
      });
      for(uint64_t k:{7u,9u}) db.create<key_value_object>([&](auto& row){
         row.t_id=table.id; row.primary_key=k; row.payer=key.code;
         const uint64_t value=k==7?10:90; row.value.assign(reinterpret_cast<const char*>(&value),8);
      });
   }
}
std::shared_ptr<const frozen_primary_state> sc_capture(const chainbase_transaction_state_view& view,
      std::span<const table_key> tables,std::span<const name> accounts) {
   std::vector<primary_query> queries;
   for(const auto& table:tables) {
      for(uint64_t k:{7u,9u,11u}) queries.push_back({table,primary_query_kind::find,k});
      queries.push_back({table,primary_query_kind::upper_bound,7});
      queries.push_back({table,primary_query_kind::previous,7});
   }
   // capture_queries(keys) captures metadata ONLY, not a complete table.
   // Enumerate the exact row/range observations these bounded fixtures need.
   return frozen_primary_state::capture_queries(view,tables,queries,{},{},accounts);
}
struct sc_state_fixture:sc_fixture {
   sc_state_fixture() {
      sc_seed(db,sc_tables);
      const std::array accounts{"test"_n,"bob"_n,"carol"_n,"empty"_n,"absent"_n};
      base=sc_capture(view,sc_tables,accounts);
   }
};
std::vector<isolated_sync_program> sc_state_programs(int64_t status=0,bool nested=false,uint64_t flags=0) {
   const auto expected=status==0?0:status<=-10000?status:-3;
   const auto root="(set_local $n "+sc_find("test"_n)+")"+sc_update("(get_local $n)",11)+
      sc_assert64(sc_call("bob"_n,flags),expected)+sc_check_row(sc_find("bob"_n),22)+
      sc_check_row("(get_local $n)",nested?44:11)+sc_update("(get_local $n)",33)+
      "(call $out (i32.const 64)(i32.const 8))";
   const auto child=sc_check_row(sc_find("test"_n),11)+sc_update(sc_find("bob"_n),22)+
      (nested?sc_assert64(sc_call("test"_n),0):"")+(status?"(call $set (i32.const 64)(i32.const 8))":"")+
      "(i64.const "+std::to_string(status)+")";
   return {{{"test"_n,sc_program(root,sc_update(sc_find("test"_n),44)+"(i64.const 0)",sc_db_imports),{}},true},
           {{"bob"_n,sc_program({},child,sc_db_imports),{}},true}};
}
std::vector<isolated_sync_program> sc_delete_programs(const std::string& after,bool recreate) {
   const auto root="(set_local $n "+sc_find("bob"_n)+")"+sc_assert64(sc_call("bob"_n),0)+after;
   const auto child="(call $remove "+sc_find("bob"_n)+")"+(recreate?sc_store(7,77):"")+"(i64.const 0)";
   return {{{"test"_n,sc_program(root,"(i64.const 0)",sc_db_imports),{}},true},
           {{"bob"_n,sc_program({},child,sc_db_imports),{}},true}};
}
}

BOOST_AUTO_TEST_SUITE(parallel_sync_private_tests)
BOOST_FIXTURE_TEST_CASE(immediate_owned_returns_partial_reads_and_empty_buffers, sc_fixture) {
   isolated_sync_executor executor;
   for (unsigned i=0;i<3;++i) {
      auto result = executor.execute(base,sc_root(17+i),sc_pair(),{});
      BOOST_CHECK_EQUAL(sc_number(result.root.return_value),17+i);
      BOOST_REQUIRE_EQUAL(result.calls.size(),1u);
      BOOST_CHECK_EQUAL(result.calls[0].status,0);
      BOOST_CHECK_EQUAL(sc_number(result.calls[0].return_value),17+i);
      BOOST_CHECK_EQUAL(result.calls[0].call_ordinal,1u);
      BOOST_CHECK_EQUAL(result.calls[0].sender_ordinal,0u);
      BOOST_CHECK(result.calls[0].sender=="test"_n);
      BOOST_CHECK(result.calls[0].receiver=="bob"_n);
      BOOST_CHECK(result.state.writes().empty());
      BOOST_CHECK_GE(result.root.elapsed.count(),0);
      BOOST_CHECK_GE(result.calls[0].exclusive_elapsed.count(),0);
      const auto inclusive = std::chrono::duration_cast<std::chrono::microseconds>(result.root.ended-result.root.began);
      BOOST_CHECK_LE((result.root.elapsed + result.calls[0].exclusive_elapsed).count(),inclusive.count());
   }
}

BOOST_FIXTURE_TEST_CASE(child_own_return_is_not_overwritten_by_its_last_nested_return, sc_fixture) {
   isolated_sync_executor executor;
   const auto result=executor.execute(base,sc_root(),sc_preserved_return(),{});
   BOOST_CHECK_EQUAL(sc_number(result.root.return_value),42u);
   BOOST_REQUIRE_EQUAL(result.calls.size(),2u);
   BOOST_CHECK_EQUAL(sc_number(result.calls[0].return_value),42u);
   BOOST_CHECK_EQUAL(sc_number(result.calls[1].return_value),99u);
}
BOOST_FIXTURE_TEST_CASE(recursive_same_code_never_reuses_a_live_vm, sc_fixture) {
   isolated_sync_executor executor;
   const auto programs=sc_recursive();
   for (uint64_t depth : {0,1,4,7}) {
      const auto result=executor.execute(base,sc_root(depth),programs,{});
      BOOST_CHECK_EQUAL(sc_number(result.root.return_value),depth+1);
      BOOST_REQUIRE_EQUAL(result.calls.size(),depth+1);
      for(size_t i=0;i<result.calls.size();++i) {
         BOOST_CHECK_EQUAL(result.calls[i].call_ordinal,i+1);
         BOOST_CHECK_EQUAL(result.calls[i].sender_ordinal,i);
         BOOST_CHECK_EQUAL(sc_number(result.calls[i].return_value),depth+1-i);
      }
   }
   BOOST_CHECK_THROW(executor.execute(base,sc_root(8),programs,{}),primary_state_unsupported);
   BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(1),programs,{}).root.return_value),2u);
}
BOOST_FIXTURE_TEST_CASE(status_normalization_clears_previous_child_return, sc_fixture) {
   const std::array<int64_t,7> statuses{0,-10000,-10001,-1,-9999,1,std::numeric_limits<int64_t>::min()};
   std::string root;
   for (int64_t status : statuses) {
      root += "(i64.store (i32.const 0)(i64.const " + std::to_string(status) + "))";
      const auto expected=status==0 ? 8 : status<=-10000 ? status : -3;
      root += sc_assert64(sc_call("bob"_n),expected);
      root += sc_assert32("(call $ret (i32.const 64)(i32.const 1))",status==0?8:0);
      root += sc_assert32("(call $ret (i32.const 65536)(i32.const 0))",status==0?8:0);
   }
   auto programs=sc_pair("(drop (call $data (i32.const 0)(i32.const 8)))(call $set (i32.const 0)(i32.const 8))(i64.load (i32.const 0))");
   programs[0].program.code=sc_program(root);
   isolated_sync_executor executor;
   const auto result=executor.execute(base,sc_root(),programs,{});
   BOOST_REQUIRE_EQUAL(result.calls.size(),statuses.size());
   for(size_t i=0;i<statuses.size();++i) {
      BOOST_CHECK_EQUAL(result.calls[i].status,statuses[i]==0?0:statuses[i]<=-10000?statuses[i]:-3);
      BOOST_CHECK_EQUAL(result.calls[i].return_value.size(),statuses[i]==0?8u:0u);
   }
}
BOOST_FIXTURE_TEST_CASE(account_flags_and_captured_export_metadata_preserve_order, sc_fixture) {
   isolated_sync_executor executor;
   auto programs=sc_pair();
   programs.push_back({{"empty"_n,{},{}},false});
   programs[1].sync_call_supported=false;
   programs[0].program.code=sc_program(sc_assert64(sc_call("absent"_n,~uint64_t{}),-1)+
      sc_assert64(sc_call("empty"_n),-1)+sc_assert64(sc_call("bob"_n),-2));
   const auto result=executor.execute(base,sc_root(),programs,{});
   BOOST_REQUIRE_EQUAL(result.calls.size(),3u);
   BOOST_CHECK_EQUAL(result.calls[0].status,-1); BOOST_CHECK_EQUAL(result.calls[1].status,-1);
   BOOST_CHECK_EQUAL(result.calls[2].status,-2);
   programs[0].program.code=sc_program("(drop "+sc_call("empty"_n,2)+")");
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported);
   programs=sc_pair(); programs[1].program.code=sc_program({}, {}, {}, {}, false);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported);
   const auto wrong_signature=wast_to_wasm(R"((module
      (func (export "apply") (param i64 i64 i64))
      (func (export "sync_call") (param i64 i64 i64)(result i64)(i64.const 0))))");
   programs[1].program.code.assign(wrong_signature.begin(),wrong_signature.end());
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported);
   programs=sc_pair(); programs.pop_back();
   BOOST_CHECK_EXCEPTION(executor.execute(base,sc_root(),programs,{}),isolated_receiver_missing,
      [](const auto& e){return e.receiver=="bob"_n;});
   const auto unknown=frozen_primary_state::capture_queries(view,{},{});
   BOOST_CHECK_THROW(executor.execute(unknown,sc_root(),sc_pair(),{}),primary_state_missing);
}
BOOST_FIXTURE_TEST_CASE(readonly_inheritance_sender_protocol_and_console_ordinals, sc_fixture) {
   isolated_sync_executor executor;
   for(bool root_ro : {false,true}) for(bool force_ro : {false,true}) for(bool fixed : {false,true}) {
      auto programs=sc_pair(sc_echo(),force_ro);
      // A sync child returns via set_call_return_value, not action-only output.
      auto nested="(drop (call $data (i32.const 0)(i32.const 8)))" + sc_assert64(sc_call("carol"_n),8) +
         "(drop (call $ret (i32.const 64)(i32.const 8)))(call $set (i32.const 64)(i32.const 8))(i64.const 0)";
      programs[1].program.code=sc_program({},nested);
      programs.push_back({{"carol"_n,sc_program({},sc_assert64("(get_local $sender)","bob"_n.to_uint64_t())+
         sc_assert64("(call $sender)",(fixed?"bob"_n:"carol"_n).to_uint64_t())+sc_echo()),{}},true});
      const auto result=executor.execute(base,sc_root(),programs,{16,512*1024,root_ro,fixed,true});
      BOOST_REQUIRE_EQUAL(result.calls.size(),2u);
      BOOST_CHECK_EQUAL(result.calls[0].read_only,root_ro||force_ro);
      BOOST_CHECK_EQUAL(result.calls[1].read_only,root_ro||force_ro);
      BOOST_CHECK_EQUAL(result.calls[1].sender_ordinal,1u);
      BOOST_CHECK(result.root_console_markers==std::vector<uint32_t>{0});
      BOOST_CHECK(result.calls[0].console_markers==std::vector<uint32_t>{0});
      BOOST_CHECK(result.calls[1].console_markers.empty());
   }
}
BOOST_FIXTURE_TEST_CASE(action_only_hosts_and_readonly_writes_require_whole_attempt_fallback, sc_fixture) {
   const std::array<std::pair<std::string,std::string>,6> cases{{
      {"", "(drop (call $read (i32.const 0)(i32.const 0)))"},
      {"", "(call $out (i32.const 0)(i32.const 0))"},
      {R"((import "env" "action_data_size" (func $bad (result i32))))","(drop (call $bad))"},
      {R"((import "env" "has_auth" (func $bad (param i64)(result i32))))","(drop (call $bad (i64.const 0)))"},
      {R"((import "env" "require_recipient" (func $bad (param i64))))","(call $bad "+sc_name("bob"_n)+")"},
      {R"((import "env" "db_store_i64" (func $bad (param i64 i64 i64 i64 i32 i32)(result i32))))",
       "(drop (call $bad (i64.const 0)(i64.const 0)(i64.const 0)(i64.const 1)(i32.const 0)(i32.const 0)))"}
   }};
   isolated_sync_executor executor;
   for(const auto& [imports,body]:cases) {
      auto programs=sc_pair(sc_echo(),1); programs[1].program.code=sc_program({},body+"(i64.const 0)",imports);
      BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported);
      programs[1].program.code=sc_program({},sc_echo(),imports,body);
      BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported);
      BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),sc_pair(),{}).root.return_value),5u);
   }
}
BOOST_FIXTURE_TEST_CASE(budgets_cancellation_faults_and_retry_do_not_publish_prefixes, sc_fixture) {
   isolated_sync_executor executor;
   auto programs=sc_pair();
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{0}),primary_state_unsupported);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{16,7}),primary_state_unsupported);
   std::stop_source cancelled; cancelled.request_stop();
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{},cancelled.get_token()),isolated_execution_interrupted);
   isolated_cancel expired; expired.deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}, {}, {},expired),isolated_execution_interrupted);
   for(const std::string body : {"unreachable (i64.const 0)","(i64.load (i32.const 65535))"}) {
      programs[1].program.code=sc_program({},body);
      BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),eosio::vm::exception);
   }
   programs=sc_pair(); programs[1].program.code=sc_program({},"(loop $forever (br $forever))(i64.const 0)");
   expired.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}, {}, {},expired),isolated_execution_interrupted);
   BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),sc_pair(),{}).root.return_value),5u);
   programs=sc_pair(); programs.push_back(programs.back());
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_budget_exceeded);
}
BOOST_FIXTURE_TEST_CASE(one_two_four_threads_use_independent_recursive_frames, sc_fixture) {
   const auto programs=sc_recursive();
   for(unsigned threads : {1u,2u,4u}) {
      std::barrier gate(threads); std::vector<std::future<isolated_sync_result>> results;
      for(unsigned i=0;i<threads;++i) results.push_back(std::async(std::launch::async,[&,i] {
         isolated_sync_executor executor; gate.arrive_and_wait();
         return executor.execute(base,sc_root(i+1),programs,{});
      }));
      for(unsigned i=0;i<threads;++i) BOOST_CHECK_EQUAL(sc_number(results[i].get().root.return_value),i+2);
   }
   isolated_sync_executor owner;
   auto wrong=std::async(std::launch::async,[&] {
      try { (void)owner.execute(base,sc_root(),programs,{}); } catch(const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(wrong.get());
}

BOOST_FIXTURE_TEST_CASE(primary_reads_share_frozen_state_but_not_iterator_caches, sc_fixture) {
   const table_key key{"bob"_n,"scope"_n,"rows"_n};
   const auto& table=db.create<table_id_object>([&](auto& t) {
      t.code=key.code; t.scope=key.scope; t.table=key.table; t.payer="bob"_n; t.count=1;
   });
   db.create<key_value_object>([&](auto& row) {
      row.t_id=table.id; row.primary_key=7; row.payer="bob"_n;
      const uint64_t value=42; row.value.assign(reinterpret_cast<const char*>(&value),8);
   });
   const std::array queries{primary_query{key,primary_query_kind::find,7}};
   const std::array accounts{"test"_n,"bob"_n};
   const auto captured=frozen_primary_state::capture_queries(view,{},queries,{},{},accounts);
   const std::string imports=R"(
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64)(result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32)(result i32))))";
   const auto find="(call $find "+sc_name(key.code)+sc_name(key.scope)+sc_name(key.table)+"(i64.const 7))";
   const auto read=sc_assert32("(call $get "+find+"(i32.const 64)(i32.const 8))",8);
   auto programs=sc_pair();
   programs[0].program.code=sc_program(read+sc_root_body()+read+"(call $out (i32.const 64)(i32.const 8))","(i64.const 0)",imports);
   programs[1].program.code=sc_program({},read+"(call $set (i32.const 64)(i32.const 8))(i64.const 0)",imports);
   isolated_sync_executor executor;
   const auto result=executor.execute(captured,sc_root(),programs,{});
   BOOST_CHECK_EQUAL(sc_number(result.root.return_value),42u);
   BOOST_REQUIRE_EQUAL(result.calls.size(),1u);
   BOOST_CHECK_EQUAL(sc_number(result.calls[0].return_value),42u);
   BOOST_CHECK(result.state.writes().empty());
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_missing);
   const std::string update=R"((import "env" "db_update_i64" (func $update (param i32 i64 i32 i32))))";
   programs[0].program.code=sc_program(read+"(call $update "+find+"(i64.const 0)(i32.const 64)(i32.const 8))","(i64.const 0)",imports+update);
   BOOST_CHECK_THROW(executor.execute(captured,sc_root(),programs,{16,512*1024,true}),primary_state_unsupported);
   BOOST_CHECK_EQUAL(sc_number(view.read_primary(queries[0],8).row->value),42u);
}
BOOST_FIXTURE_TEST_CASE(total_call_and_data_limits_and_fingerprints_are_attempt_wide, sc_fixture) {
   isolated_sync_executor executor; auto programs=sc_pair();
   std::string sixtyfour;
   for(unsigned i=0;i<64;++i) sixtyfour+=sc_assert64(sc_call("bob"_n),8);
   programs[0].program.code=sc_program(sixtyfour);
   const auto result=executor.execute(base,sc_root(),programs,{});
   BOOST_CHECK_EQUAL(result.calls.size(),64u);
   programs[0].program.code=sc_program(sixtyfour+sc_assert64(sc_call("bob"_n),8));
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_budget_exceeded);
   programs=sc_pair();
   const auto a=executor.execute(base,sc_root(),programs,{});
   const auto b=executor.execute(base,sc_root(6),programs,{});
   BOOST_CHECK(a.execution_fingerprint!=b.execution_fingerprint);
   auto protocol=isolated_sync_context{}; protocol.get_sender_fix=true;
   const auto c=executor.execute(base,sc_root(),programs,protocol);
   BOOST_CHECK(a.execution_fingerprint!=c.execution_fingerprint);
   programs[0].program.serial_only=true;
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported);
   programs=sc_pair("(i64.const 0)");
   std::string large;
   for(unsigned i=0;i<5;++i) large+="(drop "+sc_call("bob"_n,0,65536)+")";
   programs[0].program.code=sc_program(large);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_budget_exceeded);
}

BOOST_FIXTURE_TEST_CASE(immediate_primary_writes_business_errors_and_recursive_parent_visibility,sc_state_fixture) {
   isolated_sync_executor executor;
   for(int64_t status:{0,-10000,-1}) for(bool nested:{false,true}) {
      const auto programs=sc_state_programs(status,nested);
      const auto result=executor.execute(base,sc_root(),programs,{});
      BOOST_CHECK(validate_isolated_sync_journal(result,sc_root(),programs,{}));
      BOOST_CHECK_EQUAL(sc_number(result.root.return_value),33u);
      BOOST_REQUIRE_EQUAL(result.calls.size(),nested?2u:1u);
      BOOST_REQUIRE_EQUAL(result.state.writes().size(),nested?4u:3u);
      BOOST_CHECK_EQUAL(sc_number(result.state.writes()[1].value),22u); // retained on business failure
      BOOST_CHECK_EQUAL(result.calls[0].writes_begin,1u);
      BOOST_CHECK_EQUAL(result.calls[0].writes_end,nested?3u:2u);
      BOOST_CHECK_EQUAL(result.calls[0].status,status==0?0:status<=-10000?status:-3);
      BOOST_CHECK(result.calls[0].return_value.empty());
      BOOST_CHECK_EQUAL(sc_number(view.read_primary({sc_tables[0],primary_query_kind::find,7},8).row->value),10u);
      BOOST_CHECK_EQUAL(sc_number(view.read_primary({sc_tables[1],primary_query_kind::find,7},8).row->value),10u);
   }
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),sc_state_programs(0,false,1),{}),primary_state_unsupported);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),sc_state_programs(),{16,512*1024,true}),primary_state_unsupported);
}
BOOST_FIXTURE_TEST_CASE(child_erasure_invalidates_ancestor_cursor_even_after_recreation,sc_state_fixture) {
   isolated_sync_executor executor;
   const std::array operations{
      "(drop (call $get (get_local $n)(i32.const 64)(i32.const 8)))",
      "(drop (call $next (get_local $n)(i32.const 64)))",
      "(drop (call $prev (get_local $n)(i32.const 64)))"};
   for(bool recreate:{false,true}) for(const auto& op:operations) {
      const auto programs=sc_delete_programs(op,recreate);
      BOOST_CHECK_EXCEPTION(executor.execute(base,sc_root(),programs,{}),primary_state_unsupported,
         [](const auto& e){return dynamic_cast<const primary_state_missing*>(&e)==nullptr;});
      BOOST_CHECK_EQUAL(sc_number(view.read_primary({sc_tables[1],primary_query_kind::find,7},8).row->value),10u);
   }
   const auto fresh=sc_delete_programs(sc_check_row(sc_find("bob"_n),77),true);
   const auto result=executor.execute(base,sc_root(),fresh,{});
   BOOST_CHECK(validate_isolated_sync_journal(result,sc_root(),fresh,{}));
   BOOST_REQUIRE_EQUAL(result.state.writes().size(),2u);
   BOOST_CHECK(result.state.writes()[0].operation==primary_operation::remove);
   BOOST_CHECK(result.state.writes()[1].operation==primary_operation::store);
   BOOST_CHECK(result.state.writes()[0].object_id!=result.state.writes()[1].object_id);
}
BOOST_FIXTURE_TEST_CASE(sync_journal_rejects_reordered_duplicate_forged_and_readonly_mutations,sc_state_fixture) {
   isolated_sync_executor executor; const auto programs=sc_state_programs(0,true);
   const auto good=executor.execute(base,sc_root(),programs,{16,512*1024,false,false,true});
   const isolated_sync_context context{16,512*1024,false,false,true};
   BOOST_REQUIRE(validate_isolated_sync_journal(good,sc_root(),programs,context));
   auto bad=good; bad.calls[0].sender_ordinal=1;
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls[0].read_only=true;
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls[0].writes_end--;
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls[0].execution_fingerprint={};
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls[1].status=-1;
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.root_events.push_back(bad.root_events.front());
   // Repeating a pure query could be harmless; inserting a duplicate write cannot be.
   bad.root_events.push_back(isolated_cursor_event{isolated_cursor_write{0,0}});
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; std::reverse(bad.root_events.begin(),bad.root_events.end());
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.root_console_markers[0]=1;
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls[0].events.push_back(isolated_cursor_event{isolated_cursor_notify{0}});
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls[0].exclusive_elapsed=std::chrono::seconds(1);
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   bad=good; bad.calls.push_back(bad.calls.back());
   BOOST_CHECK(!validate_isolated_sync_journal(bad,sc_root(),programs,context));
   unsigned checkpoints=0;
   BOOST_CHECK_THROW(validate_isolated_sync_journal(good,sc_root(),programs,context,[&]{
      if(++checkpoints==15) throw std::runtime_error("owner cancellation must escape nested replay");
   }),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(sync_leaf_wasm_intervals_overlap_on_two_and_four_threads,sc_fixture) {
   const auto spin=R"((set_local $n (i32.const 10000))
      (loop $busy (set_local $n (i32.sub (get_local $n)(i32.const 1)))(br_if $busy (get_local $n))))";
   const auto programs=sc_pair(spin+sc_echo());
   for(unsigned threads:{1u,2u,4u}) {
      std::barrier gate(threads); std::vector<std::future<std::vector<isolated_sync_result>>> pending;
      for(unsigned i=0;i<threads;++i) pending.push_back(std::async(std::launch::async,[&,i]{
         isolated_sync_executor executor; std::vector<isolated_sync_result> results;
         gate.arrive_and_wait();
         for(unsigned j=i;j<4;j+=threads) results.push_back(executor.execute(base,sc_root(j),programs,{}));
         return results;
      }));
      std::vector<isolated_sync_result> results;
      for(auto& job:pending) for(auto& result:job.get()) results.push_back(std::move(result));
      BOOST_REQUIRE_EQUAL(results.size(),4u); bool overlap=false;
      for(size_t i=0;i<results.size();++i) {
         BOOST_REQUIRE_EQUAL(results[i].calls.size(),1u);
         const auto& a=results[i].calls[0]; BOOST_CHECK(a.executed); BOOST_CHECK(a.events.empty());
         for(size_t j=0;j<i;++j) {
            const auto& b=results[j].calls[0];
            if(std::max(a.began,b.began)<std::min(a.ended,b.ended)) overlap=true;
         }
      }
      BOOST_CHECK_EQUAL(overlap,threads>1); // leaf intervals exclude child compilation, host waiting and queued tasks
   }
}
BOOST_FIXTURE_TEST_CASE(sync_event_and_write_budgets_and_post_write_failure_discard_every_frame,sc_state_fixture) {
   isolated_sync_executor executor;
   for(unsigned count:{4096u,4097u}) {
      auto programs=sc_state_programs();
      programs[0].program.code=sc_program("(set_local $n (i32.const "+std::to_string(count)+"))"+
         "(loop $many (drop "+sc_find("test"_n)+")"+
         "(set_local $n (i32.sub (get_local $n)(i32.const 1)))(br_if $many (get_local $n)))",
         "(i64.const 0)",sc_db_imports);
      if(count==4096) {
         const auto result=executor.execute(base,sc_root(),programs,{});
         BOOST_CHECK_EQUAL(result.root_events.size(),4096u);
         BOOST_CHECK(validate_isolated_sync_journal(result,sc_root(),programs,{}));
      } else BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),primary_state_budget_exceeded);
   }
   primary_overlay_limits bounds; bounds.max_write_operations=2;
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),sc_state_programs(),{}, {},bounds),primary_state_budget_exceeded);
   bounds={}; bounds.max_write_value_bytes=16;
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),sc_state_programs(),{}, {},bounds),primary_state_budget_exceeded);
   auto programs=sc_state_programs();
   programs[1].program.code=sc_program({},sc_update(sc_find("bob"_n),22)+"unreachable (i64.const 0)",sc_db_imports);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}),eosio::vm::exception);
   programs[1].program.code=sc_program({},sc_update(sc_find("bob"_n),22)+"(loop $busy (br $busy))(i64.const 0)",sc_db_imports);
   isolated_cancel deadline; deadline.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
   BOOST_CHECK_THROW(executor.execute(base,sc_root(),programs,{}, {}, {},deadline),isolated_execution_interrupted);
   BOOST_CHECK_EQUAL(sc_number(view.read_primary({sc_tables[0],primary_query_kind::find,7},8).row->value),10u);
   BOOST_CHECK_EQUAL(sc_number(view.read_primary({sc_tables[1],primary_query_kind::find,7},8).row->value),10u);
   BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),sc_state_programs(),{}).root.return_value),33u);
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
void sc_oracle(std::vector<isolated_sync_program> programs, isolated_action_input input = sc_root(),
      std::span<const table_key> tables={},bool seed=true,bool expected_failure=false) {
   eosio::testing::tester chain;
   chain.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n});
   std::vector<name> accounts{"test"_n,"bob"_n,"carol"_n,"empty"_n,"absent"_n};
   for(auto& captured:programs) {
      const auto& p=captured.program;
      if(!p.code.empty()) chain.set_code(p.receiver,std::vector<uint8_t>(p.code.begin(),p.code.end()));
      const auto& metadata=chain.control->db().get<account_metadata_object,by_name>(p.receiver);
      if(!metadata.code_hash.empty()) {
         const auto& code=chain.control->db().get<code_object,by_code_hash>(
            boost::make_tuple(metadata.code_hash,metadata.vm_type,metadata.vm_version));
         captured.sync_call_supported=code.sync_call_supported;
      } else captured.sync_call_supported=false;
   }
   chain.produce_block();
   // Fixture-only initial state, before capture/transaction. The controller's
   // production mutable DB interface remains private, as in other oracle fixtures.
   if(seed) sc_seed(const_cast<chainbase::database&>(chain.control->db()),tables);
   chainbase_transaction_state_view view(chain.control->db());
   const auto base=sc_capture(view,tables,accounts);
   const auto& cfg=chain.control->get_global_properties().configuration;
   const isolated_sync_context context{cfg.max_sync_call_depth,cfg.max_sync_call_data_size,false,
      chain.control->is_builtin_activated(builtin_protocol_feature_t::get_sender_fix),chain.control->contracts_console()};
   isolated_sync_executor executor;
   std::optional<isolated_sync_result> attempt;
   if(expected_failure) BOOST_CHECK_EXCEPTION(executor.execute(base,input,programs,context),primary_state_unsupported,
      [](const auto& e){return dynamic_cast<const primary_state_missing*>(&e)==nullptr;});
   else {
      attempt=executor.execute(base,input,programs,context);
      BOOST_REQUIRE(validate_isolated_sync_journal(*attempt,input,programs,context));
   }
   const auto& resources=chain.control->get_resource_limits_manager();
   const auto bob_ram=resources.get_account_ram_usage("bob"_n);
   signed_transaction trx;
   action act; act.account=input.account; act.name=input.action; act.data=input.data;
   for(const auto& [actor,permission]:input.authorizations) act.authorization.push_back({actor,permission});
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("test"_n,"active"),chain.control->get_chain_id());
   const auto push=[&]{return chain.push_transaction(trx,fc::time_point::maximum(),eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US);};
   if(expected_failure) {
      BOOST_CHECK_THROW(push(),fc::exception);
      for(const auto& table:tables) for(uint64_t k:{7u,9u}) {
         const primary_query q{table,primary_query_kind::find,k};
         const auto before=base->read_primary(q,8), after=view.read_primary(q,8);
         BOOST_REQUIRE_EQUAL(bool(before.row),bool(after.row));
         if(before.row) { BOOST_CHECK(before.row->value==after.row->value); BOOST_CHECK_EQUAL(before.row->object_id,after.row->object_id); }
      }
      BOOST_CHECK_EQUAL(resources.get_account_ram_usage("bob"_n),bob_ram);
      return;
   }
   const auto actual=push(); const auto& result=*attempt;
   BOOST_REQUIRE(actual && !actual->except);
   BOOST_REQUIRE_EQUAL(actual->action_traces.size(),1u);
   const auto& root=actual->action_traces[0];
   BOOST_CHECK(root.return_value==result.root.return_value);
   BOOST_CHECK(root.console.empty());
   BOOST_REQUIRE_EQUAL(root.console_markers.size(),result.root_console_markers.size());
   for(size_t i=0;i<root.console_markers.size();++i) BOOST_CHECK_EQUAL(root.console_markers[i].value,result.root_console_markers[i]);
   BOOST_REQUIRE_EQUAL(root.call_traces.size(),result.calls.size());
   for(size_t i=0;i<result.calls.size();++i) {
      const auto& a=root.call_traces[i]; const auto& b=result.calls[i];
      BOOST_CHECK_EQUAL(a.call_ordinal.value,b.call_ordinal);
      BOOST_CHECK_EQUAL(a.sender_ordinal.value,b.sender_ordinal);
      BOOST_CHECK(a.receiver==b.receiver); BOOST_CHECK_EQUAL(a.read_only,b.read_only);
      BOOST_CHECK(a.data==b.data); BOOST_CHECK(a.return_value==b.return_value);
      BOOST_CHECK(a.console.empty()); BOOST_CHECK(!a.except); BOOST_CHECK(!a.error_code);
      BOOST_CHECK_EQUAL(bool(a.error_id),b.status<0);
      if(a.error_id) BOOST_CHECK_EQUAL(*a.error_id,b.status);
      BOOST_REQUIRE_EQUAL(a.console_markers.size(),b.console_markers.size());
      for(size_t j=0;j<a.console_markers.size();++j) BOOST_CHECK_EQUAL(a.console_markers[j].value,b.console_markers[j]);
   }
   // Canonical sync_call_context intentionally has no RAM accounting. Preserve
   // that original behavior; do not silently charge child writes to the root.
   BOOST_CHECK_EQUAL(resources.get_account_ram_usage("bob"_n),bob_ram);
   primary_state_overlay expected(base);
   for(const auto& write:result.state.writes()) switch(write.operation) {
      case primary_operation::store: expected.store(write.table,write.primary_key,write.payer,write.value); break;
      case primary_operation::update: expected.update_existing(write.table,write.primary_key,write.payer,write.value); break;
      case primary_operation::remove: expected.remove(write.table,write.primary_key); break;
   }
   for(const auto& table:tables) {
      const auto a=expected.read_table(table), b=view.read_table(table);
      BOOST_REQUIRE_EQUAL(bool(a),bool(b));
      if(a) { BOOST_CHECK_EQUAL(a->count,b->count); BOOST_CHECK(a->payer==b->payer); }
      for(uint64_t k:{7u,9u,11u}) {
         const primary_query q{table,primary_query_kind::find,k};
         const auto x=expected.read_primary(q,4096), y=view.read_primary(q,4096);
         BOOST_REQUIRE_EQUAL(bool(x.row),bool(y.row));
         if(x.row) { BOOST_CHECK(x.row->value==y.row->value); BOOST_CHECK(x.row->payer==y.row->payer); }
      }
   }
}
}
BOOST_AUTO_TEST_SUITE(parallel_sync_oracle_tests)
BOOST_AUTO_TEST_CASE(original_returns_recursive_memory_and_full_call_traces_match) {
   sc_oracle(sc_pair()); sc_oracle(sc_pair(sc_echo(),1));
   sc_oracle(sc_recursive(),sc_root(4)); sc_oracle(sc_preserved_return());
}
BOOST_AUTO_TEST_CASE(original_absence_empty_unsupported_and_business_statuses_match) {
   auto programs=sc_pair();
   programs[0].program.code=sc_program(sc_assert64(sc_call("absent"_n,~uint64_t{}),-1)+
      sc_assert64(sc_call("empty"_n),-1)+sc_assert64(sc_call("bob"_n),-2));
   programs[1].program.code=sc_program({}, {}, {}, {}, false);
   programs.push_back({{"empty"_n,{},{}},false});
   sc_oracle(programs);
   for(int64_t status : {-10000,-10001,-1,-9999,1}) {
      programs=sc_pair("(call $set (i32.const 0)(i32.const 8))(i64.const "+std::to_string(status)+")");
      programs[0].program.code=sc_program(sc_assert64(sc_call("bob"_n),status<=-10000?status:-3)+
         sc_assert32("(call $ret (i32.const 0)(i32.const 8))",0));
      sc_oracle(programs);
   }
}
BOOST_AUTO_TEST_CASE(original_shared_primary_writes_and_negative_status_side_effects_match) {
   for(int64_t status:{0,-10000,-1}) for(bool nested:{false,true})
      sc_oracle(sc_state_programs(status,nested),sc_root(),sc_tables);
   sc_oracle(sc_delete_programs(sc_check_row(sc_find("bob"_n),77),true),sc_root(),sc_tables);
   sc_oracle(sc_delete_programs(sc_assert32(sc_find("bob"_n),uint32_t(-2)),false),sc_root(),sc_tables);
}
BOOST_AUTO_TEST_CASE(original_stale_parent_iterators_and_readonly_rejection_roll_back_all_writes) {
   for(bool recreate:{false,true}) for(const auto& op:std::array{
         "(drop (call $get (get_local $n)(i32.const 64)(i32.const 8)))",
         "(drop (call $next (get_local $n)(i32.const 64)))",
         "(drop (call $prev (get_local $n)(i32.const 64)))"})
      sc_oracle(sc_delete_programs(op,recreate),sc_root(),sc_tables,true,true);
   sc_oracle(sc_state_programs(0,false,1),sc_root(),sc_tables,true,true);
   auto programs=sc_state_programs();
   programs[0].program.code=sc_program(sc_assert64(sc_call("bob"_n,1),0),sc_update(sc_find("test"_n),44)+"(i64.const 0)",sc_db_imports);
   programs[1].program.code=sc_program({},sc_assert64(sc_call("test"_n),0)+"(i64.const 0)",sc_db_imports);
   sc_oracle(programs,sc_root(),sc_tables,true,true); // read-only inherited by writable grandchild
}
BOOST_AUTO_TEST_CASE(original_sync_store_resize_payer_and_table_retirement_keep_original_ram_semantics) {
   auto programs=sc_pair();
   programs[0].program.code=sc_program(sc_assert64(sc_call("bob"_n),0));
   programs[1].program.code=sc_program({},sc_store(7,77,"absent"_n)+
      "(call $update "+sc_find("bob"_n)+sc_name("carol"_n)+"(i32.const 0)(i32.const 16))"+
      "(call $remove "+sc_find("bob"_n)+")"+sc_store(11,111,"absent"_n)+"(i64.const 0)",sc_db_imports);
   sc_oracle(programs,sc_root(),sc_tables,false);
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
// Real canonical metadata/state, but NO adopted transaction. These helpers
// only prove the read-only preflight boundary; original auth/CPU/undo and the
// synchronous replay capability still need separate integration tests.
struct sc_owner_fixture {
   eosio::testing::tester chain;
   isolated_action_input root = sc_root();
   std::vector<isolated_sync_program> programs;
   isolated_sync_context protocol;
   std::optional<packed_transaction> packed;
   std::optional<isolated_sync_result> result;
   sc_owner_fixture() {
      chain.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n});
      auto code = sc_state_programs(0,true);
      code.push_back({{"carol"_n,sc_program({},sc_echo()),{}},true}); // unused but fingerprinted
      code.push_back({{"empty"_n,{},{}},false});
      for(const auto& p:code) if(!p.program.code.empty())
         chain.set_code(p.program.receiver,std::vector<uint8_t>(p.program.code.begin(),p.program.code.end()));
      chain.produce_block();
      sc_seed(db(),sc_tables); // fixture setup only, never expose a production mutable DB API
      for(const auto& p:code) {
         auto captured=prepare_isolated_sync_program(*chain.control,p.program.receiver);
         BOOST_REQUIRE(captured); programs.push_back(std::move(*captured));
      }
      protocol=current_sync_context(*chain.control);
      root.max_return_bytes=chain.control->get_global_properties().configuration.max_action_return_value_size;
      signed_transaction trx; action act;
      act.account=root.account; act.name=root.action; act.data=root.data;
      for(const auto& [actor,permission]:root.authorizations) act.authorization.push_back({actor,permission});
      trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
      packed.emplace(trx,packed_transaction::compression_type::none);
      chainbase_transaction_state_view view(chain.control->db());
      const std::array accounts{"test"_n,"bob"_n,"carol"_n,"empty"_n,"absent"_n};
      isolated_sync_executor executor;
      result=executor.execute(sc_capture(view,sc_tables,accounts),root,programs,protocol);
      BOOST_REQUIRE(valid());
   }
   chainbase::database& db() { return const_cast<chainbase::database&>(chain.control->db()); }
   bool valid() const { return validate_isolated_sync_current_state(*chain.control,*packed,programs,*result); }
   template<typename F> void changed(F&& change) {
      auto undo=db().start_undo_session(true); change();
      BOOST_CHECK(!valid()); undo.undo(); BOOST_CHECK(valid());
   }
};
}

BOOST_AUTO_TEST_SUITE(parallel_sync_owner_tests)
BOOST_FIXTURE_TEST_CASE(owner_capture_uses_real_export_metadata_and_owns_code,sc_owner_fixture) {
   for(name n:{name{},"absent"_n,"eosio"_n,"eosio.msig"_n,"eosio.wrap"_n})
      BOOST_CHECK(!prepare_isolated_sync_program(*chain.control,n));
   const auto empty=prepare_isolated_sync_program(*chain.control,"empty"_n);
   BOOST_REQUIRE(empty); BOOST_CHECK(empty->program.code.empty()); BOOST_CHECK(!empty->sync_call_supported);
   const auto original=programs[1].program.code;
   auto owned=prepare_isolated_sync_program(*chain.control,"bob"_n);
   BOOST_REQUIRE(owned); BOOST_CHECK(owned->sync_call_supported);
   owned->program.code[0]^=1;
   BOOST_CHECK(prepare_isolated_sync_program(*chain.control,"bob"_n)->program.code==original);
   auto undo=db().start_undo_session(true);
   const auto& account=db().get<account_metadata_object,by_name>("bob"_n);
   const auto& code=db().get<code_object,by_code_hash>(boost::make_tuple(account.code_hash,account.vm_type,account.vm_version));
   db().modify(code,[](auto& c){c.sync_call_supported=false;});
   const auto unsupported=prepare_isolated_sync_program(*chain.control,"bob"_n);
   BOOST_REQUIRE(unsupported); BOOST_CHECK(unsupported->program.code==original);
   BOOST_CHECK(!unsupported->sync_call_supported); // export bytes still exist: never infer the canonical bit
   BOOST_CHECK(!valid()); undo.undo(); BOOST_CHECK(valid());
}

BOOST_FIXTURE_TEST_CASE(preflight_binds_entire_ordered_catalog_and_all_limits,sc_owner_fixture) {
   const auto original=programs;
   for(unsigned variant=0;variant<13;++variant) {
      programs=original;
      switch(variant) {
         case 0: programs.clear(); break;
         case 1: programs.push_back(programs.front()); break;
         case 2: programs.resize(65,programs.front()); break;
         case 3: std::swap(programs[1],programs[2]); break;
         case 4: programs[2].program.code[0]^=1; break; // unused receiver is still bound
         case 5: programs[2].sync_call_supported=!programs[2].sync_call_supported; break;
         case 6: programs[2].program.serial_only=true; break;
         case 7: --programs[1].program.limits.max_pages; break;
         case 8: --programs[1].program.limits.max_call_depth; break;
         case 9: --programs[1].program.limits.max_code_bytes; break;
         case 10: --programs[1].program.limits.max_action_bytes; break;
         case 11: --programs[1].program.limits.max_authorizations; break;
         case 12: --programs[1].program.limits.max_return_bytes; break;
      }
      BOOST_TEST_CONTEXT("catalog variant="<<variant) { BOOST_CHECK(!valid()); }
   }
   programs=original; programs[1].program.limits.max_attempt_time+=std::chrono::milliseconds(1);
   BOOST_CHECK(!valid()); programs=original; BOOST_CHECK(valid());
   auto corrupt=*result;
   corrupt.calls[0].read_only=true;
   BOOST_CHECK(!validate_isolated_sync_current_state(*chain.control,*packed,programs,corrupt));
   corrupt=*result; ++std::get<isolated_cursor_write>(std::get<isolated_cursor_event>(corrupt.root_events[1])).write_index;
   BOOST_CHECK(!validate_isolated_sync_current_state(*chain.control,*packed,programs,corrupt));
}

BOOST_FIXTURE_TEST_CASE(current_receiver_code_vm_privilege_and_unused_code_changes_invalidate,sc_owner_fixture) {
   for(name receiver:{"test"_n,"bob"_n,"carol"_n}) for(unsigned variant=0;variant<4;++variant) {
      BOOST_TEST_CONTEXT("receiver="<<receiver<<" variant="<<variant) {
         changed([&]{db().modify(db().get<account_metadata_object,by_name>(receiver),[&](auto& a){
            if(variant==0) a.code_hash=fc::sha256::hash("different current code");
            if(variant==1) a.vm_type=1;
            if(variant==2) a.vm_version=1;
            if(variant==3) a.set_privileged(true);
         });});
      }
   }
}

BOOST_FIXTURE_TEST_CASE(current_protocol_and_return_limits_cannot_reuse_old_proof,sc_owner_fixture) {
   for(unsigned variant=0;variant<3;++variant) changed([&]{
      db().modify(chain.control->get_global_properties(),[&](auto& g){
         if(variant==0) --g.configuration.max_sync_call_depth;
         if(variant==1) --g.configuration.max_sync_call_data_size;
         if(variant==2) --g.configuration.max_action_return_value_size;
      });
   });
   for(unsigned variant=0;variant<3;++variant) {
      auto altered=protocol;
      if(variant==0) altered.get_sender_fix=!altered.get_sender_fix;
      if(variant==1) altered.console_enabled=!altered.console_enabled;
      if(variant==2) altered.root_read_only=true;
      auto corrupt=*result; corrupt.execution_fingerprint=isolated_sync_fingerprint(root,programs,altered);
      BOOST_CHECK(!validate_isolated_sync_current_state(*chain.control,*packed,programs,corrupt));
   }
   BOOST_REQUIRE(chain.control->is_builtin_activated(builtin_protocol_feature_t::configurable_wasm_limits));
   changed([&]{db().modify(chain.control->get_global_properties(),[](auto& g){g.wasm_configuration.max_pages=8;});});
   changed([&]{db().modify(chain.control->get_global_properties(),[](auto& g){g.wasm_configuration.max_call_depth=32;});});
}

BOOST_FIXTURE_TEST_CASE(current_exact_row_dependencies_and_structural_changes_are_checked_without_writes,sc_owner_fixture) {
   const auto& table=db().get<table_id_object,by_code_scope_table>(boost::make_tuple("bob"_n,"scope"_n,"rows"_n));
   changed([&]{db().modify(db().get<key_value_object,by_scope_primary>(boost::make_tuple(table.id,uint64_t{7})),
      [](auto& row){const uint64_t value=11; row.value.assign(reinterpret_cast<const char*>(&value),sizeof(value));});});
   changed([&]{db().modify(table,[](auto& t){++t.count;});});
   { auto undo=db().start_undo_session(true);
      db().modify(db().get<key_value_object,by_scope_primary>(boost::make_tuple(table.id,uint64_t{9})),
         [](auto& row){const uint64_t value=91; row.value.assign(reinterpret_cast<const char*>(&value),sizeof(value));});
      BOOST_CHECK(valid()); // different value in same table, not an observed row
   }
   BOOST_CHECK(valid());
   chainbase_transaction_state_view view(chain.control->db());
   for(const auto& key:sc_tables)
      BOOST_CHECK_EQUAL(sc_number(view.read_primary({key,primary_query_kind::find,7},8).row->value),10u);
}

BOOST_FIXTURE_TEST_CASE(transaction_input_and_single_ordinary_root_boundaries_are_not_authorization,sc_owner_fixture) {
   // This read-only proof accepts an UNSIGNED fixture. Only the later original
   // controller authorization path can authorize any canonical transaction.
   BOOST_CHECK(packed->get_signed_transaction().signatures.empty());
   for(unsigned variant=0;variant<7;++variant) {
      auto trx=packed->get_signed_transaction();
      switch(variant) {
         case 0: trx.actions.clear(); break;
         case 1: trx.actions.push_back(trx.actions.front()); break;
         case 2: trx.context_free_actions.push_back(trx.actions.front()); break;
         case 3: trx.delay_sec=1; break;
         case 4: trx.actions[0].data[0]^=1; break;
         case 5: trx.actions[0].authorization[0].permission="owner"_n; break;
         case 6: trx.actions[0].name="other"_n; break;
      }
      const packed_transaction altered(trx,packed_transaction::compression_type::none);
      BOOST_CHECK(!validate_isolated_sync_current_state(*chain.control,altered,programs,*result));
   }
   BOOST_CHECK(valid());
}

BOOST_FIXTURE_TEST_CASE(owner_checkpoint_exceptions_propagate_and_leave_current_state_unchanged,sc_owner_fixture) {
   for(unsigned at:{1u,3u,20u}) {
      unsigned checks=0;
      BOOST_CHECK_THROW(validate_isolated_sync_current_state(*chain.control,*packed,programs,*result,[&]{
         if(++checks==at) throw std::runtime_error("owner preflight cancelled");
      }),std::runtime_error);
      BOOST_CHECK_EQUAL(checks,at); BOOST_CHECK(valid());
   }
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
void sc_check_no_result(const isolated_worker_outcome& out, isolated_worker_failure expected) {
   BOOST_CHECK(!out.result); BOOST_CHECK(!out.notification_result);
   BOOST_CHECK(!out.tree_result); BOOST_CHECK(!out.sync_result);
   BOOST_CHECK_EQUAL(out.elapsed.count(),0);
   BOOST_CHECK_MESSAGE(out.failure==expected,isolated_worker_failure_name(out.failure));
}
std::vector<isolated_sync_program> sc_keyed_programs(bool lifecycle=false) {
   const auto find="(call $find "+sc_name("bob"_n)+sc_name("scope"_n)+sc_name("rows"_n)+"(i64.load (i32.const 0)))";
   const auto child="(drop (call $data (i32.const 0)(i32.const 8)))"+
      (lifecycle ? "(call $remove "+find+")"+sc_store(7,22) : sc_update(find,22))+
      "(call $set (i32.const 64)(i32.const 8))(i64.const 0)";
   return {{{"test"_n,sc_program(sc_root_body()),{}},true},
           {{"bob"_n,sc_program({},child,sc_db_imports),{}},true}};
}
}

BOOST_AUTO_TEST_SUITE(parallel_sync_pool_tests)
BOOST_FIXTURE_TEST_CASE(sync_stage_metrics_count_each_frame_once_including_failed_attempts,sc_state_fixture) {
   auto& metrics=performance::metrics();
   struct restore_flag {
      std::atomic<bool>& flag;bool previous;
      ~restore_flag(){flag.store(previous,std::memory_order_relaxed);}
   } restore{metrics.enabled,metrics.enabled.exchange(true,std::memory_order_relaxed)};
   isolated_sync_executor executor;
   const auto pair=sc_pair(),recursive=sc_recursive(),stateful=sc_state_programs(0,true),negative=sc_state_programs(-10000,true);
   auto trap=sc_state_programs();
   trap[1].program.code=sc_program({},sc_update(sc_find("bob"_n),22)+"(unreachable)(i64.const 0)",sc_db_imports);
   auto missing=sc_pair();
   missing[0].program.code=sc_program(sc_assert64(sc_call("absent"_n,~uint64_t{}),-1)+sc_root_body());
   auto invalid_child=sc_pair();invalid_child[1].program.code={'x'};
   auto invalid_root=sc_pair();invalid_root[0].program.code={'x'};
   const auto observe=[&](const auto& run,size_t expected_compile,size_t expected_execute) {
      const auto cb=metrics.snapshot(performance::stage::parallel_compile),eb=metrics.snapshot(performance::stage::parallel_execute);
      const auto began=std::chrono::steady_clock::now();run();
      const auto wall=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
      const auto ca=metrics.snapshot(performance::stage::parallel_compile),ea=metrics.snapshot(performance::stage::parallel_execute);
      BOOST_CHECK_EQUAL(ca.count-cb.count,expected_compile);BOOST_CHECK_EQUAL(ea.count-eb.count,expected_execute);
      BOOST_CHECK_GT(ca.nanoseconds-cb.nanoseconds,0u);
      if(expected_execute) BOOST_CHECK_GT(ea.nanoseconds-eb.nanoseconds,0u);
      else BOOST_CHECK_EQUAL(ea.nanoseconds-eb.nanoseconds,0u);
      // Direct execution on ONE thread: nested intervals must form disjoint
      // compilation and exclusive-VM samples, even during exception unwind.
      // Never infer a speedup or relax an attempt deadline from this diagnostic.
      BOOST_CHECK_LE((ca.nanoseconds-cb.nanoseconds)+(ea.nanoseconds-eb.nanoseconds),static_cast<uint64_t>(wall));
   };
   for(unsigned round=0;round<3;++round) {
      observe([&]{BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),pair,{}).root.return_value),5u);},2,2);
      observe([&]{BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),missing,{}).root.return_value),5u);},2,2);
      observe([&]{BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(3),recursive,{}).root.return_value),4u);},5,5);
      observe([&]{BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),stateful,{}).root.return_value),33u);},3,3);
      observe([&]{BOOST_CHECK_EQUAL(sc_number(executor.execute(base,sc_root(),negative,{}).root.return_value),33u);},3,3);
      observe([&]{BOOST_CHECK_THROW(executor.execute(base,sc_root(),trap,{}),eosio::vm::exception);},2,2);
      observe([&]{BOOST_CHECK_THROW(executor.execute(base,sc_root(),invalid_child,{}),eosio::vm::exception);},2,1);
      observe([&]{BOOST_CHECK_THROW(executor.execute(base,sc_root(),invalid_root,{}),eosio::vm::exception);},1,0);
   }
   const auto original=view.read_primary({sc_tables[0],primary_query_kind::find,7},8);
   BOOST_REQUIRE(original.row);BOOST_CHECK_EQUAL(sc_number(original.row->value),10u);
}
BOOST_FIXTURE_TEST_CASE(ordinary_root_host_call_requests_one_local_mode_upgrade_not_export_inference,sc_state_fixture) {
   for(const auto& body:std::array{sc_root_body(),std::string("(drop (call $ret (i32.const 0)(i32.const 0)))")}) {
      // The caller deliberately has NO sync export. Export support describes
      // a callee, not whether this apply entry needs synchronous execution.
      const std::vector<isolated_receiver_program> programs{{"test"_n,sc_program(body,"",{}, {},false),{}}};
      const std::vector roots{sc_root()}; isolated_action_tree_context context;
      isolated_action_tree_executor executor;
      BOOST_CHECK_THROW(executor.execute(base,roots,programs,context),isolated_sync_mode_required);
      isolated_transaction_executor legacy;
      const std::vector<isolated_action_invocation> calls{{programs[0].code,roots[0],{}}};
      BOOST_CHECK_EXCEPTION(legacy.execute(base,calls),primary_state_unsupported,
         [](const auto& e){return dynamic_cast<const isolated_sync_mode_required*>(&e)==nullptr;});
      const std::array root_fingerprints{isolated_execution_fingerprint(
         fc::sha256::hash(programs[0].code.data(),programs[0].code.size()),programs[0].limits,roots[0])};
      const std::array requests{ordered_attempt_batch::request{fc::sha256::hash("sync hint"),
         isolated_action_tree_fingerprint(root_fingerprints,context)}};
      ordered_attempt_batch batch(base,requests); isolated_worker_pool pool(1);
      const auto outcomes=pool.run({isolated_worker_job{batch.get_ticket(0),roots,programs,context}});
      sc_check_no_result(outcomes[0],isolated_worker_failure::sync_required);
      BOOST_CHECK(!outcomes[0].request); BOOST_CHECK(!outcomes[0].receiver_request);
   }
   // Merely exporting sync_call does not request a mode change.
   const std::vector<isolated_receiver_program> plain{{"test"_n,sc_program(),{}}};
   isolated_action_tree_executor executor;
   BOOST_CHECK_NO_THROW(executor.execute(base,std::vector{sc_root()},plain,{}));
}
BOOST_FIXTURE_TEST_CASE(four_exclusive_owned_modes_switch_on_one_two_four_workers,sc_state_fixture) {
   const auto root=sc_root();
   const auto sync_programs=sc_state_programs(0,true);
   const auto plain=sc_program("(drop (call $read (i32.const 0)(i32.const 8)))(call $out (i32.const 0)(i32.const 8))");
   const std::vector<isolated_action_invocation> actions{{plain,root,{}}};
   const std::vector roots{root};
   const std::vector<isolated_receiver_program> programs{{"test"_n,plain,{}}};
   const isolated_action_tree_context tree_context;
   isolated_transaction_executor legacy; isolated_notification_executor notification;
   isolated_action_tree_executor tree; isolated_sync_executor sync;
   const std::array fingerprints{
      legacy.execute(base,actions).execution_fingerprint,
      notification.execute(base,roots,programs).root_fingerprint,
      tree.execute(base,roots,programs,tree_context).root_fingerprint,
      sync.execute(base,root,sync_programs,{}).execution_fingerprint};
   for(size_t threads:{1u,2u,4u}) {
      isolated_worker_pool pool(threads);
      for(unsigned round=0;round<3;++round) {
         std::vector<ordered_attempt_batch::request> requests;
         for(size_t i=0;i<8;++i) requests.push_back({fc::sha256::hash(std::to_string(i)),fingerprints[i%4]});
         ordered_attempt_batch batch(base,requests); std::vector<isolated_worker_job> jobs;
         for(size_t i=0;i<requests.size();++i) switch(i%4) {
            case 0: jobs.emplace_back(batch.get_ticket(i),actions); break;
            case 1: jobs.emplace_back(batch.get_ticket(i),roots,programs); break;
            case 2: jobs.emplace_back(batch.get_ticket(i),roots,programs,tree_context); break;
            case 3: {
               // Deliberately mutate and destroy the original argument buffers
               // after the job has taken its own copy, before any worker runs.
               auto owned_input=root; auto owned_programs=sync_programs;
               jobs.emplace_back(batch.get_ticket(i),owned_input,owned_programs,isolated_sync_context{});
               owned_input.data.assign(8,0); owned_programs[0].program.code.clear();
               break;
            }
         }
         const auto outputs=pool.run(std::move(jobs));
         BOOST_REQUIRE_EQUAL(outputs.size(),requests.size());
         for(size_t i=0;i<outputs.size();++i) {
            const auto& out=outputs[i];
            BOOST_CHECK_EQUAL(bool(out.result),i%4==0);
            BOOST_CHECK_EQUAL(bool(out.notification_result),i%4==1);
            BOOST_CHECK_EQUAL(bool(out.tree_result),i%4==2);
            BOOST_CHECK_EQUAL(bool(out.sync_result),i%4==3);
            BOOST_CHECK_MESSAGE(out.failure==isolated_worker_failure::none,isolated_worker_failure_name(out.failure));
            BOOST_CHECK(!out.request); BOOST_CHECK(!out.receiver_request);
            if(i%4!=3) continue;
            BOOST_REQUIRE(out.sync_result);
            const auto& result=*out.sync_result;
            BOOST_CHECK(validate_isolated_sync_journal(result,root,sync_programs,{}));
            BOOST_CHECK_EQUAL(sc_number(result.root.return_value),33u);
            BOOST_CHECK(result.execution_fingerprint==fingerprints[3]);
            auto elapsed=result.root.elapsed;
            for(const auto& call:result.calls) elapsed+=call.exclusive_elapsed;
            BOOST_CHECK(out.elapsed==elapsed);
            BOOST_CHECK(out.began==result.root.began); BOOST_CHECK(out.ended==result.root.ended);
            BOOST_CHECK(out.elapsed<=out.ended-out.began); BOOST_CHECK(out.attempt_wall>=out.elapsed);
         }
         for(const auto& table:sc_tables)
            BOOST_CHECK_EQUAL(sc_number(view.read_primary({table,primary_query_kind::find,7},8).row->value),10u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(sync_failures_discard_parent_writes_and_keep_pool_reusable,sc_state_fixture) {
   const auto programs=sc_state_programs(); const auto root=sc_root();
   const std::array<ordered_attempt_batch::request,1> requests{{
      {fc::sha256::hash("sync"),isolated_sync_fingerprint(root,programs,{})}}};
   ordered_attempt_batch batch(base,requests); isolated_worker_pool pool(2);
   const isolated_worker_job good{batch.get_ticket(0),root,programs,isolated_sync_context{}};
   const std::array reasons{isolated_worker_failure::missing_receiver,isolated_worker_failure::unsupported,
      isolated_worker_failure::vm_error,isolated_worker_failure::deadline,isolated_worker_failure::unsupported,
      isolated_worker_failure::budget};
   for(size_t failure=0;failure<reasons.size();++failure) {
      auto bad=good;
      switch(failure) {
         case 0: bad.sync->programs.pop_back(); break;
         case 1: bad.sync->programs[1].program.serial_only=true; break;
         case 2: bad.sync->programs[1].program.code=sc_program({},
            sc_update(sc_find("bob"_n),22)+"unreachable (i64.const 0)",sc_db_imports); break;
         case 3: bad.cancel.deadline=std::chrono::steady_clock::now(); break;
         case 4: bad.sync->context.root_read_only=true; break;
         case 5: bad.sync->programs.push_back(bad.sync->programs[1]); break;
      }
      const auto outputs=pool.run({bad,good});
      sc_check_no_result(outputs[0],reasons[failure]);
      BOOST_CHECK(!outputs[0].request);
      BOOST_CHECK_EQUAL(bool(outputs[0].receiver_request),failure==0);
      if(failure==0) BOOST_CHECK(*outputs[0].receiver_request=="bob"_n);
      BOOST_REQUIRE(outputs[1].sync_result);
      BOOST_CHECK(validate_isolated_sync_journal(*outputs[1].sync_result,root,programs,{}));
   }
   const auto sparse=frozen_primary_state::capture_queries(view,{},{});
   ordered_attempt_batch missing(sparse,requests);
   const auto outputs=pool.run({{missing.get_ticket(0),root,programs,isolated_sync_context{}},good});
   sc_check_no_result(outputs[0],isolated_worker_failure::missing_state);
   BOOST_REQUIRE(outputs[0].request); BOOST_CHECK(!outputs[0].receiver_request);
   BOOST_REQUIRE(outputs[1].sync_result);
   ordered_attempt_batch cancelled(base,requests); const auto tag=cancelled.get_ticket(0); cancelled.close();
   const auto stopped=pool.run({{tag,root,programs,isolated_sync_context{}},good});
   sc_check_no_result(stopped[0],isolated_worker_failure::cancelled);
   BOOST_REQUIRE(stopped[1].sync_result);
   const auto negative=sc_state_programs(-10000,true);
   const auto business=pool.run({{batch.get_ticket(0),root,negative,isolated_sync_context{}}});
   BOOST_REQUIRE(business[0].sync_result); BOOST_REQUIRE_EQUAL(business[0].sync_result->calls.size(),2u);
   BOOST_CHECK_EQUAL(business[0].sync_result->calls[0].status,-10000);
   BOOST_CHECK_EQUAL(business[0].sync_result->state.writes().size(),4u);
   BOOST_CHECK(business[0].failure==isolated_worker_failure::none);
   for(const auto& table:sc_tables)
      BOOST_CHECK_EQUAL(sc_number(view.read_primary({table,primary_query_kind::find,7},8).row->value),10u);
}

BOOST_FIXTURE_TEST_CASE(sync_input_bounds_and_all_mixed_modes_are_rejected_before_dispatch,sc_fixture) {
   const auto root=sc_root(); const auto programs=sc_pair();
   const std::array<ordered_attempt_batch::request,1> requests{{
      {fc::sha256::hash("bounds"),isolated_sync_fingerprint(root,programs,{})}}};
   ordered_attempt_batch batch(base,requests); isolated_worker_pool pool(1);
   const isolated_worker_job good{batch.get_ticket(0),root,programs,isolated_sync_context{}};
   for(unsigned failure=0;failure<10;++failure) {
      auto bad=good; auto& input=*bad.sync;
      switch(failure) {
         case 0: bad.actions.push_back({programs[0].program.code,root,{}}); break;
         case 1: bad.notifications=isolated_notification_job{{root},{programs[0].program}}; break;
         case 2: bad.tree=isolated_action_tree_job{{root},{programs[0].program},{}}; break;
         case 3: input.programs.clear(); break;
         case 4: input.programs.resize(65,programs[0]); break;
         case 5: input.programs[0].program.code.resize(65537); break;
         case 6: input.root.data.resize(65537); break;
         case 7: input.root.authorizations.resize(65); break;
         case 8: input.root.max_return_bytes=4097; break;
         case 9: {
            input.programs.resize(5,programs[0]);
            for(auto& program:input.programs) program.program.code.resize(65536);
            break;
         }
      }
      BOOST_CHECK_THROW(pool.run({bad}),primary_state_budget_exceeded);
      BOOST_REQUIRE(pool.run({good})[0].sync_result);
   }
   auto empty=good; empty.sync.reset();
   BOOST_CHECK_THROW(pool.run({empty}),primary_state_budget_exceeded);
   auto wrong=std::async(std::launch::async,[&] {
      try { (void)pool.run({good}); } catch(const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(wrong.get()); BOOST_REQUIRE(pool.run({good})[0].sync_result);
}

BOOST_FIXTURE_TEST_CASE(timed_generation_cancellation_and_child_loop_deadline_discard_entire_jobs,sc_state_fixture) {
   const auto root=sc_root(); const auto programs=sc_state_programs();
   const auto busy=sc_program({},sc_update(sc_find("bob"_n),22)+
      "(loop $forever (br $forever))(i64.const 0)",sc_db_imports);
   const std::array<ordered_attempt_batch::request,1> requests{{
      {fc::sha256::hash("cancel"),isolated_sync_fingerprint(root,programs,{})}}};
   ordered_attempt_batch batch(base,requests); isolated_worker_pool pool(2);
   const isolated_worker_job good{batch.get_ticket(0),root,programs,isolated_sync_context{}};
   for(bool interrupt:{true,false}) {
      auto bad=good; bad.sync->programs[1].program.code=busy;
      const auto generation=std::make_shared<std::atomic<uint64_t>>(0);
      std::jthread signal;
      if(interrupt) {
         bad.cancel.generation=generation;
         signal=std::jthread([generation] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            generation->fetch_add(1,std::memory_order_relaxed);
         });
      } else bad.cancel.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
      // Timed cancellation, not a claim that a particular frame/instruction
      // has been reached: scheduler delays may consume preparation time too.
      const auto outputs=pool.run({bad,good});
      sc_check_no_result(outputs[0],interrupt?isolated_worker_failure::cancelled:isolated_worker_failure::deadline);
      BOOST_CHECK(!outputs[0].request); BOOST_CHECK(!outputs[0].receiver_request);
      BOOST_REQUIRE(outputs[1].sync_result);
      BOOST_REQUIRE(pool.run({good})[0].sync_result);
      for(const auto& table:sc_tables)
         BOOST_CHECK_EQUAL(sc_number(view.read_primary({table,primary_query_kind::find,7},8).row->value),10u);
   }
}

BOOST_FIXTURE_TEST_CASE(sync_child_dependencies_control_ordered_conflicts_and_structural_aba,sc_state_fixture) {
   const auto programs=sc_keyed_programs(); isolated_worker_pool pool(4);
   const std::array<uint64_t,4> keys{7,9,7,9};
   std::vector<ordered_attempt_batch::request> requests;
   for(size_t i=0;i<keys.size();++i) requests.push_back({fc::sha256::hash(std::to_string(i)),
      isolated_sync_fingerprint(sc_root(keys[i]),programs,{})});
   ordered_attempt_batch batch(base,requests); std::vector<isolated_worker_job> jobs;
   for(size_t i=0;i<keys.size();++i) jobs.emplace_back(batch.get_ticket(i),sc_root(keys[i]),programs,isolated_sync_context{});
   const auto outputs=pool.run(std::move(jobs)); unsigned adopted=0,fallback=0;
   for(size_t i=0;i<outputs.size();++i) {
      BOOST_REQUIRE(outputs[i].sync_result);
      BOOST_CHECK(validate_isolated_sync_journal(*outputs[i].sync_result,sc_root(keys[i]),programs,{}));
      // This callback is an ordering probe only, NOT a canonical adopter.
      batch.run_next(batch.get_ticket(i),requests[i].transaction_id,outputs[i].sync_result,
         [&](const isolated_sync_result&) { ++adopted; },[&] { ++fallback; });
      BOOST_CHECK_EQUAL(adopted,std::min(i+1,size_t{2}));
      BOOST_CHECK_EQUAL(fallback,i<2?0u:unsigned(i-1));
   }
   BOOST_CHECK(batch.complete());
   BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(0),requests[0].transaction_id,outputs[0].sync_result,
      [](const isolated_sync_result&) {},[] {}),primary_state_invalid_attempt);
   const auto lifecycle=sc_keyed_programs(true);
   const std::array<ordered_attempt_batch::request,2> changes{{
      {fc::sha256::hash("lifecycle"),isolated_sync_fingerprint(sc_root(7),lifecycle,{})},
      {fc::sha256::hash("unrelated"),isolated_sync_fingerprint(sc_root(9),programs,{})}}};
   ordered_attempt_batch structure(base,changes);
   const auto structural=pool.run({{structure.get_ticket(0),sc_root(7),lifecycle,isolated_sync_context{}},
                                  {structure.get_ticket(1),sc_root(9),programs,isolated_sync_context{}}});
   adopted=fallback=0;
   for(size_t i=0;i<changes.size();++i) {
      BOOST_REQUIRE(structural[i].sync_result);
      structure.run_next(structure.get_ticket(i),changes[i].transaction_id,structural[i].sync_result,
         [&](const isolated_sync_result&) { ++adopted; },[&] { ++fallback; });
   }
   BOOST_CHECK_EQUAL(adopted,1u); BOOST_CHECK_EQUAL(fallback,1u);
   BOOST_CHECK_EQUAL(sc_number(view.read_primary({sc_tables[1],primary_query_kind::find,7},8).row->value),10u);
}

BOOST_FIXTURE_TEST_CASE(sync_coordinator_rejects_foreign_fingerprints_bases_and_callback_reentry,sc_fixture) {
   const auto root=sc_root(); const auto programs=sc_pair(); isolated_sync_executor executor;
   const std::optional good{executor.execute(base,root,programs,{})};
   const std::array<ordered_attempt_batch::request,1> requests{{
      {fc::sha256::hash("identity"),good->execution_fingerprint}}};
   const auto adopt=[](const isolated_sync_result&) {}; const auto serial=[] {};
   for(unsigned failure=0;failure<3;++failure) {
      ordered_attempt_batch batch(base,requests); auto bad=good;
      if(failure==0) bad->execution_fingerprint=fc::sha256::hash("wrong");
      if(failure==1) {
         const std::array names{"test"_n,"bob"_n};
         const auto other=frozen_primary_state::capture_queries(view,{},{},{},{},names);
         bad=executor.execute(other,root,programs,{});
      }
      if(failure==2) {
         ordered_attempt_batch foreign(base,requests);
         BOOST_CHECK_THROW(batch.run_next(foreign.get_ticket(0),requests[0].transaction_id,bad,adopt,serial),primary_state_invalid_attempt);
         batch.run_next(batch.get_ticket(0),requests[0].transaction_id,good,adopt,serial);
         BOOST_CHECK(batch.complete());
      } else {
         const auto tag=batch.get_ticket(0);
         BOOST_CHECK_THROW(batch.run_next(tag,requests[0].transaction_id,bad,adopt,serial),primary_state_invalid_attempt);
         BOOST_CHECK(tag.stop_token().stop_requested());
         BOOST_CHECK_THROW(batch.get_ticket(0),primary_state_invalid_attempt);
      }
   }
   ordered_attempt_batch reentry(base,requests); const auto tag=reentry.get_ticket(0);
   reentry.run_next(tag,requests[0].transaction_id,good,[&](const isolated_sync_result&) {
      BOOST_CHECK_THROW(reentry.run_next(tag,requests[0].transaction_id,good,adopt,serial),primary_state_invalid_attempt);
   },serial);
   BOOST_CHECK(reentry.complete());
   ordered_attempt_batch throwing(base,requests); const auto once=throwing.get_ticket(0);
   unsigned entered=0,retried=0;
   BOOST_CHECK_THROW(throwing.run_next(once,requests[0].transaction_id,good,[&](const isolated_sync_result&) {
      ++entered;
      throw std::runtime_error("test canonical callback failure");
   },[&] { ++retried; }),std::runtime_error);
   BOOST_CHECK_EQUAL(entered,1u); BOOST_CHECK_EQUAL(retried,0u);
   BOOST_CHECK(once.stop_token().stop_requested());
   BOOST_CHECK_THROW(throwing.run_next(once,requests[0].transaction_id,good,adopt,serial),primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(sync_pool_overlaps_actual_leaf_wasm_not_parent_compile_intervals,sc_fixture) {
   const auto busy=R"((set_local $n (i32.const 10000))
      (block $done (loop $again (br_if $done (i32.eqz (get_local $n)))
         (set_local $n (i32.sub (get_local $n)(i32.const 1))) (br $again))))";
   const auto programs=sc_pair(busy+sc_echo());
   for(size_t threads:{1u,2u,4u}) {
      isolated_worker_pool pool(threads); std::vector<ordered_attempt_batch::request> requests;
      for(size_t i=0;i<4;++i) requests.push_back({fc::sha256::hash(std::to_string(i)),
         isolated_sync_fingerprint(sc_root(i+1),programs,{})});
      ordered_attempt_batch batch(base,requests); std::vector<isolated_worker_job> jobs;
      for(size_t i=0;i<4;++i) jobs.emplace_back(batch.get_ticket(i),sc_root(i+1),programs,isolated_sync_context{});
      const auto outcomes=pool.run(std::move(jobs)); bool overlap=false;
      for(size_t i=0;i<outcomes.size();++i) {
         BOOST_REQUIRE_MESSAGE(outcomes[i].sync_result,isolated_worker_failure_name(outcomes[i].failure));
         const auto& result=*outcomes[i].sync_result;
         BOOST_CHECK_EQUAL(sc_number(result.root.return_value),i+1);
         BOOST_REQUIRE_EQUAL(result.calls.size(),1u);
         const auto& leaf=result.calls[0]; BOOST_CHECK(leaf.executed); BOOST_CHECK(leaf.events.empty());
         for(size_t j=0;j<i;++j) {
            const auto& other=outcomes[j].sync_result->calls[0];
            overlap|=std::max(leaf.began,other.began)<std::min(leaf.ended,other.ended);
         }
      }
      BOOST_CHECK_EQUAL(overlap,threads>1);
   }
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
struct sc_live_chain:eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time()+fc::microseconds(config::block_interval_us)); }
};
signed_transaction sc_live_trx(eosio::testing::tester& chain,uint64_t key,uint64_t nonce,name account="test"_n,
                               name method="run"_n) {
   signed_transaction trx; action act;
   act.account=account; act.name=method; act.authorization={{account,"active"_n}};
   const std::array<uint64_t,2> data{key,nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()),reinterpret_cast<const char*>(data.data())+sizeof(data));
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(account,"active"),chain.control->get_chain_id()); return trx;
}
transaction_metadata_ptr sc_live_meta(eosio::testing::tester& chain,const signed_transaction& trx) {
   return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx,packed_transaction::compression_type::none),
      chain.control->get_chain_id(),fc::seconds(5),transaction_metadata::trx_type::input);
}
transaction_trace_ptr sc_live_push(eosio::testing::tester& chain,const transaction_metadata_ptr& trx) {
   return chain.control->push_transaction(trx,fc::time_point::maximum(),fc::seconds(1),0,false,0);
}
// SHiP omits call_traces. Compare every non-timing call field explicitly; two
// independent clocks are never required to have equal diagnostic elapsed.
std::vector<char> sc_live_trace(const transaction_trace_ptr& trace) {
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id),fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->except?trace->except->code():int64_t{0}),fc::raw::pack(bool(trace->receipt))};
   if(trace->receipt) fields.push_back(fc::raw::pack(*trace->receipt));
   for(const auto& a:trace->action_traces) {
      fields.push_back(fc::raw::pack(a.action_ordinal)); fields.push_back(fc::raw::pack(a.creator_action_ordinal));
      fields.push_back(fc::raw::pack(a.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(a.receiver)); fields.push_back(fc::raw::pack(a.act));
      fields.push_back(fc::raw::pack(a.context_free)); fields.push_back(fc::raw::pack(bool(a.receipt)));
      if(a.receipt) fields.push_back(fc::raw::pack(*a.receipt));
      fields.push_back(a.return_value); fields.push_back(fc::raw::pack(a.account_ram_deltas));
      fields.push_back(fc::raw::pack(a.error_code)); fields.push_back(fc::raw::pack(a.except?a.except->code():int64_t{0}));
      fields.push_back(fc::raw::pack(a.console)); fields.push_back(fc::raw::pack(a.console_markers));
      fields.push_back(fc::raw::pack(a.call_traces.size()));
      for(const auto& c:a.call_traces) {
         fields.push_back(fc::raw::pack(c.call_ordinal)); fields.push_back(fc::raw::pack(c.sender_ordinal));
         fields.push_back(fc::raw::pack(c.receiver)); fields.push_back(fc::raw::pack(c.read_only));
         fields.push_back(c.data); fields.push_back(c.return_value); fields.push_back(fc::raw::pack(c.console));
         fields.push_back(fc::raw::pack(c.console_markers)); fields.push_back(fc::raw::pack(c.error_id));
         fields.push_back(fc::raw::pack(c.error_code)); fields.push_back(fc::raw::pack(c.except?c.except->code():int64_t{0}));
      }
   }
   return fc::raw::pack(fields);
}
struct sc_live_corpus {
   genesis_state genesis=eosio::testing::base_tester::default_genesis();
   std::vector<isolated_sync_program> programs;
   flat_set<digest_type> reviewed;
   std::vector<std::vector<char>> blocks;
   explicit sc_live_corpus(std::vector<isolated_sync_program> supplied):programs(std::move(supplied)) {
      auto [key,pub,proof]=eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key=pub; genesis.initial_savanna->proof_of_possession=proof;
      for(const auto& p:programs) if(!p.program.code.empty())
         reviewed.insert(fc::sha256::hash(p.program.code.data(),p.program.code.size()));
      fc::temp_directory dir; sc_live_chain source(configuration(dir,0),genesis);
      source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      auto seal=[&]{blocks.push_back(fc::raw::pack(*source.produce_block()));};
      // Native Savanna's manifest deliberately does NOT activate SYNC_CALL.
      // Activate it using the original bios actions; never alter the manifest.
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call,builtin_protocol_feature_t::get_sender_fix});
      seal(); seal();
      BOOST_REQUIRE(source.control->is_builtin_activated(builtin_protocol_feature_t::sync_call));
      source.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n}); seal();
      for(name account:{"test"_n,"bob"_n}) {
         const auto seed=sc_program(sc_store(7,10,account)+sc_store(9,90,account),"(i64.const 0)",sc_db_imports);
         source.set_code(account,std::vector<uint8_t>(seed.begin(),seed.end()));
         auto trx=sc_live_trx(source,7,1,account,"init"_n);
         BOOST_REQUIRE(!source.push_transaction(trx)->except);
      }
      seal();
      for(const auto& p:programs) if(!p.program.code.empty())
         source.set_code(p.program.receiver,std::vector<uint8_t>(p.program.code.begin(),p.program.code.end()));
      seal();
   }
   controller::config configuration(const fc::temp_directory& dir,uint16_t workers,bool production=false) const {
      auto cfg=eosio::testing::base_tester::default_config(dir).first;
      cfg.wasm_runtime=wasm_interface::vm_type::eos_vm; cfg.contracts_console=true;
      cfg.parallel_validation_threads=workers; cfg.parallel_production_enabled=production && workers!=0;
      cfg.parallel_code_allowlist=reviewed; return cfg;
   }
   void prefix(eosio::testing::tester& chain) const {
      for(const auto& block:blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         chain.control->abort_block();
      }
   }
};
void sc_live_success(const std::vector<isolated_sync_program>& programs,const std::vector<uint64_t>& keys,
                     size_t expected_adopted,size_t calls,uint64_t returned,bool console=true) {
   sc_live_corpus corpus(programs);
   for(uint16_t workers:{0,1,2,4}) {
      BOOST_TEST_CONTEXT("sync adoption workers="<<workers) {
         fc::temp_directory pd,sd,vd;
         auto config=[&](const fc::temp_directory& dir,uint16_t count,bool production=false) {
            auto cfg=corpus.configuration(dir,count,production);cfg.contracts_console=console;return cfg;
         };
         sc_live_chain producer(config(pd,workers,true),corpus.genesis);
         eosio::testing::tester serial(config(sd,0),corpus.genesis),parallel(config(vd,workers),corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start();
         std::vector<transaction_metadata_ptr> batch; uint64_t nonce=100;
         for(auto key:keys) batch.push_back(sc_live_meta(producer,sc_live_trx(producer,key,++nonce)));
         std::vector<std::vector<char>> expected,serial_traces,parallel_traces;
         const auto collect=[&](auto& dest,const auto& event){
            const auto trace=std::get<0>(event);
            if(std::any_of(batch.begin(),batch.end(),[&](const auto& t){return t->id()==trace->id;})) dest.push_back(sc_live_trace(trace));
         };
         boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& e){collect(serial_traces,e);});
         boost::signals2::scoped_connection b=parallel.control->applied_transaction().connect([&](const auto& e){collect(parallel_traces,e);});
         const auto before=producer.control->get_parallel_production_stats();
         const auto vb=parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(parallel_test_measure("sync production",[&]{
            return producer.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),[&](const auto& trx){
               const auto trace=sc_live_push(producer,trx);
               BOOST_REQUIRE_MESSAGE(!trace->except,(trace->except?trace->except->to_detail_string():""));
               BOOST_REQUIRE(trace->receipt); BOOST_REQUIRE_EQUAL(trace->action_traces.size(),1u);
               const auto& root=trace->action_traces[0];
               BOOST_REQUIRE_EQUAL(root.call_traces.size(),calls); BOOST_CHECK_EQUAL(sc_number(root.return_value),returned);
               BOOST_CHECK_LE(root.elapsed.count(),trace->elapsed.count());
               for(const auto& child:root.call_traces) BOOST_CHECK_LE(child.elapsed.count(),root.elapsed.count());
               BOOST_CHECK_GE(trace->receipt->cpu_usage_us,producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
               expected.push_back(sc_live_trace(trace)); return true;
            });
         }),batch.size());
         const auto after=producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before,after)) {
            BOOST_CHECK_EQUAL(after.adopted-before.adopted,workers?expected_adopted:0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks-before.serial_fallbacks,workers?keys.size()-expected_adopted:0u);
         }
         const auto block=producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(expected==serial_traces); BOOST_CHECK(expected==parallel_traces);
         BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash()==parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted-vb.adopted,workers?expected_adopted:0u);
      }
   }
}
}

BOOST_AUTO_TEST_SUITE(parallel_sync_adoption_tests)
BOOST_AUTO_TEST_CASE(one_two_four_workers_adopt_distinct_child_rows_and_fall_back_on_the_same_row) {
   sc_live_success(sc_keyed_programs(),{7,9,7},2,1,22);
}
BOOST_AUTO_TEST_CASE(recursive_parent_visibility_negative_business_writes_and_full_call_traces_match_serial) {
   sc_live_success(sc_state_programs(0,true),{7,9},1,2,33);
   sc_live_success(sc_state_programs(-10000,true),{7,9},1,2,33);
}
BOOST_AUTO_TEST_CASE(nonexecuted_calls_before_a_real_child_keep_ordinals_returns_and_single_cpu_charge) {
   auto programs=sc_keyed_programs();
   programs[0].program.code=sc_program(sc_assert64(sc_call("absent"_n,~uint64_t{}),-1)+
      sc_assert64(sc_call("empty"_n),-1)+sc_assert64(sc_call("carol"_n),-2)+sc_root_body());
   programs.push_back({{"carol"_n,sc_program({},"",{}, {},false),{}},false});
   programs.push_back({{"empty"_n,{},{}},false});
   sc_live_success(programs,{7},1,4,22);
}
BOOST_AUTO_TEST_CASE(caller_without_sync_export_and_return_only_root_are_adopted_by_actual_behavior) {
   auto programs=sc_pair();
   programs[0].program.code=sc_program(sc_root_body(),"",{}, {},false);
   programs[0].sync_call_supported=false;
   sc_live_success(programs,{7,7},2,1,7);
   const auto body=sc_assert32("(call $ret (i32.const 65536)(i32.const 0))",0)+
      "(i64.store (i32.const 0)(i64.const 42))(call $out (i32.const 0)(i32.const 8))";
   const std::vector<isolated_sync_program> return_only{{{"test"_n,sc_program(body,"",{}, {},false),{}},false}};
   sc_live_success(return_only,{7,9},2,0,42);
}
BOOST_AUTO_TEST_CASE(same_code_recursion_and_preserved_parent_returns_match_original_contexts) {
   for(uint64_t depth:{0,1,4,7})
      sc_live_success(sc_recursive(),{depth},1,depth+1,depth+1);
   sc_live_success(sc_preserved_return(),{7},1,2,42);
   sc_live_success(sc_pair(sc_echo(),1),{7},1,1,7);
}
BOOST_AUTO_TEST_CASE(normalized_statuses_clear_returns_and_trace_growth_does_not_invalidate_parents) {
   const std::array<int64_t,7> statuses{0,-10000,-10001,-1,-9999,1,std::numeric_limits<int64_t>::min()};
   std::string root;
   for(int64_t status:statuses) {
      root+="(i64.store (i32.const 0)(i64.const "+std::to_string(status)+"))";
      root+=sc_assert64(sc_call("bob"_n),status==0?8:status<=-10000?status:-3);
      root+=sc_assert32("(call $ret (i32.const 64)(i32.const 1))",status==0?8:0);
      root+=sc_assert32("(call $ret (i32.const 65536)(i32.const 0))",status==0?8:0);
   }
   root+="(i64.store (i32.const 64)(i64.const 42))(call $out (i32.const 64)(i32.const 8))";
   auto programs=sc_pair("(drop (call $data (i32.const 0)(i32.const 8)))"
      "(call $set (i32.const 0)(i32.const 8))(i64.load (i32.const 0))");
   programs[0].program.code=sc_program(root);
   sc_live_success(programs,{7},1,statuses.size(),42);
}
BOOST_AUTO_TEST_CASE(disabled_console_keeps_original_empty_markers_without_disabling_sync_adoption) {
   sc_live_success(sc_state_programs(0,true),{7,9},1,2,33,false);
}
BOOST_AUTO_TEST_CASE(child_delete_recreate_uses_fresh_parent_queries_and_original_child_ram_semantics) {
   sc_live_success(sc_delete_programs(sc_check_row(sc_find("bob"_n),77)+
      "(call $out (i32.const 64)(i32.const 8))",true),{7,9},1,1,77);
}
BOOST_AUTO_TEST_CASE(original_ram_authorization_failure_after_child_writes_undoes_without_reexecuting_parent) {
   auto programs=sc_keyed_programs();
   programs[0].program.code=sc_program(sc_update(sc_find("test"_n),11)+sc_root_body()+
      sc_update(sc_find("test"_n),33,"carol"_n),"(i64.const 0)",sc_db_imports);
   sc_live_corpus corpus(programs);
   for(uint16_t workers:{1,2,4}) {
      fc::temp_directory pd,sd;
      sc_live_chain producer(corpus.configuration(pd,workers,true),corpus.genesis),serial(corpus.configuration(sd,0),corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); producer.start(); serial.start();
      const auto signed_trx=sc_live_trx(producer,7,300);
      const auto actual_meta=sc_live_meta(producer,signed_trx), reference_meta=sc_live_meta(serial,signed_trx);
      const auto before=producer.control->calculate_integrity_hash(), serial_before=serial.control->calculate_integrity_hash();
      const auto stats=producer.control->get_parallel_production_stats();
      transaction_trace_ptr actual;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({actual_meta},fc::time_point::maximum(),[&](const auto& trx){
         actual=sc_live_push(producer,trx); return true;
      }),1u);
      const auto reference=sc_live_push(serial,reference_meta);
      BOOST_REQUIRE(actual && actual->except); BOOST_REQUIRE(reference && reference->except);
      BOOST_REQUIRE_EQUAL(actual->action_traces.size(),1u);
      BOOST_CHECK_EQUAL(actual->action_traces[0].call_traces.size(),1u); // no retry over the canonical prefix
      BOOST_CHECK(sc_live_trace(actual)==sc_live_trace(reference));
      BOOST_CHECK(producer.control->calculate_integrity_hash()==before);
      BOOST_CHECK(serial.control->calculate_integrity_hash()==serial_before);
      const auto after=producer.control->get_parallel_production_stats();
      BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(stats,after)) {
         BOOST_CHECK_GT(after.succeeded-stats.succeeded,0u); // reaches the original host rejection after private success
         BOOST_CHECK_EQUAL(after.adopted-stats.adopted,0u);
      }
   }
}
BOOST_AUTO_TEST_CASE(child_failures_and_timeouts_undo_then_reuse_the_same_chain_and_workers) {
   for(unsigned failure=0;failure<4;++failure) {
      const std::array<std::string,4> faults{
         "(unreachable)", "(drop (call $read (i32.const 0)(i32.const 0)))", "",
         "(loop $forever (br $forever))"};
      auto programs=sc_pair();
      const auto child="(drop (call $data (i32.const 0)(i32.const 8)))"+sc_update(sc_find("bob"_n),22)+
         "(if (i64.eq (i64.load (i32.const 0))(i64.const 7))(then "+faults[failure]+"))"
         "(call $set (i32.const 64)(i32.const 8))(i64.const 0)";
      programs[1].program.code=sc_program({},child,sc_db_imports);
      if(failure==2) programs[0].program.code=sc_program(
         "(drop (call $read (i32.const 0)(i32.const 8)))"
         "(if (i64.eq (i64.load (i32.const 0))(i64.const 7))(then "+sc_root_body("bob"_n,1)+")"
         "(else "+sc_root_body()+"))");
      sc_live_corpus corpus(programs);
      for(uint16_t workers:{1,2,4}) {
         BOOST_TEST_CONTEXT("failure="<<failure<<" workers="<<workers) {
            fc::temp_directory pd,sd;
            sc_live_chain producer(corpus.configuration(pd,workers,true),corpus.genesis),serial(corpus.configuration(sd,0),corpus.genesis);
            corpus.prefix(producer); corpus.prefix(serial); producer.start(); serial.start();
            for(unsigned round=0;round<3;++round) {
               const auto snapshot=producer.control->calculate_integrity_hash(), original=serial.control->calculate_integrity_hash();
               const auto failed=sc_live_trx(producer,7,500+round*2);
               const auto stats=producer.control->get_parallel_production_stats();
               transaction_trace_ptr actual;
               BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({sc_live_meta(producer,failed)},
                  fc::time_point::maximum(),[&](const auto& trx){actual=sc_live_push(producer,trx);return true;}),1u);
               const auto reference=sc_live_push(serial,sc_live_meta(serial,failed));
               BOOST_REQUIRE(actual && actual->except); BOOST_REQUIRE(reference && reference->except);
               BOOST_CHECK(sc_live_trace(actual)==sc_live_trace(reference));
               BOOST_CHECK(producer.control->calculate_integrity_hash()==snapshot);
               BOOST_CHECK(serial.control->calculate_integrity_hash()==original);
               BOOST_CHECK(!producer.control->is_known_unexpired_transaction(actual->id));
               BOOST_CHECK_EQUAL(producer.control->get_parallel_production_stats().adopted-stats.adopted,0u);
               const auto fresh=sc_live_trx(producer,9,501+round*2);
               const auto before=producer.control->get_parallel_production_stats();
               BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({sc_live_meta(producer,fresh)},
                  fc::time_point::maximum(),[&](const auto& trx){actual=sc_live_push(producer,trx);return true;}),1u);
               BOOST_REQUIRE(actual && !actual->except && actual->receipt);
               BOOST_REQUIRE_EQUAL(actual->action_traces.size(),1u);
               BOOST_REQUIRE_EQUAL(actual->action_traces[0].call_traces.size(),1u);
               BOOST_CHECK_EQUAL(sc_number(actual->action_traces[0].return_value),22u);
               // Same accepted CPU bill, as in full validation. Independent
               // speculative clocks must not be mistaken for consensus data.
               const auto accepted=serial.control->push_transaction(sc_live_meta(serial,fresh),fc::time_point::maximum(),
                  fc::seconds(1),actual->receipt->cpu_usage_us,true,0);
               BOOST_REQUIRE(!accepted->except);
               BOOST_CHECK(sc_live_trace(actual)==sc_live_trace(accepted));
               BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
               const auto after=producer.control->get_parallel_production_stats();
               BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before,after)) {
                  BOOST_CHECK_EQUAL(after.adopted-before.adopted,1u);
                  BOOST_CHECK_EQUAL(after.serial_fallbacks-before.serial_fallbacks,0u);
               }
            }
         }
      }
   }
}
BOOST_AUTO_TEST_CASE(sync_queue_stop_exception_identity_abort_cancel_and_reentry_never_reuse_old_results) {
   sc_live_corpus corpus(sc_keyed_programs());
   for(unsigned mode=0;mode<7;++mode) {
      BOOST_TEST_CONTEXT("sync queue mode="<<mode) {
         fc::temp_directory dir; sc_live_chain chain(corpus.configuration(dir,2,true),corpus.genesis);
         corpus.prefix(chain); chain.start();
         const std::vector<transaction_metadata_ptr> batch{
            sc_live_meta(chain,sc_live_trx(chain,7,700)),sc_live_meta(chain,sc_live_trx(chain,9,701))};
         const auto snapshot=chain.control->calculate_integrity_hash();
         const auto before=chain.control->get_parallel_production_stats();
         size_t callbacks=0;
         const auto deadline=mode==3?fc::time_point::now()-fc::seconds(1):fc::time_point::maximum();
         auto run=[&]{return chain.control->for_each_parallel_production_transaction(batch,deadline,[&](const auto& trx){
            ++callbacks;
            if(mode==0) return false;
            if(mode==1) {chain.control->abort_block();chain.start();return true;}
            if(mode==2) throw std::runtime_error("sync queue callback failed");
            if(mode==4) {
               BOOST_CHECK_THROW(chain.control->for_each_parallel_production_transaction(batch,deadline,[](const auto&){return true;}),transaction_exception);
               return false;
            }
            if(mode==5) {
               const auto other=sc_live_meta(chain,trx->packed_trx()->get_signed_transaction());
               BOOST_REQUIRE(!sc_live_push(chain,other)->except);return true;
            }
            BOOST_REQUIRE(!sc_live_push(chain,trx)->except);
            chain.control->interrupt_transaction(controller::interrupt_t::speculative_block_trx);return true;
         });};
         if(mode==2) BOOST_CHECK_THROW(run(),std::runtime_error);
         else BOOST_CHECK_EQUAL(run(),mode==3?0u:mode==5?2u:1u);
         BOOST_CHECK_EQUAL(callbacks,mode==3?0u:mode==5?2u:1u);
         if(mode<=4 && mode!=1) BOOST_CHECK(chain.control->calculate_integrity_hash()==snapshot);
         if(mode==5) {
            const auto after=chain.control->get_parallel_production_stats();
            BOOST_CHECK_EQUAL(after.adopted-before.adopted,0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks-before.serial_fallbacks,2u);
         }
         chain.control->abort_block();chain.start();
         const auto fresh=sc_live_meta(chain,sc_live_trx(chain,9,710));
         const auto reset=chain.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({fresh},fc::time_point::maximum(),[&](const auto& trx){
            const auto trace=sc_live_push(chain,trx);
            BOOST_REQUIRE(!trace->except);BOOST_REQUIRE_EQUAL(trace->action_traces.size(),1u);
            BOOST_CHECK_EQUAL(trace->action_traces[0].call_traces.size(),1u);return true;
         }),1u);
         const auto after=chain.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(reset,after)) {BOOST_CHECK_EQUAL(after.adopted-reset.adopted,1u);}
      }
   }
}
BOOST_AUTO_TEST_CASE(unreviewed_child_code_and_multiple_sync_roots_keep_original_whole_transaction_execution) {
   sc_live_corpus corpus(sc_keyed_programs());
   const auto& child=corpus.programs[1].program.code;
   const auto child_hash=fc::sha256::hash(child.data(),child.size());
   for(unsigned mode=0;mode<2;++mode) for(uint16_t workers:{1,2,4}) {
      BOOST_TEST_CONTEXT("sync fallback mode="<<mode<<" workers="<<workers) {
         fc::temp_directory pd,sd,vd;
         auto pc=corpus.configuration(pd,workers,true),vc=corpus.configuration(vd,workers);
         if(mode==0) {pc.parallel_code_allowlist.erase(child_hash);vc.parallel_code_allowlist.erase(child_hash);}
         sc_live_chain producer(pc,corpus.genesis);
         eosio::testing::tester serial(corpus.configuration(sd,0),corpus.genesis),parallel(vc,corpus.genesis);
         corpus.prefix(producer);corpus.prefix(serial);corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name,1>{"eosio"_n});producer.start();
         auto trx=sc_live_trx(producer,7,800);
         if(mode==1) {
            auto second=trx.actions[0];const uint64_t key=9;
            std::memcpy(second.data.data(),&key,sizeof(key));trx.actions.push_back(std::move(second));
            trx.signatures.clear();trx.sign(producer.get_private_key("test"_n,"active"),producer.control->get_chain_id());
         }
         const auto metadata=sc_live_meta(producer,trx);
         transaction_trace_ptr actual;
         std::vector<std::vector<char>> originals,candidates;
         const auto collect=[&](auto& dest,const auto& event){
            const auto trace=std::get<0>(event);if(trace->id==metadata->id())dest.push_back(sc_live_trace(trace));
         };
         boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& event){collect(originals,event);});
         boost::signals2::scoped_connection b=parallel.control->applied_transaction().connect([&](const auto& event){collect(candidates,event);});
         const auto before=producer.control->get_parallel_production_stats(), vb=parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({metadata},fc::time_point::maximum(),[&](const auto& tx){
            actual=sc_live_push(producer,tx);return true;
         }),1u);
         BOOST_REQUIRE(actual && !actual->except && actual->receipt);
         BOOST_REQUIRE_EQUAL(actual->action_traces.size(),mode?2u:1u);
         for(const auto& root:actual->action_traces) {
            BOOST_REQUIRE_EQUAL(root.call_traces.size(),1u);BOOST_CHECK_EQUAL(sc_number(root.return_value),22u);
         }
         const auto after=producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before,after)) {
            BOOST_CHECK_GT(after.attempts-before.attempts,0u);
            BOOST_CHECK_EQUAL(after.adopted-before.adopted,0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks-before.serial_fallbacks,1u);
         }
         const auto expected=sc_live_trace(actual);const auto block=producer.finish_block();
         serial.push_block(block);serial.control->abort_block();parallel.push_block(block);parallel.control->abort_block();
         BOOST_REQUIRE_EQUAL(originals.size(),1u);BOOST_REQUIRE_EQUAL(candidates.size(),1u);
         BOOST_CHECK(originals[0]==expected);BOOST_CHECK(candidates[0]==expected);
         BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash()==parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted-vb.adopted,0u);
      }
   }
}
BOOST_AUTO_TEST_CASE(original_authorization_and_duplicate_checks_still_guard_a_successful_private_attempt) {
   sc_live_corpus corpus(sc_keyed_programs());
   for(uint16_t workers:{1,2,4}) {
      fc::temp_directory dir;sc_live_chain chain(corpus.configuration(dir,workers,true),corpus.genesis);
      corpus.prefix(chain);chain.start();
      const auto valid=sc_live_trx(chain,7,900);
      auto invalid=valid;invalid.signatures.clear();
      invalid.sign(chain.get_private_key("bob"_n,"active"),chain.control->get_chain_id());
      const auto bad=sc_live_meta(chain,invalid),good=sc_live_meta(chain,valid);
      BOOST_CHECK(bad->id()==good->id()); // ID binds unsigned bytes, not authorization success.
      const auto snapshot=chain.control->calculate_integrity_hash();
      const auto before=chain.control->get_parallel_production_stats();
      transaction_trace_ptr trace;
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({bad},fc::time_point::maximum(),[&](const auto& trx){
         trace=sc_live_push(chain,trx);return true;
      }),1u);
      BOOST_REQUIRE(trace && trace->except);BOOST_CHECK(!trace->receipt);
      BOOST_CHECK(trace->action_traces.empty());
      BOOST_CHECK(chain.control->calculate_integrity_hash()==snapshot);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(good->id()));
      const auto rejected=chain.control->get_parallel_production_stats();
      BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before,rejected)) {
         BOOST_CHECK_GT(rejected.succeeded-before.succeeded,0u);
         BOOST_CHECK_EQUAL(rejected.adopted-before.adopted,0u);
      }
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({good},fc::time_point::maximum(),[&](const auto& trx){
         trace=sc_live_push(chain,trx);return true;
      }),1u);
      BOOST_REQUIRE(trace && !trace->except && trace->receipt);
      BOOST_REQUIRE_EQUAL(trace->action_traces.size(),1u);BOOST_CHECK_EQUAL(trace->action_traces[0].call_traces.size(),1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted-rejected.adopted,1u);
      const auto accepted=chain.control->calculate_integrity_hash();
      const auto duplicate=sc_live_push(chain,good);
      BOOST_REQUIRE(duplicate && duplicate->except);
      BOOST_CHECK_EQUAL(duplicate->except->code(),tx_duplicate::code_value);
      BOOST_CHECK(chain.control->calculate_integrity_hash()==accepted);
   }
}
BOOST_AUTO_TEST_SUITE_END()
