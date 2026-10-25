#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/vm/exceptions.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <barrier>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <future>
#include "parallel_test_diagnostics.hpp"
#include "parallel_test_runtime.hpp"

using namespace eosio::chain;
using namespace eosio::chain::experimental;

// Test-only capability installer. Never installs a replay guard, authorizes a
// transaction or exposes a production setter. Full controller tests below are
// separate from these deliberately narrow late-fallback boundary probes.
namespace eosio::testing {
struct mixed_adoption_test_access {
   static void install(transaction_context& tx, const isolated_mixed_tree_job& input,
                       const isolated_mixed_tree_result& result) {
      tx.parallel_mixed_input=&input; tx.parallel_mixed_result=&result;
      fc::microseconds total{};
      for(const auto& step:result.steps) {
         total+=fc::microseconds(step.effects.elapsed.count());
         for(const auto& call:step.calls) total+=fc::microseconds(call.exclusive_elapsed.count());
      }
      tx.parallel_transaction_elapsed=total;
   }
   static size_t consumed(const transaction_context& tx) { return tx.parallel_next_receiver; }
   static bool adopted(const transaction_context& tx) { return tx.parallel_transaction_adopted; }
   static bool cleared(const transaction_context& tx) {
      return !tx.parallel_mixed_input && !tx.parallel_mixed_result && !tx.parallel_mixed_replay_guard &&
             !tx.parallel_sync_replay_guard;
   }
};
}

namespace {
constexpr table_key mixed_rows{"bob"_n,"bob"_n,"rows"_n};
const std::array mixed_accounts{"test"_n,"bob"_n,"carol"_n,"empty"_n,"absent"_n};
std::string mc(uint64_t n) { return "(i64.const "+std::to_string(n)+")"; }
std::string mn(name n) { return mc(n.to_uint64_t()); }
std::string ma(const std::string& condition) { return "(call $assert "+condition+"(i64.const 7))"; }
std::string call_sync(name n,uint64_t flags=0,uint32_t bytes=0) {
   return "(call $call "+mn(n)+mc(flags)+"(i32.const 0)(i32.const "+std::to_string(bytes)+"))";
}
std::string copy_sync(name n,uint32_t bytes=0) {
   return ma("(i64.eq "+call_sync(n,0,bytes)+"(i64.const 16))")+
      ma("(i32.eq (call $ret (i32.const 0)(i32.const 16))(i32.const 16))");
}
std::string mf(uint64_t key=7) { return "(call $find "+mn(mixed_rows.code)+mn(mixed_rows.scope)+mn(mixed_rows.table)+mc(key)+")"; }
std::string when(name method,const std::string& body) {
   return "(if (i64.eq (get_local $action) "+mn(method)+")(then "+body+"))";
}
std::string escaped(std::span<const char> bytes) {
   constexpr char hex[]="0123456789abcdef";
   std::string out;
   for(unsigned char b:bytes) { out+='\\'; out+=hex[b>>4]; out+=hex[b&15]; }
   return out;
}
std::vector<char> child_bytes(name receiver,name method,bool auth=false) {
   action child; child.account=receiver; child.name=method;
   if(auth) child.authorization={{receiver,"active"_n}};
   return fc::raw::pack(child);
}
std::vector<char> mixed_program(const std::string& apply={},const std::string& sync="(i64.const 0)",
      const std::vector<std::pair<bool,std::vector<char>>>& sends={},const std::string& start={},bool with_console=false) {
   std::string data, functions;
   size_t offset=1024;
   for(size_t i=0;i<sends.size();++i) {
      const auto& [cf,bytes]=sends[i];
      data+="(data (i32.const "+std::to_string(offset)+") \""+escaped(bytes)+"\")";
      functions+="(func $send"+std::to_string(i)+" (call $"+(cf?"cfi":"inline")+
         " (i32.const "+std::to_string(offset)+")(i32.const "+std::to_string(bytes.size())+")))";
      offset+=bytes.size();
   }
   const auto wasm=wast_to_wasm(R"((module
      (import "env" "call" (func $call (param i64 i64 i32 i32)(result i64)))
      (import "env" "get_call_return_value" (func $ret (param i32 i32)(result i32)))
      (import "env" "get_call_data" (func $data (param i32 i32)(result i32)))
      (import "env" "set_call_return_value" (func $set (param i32 i32)))
      (import "env" "set_action_return_value" (func $out (param i32 i32)))
      (import "env" "eosio_assert_code" (func $assert (param i32 i64)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "send_inline" (func $inline (param i32 i32)))
      (import "env" "send_context_free_inline" (func $cfi (param i32 i32)))
      (import "env" "get_context_free_data" (func $cfdata (param i32 i32 i32)(result i32)))
      (import "env" "get_sender" (func $sender (result i64)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64)(result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32)(result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32)(result i32)))
      (import "env" "db_remove_i64" (func $remove (param i32))))"+
      (with_console?R"((import "env" "printi" (func $printi (param i64))))":std::string{})+R"(
      (memory 1 1) (global $guard (mut i32)(i32.const 0)))"+data+functions+
      (start.empty()?std::string{}:"(func $start "+start+")(start $start)")+R"(
      (func (export "apply") (param $receiver i64)(param $account i64)(param $action i64)
         (local $it i32)(local $n i32) )"+apply+R"()
      (func (export "sync_call") (param $sender i64)(param $receiver i64)(param $size i32)(result i64)
         (local $it i32)(local $n i32) )"+sync+"))");
   return {wasm.begin(),wasm.end()};
}
std::string increment() {
   return ma("(i32.eqz (get_global $guard))")+"(set_global $guard (i32.const 9))"+
      "(set_local $it "+mf()+")(drop (call $get (get_local $it)(i32.const 0)(i32.const 8)))"
      "(i64.store (i32.const 0)(i64.add (i64.load (i32.const 0))(i64.const 1)))"
      "(call $update (get_local $it)(i64.const 0)(i32.const 0)(i32.const 8))"
      "(i64.store (i32.const 8)(call $sender))(call $set (i32.const 0)(i32.const 16))(i64.const 0)";
}
std::vector<isolated_sync_program> graph(bool auth=false,bool cursor_checks=false) {
   const auto first=cursor_checks?ma("(i32.eq "+mf(9)+"(i32.const 0))"):"";
   const auto second=cursor_checks?ma("(i32.eq "+mf()+"(i32.const 1))"):"";
   const auto fresh=cursor_checks?ma("(i32.eq "+mf()+"(i32.const 0))"):"";
   return {
      {{"test"_n,mixed_program(first+copy_sync("bob"_n)+second+"(call $send0)(call $notify "+mn("carol"_n)+
         ")(call $send1)"+copy_sync("bob"_n)+"(call $out (i32.const 0)(i32.const 16))","(i64.const 0)",
         {{false,child_bytes("bob"_n,"normal"_n,auth)},{true,child_bytes("empty"_n,"pure"_n)}}),{}},true},
      {{"bob"_n,mixed_program(when("normal"_n,fresh+ma("(i32.eqz (get_global $guard))")+
         "(set_global $guard (i32.const 17))"+copy_sync("bob"_n)+
         ma("(i32.eq (get_global $guard)(i32.const 17))")+"(call $out (i32.const 0)(i32.const 16))"),increment()),{}},true},
      {{"carol"_n,mixed_program(first+second+copy_sync("bob"_n)+"(call $notify "+mn("test"_n)+
         ")(call $out (i32.const 0)(i32.const 16))"),{}},true},
      {{"empty"_n,mixed_program(ma("(i32.eq (call $cfdata (i32.const 0)(i32.const 0)(i32.const 8))(i32.const 8))")+
         ma("(i64.eq (i64.load (i32.const 0))(i64.const 91))")),{}},true}};
}
std::vector<isolated_sync_program> negative_graph() {
   auto p=graph();
   p[0].program.code=mixed_program(copy_sync("bob"_n)+
      ma("(i64.eq "+call_sync("bob"_n,0,1)+"(i64.const -10017))")+
      ma("(i32.eqz (call $ret (i32.const 0)(i32.const 16)))")+"(call $send0)"+
      ma("(i64.eq "+call_sync("absent"_n,99)+"(i64.const -1))")+
      ma("(i32.eqz (call $ret (i32.const 0)(i32.const 16)))"),"(i64.const 0)",
      {{false,child_bytes("empty"_n,"normal"_n)}});
   auto body=increment();
   body.resize(body.size()-std::string("(i64.const 0)").size());
   body+="(if (get_local $size)(then (return (i64.const -10017))))(i64.const 0)";
   p[1].program.code=mixed_program({},body);
   p[3].program.code=mixed_program();
   return p;
}
std::vector<isolated_sync_program> return_scope_graph(bool clear_in_notification,unsigned depth=0) {
   auto p=graph();
   const auto empty_return=ma("(i32.eqz (call $ret (i32.const 0)(i32.const 16)))");
   const auto inherited=ma("(i32.eq (call $ret (i32.const 0)(i32.const 16))(i32.const 16))");
   const auto notifications=clear_in_notification?
      "(call $notify "+mn("carol"_n)+")(call $notify "+mn("empty"_n)+")":
      "(call $notify "+mn("empty"_n)+")(call $notify "+mn("carol"_n)+")";
   p[0].program.code=mixed_program(empty_return+copy_sync("bob"_n,depth)+notifications+
      "(call $send0)(call $send1)(call $out (i32.const 0)(i32.const 16))","(i64.const 0)",
      {{false,child_bytes("bob"_n,"normal"_n)},{true,child_bytes("empty"_n,"pure"_n)}});
   const auto recurse=depth?"(if (get_local $size)(then (drop (call $call "+mn("bob"_n)+
      "(i64.const 0)(i32.const 0)(i32.sub (get_local $size)(i32.const 1))))))":std::string{};
   p[1].program.code=mixed_program(when("normal"_n,empty_return+copy_sync("bob"_n)+
      "(call $out (i32.const 0)(i32.const 16))"),empty_return+recurse+increment());
   p[2].program.code=mixed_program(inherited+"(call $out (i32.const 0)(i32.const 16))"+
      (clear_in_notification?ma("(i64.eq "+call_sync("absent"_n,99)+"(i64.const -1))")+empty_return:""));
   p[3].program.code=clear_in_notification?mixed_program(when("run"_n,empty_return)):std::vector<char>{};
   return p;
}
std::vector<isolated_sync_program> readonly_graph() {
   auto p=graph();
   const auto copy=[](name target,uint64_t flags) {
      return ma("(i64.eq "+call_sync(target,flags)+"(i64.const 8))")+
         ma("(i32.eq (call $ret (i32.const 0)(i32.const 8))(i32.const 8))");
   };
   const auto forward="(call $set (i32.const 0)(i32.const 8))(i64.const 0)";
   // Only the outer calls request read-only. Every deeper flags=0 frame must
   // inherit it, including the call back into the original root's account.
   p[0].program.code=mixed_program(copy("bob"_n,1)+"(call $notify "+mn("carol"_n)+")"
      "(call $out (i32.const 0)(i32.const 8))",
      "(drop (call $get "+mf()+"(i32.const 0)(i32.const 8)))"+forward);
   p[1].program.code=mixed_program({},copy("carol"_n,0)+forward);
   p[2].program.code=mixed_program(copy("bob"_n,1)+"(call $out (i32.const 0)(i32.const 8))",
      copy("test"_n,0)+forward);
   return p;
}
std::vector<isolated_sync_program> recreated_cursor_graph(bool failing=false) {
   auto p=graph();
   const auto fresh=[](unsigned handle) {
      return ma("(i32.eq "+mf()+"(i32.const "+std::to_string(handle)+"))")+
         ma("(i32.eq (call $get "+mf()+"(i32.const 0)(i32.const 8))(i32.const 8))")+
         ma("(i64.eq (i64.load (i32.const 0))(i64.const 88))");
   };
   p[0].program.code=mixed_program("(call $notify "+mn("carol"_n)+")"
      "(set_local $it "+mf()+")"+ma("(i32.eqz (get_local $it))")+
      "(drop "+call_sync("bob"_n)+")"+
      (failing?when("fail"_n,"(drop (call $get (get_local $it)(i32.const 0)(i32.const 8)))"):"")+
      fresh(1)+"(call $send0)(call $out (i32.const 0)(i32.const 8))","(i64.const 0)",
      {{false,child_bytes("bob"_n,"normal"_n)}});
   p[1].program.code=mixed_program(fresh(0)+"(call $out (i32.const 0)(i32.const 8))",
      "(call $remove "+mf()+")(i64.store (i32.const 0)(i64.const 88))"
      "(drop (call $store "+mn(mixed_rows.scope)+mn(mixed_rows.table)+mn("bob"_n)+mc(7)+
      "(i32.const 0)(i32.const 8)))(i64.const 0)");
   p[2].program.code=mixed_program(fresh(1)+"(drop "+call_sync("bob"_n)+")"+
      fresh(2)+"(call $out (i32.const 0)(i32.const 8))");
   return p;
}
isolated_action_input root() {
   isolated_action_input r; r.receiver=r.account="test"_n; r.action="run"_n;
   r.authorizations={{"test"_n,"active"_n}}; return r;
}
isolated_mixed_tree_context context() {
   isolated_mixed_tree_context c; c.sync.get_sender_fix=true;
   const uint64_t value=91;
   const std::vector<std::vector<char>> data{{reinterpret_cast<const char*>(&value),reinterpret_cast<const char*>(&value)+8}};
   c.tree.context_free_data=isolated_context_free_data::capture(data); return c;
}
uint64_t number(const std::vector<char>& bytes,size_t offset=0) {
   if(bytes.size()<offset+8) throw std::logic_error("short mixed return");
   uint64_t value; std::memcpy(&value,bytes.data()+offset,8); return value;
}
void seed(chainbase::database& db) {
   const auto& table=db.create<table_id_object>([](auto& t){
      t.code=mixed_rows.code; t.scope=mixed_rows.scope; t.table=mixed_rows.table;
      t.payer=mixed_rows.code; t.count=2;
   });
   for(uint64_t k:{7u,9u}) db.create<key_value_object>([&](auto& r){
      r.t_id=table.id; r.primary_key=k; r.payer=mixed_rows.code;
      const uint64_t n=k==7?10:90; r.value.assign(reinterpret_cast<const char*>(&n),8);
   });
}
std::shared_ptr<const frozen_primary_state> capture(const chainbase_transaction_state_view& view) {
   const std::array tables{mixed_rows};
   const std::array queries{primary_query{mixed_rows,primary_query_kind::find,7},
                           primary_query{mixed_rows,primary_query_kind::find,9}};
   return frozen_primary_state::capture_queries(view,tables,queries,{}, {},mixed_accounts);
}
struct mixed_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(),chainbase::database::read_write,8*1024*1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   mixed_fixture() {
      db.add_index<account_index>(); db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      for(auto n:mixed_accounts) if(n!="absent"_n) db.create<account_object>([&](auto& a){a.name=n;});
      seed(db); base=capture(view);
   }
   uint64_t canonical() {return number(view.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value);}
};

struct mixed_cache_metrics {
   const bool previous=performance::metrics().enabled.exchange(true,std::memory_order_relaxed);
   const uint64_t initial=performance::metrics().snapshot(performance::stage::parallel_compile).count;
   ~mixed_cache_metrics() {performance::metrics().enabled.store(previous,std::memory_order_relaxed);}
   uint64_t compilations() const {
      return performance::metrics().snapshot(performance::stage::parallel_compile).count-initial;
   }
};
std::vector<isolated_sync_program> mixed_cache_recursive() {
   const auto guard=ma("(i32.eqz (get_global $guard))")+
      ma("(i64.eqz (i64.load (i32.const 128)))")+
      "(set_global $guard (i32.const 9))(i64.store (i32.const 128)(i64.const 9))";
   const auto preserved=ma("(i32.eq (get_global $guard)(i32.const 9))")+
      ma("(i64.eq (i64.load (i32.const 128))(i64.const 9))");
   const auto child=guard+
      "(drop (call $data (i32.const 0)(i32.const 4)))(set_local $n (i32.load (i32.const 0)))"
      "(i32.store (i32.const 32)(i32.add (get_local $n)(i32.const 100)))"
      "(i64.store (i32.const 64)(i64.const 1))(if (get_local $n)(then "
      "(i32.store (i32.const 0)(i32.sub (get_local $n)(i32.const 1)))"+
      ma("(i64.eq "+call_sync("test"_n,0,4)+"(i64.const 16))")+
      "(drop (call $ret (i32.const 64)(i32.const 16)))"
      "(i64.store (i32.const 64)(i64.add (i64.load (i32.const 64))(i64.const 1)))))"+
      ma("(i32.eq (i32.load (i32.const 32))(i32.add (get_local $n)(i32.const 100)))")+
      preserved+"(call $set (i32.const 64)(i32.const 16))(i64.const 0)";
   const auto apply=guard+"(i32.store (i32.const 0)(i32.wrap/i64 (get_local $action)))"+
      ma("(i64.eq "+call_sync("test"_n,0,4)+"(i64.const 16))")+
      "(drop (call $ret (i32.const 64)(i32.const 16)))"+preserved+
      "(call $out (i32.const 64)(i32.const 16))";
   return {{{"test"_n,mixed_program(apply,child),{}},true}};
}
}

BOOST_AUTO_TEST_SUITE(parallel_mixed_tree_private_tests)

BOOST_FIXTURE_TEST_CASE(mixed_leases_reuse_idle_modules_between_depths_and_notification_groups,mixed_fixture) {
   mixed_cache_metrics metrics;
   isolated_mixed_tree_executor executor;
   const std::array roots{root(),root()}; const auto p=graph(false,true); const auto c=context();
   for(unsigned round=0;round<4;++round) {
      const auto result=executor.execute(base,roots,p,c);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
      BOOST_REQUIRE_EQUAL(result.steps.size(),8u); BOOST_REQUIRE_EQUAL(result.state.writes().size(),8u);
      BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),18u);
      // Four distinct programs; bob ordinary and bob sync must coexist once.
      BOOST_CHECK_EQUAL(metrics.compilations(),5u);
      BOOST_CHECK_EQUAL(canonical(),10u);
   }
}

BOOST_FIXTURE_TEST_CASE(mixed_idle_lru_has_nine_total_entries_and_keeps_recent_exact_code,mixed_fixture) {
   std::array<std::vector<char>,10> variants;
   for(unsigned i=0;i<variants.size();++i) variants[i]=mixed_program(
      ma("(i32.eqz (get_global $guard))")+"(set_global $guard (i32.const 9))"
      "(i64.store (i32.const 0)"+mc(i)+")(call $out (i32.const 0)(i32.const 8))");
   mixed_cache_metrics metrics; isolated_mixed_tree_executor executor; const std::array roots{root()};
   const auto run=[&](unsigned variant) {
      const std::array p{isolated_sync_program{{"test"_n,variants[variant],{}},true}};
      const auto result=executor.execute(base,roots,p,context());
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,context()));
      BOOST_CHECK_EQUAL(number(result.steps.front().effects.return_value),variant);
   };
   for(unsigned i=0;i<9;++i) {run(i); BOOST_CHECK_EQUAL(metrics.compilations(),i+1);}
   run(0); BOOST_CHECK_EQUAL(metrics.compilations(),9u);
   run(9); BOOST_CHECK_EQUAL(metrics.compilations(),10u); // evicts B, not recently touched A
   run(0); BOOST_CHECK_EQUAL(metrics.compilations(),10u);
   run(1); BOOST_CHECK_EQUAL(metrics.compilations(),11u);
   run(8); BOOST_CHECK_EQUAL(metrics.compilations(),11u);
   run(2); BOOST_CHECK_EQUAL(metrics.compilations(),12u);
}

