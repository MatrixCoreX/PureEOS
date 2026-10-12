#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

#include "parallel_test_diagnostics.hpp"

namespace {
std::string cf_name(name n) { return "(i64.const " + std::to_string(n.to_uint64_t()) + ")"; }
std::string cf_escape(std::span<const char> bytes) {
   constexpr char hex[] = "0123456789abcdef";
   std::string out;
   for (unsigned char c : bytes) { out += '\\'; out += hex[c >> 4]; out += hex[c & 15]; }
   return out;
}
std::vector<char> cf_pack(name receiver, name method, bool auth = false) {
   action act; act.account = receiver; act.name = method; act.data = {'x','y','z'};
   if (auth) act.authorization = {{receiver, "active"_n}};
   return fc::raw::pack(act);
}
std::string cf_when(name method, const std::string& body) {
   return "(if (i64.eq (get_local $action) " + cf_name(method) + ") (then " + body + "))";
}
std::vector<char> cf_program(const std::string& body = {},
      const std::vector<std::pair<bool,std::vector<char>>>& sends = {}, const std::string& imports = {},
      const std::string& start_body = {}) {
   std::string data, functions;
   size_t offset = 1024;
   for (size_t i = 0; i < sends.size(); ++i) {
      const auto& [cf, bytes] = sends[i];
      data += "(data (i32.const " + std::to_string(offset) + ") \"" + cf_escape(bytes) + "\")";
      functions += "(func $send" + std::to_string(i) + " (call $" + (cf ? "cfi" : "inl") +
         " (i32.const " + std::to_string(offset) + ") (i32.const " + std::to_string(bytes.size()) + ")))";
      offset += bytes.size();
   }
   if (!start_body.empty()) functions += "(func $start " + start_body + ")(start $start)";
   const auto wasm = wast_to_wasm(R"((module
      (import "env" "send_inline" (func $inl (param i32 i32)))
      (import "env" "send_context_free_inline" (func $cfi (param i32 i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "get_context_free_data" (func $data (param i32 i32 i32) (result i32)))
      (import "env" "eosio_assert_code" (func $assert (param i32 i64)))
      )" + imports + "(memory 1 1)" + data + functions + R"(
      (func (export "apply") (param $receiver i64) (param $account i64) (param $action i64) )" + body + "))");
   return {wasm.begin(), wasm.end()};
}
isolated_action_input cf_root() {
   isolated_action_input input;
   input.receiver = input.account = "test"_n; input.action = "run"_n;
   input.authorizations = {{"test"_n, "active"_n}};
   return input;
}
isolated_action_tree_context cf_context(const std::vector<std::vector<char>>& data = {}) {
   isolated_action_tree_context context;
   context.context_free_data = isolated_context_free_data::capture(data);
   return context;
}
std::vector<isolated_receiver_program> cf_graph(const std::string& pure_checks = {}, bool writes = false) {
   const auto write_imports = writes ? R"(
      (import "env" "read_action_data" (func $read (param i32 i32)(result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32)(result i32))))" : "";
   const auto write = writes ? "(drop (call $read (i32.const 0)(i32.const 8))) (drop (call $store (i64.load (i32.const 0)) " +
      cf_name("rows"_n) + cf_name("test"_n) + "(i64.const 1)(i32.const 0)(i32.const 8)))" : "";
   return {
      {"test"_n, cf_program(write + "(call $send0)(call $send1)(call $notify " + cf_name("dave"_n) +
         ")(call $send2)(call $send3)", {{false,cf_pack("bob"_n,"normal"_n)}, {true,cf_pack("carol"_n,"pure"_n)},
         {false,cf_pack("dave"_n,"normal"_n)}, {true,cf_pack("bob"_n,"pure"_n)}},write_imports), {}},
      {"bob"_n, cf_program(cf_when("pure"_n,pure_checks)+cf_when("normal"_n,"(call $send0)(call $send1)"),
         {{true,cf_pack("dave"_n,"pure"_n)}, {false,cf_pack("carol"_n,"normal"_n)}}), {}},
      {"carol"_n, cf_program(cf_when("pure"_n,pure_checks)), {}},
      {"dave"_n, cf_program(cf_when("pure"_n,pure_checks)+cf_when("run"_n,"(call $send0)"), {{true,cf_pack("carol"_n,"pure"_n)}}), {}}
   };
}
std::vector<isolated_receiver_program> cf_pair(std::vector<char> child, bool auth = false) {
   return {{"test"_n, cf_program("(call $send0)", {{true,cf_pack("bob"_n,"pure"_n,auth)}}), {}},
           {"bob"_n, std::move(child), {}}};
}
std::vector<char> cf_adjacent_program(const std::string& api, uint32_t output) {
   const auto imports = R"((import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32)(result i32))))" +
      std::string("(import \"env\" \"") + api + "\" (func $adj (param i32 i32)(result i32)))";
   return cf_program("(drop (call $adj (call $store " + cf_name("fresh"_n) + cf_name("rows"_n) +
      cf_name("test"_n) + "(i64.const 1)(i32.const 64)(i32.const 8)) (i32.const " + std::to_string(output) + ")))",{},imports);
}
struct cf_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   cf_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      base = frozen_primary_state::capture_queries(view, {}, {});
   }
};
std::string cf_data_assertions() {
   return R"(
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 0)(i32.const 0)) (i32.const 3)) (i64.const 1))
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 0)(i32.const 2)) (i32.const 2)) (i64.const 2))
      (call $assert (i32.eq (i32.load16_u (i32.const 0)) (i32.const 25185)) (i64.const 3))
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 8)(i32.const 20)) (i32.const 3)) (i64.const 4))
      (call $assert (i32.eq (i32.load8_u (i32.const 10)) (i32.const 99)) (i64.const 5))
      (call $assert (i32.eq (call $data (i32.const 1)(i32.const 0)(i32.const 9)) (i32.const 0)) (i64.const 6))
      (call $assert (i32.eq (i32.load16_u (i32.const 0)) (i32.const 25185)) (i64.const 12))
      (call $assert (i32.eq (call $data (i32.const 1)(i32.const 65536)(i32.const 0)) (i32.const 0)) (i64.const 13))
      (call $assert (i32.eq (call $data (i32.const 2)(i32.const 0)(i32.const 9)) (i32.const -1)) (i64.const 7))
      (call $assert (i32.eq (call $data (i32.const -1)(i32.const 0)(i32.const 0)) (i32.const -1)) (i64.const 8))
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 65536)(i32.const 0)) (i32.const 3)) (i64.const 9))
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 65535)(i32.const 1)) (i32.const 1)) (i64.const 10))
      (call $assert (i32.eq (i32.load8_u (i32.const 65535)) (i32.const 97)) (i64.const 11)))";
}
const std::string cf_input_imports = R"(
   (import "env" "current_receiver" (func $receiver (result i64)))
   (import "env" "action_data_size" (func $size (result i32)))
   (import "env" "read_action_data" (func $read (param i32 i32)(result i32))))";
