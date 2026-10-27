#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/raw.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <limits>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key rows{"test"_n, "alice"_n, "rows"_n};
const std::array<secondary_key, 5> keys{uint64_t{10}, uint128_t{10}, key256_t{10, 11},
   secondary_float64{10}, secondary_float128{10}};
std::string n64(name n) { return "(i64.const " + std::to_string(n.to_uint64_t()) + ")"; }
std::vector<char> program(const std::string& body = {}, const std::string& imports = {}, bool sync = false) {
   const auto bytes = wast_to_wasm("(module " + imports + " (memory 1 1) "
      "(func (export \"apply\") (param $receiver i64) (param i64) (param $action i64) " + body + ")" +
      (sync ? "(func (export \"sync_call\") (param i64 i64 i32) (result i64) (i64.const 0))" : "") + ")");
   return {bytes.begin(), bytes.end()};
}
isolated_cursor_event secondary(isolated_secondary_cursor_event event) { return event; }
template<class Event>
std::vector<Event> wrap(std::span<const isolated_cursor_event> events) {
   std::vector<Event> result;
   for (const auto& event : events) result.emplace_back(event);
   return result;
}
struct transcript_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   isolated_action_input root;
   transcript_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      db.add_index<index64_index>(); db.add_index<index128_index>(); db.add_index<index256_index>();
      db.add_index<index_double_index>(); db.add_index<index_long_double_index>();
      for (name n : {"test"_n, "bob"_n}) db.create<account_object>([&](auto& a) { a.name = n; });
      const std::array tables{rows}; const std::array accounts{"test"_n, "bob"_n};
      base = frozen_primary_state::capture_queries_with_secondary(view, tables, {}, {}, {}, {}, accounts);
      root.receiver = root.account = "test"_n; root.action = "run"_n;
      root.authorizations = {{"test"_n, "active"_n}};
   }
   void accounts(primary_state_overlay& state) {
      state.read_account("test"_n); state.read_account("bob"_n);
   }
   std::vector<isolated_cursor_event> stores(primary_state_overlay& state, secondary_cursor_group& group,
         const secondary_key& key) {
      std::vector<isolated_cursor_event> events;
      for (uint64_t pk : {19, 7}) {
         const auto position = state.write_count();
         const auto handle = group.get(secondary_kind(key)).store(rows, pk, "test"_n, key);
         events.push_back(secondary(isolated_secondary_write{secondary_kind(key), position, handle}));
      }
      return events;
   }
   isolated_cursor_event find(secondary_cursor_group& group, const secondary_key& before, uint64_t pk) {
      auto after = before;
      const auto handle = group.get(secondary_kind(before)).find_primary(rows, after, pk);
      return secondary(isolated_secondary_find_primary{rows, pk, before, after, handle});
   }
};
// These are deliberately SYNTHETIC structural-proof fixtures: real executors
// establish code/input/scheduling fingerprints; owned cursor operations supply
// the typed database transcript. They do NOT execute secondary WASM imports,
// prove original pointer semantics/authorization, or authorize canonical writes.
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_secondary_transcript_tests)