BOOST_FIXTURE_TEST_CASE(mixed_leases_bind_every_limit_and_do_not_reuse_captured_authority,mixed_fixture) {
   const std::array roots{root()};
   auto original=graph();
   original[0].program.code=mixed_program(copy_sync("bob"_n)+"(call $out (i32.const 0)(i32.const 16))");
   original.resize(2);
   for(unsigned field=0;field<7;++field) {
      mixed_cache_metrics metrics; isolated_mixed_tree_executor executor; auto p=original;
      const auto run=[&] {
         const auto result=executor.execute(base,roots,p,context());
         BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,context()));
         BOOST_CHECK_EQUAL(number(result.steps.front().effects.return_value),11u);
      };
      run(); BOOST_CHECK_EQUAL(metrics.compilations(),2u);
      auto& limits=p[1].program.limits;
      switch(field) {
         case 0: limits.max_pages=8; break;
         case 1: limits.max_call_depth=32; break;
         case 2: limits.max_code_bytes=32*1024; break;
         case 3: limits.max_action_bytes=32*1024; break;
         case 4: limits.max_authorizations=32; break;
         case 5: limits.max_return_bytes=2048; break;
         case 6: limits.max_attempt_time=std::chrono::milliseconds(400); break;
      }
      run(); run(); BOOST_CHECK_EQUAL(metrics.compilations(),3u);
      limits={}; run(); BOOST_CHECK_EQUAL(metrics.compilations(),3u); // old exact key remains idle
      p[1].sync_call_supported=false;
      BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_unsupported);
      p[1].sync_call_supported=true; run();
      p[1].program.serial_only=true;
      BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_unsupported);
      p[1].program.serial_only=false; run();
      auto changed=context(); changed.sync.max_depth=0;
      BOOST_CHECK_THROW(executor.execute(base,roots,p,changed),primary_state_unsupported);
      BOOST_CHECK_EQUAL(metrics.compilations(),3u);
      BOOST_CHECK_EQUAL(canonical(),10u);
   }
}

BOOST_FIXTURE_TEST_CASE(mixed_recursive_leases_keep_nine_live_instances_and_release_on_depth_failure,mixed_fixture) {
   mixed_cache_metrics metrics; isolated_mixed_tree_executor executor;
   const auto p=mixed_cache_recursive(); auto c=context(); c.sync.max_depth=8;
   auto input=root(); input.action=name{7}; const std::array roots{input,input};
   const auto run=[&] {
      const auto result=executor.execute(base,roots,p,c);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
      BOOST_REQUIRE_EQUAL(result.steps.size(),2u);
      for(const auto& step:result.steps) {
         BOOST_REQUIRE_EQUAL(step.calls.size(),8u);
         BOOST_CHECK_EQUAL(number(step.effects.return_value),8u);
         for(size_t i=0;i<step.calls.size();++i) {
            BOOST_CHECK_EQUAL(number(step.calls[i].return_value),8-i);
            BOOST_CHECK_EQUAL(step.calls[i].call_ordinal,i+1);
            BOOST_CHECK_EQUAL(step.calls[i].sender_ordinal,i);
         }
      }
      BOOST_CHECK_EQUAL(metrics.compilations(),9u);
   };
   run(); run();
   input.action=name{8}; const std::array excessive{input};
   BOOST_CHECK_THROW(executor.execute(base,excessive,p,c),primary_state_unsupported);
   run();
}

BOOST_FIXTURE_TEST_CASE(mixed_failed_compilation_never_publishes_key_and_preserves_other_idle_modules,mixed_fixture) {
   mixed_cache_metrics metrics; isolated_mixed_tree_executor executor; const std::array roots{root()};
   std::array p{isolated_sync_program{{"test"_n,mixed_program(),{}},true}};
   for(unsigned i=0;i<9;++i) {
      p[0].program.code=mixed_program("(drop "+mc(i)+")");
      BOOST_CHECK_NO_THROW(executor.execute(base,roots,p,context()));
   }
   BOOST_CHECK_EQUAL(metrics.compilations(),9u);
   const auto hot=p;
   p[0].program.code={'x'};
   for(unsigned i=0;i<3;++i) {
      BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),eosio::vm::exception);
      BOOST_CHECK_EQUAL(metrics.compilations(),10u+i);
   }
   BOOST_CHECK_NO_THROW(executor.execute(base,roots,hot,context()));
   BOOST_CHECK_EQUAL(metrics.compilations(),12u); // failed LRU replacement did not evict this hot module
   p=hot; p[0].program.limits.max_pages=0;
   BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_budget_exceeded);
   BOOST_CHECK_NO_THROW(executor.execute(base,roots,hot,context()));
   BOOST_CHECK_EQUAL(metrics.compilations(),13u);
   p=hot; p[0].program.code=mixed_program("(drop (i64.const 0))");
   BOOST_CHECK_NO_THROW(executor.execute(base,roots,p,context())); // oldest A was really evicted
   BOOST_CHECK_EQUAL(metrics.compilations(),14u);
}

BOOST_FIXTURE_TEST_CASE(mixed_leases_are_worker_local_with_exact_cold_and_warm_compilation_counts,mixed_fixture) {
   const auto p=mixed_cache_recursive(); auto c=context(); c.sync.max_depth=8;
   auto input=root(); input.action=name{7}; const std::array roots{input,input};
   for(unsigned threads:{1u,2u,4u}) {
      mixed_cache_metrics metrics;
      std::barrier start(threads); std::vector<std::future<bool>> workers;
      for(unsigned i=0;i<threads;++i) workers.push_back(std::async(std::launch::async,[&] {
         isolated_mixed_tree_executor executor; start.arrive_and_wait();
         for(unsigned round=0;round<3;++round) {
            const auto result=executor.execute(base,roots,p,c);
            if(!validate_isolated_mixed_tree_journal(result,roots,p,c) || result.steps.size()!=2) return false;
            for(const auto& step:result.steps)
               if(step.calls.size()!=8 || number(step.effects.return_value)!=8) return false;
         }
         return true;
      }));
      for(auto& worker:workers) BOOST_CHECK(worker.get());
      BOOST_CHECK_EQUAL(metrics.compilations(),9u*threads);
   }
}

BOOST_FIXTURE_TEST_CASE(mixed_idle_modules_reset_start_memory_and_globals_after_failures_and_receiver_changes,mixed_fixture) {
   const auto start=ma("(i32.eqz (get_global $guard))")+ma("(i64.eqz (i64.load (i32.const 128)))")+
      "(set_global $guard (i32.const 1))(i64.store (i32.const 128)(i64.const 17))";
   const auto apply=ma("(i32.eq (get_global $guard)(i32.const 1))")+
      ma("(i64.eq (i64.load (i32.const 128))(i64.const 17))")+
      "(set_global $guard (i32.const 9))(i64.store (i32.const 128)(i64.const 99))"+
      when("trap"_n,"unreachable")+when("spin"_n,"(loop $busy (br $busy))")+
      "(i64.store (i32.const 0)(get_local $receiver))(call $out (i32.const 0)(i32.const 8))";
   const auto code=mixed_program(apply,"(i64.const 0)",{},start);
   const std::array p{isolated_sync_program{{"test"_n,code,{}},true},
                      isolated_sync_program{{"carol"_n,code,{}},true}};
   auto other=root(); other.receiver=other.account="carol"_n; other.authorizations.clear();
   const std::array roots{root(),other}; mixed_cache_metrics metrics; isolated_mixed_tree_executor executor;
   const auto healthy=[&] {
      const auto result=executor.execute(base,roots,p,context());
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,context()));
      BOOST_REQUIRE_EQUAL(result.steps.size(),2u);
      BOOST_CHECK_EQUAL(number(result.steps[0].effects.return_value),"test"_n.to_uint64_t());
      BOOST_CHECK_EQUAL(number(result.steps[1].effects.return_value),"carol"_n.to_uint64_t());
      BOOST_CHECK_EQUAL(metrics.compilations(),1u);
   };
   healthy();
   for(unsigned round=0;round<3;++round) {
      auto input=roots; input[1].action="trap"_n;
      BOOST_CHECK_THROW(executor.execute(base,input,p,context()),eosio::vm::exception); healthy();
      input[1].action="spin"_n;
      isolated_cancel deadline; deadline.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
      BOOST_CHECK_THROW(executor.execute(base,input,p,context(),{}, {},deadline),isolated_execution_interrupted); healthy();
      std::stop_source stopped; stopped.request_stop();
      BOOST_CHECK_THROW(executor.execute(base,roots,p,context(),stopped.get_token()),isolated_execution_interrupted); healthy();
   }
   auto wrong=std::async(std::launch::async,[&] {
      try {executor.execute(base,roots,p,context());} catch(const state_view_wrong_thread&) {return true;} return false;
   });
   BOOST_CHECK(wrong.get()); healthy();
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(shared_step_fingerprint_preserves_the_existing_encoding,mixed_fixture) {
   isolated_mixed_tree_executor executor;
   const std::array roots{root(),root()}; const auto p=graph(); const auto c=context();
   const auto result=executor.execute(base,roots,p,c);
   for(const auto& step:result.steps) {
      const auto& group=result.groups.at(step.group_index);
      const auto program=std::find_if(p.begin(),p.end(),[&](const auto& x){return x.program.receiver==step.receiver;});
      BOOST_REQUIRE(program!=p.end());
      auto input=group.input; input.receiver=step.receiver;
      const auto action=isolated_execution_fingerprint(step.code_hash,program->program.limits,input);
      isolated_action_tree_step identity;
      identity.group_index=step.group_index; identity.action_ordinal=step.action_ordinal;
      identity.creator_action_ordinal=step.creator_action_ordinal;
      identity.closest_unnotified_ancestor=step.closest_unnotified_ancestor; identity.sender=step.sender;
      fc::sha256::encoder original;
      fc::raw::pack(original,result.execution_fingerprint);
      fc::raw::pack(original,isolated_action_tree_step_fingerprint(action,c.tree,group,identity));
      const auto expected=original.result(); // encoder::result finalizes its state
      BOOST_CHECK(isolated_mixed_tree_step_fingerprint(action,result.execution_fingerprint,c,group,step)==expected);
      BOOST_CHECK(step.effects.execution_fingerprint==expected);
   }
}

BOOST_FIXTURE_TEST_CASE(queued_inline_immediate_sync_notifications_and_cf_share_one_transaction,mixed_fixture) {
   isolated_mixed_tree_executor executor;
   const std::array roots{root(),root()};
   const auto result=executor.execute(base,roots,graph(),context());
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,graph(),context()));
   BOOST_REQUIRE_EQUAL(result.steps.size(),8u); BOOST_REQUIRE_EQUAL(result.groups.size(),6u);
   const std::array ordinals{1u,4u,5u,3u,2u,7u,8u,6u};
   const std::array values{12u,13u,0u,14u,16u,17u,0u,18u};
   for(size_t i=0;i<result.steps.size();++i) {
      const auto& s=result.steps[i];
      BOOST_CHECK_EQUAL(s.action_ordinal,ordinals[i]);
      BOOST_CHECK_EQUAL(result.groups[s.group_index].context_free,i%4==2);
      if(i%4==2) {BOOST_CHECK(s.calls.empty()); BOOST_CHECK(s.effects.return_value.empty()); continue;}
      BOOST_CHECK_EQUAL(number(s.effects.return_value),values[i]);
      BOOST_REQUIRE_EQUAL(s.calls.size(),i%4==0?2u:1u);
      for(size_t j=0;j<s.calls.size();++j) {
         BOOST_CHECK_EQUAL(s.calls[j].call_ordinal,j+1); BOOST_CHECK_EQUAL(s.calls[j].sender_ordinal,0u);
         BOOST_CHECK(s.calls[j].sender==s.receiver); BOOST_CHECK_EQUAL(number(s.calls[j].return_value,8),s.receiver.to_uint64_t());
         BOOST_CHECK_GE(s.calls[j].exclusive_elapsed.count(),0); BOOST_CHECK(s.calls[j].executed);
      }
      BOOST_CHECK_GE(s.effects.elapsed.count(),0);
   }
   BOOST_REQUIRE_EQUAL(result.steps[0].events.size(),5u);
   BOOST_CHECK(std::holds_alternative<isolated_sync_call_event>(result.steps[0].events[0]));
   BOOST_CHECK(std::holds_alternative<isolated_tree_inline_event>(result.steps[0].events[1]));
   BOOST_CHECK(std::holds_alternative<isolated_cursor_event>(result.steps[0].events[2]));
   BOOST_CHECK(std::holds_alternative<isolated_tree_inline_event>(result.steps[0].events[3]));
   BOOST_CHECK(std::holds_alternative<isolated_sync_call_event>(result.steps[0].events[4]));
   BOOST_REQUIRE_EQUAL(result.state.writes().size(),8u);
   BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),18u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(legacy_entry_points_still_reject_mixed_capabilities,mixed_fixture) {
   const std::array roots{root()}; const auto programs=graph();
   std::vector<isolated_receiver_program> plain;
   for(const auto& p:programs) plain.push_back(p.program);
   isolated_action_tree_executor tree; isolated_sync_executor sync;
   BOOST_CHECK_THROW(tree.execute(base,roots,plain,context().tree),isolated_sync_mode_required);
   BOOST_CHECK_THROW(sync.execute(base,roots[0],programs,context().sync),primary_state_unsupported);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(notification_cursors_survive_sync_frames_but_inline_starts_fresh,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root(),root()};
   const auto result=executor.execute(base,roots,graph(false,true),context());
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,graph(false,true),context()));
   BOOST_REQUIRE_EQUAL(result.steps.size(),8u);
   const auto query=[&](size_t step,size_t event) -> const isolated_cursor_query& {
      return std::get<isolated_cursor_query>(std::get<isolated_cursor_event>(result.steps.at(step).events.at(event)));
   };
   for(size_t i:{0u,4u}) {
      BOOST_CHECK_EQUAL(query(i,0).result,0); BOOST_CHECK_EQUAL(query(i,2).result,1);
      BOOST_CHECK_EQUAL(query(i+1,0).result,0); BOOST_CHECK_EQUAL(query(i+1,1).result,1);
      BOOST_CHECK_EQUAL(query(i+3,0).result,0);
   }
   BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),18u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(business_error_keeps_writes_clears_return_and_missing_account_precedes_flags,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()};
   const auto result=executor.execute(base,roots,negative_graph(),context());
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,negative_graph(),context()));
   BOOST_REQUIRE_EQUAL(result.steps.size(),2u); BOOST_REQUIRE_EQUAL(result.steps[0].calls.size(),3u);
   const auto& calls=result.steps[0].calls;
   BOOST_CHECK_EQUAL(calls[0].status,0); BOOST_CHECK_EQUAL(calls[0].return_value.size(),16u);
   BOOST_CHECK_EQUAL(calls[1].status,-10017); BOOST_CHECK(calls[1].return_value.empty());
   BOOST_CHECK(calls[1].executed); BOOST_CHECK_EQUAL(calls[1].writes_end-calls[1].writes_begin,1u);
   BOOST_CHECK_EQUAL(calls[2].status,-1); BOOST_CHECK(!calls[2].executed);
   BOOST_CHECK(calls[2].return_value.empty()); BOOST_CHECK_EQUAL(calls[2].flags,99u);
   BOOST_CHECK_EQUAL(result.state.writes().size(),2u);
   BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),12u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(recent_sync_return_lives_with_notification_group_not_receiver_or_executor,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root(),root()};
   for(unsigned round=0;round<3;++round) for(bool clear:{false,true}) {
      const auto p=return_scope_graph(clear);
      const auto result=executor.execute(base,roots,p,context());
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,context()));
      BOOST_REQUIRE_EQUAL(result.steps.size(),10u); BOOST_REQUIRE_EQUAL(result.groups.size(),6u);
      size_t seen=0;
      for(const auto& step:result.steps) if(step.receiver=="carol"_n) {
         BOOST_CHECK_EQUAL(number(step.effects.return_value),11u+2*seen);
         BOOST_CHECK_EQUAL(number(step.effects.return_value,8),"test"_n.to_uint64_t());
         BOOST_REQUIRE_EQUAL(step.calls.size(),clear?1u:0u);
         if(clear) {BOOST_CHECK_EQUAL(step.calls[0].status,-1); BOOST_CHECK(step.calls[0].return_value.empty());}
         ++seen;
      }
      BOOST_CHECK_EQUAL(seen,2u); BOOST_REQUIRE_EQUAL(result.state.writes().size(),4u);
      BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),14u);
   }
   auto broken=return_scope_graph(false);
   broken[2].program.code=mixed_program("unreachable");
   BOOST_CHECK_THROW(executor.execute(base,roots,broken,context()),eosio::vm::exception);
   const auto healthy=return_scope_graph(false);
   const auto result=executor.execute(base,roots,healthy,context());
   BOOST_CHECK(validate_isolated_mixed_tree_journal(result,roots,healthy,context()));
   BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),14u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(recent_return_groups_are_private_across_one_two_four_threads,mixed_fixture) {
   const std::array programs{return_scope_graph(false),return_scope_graph(true)};
   const auto c=context(); const std::array roots{root(),root()};
   for(unsigned count:{1u,2u,4u}) {
      std::barrier start(count); std::vector<std::future<bool>> workers;
      for(unsigned i=0;i<count;++i) workers.push_back(std::async(std::launch::async,[&,i]{
         isolated_mixed_tree_executor executor; start.arrive_and_wait();
         for(unsigned round=0;round<3;++round) {
            const auto& p=programs[(i+round)%2];
            const auto result=executor.execute(base,roots,p,c);
            if(!validate_isolated_mixed_tree_journal(result,roots,p,c) || result.steps.size()!=10 ||
               result.state.writes().size()!=4 || number(require_primary_mutation(result.state.writes().back()).value)!=14) return false;
            size_t seen=0;
            for(const auto& step:result.steps) if(step.receiver=="carol"_n) {
               if(number(step.effects.return_value)!=11+2*seen ||
                  number(step.effects.return_value,8)!="test"_n.to_uint64_t()) return false;
               ++seen;
            }
            if(seen!=2) return false;
         }
         return true;
      }));
      for(auto& worker:workers) BOOST_CHECK(worker.get());
   }
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(same_code_recursive_sync_depth_is_independent_of_inline_depth,mixed_fixture) {
   auto p=graph();
   p[0].program.code=mixed_program("(call $send0)","(i64.const 0)",{{false,child_bytes("bob"_n,"normal"_n)}});
   const auto body=ma("(i32.eqz (get_global $guard))")+"(set_global $guard (i32.const 9))"
      "(drop (call $data (i32.const 0)(i32.const 4)))(set_local $n (i32.load (i32.const 0)))"
      "(i64.store (i32.const 64)(i64.const 1))(if (get_local $n)(then "
      "(i32.store (i32.const 0)(i32.sub (get_local $n)(i32.const 1)))"+
      ma("(i64.eq "+call_sync("bob"_n,0,4)+"(i64.const 16))")+
      "(drop (call $ret (i32.const 64)(i32.const 16)))"
      "(i64.store (i32.const 64)(i64.add (i64.load (i32.const 64))(i64.const 1)))))"+
      ma("(i32.eq (get_global $guard)(i32.const 9))")+"(call $set (i32.const 64)(i32.const 16))(i64.const 0)";
   p[1].program.code=mixed_program("(set_global $guard (i32.const 17))(i32.store (i32.const 0)(i32.const 2))"+
      ma("(i64.eq "+call_sync("bob"_n,0,4)+"(i64.const 16))")+
      "(drop (call $ret (i32.const 0)(i32.const 16)))"+ma("(i32.eq (get_global $guard)(i32.const 17))")+
      "(call $out (i32.const 0)(i32.const 16))",body);
   auto c=context(); c.tree.max_inline_action_depth=1; c.sync.max_depth=3;
   isolated_mixed_tree_executor executor; const std::array roots{root()};
   for(unsigned repeat=0;repeat<3;++repeat) {
      const auto result=executor.execute(base,roots,p,c);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
      BOOST_REQUIRE_EQUAL(result.steps.size(),2u); BOOST_REQUIRE_EQUAL(result.steps[1].calls.size(),3u);
      BOOST_CHECK_EQUAL(number(result.steps[1].effects.return_value),3u);
      for(size_t i=0;i<3;++i) {
         BOOST_CHECK_EQUAL(result.steps[1].calls[i].call_ordinal,i+1);
         BOOST_CHECK_EQUAL(result.steps[1].calls[i].sender_ordinal,i);
      }
   }
   c.sync.max_depth=2;
   BOOST_CHECK_THROW(executor.execute(base,roots,p,c),primary_state_unsupported);
   c.sync.max_depth=3; c.tree.max_inline_action_depth=0;
   BOOST_CHECK_THROW(executor.execute(base,roots,p,c),primary_state_unsupported);
}

BOOST_FIXTURE_TEST_CASE(sync_descendants_cannot_send_and_cf_cannot_call,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()};
   for(const std::string& body:std::array<std::string,3>{"(call $notify "+mn("test"_n)+")(i64.const 0)",
                             "(call $inline (i32.const 0)(i32.const 0))(i64.const 0)",
                             "(call $out (i32.const 0)(i32.const 0))(i64.const 0)"}) {
      auto p=graph(); p[1].program.code=mixed_program({},body);
      BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_unsupported);
   }
   auto p=graph(); p[3].program.code=mixed_program("(drop "+call_sync("bob"_n)+")");
   BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_unsupported);
   auto readonly=context(); readonly.sync.root_read_only=true;
   BOOST_CHECK_THROW(executor.execute(base,roots,graph(),readonly),primary_state_unsupported);
   BOOST_CHECK_EQUAL(number(require_primary_mutation(executor.execute(base,roots,graph(),context()).state.writes().back()).value),14u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(ancestor_cursor_is_invalid_after_child_remove_and_recreate,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()};
   auto p=graph();
   p[0].program.code=mixed_program("(set_local $it "+mf()+")(drop "+call_sync("bob"_n)+
      ")(drop (call $get (get_local $it)(i32.const 0)(i32.const 8)))");
   p[1].program.code=mixed_program({},"(call $remove "+mf()+")"
      "(i64.store (i32.const 0)(i64.const 88))(drop (call $store "+mn(mixed_rows.scope)+mn(mixed_rows.table)+
      mn("bob"_n)+mc(7)+"(i32.const 0)(i32.const 8)))(i64.const 0)");
   BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_unsupported);
   p[0].program.code=mixed_program("(drop "+call_sync("bob"_n)+")(drop (call $get "+mf()+
      "(i32.const 0)(i32.const 8)))(call $out (i32.const 0)(i32.const 8))");
   const auto result=executor.execute(base,roots,p,context());
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,context()));
   BOOST_CHECK_EQUAL(number(result.steps[0].effects.return_value),88u);
   BOOST_REQUIRE_EQUAL(result.state.writes().size(),2u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(total_sync_budget_is_not_reset_for_queued_actions_or_roots,mixed_fixture) {
   isolated_mixed_tree_executor executor; auto p=graph();
   const auto loop="(set_local $n (i32.const 33))(block $done (loop $again "
      "(br_if $done (i32.eqz (get_local $n)))(drop "+call_sync("absent"_n,99)+")"
      "(set_local $n (i32.sub (get_local $n)(i32.const 1)))(br $again)))";
   p[0].program.code=mixed_program(loop);
   const std::array roots{root(),root()};
   BOOST_CHECK_EQUAL(executor.execute(base,std::span(roots).first(1),p,context()).steps[0].calls.size(),33u);
   BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),primary_state_budget_exceeded);
   p[0].program.code=mixed_program(loop+"(call $send0)","(i64.const 0)",{{false,child_bytes("carol"_n,"run"_n)}});
   p[2].program.code=mixed_program(loop);
   BOOST_CHECK_THROW(executor.execute(base,std::span(roots).first(1),p,context()),primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(context_catalog_and_cf_payload_are_bound_to_the_new_fingerprint,mixed_fixture) {
   const std::array roots{root()}; const auto p=graph(); const auto c=context();
   const auto original=isolated_mixed_tree_fingerprint(roots,p,c);
   auto changed=c; changed.sync.get_sender_fix=false;
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,p,changed));
   changed=c; ++changed.tree.max_inline_action_depth;
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,p,changed));
   changed=c; changed.tree.context_free_data=isolated_context_free_data::capture(std::vector<std::vector<char>>{{'x'}});
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,p,changed));
   auto programs=p; programs.back().sync_call_supported=false;
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,programs,c));
   programs=p; programs.back().program.limits.max_pages=2;
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,programs,c));
   programs=p; programs.back().program.serial_only=true;
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,programs,c));
   programs=p; programs.back().program.code=mixed_program();
   BOOST_CHECK(original!=isolated_mixed_tree_fingerprint(roots,programs,c));
}