std::string cf_input_assertions() {
   return "(call $assert (i64.eq (call $receiver) " + cf_name("bob"_n) + R"() (i64.const 20))
      (call $assert (i32.eq (call $size) (i32.const 3)) (i64.const 21))
      (call $assert (i32.eq (call $read (i32.const 0)(i32.const 0)) (i32.const 3)) (i64.const 22))
      (call $assert (i32.eq (call $read (i32.const 0)(i32.const 2)) (i32.const 2)) (i64.const 23))
      (call $assert (i32.eq (i32.load16_u (i32.const 0)) (i32.const 31096)) (i64.const 24)))";
}
// Each call must fall back EVEN when the same API would be harmless or return a
// negative result in an ordinary action. In particular self-notify and size=0.
const std::vector<std::pair<std::string,std::string>>& cf_forbidden() {
   static const std::vector<std::pair<std::string,std::string>> cases{
      {R"((import "env" "has_auth" (func $bad (param i64)(result i32))))", "(drop (call $bad " + cf_name("test"_n) + "))"},
      {R"((import "env" "require_auth" (func $bad (param i64))))", "(call $bad " + cf_name("test"_n) + ")"},
      {R"((import "env" "require_auth2" (func $bad (param i64 i64))))", "(call $bad " + cf_name("test"_n) + cf_name("active"_n) + ")"},
      {R"((import "env" "is_account" (func $bad (param i64)(result i32))))", "(drop (call $bad " + cf_name("test"_n) + "))"},
      {R"((import "env" "get_sender" (func $bad (result i64))))", "(drop (call $bad))"},
      {R"((import "env" "set_action_return_value" (func $bad (param i32 i32))))", "(call $bad (i32.const 0)(i32.const 0))"},
      {"", "(call $notify " + cf_name("bob"_n) + ")"},
      {"", "(call $inl (i32.const 0)(i32.const 0))"},
      {"", "(call $cfi (i32.const 0)(i32.const 0))"},
      {R"((import "env" "db_find_i64" (func $bad (param i64 i64 i64 i64)(result i32))))", "(drop (call $bad (i64.const 0)(i64.const 0)(i64.const 0)(i64.const 0)))"},
      {R"((import "env" "db_get_i64" (func $bad (param i32 i32 i32)(result i32))))", "(drop (call $bad (i32.const -1)(i32.const 0)(i32.const 0)))"},
      {R"((import "env" "db_store_i64" (func $bad (param i64 i64 i64 i64 i32 i32)(result i32))))", "(drop (call $bad (i64.const 0)(i64.const 0)(i64.const 0)(i64.const 0)(i32.const 0)(i32.const 0)))"},
      {R"((import "env" "db_update_i64" (func $bad (param i32 i64 i32 i32))))", "(call $bad (i32.const -1)(i64.const 0)(i32.const 0)(i32.const 0))"},
      {R"((import "env" "db_remove_i64" (func $bad (param i32))))", "(call $bad (i32.const -1))"},
      {R"((import "env" "db_end_i64" (func $bad (param i64 i64 i64)(result i32))))", "(drop (call $bad (i64.const 0)(i64.const 0)(i64.const 0)))"},
      {R"((import "env" "db_lowerbound_i64" (func $bad (param i64 i64 i64 i64)(result i32))))", "(drop (call $bad (i64.const 0)(i64.const 0)(i64.const 0)(i64.const 0)))"},
      {R"((import "env" "db_upperbound_i64" (func $bad (param i64 i64 i64 i64)(result i32))))", "(drop (call $bad (i64.const 0)(i64.const 0)(i64.const 0)(i64.const 0)))"},
      {R"((import "env" "db_next_i64" (func $bad (param i32 i32)(result i32))))", "(drop (call $bad (i32.const -1)(i32.const 8)))"},
      {R"((import "env" "db_previous_i64" (func $bad (param i32 i32)(result i32))))", "(drop (call $bad (i32.const -1)(i32.const 8)))"}
   };
   return cases;
}
transaction_trace_ptr cf_push(eosio::testing::tester& chain, const std::vector<std::vector<char>>& data = {}, bool no_throw = false) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = "run"_n; act.authorization = {{"test"_n,"active"_n}};
   trx.actions.push_back(act); trx.context_free_data = data;
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("test"_n,"active"), chain.control->get_chain_id());
   return chain.push_transaction(trx, fc::time_point::maximum(), eosio::testing::tester::DEFAULT_BILLED_CPU_TIME_US, no_throw);
}
void cf_install(eosio::testing::tester& chain, const std::vector<isolated_receiver_program>& programs) {
   for (const auto& p : programs) {
      const auto expected = p.code.empty() ? fc::sha256{} : fc::sha256::hash(p.code.data(),p.code.size());
      // The original setcode correctly rejects an identical deployment. Do not
      // turn a helper's repeated parent installation into a contract failure.
      if (chain.control->db().get<account_metadata_object,by_name>(p.receiver).code_hash != expected)
         chain.set_code(p.receiver, std::vector<uint8_t>(p.code.begin(), p.code.end()));
   }
   chain.produce_block();
}

struct cf_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time()+fc::microseconds(config::block_interval_us)); }
};
std::string cf_business_checks() {
   return R"(
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 32)(i32.const 0))(i32.const 3))(i64.const 1))
      (call $assert (i32.eq (call $data (i32.const 0)(i32.const 32)(i32.const 3))(i32.const 3))(i64.const 2))
      (call $assert (i32.eq (i32.load16_u (i32.const 32))(i32.const 25185))(i64.const 3))
      (call $assert (i32.eq (i32.load8_u (i32.const 34))(i32.const 99))(i64.const 4)))";
}
struct cf_corpus {
   genesis_state genesis=eosio::testing::base_tester::default_genesis();
   const std::vector<isolated_receiver_program> programs;
   flat_set<digest_type> reviewed;
   std::vector<std::vector<char>> blocks;
   explicit cf_corpus(std::vector<isolated_receiver_program> supplied=cf_graph(cf_business_checks(),true))
      : programs(std::move(supplied)) {
      auto [key,pub,proof]=eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key=pub; genesis.initial_savanna->proof_of_possession=proof;
      for(const auto& p:programs) if(!p.code.empty()) reviewed.insert(fc::sha256::hash(p.code.data(),p.code.size()));
      fc::temp_directory dir; cf_chain source(configuration(dir,0),genesis);
      source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      auto seal=[&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n,"bob"_n,"carol"_n,"dave"_n}); seal();
      for(const auto& p:programs) if(!p.code.empty()) source.set_code(p.receiver,std::vector<uint8_t>(p.code.begin(),p.code.end()));
      seal();
   }
   controller::config configuration(const fc::temp_directory& dir,uint16_t workers,bool production=false) const {
      auto cfg=eosio::testing::base_tester::default_config(dir).first;
      cfg.wasm_runtime=wasm_interface::vm_type::eos_vm; cfg.parallel_validation_threads=workers;
      cfg.parallel_production_enabled=production && workers!=0; cfg.parallel_code_allowlist=reviewed;
      return cfg;
   }
   void prefix(eosio::testing::tester& chain) const {
      for(const auto& block:blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         chain.control->abort_block();
      }
   }
};
signed_transaction cf_business(eosio::testing::tester& chain,uint64_t nonce,size_t roots=1,int data_mode=0) {
   signed_transaction trx;
   for(size_t i=0;i<roots;++i) {
      action act; act.account="test"_n; act.name="run"_n; act.authorization={{"test"_n,"active"_n}};
      act.data=fc::raw::pack(nonce+i); trx.actions.push_back(std::move(act));
   }
   trx.context_free_data={{'a','b','c'}, {}};
   if(data_mode==1) trx.context_free_data[0][0]='z';
   if(data_mode==2) trx.context_free_data.clear();
   if(data_mode==3) trx.context_free_data.push_back(std::vector<char>(isolated_context_free_data::max_bytes,'x'));
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key("test"_n,"active"),chain.control->get_chain_id());
   return trx;
}
transaction_metadata_ptr cf_metadata(eosio::testing::tester& chain,const signed_transaction& trx) {
   return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx,packed_transaction::compression_type::none),
      chain.control->get_chain_id(),fc::seconds(5),transaction_metadata::trx_type::input);
}
transaction_trace_ptr cf_produce_push(eosio::testing::tester& chain,const transaction_metadata_ptr& trx) {
   return chain.control->push_transaction(trx,fc::time_point::maximum(),fc::seconds(1),0,false,0);
}
std::vector<char> cf_trace(const transaction_trace_ptr& trace,bool cpu=true) {
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id),fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->except?trace->except->code():int64_t{0})};
   if(trace->receipt) { auto receipt=*trace->receipt; if(!cpu) receipt.cpu_usage_us=0; fields.push_back(fc::raw::pack(receipt)); }
   for(const auto& a:trace->action_traces) {
      fields.push_back(fc::raw::pack(a.action_ordinal)); fields.push_back(fc::raw::pack(a.creator_action_ordinal));
      fields.push_back(fc::raw::pack(a.closest_unnotified_ancestor_action_ordinal)); fields.push_back(fc::raw::pack(a.context_free));
      fields.push_back(fc::raw::pack(a.receiver)); fields.push_back(fc::raw::pack(a.act)); fields.push_back(fc::raw::pack(a.receipt));
      fields.push_back(a.return_value); fields.push_back(fc::raw::pack(a.account_ram_deltas));
      fields.push_back(fc::raw::pack(a.console));
      fields.push_back(fc::raw::pack(a.error_code)); fields.push_back(fc::raw::pack(a.except?a.except->code():int64_t{0}));
   }
   return fc::raw::pack(fields);
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_context_free_tests)

