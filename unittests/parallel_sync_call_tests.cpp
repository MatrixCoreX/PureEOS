#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/vm/exceptions.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <barrier>
#include <cstring>
#include <future>
#include <limits>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
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