BOOST_FIXTURE_TEST_CASE(late_trap_timeout_cancel_and_wrong_thread_preserve_reusability,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()}; auto p=graph();
   p[1].program.code=mixed_program("unreachable",increment());
   BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),eosio::vm::exception);
   p[1].program.code=mixed_program("(loop $forever (br $forever))",increment());
   p[1].program.limits.max_attempt_time=std::chrono::milliseconds(20);
   BOOST_CHECK_THROW(executor.execute(base,roots,p,context()),eosio::vm::timeout_exception);
   std::stop_source stopped; stopped.request_stop();
   BOOST_CHECK_THROW(executor.execute(base,roots,graph(),context(),stopped.get_token()),isolated_execution_interrupted);
   isolated_cancel expired; expired.deadline=std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(executor.execute(base,roots,graph(),context(),{}, {},expired),isolated_execution_interrupted);
   auto wrong=std::async(std::launch::async,[&]{
      try {executor.execute(base,roots,graph(),context());} catch(const state_view_wrong_thread&) {return true;} return false;
   });
   BOOST_CHECK(wrong.get());
   for(unsigned i=0;i<3;++i) BOOST_CHECK_EQUAL(number(require_primary_mutation(executor.execute(base,roots,graph(),context()).state.writes().back()).value),14u);
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(one_two_four_workers_reuse_only_their_own_modules,mixed_fixture) {
   const auto p=graph(); const auto c=context(); const std::array roots{root(),root()};
   for(unsigned count:{1u,2u,4u}) {
      std::barrier start(count); std::vector<std::future<bool>> workers;
      for(unsigned i=0;i<count;++i) workers.push_back(std::async(std::launch::async,[&]{
         isolated_mixed_tree_executor executor; start.arrive_and_wait();
         for(unsigned round=0;round<3;++round) {
            const auto result=executor.execute(base,roots,p,c);
            if(!validate_isolated_mixed_tree_journal(result,roots,p,c)) return false;
            if(result.steps.size()!=8 || result.state.writes().size()!=8 || number(require_primary_mutation(result.state.writes().back()).value)!=18)
               return false;
         }
         return true;
      }));
      for(auto& worker:workers) BOOST_CHECK(worker.get());
   }
   BOOST_CHECK_EQUAL(canonical(),10u);
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
void mixed_no_result(const isolated_worker_outcome& out, isolated_worker_failure expected) {
   BOOST_CHECK_MESSAGE(out.failure == expected, isolated_worker_failure_name(out.failure));
   BOOST_CHECK(!out.result); BOOST_CHECK(!out.notification_result); BOOST_CHECK(!out.tree_result);
   BOOST_CHECK(!out.sync_result); BOOST_CHECK(!out.sync_transaction_result); BOOST_CHECK(!out.mixed_result);
   BOOST_CHECK_EQUAL(out.elapsed.count(), 0);
}
std::vector<isolated_receiver_program> ordinary_catalog(const std::vector<isolated_sync_program>& programs) {
   std::vector<isolated_receiver_program> out;
   for (const auto& p : programs) out.push_back(p.program);
   return out;
}
}

BOOST_AUTO_TEST_SUITE(parallel_mixed_tree_pool_tests)

BOOST_FIXTURE_TEST_CASE(six_owned_modes_switch_safely_with_exact_exclusive_cpu_on_one_two_four_workers,mixed_fixture) {
   const std::vector roots{root(), root()};
   const auto plain = mixed_program("(i64.store (i32.const 0)(i64.const 31))(call $out (i32.const 0)(i32.const 8))");
   const std::vector<isolated_action_invocation> actions{{plain, roots[0], {}}};
   const std::vector<isolated_receiver_program> programs{{"test"_n, plain, {}}};
   auto synchronous = graph();
   synchronous[0].program.code = mixed_program(copy_sync("bob"_n)+"(call $out (i32.const 0)(i32.const 16))");
   const auto mixed_programs = return_scope_graph(false);
   const auto c = context();
   isolated_transaction_executor legacy; isolated_notification_executor notification;
   isolated_action_tree_executor tree; isolated_sync_executor sync; isolated_mixed_tree_executor mixed;
   const std::array fingerprints{
      legacy.execute(base, actions).execution_fingerprint,
      notification.execute(base, roots, programs).root_fingerprint,
      tree.execute(base, roots, programs, c.tree).root_fingerprint,
      sync.execute(base, roots[0], synchronous, c.sync).execution_fingerprint,
      sync.execute_transaction(base, roots, synchronous, c.sync).execution_fingerprint,
      mixed.execute(base, roots, mixed_programs, c).execution_fingerprint};
   for (size_t threads : {1u, 2u, 4u}) {
      isolated_worker_pool pool(threads);
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<ordered_attempt_batch::request> requests;
         for (size_t i = 0; i < 12; ++i)
            requests.push_back({fc::sha256::hash(std::to_string(i)), fingerprints[i % 6]});
         ordered_attempt_batch batch(base, requests);
         std::vector<isolated_worker_job> jobs;
         for (size_t i = 0; i < requests.size(); ++i) switch (i % 6) {
            case 0: jobs.emplace_back(batch.get_ticket(i), actions); break;
            case 1: jobs.emplace_back(batch.get_ticket(i), roots, programs); break;
            case 2: jobs.emplace_back(batch.get_ticket(i), roots, programs, c.tree); break;
            case 3: jobs.emplace_back(batch.get_ticket(i), roots[0], synchronous, c.sync); break;
            case 4: jobs.emplace_back(batch.get_ticket(i), roots, synchronous, c.sync); break;
            case 5: {
               isolated_mixed_tree_job owned{roots, mixed_programs, c};
               jobs.emplace_back(batch.get_ticket(i), owned);
               owned.roots[0].data.assign(8, 0);
               owned.programs[0].program.code.clear();
               owned.context.tree.context_free_data.reset();
               break;
            }
         }
         const auto outputs = pool.run(std::move(jobs));
         BOOST_REQUIRE_EQUAL(outputs.size(), requests.size());
         for (size_t i = 0; i < outputs.size(); ++i) {
            const auto& out = outputs[i];
            BOOST_CHECK_EQUAL(bool(out.result), i % 6 == 0);
            BOOST_CHECK_EQUAL(bool(out.notification_result), i % 6 == 1);
            BOOST_CHECK_EQUAL(bool(out.tree_result), i % 6 == 2);
            BOOST_CHECK_EQUAL(bool(out.sync_result), i % 6 == 3);
            BOOST_CHECK_EQUAL(bool(out.sync_transaction_result), i % 6 == 4);
            BOOST_REQUIRE_MESSAGE(bool(out.mixed_result) == (i % 6 == 5),
               "threads=" << threads << " round=" << round << " job=" << i << " " << isolated_worker_failure_name(out.failure));
            BOOST_CHECK(out.failure == isolated_worker_failure::none);
            BOOST_CHECK(!out.request); BOOST_CHECK(!out.receiver_request);
            if (i % 6 != 5) continue;
            const auto& result = *out.mixed_result;
            BOOST_CHECK(validate_isolated_mixed_tree_journal(result, roots, mixed_programs, c));
            BOOST_CHECK(result.execution_fingerprint == fingerprints[5]);
            BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value), 14u);
            std::chrono::microseconds elapsed{};
            auto last = result.steps.front().effects.ended;
            for (const auto& step : result.steps) {
               elapsed += step.effects.elapsed;
               for (const auto& call : step.calls) elapsed += call.exclusive_elapsed;
               if (step.has_code) last = step.effects.ended;
            }
            BOOST_CHECK(out.elapsed == elapsed);
            BOOST_CHECK(out.began == result.steps.front().effects.began); BOOST_CHECK(out.ended == last);
            BOOST_CHECK(out.attempt_wall >= out.elapsed);
            BOOST_CHECK(out.elapsed <= out.ended - out.began);
         }
      }
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(all_fifteen_mode_pairs_and_mixed_input_bounds_fail_before_dispatch,mixed_fixture) {
   const isolated_mixed_tree_job input{{root()}, graph(), context()};
   const std::array requests{ordered_attempt_batch::request{fc::sha256::hash("bounds"),
      isolated_mixed_tree_fingerprint(input.roots, input.programs, input.context)}};
   ordered_attempt_batch batch(base, requests); isolated_worker_pool pool(1);
   const isolated_worker_job good{batch.get_ticket(0), input};
   const auto add_mode = [&](isolated_worker_job& job, unsigned mode) {
      switch (mode) {
         case 0: job.actions.push_back({input.programs[0].program.code, root(), {}}); break;
         case 1: job.notifications = isolated_notification_job{}; break;
         case 2: job.tree = isolated_action_tree_job{}; break;
         case 3: job.sync = isolated_sync_job{}; break;
         case 4: job.sync_transaction = isolated_sync_transaction_job{}; break;
         case 5: job.mixed = input; break;
      }
   };
   for (unsigned a = 0; a < 6; ++a) for (unsigned b = a + 1; b < 6; ++b) {
      isolated_worker_job bad{batch.get_ticket(0), std::vector<isolated_action_invocation>{}};
      add_mode(bad, a); add_mode(bad, b);
      BOOST_CHECK_THROW(pool.run({bad}), primary_state_budget_exceeded);
   }
   BOOST_CHECK_THROW(pool.run({isolated_worker_job{batch.get_ticket(0), std::vector<isolated_action_invocation>{}}}),
      primary_state_budget_exceeded);
   for (unsigned reason = 0; reason < 10; ++reason) {
      auto bad = good; auto in = *good.mixed;
      switch (reason) {
         case 0: in.roots.clear(); break;
         case 1: in.roots.resize(17, root()); break;
         case 2: in.programs.clear(); break;
         case 3: in.programs.resize(65, input.programs[0]); break;
         case 4: in.programs[0].program.code.resize(isolated_wasm_limits::max_module_code_bytes + 1); break;
         case 5: in.roots[0].data.resize(65537); break;
         case 6: in.roots[0].authorizations.resize(65); break;
         case 7: in.roots[0].max_return_bytes = 4097; break;
         case 8:
            in.roots.resize(5, root());
            for (auto& r : in.roots) r.data.resize(65536);
            in.roots.back().data.resize(1);
            break;
         case 9:
            in.programs.resize(5, input.programs[0]);
            for (auto& p : in.programs) p.program.code.resize(65536);
            break;
      }
      bad.mixed = std::move(in);
      BOOST_CHECK_THROW(pool.run({bad}), primary_state_budget_exceeded);
      BOOST_REQUIRE(pool.run({good})[0].mixed_result);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(discovery_late_traps_cancellation_and_empty_tails_never_publish_a_prefix,mixed_fixture) {
   isolated_worker_pool pool(2);
   const isolated_mixed_tree_job input{{root()}, graph(), context()};
   const std::array requests{ordered_attempt_batch::request{fc::sha256::hash("failure"),
      isolated_mixed_tree_fingerprint(input.roots, input.programs, input.context)}};
   ordered_attempt_batch batch(base, requests);
   const isolated_worker_job good{batch.get_ticket(0), input};
   const std::array reasons{isolated_worker_failure::missing_receiver, isolated_worker_failure::unsupported,
      isolated_worker_failure::vm_error, isolated_worker_failure::unsupported,
      isolated_worker_failure::deadline, isolated_worker_failure::cancelled};
   for (size_t reason = 0; reason < reasons.size(); ++reason) {
      auto bad = good;
      auto forged = *good.mixed;
      switch (reason) {
         case 0: forged.programs.erase(forged.programs.begin() + 2); break;
         case 1: forged.programs[2].program.serial_only = true; break;
         case 2: forged.programs[2].program.code = mixed_program("unreachable"); break;
         case 3: forged.context.tree.max_inline_action_depth = 0; break;
         case 4: bad.cancel.deadline = std::chrono::steady_clock::now(); break;
         case 5: bad.cancel.generation = std::make_shared<const std::atomic<uint64_t>>(1); break;
      }
      bad.mixed = std::move(forged);
      const auto out = pool.run({bad, good});
      mixed_no_result(out[0], reasons[reason]);
      BOOST_CHECK(!out[0].request); BOOST_CHECK_EQUAL(bool(out[0].receiver_request), reason == 0);
      if (reason == 0) BOOST_CHECK(*out[0].receiver_request == "carol"_n);
      BOOST_REQUIRE(out[1].mixed_result);
      BOOST_CHECK(validate_isolated_mixed_tree_journal(*out[1].mixed_result, input.roots, input.programs, input.context));
   }
   const auto sparse = frozen_primary_state::capture_queries(view, {}, {});
   ordered_attempt_batch missing(sparse, requests);
   const auto discovered = pool.run({isolated_worker_job{missing.get_ticket(0), input}});
   mixed_no_result(discovered[0], isolated_worker_failure::missing_state);
   BOOST_REQUIRE(discovered[0].request); BOOST_CHECK(!discovered[0].receiver_request);
   auto tail = input;
   tail.programs[0].program.code = mixed_program(copy_sync("bob"_n)+"(call $notify "+mn("empty"_n)+")");
   tail.programs[3].program.code.clear();
   const auto out = pool.run({isolated_worker_job{batch.get_ticket(0), tail}});
   BOOST_REQUIRE(out[0].mixed_result);
   const auto& result = *out[0].mixed_result;
   BOOST_REQUIRE_EQUAL(result.steps.size(), 2u);
   BOOST_CHECK(!result.steps.back().has_code);
   BOOST_CHECK(out[0].began == result.steps[0].effects.began);
   BOOST_CHECK(out[0].ended == result.steps[0].effects.ended);
   BOOST_CHECK(out[0].elapsed == result.steps[0].effects.elapsed + result.steps[0].calls[0].exclusive_elapsed);
   BOOST_CHECK(validate_isolated_mixed_tree_journal(result, tail.roots, tail.programs, tail.context));
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(mode_hints_are_not_authority_for_sync_children_or_context_free_actions,mixed_fixture) {
   isolated_worker_pool pool(1);
   const std::vector roots{root()}; const auto c = context();
   const std::array requests{ordered_attempt_batch::request{fc::sha256::hash("hint"), fc::sha256::hash("hint input")}};
   ordered_attempt_batch batch(base, requests);
   const auto check = [&](isolated_worker_job job, isolated_worker_failure reason) {
      const auto out = pool.run({std::move(job)});
      mixed_no_result(out[0], reason); BOOST_CHECK(!out[0].request); BOOST_CHECK(!out[0].receiver_request);
   };
   // First sync host in a notification, then in an inline group. The ordinary
   // root still uses the existing sync hint, never export-based inference.
   for (bool notification : {false, true}) {
      auto p = graph();
      p[0].program.code = notification ? mixed_program("(call $notify "+mn("carol"_n)+")") :
         mixed_program("(call $send0)", "(i64.const 0)", {{false, child_bytes("bob"_n, "normal"_n)}});
      check({batch.get_ticket(0), roots, ordinary_catalog(p), c.tree}, isolated_worker_failure::mixed_required);
   }
   for (unsigned send = 0; send < 3; ++send) {
      const auto body = send == 0 ? "(call $notify "+mn("carol"_n)+")" : "(call $send0)";
      auto p = graph();
      p[0].program.code = mixed_program(body, "(i64.const 0)", {{send == 2, child_bytes("empty"_n, "pure"_n)}});
      check({batch.get_ticket(0), roots, p, c.sync}, isolated_worker_failure::mixed_required);
      // Same import reached from a sync child is forbidden, not an upgrade.
      p[0].program.code = mixed_program("(drop "+call_sync("bob"_n)+")");
      p[1].program.code = mixed_program({}, body+"(i64.const 0)", {{send == 2, child_bytes("empty"_n, "pure"_n)}});
      check({batch.get_ticket(0), roots, p, c.sync}, isolated_worker_failure::unsupported);
      check({batch.get_ticket(0), isolated_mixed_tree_job{roots, p, c}}, isolated_worker_failure::unsupported);
   }
   for (const auto& body : std::array<std::string,4>{"(drop "+call_sync("bob"_n)+")",
         "(call $notify "+mn("carol"_n)+")", "(call $inline (i32.const 0)(i32.const 0))",
         "(call $cfi (i32.const 0)(i32.const 0))"}) {
      auto p = graph();
      p[0].program.code = mixed_program("(call $send0)", "(i64.const 0)", {{true, child_bytes("empty"_n, "pure"_n)}});
      p[3].program.code = mixed_program(body);
      check({batch.get_ticket(0), roots, ordinary_catalog(p), c.tree}, isolated_worker_failure::unsupported);
      check({batch.get_ticket(0), isolated_mixed_tree_job{roots, p, c}}, isolated_worker_failure::unsupported);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(whole_mixed_write_set_controls_ordering_and_rejects_foreign_capabilities,mixed_fixture) {
   const isolated_mixed_tree_job input{{root()}, graph(), context()};
   isolated_mixed_tree_executor executor;
   const std::optional result{executor.execute(base, input.roots, input.programs, input.context)};
   const std::array requests{
      ordered_attempt_batch::request{fc::sha256::hash("first"), result->execution_fingerprint},
      ordered_attempt_batch::request{fc::sha256::hash("second"), result->execution_fingerprint}};
   ordered_attempt_batch batch(base, requests), foreign(base, requests);
   unsigned adopted = 0, serial = 0;
   const auto adopt = [&](const isolated_mixed_tree_result&) { ++adopted; };
   const auto fallback = [&] { ++serial; };
   BOOST_CHECK_THROW(batch.run_next(foreign.get_ticket(0), requests[0].transaction_id, result, adopt, fallback), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(1), requests[1].transaction_id, result, adopt, fallback), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(0), requests[1].transaction_id, result, adopt, fallback), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(batch.next_index(), 0u);
   batch.run_next(batch.get_ticket(0), requests[0].transaction_id, result, [&](const isolated_mixed_tree_result&) {
      ++adopted;
      BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(0), requests[0].transaction_id, result, adopt, fallback), primary_state_invalid_attempt);
   }, fallback);
   batch.run_next(batch.get_ticket(1), requests[1].transaction_id, result, adopt, fallback);
   BOOST_CHECK_EQUAL(adopted, 1u); BOOST_CHECK_EQUAL(serial, 1u); BOOST_CHECK(batch.complete());
   BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(0), requests[0].transaction_id, result, adopt, fallback), primary_state_invalid_attempt);
   for (unsigned forged = 0; forged < 2; ++forged) {
      ordered_attempt_batch order(base, requests); const auto tag = order.get_ticket(0);
      auto bad = result;
      if (forged == 0) bad->execution_fingerprint = fc::sha256::hash("foreign input");
      else bad = executor.execute(capture(view), input.roots, input.programs, input.context);
      BOOST_CHECK_THROW(order.run_next(tag, requests[0].transaction_id, bad, adopt, fallback), primary_state_invalid_attempt);
      BOOST_CHECK(tag.stop_token().stop_requested());
   }
   ordered_attempt_batch throwing(base, requests); const auto tag = throwing.get_ticket(0);
   unsigned entered = 0, retried = 0;
   BOOST_CHECK_THROW(throwing.run_next(tag, requests[0].transaction_id, result,
      [&](const isolated_mixed_tree_result&) { ++entered; throw std::runtime_error("mixed callback failure"); },
      [&] { ++retried; }), std::runtime_error);
   BOOST_CHECK_EQUAL(entered, 1u); BOOST_CHECK_EQUAL(retried, 0u); BOOST_CHECK(tag.stop_token().stop_requested());
   BOOST_CHECK_EQUAL(canonical(), 10u);
}