BOOST_AUTO_TEST_CASE(owned_input_is_bounded_immutable_and_content_addressed) {
   std::vector<std::vector<char>> source{{'a','b','c'}, {}};
   const auto data = isolated_context_free_data::capture(source);
   const auto same = isolated_context_free_data::capture(source);
   BOOST_CHECK(data->fingerprint() == same->fingerprint());
   source[0][0] = 'z';
   BOOST_CHECK(data->fingerprint() != isolated_context_free_data::capture(source)->fingerprint());
   std::array<char,5> bytes{'!','!','!','!','!'};
   BOOST_CHECK_EQUAL(data->read(0,bytes),3); BOOST_CHECK_EQUAL(bytes[0],'a'); BOOST_CHECK_EQUAL(bytes[3],'!');
   BOOST_CHECK_EQUAL(data->read(0,{}),3); BOOST_CHECK_EQUAL(data->read(1,bytes),0);
   BOOST_CHECK_EQUAL(data->read(2,bytes),-1); BOOST_CHECK_EQUAL(data->read(UINT32_MAX,{}),-1);
   BOOST_CHECK_EQUAL(data->read(0,std::span(bytes).first(2)),2);
   BOOST_CHECK_THROW(isolated_context_free_data::capture(std::vector<std::vector<char>>(1025)),primary_state_budget_exceeded);
   const std::vector<std::vector<char>> limit{std::vector<char>(isolated_context_free_data::max_bytes)};
   BOOST_CHECK_NO_THROW(isolated_context_free_data::capture(limit));
   auto excess = limit; excess.push_back({'x'});
   BOOST_CHECK_THROW(isolated_context_free_data::capture(excess),primary_state_budget_exceeded);
   const std::array roots{fc::sha256::hash("root")};
   BOOST_CHECK(isolated_action_tree_fingerprint(roots,{}) != isolated_action_tree_fingerprint(roots,cf_context()));
}

BOOST_FIXTURE_TEST_CASE(two_child_queues_preserve_send_ordinals_and_execution_order, cf_fixture) {
   const std::array roots{cf_root()}; const auto programs = cf_graph(); const auto context = cf_context();
   isolated_action_tree_executor executor;
   const auto result = executor.execute(base,roots,programs,context);
   BOOST_REQUIRE(validate_isolated_action_tree_journal(result,roots,programs,context));
   const std::array ordinals{1u,4u,3u,6u,7u,2u,8u,9u,5u};
   BOOST_REQUIRE_EQUAL(result.steps.size(),ordinals.size()); BOOST_CHECK_EQUAL(result.groups.size(),8u);
   for (size_t i=0;i<ordinals.size();++i) BOOST_CHECK_EQUAL(result.steps[i].action_ordinal,ordinals[i]);
   BOOST_CHECK(result.state.writes().empty()); BOOST_CHECK(result.state.read_accounts().empty());
   BOOST_CHECK(result.state.read_queries().empty()); BOOST_CHECK(result.state.read_tables().empty());
   BOOST_CHECK(!validate_isolated_action_tree_journal(result,roots,programs,{}));
   BOOST_CHECK_THROW(executor.execute(base,roots,programs,{}),primary_state_unsupported);
}

BOOST_FIXTURE_TEST_CASE(data_host_limits_and_context_aware_hosts_are_enforced, cf_fixture) {
   const std::array roots{cf_root()}; const auto context = cf_context({{'a','b','c'}, {}});
   isolated_action_tree_executor executor;
   const auto programs = cf_pair(cf_program(cf_data_assertions()));
   const auto result = executor.execute(base,roots,programs,context);
   BOOST_REQUIRE(validate_isolated_action_tree_journal(result,roots,programs,context));
   BOOST_CHECK(validate_isolated_action_tree_journal(result,roots,programs,cf_context({{'a','b','c'}, {}})));
   BOOST_CHECK(!validate_isolated_action_tree_journal(result,roots,programs,cf_context({{'z','b','c'}, {}})));
   for (const auto& [imports,body] : cf_forbidden()) {
      BOOST_TEST_CONTEXT("forbidden " << imports << body) {
         BOOST_CHECK_THROW(executor.execute(base,roots,cf_pair(cf_program(body,{},imports)),context),primary_state_unsupported);
      }
   }
   BOOST_CHECK_THROW(executor.execute(base,roots,cf_pair(cf_program(),true),context),primary_state_unsupported);
   BOOST_CHECK_THROW(executor.execute(base,roots,cf_pair(cf_program("(drop (call $data (i32.const 99)(i32.const 65536)(i32.const 1)))")),context),primary_state_unsupported);
   auto bad_root = programs; bad_root[0].code = cf_program(cf_data_assertions());
   BOOST_CHECK_THROW(executor.execute(base,roots,bad_root,context),primary_state_unsupported);
}

BOOST_FIXTURE_TEST_CASE(forged_flags_queues_and_context_free_effects_fail_preflight, cf_fixture) {
   const std::array roots{cf_root()}; const auto context = cf_context(); const auto programs = cf_graph();
   isolated_action_tree_executor executor; const auto good = executor.execute(base,roots,programs,context);
   for (unsigned test=0;test<10;++test) {
      auto bad=good;
      if(test==0) bad.groups[0].context_free=true;
      if(test==1) bad.groups[2].context_free=false;
      if(test==2) std::get<isolated_tree_inline_event>(bad.steps[0].events[1]).context_free=false;
      if(test==3) std::swap(bad.steps[2],bad.steps[5]);
      if(test==4) bad.steps[2].effects.return_value={'x'};
      if(test==5) bad.steps[2].effects.recipients.push_back({"test"_n,0});
      if(test==6) bad.steps[2].events.emplace_back(isolated_cursor_event{isolated_cursor_query{}});
      if(test==7) bad.groups[2].input.authorizations.push_back({"test"_n,"active"_n});
      if(test==8) bad.context_free_data.reset();
      if(test==9) bad.context_free_data=cf_context({{'z'}}).context_free_data;
      BOOST_TEST_CONTEXT("mutation " << test) { BOOST_CHECK(!validate_isolated_action_tree_journal(bad,roots,programs,context)); }
   }
}