BOOST_FIXTURE_TEST_CASE(all_private_proofs_reconstruct_typed_writes_and_reset_each_root_cache, transcript_fixture) {
   const std::array roots{root, root};
   const std::vector<isolated_receiver_program> programs{{"test"_n, program(), {}}};
   const std::vector<isolated_sync_program> sync_programs{{programs[0], false}};
   isolated_notification_executor ne; isolated_action_tree_executor te;
   isolated_sync_executor se; isolated_mixed_tree_executor me;
   const auto n0 = ne.execute(base, roots, programs);
   const auto t0 = te.execute(base, roots, programs, {});
   const auto s0 = se.execute(base, root, sync_programs, {});
   const auto m0 = se.execute_transaction(base, roots, sync_programs, {});
   const auto x0 = me.execute(base, roots, sync_programs, {});
   for (const auto& key : keys) BOOST_TEST_CONTEXT("family=" << key.index()) {
      primary_state_overlay state(base); accounts(state);
      secondary_cursor_group first(state, root.receiver), second(state, root.receiver);
      const auto writes = stores(state, first, key);
      const std::array reads{find(second, key, 7)};
      BOOST_REQUIRE_EQUAL(std::get<isolated_secondary_find_primary>(
         std::get<isolated_secondary_cursor_event>(reads[0])).result, 0);
      const auto expected = state.seal();
      auto n = n0; auto t = t0; auto s = s0; auto m = m0; auto x = x0;
      n.state = t.state = s.state = m.state = x.state = expected;
      for (size_t i = 0; i < 2; ++i) {
         const auto events = i == 0 ? std::span<const isolated_cursor_event>(writes) :
                                    std::span<const isolated_cursor_event>(reads);
         for (auto* effects : {&n.steps[i].effects, &t.steps[i].effects, &m.roots[i].root, &x.steps[i].effects}) {
            effects->writes_begin = i == 0 ? 0 : 2; effects->writes_end = 2;
         }
         n.steps[i].cursor_events.assign(events.begin(), events.end());
         t.steps[i].events = wrap<isolated_action_tree_event>(events);
         m.roots[i].root_events = wrap<isolated_sync_event>(events);
         x.steps[i].events = wrap<isolated_mixed_tree_event>(events);
      }
      s.root.writes_end = 2; s.root_events = wrap<isolated_sync_event>(writes);
      const auto check_all = [&](bool success) {
         BOOST_CHECK_EQUAL(validate_isolated_notification_journal(n, roots, programs), success);
         BOOST_CHECK_EQUAL(validate_isolated_action_tree_journal(t, roots, programs, {}), success);
         BOOST_CHECK_EQUAL(validate_isolated_sync_journal(s, root, sync_programs, {}), success);
         BOOST_CHECK_EQUAL(validate_isolated_sync_transaction_journal(m, roots, sync_programs, {}), success);
         BOOST_CHECK_EQUAL(validate_isolated_mixed_tree_journal(x, roots, sync_programs, {}), success);
      };
      check_all(true);
      // A structurally valid handle from the PREVIOUS group is not valid here.
      auto wrong = reads[0];
      std::get<isolated_secondary_find_primary>(std::get<isolated_secondary_cursor_event>(wrong)).result = 1;
      n.steps[1].cursor_events[0] = wrong; t.steps[1].events[0] = wrong;
      m.roots[1].root_events[0] = wrong; x.steps[1].events[0] = wrong;
      s.root_events.pop_back(); // missing write event in the single-root form
      check_all(false);
      BOOST_CHECK(!view.read_table(rows));
   }
}

BOOST_FIXTURE_TEST_CASE(notification_receivers_share_all_secondary_caches_but_not_write_authority, transcript_fixture) {
   const auto code = program("(call $notify " + n64("bob"_n) + ")",
      "(import \"env\" \"require_recipient\" (func $notify (param i64)))");
   const std::array roots{root};
   const std::vector<isolated_receiver_program> programs{{"test"_n, code, {}}, {"bob"_n, program(), {}}};
   const std::vector<isolated_sync_program> sync_programs{{programs[0], false}, {programs[1], false}};
   isolated_notification_executor ne; isolated_action_tree_executor te; isolated_mixed_tree_executor me;
   const auto n0 = ne.execute(base, roots, programs);
   const auto t0 = te.execute(base, roots, programs, {});
   const auto x0 = me.execute(base, roots, sync_programs, {});
   for (const auto& key : keys) BOOST_TEST_CONTEXT("family=" << key.index()) {
      primary_state_overlay state(base); accounts(state); secondary_cursor_group group(state, root.receiver);
      auto writes = stores(state, group, key);
      group.set_receiver_for_notification("bob"_n);
      const std::array reads{find(group, key, 7)};
      BOOST_REQUIRE_EQUAL(std::get<isolated_secondary_find_primary>(
         std::get<isolated_secondary_cursor_event>(reads[0])).result, 1);
      const auto expected = state.seal();
      auto n = n0; auto t = t0; auto x = x0;
      n.state = t.state = x.state = expected;
      writes.push_back(isolated_cursor_notify{0});
      for (auto* effects : {&n.steps[0].effects, &t.steps[0].effects, &x.steps[0].effects}) {
         effects->writes_end = 2; effects->recipients[0].after_writes = 2;
      }
      for (auto* effects : {&n.steps[1].effects, &t.steps[1].effects, &x.steps[1].effects})
         effects->writes_begin = effects->writes_end = 2;
      n.steps[0].cursor_events = writes; n.steps[1].cursor_events.assign(reads.begin(), reads.end());
      t.steps[0].events = wrap<isolated_action_tree_event>(writes); t.steps[1].events = wrap<isolated_action_tree_event>(reads);
      x.steps[0].events = wrap<isolated_mixed_tree_event>(writes); x.steps[1].events = wrap<isolated_mixed_tree_event>(reads);
      BOOST_CHECK(validate_isolated_notification_journal(n, roots, programs));
      BOOST_CHECK(validate_isolated_action_tree_journal(t, roots, programs, {}));
      BOOST_CHECK(validate_isolated_mixed_tree_journal(x, roots, sync_programs, {}));
      auto wrong = reads[0];
      std::get<isolated_secondary_find_primary>(std::get<isolated_secondary_cursor_event>(wrong)).result = 0;
      n.steps[1].cursor_events[0] = wrong; t.steps[1].events[0] = wrong; x.steps[1].events[0] = wrong;
      BOOST_CHECK(!validate_isolated_notification_journal(n, roots, programs));
      BOOST_CHECK(!validate_isolated_action_tree_journal(t, roots, programs, {}));
      BOOST_CHECK(!validate_isolated_mixed_tree_journal(x, roots, sync_programs, {}));
      // Invent a bob write to test's table. Reject before an index is mutated.
      primary_state_overlay foreign(base); secondary_cursor_group foreign_group(foreign, "bob"_n); size_t next = 0;
      BOOST_CHECK(!replay_isolated_secondary_cursor_event(foreign_group,
         std::get<isolated_secondary_cursor_event>(writes[0]), expected, next, 2, false));
      BOOST_CHECK_EQUAL(foreign.write_count(), 0u);
   }
}