BOOST_FIXTURE_TEST_CASE(mixed_pool_overlaps_real_sync_leaf_execution_without_sharing_state,mixed_fixture) {
   auto p = graph(); const auto c = context(); const std::vector roots{root()};
   p[0].program.code = mixed_program(copy_sync("bob"_n)+"(call $send0)", "(i64.const 0)",
      {{false, child_bytes("bob"_n, "normal"_n)}});
   const auto busy = R"((set_local $n (i32.const 5000))
      (block $done (loop $again (br_if $done (i32.eqz (get_local $n)))
         (set_local $n (i32.sub (get_local $n)(i32.const 1))) (br $again))))";
   p[1].program.code = mixed_program(copy_sync("bob"_n), std::string(busy)+increment());
   for (size_t threads : {1u, 2u, 4u}) {
      isolated_worker_pool pool(threads); std::vector<ordered_attempt_batch::request> requests;
      for (size_t i = 0; i < 4; ++i) requests.push_back({fc::sha256::hash(std::to_string(i)),
         isolated_mixed_tree_fingerprint(roots, p, c)});
      ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
      for (size_t i = 0; i < 4; ++i) jobs.emplace_back(batch.get_ticket(i), isolated_mixed_tree_job{roots, p, c});
      const auto outputs = pool.run(std::move(jobs)); bool overlap = false;
      for (size_t i = 0; i < outputs.size(); ++i) {
         BOOST_REQUIRE_MESSAGE(outputs[i].mixed_result, isolated_worker_failure_name(outputs[i].failure));
         const auto& result = *outputs[i].mixed_result;
         BOOST_CHECK(validate_isolated_mixed_tree_journal(result, roots, p, c));
         BOOST_REQUIRE_EQUAL(result.steps.size(), 2u);
         BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value), 12u);
         for (const auto& step : result.steps) {
            BOOST_REQUIRE_EQUAL(step.calls.size(), 1u); const auto& leaf = step.calls[0];
            BOOST_CHECK(leaf.executed); BOOST_CHECK_EQUAL(leaf.sender_ordinal, 0u);
            for (size_t j = 0; j < i; ++j) for (const auto& other : outputs[j].mixed_result->steps) {
               const auto& prior = other.calls[0];
               overlap |= std::max(leaf.began, prior.began) < std::min(leaf.ended, prior.ended);
            }
         }
      }
      BOOST_CHECK_EQUAL(overlap, threads > 1);
   }
   BOOST_CHECK_EQUAL(canonical(), 10u);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_mixed_tree_journal_tests)

BOOST_FIXTURE_TEST_CASE(one_overlay_proves_every_group_and_call_with_console_on_or_off,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root(),root()};
   for(bool console:{false,true}) for(unsigned mode:{0u,1u,2u,3u}) {
      auto p=mode==2?negative_graph():graph(false,mode==1);
      if(mode==3) p[1].program.code=mixed_program("(drop "+call_sync("bob"_n)+")",
         "(drop "+call_sync("carol"_n)+")"+increment());
      auto c=context(); c.tree.console_enabled=c.sync.console_enabled=console;
      const auto result=executor.execute(base,roots,p,c);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
      BOOST_CHECK(result.state.matches_base_reads(view,[]{}));
      BOOST_CHECK_EQUAL(canonical(),10u);
   }
}

BOOST_FIXTURE_TEST_CASE(forged_scheduling_sends_calls_cursors_and_metering_are_rejected,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root(),root()};
   const auto p=graph(); auto c=context(); c.tree.console_enabled=c.sync.console_enabled=true;
   const auto good=executor.execute(base,roots,p,c);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(good,roots,p,c));
   const std::vector<std::function<void(isolated_mixed_tree_result&)>> corruptions{
      [](auto& r){r.groups.pop_back();}, [](auto& r){r.steps.pop_back();},
      [](auto& r){r.groups.push_back(r.groups.back());}, [](auto& r){r.steps.push_back(r.steps.back());},
      [](auto& r){std::swap(r.steps[1],r.steps[2]);}, [](auto& r){++r.groups[0].root_index;},
      [](auto& r){++r.groups[0].depth;}, [](auto& r){r.groups[0].sender="bob"_n;},
      [](auto& r){r.groups[0].context_free=true;}, [](auto& r){++r.groups[2].creator_action_ordinal;},
      [](auto& r){++r.groups[2].closest_unnotified_ancestor;}, [](auto& r){++r.groups[2].action_ordinal;},
      [](auto& r){r.groups[2].sender="wrong"_n;}, [](auto& r){++r.groups[2].depth;},
      [](auto& r){r.groups[2].input.account="wrong"_n;}, [](auto& r){r.groups[2].input.receiver="wrong"_n;},
      [](auto& r){r.groups[2].input.action="wrong"_n;}, [](auto& r){r.groups[2].input.data.push_back('x');},
      [](auto& r){r.groups[2].input.authorizations={{"bob"_n,"active"_n}};},
      [](auto& r){++r.groups[2].input.max_return_bytes;}, [](auto& r){++r.steps[3].group_index;},
      [](auto& r){++r.steps[3].action_ordinal;}, [](auto& r){++r.steps[3].creator_action_ordinal;},
      [](auto& r){++r.steps[3].closest_unnotified_ancestor;}, [](auto& r){r.steps[3].sender="wrong"_n;},
      [](auto& r){r.steps[3].code_hash={};}, [](auto& r){r.steps[3].has_code=false;},
      [](auto& r){r.steps[3].effects.execution_fingerprint={};},
      [](auto& r){r.steps[0].effects.elapsed=std::chrono::microseconds(-1);},
      [](auto& r){r.steps[0].effects.elapsed=std::chrono::milliseconds(300);
                 r.steps[4].effects.elapsed=std::chrono::milliseconds(300);},
      [](auto& r){r.steps[0].calls[0].exclusive_elapsed=std::chrono::seconds(1);},
      [](auto& r){r.steps[0].calls[0].exclusive_elapsed=std::chrono::microseconds(-1);},
      [](auto& r){r.steps[0].effects.writes_begin=1;}, [](auto& r){r.steps[0].effects.writes_end=0;},
      [](auto& r){r.steps[4].effects.writes_begin=0;}, [](auto& r){r.execution_fingerprint={};},
      [](auto& r){r.context_free_data.reset();},
      [](auto& r){++std::get<isolated_tree_inline_event>(r.steps[0].events[1]).after_writes;},
      [](auto& r){++std::get<isolated_tree_inline_event>(r.steps[0].events[1]).action_ordinal;},
      [](auto& r){++std::get<isolated_tree_inline_event>(r.steps[0].events[1]).sent.decoded_bytes;},
      [](auto& r){std::get<isolated_tree_inline_event>(r.steps[0].events[1]).sent.decoded.data.push_back('x');},
      [](auto& r){std::get<isolated_tree_inline_event>(r.steps[0].events[1]).sent.packed[0]^=1;},
      [](auto& r){std::get<isolated_tree_inline_event>(r.steps[0].events[1]).sent.packed.resize(17);},
      [](auto& r){std::get<isolated_tree_inline_event>(r.steps[0].events[1]).context_free=true;},
      [](auto& r){std::swap(r.steps[0].events[0],r.steps[0].events[1]);},
      [](auto& r){std::swap(r.steps[0].events[1],r.steps[0].events[2]);},
      [](auto& r){r.steps[0].events.erase(r.steps[0].events.begin());},
      [](auto& r){r.steps[0].calls.pop_back();}, [](auto& r){r.steps[0].calls.push_back(r.steps[0].calls.back());},
      [](auto& r){std::swap(r.steps[0].calls[0],r.steps[0].calls[1]);},
      [](auto& r){++r.steps[4].calls[0].call_ordinal;}, [](auto& r){++r.steps[4].calls[0].sender_ordinal;},
      [](auto& r){r.steps[4].calls[0].sender="wrong"_n;}, [](auto& r){r.steps[4].calls[0].receiver="carol"_n;},
      [](auto& r){r.steps[4].calls[0].read_only=true;}, [](auto& r){r.steps[4].calls[0].flags=2;},
      [](auto& r){r.steps[4].calls[0].data.push_back('x');}, [](auto& r){r.steps[4].calls[0].status=-1;},
      [](auto& r){r.steps[4].calls[0].executed=false;}, [](auto& r){r.steps[4].calls[0].execution_fingerprint={};},
      [](auto& r){++r.steps[4].calls[0].writes_begin;}, [](auto& r){++r.steps[4].calls[0].writes_end;},
      [](auto& r){r.steps[4].calls[0].return_value.resize(4097);},
      [](auto& r){++std::get<isolated_sync_call_event>(r.steps[0].events[0]).after_writes;},
      [](auto& r){r.steps[0].console_markers[0]=1;}, [](auto& r){r.steps[0].console_markers.pop_back();},
      [](auto& r){r.steps[0].calls[0].console_markers.push_back(0);},
      [](auto& r){r.steps[0].effects.recipients[0].after_writes=0;},
      [](auto& r){r.steps[0].effects.recipients[0].recipient="test"_n;},
      [](auto& r){++std::get<isolated_cursor_notify>(std::get<isolated_cursor_event>(r.steps[0].events[2])).notification_index;},
      [](auto& r){r.steps[2].events.push_back(r.steps[0].events[0]);},
      [](auto& r){r.steps[2].effects.return_value={'x'};},
      [](auto& r){++std::get<isolated_cursor_query>(std::get<isolated_cursor_event>(r.steps[0].calls[0].events[0])).result;},
      [](auto& r){++std::get<isolated_cursor_write>(std::get<isolated_cursor_event>(r.steps[0].calls[0].events[1])).iterator;},
      [](auto& r){r.steps[0].calls[0].events.emplace_back(isolated_cursor_event{isolated_cursor_notify{0}});},
      [](auto& r){r.steps[0].events.resize(isolated_mixed_tree_executor::max_events+1);},
      [](auto& r){r.steps[0].calls.resize(isolated_mixed_tree_executor::max_calls+1);},
   };
   for(size_t i=0;i<corruptions.size();++i) BOOST_TEST_CONTEXT("mixed corruption "<<i) {
      auto bad=good; corruptions[i](bad);
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad,roots,p,c));
   }
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(missing_empty_unsupported_and_business_errors_have_distinct_proofs,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()}; auto p=negative_graph(); auto c=context();
   const auto good=executor.execute(base,roots,p,c);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(good,roots,p,c));
   for(unsigned mutation=0;mutation<11;++mutation) {
      auto bad=good; auto& missing=bad.steps[0].calls[2];
      switch(mutation) {
         case 0: missing.executed=true; break;
         case 1: missing.status=-2; break;
         case 2: missing.return_value={'x'}; break;
         case 3: ++missing.writes_end; break;
         case 4: missing.execution_fingerprint=good.execution_fingerprint; break;
         case 5: missing.exclusive_elapsed=std::chrono::microseconds(1); break;
         case 6: missing.events.push_back(isolated_cursor_event{isolated_cursor_notify{0}}); break;
         case 7: missing.began=std::chrono::steady_clock::now(); break;
         case 8: bad.steps[0].calls[1].return_value={'x'}; break;
         case 9: bad.steps[0].calls[1].status=-9999; break;
         case 10: bad.steps[0].calls[1].executed=false; break;
      }
      BOOST_TEST_CONTEXT("missing/negative corruption "<<mutation) {
         BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad,roots,p,c));
      }
   }
   p[0].program.code=mixed_program("(drop "+call_sync("bob"_n)+")(call $send0)","(i64.const 0)",
      {{false,child_bytes("empty"_n,"normal"_n)}});
   for(bool empty:{false,true}) {
      p[1].sync_call_supported=false;
      if(empty) p[1].program.code.clear();
      const auto result=executor.execute(base,roots,p,c);
      BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
      BOOST_CHECK_EQUAL(result.steps[0].calls[0].status,empty?-1:-2);
   }
}

BOOST_FIXTURE_TEST_CASE(all_owner_checkpoints_propagate_including_deep_nested_calls,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root(),root()}; auto p=graph(); auto c=context();
   p[1].program.code=mixed_program("(drop "+call_sync("bob"_n)+")","(drop "+call_sync("carol"_n)+")"+increment());
   c.tree.console_enabled=c.sync.console_enabled=true;
   const auto result=executor.execute(base,roots,p,c);
   size_t count=0;
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c,[&]{++count;}));
   size_t minimum_checks=result.steps.size();
   for(const auto& step:result.steps) {
      minimum_checks+=step.events.size()+step.calls.size();
      for(const auto& call:step.calls) minimum_checks+=call.events.size();
   }
   BOOST_REQUIRE_GE(count,minimum_checks); // at least each step, event and nested frame, not an arbitrary fixture constant
   for(size_t stop_at=1;stop_at<=count;++stop_at) {
      size_t current=0;
      BOOST_CHECK_THROW(validate_isolated_mixed_tree_journal(result,roots,p,c,[&]{
         if(++current==stop_at) throw primary_state_invalid_attempt();
      }),primary_state_invalid_attempt);
      BOOST_CHECK_EQUAL(current,stop_at);
   }
   BOOST_CHECK_EQUAL(canonical(),10u);
}

BOOST_FIXTURE_TEST_CASE(dependencies_cover_replayed_reads_and_accounts_but_proof_is_not_live_authority,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root(),root()}; const auto p=graph(); const auto c=context();
   const auto good=executor.execute(base,roots,p,c);
   auto bad=good;
   bad.steps[0].events.emplace_back(isolated_cursor_event{
      isolated_cursor_query{isolated_cursor_query_kind::find,mixed_rows,9,-1,0}});
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad,roots,p,c)); // captured, but not proved read
   bad=good;
   bad.steps[0].events.emplace_back(isolated_cursor_event{
      isolated_cursor_query{isolated_cursor_query_kind::find,mixed_rows,7,-1,0}});
   BOOST_CHECK(validate_isolated_mixed_tree_journal(bad,roots,p,c)); // structural, NOT a second WASM execution
   for(unsigned mode:{0u,1u}) {
      primary_state_overlay other(base);
      for(auto account:good.state.read_accounts()) if(mode!=0 || account!="carol"_n) (void)other.read_account(account);
      for(const auto& q:good.state.read_queries()) (void)other.locate_primary(q);
      for(const auto& mutation:good.state.writes()) {
         const auto& write=require_primary_mutation(mutation);
         other.update_existing(write.table,mode==1?9:write.primary_key,write.payer,write.value);
      }
      bad=good; bad.state=other.seal();
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad,roots,p,c));
   }
   const auto& table=db.get<table_id_object,by_code_scope_table>(boost::make_tuple(mixed_rows.code,mixed_rows.scope,mixed_rows.table));
   const auto& row=db.get<key_value_object,by_scope_primary>(boost::make_tuple(table.id,uint64_t(7)));
   db.modify(row,[](auto& r){const uint64_t value=99; r.value.assign(reinterpret_cast<const char*>(&value),8);});
   BOOST_CHECK(validate_isolated_mixed_tree_journal(good,roots,p,c));
   BOOST_CHECK(!good.state.matches_base_reads(view,[]{}));
   BOOST_CHECK_EQUAL(canonical(),99u);
}

BOOST_FIXTURE_TEST_CASE(catalog_context_and_empty_receivers_are_bound_before_replay,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()}; auto p=graph(); const auto c=context();
   p[2].program.code.clear();
   const auto good=executor.execute(base,roots,p,c);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(good,roots,p,c));
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,{},p,c));
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,{},c));
   for(size_t i=0;i<p.size();++i) {
      auto changed=p; changed[i].sync_call_supported=!changed[i].sync_call_supported;
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,changed,c));
      changed=p; changed[i].program.limits.max_pages=0;
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,changed,c));
      changed=p; changed[i].program.serial_only=true;
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,changed,c));
   }
   auto changed=c; changed.sync.get_sender_fix=false;
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,p,changed));
   changed=c; --changed.tree.max_inline_action_depth;
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,p,changed));
   changed=c; changed.tree.context_free_data.reset();
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(good,roots,p,changed));
   for(unsigned mutation=0;mutation<4;++mutation) {
      auto bad=good; auto& empty=bad.steps[1];
      switch(mutation) {
         case 0: empty.events.push_back(good.steps[0].events[0]); break;
         case 1: empty.effects.elapsed=std::chrono::microseconds(1); break;
         case 2: empty.effects.return_value={'x'}; break;
         case 3: empty.calls.push_back(good.steps[0].calls[0]); break;
      }
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad,roots,p,c));
   }
}