BOOST_FIXTURE_TEST_CASE(fixed_input_is_allowed_but_start_functions_keep_context_restrictions, cf_fixture) {
   const std::array roots{cf_root()}; const auto context=cf_context(); isolated_action_tree_executor executor;
   const auto programs=cf_pair(cf_program(cf_input_assertions(),{},cf_input_imports));
   const auto result=executor.execute(base,roots,programs,context);
   BOOST_CHECK(validate_isolated_action_tree_journal(result,roots,programs,context));
   const auto bad=cf_pair(cf_program("",{},R"((import "env" "get_sender" (func $bad (result i64))))","(drop (call $bad))"));
   BOOST_CHECK_THROW(executor.execute(base,roots,bad,context),primary_state_unsupported);
}

BOOST_FIXTURE_TEST_CASE(same_transaction_id_with_different_context_free_data_cannot_reuse_result, cf_fixture) {
   signed_transaction one;
   action act; act.account="test"_n; act.name="run"_n; act.authorization={{"test"_n,"active"_n}};
   one.actions.push_back(act); one.context_free_data={{'a'}};
   signed_transaction two; two.actions=one.actions; two.context_free_data={{'b'}};
   BOOST_REQUIRE(one.id()==two.id());
   const std::array roots{cf_root()}; const auto programs=cf_pair(cf_program()); isolated_action_tree_executor executor;
   const auto right=executor.execute(base,roots,programs,cf_context(one.context_free_data));
   const auto other=executor.execute(base,roots,programs,cf_context(two.context_free_data));
   BOOST_REQUIRE(right.root_fingerprint!=other.root_fingerprint);
   const std::array<ordered_attempt_batch::request,1> requests{{{one.id(),right.root_fingerprint}}};
   ordered_attempt_batch batch(base,requests); unsigned adopted=0,serial=0;
   const std::optional<isolated_action_tree_result> wrong{other};
   BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(0),one.id(),wrong,
      [&](const auto&) { ++adopted; return true; },[&] { ++serial; return false; }),primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(adopted,0u); BOOST_CHECK_EQUAL(serial,0u);
}

BOOST_FIXTURE_TEST_CASE(legacy_entries_and_no_code_children_keep_explicit_capability_boundaries, cf_fixture) {
   const std::array roots{cf_root()}; const auto context=cf_context(); const auto programs=cf_pair({});
   isolated_action_tree_executor tree;
   const auto result=tree.execute(base,roots,programs,context);
   BOOST_REQUIRE_EQUAL(result.steps.size(),2u); BOOST_CHECK(!result.steps[1].has_code);
   BOOST_CHECK(result.groups[1].context_free); BOOST_CHECK_EQUAL(result.steps[1].effects.elapsed.count(),0);
   BOOST_CHECK(validate_isolated_action_tree_journal(result,roots,programs,context));
   isolated_wasm_module module(programs[0].code); isolated_transaction_executor transaction;
   isolated_notification_executor notification;
   const std::array calls{isolated_action_invocation{programs[0].code,roots[0],{}}};
   BOOST_CHECK_THROW(module.execute(base,roots[0]),primary_state_unsupported);
   BOOST_CHECK_THROW(transaction.execute(base,calls),primary_state_unsupported);
   BOOST_CHECK_THROW(notification.execute(base,roots,programs),primary_state_unsupported);
}

