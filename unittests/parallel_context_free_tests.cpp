#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/raw.hpp>
#include <array>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

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
std::vector<isolated_receiver_program> cf_graph() {
   return {
      {"test"_n, cf_program("(call $send0)(call $send1)(call $notify " + cf_name("dave"_n) +
         ")(call $send2)(call $send3)", {{false,cf_pack("bob"_n,"normal"_n)}, {true,cf_pack("carol"_n,"pure"_n)},
         {false,cf_pack("dave"_n,"normal"_n)}, {true,cf_pack("bob"_n,"pure"_n)}}), {}},
      {"bob"_n, cf_program(cf_when("normal"_n,"(call $send0)(call $send1)"),
         {{true,cf_pack("dave"_n,"pure"_n)}, {false,cf_pack("carol"_n,"normal"_n)}}), {}},
      {"carol"_n, cf_program(), {}},
      {"dave"_n, cf_program(cf_when("run"_n,"(call $send0)"), {{true,cf_pack("carol"_n,"pure"_n)}}), {}}
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
   for (unsigned test=0;test<8;++test) {
      auto bad=good;
      if(test==0) bad.groups[0].context_free=true;
      if(test==1) bad.groups[2].context_free=false;
      if(test==2) std::get<isolated_tree_inline_event>(bad.steps[0].events[1]).context_free=false;
      if(test==3) std::swap(bad.steps[2],bad.steps[5]);
      if(test==4) bad.steps[2].effects.return_value={'x'};
      if(test==5) bad.steps[2].effects.recipients.push_back({"test"_n,0});
      if(test==6) bad.steps[2].events.emplace_back(isolated_cursor_event{isolated_cursor_query{}});
      if(test==7) bad.groups[2].input.authorizations.push_back({"test"_n,"active"_n});
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