BOOST_FIXTURE_TEST_CASE(recreated_rows_cannot_revive_an_ancestor_handle_in_frozen_replay,mixed_fixture) {
   isolated_mixed_tree_executor executor; const std::array roots{root()}; auto p=graph(); const auto c=context();
   p[0].program.code=mixed_program("(drop "+mf()+")(drop "+call_sync("bob"_n)+")"
      "(drop (call $get "+mf()+"(i32.const 0)(i32.const 8)))(call $out (i32.const 0)(i32.const 8))");
   p[1].program.code=mixed_program({},"(call $remove "+mf()+")"
      "(i64.store (i32.const 0)(i64.const 88))(drop (call $store "+mn(mixed_rows.scope)+mn(mixed_rows.table)+
      mn("bob"_n)+mc(7)+"(i32.const 0)(i32.const 8)))(i64.const 0)");
   const auto good=executor.execute(base,roots,p,c);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(good,roots,p,c));
   BOOST_CHECK_EQUAL(number(good.steps[0].effects.return_value),88u);
   BOOST_REQUIRE_EQUAL(good.steps[0].events.size(),3u);
   auto bad=good;
   auto& query=std::get<isolated_cursor_query>(std::get<isolated_cursor_event>(bad.steps[0].events[2]));
   BOOST_REQUIRE_EQUAL(query.result,1); query.result=0;
   BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad,roots,p,c));
   BOOST_CHECK_EQUAL(canonical(),10u);
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
// Owner-only boundary fixture, NOT a production/validation corpus. Direct
// seeded rows are used solely to test read-only proof against real metadata.
// The later live corpus must seed via original signed contract actions.
struct mixed_owner_fixture {
   eosio::testing::tester chain;
   std::vector<isolated_action_input> roots;
   std::vector<isolated_sync_program> programs;
   isolated_mixed_tree_context protocol;
   std::optional<packed_transaction> packed;
   std::optional<isolated_mixed_tree_result> result;
   mixed_owner_fixture() {
      chain.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n,"unused"_n});
      auto code=graph(); code.push_back({{"unused"_n,mixed_program(),{}},true});
      for(const auto& p:code)
         chain.set_code(p.program.receiver,std::vector<uint8_t>(p.program.code.begin(),p.program.code.end()));
      chain.produce_block(); seed(db());
      for(const auto& p:code) {
         auto captured=prepare_isolated_sync_program(*chain.control,p.program.receiver);
         BOOST_REQUIRE(captured); programs.push_back(std::move(*captured));
      }
      roots={root(),root()}; roots.back().data={'b'};
      signed_transaction trx;
      for(const auto& r:roots) {
         action a; a.account=r.account; a.name=r.action; a.data=r.data;
         for(const auto& [actor,permission]:r.authorizations) a.authorization.push_back({actor,permission});
         trx.actions.push_back(std::move(a));
      }
      trx.context_free_data={fc::raw::pack(uint64_t{91})};
      chain.set_transaction_headers(trx);
      packed.emplace(trx,packed_transaction::compression_type::none);
      const auto prepared=prepare_isolated_transaction(*chain.control,*packed); BOOST_REQUIRE(prepared);
      roots.clear(); for(const auto& a:*prepared) roots.push_back(a.input);
      protocol={current_action_tree_context(*chain.control),current_sync_context(*chain.control)};
      protocol.tree.context_free_data=isolated_context_free_data::capture(packed->get_context_free_data());
      chainbase_transaction_state_view live(chain.control->db());
      isolated_mixed_tree_executor executor; result=executor.execute(capture(live),roots,programs,protocol);
      BOOST_REQUIRE(valid());
   }
   chainbase::database& db() {return const_cast<chainbase::database&>(chain.control->db());}
   bool valid() const {return validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,*result);}
   template<typename F> void changed(F&& change) {
      auto undo=db().start_undo_session(true); change();
      BOOST_CHECK(!valid()); undo.undo(); BOOST_CHECK(valid());
   }
};
}

BOOST_AUTO_TEST_SUITE(parallel_mixed_tree_owner_tests)

BOOST_FIXTURE_TEST_CASE(full_catalog_including_unused_entries_and_every_vm_limit_is_bound,mixed_owner_fixture) {
   const auto original=programs;
   for(unsigned variant=0;variant<15;++variant) {
      programs=original;
      switch(variant) {
         case 0: programs.clear(); break;
         case 1: programs.push_back(programs.front()); break;
         case 2: programs.resize(65,programs.front()); break;
         case 3: std::swap(programs[1],programs.back()); break;
         case 4: programs.back().program.code[0]^=1; break;
         case 5: programs.back().sync_call_supported=!programs.back().sync_call_supported; break;
         case 6: programs.back().program.serial_only=true; break;
         case 7: --programs.back().program.limits.max_pages; break;
         case 8: --programs.back().program.limits.max_call_depth; break;
         case 9: --programs.back().program.limits.max_code_bytes; break;
         case 10: --programs.back().program.limits.max_action_bytes; break;
         case 11: --programs.back().program.limits.max_authorizations; break;
         case 12: --programs.back().program.limits.max_return_bytes; break;
         case 13: programs.back().program.limits.max_attempt_time+=std::chrono::milliseconds(1); break;
         case 14: programs.pop_back(); break;
      }
      BOOST_TEST_CONTEXT("mixed catalog variant="<<variant) {BOOST_CHECK(!valid());}
   }
   programs=original; BOOST_CHECK(valid());
   for(name receiver:{"bob"_n,"unused"_n}) changed([&]{
      const auto& a=db().get<account_metadata_object,by_name>(receiver);
      db().modify(db().get<code_object,by_code_hash>(boost::make_tuple(a.code_hash,a.vm_type,a.vm_version)),
         [](auto& code){code.sync_call_supported=false;});
   });
}

BOOST_FIXTURE_TEST_CASE(current_ordinary_sync_unused_metadata_and_protocol_cannot_reuse_old_proof,mixed_owner_fixture) {
   for(name receiver:{"test"_n,"bob"_n,"carol"_n,"empty"_n,"unused"_n}) for(unsigned variant=0;variant<4;++variant)
      BOOST_TEST_CONTEXT("mixed current receiver="<<receiver<<" variant="<<variant) {changed([&]{
         db().modify(db().get<account_metadata_object,by_name>(receiver),[&](auto& a){
            if(variant==0) a.code_hash=fc::sha256::hash("changed mixed current code");
            if(variant==1) a.vm_type=1;
            if(variant==2) a.vm_version=1;
            if(variant==3) a.set_privileged(true);
         });
      });}
   changed([&]{db().remove(db().get<account_object,by_name>("unused"_n));});
   for(unsigned variant=0;variant<7;++variant) changed([&]{
      db().modify(chain.control->get_global_properties(),[&](auto& g){
         if(variant==0) --g.configuration.max_inline_action_size;
         if(variant==1) --g.configuration.max_inline_action_depth;
         if(variant==2) --g.configuration.max_sync_call_depth;
         if(variant==3) --g.configuration.max_sync_call_data_size;
         if(variant==4) --g.configuration.max_action_return_value_size;
         if(variant==5) g.wasm_configuration.max_pages=8;
         if(variant==6) g.wasm_configuration.max_call_depth=32;
      });
   });
   for(unsigned variant=0;variant<3;++variant) {
      auto p=protocol;
      if(variant==0) p.sync.get_sender_fix=!p.sync.get_sender_fix;
      if(variant==1) p.sync.console_enabled=!p.sync.console_enabled;
      if(variant==2) p.sync.root_read_only=true;
      auto bad=*result; bad.execution_fingerprint=isolated_mixed_tree_fingerprint(roots,programs,p);
      BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,bad));
   }
}

BOOST_FIXTURE_TEST_CASE(actual_packed_inputs_and_cf_data_are_bound_but_proof_is_not_authorization,mixed_owner_fixture) {
   BOOST_CHECK(packed->get_signed_transaction().signatures.empty()); BOOST_CHECK(valid());
   const auto before=chain.control->calculate_integrity_hash();
   auto unsigned_transaction=packed->get_signed_transaction();
   BOOST_CHECK_THROW(chain.push_transaction(unsigned_transaction,fc::time_point::maximum(),
      eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US),fc::exception);
   BOOST_CHECK(chain.control->calculate_integrity_hash()==before);
   for(unsigned variant=0;variant<12;++variant) {
      auto trx=packed->get_signed_transaction();
      switch(variant) {
         case 0: trx.actions.clear(); break;
         case 1: trx.actions.pop_back(); break;
         case 2: std::swap(trx.actions[0],trx.actions[1]); break;
         case 3: trx.actions.back().data.push_back('x'); break;
         case 4: trx.actions.back().authorization[0].permission="owner"_n; break;
         case 5: trx.actions.back().name="other"_n; break;
         case 6: trx.context_free_actions.push_back(trx.actions.front()); break;
         case 7: trx.delay_sec=1; break;
         case 8: trx.context_free_data[0][0]^=1; break;
         case 9: trx.context_free_data.clear(); break;
         case 10: trx.context_free_data.push_back({'x'}); break;
         case 11: trx.context_free_data[0].resize(isolated_context_free_data::max_bytes+1); break;
      }
      const packed_transaction altered(trx,packed_transaction::compression_type::none);
      if(variant>=8) BOOST_CHECK(altered.id()==packed->id());
      BOOST_TEST_CONTEXT("mixed packed variant="<<variant) {
         BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,altered,programs,*result));
      }
   }
   auto bad=*result; bad.context_free_data.reset();
   BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,bad));
   BOOST_CHECK(valid()); BOOST_CHECK(chain.control->calculate_integrity_hash()==before);
}

BOOST_FIXTURE_TEST_CASE(legacy_missing_cf_capability_is_not_evidence_even_without_a_cf_child,mixed_owner_fixture) {
   const auto ordinary=mixed_program(copy_sync("bob"_n)+"(call $notify "+mn("carol"_n)+")");
   const auto empty=mixed_program();
   chain.set_code("test"_n,std::vector<uint8_t>(ordinary.begin(),ordinary.end()));
   chain.set_code("carol"_n,std::vector<uint8_t>(empty.begin(),empty.end()));
   chain.produce_block();
   for(auto& p:programs) {
      auto current=prepare_isolated_sync_program(*chain.control,p.program.receiver);
      BOOST_REQUIRE(current); p=std::move(*current);
   }
   chainbase_transaction_state_view live(chain.control->db());
   const auto base=capture(live); const auto before=chain.control->calculate_integrity_hash();
   isolated_mixed_tree_executor executor;
   auto legacy=protocol; legacy.tree.context_free_data.reset();
   const auto unbound=executor.execute(base,roots,programs,legacy);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(unbound,roots,programs,legacy));
   BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,unbound));
   const auto bound=executor.execute(base,roots,programs,protocol);
   BOOST_CHECK(validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,bound));
   auto trx=packed->get_signed_transaction(); trx.context_free_data.clear();
   const packed_transaction actual_empty(trx,packed_transaction::compression_type::none);
   BOOST_CHECK(actual_empty.id()==packed->id());
   BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,actual_empty,programs,unbound));
   BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,actual_empty,programs,bound));
   auto captured_empty=protocol;
   captured_empty.tree.context_free_data=isolated_context_free_data::capture(actual_empty.get_context_free_data());
   const auto empty_bound=executor.execute(base,roots,programs,captured_empty);
   BOOST_CHECK(validate_isolated_mixed_tree_current_state(*chain.control,actual_empty,programs,empty_bound));
   BOOST_CHECK(chain.control->calculate_integrity_hash()==before);
}

BOOST_FIXTURE_TEST_CASE(current_exact_reads_identity_and_shared_table_metadata_are_checked_without_writes,mixed_owner_fixture) {
   const auto id=db().get<table_id_object,by_code_scope_table>(boost::make_tuple(mixed_rows.code,mixed_rows.scope,mixed_rows.table)).id;
   for(unsigned variant=0;variant<5;++variant) changed([&]{
      if(variant<3) db().modify(db().get<key_value_object,by_scope_primary>(boost::make_tuple(id,uint64_t{7})),[&](auto& r){
         if(variant==0) {const uint64_t n=77; r.value.assign(reinterpret_cast<const char*>(&n),8);}
         if(variant==1) r.payer="carol"_n;
         if(variant==2) {const std::array<char,9> bytes{}; r.value.assign(bytes.data(),bytes.size());}
      });
      else db().modify(db().get<table_id_object>(id),[&](auto& t){
         if(variant==3) ++t.count; else t.payer="carol"_n;
      });
   });
   changed([&]{
      db().remove(db().get<key_value_object,by_scope_primary>(boost::make_tuple(id,uint64_t{7})));
      db().create<key_value_object>([&](auto& r){
         r.t_id=id; r.primary_key=7; r.payer="bob"_n;
         const uint64_t n=10; r.value.assign(reinterpret_cast<const char*>(&n),8);
      });
   });
   {
      auto undo=db().start_undo_session(true);
      db().modify(db().get<key_value_object,by_scope_primary>(boost::make_tuple(id,uint64_t{9})),[](auto& r){
         const uint64_t n=99; r.value.assign(reinterpret_cast<const char*>(&n),8);
      });
      BOOST_CHECK(valid()); // captured but never observed by this mixed graph
   }
   BOOST_CHECK(valid());
}

BOOST_FIXTURE_TEST_CASE(frozen_structure_is_still_proved_before_any_live_adoption,mixed_owner_fixture) {
   for(unsigned variant=0;variant<8;++variant) {
      auto bad=*result;
      switch(variant) {
         case 0: bad.execution_fingerprint={}; break;
         case 1: bad.steps.back().effects.execution_fingerprint={}; break;
         case 2: bad.steps[0].group_index=bad.groups.size(); break;
         case 3: ++bad.steps[1].action_ordinal; break;
         case 4: bad.steps[0].calls[0].sender="carol"_n; break;
         case 5: bad.steps[0].calls[0].read_only=true; break;
         case 6: bad.steps[0].effects.elapsed=std::chrono::microseconds{-1}; break;
         case 7: bad.steps.back().effects.writes_begin=0; break;
      }
      BOOST_CHECK(!validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,bad));
   }
   BOOST_CHECK(valid());
}

BOOST_FIXTURE_TEST_CASE(every_owner_checkpoint_exception_propagates_without_mutating_the_ledger,mixed_owner_fixture) {
   const auto before=chain.control->calculate_integrity_hash(); size_t checkpoints=0;
   BOOST_REQUIRE(validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,*result,[&]{++checkpoints;}));
   BOOST_REQUIRE_GT(checkpoints,20u);
   for(size_t cutoff=1;cutoff<=checkpoints;++cutoff) {
      size_t seen=0;
      BOOST_CHECK_THROW(validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,*result,[&]{
         if(++seen==cutoff) throw std::runtime_error("mixed owner cancellation");
      }),std::runtime_error);
      BOOST_CHECK_EQUAL(seen,cutoff);
      seen=0;
      BOOST_CHECK_THROW(validate_isolated_mixed_tree_current_state(*chain.control,*packed,programs,*result,[&]{
         if(++seen==cutoff) throw primary_state_budget_exceeded();
      }),primary_state_budget_exceeded);
      BOOST_CHECK_EQUAL(seen,cutoff);
      BOOST_CHECK(chain.control->calculate_integrity_hash()==before);
   }
   BOOST_CHECK(valid());
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
void mixed_return_scope_oracle(bool clear_in_notification) {
   eosio::testing::tester chain;
   chain.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n});
   auto p=return_scope_graph(clear_in_notification);
   for(auto& item:p) {
      const auto& program=item.program;
      if(!program.code.empty())
         chain.set_code(program.receiver,std::vector<uint8_t>(program.code.begin(),program.code.end()));
      const auto& metadata=chain.control->db().get<account_metadata_object,by_name>(program.receiver);
      item.sync_call_supported=metadata.code_hash!=fc::sha256{} && chain.control->db().get<code_object,by_code_hash>(
         boost::make_tuple(metadata.code_hash,metadata.vm_type,metadata.vm_version)).sync_call_supported;
   }
   chain.produce_block(); seed(const_cast<chainbase::database&>(chain.control->db()));
   chainbase_transaction_state_view live(chain.control->db());
   const auto base=capture(live);
   auto c=context(); const auto& cfg=chain.control->get_global_properties().configuration;
   c.tree.max_inline_action_size=cfg.max_inline_action_size; c.tree.max_inline_action_depth=cfg.max_inline_action_depth;
   c.sync={cfg.max_sync_call_depth,cfg.max_sync_call_data_size,false,
      chain.control->is_builtin_activated(builtin_protocol_feature_t::get_sender_fix),chain.control->contracts_console()};
   c.tree.console_enabled=c.sync.console_enabled;
   const std::array roots{root(),root()};
   signed_transaction trx;
   for(const auto& r:roots) {
      action a; a.account=r.account; a.name=r.action; a.authorization={{"test"_n,"active"_n}};
      trx.actions.push_back(std::move(a));
   }
   const uint64_t value=91;
   trx.context_free_data={{reinterpret_cast<const char*>(&value),reinterpret_cast<const char*>(&value)+8}};
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key("test"_n,"active"),chain.control->get_chain_id());
   // Run the ORIGINAL path FIRST: retain evidence of its behavior even if the
   // private attempt incorrectly throws or chooses a different branch.
   const auto actual=chain.push_transaction(trx,fc::time_point::maximum(),eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US);
   BOOST_REQUIRE(actual && !actual->except);
   BOOST_REQUIRE_EQUAL(actual->action_traces.size(),10u);
   size_t seen=0;
   for(const auto& action:actual->action_traces) if(action.receiver=="carol"_n) {
      BOOST_CHECK_EQUAL(number(action.return_value),11u+2*seen);
      BOOST_CHECK_EQUAL(action.call_traces.size(),clear_in_notification?1u:0u);
      ++seen;
   }
   BOOST_CHECK_EQUAL(seen,2u);
   BOOST_CHECK_EQUAL(number(live.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value),14u);
   const auto canonical=chain.control->calculate_integrity_hash();
   isolated_mixed_tree_executor executor;
   const auto result=executor.execute(base,roots,p,c);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
   BOOST_REQUIRE_EQUAL(result.steps.size(),actual->action_traces.size());
   for(const auto& step:result.steps) {
      const auto& action=actual->action_traces.at(step.action_ordinal-1);
      BOOST_CHECK(action.receiver==step.receiver); BOOST_CHECK(action.return_value==step.effects.return_value);
      BOOST_CHECK_EQUAL(action.creator_action_ordinal.value,step.creator_action_ordinal);
      BOOST_CHECK_EQUAL(action.closest_unnotified_ancestor_action_ordinal.value,step.closest_unnotified_ancestor);
      BOOST_REQUIRE_EQUAL(action.call_traces.size(),step.calls.size());
      for(size_t i=0;i<step.calls.size();++i) {
         const auto& a=action.call_traces[i]; const auto& b=step.calls[i];
         BOOST_CHECK_EQUAL(a.call_ordinal.value,b.call_ordinal); BOOST_CHECK_EQUAL(a.sender_ordinal.value,b.sender_ordinal);
         BOOST_CHECK(a.receiver==b.receiver); BOOST_CHECK(a.return_value==b.return_value);
         BOOST_CHECK_EQUAL(bool(a.error_id),b.status<0); if(a.error_id) BOOST_CHECK_EQUAL(*a.error_id,b.status);
      }
   }
   BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),14u);
   BOOST_CHECK(chain.control->calculate_integrity_hash()==canonical);
}