BOOST_FIXTURE_TEST_CASE(shared_immutable_data_survives_pool_concurrency_and_late_failure, cf_fixture) {
   const std::vector roots{cf_root()}; const auto context=cf_context({{'a','b','c'}, {}});
   const auto programs=cf_pair(cf_program(cf_data_assertions()));
   isolated_action_tree_executor executor; const auto reference=executor.execute(base,roots,programs,context);
   for (size_t threads : {1u,2u,4u}) {
      isolated_worker_pool pool(threads);
      for (unsigned round=0;round<3;++round) {
         std::vector<ordered_attempt_batch::request> requests;
         for(unsigned i=0;i<8;++i) requests.push_back({fc::sha256::hash(std::to_string(i)),reference.root_fingerprint});
         ordered_attempt_batch batch(base,requests); std::vector<isolated_worker_job> jobs;
         for(unsigned i=0;i<8;++i) {
            auto catalog=programs;
            if(i%2) catalog[1].code=cf_program("unreachable");
            jobs.emplace_back(batch.get_ticket(i),roots,catalog,context);
         }
         const auto results=pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(results.size(),8u);
         for(unsigned i=0;i<8;++i) {
            BOOST_CHECK_EQUAL(bool(results[i].tree_result),i%2==0);
            if(i%2==0) BOOST_CHECK(validate_isolated_action_tree_journal(*results[i].tree_result,roots,programs,context));
            else { BOOST_CHECK(!results[i].request); BOOST_CHECK(!results[i].receiver_request); BOOST_CHECK_EQUAL(results[i].elapsed.count(),0); }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(ordinary_adjacent_null_output_rejects_the_whole_private_write_prefix, cf_fixture) {
   const std::array tables{table_key{"test"_n,"fresh"_n,"rows"_n}};
   const auto full=frozen_primary_state::capture(view,tables);
   const std::array roots{cf_root()};
   for(const std::string api : {"db_next_i64","db_previous_i64"}) {
      for(uint32_t output : {0u,8u}) {
         BOOST_TEST_CONTEXT(api << " output=" << output) {
            const auto code=cf_adjacent_program(api,output);
            isolated_wasm_module module(code); isolated_transaction_executor transaction;
            isolated_notification_executor notification; isolated_action_tree_executor tree;
            const std::array calls{isolated_action_invocation{code,roots[0],{}}};
            const std::array programs{isolated_receiver_program{"test"_n,code,{}}};
            if(!output) {
               BOOST_CHECK_THROW(module.execute(full,roots[0]),primary_state_unsupported);
               BOOST_CHECK_THROW(transaction.execute(full,calls),primary_state_unsupported);
               BOOST_CHECK_THROW(notification.execute(full,roots,programs),primary_state_unsupported);
               BOOST_CHECK_THROW(tree.execute(full,roots,programs,cf_context()),primary_state_unsupported);
            } else {
               BOOST_CHECK_EQUAL(module.execute(full,roots[0]).state.writes().size(),1u);
               BOOST_CHECK_EQUAL(transaction.execute(full,calls).state.writes().size(),1u);
               BOOST_CHECK_EQUAL(notification.execute(full,roots,programs).state.writes().size(),1u);
               BOOST_CHECK_EQUAL(tree.execute(full,roots,programs,cf_context()).state.writes().size(),1u);
            }
            BOOST_CHECK(!view.read_table(tables[0]));
         }
      }
   }
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_context_free_adoption_tests)

BOOST_FIXTURE_TEST_CASE(production_and_validation_adopt_both_queues_and_match_original_state,cf_corpus) {
   for(uint16_t workers:{0,1,2,4}) {
      BOOST_TEST_CONTEXT("CF adoption workers=" << workers) {
         fc::temp_directory pd,sd,vd;
         cf_chain producer(configuration(pd,workers,true),genesis);
         eosio::testing::tester serial(configuration(sd,0),genesis),validator(configuration(vd,workers),genesis);
         prefix(producer); prefix(serial); prefix(validator);
         producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start();
         for(size_t roots:{1u,2u}) {
            std::vector<transaction_metadata_ptr> batch;
            for(uint64_t i=0;i<4;++i) batch.push_back(cf_metadata(producer,cf_business(producer,1000*roots+10*i,roots)));
            std::vector<std::vector<char>> expected,serial_traces,parallel_traces;
            auto collect=[&](auto& out,const auto& event) {
               if(std::any_of(batch.begin(),batch.end(),[&](const auto& tx) { return tx->id()==std::get<0>(event)->id; }))
                  out.push_back(cf_trace(std::get<0>(event)));
            };
            boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& e) { collect(serial_traces,e); });
            boost::signals2::scoped_connection b=validator.control->applied_transaction().connect([&](const auto& e) { collect(parallel_traces,e); });
            const auto before=producer.control->get_parallel_production_stats(); const auto vb=validator.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(parallel_test_measure("CF production", [&] {
               return producer.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),[&](const auto& tx) {
               const auto trace=cf_produce_push(producer,tx);
               BOOST_REQUIRE_MESSAGE(!trace->except,(trace->except?trace->except->to_detail_string():""));
               BOOST_REQUIRE(trace->receipt); BOOST_CHECK_EQUAL(trace->action_traces.size(),9*roots);
               BOOST_CHECK_EQUAL(std::count_if(trace->action_traces.begin(),trace->action_traces.end(),[](const auto& t) { return t.context_free; }),static_cast<std::ptrdiff_t>(4*roots));
               BOOST_CHECK_GE(trace->receipt->cpu_usage_us,producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
               expected.push_back(cf_trace(trace)); return true;
            }); }),batch.size());
            const auto after=producer.control->get_parallel_production_stats();
            BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before, after)) {
               BOOST_CHECK_EQUAL(after.adopted-before.adopted,workers?4u:0u);
               BOOST_CHECK_EQUAL(after.serial_fallbacks-before.serial_fallbacks,0u);
            }
            const auto block=producer.finish_block(); serial.push_block(block); serial.control->abort_block();
            validator.push_block(block); validator.control->abort_block();
            BOOST_CHECK(expected==serial_traces); BOOST_CHECK(expected==parallel_traces);
            BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
            BOOST_CHECK(serial.control->calculate_integrity_hash()==validator.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(validator.control->get_parallel_execution_stats().adopted-vb.adopted,workers?4u:0u);
            for(uint64_t i=0;i<4;++i) for(size_t j=0;j<roots;++j)
               BOOST_CHECK(producer.get_row_by_account("test"_n,name{1000*roots+10*i+j},"rows"_n,name{1})==fc::raw::pack(1000*roots+10*i+j));
            producer.start();
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(failed_cf_child_undoes_parent_and_next_batch_does_not_reuse_old_capability,cf_corpus) {
   for(uint16_t workers:{0,1,2,4}) for(int failure:{1,2}) {
      BOOST_TEST_CONTEXT("CF rollback workers=" << workers << " failure=" << failure) {
         fc::temp_directory pd,sd; cf_chain producer(configuration(pd,workers,true),genesis),serial(configuration(sd,0),genesis);
         prefix(producer); prefix(serial); producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start(); serial.start();
         const auto bad=cf_business(producer,5000,1,failure); const auto good=cf_business(producer,5001);
         const auto original_before=serial.control->calculate_integrity_hash();
         const auto original=cf_produce_push(serial,cf_metadata(serial,bad)); BOOST_REQUIRE(original->except);
         BOOST_CHECK(serial.control->calculate_integrity_hash()==original_before);
         const auto before=producer.control->calculate_integrity_hash(); const auto stats=producer.control->get_parallel_production_stats();
         const std::vector<transaction_metadata_ptr> batch{cf_metadata(producer,bad),cf_metadata(producer,good)};
         size_t called=0;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),[&](const auto& tx) {
            const auto trace=cf_produce_push(producer,tx);
            if(called++==0) {
               BOOST_REQUIRE(trace->except); BOOST_CHECK(cf_trace(trace,false)==cf_trace(original,false));
               BOOST_CHECK(producer.control->calculate_integrity_hash()==before);
               BOOST_CHECK(producer.get_row_by_account("test"_n,name{5000},"rows"_n,name{1}).empty());
            } else BOOST_REQUIRE(!trace->except);
            return true;
         }),2u);
         const auto after=producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(after.adopted-stats.adopted,0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks-stats.serial_fallbacks,workers?2u:0u);
         const auto fresh=cf_metadata(producer,cf_business(producer,5002));
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({fresh},fc::time_point::maximum(),[&](const auto& tx) {
            BOOST_REQUIRE(!cf_produce_push(producer,tx)->except); return true;
         }),1u);
         BOOST_CHECK_EQUAL(producer.control->get_parallel_production_stats().adopted-after.adopted,workers?1u:0u);
      }
   }
}

BOOST_AUTO_TEST_CASE(empty_code_unreviewed_code_budget_and_top_level_cf_preserve_original_acceptance) {
   // Capture limits and local review are NEVER new transaction validity rules.
   for(unsigned mode=0;mode<4;++mode) for(uint16_t workers:{0,1,2,4}) {
      BOOST_TEST_CONTEXT("CF boundary mode=" << mode << " workers=" << workers) {
         auto code=cf_graph(cf_business_checks(),true);
         if(mode==0) code[2].code.clear(); // both pure and ordinary carol receivers have no code
         cf_corpus fixture(std::move(code));
         fc::temp_directory pd,sd,vd;
         auto pc=fixture.configuration(pd,workers,true),vc=fixture.configuration(vd,workers);
         if(mode==1) {
            const auto& bytes=fixture.programs[2].code;
            const auto hash=fc::sha256::hash(bytes.data(),bytes.size());
            pc.parallel_code_allowlist.erase(hash); vc.parallel_code_allowlist.erase(hash);
         }
         cf_chain producer(pc,fixture.genesis);
         eosio::testing::tester serial(fixture.configuration(sd,0),fixture.genesis),validator(vc,fixture.genesis);
         fixture.prefix(producer); fixture.prefix(serial); fixture.prefix(validator);
         producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start();
         auto trx=cf_business(producer,6000,1,mode==2?3:0);
         if(mode==3) {
            action pure; pure.account="bob"_n; pure.name="pure"_n; pure.data={'x','y','z'};
            trx.context_free_actions.push_back(std::move(pure));
            trx.signatures.clear(); trx.sign(producer.get_private_key("test"_n,"active"),producer.control->get_chain_id());
         }
         const auto tx=cf_metadata(producer,trx); std::vector<char> expected,original,observed;
         boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& e) {
            if(std::get<0>(e)->id==tx->id()) original=cf_trace(std::get<0>(e));
         });
         boost::signals2::scoped_connection b=validator.control->applied_transaction().connect([&](const auto& e) {
            if(std::get<0>(e)->id==tx->id()) observed=cf_trace(std::get<0>(e));
         });
         const auto before=producer.control->get_parallel_production_stats();
         const auto vb=validator.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({tx},fc::time_point::maximum(),[&](const auto& tx) {
            const auto trace=cf_produce_push(producer,tx);
            BOOST_REQUIRE_MESSAGE(!trace->except,(trace->except?trace->except->to_detail_string():""));
            BOOST_CHECK_EQUAL(trace->action_traces.size(),mode==3?10u:9u);
            BOOST_CHECK_EQUAL(std::count_if(trace->action_traces.begin(),trace->action_traces.end(),[](const auto& t) { return t.context_free; }),mode==3?5u:4u);
            expected=cf_trace(trace); return true;
         }),1u);
         const auto after=producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(after.adopted-before.adopted,workers && mode==0?1u:0u);
         if(mode>=2) BOOST_CHECK_EQUAL(after.attempts-before.attempts,0u);
         const auto block=producer.finish_block(); serial.push_block(block); serial.control->abort_block();
         validator.push_block(block); validator.control->abort_block();
         BOOST_CHECK(expected==original); BOOST_CHECK(expected==observed);
         BOOST_CHECK(producer.control->calculate_integrity_hash()==serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash()==validator.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(validator.control->get_parallel_execution_stats().adopted-vb.adopted,workers && mode==0?1u:0u);
         BOOST_CHECK(producer.get_row_by_account("test"_n,name{6000},"rows"_n,name{1})==fc::raw::pack(uint64_t{6000}));
      }
   }
}