BOOST_FIXTURE_TEST_CASE(inline_groups_reset_secondary_caches_and_context_free_groups_reject_state_events, transcript_fixture) {
   action child; child.account = "test"_n; child.name = "leaf"_n;
   const auto packed = fc::raw::pack(child);
   constexpr char hex[] = "0123456789abcdef"; std::string data;
   for (unsigned char byte : packed) { data += '\\'; data += hex[byte >> 4]; data += hex[byte & 15]; }
   for (bool cf : {false, true}) {
      const auto code = program("(if (i64.eq (get_local $action) " + n64("run"_n) + ") (then "
         "(call $send (i32.const 1024) (i32.const " + std::to_string(packed.size()) + "))))",
         "(import \"env\" \"" + std::string(cf ? "send_context_free_inline" : "send_inline") +
         "\" (func $send (param i32 i32))) (data (i32.const 1024) \"" + data + "\")");
      const std::array roots{root}; const std::vector<isolated_receiver_program> programs{{"test"_n, code, {}}};
      const std::vector<isolated_sync_program> sync_programs{{programs[0], false}};
      isolated_action_tree_context context;
      context.context_free_data = isolated_context_free_data::capture({});
      isolated_mixed_tree_context mixed_context{context, {}};
      isolated_action_tree_executor te; isolated_mixed_tree_executor me;
      const auto t0 = te.execute(base, roots, programs, context);
      const auto x0 = me.execute(base, roots, sync_programs, mixed_context);
      for (const auto& key : keys) BOOST_TEST_CONTEXT("family=" << key.index() << " cf=" << cf) {
         primary_state_overlay state(base); accounts(state);
         secondary_cursor_group parent(state, root.receiver), leaf(state, root.receiver);
         const auto writes = stores(state, parent, key); const std::array reads{find(leaf, key, 7)};
         const auto expected = state.seal(); auto t = t0; auto x = x0; t.state = x.state = expected;
         auto send = std::get<isolated_tree_inline_event>(t.steps[0].events[0]); send.after_writes = 2;
         t.steps[0].events = wrap<isolated_action_tree_event>(writes); t.steps[0].events.push_back(send);
         x.steps[0].events = wrap<isolated_mixed_tree_event>(writes); x.steps[0].events.push_back(send);
         t.steps[1].events = wrap<isolated_action_tree_event>(reads); x.steps[1].events = wrap<isolated_mixed_tree_event>(reads);
         t.steps[0].effects.writes_end = x.steps[0].effects.writes_end = 2;
         for (auto* e : {&t.steps[1].effects, &x.steps[1].effects}) e->writes_begin = e->writes_end = 2;
         BOOST_CHECK_EQUAL(validate_isolated_action_tree_journal(t, roots, programs, context), !cf);
         BOOST_CHECK_EQUAL(validate_isolated_mixed_tree_journal(x, roots, sync_programs, mixed_context), !cf);
         if (!cf) {
            auto wrong = reads[0];
            std::get<isolated_secondary_find_primary>(std::get<isolated_secondary_cursor_event>(wrong)).result = 1;
            t.steps[1].events[0] = wrong; x.steps[1].events[0] = wrong;
            BOOST_CHECK(!validate_isolated_action_tree_journal(t, roots, programs, context));
            BOOST_CHECK(!validate_isolated_mixed_tree_journal(x, roots, sync_programs, mixed_context));
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(sync_child_removal_invalidates_ancestor_and_readonly_inheritance_rejects_writes, transcript_fixture) {
   const auto code = program("(drop (call $call " + n64("test"_n) + " (i64.const 0) (i32.const 0) (i32.const 0)))",
      "(import \"env\" \"call\" (func $call (param i64 i64 i32 i32) (result i64)))", true);
   const std::array roots{root}; const std::vector<isolated_sync_program> programs{{{"test"_n, code, {}}, true}};
   isolated_sync_executor se; isolated_mixed_tree_executor me;
   for (bool readonly : {false, true}) {
      isolated_sync_context context; context.root_read_only = readonly;
      isolated_mixed_tree_context mixed_context{{}, context};
      const auto s0 = se.execute(base, root, programs, context);
      const auto m0 = se.execute_transaction(base, roots, programs, context);
      const auto x0 = me.execute(base, roots, programs, mixed_context);
      for (const auto& key : keys) BOOST_TEST_CONTEXT("family=" << key.index() << " readonly=" << readonly) {
         primary_state_overlay state(base); accounts(state);
         secondary_cursor_group parent(state, root.receiver), child(state, root.receiver);
         const auto writes = stores(state, parent, key);
         const auto child_find = find(child, key, 7);
         child.get(secondary_kind(key)).remove(0);
         const std::array child_events{child_find, secondary(isolated_secondary_write{secondary_kind(key), 2, 0})};
         const auto after = find(parent, key, 7);
         const auto expected = state.seal(); auto s = s0; auto m = m0; auto x = x0;
         s.state = m.state = x.state = expected;
         auto call = std::get<isolated_sync_call_event>(s.root_events[0]); call.after_writes = 2;
         auto root_events = wrap<isolated_sync_event>(writes); root_events.push_back(call); root_events.push_back(after);
         s.root_events = m.roots[0].root_events = root_events;
         x.steps[0].events = wrap<isolated_mixed_tree_event>(writes); x.steps[0].events.push_back(call); x.steps[0].events.push_back(after);
         s.root.writes_end = m.roots[0].root.writes_end = x.steps[0].effects.writes_end = 3;
         for (auto* trace : {&s.calls[0], &m.roots[0].calls[0], &x.steps[0].calls[0]}) {
            trace->writes_begin = 2; trace->writes_end = 3; trace->events = wrap<isolated_sync_event>(child_events);
         }
         const auto check_all = [&](bool success) {
            BOOST_CHECK_EQUAL(validate_isolated_sync_journal(s, root, programs, context), success);
            BOOST_CHECK_EQUAL(validate_isolated_sync_transaction_journal(m, roots, programs, context), success);
            BOOST_CHECK_EQUAL(validate_isolated_mixed_tree_journal(x, roots, programs, mixed_context), success);
         };
         check_all(!readonly);
         const auto stale = secondary(isolated_secondary_adjacent{secondary_kind(key), false, 1, -2, 777, 777});
         s.root_events.back() = stale; m.roots[0].root_events.back() = stale; x.steps[0].events.back() = stale;
         check_all(false);
         BOOST_CHECK(!view.read_table(rows));
      }
   }
}

BOOST_FIXTURE_TEST_CASE(end_only_readonly_transcripts_need_capability_proof_and_propagate_owner_cancellation, transcript_fixture) {
   const std::array roots{root};
   const std::vector<isolated_receiver_program> programs{{"test"_n, program(), {}}};
   const std::vector<isolated_sync_program> sync_programs{{programs[0], false}};
   isolated_sync_context context; context.root_read_only = true;
   isolated_mixed_tree_context mixed_context{{}, context};
   isolated_notification_executor ne; isolated_action_tree_executor te;
   isolated_sync_executor se; isolated_mixed_tree_executor me;
   const auto n0 = ne.execute(base, roots, programs);
   const auto t0 = te.execute(base, roots, programs, {});
   const auto s0 = se.execute(base, root, sync_programs, context);
   const auto m0 = se.execute_transaction(base, roots, sync_programs, context);
   const auto x0 = me.execute(base, roots, sync_programs, mixed_context);
   for (const auto& key : keys) BOOST_TEST_CONTEXT("family=" << key.index()) {
      const auto kind = secondary_kind(key);
      primary_state_overlay state(base); accounts(state); secondary_cursor_group group(state, root.receiver);
      auto& cursor = group.get(kind);
      const auto end = cursor.end(rows); uint64_t primary = 777;
      const auto next = cursor.next(std::numeric_limits<int32_t>::min(), primary);
      BOOST_REQUIRE_EQUAL(end, -1); BOOST_REQUIRE_EQUAL(next, -1); BOOST_REQUIRE_EQUAL(primary, 777u);
      const std::array events{
         secondary(isolated_secondary_end{rows, kind, end}),
         secondary(isolated_secondary_adjacent{kind, false, std::numeric_limits<int32_t>::min(), next, 777, primary})};
      const auto expected = state.seal();
      BOOST_REQUIRE(expected.writes().empty()); BOOST_REQUIRE(expected.secondary_read_queries().empty());
      BOOST_REQUIRE(!expected.primary_only());
      auto n = n0; auto t = t0; auto s = s0; auto m = m0; auto x = x0;
      n.state = t.state = s.state = m.state = x.state = expected;
      const auto replace = [&](std::span<const isolated_cursor_event> incoming) {
         n.steps[0].cursor_events.assign(incoming.begin(), incoming.end());
         t.steps[0].events = wrap<isolated_action_tree_event>(incoming);
         s.root_events = m.roots[0].root_events = wrap<isolated_sync_event>(incoming);
         x.steps[0].events = wrap<isolated_mixed_tree_event>(incoming);
      };
      using checkpoint = std::function<void()>;
      const std::array<std::function<bool(const checkpoint&)>, 5> proofs{
         [&](const checkpoint& check) { return validate_isolated_notification_journal(n, roots, programs, check); },
         [&](const checkpoint& check) { return validate_isolated_action_tree_journal(t, roots, programs, {}, check); },
         [&](const checkpoint& check) { return validate_isolated_sync_journal(s, root, sync_programs, context, check); },
         [&](const checkpoint& check) { return validate_isolated_sync_transaction_journal(m, roots, sync_programs, context, check); },
         [&](const checkpoint& check) { return validate_isolated_mixed_tree_journal(x, roots, sync_programs, mixed_context, check); }};
      replace(events);
      struct cancellation : std::runtime_error { using std::runtime_error::runtime_error; };
      for (const auto& proof : proofs) {
         size_t count = 0;
         BOOST_REQUIRE(proof([&] { ++count; })); BOOST_CHECK_GT(count, 2u);
         for (size_t target = 1; target <= count; ++target) {
            size_t visited = 0;
            BOOST_CHECK_THROW(proof([&] { if (++visited == target) throw cancellation("owner cancelled"); }), cancellation);
         }
         BOOST_CHECK(proof({}));
      }
      replace({}); // neither writes nor row dependencies can detect this alone
      for (const auto& proof : proofs) BOOST_CHECK(!proof({}));
      auto invalid = events;
      std::get<isolated_secondary_end>(std::get<isolated_secondary_cursor_event>(invalid[0])).kind =
         static_cast<secondary_key_kind>(255);
      replace(invalid);
      for (const auto& proof : proofs) BOOST_CHECK(!proof({}));
      BOOST_CHECK(!view.read_table(rows));
   }
}

BOOST_AUTO_TEST_SUITE_END()