void mixed_oracle(bool permission_failure=false,bool cursor_checks=false,bool negative=false) {
   eosio::testing::tester chain;
   chain.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n});
   auto p=negative?negative_graph():graph(permission_failure,cursor_checks);
   for(auto& item:p) {
      const auto& program=item.program;
      chain.set_code(program.receiver,std::vector<uint8_t>(program.code.begin(),program.code.end()));
      const auto& metadata=chain.control->db().get<account_metadata_object,by_name>(program.receiver);
      item.sync_call_supported=chain.control->db().get<code_object,by_code_hash>(
         boost::make_tuple(metadata.code_hash,metadata.vm_type,metadata.vm_version)).sync_call_supported;
   }
   chain.produce_block(); seed(const_cast<chainbase::database&>(chain.control->db()));
   chainbase_transaction_state_view live(chain.control->db());
   auto c=context(); const auto& cfg=chain.control->get_global_properties().configuration;
   c.tree.max_inline_action_size=cfg.max_inline_action_size; c.tree.max_inline_action_depth=cfg.max_inline_action_depth;
   c.sync={cfg.max_sync_call_depth,cfg.max_sync_call_data_size,false,
      chain.control->is_builtin_activated(builtin_protocol_feature_t::get_sender_fix),chain.control->contracts_console()};
   c.tree.console_enabled=c.sync.console_enabled;
   const std::array roots{root(),root()}; isolated_mixed_tree_executor executor;
   const auto before=chain.control->calculate_integrity_hash();
   const auto result=executor.execute(capture(live),roots,p,c);
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(result,roots,p,c));
   BOOST_CHECK(chain.control->calculate_integrity_hash()==before);
   signed_transaction trx;
   for(const auto& r:roots) {
      action a; a.account=r.account; a.name=r.action; a.data=r.data;
      for(const auto& [actor,permission]:r.authorizations) a.authorization.push_back({actor,permission});
      trx.actions.push_back(std::move(a));
   }
   const uint64_t value=91;
   trx.context_free_data={{reinterpret_cast<const char*>(&value),reinterpret_cast<const char*>(&value)+8}};
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key("test"_n,"active"),chain.control->get_chain_id());
   const auto ram_before=chain.control->get_resource_limits_manager().get_account_ram_usage("bob"_n);
   if(permission_failure) {
      BOOST_CHECK_THROW(chain.push_transaction(trx,fc::time_point::maximum(),
         eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US),fc::exception);
      BOOST_CHECK_EQUAL(number(live.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value),10u);
      BOOST_CHECK_EQUAL(chain.control->get_resource_limits_manager().get_account_ram_usage("bob"_n),ram_before);
      return; // private success is intentionally NOT send-time authority
   }
   const auto actual=chain.push_transaction(trx,fc::time_point::maximum(),eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US);
   BOOST_REQUIRE(actual && !actual->except);
   BOOST_REQUIRE_EQUAL(actual->action_traces.size(),result.steps.size());
   uint64_t sequence=0;
   for(const auto& s:result.steps) {
      const auto& a=actual->action_traces.at(s.action_ordinal-1); const auto& g=result.groups.at(s.group_index);
      BOOST_CHECK_EQUAL(a.action_ordinal.value,s.action_ordinal);
      BOOST_CHECK_EQUAL(a.creator_action_ordinal.value,s.creator_action_ordinal);
      BOOST_CHECK_EQUAL(a.closest_unnotified_ancestor_action_ordinal.value,s.closest_unnotified_ancestor);
      BOOST_CHECK(a.receiver==s.receiver); BOOST_CHECK(a.return_value==s.effects.return_value);
      BOOST_CHECK(a.act.account==g.input.account); BOOST_CHECK(a.act.name==g.input.action); BOOST_CHECK(a.act.data==g.input.data);
      std::vector<std::pair<name,name>> auth;
      for(const auto& permission:a.act.authorization) auth.emplace_back(permission.actor,permission.permission);
      BOOST_CHECK(auth==g.input.authorizations);
      BOOST_REQUIRE(a.receipt); BOOST_CHECK_GT(a.receipt->global_sequence,sequence); sequence=a.receipt->global_sequence;
      BOOST_CHECK(a.console.empty()); BOOST_CHECK(!a.except); BOOST_CHECK(!a.error_code);
      BOOST_REQUIRE_EQUAL(a.console_markers.size(),s.console_markers.size());
      for(size_t i=0;i<s.console_markers.size();++i) BOOST_CHECK_EQUAL(a.console_markers[i].value,s.console_markers[i]);
      BOOST_REQUIRE_EQUAL(a.call_traces.size(),s.calls.size());
      for(size_t i=0;i<s.calls.size();++i) {
         const auto& x=a.call_traces[i]; const auto& y=s.calls[i];
         BOOST_CHECK_EQUAL(x.call_ordinal.value,y.call_ordinal); BOOST_CHECK_EQUAL(x.sender_ordinal.value,y.sender_ordinal);
         BOOST_CHECK(x.receiver==y.receiver); BOOST_CHECK_EQUAL(x.read_only,y.read_only);
         BOOST_CHECK(x.data==y.data); BOOST_CHECK(x.return_value==y.return_value);
         BOOST_CHECK(x.console.empty()); BOOST_CHECK(!x.except); BOOST_CHECK(!x.error_code);
         BOOST_CHECK_EQUAL(bool(x.error_id),y.status<0); if(x.error_id) BOOST_CHECK_EQUAL(*x.error_id,y.status);
         BOOST_REQUIRE_EQUAL(x.console_markers.size(),y.console_markers.size());
         for(size_t j=0;j<y.console_markers.size();++j) BOOST_CHECK_EQUAL(x.console_markers[j].value,y.console_markers[j]);
      }
   }
   BOOST_CHECK_EQUAL(number(live.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value),negative?14u:18u);
   BOOST_CHECK_EQUAL(number(require_primary_mutation(result.state.writes().back()).value),negative?14u:18u);
   BOOST_CHECK_EQUAL(chain.control->get_resource_limits_manager().get_account_ram_usage("bob"_n),ram_before);
}
}
BOOST_AUTO_TEST_SUITE(parallel_mixed_tree_oracle_tests)
BOOST_AUTO_TEST_CASE(mixed_trace_return_values_queue_order_and_state_match_original) { mixed_oracle(); }
BOOST_AUTO_TEST_CASE(private_success_cannot_bypass_original_inline_authority_or_undo) { mixed_oracle(true); }
BOOST_AUTO_TEST_CASE(shared_notification_and_fresh_sync_inline_cursors_match_original) { mixed_oracle(false,true); }
BOOST_AUTO_TEST_CASE(negative_status_writes_and_return_reset_match_original) { mixed_oracle(false,false,true); }
BOOST_AUTO_TEST_CASE(notification_inherits_sync_return_across_empty_receiver_but_new_groups_start_empty) {
   mixed_return_scope_oracle(false);
}
BOOST_AUTO_TEST_CASE(notification_failed_call_clears_shared_return_for_later_receiver) {
   mixed_return_scope_oracle(true);
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
struct mixed_live_chain:eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time()+fc::microseconds(config::block_interval_us)); }
};
signed_transaction mixed_live_transaction(eosio::testing::tester& chain,uint64_t nonce,size_t roots=2,
                                          name account="test"_n,name method="run"_n) {
   signed_transaction trx;
   for(size_t i=0;i<roots;++i) {
      action act; act.account=account; act.name=method; act.authorization={{account,"active"_n}};
      act.data=fc::raw::pack(nonce+i); trx.actions.push_back(std::move(act));
   }
   trx.context_free_data={fc::raw::pack(uint64_t{91})};
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(account,"active"),chain.control->get_chain_id());
   return trx;
}
transaction_metadata_ptr mixed_live_metadata(eosio::testing::tester& chain,const signed_transaction& trx) {
   return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx,packed_transaction::compression_type::none),
      chain.control->get_chain_id(),fc::seconds(5),transaction_metadata::trx_type::input);
}
transaction_trace_ptr mixed_live_push(eosio::testing::tester& chain,const transaction_metadata_ptr& trx) {
   return chain.control->push_transaction(trx,fc::time_point::maximum(),fc::seconds(1),0,false,0);
}
std::vector<char> mixed_live_trace(const transaction_trace_ptr& trace) {
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id),fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->except?trace->except->code():int64_t{0}),fc::raw::pack(bool(trace->receipt)),
      fc::raw::pack(trace->block_num),fc::raw::pack(trace->block_time),fc::raw::pack(trace->scheduled),
      fc::raw::pack(trace->account_ram_delta),fc::raw::pack(trace->error_code)};
   // The producer has no final block ID while executing; validators already
   // know it. Compare this provenance separately against the sealed block.
   // Local clocks, exception_ptr and diagnostic exception stacks are excluded.
   BOOST_REQUIRE(!trace->failed_dtrx_trace); // these are ordinary, not deferred transactions
   if(trace->receipt) fields.push_back(fc::raw::pack(*trace->receipt));
   for(const auto& a:trace->action_traces) {
      fields.push_back(fc::raw::pack(a.action_ordinal)); fields.push_back(fc::raw::pack(a.creator_action_ordinal));
      fields.push_back(fc::raw::pack(a.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(a.receiver)); fields.push_back(fc::raw::pack(a.act));
      fields.push_back(fc::raw::pack(a.trx_id)); fields.push_back(fc::raw::pack(a.block_num));
      fields.push_back(fc::raw::pack(a.block_time));
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
struct mixed_live_corpus {
   genesis_state genesis=eosio::testing::base_tester::default_genesis();
   std::vector<isolated_sync_program> programs;
   flat_set<digest_type> reviewed;
   std::vector<std::vector<char>> blocks;
   explicit mixed_live_corpus(std::vector<isolated_sync_program> supplied,bool fresh=false):programs(std::move(supplied)) {
      if(fresh) genesis.initial_timestamp=block_timestamp_type(fc::time_point::now()-fc::seconds(5)).to_time_point();
      auto [key,pub,proof]=eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key=pub;
      genesis.initial_savanna->version = 2;
      genesis.initial_savanna->proof_of_possession=proof;
      for(const auto& p:programs) if(!p.program.code.empty())
         reviewed.insert(fc::sha256::hash(p.program.code.data(),p.program.code.size()));
      fc::temp_directory dir; mixed_live_chain source(configuration(dir,0),genesis);
      source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      auto seal=[&]{blocks.push_back(fc::raw::pack(*source.produce_block()));};
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call,builtin_protocol_feature_t::get_sender_fix});
      seal(); seal();
      source.create_accounts({"test"_n,"bob"_n,"carol"_n,"empty"_n}); seal();
      // All rows in the live corpus are created by original signed WASM actions.
      std::string stores;
      for(const auto& [id,value]:std::array<std::pair<uint64_t,uint64_t>,2>{{{7,10},{9,90}}})
         stores+="(i64.store (i32.const 0) "+mc(value)+")(drop (call $store "+mn(mixed_rows.scope)+
            mn(mixed_rows.table)+mn("bob"_n)+mc(id)+"(i32.const 0)(i32.const 8)))";
      const auto seed_code=mixed_program(stores);
      source.set_code("bob"_n,std::vector<uint8_t>(seed_code.begin(),seed_code.end()));
      auto seed_trx=mixed_live_transaction(source,1,1,"bob"_n,"init"_n);
      BOOST_REQUIRE(!source.push_transaction(seed_trx)->except); seal();
      for(const auto& p:programs) if(!p.program.code.empty())
         source.set_code(p.program.receiver,std::vector<uint8_t>(p.program.code.begin(),p.program.code.end()));
      seal();
   }
   controller::config configuration(const fc::temp_directory& dir,uint16_t workers,bool production=false) const {
      auto cfg=eosio::testing::base_tester::default_config(dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg); cfg.contracts_console=true;
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
void mixed_live_success(const std::vector<isolated_sync_program>& programs,size_t roots=2,size_t batch_size=1,
                        size_t expected_adopted=1,size_t expected_refreshes=0) {
   mixed_live_corpus corpus(programs);
   for(uint16_t workers:{0,1,2,4}) BOOST_TEST_CONTEXT("mixed original controller workers="<<workers) {
      fc::temp_directory pd,sd,vd;
      mixed_live_chain producer(corpus.configuration(pd,workers,true),corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd,0),corpus.genesis),
         parallel(corpus.configuration(vd,workers),corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start();
      std::vector<transaction_metadata_ptr> batch;
      for(size_t i=0;i<batch_size;++i) {
         const auto trx=mixed_live_transaction(producer,100+16*i,roots);
         batch.push_back(transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx,packed_transaction::compression_type::none),
            producer.control->get_chain_id(),fc::seconds(5),transaction_metadata::trx_type::input));
      }
      std::vector<std::vector<char>> originals,candidates,expected;
      std::vector<std::optional<block_id_type>> original_blocks,candidate_blocks;
      const auto collect=[&](auto& dest,const auto& event) {
         const auto trace=std::get<0>(event);
         if(std::any_of(batch.begin(),batch.end(),[&](const auto& tx){return trace->id==tx->id();})) {
            dest.push_back(mixed_live_trace(trace));
            auto& ids=&dest==&originals?original_blocks:candidate_blocks;
            ids.push_back(trace->producer_block_id);
            for(const auto& action:trace->action_traces) BOOST_CHECK(action.producer_block_id==trace->producer_block_id);
         }
      };
      boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& e){collect(originals,e);});
      boost::signals2::scoped_connection b=parallel.control->applied_transaction().connect([&](const auto& e){collect(candidates,e);});
      const auto before=producer.control->get_parallel_production_stats(),vb=parallel.control->get_parallel_execution_stats();
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),
         [&](const auto& tx){
            const auto trace=producer.control->push_transaction(tx,fc::time_point::maximum(),fc::seconds(1),0,false,0);
            BOOST_REQUIRE_MESSAGE(trace && !trace->except && trace->receipt,
               (trace && trace->except?trace->except->to_detail_string():"missing mixed production trace"));
            BOOST_CHECK_GT(trace->action_traces.size(),roots);
            BOOST_CHECK(!trace->producer_block_id);
            for(const auto& action:trace->action_traces) BOOST_CHECK(!action.producer_block_id);
            expected.push_back(mixed_live_trace(trace)); return true;
         }),batch_size);
      const auto after=producer.control->get_parallel_production_stats();
      const auto production_diagnostics=parallel_attempt_diagnostics(before,after);
      BOOST_TEST_MESSAGE("MIXED_LIVE production workers="<<workers<<" roots="<<roots<<" batch="<<batch_size
         <<" adopted="<<after.adopted-before.adopted<<" serial_fallbacks="<<after.serial_fallbacks-before.serial_fallbacks
         <<" "<<production_diagnostics);
      // Writes to bob/rows/7 require a fresh whole mixed tree. Read-only and
      // independent fixtures keep their initial results without any refresh.
      BOOST_TEST_CONTEXT("mixed production workers="<<workers<<" "<<production_diagnostics) {
         BOOST_CHECK_EQUAL(after.adopted-before.adopted,workers?expected_adopted:0u);
         if(workers) BOOST_CHECK_EQUAL(after.serial_fallbacks-before.serial_fallbacks,batch_size-expected_adopted);
         BOOST_CHECK_EQUAL(after.prefix_refresh_requests-before.prefix_refresh_requests,workers?expected_refreshes:0u);
         BOOST_CHECK_EQUAL(after.prefix_refresh_successes-before.prefix_refresh_successes,workers?expected_refreshes:0u);
      }
      const auto block=producer.finish_block();
      serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
      BOOST_CHECK(originals==expected); BOOST_CHECK(candidates==expected);
      BOOST_REQUIRE_EQUAL(originals.size(),batch_size); BOOST_REQUIRE_EQUAL(candidates.size(),batch_size);
      BOOST_REQUIRE_EQUAL(original_blocks.size(),batch_size); BOOST_REQUIRE_EQUAL(candidate_blocks.size(),batch_size);
      for(size_t i=0;i<batch_size;++i) {
         BOOST_REQUIRE(original_blocks[i]); BOOST_REQUIRE(candidate_blocks[i]);
         BOOST_CHECK(*original_blocks[i]==block->calculate_id()); BOOST_CHECK(*candidate_blocks[i]==block->calculate_id());
      }
      BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
      BOOST_CHECK(serial.control->calculate_integrity_hash()==parallel.control->calculate_integrity_hash());
      const auto validation_after=parallel.control->get_parallel_execution_stats();
      const auto validation_diagnostics=parallel_attempt_diagnostics(vb,validation_after);
      BOOST_TEST_MESSAGE("MIXED_LIVE validation workers="<<workers<<" roots="<<roots<<" batch="<<batch_size
         <<" adopted="<<validation_after.adopted-vb.adopted
         <<" serial_fallbacks="<<validation_after.serial_fallbacks-vb.serial_fallbacks<<" "<<validation_diagnostics);
      BOOST_TEST_CONTEXT("mixed validation workers="<<workers<<" "<<validation_diagnostics) {
         BOOST_CHECK_EQUAL(validation_after.adopted-vb.adopted,workers?expected_adopted:0u);
         BOOST_CHECK_EQUAL(validation_after.prefix_refresh_requests-vb.prefix_refresh_requests,workers?expected_refreshes:0u);
         BOOST_CHECK_EQUAL(validation_after.prefix_refresh_successes-vb.prefix_refresh_successes,workers?expected_refreshes:0u);
      }
   }
}

// Corrupt ONLY a future step after whole-tree proof, at an original notification
// event. The derived context executes the actual group, not fabricated traces
// for a selected receiver; queued inline actions and cursors are preserved.
struct mixed_late_context:apply_context {
   isolated_mixed_tree_result* corrupt;
   size_t step_to_corrupt;
   bool injected=false;
   bool probe_cpu=false,probed=false;
   mixed_late_context(controller& control,transaction_context& trx,uint32_t ordinal,
                      isolated_mixed_tree_result* result,size_t step,bool probe=false)
      :apply_context(control,trx,ordinal),corrupt(result),step_to_corrupt(step),probe_cpu(probe) {}
   void require_recipient(account_name account) override {
      apply_context::require_recipient(account);
      if(probe_cpu && !probed) {
         const auto before=trx_context.isolated_cpu_charged();
         BOOST_CHECK_THROW(trx_context.charge_isolated_mixed_cpu(fc::microseconds{},0,1),transaction_exception);
         BOOST_CHECK_THROW(trx_context.charge_isolated_cpu(fc::microseconds{},1),transaction_exception);
         BOOST_CHECK_THROW(trx_context.charge_isolated_notification_cpu(fc::microseconds{},0,1),transaction_exception);
         BOOST_CHECK_THROW(trx_context.charge_isolated_tree_cpu(fc::microseconds{},0,1),transaction_exception);
         BOOST_CHECK(trx_context.isolated_cpu_charged()==before);
         probed=true;
      }
      if(corrupt && !injected) {
         corrupt->steps.at(step_to_corrupt).effects.execution_fingerprint=fc::sha256::hash("late-mismatch",13);
         injected=true;
      }
   }
};

// Deterministic TEST-ONLY expiration at an actual original host boundary.
// No production limits or clocks are changed. The original VM and private
// replay encounter the identical print after the same preceding sync write.
struct mixed_console_timer_context:apply_context {
   platform_timer& timer;
   unsigned fault;
   std::string cutoff;
   bool injected=false;
   mixed_console_timer_context(controller& control,transaction_context& trx,platform_timer& timer,
                               unsigned fault,std::string cutoff)
      :apply_context(control,trx,1),timer(timer),fault(fault),cutoff(std::move(cutoff)) {}
   void console_append(std::string_view value) override {
      apply_context::console_append(value);
      if(fault && !injected && value==cutoff) {
         injected=true;
         chainbase_transaction_state_view live(control.db());
         BOOST_CHECK_EQUAL(number(live.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value),11u);
         BOOST_REQUIRE(trx_context.transaction_timer.timer_state()==platform_timer::state_t::running);
         if(fault==1) timer.interrupt_timer();
         else {
            trx_context.transaction_timer.stop();
            trx_context.transaction_timer.start(fc::time_point{}); // original platform timer: immediately expired
         }
         trx_context.checktime(); // original interrupt/CPU exception selection
         BOOST_FAIL("injected console timer did not throw");
      }
   }
};