BOOST_FIXTURE_TEST_CASE(light_validation_never_consumes_a_partial_context_free_tree,cf_corpus) {
   fc::temp_directory pd,sd,ld;
   cf_chain producer(configuration(pd,0),genesis);
   auto light=configuration(ld,4); light.block_validation_mode=validation_mode::LIGHT;
   eosio::testing::tester serial(configuration(sd,0),genesis),validator(light,genesis);
   prefix(producer); prefix(serial); prefix(validator);
   producer.set_node_finalizers(std::array<name,1>{"eosio"_n}); producer.start();
   const auto tx=cf_business(producer,6100);
   BOOST_REQUIRE(!cf_produce_push(producer,cf_metadata(producer,tx))->except);
   std::vector<char> expected,observed;
   boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& e) {
      if(std::get<0>(e)->id==tx.id()) expected=cf_trace(std::get<0>(e));
   });
   boost::signals2::scoped_connection b=validator.control->applied_transaction().connect([&](const auto& e) {
      if(std::get<0>(e)->id==tx.id()) observed=cf_trace(std::get<0>(e));
   });
   const auto block=producer.finish_block(); serial.push_block(block); serial.control->abort_block();
   validator.push_block(block); validator.control->abort_block();
   BOOST_REQUIRE(!expected.empty()); BOOST_CHECK(expected==observed);
   BOOST_CHECK(serial.control->calculate_integrity_hash()==validator.control->calculate_integrity_hash());
   BOOST_CHECK_EQUAL(validator.control->get_parallel_execution_stats().attempts,0u);
}

BOOST_AUTO_TEST_CASE(native_cf_receiver_falls_back_and_preserves_original_failure_and_parent_undo) {
   action native; native.account="eosio"_n; native.name="linkauth"_n;
   native.data=fc::raw::pack(linkauth{"test"_n,"bob"_n,"normal"_n,"active"_n});
   auto code=cf_graph(cf_business_checks(),true);
   const std::string imports=R"(
      (import "env" "read_action_data" (func $read (param i32 i32)(result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32)(result i32))))";
   code[0].code=cf_program("(drop (call $read (i32.const 0)(i32.const 8))) (drop (call $store (i64.load (i32.const 0)) " +
      cf_name("rows"_n)+cf_name("test"_n)+"(i64.const 1)(i32.const 0)(i32.const 8))) (call $send0)",{{true,fc::raw::pack(native)}},imports);
   cf_corpus fixture(std::move(code));
   for(uint16_t workers:{0,1,2,4}) {
      BOOST_TEST_CONTEXT("native CF barrier workers=" << workers) {
         fc::temp_directory pd,sd;
         cf_chain producer(fixture.configuration(pd,workers,true),fixture.genesis),serial(fixture.configuration(sd,0),fixture.genesis);
         fixture.prefix(producer); fixture.prefix(serial); producer.start(); serial.start();
         const auto trx=cf_business(producer,6200);
         const auto sb=serial.control->calculate_integrity_hash(),pb=producer.control->calculate_integrity_hash();
         const auto original=cf_produce_push(serial,cf_metadata(serial,trx)); BOOST_REQUIRE(original->except);
         BOOST_CHECK_EQUAL(original->except->code(),missing_auth_exception::code_value);
         const auto before=producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({cf_metadata(producer,trx)},fc::time_point::maximum(),[&](const auto& tx) {
            const auto trace=cf_produce_push(producer,tx); BOOST_REQUIRE(trace->except);
            BOOST_CHECK(cf_trace(trace,false)==cf_trace(original,false)); return true;
         }),1u);
         BOOST_CHECK_EQUAL(producer.control->get_parallel_production_stats().adopted-before.adopted,0u);
         BOOST_CHECK(serial.control->calculate_integrity_hash()==sb); BOOST_CHECK(producer.control->calculate_integrity_hash()==pb);
         BOOST_CHECK(producer.get_row_by_account("test"_n,name{6200},"rows"_n,name{1}).empty());
      }
   }
}

BOOST_FIXTURE_TEST_CASE(same_transaction_id_cannot_substitute_context_free_data_at_canonical_adoption,cf_corpus) {
   fc::temp_directory dir; cf_chain chain(configuration(dir,0),genesis); prefix(chain); chain.start();
   const auto good=cf_business(chain,6300),bad=cf_business(chain,6300,1,1);
   BOOST_REQUIRE(good.id()==bad.id());
   const packed_transaction original(good,packed_transaction::compression_type::none);
   const auto prepared=prepare_isolated_transaction(*chain.control,original); BOOST_REQUIRE(prepared);
   const std::vector roots{prepared->front().input}; std::vector<isolated_receiver_program> captured;
   for(const auto& p:programs) {
      const auto code=prepare_isolated_tree_program(*chain.control,p.receiver); BOOST_REQUIRE(code); captured.push_back(*code);
   }
   auto context=current_action_tree_context(*chain.control);
   context.context_free_data=isolated_context_free_data::capture(good.context_free_data);
   const std::array<table_key,1> tables{{{"test"_n,name{6300},"rows"_n}}};
   chainbase_transaction_state_view view(chain.control->db()); isolated_action_tree_executor executor;
   const auto result=executor.execute(frozen_primary_state::capture(view,tables),roots,captured,context);
   const auto initial=chain.control->calculate_integrity_hash();
   for(bool different:{true,false}) {
      {
         const packed_transaction actual(different?bad:good,packed_transaction::compression_type::none);
         platform_timer timer; const auto start=fc::time_point::now();
         transaction_context transaction(*chain.control,actual,actual.id(),transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna,start,transaction_metadata::trx_type::input);
         transaction.block_deadline=start+fc::seconds(5); transaction.explicit_billed_cpu_time=true; transaction.billed_cpu_time_us=100;
         transaction.init_for_input_trx(actual.get_unprunable_size(),actual.get_prunable_size());
         transaction.trace->action_traces.emplace_back(*transaction.trace,actual.get_transaction().actions.front(),"test"_n,false,1,0,0);
         apply_context apply(*chain.control,transaction,1);
         const auto before=chain.control->calculate_integrity_hash();
         BOOST_CHECK_EQUAL(try_adopt_action_tree_result(apply,result.steps[0].code_hash,0,0,captured[0].limits,result,0),!different);
         if(different) {
            BOOST_CHECK(chain.control->calculate_integrity_hash()==before);
            BOOST_CHECK_EQUAL(transaction.trace->action_traces.size(),1u);
         } else {
            BOOST_CHECK_EQUAL(transaction.trace->action_traces.size(),6u);
            BOOST_CHECK(chain.get_row_by_account("test"_n,name{6300},"rows"_n,name{1})==fc::raw::pack(uint64_t{6300}));
         }
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash()==initial);
   }
}

BOOST_AUTO_TEST_CASE(export_fresh_context_free_producer_fixture) {
   using namespace eosio::testing;
   const char* destination=std::getenv("PUREEOS_PARALLEL_PRODUCER_CF_EXPORT_DIR");
   if(!destination) { BOOST_TEST_MESSAGE("CF process fixture export not requested"); return; }
   const std::filesystem::path directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
   auto genesis=base_tester::default_genesis();
   genesis.initial_timestamp=block_timestamp_type(fc::time_point::now()-fc::seconds(5)).to_time_point();
   auto [key,pub,proof]=get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key=pub; genesis.initial_savanna->proof_of_possession=proof;
   BOOST_REQUIRE(fc::json::from_string(fc::json::to_string(genesis,fc::time_point::maximum())).as<genesis_state>().compute_chain_id()==genesis.compute_chain_id());
   fc::temp_directory dir; auto cfg=base_tester::default_config(dir).first; cfg.wasm_runtime=wasm_interface::vm_type::eos_vm;
   cf_chain source(cfg,genesis); source.set_node_finalizers(std::array<name,1>{"eosio"_n});
   const auto code=cf_graph(cf_business_checks(),true); fc::variants checkpoints;
   auto seal=[&] {
      const auto block=source.produce_block(); source.control->abort_block();
      const auto file="block-"+std::to_string(block->block_num())+".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block,directory/file));
      checkpoints.emplace_back(fc::mutable_variant_object()("block_num",block->block_num())("block_id",source.head().id())
         ("lib",source.last_irreversible_block_num())("integrity_hash",source.control->calculate_integrity_hash())("file",file));
   };
   source.create_accounts({"test"_n,"bob"_n,"carol"_n,"dave"_n}); seal(); source.start();
   for(const auto& p:code) source.set_code(p.receiver,std::vector<uint8_t>(p.code.begin(),p.code.end()));
   seal(); source.start();
   unsigned count=16;
   if(const char* value=std::getenv("PUREEOS_PARALLEL_PRODUCER_TRANSACTION_COUNT")) {
      char* end=nullptr; const auto parsed=std::strtoul(value,&end,10);
      BOOST_REQUIRE(end && *end=='\0' && parsed>=16 && parsed<=4096); count=parsed;
   }
   fc::variants transactions;
   for(unsigned i=0;i<count;++i) {
      auto trx=cf_business(source,10000+i*2,2);
      trx.expiration=fc::time_point_sec(fc::time_point::now()+fc::seconds(1800));
      trx.signatures.clear(); trx.sign(source.get_private_key("test"_n,"active"),source.control->get_chain_id());
      const packed_transaction packed(trx,packed_transaction::compression_type::none);
      transactions.emplace_back(fc::mutable_variant_object()("id",packed.id())("packed",packed)("actions",trx.actions.size()));
   }
   BOOST_REQUIRE(fc::json::save_to_file(genesis,directory/"genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions,directory/"transactions.json"));
   const std::vector<std::string> providers{
      base_tester::get_public_key("eosio"_n,"active").to_string({})+"=KEY:"+base_tester::get_private_key("eosio"_n,"active").to_string({}),
      pub.to_string()+"=KEY:"+key.to_string()}; // disposable deterministic TEST keys only
   BOOST_REQUIRE(fc::json::save_to_file(providers,directory/"TEST-ONLY-keys.json"));
   std::vector<fc::sha256> hashes;
   for(const auto& p:code) hashes.push_back(fc::sha256::hash(p.code.data(),p.code.size()));
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format",1)("fixture_kind","context-free-inline-multiple-actions")
      ("chain_id",source.control->get_chain_id())("code_hashes",hashes)("checkpoints",checkpoints),directory/"manifest.json"));
}

BOOST_AUTO_TEST_CASE(export_checked_context_free_recovery_corpus) {
   using namespace eosio::testing;
   const char* destination=std::getenv("PUREEOS_PARALLEL_CF_RECOVERY_EXPORT_DIR");
   if(!destination) { BOOST_TEST_MESSAGE("CF recovery corpus export not requested"); return; }
   const std::filesystem::path directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
   cf_corpus fixture; BOOST_REQUIRE(fc::json::save_to_file(fixture.genesis,directory/"genesis.json"));
   std::filesystem::create_directory(directory/"blocks");
   fc::variants checkpoints,forks; std::vector<std::vector<char>> corpus;
   fc::temp_directory dir; const auto cfg=fixture.configuration(dir,0);
   {
      cf_chain source(cfg,fixture.genesis); source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      auto record=[&](const signed_block_ptr& block) {
         source.control->abort_block(); const auto file="block-"+std::to_string(block->block_num())+".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block,directory/file)); corpus.push_back(fc::raw::pack(*block));
         checkpoints.emplace_back(fc::mutable_variant_object()("block_num",block->block_num())("block_id",source.head().id())
            ("lib",source.last_irreversible_block_num())("integrity_hash",source.control->calculate_integrity_hash())("file",file));
      };
      for(const auto& bytes:fixture.blocks) {
         auto block=std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)); source.push_block(block); record(block);
      }
      for(unsigned i=0;i<16;++i) {
         source.start();
         for(unsigned j=0;j<4;++j) {
            auto trx=cf_business(source,20000+100*i+10*j,2);
            if(j==3) { // explicitly serial top-level CF, with the same mixed inline tree afterwards
               action pure; pure.account="bob"_n; pure.name="pure"_n; pure.data={'x','y','z'};
               trx.context_free_actions.push_back(std::move(pure)); trx.signatures.clear();
               trx.sign(source.get_private_key("test"_n,"active"),source.control->get_chain_id());
            }
            BOOST_REQUIRE(!source.push_transaction(trx)->except);
         }
         record(source.produce_block());
      }
   }
   for(const char* file:{"blocks.log","blocks.index"})
      BOOST_REQUIRE(std::filesystem::copy_file(cfg.blocks_dir/file,directory/"blocks"/file));
   for(uint16_t workers:{0,1,2,4}) {
      fc::temp_directory sd,pd;
      tester serial(fixture.configuration(sd,0),fixture.genesis),parallel(fixture.configuration(pd,workers),fixture.genesis);
      std::vector<std::vector<char>> expected,observed;
      boost::signals2::scoped_connection a=serial.control->applied_transaction().connect([&](const auto& e) { expected.push_back(cf_trace(std::get<0>(e))); });
      boost::signals2::scoped_connection b=parallel.control->applied_transaction().connect([&](const auto& e) { observed.push_back(cf_trace(std::get<0>(e))); });
      for(const auto& bytes:corpus) {
         expected.clear(); observed.clear();
         serial.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
         parallel.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
         serial.control->abort_block(); parallel.control->abort_block();
         BOOST_CHECK(expected==observed); BOOST_CHECK(serial.head().id()==parallel.head().id());
         BOOST_CHECK_EQUAL(serial.last_irreversible_block_num(),parallel.last_irreversible_block_num());
         BOOST_CHECK(serial.control->calculate_integrity_hash()==parallel.control->calculate_integrity_hash());
      }
      if(workers) BOOST_CHECK_GT(parallel.control->get_parallel_execution_stats().adopted,0u);
   }
   constexpr size_t prefix_blocks=7;
   fc::temp_directory fd,sd,rd;
   cf_chain first(fixture.configuration(fd,0),fixture.genesis),second(fixture.configuration(sd,0),fixture.genesis),
      reference(fixture.configuration(rd,0),fixture.genesis);
   for(auto* chain:{&first,&second,&reference}) for(size_t i=0;i<prefix_blocks;++i) {
      chain->push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[i]))); chain->control->abort_block();
   }
   first.set_node_finalizers(std::array<name,1>{"eosio"_n}); second.set_node_finalizers(std::array<name,1>{"eosio"_n});
   auto fork=[&](const signed_block_ptr& block) {
      reference.push_block(block); reference.control->abort_block(); BOOST_REQUIRE(reference.head().id()==block->calculate_id());
      const auto file="fork-"+std::to_string(forks.size())+".json"; BOOST_REQUIRE(fc::json::save_to_file(*block,directory/file));
      forks.emplace_back(fc::mutable_variant_object()("block_num",reference.head().block_num())("block_id",reference.head().id())
         ("lib",reference.last_irreversible_block_num())("integrity_hash",reference.control->calculate_integrity_hash())("file",file));
   };
   auto push=[&](cf_chain& chain,uint64_t nonce) {
      auto trx=cf_business(chain,nonce,2); BOOST_REQUIRE(!chain.push_transaction(trx)->except);
   };
   push(first,30000); fork(first.produce_block());
   push(second,30100); fork(second.produce_block(fc::milliseconds(1000)));
   push(second,30200); fork(second.produce_block());
   std::vector<fc::sha256> hashes;
   for(const auto& p:fixture.programs) hashes.push_back(fc::sha256::hash(p.code.data(),p.code.size()));
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format",1)("fixture_kind","context-free-inline-recovery")
      ("chain_id",fixture.genesis.compute_chain_id())("code_hashes",hashes)("checkpoints",checkpoints)
      ("fork_prefix_blocks",prefix_blocks)("forks",forks),directory/"manifest.json"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_context_free_oracle_tests)