void mixed_console_timer_failure_and_reuse() {
   const auto print=[](int n){return "(call $printi (i64.const "+std::to_string(n)+"))";};
   auto programs=graph();
   programs[0].program.code=mixed_program(print(11)+copy_sync("bob"_n)+print(22)+
      "(call $notify "+mn("carol"_n)+")"+print(33)+"(call $send0)"+print(44),"(i64.const 0)",
      {{false,child_bytes("bob"_n,"normal"_n)}},{},true);
   auto increment_body=increment(); increment_body.resize(increment_body.size()-std::string("(i64.const 0)").size());
   programs[1].program.code=mixed_program(print(55)+copy_sync("bob"_n),
      print(66)+increment_body+print(77)+"(i64.const 0)",{},{},true);
   programs[2].program.code=mixed_program(print(88)+copy_sync("bob"_n),"(i64.const 0)",{},{},true);
   mixed_live_corpus corpus(programs);
   fc::temp_directory directory; mixed_live_chain chain(corpus.configuration(directory,0),corpus.genesis);
   corpus.prefix(chain); chain.start();
   auto trx=mixed_live_transaction(chain,1700,1);
   trx.max_cpu_usage_ms=50; trx.signatures.clear();
   trx.sign(chain.get_private_key("test"_n,"active"),chain.control->get_chain_id());
   const packed_transaction packed(trx,packed_transaction::compression_type::none);
   isolated_mixed_tree_job input;
   const auto prepared=prepare_isolated_transaction(*chain.control,packed); BOOST_REQUIRE(prepared);
   for(const auto& a:*prepared) input.roots.push_back(a.input);
   for(const auto& p:programs) {
      const auto captured=prepare_isolated_sync_program(*chain.control,p.program.receiver); BOOST_REQUIRE(captured);
      input.programs.push_back(*captured);
   }
   input.context={current_action_tree_context(*chain.control),current_sync_context(*chain.control)};
   input.context.tree.context_free_data=isolated_context_free_data::capture(packed.get_context_free_data());
   chainbase_transaction_state_view live(chain.control->db()); const auto base=capture(live);
   isolated_mixed_tree_executor executor;
   const auto result=executor.execute(base,input.roots,input.programs,input.context);
   BOOST_REQUIRE(validate_isolated_mixed_tree_current_state(*chain.control,packed,input.programs,result));
   BOOST_REQUIRE_EQUAL(result.state.writes().size(),3u);
   const auto initial=chain.control->calculate_integrity_hash();
   const auto ram_before=chain.control->get_resource_limits_manager().get_account_ram_usage("bob"_n);
   const auto run=[&](bool adopted,unsigned fault,const std::string& cutoff) {
      std::vector<char> trace_bytes;
      {
         platform_timer timer; const auto start=fc::time_point::now();
         transaction_context tx(*chain.control,packed,packed.id(),transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna,start,transaction_metadata::trx_type::input);
         tx.block_deadline=start+fc::seconds(5); tx.explicit_billed_cpu_time=false;
         tx.init_for_input_trx(packed.get_unprunable_size(),packed.get_prunable_size());
         tx.trace->action_traces.emplace_back(*tx.trace,trx.actions[0],"test"_n,false,1,0,0);
         if(adopted) eosio::testing::mixed_adoption_test_access::install(tx,input,result);
         mixed_console_timer_context context(*chain.control,tx,timer,fault,cutoff);
         if(fault==1) BOOST_CHECK_THROW(context.exec(),interrupt_exception);
         else if(fault==2) BOOST_CHECK_THROW(context.exec(),tx_cpu_usage_exceeded);
         else context.exec();
         BOOST_CHECK_EQUAL(context.injected,fault!=0);
         BOOST_REQUIRE(!tx.trace->action_traces.empty());
         const auto& root=tx.trace->action_traces[0];
         BOOST_CHECK_EQUAL(root.console,!fault || cutoff=="44"?"11223344":cutoff=="33"?"112233":"1122");
         BOOST_REQUIRE_EQUAL(root.call_traces.size(),1u);
         BOOST_CHECK_EQUAL(root.call_traces[0].console,"6677");
         BOOST_REQUIRE_EQUAL(root.console_markers.size(),1u); BOOST_CHECK_EQUAL(root.console_markers[0].value,2u);
         BOOST_CHECK_EQUAL(number(live.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value),fault?11u:13u);
         if(fault) {
            BOOST_REQUIRE(root.except); BOOST_CHECK(!root.receipt);
            BOOST_CHECK_EQUAL(root.except->code(),fault==1?int64_t(interrupt_exception::code_value):int64_t(tx_cpu_usage_exceeded::code_value));
            BOOST_CHECK(!eosio::testing::mixed_adoption_test_access::adopted(tx));
         } else {
            BOOST_REQUIRE(root.receipt); BOOST_CHECK(!root.except);
            BOOST_CHECK_EQUAL(eosio::testing::mixed_adoption_test_access::adopted(tx),adopted);
            if(adopted) BOOST_CHECK_EQUAL(eosio::testing::mixed_adoption_test_access::consumed(tx),result.steps.size());
         }
         trace_bytes=mixed_live_trace(tx.trace);
         tx.undo(); BOOST_CHECK(eosio::testing::mixed_adoption_test_access::cleared(tx));
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash()==initial);
      BOOST_CHECK_EQUAL(chain.control->get_resource_limits_manager().get_account_ram_usage("bob"_n),ram_before);
      return trace_bytes;
   };
   for(unsigned fault:{1u,2u}) for(const std::string cutoff:{"22","33","44"})
      BOOST_TEST_CONTEXT("console timer fault="<<fault<<" cutoff="<<cutoff) {
      const auto original=run(false,fault,cutoff);
      BOOST_CHECK(run(true,fault,cutoff)==original);
      const auto healthy=run(false,0,cutoff);
      BOOST_CHECK(run(true,0,cutoff)==healthy);
      const auto reused=executor.execute(base,input.roots,input.programs,input.context);
      BOOST_CHECK(validate_isolated_mixed_tree_current_state(*chain.control,packed,input.programs,reused));
   }
}

void mixed_late_fallback(bool clear_return,bool explicit_cpu=true,unsigned depth=0) {
   mixed_live_corpus corpus(return_scope_graph(clear_return,depth));
   fc::temp_directory dir; mixed_live_chain chain(corpus.configuration(dir,0),corpus.genesis);
   corpus.prefix(chain); chain.start();
   const auto trx=mixed_live_transaction(chain,700,2);
   const packed_transaction packed(trx,packed_transaction::compression_type::none);
   isolated_mixed_tree_job input;
   const auto prepared=prepare_isolated_transaction(*chain.control,packed); BOOST_REQUIRE(prepared);
   for(const auto& p:*prepared) input.roots.push_back(p.input);
   for(const auto& p:corpus.programs) {
      const auto captured=prepare_isolated_sync_program(*chain.control,p.program.receiver); BOOST_REQUIRE(captured);
      input.programs.push_back(*captured);
   }
   input.context={current_action_tree_context(*chain.control),current_sync_context(*chain.control)};
   input.context.tree.context_free_data=isolated_context_free_data::capture(packed.get_context_free_data());
   chainbase_transaction_state_view live(chain.control->db());
   isolated_mixed_tree_executor executor;
   const auto good=executor.execute(capture(live),input.roots,input.programs,input.context);
   BOOST_REQUIRE(validate_isolated_mixed_tree_current_state(*chain.control,packed,input.programs,good));
   const auto initial=chain.control->calculate_integrity_hash();
   std::vector<char> expected;
   // Original, full replay, then every possible late receiver after the first.
   for(size_t mode=0;mode<=good.steps.size();++mode) BOOST_TEST_CONTEXT("late mixed fallback mode="<<mode) {
      {
         auto result=good;
         platform_timer timer; const auto start=fc::time_point::now();
         transaction_context transaction(*chain.control,packed,packed.id(),transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna,start,transaction_metadata::trx_type::input);
         transaction.block_deadline=start+fc::seconds(5); transaction.explicit_billed_cpu_time=explicit_cpu;
         transaction.billed_cpu_time_us=explicit_cpu?100:0;
         transaction.init_for_input_trx(packed.get_unprunable_size(),packed.get_prunable_size());
         BOOST_CHECK_THROW(transaction.charge_isolated_mixed_cpu(fc::microseconds{},0,1),transaction_exception);
         for(size_t i=0;i<trx.actions.size();++i)
            transaction.trace->action_traces.emplace_back(*transaction.trace,trx.actions[i],trx.actions[i].account,false,i+1,0,0);
         if(mode) eosio::testing::mixed_adoption_test_access::install(transaction,input,result);
         mixed_late_context first(*chain.control,transaction,1,mode>=2?&result:nullptr,mode-1,!explicit_cpu && mode>0);
         first.exec();
         apply_context second(*chain.control,transaction,2); second.exec();
         const auto actual=mixed_live_trace(transaction.trace);
         if(mode==0) expected=actual;
         else BOOST_CHECK(actual==expected);
         if(mode==1) {
            BOOST_CHECK(eosio::testing::mixed_adoption_test_access::adopted(transaction));
            BOOST_CHECK_EQUAL(eosio::testing::mixed_adoption_test_access::consumed(transaction),good.steps.size());
         } else if(mode>=2) {
            BOOST_CHECK(first.injected);
            BOOST_CHECK(!eosio::testing::mixed_adoption_test_access::adopted(transaction));
            BOOST_CHECK_EQUAL(eosio::testing::mixed_adoption_test_access::consumed(transaction),mode-1);
         }
         BOOST_CHECK(eosio::testing::mixed_adoption_test_access::cleared(transaction));
         BOOST_CHECK_EQUAL(number(live.read_primary({mixed_rows,primary_query_kind::find,7},8).row->value),14u+2*depth);
         fc::microseconds expected_cpu{};
         if(!explicit_cpu && mode>0) {
            BOOST_CHECK(first.probed);
            const auto consumed=eosio::testing::mixed_adoption_test_access::consumed(transaction);
            for(size_t i=0;i<consumed;++i) {
               expected_cpu+=fc::microseconds(good.steps[i].effects.elapsed.count());
               for(const auto& call:good.steps[i].calls) expected_cpu+=fc::microseconds(call.exclusive_elapsed.count());
            }
         }
         BOOST_CHECK_EQUAL(transaction.isolated_cpu_charged().count(),expected_cpu.count());
         BOOST_CHECK_THROW(transaction.charge_isolated_mixed_cpu(fc::microseconds{},0,1),transaction_exception);
         transaction.undo();
         BOOST_CHECK(eosio::testing::mixed_adoption_test_access::cleared(transaction));
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash()==initial);
   }
}

void mixed_original_failure_and_reuse(unsigned failure) {
   BOOST_REQUIRE_LE(failure,5u);
   auto programs=graph();
   programs[0].program.code=mixed_program(copy_sync("bob"_n)+"(call $notify "+mn("carol"_n)+")"+
      when("run"_n,"(call $send0)")+when("fail"_n,"(call $send1)"),"(i64.const 0)",
      {{false,child_bytes("bob"_n,"normal"_n)},
       {false,child_bytes("bob"_n,"fail"_n,failure==0)}});
   const auto bad_write="(call $update "+mf()+mn(failure==1?"carol"_n:"bob"_n)+
      "(i32.const 0)(i32.const "+std::to_string(failure==2?16:8)+"))";
   programs[1].program.code=mixed_program(copy_sync("bob"_n)+when("fail"_n,bad_write),increment());
   if(failure==3) programs=recreated_cursor_graph(true);
   if(failure==4) {
      programs[0].program.code=mixed_program("(call $notify "+mn("carol"_n)+")"+
         when("fail"_n,"(drop "+call_sync("bob"_n,0,1)+")")+when("run"_n,copy_sync("bob"_n))+
         "(call $send0)","(i64.const 0)",{{false,child_bytes("bob"_n,"normal"_n)}});
      auto body=increment(); body.resize(body.size()-std::string("(i64.const 0)").size());
      programs[1].program.code=mixed_program(copy_sync("bob"_n),
         body+"(if (get_local $size)(then (unreachable)))(i64.const 0)");
   }
   if(failure==5) {
      programs[0].program.code=mixed_program("(call $notify "+mn("empty"_n)+")"+
         when("fail"_n,"(drop "+call_sync("carol"_n,1)+")")+when("run"_n,copy_sync("carol"_n)));
      programs[1].program.code=mixed_program({},increment());
      programs[2].program.code=mixed_program({},copy_sync("bob"_n)+
         "(call $set (i32.const 0)(i32.const 16))(i64.const 0)");
      programs[3].program.code=mixed_program();
   }
   mixed_live_corpus corpus(programs);
   for(uint16_t workers:{0,1,2,4}) BOOST_TEST_CONTEXT("mixed original failure="<<failure<<" workers="<<workers) {
      fc::temp_directory pd,sd;
      mixed_live_chain producer(corpus.configuration(pd,workers,true),corpus.genesis),
         serial(corpus.configuration(sd,0),corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial);
      producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start();
      if(failure==2) {
         const auto usage=producer.control->get_resource_limits_manager().get_account_ram_usage("bob"_n);
         BOOST_REQUIRE(!producer.push_action("eosio"_n,"setalimits"_n,"eosio"_n,fc::mutable_variant_object()
            ("account","bob")("ram_bytes",usage)("net_weight",-1)("cpu_weight",-1))->except);
         const auto setup=producer.produce_block(); producer.control->abort_block();
         serial.push_block(setup); serial.control->abort_block(); producer.start();
      }
      serial.start();
      const auto metadata=[&](eosio::testing::tester& chain,const signed_transaction& trx) {
         return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx,packed_transaction::compression_type::none),
            chain.control->get_chain_id(),fc::seconds(5),transaction_metadata::trx_type::input);
      };
      const auto push=[](eosio::testing::tester& chain,const transaction_metadata_ptr& trx) {
         return chain.control->push_transaction(trx,fc::time_point::maximum(),fc::seconds(1),0,false,0);
      };
      const auto bad=mixed_live_transaction(producer,900,1,"test"_n,"fail"_n);
      const auto bad_meta=metadata(producer,bad);
      const auto before=producer.control->calculate_integrity_hash(),original_before=serial.control->calculate_integrity_hash();
      std::array<int64_t,4> ram;
      const std::array accounts{"test"_n,"bob"_n,"carol"_n,"empty"_n};
      for(size_t i=0;i<accounts.size();++i) ram[i]=producer.control->get_resource_limits_manager().get_account_ram_usage(accounts[i]);
      const auto stats=producer.control->get_parallel_production_stats();
      transaction_trace_ptr failed;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({bad_meta},fc::time_point::maximum(),
         [&](const auto& trx){failed=push(producer,trx); return true;}),1u);
      const auto reference=push(serial,metadata(serial,bad));
      BOOST_REQUIRE(failed && failed->except); BOOST_REQUIRE(reference && reference->except);
      BOOST_CHECK(mixed_live_trace(failed)==mixed_live_trace(reference));
      BOOST_CHECK(!failed->receipt); BOOST_CHECK(!reference->receipt);
      if(failure==2) BOOST_CHECK_EQUAL(failed->except->code(),ram_usage_exceeded::code_value);
      BOOST_REQUIRE(!failed->action_traces.empty());
      BOOST_CHECK_EQUAL(failed->action_traces[0].call_traces.size(),failure==5?2u:1u); // no duplicated canonical prefix
      BOOST_CHECK(producer.control->calculate_integrity_hash()==before);
      BOOST_CHECK(serial.control->calculate_integrity_hash()==original_before);
      BOOST_CHECK(!producer.control->is_known_unexpired_transaction(bad_meta->id()));
      for(size_t i=0;i<accounts.size();++i)
         BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(accounts[i]),ram[i]);
      const auto after=producer.control->get_parallel_production_stats();
      const auto failure_diagnostics=parallel_attempt_diagnostics(stats,after);
      BOOST_TEST_MESSAGE("MIXED_FAILURE original failure="<<failure<<" workers="<<workers
         <<" adopted="<<after.adopted-stats.adopted<<" serial_fallbacks="<<after.serial_fallbacks-stats.serial_fallbacks
         <<" "<<failure_diagnostics);
      BOOST_TEST_CONTEXT("mixed original failure="<<failure<<" workers="<<workers<<" "<<failure_diagnostics) {
         if(workers) {
            if(failure<3) BOOST_CHECK_GT(after.succeeded-stats.succeeded,0u); // private success is NOT authority or a RAM entitlement
            BOOST_CHECK_EQUAL(after.adopted-stats.adopted,failure==2?1u:0u); // execution adopted, not acceptance
         }
      }
      // Same chain, code and workers: a new healthy transaction must succeed,
      // while the failed transaction remains absent from the produced block.
      const auto healthy=mixed_live_transaction(producer,901,1);
      const auto healthy_meta=metadata(producer,healthy);
      transaction_trace_ptr accepted;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({healthy_meta},fc::time_point::maximum(),
         [&](const auto& trx){accepted=push(producer,trx); return true;}),1u);
      BOOST_REQUIRE_MESSAGE(accepted && !accepted->except && accepted->receipt,
         (accepted && accepted->except?accepted->except->to_detail_string():"missing healthy mixed trace"));
      const auto healthy_after=producer.control->get_parallel_production_stats();
      const auto healthy_diagnostics=parallel_attempt_diagnostics(after,healthy_after);
      BOOST_TEST_MESSAGE("MIXED_FAILURE healthy failure="<<failure<<" workers="<<workers
         <<" adopted="<<healthy_after.adopted-after.adopted
         <<" serial_fallbacks="<<healthy_after.serial_fallbacks-after.serial_fallbacks<<" "<<healthy_diagnostics);
      BOOST_TEST_CONTEXT("mixed healthy failure="<<failure<<" workers="<<workers<<" "<<healthy_diagnostics) {
         BOOST_CHECK_EQUAL(healthy_after.adopted-after.adopted,workers?1u:0u);
      }
      const auto expected=mixed_live_trace(accepted);
      std::vector<std::vector<char>> observed;
      boost::signals2::scoped_connection collect=serial.control->applied_transaction().connect([&](const auto& e){
         const auto trace=std::get<0>(e); if(trace->id==healthy_meta->id()) observed.push_back(mixed_live_trace(trace));
      });
      serial.control->abort_block(); const auto block=producer.finish_block();
      BOOST_REQUIRE_EQUAL(block->transactions.size(),1u);
      serial.push_block(block); serial.control->abort_block();
      BOOST_REQUIRE_EQUAL(observed.size(),1u); BOOST_CHECK(observed[0]==expected);
      BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
      BOOST_CHECK(!serial.control->is_known_unexpired_transaction(bad_meta->id()));
   }
}

std::vector<isolated_sync_program> mixed_process_programs() {
   auto programs=return_scope_graph(false,2);
   // Exercise an actual CF data read, not merely an empty CF receiver. The
   // ordinary notification still shares its group's latest sync return.
   programs[3].program.code=mixed_program(
      when("run"_n,ma("(i32.eq (call $ret (i32.const 0)(i32.const 16))(i32.const 16))"))+
      when("pure"_n,ma("(i32.eq (call $cfdata (i32.const 0)(i32.const 0)(i32.const 8))(i32.const 8))")+
         ma("(i64.eq (i64.load (i32.const 0))(i64.const 91))")));
   return programs;
}
fc::variant mixed_process_expectations(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE(trace && !trace->except && trace->receipt);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(),10u);
   fc::variants actions;
   std::vector<std::pair<uint64_t,uint32_t>> sequences;
   size_t calls=0;
   for(const auto& a:trace->action_traces) {
      BOOST_REQUIRE(a.receipt); BOOST_REQUIRE(!a.except && !a.error_code);
      BOOST_REQUIRE(a.console.empty() && a.console_markers.empty() && a.account_ram_deltas.empty());
      sequences.emplace_back(a.receipt->global_sequence,a.action_ordinal.value);
      fc::variants children;
      for(const auto& c:a.call_traces) {
         ++calls;
         BOOST_REQUIRE(!c.except && !c.error_code && !c.error_id);
         BOOST_REQUIRE(c.console.empty() && c.console_markers.empty());
         children.emplace_back(fc::mutable_variant_object()("call_ordinal",c.call_ordinal)
            ("sender_ordinal",c.sender_ordinal)("receiver",c.receiver)("read_only",c.read_only)
            ("hex_data",c.data)("return_value_hex_data",c.return_value));
      }
      actions.emplace_back(fc::mutable_variant_object()("action_ordinal",a.action_ordinal)
         ("creator_action_ordinal",a.creator_action_ordinal)
         ("closest_unnotified_ancestor_action_ordinal",a.closest_unnotified_ancestor_action_ordinal)
         ("receiver",a.receiver)("context_free",a.context_free)("act",a.act)
         ("return_value_hex_data",a.return_value)("calls",children));
   }
   BOOST_REQUIRE_EQUAL(calls,8u);
   std::sort(sequences.begin(),sequences.end());
   std::vector<uint32_t> order;
   for(const auto& [sequence,ordinal]:sequences) order.push_back(ordinal);
   return fc::mutable_variant_object()("actions",actions)("execution_order",order);
}
void mixed_export_producer(const char* destination) {
   const std::filesystem::path directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
   mixed_live_corpus corpus(mixed_process_programs(),true);
   BOOST_REQUIRE(fc::json::from_string(fc::json::to_string(corpus.genesis,fc::time_point::maximum()))
      .as<genesis_state>().compute_chain_id()==corpus.genesis.compute_chain_id());
   fc::temp_directory source_dir;
   auto cfg=corpus.configuration(source_dir,0);
   cfg.contracts_console=false; // exactly the real process fixture's configuration
   mixed_live_chain source(cfg,corpus.genesis);
   fc::variants checkpoints,transactions;
   for(const auto& bytes:corpus.blocks) {
      const auto block=std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes));
      source.push_block(block); source.control->abort_block();
      const auto file="block-"+std::to_string(block->block_num())+".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block,directory/file));
      checkpoints.emplace_back(fc::mutable_variant_object()("block_num",block->block_num())
         ("block_id",source.head().id())("lib",source.last_irreversible_block_num())
         ("integrity_hash",source.control->calculate_integrity_hash())("file",file));
   }
   source.start();
   const auto submission_state=source.control->calculate_integrity_hash();
   for(unsigned i=0;i<16;++i) {
      auto trx=mixed_live_transaction(source,40000+2*i);
      trx.expiration=fc::time_point_sec(fc::time_point::now()+fc::seconds(1800));
      trx.signatures.clear(); trx.sign(source.get_private_key("test"_n,"active"),source.control->get_chain_id());
      const packed_transaction packed(trx,packed_transaction::compression_type::none);
      // The real paused producer uses HEAD/ephemeral: each HTTP speculation
      // is undone, then the queue is applied cumulatively during production.
      // Generate each submission oracle from the SAME original head state;
      // do not confuse its return bytes with later actual production traces.
      const auto trace=source.push_transaction(trx);
      transactions.emplace_back(fc::mutable_variant_object()("id",packed.id())("packed",packed)
         ("actions",trx.actions.size())("expected",mixed_process_expectations(trace)));
      BOOST_CHECK_EQUAL(number(trace->action_traces[0].return_value),13u);
      BOOST_CHECK_EQUAL(number(trace->action_traces[1].return_value),17u);
      source.control->abort_block(); source.start();
      BOOST_CHECK(source.control->calculate_integrity_hash()==submission_state);
   }
   BOOST_REQUIRE(fc::json::save_to_file(corpus.genesis,directory/"genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions,directory/"transactions.json"));
   // Deterministic disposable TEST credentials only, never log or install.
   auto [key,pub,proof]=eosio::testing::get_bls_key("eosio"_n);
   const std::vector<std::string> providers{
      eosio::testing::base_tester::get_public_key("eosio"_n,"active").to_string({})+"=KEY:"+
         eosio::testing::base_tester::get_private_key("eosio"_n,"active").to_string({}),
      pub.to_string()+"=KEY:"+key.to_string()};
   BOOST_REQUIRE(fc::json::save_to_file(providers,directory/"TEST-ONLY-keys.json"));
   const std::vector<fc::sha256> hashes(corpus.reviewed.begin(),corpus.reviewed.end());
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format",1)
      ("fixture_kind","mixed-sync-notification-inline-primary")("chain_id",source.control->get_chain_id())
      ("code_hashes",hashes)("checkpoints",checkpoints),directory/"manifest.json"));
}
void mixed_export_recovery(const char* destination) {
   const std::filesystem::path directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
   mixed_live_corpus fixture(mixed_process_programs());
   BOOST_REQUIRE(fc::json::save_to_file(fixture.genesis,directory/"genesis.json"));
   BOOST_REQUIRE(std::filesystem::create_directory(directory/"blocks"));
   fc::variants checkpoints,forks;
   std::vector<std::vector<char>> corpus;
   fc::temp_directory source_dir;
   const auto cfg=fixture.configuration(source_dir,0);
   {
      mixed_live_chain source(cfg,fixture.genesis);
      source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      const auto record=[&](const signed_block_ptr& block) {
         source.control->abort_block();
         const auto file="block-"+std::to_string(block->block_num())+".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block,directory/file));
         corpus.push_back(fc::raw::pack(*block));
         checkpoints.emplace_back(fc::mutable_variant_object()("block_num",block->block_num())
            ("block_id",source.head().id())("lib",source.last_irreversible_block_num())
            ("integrity_hash",source.control->calculate_integrity_hash())("file",file));
      };
      for(const auto& bytes:fixture.blocks) {
         const auto block=std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes));
         source.push_block(block); record(block);
      }
      uint64_t nonce=50000;
      for(unsigned i=0;i<16;++i) {
         source.start();
         for(unsigned j=0;j<3;++j) {
            auto trx=mixed_live_transaction(source,nonce); nonce+=2;
            BOOST_REQUIRE(!source.push_transaction(trx)->except);
         }
         record(source.produce_block());
      }
   }
   for(const char* file:{"blocks.log","blocks.index"})
      BOOST_REQUIRE(std::filesystem::copy_file(cfg.blocks_dir/file,directory/"blocks"/file));
   for(uint16_t workers:{0,1,2,4}) {
      fc::temp_directory sd,pd;
      eosio::testing::tester serial(fixture.configuration(sd,0),fixture.genesis),
         parallel(fixture.configuration(pd,workers),fixture.genesis);
      std::vector<std::vector<char>> expected,observed;
      boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& event) {
         expected.push_back(mixed_live_trace(std::get<0>(event)));
      });
      boost::signals2::scoped_connection b=parallel.control->applied_transaction().connect([&](const auto& event) {
         observed.push_back(mixed_live_trace(std::get<0>(event)));
      });
      for(const auto& bytes:corpus) {
         expected.clear(); observed.clear();
         serial.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
         parallel.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
         serial.control->abort_block(); parallel.control->abort_block();
         BOOST_CHECK(expected==observed); BOOST_CHECK(serial.head().id()==parallel.head().id());
         BOOST_CHECK_EQUAL(serial.last_irreversible_block_num(),parallel.last_irreversible_block_num());
         BOOST_CHECK(serial.control->calculate_integrity_hash()==parallel.control->calculate_integrity_hash());
      }
      if(workers) {
         const auto stats=parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_GT(stats.adopted,0u); BOOST_CHECK_GT(stats.serial_fallbacks,0u);
      }
   }
   const size_t prefix_blocks=fixture.blocks.size()+1;
   fc::temp_directory fd,sd,rd;
   mixed_live_chain first(fixture.configuration(fd,0),fixture.genesis),second(fixture.configuration(sd,0),fixture.genesis),
      reference(fixture.configuration(rd,0),fixture.genesis);
   for(auto* chain:{&first,&second,&reference}) for(size_t i=0;i<prefix_blocks;++i) {
      chain->push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[i])));
      chain->control->abort_block();
   }
   first.set_node_finalizers(std::array<name,1>{"eosio"_n});
   second.set_node_finalizers(std::array<name,1>{"eosio"_n});
   const auto fork=[&](const signed_block_ptr& block) {
      reference.push_block(block); reference.control->abort_block();
      BOOST_REQUIRE(reference.head().id()==block->calculate_id());
      const auto file="fork-"+std::to_string(forks.size())+".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block,directory/file));
      forks.emplace_back(fc::mutable_variant_object()("block_num",reference.head().block_num())
         ("block_id",reference.head().id())("lib",reference.last_irreversible_block_num())
         ("integrity_hash",reference.control->calculate_integrity_hash())("file",file));
   };
   const auto balance=[&] {
      chainbase_transaction_state_view view(reference.control->db());
      const auto row=view.read_primary({mixed_rows,primary_query_kind::find,7},8);
      BOOST_REQUIRE(row.row); return number(row.row->value);
   };
   const auto before=balance();
   auto one=mixed_live_transaction(first,60000,2); BOOST_REQUIRE(!first.push_transaction(one)->except);
   fork(first.produce_block()); BOOST_CHECK_EQUAL(balance(),before+8);
   // Different root counts give different real balances on competing forks;
   // replacing one constant with itself would not prove rollback.
   auto two=mixed_live_transaction(second,60100,1); BOOST_REQUIRE(!second.push_transaction(two)->except);
   fork(second.produce_block(fc::milliseconds(1000))); BOOST_CHECK_EQUAL(balance(),before+4);
   auto three=mixed_live_transaction(second,60200,2); BOOST_REQUIRE(!second.push_transaction(three)->except);
   fork(second.produce_block()); BOOST_CHECK_EQUAL(balance(),before+12);
   const std::vector<fc::sha256> hashes(fixture.reviewed.begin(),fixture.reviewed.end());
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format",1)
      ("fixture_kind","mixed-sync-notification-inline-recovery")("chain_id",fixture.genesis.compute_chain_id())
      ("code_hashes",hashes)("checkpoints",checkpoints)("fork_prefix_blocks",prefix_blocks)("forks",forks),
      directory/"manifest.json"));
}
}

BOOST_AUTO_TEST_SUITE(parallel_mixed_tree_adoption_tests)
BOOST_AUTO_TEST_CASE(export_fresh_mixed_producer_fixture) {
   const char* destination=std::getenv("PUREEOS_PARALLEL_PRODUCER_MIXED_EXPORT_DIR");
   if(!destination) { BOOST_TEST_MESSAGE("Mixed producer export not requested"); return; }
   mixed_export_producer(destination);
}
BOOST_AUTO_TEST_CASE(export_checked_mixed_recovery_corpus) {
   const char* destination=std::getenv("PUREEOS_PARALLEL_MIXED_RECOVERY_EXPORT_DIR");
   if(!destination) { BOOST_TEST_MESSAGE("Mixed recovery export not requested"); return; }
   mixed_export_recovery(destination);
}
BOOST_AUTO_TEST_CASE(production_and_validation_replay_shared_cursors_both_queues_and_conflicting_tail) {
   mixed_live_success(graph(false,true),2,3,3,2);
}
BOOST_AUTO_TEST_CASE(notification_and_inline_first_sync_calls_upgrade_to_mixed_execution) {
   for(bool notification:{true,false}) {
      auto p=graph();
      p[0].program.code=notification?mixed_program("(call $notify "+mn("carol"_n)+")"):
         mixed_program("(call $send0)","(i64.const 0)",{{false,child_bytes("bob"_n,"normal"_n)}});
      mixed_live_success(p);
   }
}
BOOST_AUTO_TEST_CASE(group_return_scope_empty_receivers_and_negative_calls_match_original_controller) {
   mixed_live_success(return_scope_graph(false)); mixed_live_success(return_scope_graph(true));
   mixed_live_success(negative_graph());
}
BOOST_AUTO_TEST_CASE(every_late_fallback_keeps_prefix_cursors_queues_and_inherited_sync_return) {
   mixed_late_fallback(false); mixed_late_fallback(true);
}
BOOST_AUTO_TEST_CASE(independent_mixed_batches_adopt_every_transaction) {
   auto programs=graph();
   programs[1].program.code=mixed_program(when("normal"_n,copy_sync("bob"_n)+"(call $out (i32.const 0)(i32.const 16))"),
      "(i64.store (i32.const 0)(i64.const 11))(i64.store (i32.const 8)(call $sender))"
      "(call $set (i32.const 0)(i32.const 16))(i64.const 0)");
   mixed_live_success(programs,2,4,4);
}
BOOST_AUTO_TEST_CASE(private_cpu_is_charged_once_per_adopted_step_and_cannot_switch_modes) {
   mixed_late_fallback(false,false); mixed_late_fallback(true,false);
}
BOOST_AUTO_TEST_CASE(console_prefix_and_sync_write_undo_match_original_on_exact_late_cpu_and_interrupt_boundaries) {
   mixed_console_timer_failure_and_reuse();
}
BOOST_AUTO_TEST_CASE(original_inline_authority_and_ram_checks_undo_then_reuse_the_same_workers) {
   mixed_original_failure_and_reuse(0); mixed_original_failure_and_reuse(1);
}
BOOST_AUTO_TEST_CASE(transaction_final_ram_limit_rejects_even_a_fully_adopted_mixed_tree) {
   mixed_original_failure_and_reuse(2);
}
BOOST_AUTO_TEST_CASE(recursive_mixed_calls_keep_return_scope_and_charge_only_exclusive_cpu) {
   mixed_live_success(return_scope_graph(false,4)); mixed_live_success(return_scope_graph(true,4));
   mixed_late_fallback(false,false,4); mixed_late_fallback(true,false,4);
}
BOOST_AUTO_TEST_CASE(readonly_is_inherited_across_nested_calls_and_multiple_ordinary_receivers) {
   mixed_live_success(readonly_graph(),2,2,2);
}
BOOST_AUTO_TEST_CASE(child_recreation_invalidates_shared_ancestor_cursors_but_inline_starts_fresh) {
   mixed_live_success(recreated_cursor_graph());
}
BOOST_AUTO_TEST_CASE(stale_cursor_trap_and_inherited_readonly_failures_undo_and_reuse_workers) {
   mixed_original_failure_and_reuse(3); mixed_original_failure_and_reuse(4); mixed_original_failure_and_reuse(5);
}
BOOST_AUTO_TEST_CASE(mixed_queue_stop_exception_identity_abort_cancel_and_reentry_clear_results) {
   mixed_live_corpus corpus(return_scope_graph(false));
   for(unsigned mode=0;mode<7;++mode) BOOST_TEST_CONTEXT("mixed queue mode="<<mode) {
      fc::temp_directory dir; mixed_live_chain chain(corpus.configuration(dir,2,true),corpus.genesis);
      corpus.prefix(chain); chain.start();
      const std::vector<transaction_metadata_ptr> batch{
         mixed_live_metadata(chain,mixed_live_transaction(chain,1100)),
         mixed_live_metadata(chain,mixed_live_transaction(chain,1102))};
      const auto snapshot=chain.control->calculate_integrity_hash();
      const auto before=chain.control->get_parallel_production_stats();
      size_t callbacks=0;
      const auto deadline=mode==3?fc::time_point::now()-fc::seconds(1):fc::time_point::maximum();
      const auto run=[&]{return chain.control->for_each_parallel_production_transaction(batch,deadline,[&](const auto& trx){
         ++callbacks;
         if(mode==0) return false;
         if(mode==1) {chain.control->abort_block();chain.start();return true;}
         if(mode==2) throw std::runtime_error("mixed queue callback failed");
         if(mode==4) {
            BOOST_CHECK_THROW(chain.control->for_each_parallel_production_transaction(batch,deadline,[](const auto&){return true;}),
               transaction_exception);
            return false;
         }
         if(mode==5) {
            const auto other=mixed_live_metadata(chain,trx->packed_trx()->get_signed_transaction());
            BOOST_REQUIRE(!mixed_live_push(chain,other)->except);return true;
         }
         BOOST_REQUIRE(!mixed_live_push(chain,trx)->except);
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
      const auto fresh=mixed_live_metadata(chain,mixed_live_transaction(chain,1120));
      const auto reset=chain.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({fresh},fc::time_point::maximum(),[&](const auto& trx){
         const auto trace=mixed_live_push(chain,trx);
         BOOST_REQUIRE(trace && !trace->except && trace->receipt);
         BOOST_REQUIRE_GE(trace->action_traces.size(),2u);
         BOOST_CHECK_EQUAL(trace->action_traces[0].call_traces.size(),1u);
         BOOST_CHECK_EQUAL(trace->action_traces[1].call_traces.size(),1u);return true;
      }),1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted-reset.adopted,1u);
   }
}
BOOST_AUTO_TEST_CASE(mixed_roots_cannot_bypass_signatures_duplicate_checks_or_abort_reset) {
   mixed_live_corpus corpus(return_scope_graph(false));
   for(uint16_t workers:{1,2,4}) BOOST_TEST_CONTEXT("mixed authorization workers="<<workers) {
      fc::temp_directory dir; mixed_live_chain chain(corpus.configuration(dir,workers,true),corpus.genesis);
      corpus.prefix(chain);chain.start();
      const auto valid=mixed_live_transaction(chain,1200);
      auto invalid=valid; invalid.signatures.clear();
      invalid.sign(chain.get_private_key("bob"_n,"active"),chain.control->get_chain_id());
      const auto bad=mixed_live_metadata(chain,invalid),good=mixed_live_metadata(chain,valid);
      BOOST_CHECK(bad->id()==good->id()); // unsigned transaction identity is not an authorization proof
      const auto snapshot=chain.control->calculate_integrity_hash();
      const auto before=chain.control->get_parallel_production_stats();
      transaction_trace_ptr trace;
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({bad},fc::time_point::maximum(),[&](const auto& tx){
         trace=mixed_live_push(chain,tx);return true;
      }),1u);
      BOOST_REQUIRE(trace && trace->except);BOOST_CHECK(!trace->receipt);BOOST_CHECK(trace->action_traces.empty());
      BOOST_CHECK(chain.control->calculate_integrity_hash()==snapshot);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(good->id()));
      const auto rejected=chain.control->get_parallel_production_stats();
      BOOST_CHECK_GT(rejected.succeeded-before.succeeded,0u);
      BOOST_CHECK_EQUAL(rejected.adopted-before.adopted,0u);
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({good},fc::time_point::maximum(),[&](const auto& tx){
         trace=mixed_live_push(chain,tx);return true;
      }),1u);
      BOOST_REQUIRE(trace && !trace->except && trace->receipt);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted-rejected.adopted,1u);
      const auto accepted=chain.control->calculate_integrity_hash();
      const auto duplicate=mixed_live_push(chain,good);
      BOOST_REQUIRE(duplicate && duplicate->except);
      BOOST_CHECK_EQUAL(duplicate->except->code(),tx_duplicate::code_value);
      BOOST_CHECK(chain.control->calculate_integrity_hash()==accepted);
      chain.control->abort_block();chain.start();
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(good->id()));
      const auto fresh=mixed_live_metadata(chain,mixed_live_transaction(chain,1210));
      const auto reset=chain.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({fresh},fc::time_point::maximum(),[&](const auto& tx){
         trace=mixed_live_push(chain,tx);return true;
      }),1u);
      BOOST_REQUIRE(trace && !trace->except && trace->receipt);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted-reset.adopted,1u);
   }
}
BOOST_AUTO_TEST_SUITE_END()