BOOST_AUTO_TEST_CASE(private_queue_order_matches_original_context_flags_and_receipts) {
   eosio::testing::tester chain; chain.create_accounts({"test"_n,"bob"_n,"carol"_n,"dave"_n});
   const auto programs=cf_graph(); cf_install(chain,programs);
   chainbase_transaction_state_view view(chain.control->db());
   const auto base=frozen_primary_state::capture_queries(view,{},{});
   const std::array roots{cf_root()}; const auto context=cf_context();
   isolated_action_tree_executor executor; const auto result=executor.execute(base,roots,programs,context);
   const auto trace=cf_push(chain); BOOST_REQUIRE(trace && !trace->except);
   BOOST_REQUIRE_EQUAL(trace->action_traces.size(),result.steps.size());
   uint64_t sequence=0;
   for(const auto& step:result.steps) {
      const auto& actual=trace->action_traces.at(step.action_ordinal-1); const auto& group=result.groups.at(step.group_index);
      BOOST_CHECK_EQUAL(actual.context_free,group.context_free); BOOST_CHECK(actual.receiver==step.receiver);
      BOOST_CHECK_EQUAL(actual.creator_action_ordinal.value,step.creator_action_ordinal);
      BOOST_CHECK_EQUAL(actual.closest_unnotified_ancestor_action_ordinal.value,step.closest_unnotified_ancestor);
      BOOST_CHECK(actual.act.account==group.input.account); BOOST_CHECK(actual.act.name==group.input.action);
      BOOST_CHECK(actual.act.data==group.input.data); BOOST_CHECK(actual.return_value.empty());
      BOOST_REQUIRE(actual.receipt); BOOST_CHECK_GT(actual.receipt->global_sequence,sequence); sequence=actual.receipt->global_sequence;
   }
}
BOOST_AUTO_TEST_CASE(original_data_host_and_forbidden_apis_match_private_decisions) {
   eosio::testing::tester chain; chain.create_accounts({"test"_n,"bob"_n});
   const auto programs=cf_pair(cf_program(cf_data_assertions())); cf_install(chain,programs);
   BOOST_REQUIRE(!cf_push(chain,{{'a','b','c'}, {}})->except); chain.produce_block();
   cf_install(chain,cf_pair(cf_program(cf_input_assertions(),{},cf_input_imports)));
   BOOST_REQUIRE(!cf_push(chain)->except); chain.produce_block();
   for (const auto& [imports,body] : cf_forbidden()) {
      BOOST_TEST_CONTEXT("original forbidden " << imports << body) {
         cf_install(chain,cf_pair(cf_program(body,{},imports)));
         const auto trace=cf_push(chain,{},true);
         BOOST_REQUIRE(trace && trace->except);
         BOOST_CHECK_MESSAGE(trace->except->code()==unaccessible_api::code_value,trace->except->to_detail_string());
      }
   }
   cf_install(chain,cf_pair(cf_program("",{},R"((import "env" "get_sender" (func $bad (result i64))))","(drop (call $bad))")));
   const auto start_failure=cf_push(chain,{},true); BOOST_REQUIRE(start_failure && start_failure->except);
   BOOST_CHECK_EQUAL(start_failure->except->code(),unaccessible_api::code_value);
   cf_install(chain,cf_pair(cf_program(),true));
   const auto trace=cf_push(chain,{},true); BOOST_REQUIRE(trace && trace->except);
   BOOST_CHECK_EQUAL(trace->except->code(),action_validate_exception::code_value);
}

BOOST_AUTO_TEST_CASE(original_adjacent_null_output_fails_before_call_and_undoes_parent_write) {
   for(const std::string api : {"db_next_i64","db_previous_i64"}) {
      for(uint32_t output : {0u,8u}) {
         BOOST_TEST_CONTEXT(api << " output=" << output) {
            eosio::testing::tester chain; chain.create_accounts({"test"_n});
            cf_install(chain,{{"test"_n,cf_adjacent_program(api,output),{}}});
            const auto trace=cf_push(chain,{},true); BOOST_REQUIRE(trace);
            const auto row=chain.get_row_by_account("test"_n,"fresh"_n,"rows"_n,name{1});
            if(!output) {
               BOOST_REQUIRE(trace->except); BOOST_CHECK_EQUAL(trace->except->code(),wasm_execution_error::code_value);
               BOOST_CHECK(trace->except->to_detail_string().find("references cannot be created for null pointers")!=std::string::npos);
               BOOST_CHECK(row.empty());
            } else { BOOST_CHECK(!trace->except); BOOST_CHECK_EQUAL(row.size(),8u); }
         }
      }
   }
}
BOOST_AUTO_TEST_SUITE_END()
