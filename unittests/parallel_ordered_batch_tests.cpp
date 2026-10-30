#include <eosio/chain/parallel_ordered_batch.hpp>
#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <cstring>
#include <future>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key a{"test"_n, "alice"_n, "rows"_n};
constexpr table_key b{"test"_n, "bob"_n, "rows"_n};
constexpr table_key absent{"test"_n, "missing"_n, "rows"_n};
fc::sha256 digest(const char* text) { return fc::sha256::hash(text, std::strlen(text)); }

struct batch_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   const std::array<ordered_attempt_batch::request, 3> requests{{
      {digest("tx0"), digest("execution0")}, {digest("tx1"), digest("execution1")},
      {digest("tx2"), digest("execution2")}}};
   batch_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      for (const auto& key : {a, b}) {
         const auto& table = db.create<table_id_object>([&](auto& row) {
            row.code = key.code; row.scope = key.scope; row.table = key.table;
            row.payer = "alice"_n; row.count = 1;
         });
         db.create<key_value_object>([&](auto& row) {
            row.t_id = table.id; row.primary_key = 1; row.payer = "alice"_n;
            row.value.assign("x", 1);
         });
      }
      const std::array keys{a, b, absent};
      base = frozen_primary_state::capture(view, keys);
   }
   isolated_action_result output(size_t index, table_key key, bool write = true) {
      primary_state_overlay overlay(base);
      overlay.locate_primary({key, primary_query_kind::find, 1});
      if (write) overlay.update_existing(key, 1, name{}, std::span<const char>("y", 1));
      return {overlay.seal(), {}, requests[index].execution_fingerprint};
   }
};

secondary_key batch_key(unsigned family, uint64_t value) {
   switch (family) {
      case 0: return value;
      case 1: return uint128_t{value};
      case 2: return key256_t{uint128_t{value}, uint128_t{0}};
      case 3: return secondary_float64{ui64_to_f64(value).v};
      case 4: {
         secondary_float128 out; fc::float128_to_uint128(ui64_to_f128(value), out.bits); return out;
      }
   }
   throw std::logic_error("bad test family");
}
struct secondary_batch_fixture : batch_fixture {
   template<class Object> void seed(unsigned family) {
      db.add_index<typename chainbase::get_index_type<Object>::type>();
      const auto& table = db.get<table_id_object, by_code_scope_table>(boost::make_tuple(a.code, a.scope, a.table));
      for (uint64_t pk : {1, 2, 3, 4}) db.create<Object>([&](auto& row) {
         row.t_id = table.id; row.primary_key = pk; row.payer = "alice"_n;
         const auto owned = batch_key(family, pk == 4 ? 20 : pk * 10);
         using native = typename Object::secondary_key_type;
         if constexpr (std::is_same_v<native, float64_t>) row.secondary_key = {std::get<secondary_float64>(owned).bits};
         else if constexpr (std::is_same_v<native, float128_t>) fc::uint128_to_float128(std::get<secondary_float128>(owned).bits, row.secondary_key);
         else row.secondary_key = std::get<native>(owned);
      });
      db.modify(table, [](auto& row) { row.count += 4; });
   }
   secondary_batch_fixture() {
      seed<index64_object>(0); seed<index128_object>(1); seed<index256_object>(2);
      seed<index_double_object>(3); seed<index_long_double_object>(4);
   }
   void capture(const secondary_query& query) {
      std::vector<secondary_query> reads{query};
      for (unsigned family = 0; family < 5; ++family) for (uint64_t pk : {1, 2, 3, 4, 5})
         reads.push_back({a, secondary_query_kind::find_primary, batch_key(family, 0), pk});
      const std::array tables{a, b, absent};
      const std::array primary{primary_query{a, primary_query_kind::find, 1}};
      base = frozen_primary_state::capture_queries_with_secondary(view, tables, primary, reads);
   }
   // Coordinator tests only: callback routing is NOT proof of canonical
   // adoption. Actual WASM/original-host and controller tests are separate.
   void route(primary_overlay_result prefix, primary_overlay_result reader, bool conflict) {
      ordered_attempt_batch batch(base, requests);
      const isolated_notification_result first{std::move(prefix), {}, requests[0].execution_fingerprint};
      const isolated_notification_result second{std::move(reader), {}, requests[1].execution_fingerprint};
      unsigned callbacks = 0, serial = 0;
      batch.run_next<isolated_notification_result>(batch.get_ticket(0), requests[0].transaction_id, first,
         [&](const auto&) { ++callbacks; }, [&] { ++serial; });
      BOOST_CHECK_EQUAL(callbacks, 1u); BOOST_CHECK_EQUAL(serial, 0u);
      const auto status = conflict ? ordered_attempt_batch::candidate_status::dependency_conflict :
                                    ordered_attempt_batch::candidate_status::reusable;
      BOOST_CHECK(batch.inspect_next<isolated_notification_result>(batch.get_ticket(1), requests[1].transaction_id, second) == status);
      BOOST_CHECK_EQUAL(batch.next_index(), 1u);
      BOOST_CHECK(!batch.get_ticket(2).stop_token().stop_requested());
      batch.run_next<isolated_notification_result>(batch.get_ticket(1), requests[1].transaction_id, second,
         [&](const auto&) { ++callbacks; }, [&] { ++serial; });
      BOOST_CHECK_EQUAL(callbacks, conflict ? 1u : 2u); BOOST_CHECK_EQUAL(serial, conflict ? 1u : 0u);
      BOOST_CHECK_EQUAL(batch.get_ticket(2).stop_token().stop_requested(), conflict);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_ordered_batch_tests)

BOOST_FIXTURE_TEST_CASE(secondary_ordered_ranges_duplicates_absence_anchor_and_aba_use_original_order, secondary_batch_fixture) {
   using kind = secondary_query_kind;
   struct scenario { kind query; uint64_t key, anchor, written, value; bool conflict; };
   const scenario cases[]{
      scenario{kind::find_primary,0,2,3,35,false}, {kind::find_primary,0,3,3,30,true},
      {kind::find_secondary,20,0,3,20,false}, {kind::find_secondary,20,0,1,20,true},
      {kind::lower_bound,20,0,1,25,false}, {kind::lower_bound,20,0,1,20,true},
      {kind::upper_bound,20,0,1,25,true}, {kind::upper_bound,20,0,1,20,false},
      {kind::find_secondary,25,0,3,25,true}, {kind::lower_bound,40,0,3,50,true},
      {kind::upper_bound,40,0,3,50,true}, {kind::previous,30,3,1,25,true},
      {kind::next,20,2,1,20,false}, {kind::next,20,2,3,20,true},
      {kind::previous,20,4,3,20,true}, {kind::first,0,0,3,5,true},
      {kind::last,0,0,1,50,true}, {kind::next,30,3,1,50,true},
      {kind::previous,10,1,3,5,true}, {kind::next,20,2,2,60,true},
      {kind::previous,20,4,1,5,false}, {kind::last,0,0,1,5,false},
      {kind::first,0,0,3,50,false}, {kind::find_secondary,25,0,3,26,false}};
   for (unsigned family = 0; family < 5; ++family) {
      for (const auto& s : cases) BOOST_TEST_CONTEXT("family=" << family << " query=" << unsigned(s.query)
         << " key=" << s.key << " written=" << s.written << " value=" << s.value) {
         const secondary_query query{a, s.query, batch_key(family, s.key), s.anchor}; capture(query);
         primary_state_overlay prefix(base), reader(base);
         prefix.update_secondary(a, s.written, name{}, batch_key(family, s.value));
         reader.read_secondary(query);
         route(prefix.seal(), reader.seal(), s.conflict);
      }
      const secondary_query query{a, kind::lower_bound, batch_key(family, 11), 0}; capture(query);
      primary_state_overlay prefix(base), reader(base);
      prefix.update_secondary(a, 3, name{}, batch_key(family, 15));
      prefix.update_secondary(a, 3, name{}, batch_key(family, 30));
      reader.read_secondary(query); route(prefix.seal(), reader.seal(), true);
      // Index kinds are independent when no shared table structure changes.
      primary_state_overlay other(base), same_reader(base);
      other.update_secondary(a, 2, name{}, batch_key((family + 1) % 5, 50));
      same_reader.read_secondary(query); route(other.seal(), same_reader.seal(), false);
      // Primary and secondary lifecycle changes share the table count/barrier.
      for (bool primary_lifecycle : {false, true}) {
         primary_state_overlay structural(base), observed(base);
         if (primary_lifecycle) {
            structural.remove(a, 1); structural.store(a, 1, "alice"_n, std::span<const char>("x", 1));
            observed.read_secondary(query);
         } else {
            structural.store_secondary(a, 5, "alice"_n, batch_key(family, 50));
            structural.remove_secondary(a, 5, batch_key(family, 0));
            observed.locate_primary({a, primary_query_kind::find, 1});
         }
         route(structural.seal(), observed.seal(), true);
      }
   }
   for (unsigned family : {3, 4}) {
      const auto positive = batch_key(family, 0);
      const secondary_key negative = family == 3 ? secondary_key{secondary_float64{uint64_t{1} << 63}} :
         secondary_key{secondary_float128{uint128_t{1} << 127}};
      const secondary_query query{a, kind::find_secondary, positive, 0}; capture(query);
      primary_state_overlay prefix(base), reader(base);
      prefix.update_secondary(a, 1, name{}, negative);
      reader.read_secondary(query); route(prefix.seal(), reader.seal(), true);
      BOOST_CHECK(positive != negative); BOOST_CHECK(secondary_key_equivalent(positive, negative));
   }
}

// Untranscribed secondary effects must fail every private proof, even an
// end-only call with no row query or write. Legacy results remain denied;
// rich results reach an adopter, which MUST reject the incomplete proof.
BOOST_FIXTURE_TEST_CASE(untranscribed_secondary_effects_fail_proofs_and_ordered_callbacks, batch_fixture) {
   db.add_index<account_index>();
   db.create<account_object>([](auto& account) { account.name = "test"_n; });
   const std::array tables{a, b, absent};
   const std::array queries{primary_query{a, primary_query_kind::find, 1},
                            primary_query{b, primary_query_kind::find, 1}};
   const std::array accounts{"test"_n};
   base = frozen_primary_state::capture_queries(view, tables, queries, {}, {}, accounts);
   const auto bytes = wast_to_wasm(R"((module (memory 1 1)
      (func (export "apply") (param i64 i64 i64))))");
   const std::vector<isolated_receiver_program> programs{{"test"_n, {bytes.begin(), bytes.end()}, {}}};
   const std::vector<isolated_sync_program> sync_programs{{programs.front(), false}};
   isolated_action_input root;
   root.receiver = root.account = "test"_n; root.action = "run"_n;
   root.authorizations = {{"test"_n, "active"_n}};
   const std::array roots{root, root};
   isolated_notification_executor notification_executor;
   isolated_action_tree_executor tree_executor;
   isolated_sync_executor sync_executor;
   isolated_mixed_tree_executor mixed_executor;
   const auto notification = notification_executor.execute(base, roots, programs);
   const auto tree = tree_executor.execute(base, roots, programs, {});
   const auto sync = sync_executor.execute(base, root, sync_programs, {});
   const auto multi_sync = sync_executor.execute_transaction(base, roots, sync_programs, {});
   const auto mixed = mixed_executor.execute(base, roots, sync_programs, {});
   BOOST_REQUIRE(validate_isolated_notification_journal(notification, roots, programs));
   BOOST_REQUIRE(validate_isolated_action_tree_journal(tree, roots, programs, {}));
   BOOST_REQUIRE(validate_isolated_sync_journal(sync, root, sync_programs, {}));
   BOOST_REQUIRE(validate_isolated_sync_transaction_journal(multi_sync, roots, sync_programs, {}));
   BOOST_REQUIRE(validate_isolated_mixed_tree_journal(mixed, roots, sync_programs, {}));
   const std::array<secondary_key, 5> types{uint64_t{0}, uint128_t{0}, std::array<uint128_t, 2>{},
      secondary_float64{}, secondary_float128{}};
   for (const auto& type : types) for (unsigned mode = 0; mode < 4; ++mode) {
      BOOST_TEST_CONTEXT("index=" << type.index() << " capability=" << mode) {
         primary_state_overlay state(base);
         secondary_overlay_cursor cursor(state, "test"_n, secondary_kind(type));
         if (mode == 0) BOOST_CHECK_EQUAL(cursor.end(a), -2);
         if (mode == 1) {
            uint64_t unchanged = 123;
            BOOST_CHECK_EQUAL(cursor.next(-2, unchanged), -1);
            BOOST_CHECK_EQUAL(unchanged, 123u);
         }
         if (mode == 2) BOOST_CHECK(!state.read_secondary({absent, secondary_query_kind::find_primary, type, 1}).row);
         if (mode == 3) state.store_secondary(absent, 1, "alice"_n, type);
         const auto unsupported = state.seal();
         BOOST_CHECK(!unsupported.primary_only());
         BOOST_CHECK_EQUAL(unsupported.secondary_read_queries().size(), mode < 2 ? 0u : 1u);
         BOOST_CHECK_EQUAL(unsupported.writes().size(), mode == 3 ? 1u : 0u);
         if (mode == 3) BOOST_CHECK_THROW(unsupported.primary_write(0), primary_state_unsupported);
         auto bad_notification = notification; bad_notification.state = unsupported;
         auto bad_tree = tree; bad_tree.state = unsupported;
         auto bad_sync = sync; bad_sync.state = unsupported;
         auto bad_multi_sync = multi_sync; bad_multi_sync.state = unsupported;
         auto bad_mixed = mixed; bad_mixed.state = unsupported;
         BOOST_CHECK(!validate_isolated_notification_journal(bad_notification, roots, programs));
         BOOST_CHECK(!validate_isolated_action_tree_journal(bad_tree, roots, programs, {}));
         BOOST_CHECK(!validate_isolated_sync_journal(bad_sync, root, sync_programs, {}));
         BOOST_CHECK(!validate_isolated_sync_transaction_journal(bad_multi_sync, roots, sync_programs, {}));
         BOOST_CHECK(!validate_isolated_mixed_tree_journal(bad_mixed, roots, sync_programs, {}));
         // Rejection still preserves the owner's cancellation checkpoint.
         struct stop_probe {};
         const auto stop = [] { throw stop_probe{}; };
         BOOST_CHECK_THROW(validate_isolated_notification_journal(bad_notification, roots, programs, stop), stop_probe);
         BOOST_CHECK_THROW(validate_isolated_action_tree_journal(bad_tree, roots, programs, {}, stop), stop_probe);
         BOOST_CHECK_THROW(validate_isolated_sync_journal(bad_sync, root, sync_programs, {}, stop), stop_probe);
         BOOST_CHECK_THROW(validate_isolated_sync_transaction_journal(bad_multi_sync, roots, sync_programs, {}, stop), stop_probe);
         BOOST_CHECK_THROW(validate_isolated_mixed_tree_journal(bad_mixed, roots, sync_programs, {}, stop), stop_probe);
         ordered_attempt_batch batch(base, requests);
         isolated_action_result result{unsupported, {}, requests[0].execution_fingerprint};
         unsigned adopted = 0, serial = 0;
         batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, result,
            [&](const auto&) { ++adopted; }, [&] { ++serial; });
         BOOST_CHECK(batch.get_ticket(1).stop_token().stop_requested());
         for (size_t i = 1; i < requests.size(); ++i)
            batch.run_next<isolated_action_result>(batch.get_ticket(i), requests[i].transaction_id, output(i, b),
               [&](const auto&) { ++adopted; }, [&] { ++serial; });
         BOOST_CHECK_EQUAL(adopted, 0u); BOOST_CHECK_EQUAL(serial, 3u);
         BOOST_CHECK(batch.complete());
         BOOST_CHECK(!view.read_table(absent)); // no speculative write reached Chainbase
         const std::array rich_requests{ordered_attempt_batch::request{requests[0].transaction_id, bad_notification.root_fingerprint}};
         ordered_attempt_batch rich_batch(base, rich_requests);
         unsigned checked = 0, committed = 0;
         rich_batch.run_next<isolated_notification_result>(rich_batch.get_ticket(0), rich_requests[0].transaction_id,
            bad_notification, [&](const auto& candidate) {
               ++checked;
               if (validate_isolated_notification_journal(candidate, roots, programs)) ++committed;
               else rich_batch.discard_remaining();
            }, [] { BOOST_FAIL("rich dependency routing is separate from full proof rejection"); });
         BOOST_CHECK_EQUAL(checked, 1u); BOOST_CHECK_EQUAL(committed, 0u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(independent_tables_adopt_in_original_order_and_own_result, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   std::vector<size_t> seen;
   for (size_t i = 0; i < 3; ++i) {
      const auto result = output(i, i == 0 ? a : (i == 1 ? b : absent), i != 2);
      auto adopted = batch.run_next<isolated_action_result>(batch.get_ticket(i), requests[i].transaction_id, result,
         [&](const auto&) { seen.push_back(i); return std::make_unique<size_t>(i); },
         [&]() -> std::unique_ptr<size_t> { BOOST_FAIL("unexpected serial fallback"); return {}; });
      BOOST_REQUIRE(adopted); BOOST_CHECK_EQUAL(*adopted, i);
      BOOST_CHECK_EQUAL(batch.next_index(), i + 1);
   }
   BOOST_CHECK(batch.complete());
   BOOST_CHECK(seen == std::vector<size_t>({0, 1, 2}));
}

BOOST_FIXTURE_TEST_CASE(conflict_disables_remaining_speculation_before_serial_callback, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   const auto last = batch.get_ticket(2);
   size_t adopted = 0, serial = 0;
   for (size_t i = 0; i < 3; ++i) {
      auto result = output(i, i == 2 ? b : a);
      batch.run_next<isolated_action_result>(batch.get_ticket(i), requests[i].transaction_id, result,
         [&](const auto&) { ++adopted; }, [&] {
            BOOST_CHECK(last.stop_token().stop_requested()); ++serial;
         });
   }
   BOOST_CHECK_EQUAL(adopted, 1u); BOOST_CHECK_EQUAL(serial, 2u);
   BOOST_CHECK(batch.complete());
}

BOOST_FIXTURE_TEST_CASE(restored_value_still_invalidates_queries_selecting_the_written_row, batch_fixture) {
   for (const auto query : std::array{
         primary_query{a, primary_query_kind::find, 1},
         primary_query{a, primary_query_kind::lower_bound, 0},
         primary_query{a, primary_query_kind::upper_bound, 0},
         primary_query{a, primary_query_kind::last, 0},
         primary_query{a, primary_query_kind::previous, 99}}) {
      ordered_attempt_batch batch(base, std::span(requests).first(2));
      primary_state_overlay first(base);
      first.update_existing(a, 1, name{}, std::span<const char>("y", 1));
      first.update_existing(a, 1, name{}, std::span<const char>("x", 1)); // ABA
      isolated_action_result first_result{first.seal(), {}, requests[0].execution_fingerprint};
      primary_state_overlay second(base);
      (void)second.locate_primary(query);
      isolated_action_result second_result{second.seal(), {}, requests[1].execution_fingerprint};
      batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, first_result, [](const auto&) {}, [] {});
      bool serial = false;
      batch.run_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, second_result,
         [](const auto&) { BOOST_FAIL("selected-row dependency was lost"); }, [&] { serial = true; });
      BOOST_CHECK(serial);
   }
}

BOOST_FIXTURE_TEST_CASE(absent_queries_and_end_metadata_ignore_unrelated_value_updates, batch_fixture) {
   for (const auto query : std::array{
         primary_query{a, primary_query_kind::find, 99},
         primary_query{a, primary_query_kind::lower_bound, 99},
         primary_query{a, primary_query_kind::upper_bound, 1},
         primary_query{a, primary_query_kind::previous, 1}}) {
      ordered_attempt_batch batch(base, std::span(requests).first(2));
      primary_state_overlay second(base);
      BOOST_CHECK(!second.locate_primary(query).row);
      (void)second.read_table(a);
      isolated_action_result result{second.seal(), {}, requests[1].execution_fingerprint};
      batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, output(0, a), [](const auto&) {}, [] {});
      bool adopted = false;
      batch.run_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, result,
         [&](const auto&) { adopted = true; }, [] { BOOST_FAIL("unrelated value update is not a phantom"); });
      BOOST_CHECK(adopted);
   }
}

BOOST_FIXTURE_TEST_CASE(structural_writes_invalidate_absence_ranges_end_and_aba_but_not_other_tables, batch_fixture) {
   for (unsigned operation = 0; operation < 3; ++operation) for (unsigned query = 0; query < 6; ++query) {
      ordered_attempt_batch batch(base, std::span(requests).first(2));
      primary_state_overlay first(base), second(base);
      if (operation == 0) first.store(a, 2, "alice"_n, std::string("new"));
      else {
         first.remove(a, 1);
         if (operation == 2) first.store(a, 1, "alice"_n, std::string("x"));
      }
      const auto key = query == 5 ? b : a;
      if (query == 0 || query == 5) (void)second.read_table(key); // end-only read also depends on structure
      else (void)second.locate_primary({key,
         query == 1 ? primary_query_kind::find : query == 2 ? primary_query_kind::lower_bound :
         query == 3 ? primary_query_kind::upper_bound : primary_query_kind::last, 99});
      isolated_action_result first_result{first.seal(), {}, requests[0].execution_fingerprint};
      isolated_action_result second_result{second.seal(), {}, requests[1].execution_fingerprint};
      batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, first_result, [](const auto&) {}, [] { BOOST_FAIL("first"); });
      const auto used = batch.run_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, second_result,
         [](const auto&) { return true; }, [] { return false; });
      BOOST_CHECK_EQUAL(used, query == 5);
   }
}

BOOST_FIXTURE_TEST_CASE(distinct_rows_in_one_table_adopt_without_losing_same_row_conflicts, batch_fixture) {
   const auto& table = db.get<table_id_object, by_code_scope_table>(boost::make_tuple(a.code, a.scope, a.table));
   db.create<key_value_object>([&](auto& row) {
      row.t_id = table.id; row.primary_key = 2; row.payer = "alice"_n; row.value.assign("x", 1);
   });
   db.modify(table, [](auto& row) { ++row.count; });
   const std::array keys{a};
   base = frozen_primary_state::capture(view, keys);
   ordered_attempt_batch batch(base, requests);
   size_t adopted = 0, serial = 0;
   for (size_t i = 0; i < 3; ++i) {
      primary_state_overlay overlay(base);
      overlay.update_existing(a, i == 0 ? 1 : 2, name{}, std::span<const char>("y", 1));
      isolated_action_result result{overlay.seal(), {}, requests[i].execution_fingerprint};
      batch.run_next<isolated_action_result>(batch.get_ticket(i), requests[i].transaction_id, result,
         [&](const auto&) { ++adopted; }, [&] { ++serial; });
   }
   BOOST_CHECK_EQUAL(adopted, 2u); BOOST_CHECK_EQUAL(serial, 1u);
}

BOOST_FIXTURE_TEST_CASE(no_attempt_is_a_whole_batch_serial_barrier, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   size_t serial = 0;
   batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, std::optional<isolated_action_result>{},
      [](const auto&) { BOOST_FAIL("no result must not be adopted"); }, [&] { ++serial; });
   for (size_t i = 1; i < requests.size(); ++i)
      batch.run_next<isolated_action_result>(batch.get_ticket(i), requests[i].transaction_id, output(i, b),
         [](const auto&) { BOOST_FAIL("barrier must invalidate tail"); }, [&] { ++serial; });
   BOOST_CHECK_EQUAL(serial, 3u);
}

BOOST_FIXTURE_TEST_CASE(foreign_out_of_order_duplicate_and_wrong_transaction_never_enter_callback, batch_fixture) {
   ordered_attempt_batch batch(base, requests), other(base, requests);
   auto first = batch.get_ticket(0);
   auto attempt = output(0, a);
   size_t calls = 0;
   auto adopt = [&](const auto&) { ++calls; };
   auto serial = [&] { ++calls; };
   BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(other.get_ticket(0), requests[0].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(first, requests[1].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(calls, 0u); BOOST_CHECK_EQUAL(batch.next_index(), 0u);
   batch.run_next<isolated_action_result>(first, requests[0].transaction_id, attempt, adopt, serial);
   BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(first, requests[0].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(calls, 1u);
}

BOOST_FIXTURE_TEST_CASE(wrong_base_and_execution_fingerprint_poison_the_batch, batch_fixture) {
   for (bool wrong_base : {false, true}) {
      ordered_attempt_batch batch(base, requests);
      auto ticket = batch.get_ticket(0);
      auto attempt = output(0, a);
      if (wrong_base) {
         const std::array keys{a};
         primary_state_overlay other(frozen_primary_state::capture(view, keys));
         attempt.state = other.seal(); // identical content, DIFFERENT snapshot identity
      } else attempt.execution_fingerprint = requests[1].execution_fingerprint;
      size_t calls = 0;
      BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(ticket, requests[0].transaction_id, attempt,
         [&](const auto&) { ++calls; }, [&] { ++calls; }), primary_state_invalid_attempt);
      BOOST_CHECK_EQUAL(calls, 0u);
      BOOST_CHECK(ticket.stop_token().stop_requested());
      BOOST_CHECK_THROW(batch.get_ticket(0), primary_state_invalid_attempt);
   }
}

BOOST_FIXTURE_TEST_CASE(callback_failure_never_retries_a_partial_canonical_action, batch_fixture) {
   for (bool with_attempt : {false, true}) {
      ordered_attempt_batch batch(base, requests);
      auto ticket = batch.get_ticket(0);
      size_t mutations = 0;
      std::optional<isolated_action_result> attempt;
      if (with_attempt) attempt = output(0, a);
      BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(ticket, requests[0].transaction_id, attempt,
         [&](const auto&) { ++mutations; throw std::runtime_error("adopt failure"); },
         [&] { ++mutations; throw std::runtime_error("serial failure"); }), std::runtime_error);
      BOOST_CHECK_EQUAL(mutations, 1u); BOOST_CHECK_EQUAL(batch.next_index(), 0u);
      BOOST_CHECK(ticket.stop_token().stop_requested());
      BOOST_CHECK_THROW(batch.get_ticket(0), primary_state_invalid_attempt);
   }
}

BOOST_FIXTURE_TEST_CASE(bookkeeping_budget_and_owner_interrupts_precede_all_callbacks, batch_fixture) {
   primary_overlay_limits limits; limits.max_write_operations = 4097;
   primary_state_overlay large(base, limits);
   for (size_t i = 0; i < 4097; ++i) large.update_existing(a, 1, name{}, std::span<const char>("y", 1));
   const isolated_action_result oversized{large.seal(), {}, requests[0].execution_fingerprint};
   ordered_attempt_batch bounded(base, requests);
   unsigned adopted = 0, serial = 0;
   bounded.run_next<isolated_action_result>(bounded.get_ticket(0), requests[0].transaction_id, oversized,
      [&](const auto&) { ++adopted; }, [&] { ++serial; });
   BOOST_CHECK_EQUAL(adopted, 0u); BOOST_CHECK_EQUAL(serial, 1u);
   BOOST_CHECK(bounded.get_ticket(1).stop_token().stop_requested());
   // An owner exception is not a local allocation failure, including when
   // raised from inside the write-bookkeeping loop rather than at entry.
   for (unsigned stop_at : {1, 3}) {
      ordered_attempt_batch batch(base, requests); const auto tag = batch.get_ticket(0);
      unsigned checkpoints = 0, callbacks = 0;
      BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(tag, requests[0].transaction_id, output(0, a),
         [&](const auto&) { ++callbacks; }, [&] { ++callbacks; }, [&] {
            if (++checkpoints == stop_at) throw std::bad_alloc{};
         }), std::bad_alloc);
      BOOST_CHECK_EQUAL(callbacks, 0u); BOOST_CHECK_EQUAL(checkpoints, stop_at);
      BOOST_CHECK_EQUAL(batch.next_index(), 0u); BOOST_CHECK(tag.stop_token().stop_requested());
      BOOST_CHECK_THROW(batch.get_ticket(0), primary_state_invalid_attempt);
   }
   ordered_attempt_batch guarded(base, requests);
   const auto tag = guarded.get_ticket(0);
   unsigned checkpoints = 0, commits = 0;
   const auto result = output(0, a);
   guarded.run_next<isolated_action_result>(tag, requests[0].transaction_id, result,
      [&](const auto&) { ++commits; }, [] { BOOST_FAIL("unexpected checkpoint fallback"); }, [&] {
         ++checkpoints;
         BOOST_CHECK_THROW(guarded.close(), primary_state_invalid_attempt);
         BOOST_CHECK_THROW(guarded.run_next<isolated_action_result>(tag, requests[0].transaction_id, result,
            [](const auto&) {}, [] {}), primary_state_invalid_attempt);
      });
   BOOST_CHECK_GE(checkpoints, 3u); BOOST_CHECK_EQUAL(commits, 1u); BOOST_CHECK_EQUAL(guarded.next_index(), 1u);
}

BOOST_FIXTURE_TEST_CASE(reentrancy_and_foreign_thread_cannot_advance_or_close_batch, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   auto ticket = batch.get_ticket(1);
   auto result = output(1, b);
   batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, output(0, a), [&](const auto&) {
      BOOST_CHECK_THROW(batch.close(), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(ticket, requests[1].transaction_id, result, [](const auto&) {}, [] {}), primary_state_invalid_attempt);
   }, [] { BOOST_FAIL("unexpected fallback"); });
   auto worker = std::async(std::launch::async, [&] {
      unsigned rejected = 0;
      try { (void)batch.next_index(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { batch.close(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { batch.run_next<isolated_action_result>(ticket, requests[1].transaction_id, result, [](const auto&) {}, [] {}); }
      catch (const state_view_wrong_thread&) { ++rejected; }
      return rejected;
   });
   BOOST_CHECK_EQUAL(worker.get(), 3u); BOOST_CHECK_EQUAL(batch.next_index(), 1u);
   BOOST_CHECK(!ticket.stop_token().stop_requested());
}

BOOST_FIXTURE_TEST_CASE(tickets_outlive_closed_coordinator_but_cannot_enter_new_same_height_batch, batch_fixture) {
   auto coordinator = std::make_unique<ordered_attempt_batch>(base, requests);
   auto old = coordinator->get_ticket(0);
   coordinator.reset();
   BOOST_CHECK(old.stop_token().stop_requested());
   BOOST_CHECK(old.base() == base);
   auto reader = std::async(std::launch::async, [old] {
      return old.base()->locate_primary({a, primary_query_kind::find, 1}).row.has_value();
   });
   BOOST_CHECK(reader.get());
   ordered_attempt_batch replacement(base, requests);
   BOOST_CHECK_THROW(replacement.run_next<isolated_action_result>(old, requests[0].transaction_id, output(0, a),
      [](const auto&) {}, [] {}), primary_state_invalid_attempt);
   replacement.close();
   BOOST_CHECK(!replacement.complete());
   BOOST_CHECK_THROW(replacement.get_ticket(0), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(batch_size_and_identity_budgets_fail_before_publication, batch_fixture) {
   BOOST_CHECK_THROW(ordered_attempt_batch(nullptr, requests), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(ordered_attempt_batch(base, {}), primary_state_budget_exceeded);
   std::vector<ordered_attempt_batch::request> large(parallel_runtime_limits::max_batch_transactions + 1, requests[0]);
   BOOST_CHECK_THROW(ordered_attempt_batch(base, large), primary_state_budget_exceeded);
   auto invalid = requests; invalid[0].transaction_id = {};
   BOOST_CHECK_THROW(ordered_attempt_batch(base, invalid), primary_state_invalid_attempt);
   invalid = requests; invalid[0].execution_fingerprint = {};
   BOOST_CHECK_THROW(ordered_attempt_batch(base, invalid), primary_state_invalid_attempt);
   ordered_attempt_batch batch(base, requests);
   BOOST_CHECK_THROW(batch.get_ticket(requests.size()), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(discovery_retries_preserve_completed_slots_and_reject_replaced_tickets, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   const auto complete = batch.get_ticket(0), previous = batch.get_ticket(1), tail = batch.get_ticket(2);
   const auto completed_result = output(0, a, false);
   const std::array keys{a, b, absent};
   const auto fresh = frozen_primary_state::capture(view, keys);
   auto changed = requests[1]; changed.execution_fingerprint = digest("expanded-owned-input");
   const std::array retries{ordered_attempt_batch::retry_request{1, changed}};
   batch.prepare_retries(fresh, retries);
   BOOST_CHECK(complete.base() == base);
   BOOST_CHECK(previous.base() == base); // immutable even after replacement
   BOOST_CHECK(tail.base() == base);
   BOOST_CHECK(batch.get_ticket(1).base() == fresh);
   unsigned adopted = 0;
   batch.run_next<isolated_action_result>(complete, requests[0].transaction_id, completed_result,
      [&](const auto&) { ++adopted; }, [] { BOOST_FAIL("completed slot was lost"); });
   primary_state_overlay second(fresh); second.locate_primary({b, primary_query_kind::find, 1});
   const isolated_action_result retried{second.seal(), {}, changed.execution_fingerprint};
   BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(previous, requests[1].transaction_id, retried,
      [](const auto&) {}, [] {}), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(batch.next_index(), 1u);
   batch.run_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, retried,
      [&](const auto&) { ++adopted; }, [] { BOOST_FAIL("retried slot not adopted"); });
   batch.run_next<isolated_action_result>(tail, requests[2].transaction_id, output(2, b, false),
      [&](const auto&) { ++adopted; }, [] { BOOST_FAIL("unmodified tail lost"); });
   BOOST_CHECK_EQUAL(adopted, 3u); BOOST_CHECK(batch.complete());
}

BOOST_FIXTURE_TEST_CASE(discovery_retry_validation_is_atomic_and_cannot_reopen_committed_or_cancelled_slots, batch_fixture) {
   const std::array keys{a, b};
   const auto fresh = frozen_primary_state::capture(view, keys);
   const std::array retry{ordered_attempt_batch::retry_request{1, requests[1]}};
   ordered_attempt_batch batch(base, requests);
   auto bad = retry; bad[0].transaction.transaction_id = requests[0].transaction_id;
   BOOST_CHECK_THROW(batch.prepare_retries(fresh, bad), primary_state_invalid_attempt);
   bad = retry; bad[0].transaction.execution_fingerprint = {};
   BOOST_CHECK_THROW(batch.prepare_retries(fresh, bad), primary_state_invalid_attempt);
   bad = retry; bad[0].index = 64;
   const std::array partially_valid{retry[0], bad[0]};
   BOOST_CHECK_THROW(batch.prepare_retries(fresh, partially_valid), primary_state_invalid_attempt);
   const std::array duplicate{retry[0], retry[0]};
   BOOST_CHECK_THROW(batch.prepare_retries(fresh, duplicate), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.prepare_retries(nullptr, retry), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(batch.prepare_retries(fresh, {}), primary_state_invalid_attempt);
   BOOST_CHECK(batch.get_ticket(1).base() == base); // no partial publication
   batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, output(0, a, false),
      [&](const auto&) { BOOST_CHECK_THROW(batch.prepare_retries(fresh, retry), primary_state_invalid_attempt); }, [] {});
   BOOST_CHECK_THROW(batch.prepare_retries(fresh, retry), primary_state_invalid_attempt);
   ordered_attempt_batch cancelled(base, requests); cancelled.discard_remaining();
   BOOST_CHECK_THROW(cancelled.prepare_retries(fresh, retry), primary_state_invalid_attempt);
   ordered_attempt_batch closed(base, requests); closed.close();
   BOOST_CHECK_THROW(closed.prepare_retries(fresh, retry), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(discovery_tickets_keep_owned_values_during_owner_replacement, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   const auto original = batch.get_ticket(0);
   const std::array keys{a, b};
   const auto fresh = frozen_primary_state::capture(view, keys);
   const std::array retry{ordered_attempt_batch::retry_request{0, requests[0]}};
   auto reader = std::async(std::launch::async, [&] {
      for (unsigned i = 0; i < 1000; ++i)
         if (original.base() != base || original.transaction_id() != requests[0].transaction_id ||
             !original.base()->locate_primary({a, primary_query_kind::find, 1}).row) return false;
      try { batch.prepare_retries(fresh, retry); }
      catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   for (unsigned i = 0; i < 20; ++i) batch.prepare_retries(fresh, retry);
   BOOST_CHECK(reader.get());
   BOOST_CHECK(original.base() == base); BOOST_CHECK(batch.get_ticket(0).base() == fresh);
   // Giving an old result a fresh ticket cannot turn it into a new proof.
   BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id,
      output(0, a, false), [](const auto&) {}, [] {}), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(discovery_retained_snapshot_budget_counts_shared_bases_once_and_rejects_growth_atomically, batch_fixture) {
   const std::vector<char> payload(2 * 1024 * 1024, 'x');
   for (const auto& row : db.get_index<key_value_index, by_scope_primary>())
      db.modify(row, [&](auto& value) { value.value.assign(payload.data(), payload.size()); });
   const std::array first_table{a};
   const std::array both_tables{a, b};
   const auto first = frozen_primary_state::capture(view, first_table);
   const auto both = frozen_primary_state::capture(view, both_tables);
   BOOST_CHECK_EQUAL(first->value_bytes(), payload.size());
   BOOST_CHECK_EQUAL(both->value_bytes(), 2 * payload.size());
   ordered_attempt_batch batch(first, requests);
   const std::array one{ordered_attempt_batch::retry_request{1, requests[1]}};
   BOOST_CHECK_THROW(batch.prepare_retries(both, one), primary_state_budget_exceeded);
   for (size_t i = 0; i < requests.size(); ++i) BOOST_CHECK(batch.get_ticket(i).base() == first);
   const std::array all{ordered_attempt_batch::retry_request{0, requests[0]},
      ordered_attempt_batch::retry_request{1, requests[1]}, ordered_attempt_batch::retry_request{2, requests[2]}};
   BOOST_CHECK_NO_THROW(batch.prepare_retries(both, all));
   for (size_t i = 0; i < requests.size(); ++i) BOOST_CHECK(batch.get_ticket(i).base() == both);
}

BOOST_FIXTURE_TEST_CASE(adopter_can_report_serial_fallback_and_moved_ticket_fails_safely, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   auto ticket = batch.get_ticket(0);
   auto moved = std::move(ticket);
   BOOST_CHECK_THROW(ticket.base(), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(ticket.stop_token(), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(ticket.transaction_id(), primary_state_invalid_attempt);
   batch.run_next<isolated_action_result>(moved, requests[0].transaction_id, output(0, a), [&](const auto&) { batch.discard_remaining(); }, [] {});
   bool serial = false;
   batch.run_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, output(1, b),
      [](const auto&) { BOOST_FAIL("canonical fallback must disable later adoption"); }, [&] { serial = true; });
   BOOST_CHECK(serial);
}

BOOST_FIXTURE_TEST_CASE(inspection_is_non_consuming_and_does_not_record_speculative_writes_or_metrics, batch_fixture) {
   using status = ordered_attempt_batch::candidate_status;
   auto& metrics = performance::metrics();
   struct restore_metrics {
      bool enabled;
      ~restore_metrics() { performance::metrics().enabled.store(enabled); }
   } restore{metrics.enabled.exchange(true)};
   const auto count = [&] { return metrics.snapshot(performance::stage::parallel_conflict).count; };
   const auto before = count();
   ordered_attempt_batch batch(base, requests);
   const auto first = batch.get_ticket(0);
   for (unsigned i = 0; i < 3; ++i) {
      BOOST_CHECK(batch.inspect_next<isolated_action_result>(first, requests[0].transaction_id, output(0, a)) == status::reusable);
      BOOST_CHECK_EQUAL(batch.next_index(), 0u);
      BOOST_CHECK(!first.stop_token().stop_requested());
   }
   // Inspecting a writer does not make it a committed writer. Commit a reader
   // instead; the next reader must remain reusable with its original snapshot.
   batch.run_next<isolated_action_result>(first, requests[0].transaction_id, output(0, a, false),
      [](const auto&) {}, [] { BOOST_FAIL("reader unexpectedly discarded"); });
   const auto second = batch.get_ticket(1);
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(second, requests[1].transaction_id, output(1, a)) == status::reusable);
   batch.run_next<isolated_action_result>(second, requests[1].transaction_id, output(1, a), [](const auto&) {}, [] {});
   const auto third = batch.get_ticket(2);
   for (unsigned i = 0; i < 3; ++i)
      BOOST_CHECK(batch.inspect_next<isolated_action_result>(third, requests[2].transaction_id, output(2, a)) == status::dependency_conflict);
   BOOST_CHECK_EQUAL(count(), before);
   BOOST_CHECK_EQUAL(batch.next_index(), 2u);
   BOOST_CHECK(!third.stop_token().stop_requested());
   batch.run_next<isolated_action_result>(third, requests[2].transaction_id, output(2, a),
      [](const auto&) { BOOST_FAIL("conflict adopted"); }, [] {});
   BOOST_CHECK_EQUAL(count(), before + 1);
   BOOST_CHECK(third.stop_token().stop_requested());
}

BOOST_FIXTURE_TEST_CASE(inspection_distinguishes_missing_over_budget_and_cancelled_results, batch_fixture) {
   using status = ordered_attempt_batch::candidate_status;
   ordered_attempt_batch batch(base, requests);
   const auto tag = batch.get_ticket(0);
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, {}) == status::unavailable);
   primary_overlay_limits limits; limits.max_write_operations = 4097;
   primary_state_overlay oversized(base, limits);
   for (size_t i = 0; i < 4097; ++i) oversized.update_existing(a, 1, name{}, std::span<const char>("z", 1));
   const isolated_action_result large{oversized.seal(), {}, requests[0].execution_fingerprint};
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, large) == status::unavailable);
   BOOST_CHECK(!tag.stop_token().stop_requested());
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, output(0, a), [&] {
      batch.discard_remaining();
   }) == status::unavailable);
   BOOST_CHECK_EQUAL(batch.next_index(), 0u);
   BOOST_CHECK(tag.stop_token().stop_requested());
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, output(0, b)) == status::unavailable);
}

BOOST_FIXTURE_TEST_CASE(inspection_checks_all_result_fingerprint_fields_and_snapshot_identity, batch_fixture) {
   auto check = [&]<typename Result>() {
      for (unsigned fault = 0; fault < 3; ++fault) {
         ordered_attempt_batch batch(base, requests);
         const auto tag = batch.get_ticket(0);
         auto candidate_base = base;
         if (fault == 2) {
            const std::array keys{a, b}; candidate_base = frozen_primary_state::capture(view, keys);
         }
         primary_state_overlay overlay(candidate_base); overlay.locate_primary({a, primary_query_kind::find, 1});
         Result candidate{overlay.seal()};
         const auto fingerprint = fault == 1 ? requests[1].execution_fingerprint : requests[0].execution_fingerprint;
         if constexpr (requires { candidate.root_fingerprint; }) candidate.root_fingerprint = fingerprint;
         else candidate.execution_fingerprint = fingerprint;
         if (fault == 0) {
            BOOST_CHECK(batch.inspect_next<Result>(tag, requests[0].transaction_id, candidate) == ordered_attempt_batch::candidate_status::reusable);
            BOOST_CHECK_EQUAL(batch.next_index(), 0u);
         } else {
            BOOST_CHECK_THROW(batch.inspect_next<Result>(tag, requests[0].transaction_id, candidate), primary_state_invalid_attempt);
            BOOST_CHECK(tag.stop_token().stop_requested());
            BOOST_CHECK_THROW(batch.get_ticket(0), primary_state_invalid_attempt);
         }
      }
   };
   check.template operator()<isolated_action_result>();
   check.template operator()<isolated_transaction_result>();
   check.template operator()<isolated_notification_result>();
   check.template operator()<isolated_action_tree_result>();
   check.template operator()<isolated_sync_result>();
   check.template operator()<isolated_sync_transaction_result>();
   check.template operator()<isolated_mixed_tree_result>();
}

BOOST_FIXTURE_TEST_CASE(inspection_rejects_wrong_callers_and_guards_checkpoint_reentry_and_exceptions, batch_fixture) {
   ordered_attempt_batch batch(base, requests), other(base, requests);
   const auto tag = batch.get_ticket(0);
   const auto result = output(0, a);
   BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(other.get_ticket(0), requests[0].transaction_id, result), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(batch.get_ticket(1), requests[1].transaction_id, result), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(tag, requests[1].transaction_id, result), primary_state_invalid_attempt);
   auto foreign = std::async(std::launch::async, [&] {
      try { batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, result); }
      catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(foreign.get());
   unsigned checks = 0;
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, result, [&] {
      ++checks;
      BOOST_CHECK_THROW(batch.close(), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, result), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(tag, requests[0].transaction_id, result, [](const auto&) {}, [] {}), primary_state_invalid_attempt);
      const std::array retries{ordered_attempt_batch::retry_request{0, requests[0]}};
      BOOST_CHECK_THROW(batch.prepare_retries(base, retries), primary_state_invalid_attempt);
   }) == ordered_attempt_batch::candidate_status::reusable);
   BOOST_CHECK_GT(checks, 0u);
   BOOST_CHECK_EQUAL(batch.next_index(), 0u);
   BOOST_CHECK(!tag.stop_token().stop_requested());
   BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(tag, requests[0].transaction_id, result, [] { throw std::bad_alloc{}; }), std::bad_alloc);
   BOOST_CHECK(tag.stop_token().stop_requested());
   BOOST_CHECK_THROW(batch.get_ticket(0), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(current_prefix_refresh_preserves_chained_conflicts_and_independent_tail, batch_fixture) {
   using status = ordered_attempt_batch::candidate_status;
   const std::array sequence{requests[0], requests[1], requests[2], ordered_attempt_batch::request{digest("tx3"), digest("execution3")}};
   ordered_attempt_batch batch(base, sequence);
   const auto old_second = batch.get_ticket(1), old_third = batch.get_ticket(2), tail = batch.get_ticket(3);
   const auto second_result = output(1, a, false), third_result = output(2, b, false);
   primary_state_overlay independent(base); independent.locate_primary({absent, primary_query_kind::find, 1});
   const isolated_action_result fourth{independent.seal(), {}, sequence[3].execution_fingerprint};
   // Synthetic owner commits for coordinator routing only, NOT canonical
   // adoption. Controller/real-WASM integration is a separate N3 stage.
   auto put = [&](table_key key, const char* bytes) {
      const auto& table = db.get<table_id_object, by_code_scope_table>(boost::make_tuple(key.code, key.scope, key.table));
      const auto& row = db.get<key_value_object, by_scope_primary>(boost::make_tuple(table.id, uint64_t{1}));
      db.modify(row, [&](auto& value) { value.value.assign(bytes, 1); });
   };
   batch.run_next<isolated_action_result>(batch.get_ticket(0), sequence[0].transaction_id, output(0, a),
      [&](const auto&) { put(a, "y"); }, [] { BOOST_FAIL("first writer lost"); });
   const std::array keys{a, b, absent};
   for (size_t index : {1u, 2u}) {
      const auto old = index == 1 ? old_second : old_third;
      const auto& outdated = index == 1 ? second_result : third_result;
      BOOST_CHECK(batch.inspect_next<isolated_action_result>(old, sequence[index].transaction_id, outdated) == status::dependency_conflict);
      auto changed = sequence[index]; changed.execution_fingerprint = digest(index == 1 ? "fresh1" : "fresh2");
      const auto fresh = batch.refresh_next(old, changed.transaction_id, [&] {
         return ordered_attempt_batch::refreshed_input{frozen_primary_state::capture(view, keys), changed};
      });
      BOOST_CHECK(old.base() == base);
      BOOST_CHECK(fresh.base() != base);
      BOOST_CHECK(tail.base() == base);
      BOOST_CHECK_EQUAL(batch.next_index(), index);
      BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(old, changed.transaction_id, outdated), primary_state_invalid_attempt);
      primary_state_overlay retried(fresh.base());
      const auto selected = retried.read_primary({index == 1 ? a : b, primary_query_kind::find, 1}, 1);
      BOOST_REQUIRE(selected.row);
      BOOST_CHECK(selected.row->value == std::vector<char>{'y'});
      retried.update_existing(index == 1 ? b : a, 1, name{}, std::span<const char>("y", 1));
      const isolated_action_result candidate{retried.seal(), {}, changed.execution_fingerprint};
      BOOST_CHECK(batch.inspect_next<isolated_action_result>(fresh, changed.transaction_id, candidate) == status::reusable);
      batch.run_next<isolated_action_result>(fresh, changed.transaction_id, candidate,
         [&](const auto&) { put(index == 1 ? b : a, "y"); }, [] { BOOST_FAIL("fresh current-prefix result lost"); });
   }
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tail, sequence[3].transaction_id, fourth) == status::reusable);
   batch.run_next<isolated_action_result>(tail, sequence[3].transaction_id, fourth, [](const auto&) {}, [] { BOOST_FAIL("independent tail lost"); });
   BOOST_CHECK(batch.complete());
   BOOST_CHECK(!tail.stop_token().stop_requested());
}

BOOST_FIXTURE_TEST_CASE(refresh_validation_and_budget_failures_leave_the_old_descriptor_intact, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, output(0, a, false), [](const auto&) {}, [] {});
   const auto old = batch.get_ticket(1);
   const std::array keys{a, b};
   for (unsigned fault = 0; fault < 4; ++fault) {
      auto capture = [&] {
         auto input = ordered_attempt_batch::refreshed_input{frozen_primary_state::capture(view, keys), requests[1]};
         if (fault == 0) input.transaction.transaction_id = requests[0].transaction_id;
         if (fault == 1) input.transaction.execution_fingerprint = {};
         if (fault == 2) input.base = base;
         if (fault == 3) input.base.reset();
         return input;
      };
      if (fault == 3) BOOST_CHECK_THROW(batch.refresh_next(old, requests[1].transaction_id, capture), primary_state_budget_exceeded);
      else BOOST_CHECK_THROW(batch.refresh_next(old, requests[1].transaction_id, capture), primary_state_invalid_attempt);
      BOOST_CHECK(batch.get_ticket(1).base() == base);
      BOOST_CHECK(!old.stop_token().stop_requested());
      BOOST_CHECK(batch.inspect_next<isolated_action_result>(old, requests[1].transaction_id, output(1, b)) == ordered_attempt_batch::candidate_status::reusable);
   }
   const std::vector<char> payload(2 * 1024 * 1024, 'x');
   for (const auto& row : db.get_index<key_value_index, by_scope_primary>())
      db.modify(row, [&](auto& value) { value.value.assign(payload.data(), payload.size()); });
   BOOST_CHECK_THROW(batch.refresh_next(old, requests[1].transaction_id, [&] {
      return ordered_attempt_batch::refreshed_input{frozen_primary_state::capture(view, keys), requests[1]};
   }), primary_state_budget_exceeded); // 4 MiB new + original retained snapshot
   BOOST_CHECK(batch.get_ticket(1).base() == base);
   BOOST_CHECK_EQUAL(batch.next_index(), 1u);
   batch.run_next<isolated_action_result>(old, requests[1].transaction_id, {}, [](const auto&) {}, [] {});
   BOOST_CHECK(old.stop_token().stop_requested());
}

BOOST_FIXTURE_TEST_CASE(refresh_cannot_retag_old_results_or_refresh_future_or_stopped_slots, batch_fixture) {
   for (unsigned scenario = 0; scenario < 6; ++scenario) {
      ordered_attempt_batch batch(base, requests);
      const auto first = batch.get_ticket(0), next = batch.get_ticket(1), tail = batch.get_ticket(2);
      unsigned captures = 0;
      const std::array keys{a, b};
      auto capture = [&] {
         ++captures;
         return ordered_attempt_batch::refreshed_input{frozen_primary_state::capture(view, keys), requests[1]};
      };
      BOOST_CHECK_THROW(batch.refresh_next(first, requests[0].transaction_id, capture), primary_state_invalid_attempt);
      batch.run_next<isolated_action_result>(first, requests[0].transaction_id, output(0, a, false), [](const auto&) {}, [] {});
      BOOST_CHECK_THROW(batch.refresh_next(tail, requests[2].transaction_id, capture), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.refresh_next(next, requests[1].transaction_id, {}), primary_state_invalid_attempt);
      BOOST_CHECK_EQUAL(captures, 0u);
      if (scenario == 0) {
         const auto fresh = batch.refresh_next(next, requests[1].transaction_id, capture);
         BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(fresh, requests[1].transaction_id, output(1, b)), primary_state_invalid_attempt);
      } else if (scenario == 1) {
         batch.discard_remaining();
         BOOST_CHECK_THROW(batch.refresh_next(next, requests[1].transaction_id, capture), primary_state_invalid_attempt);
      } else if (scenario == 2) {
         batch.close();
         BOOST_CHECK_THROW(batch.refresh_next(next, requests[1].transaction_id, capture), primary_state_invalid_attempt);
      } else if (scenario == 3) {
         BOOST_CHECK_THROW(batch.refresh_next(next, requests[1].transaction_id, [&]() -> ordered_attempt_batch::refreshed_input {
            throw std::bad_alloc{};
         }), std::bad_alloc);
      } else if (scenario == 4) {
         BOOST_CHECK_THROW(batch.refresh_next(next, requests[1].transaction_id, [&] {
            batch.discard_remaining(); return capture();
         }), primary_state_invalid_attempt);
         BOOST_CHECK(batch.get_ticket(1).base() == base);
      } else {
         auto foreign = std::async(std::launch::async, [&] {
            try { batch.refresh_next(next, requests[1].transaction_id, capture); }
            catch (const state_view_wrong_thread&) { return true; }
            return false;
         });
         BOOST_CHECK(foreign.get());
         BOOST_CHECK_EQUAL(captures, 0u);
         BOOST_CHECK(!next.stop_token().stop_requested());
         continue;
      }
      BOOST_CHECK(next.stop_token().stop_requested());
      BOOST_CHECK_EQUAL(batch.next_index(), 1u);
   }
}

BOOST_FIXTURE_TEST_CASE(refresh_callback_is_guarded_and_total_replacements_are_bounded, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, output(0, a, false), [](const auto&) {}, [] {});
   auto tag = batch.get_ticket(1);
   const auto old = tag;
   const std::array keys{a, b};
   unsigned captures = 0;
   auto capture = [&] {
      ++captures;
      BOOST_CHECK_THROW(batch.close(), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.inspect_next<isolated_action_result>(tag, requests[1].transaction_id, output(1, b)), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.run_next<isolated_action_result>(tag, requests[1].transaction_id, output(1, b), [](const auto&) {}, [] {}), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.refresh_next(tag, requests[1].transaction_id, {}), primary_state_invalid_attempt);
      return ordered_attempt_batch::refreshed_input{frozen_primary_state::capture(view, keys), requests[1]};
   };
   for (size_t i = 0; i < parallel_runtime_limits::max_discovery_rounds; ++i)
      tag = batch.refresh_next(tag, requests[1].transaction_id, capture);
   BOOST_CHECK_EQUAL(captures, parallel_runtime_limits::max_discovery_rounds);
   BOOST_CHECK_THROW(batch.refresh_next(tag, requests[1].transaction_id, capture), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(captures, parallel_runtime_limits::max_discovery_rounds);
   BOOST_CHECK(old.base() == base);
   BOOST_CHECK(!tag.stop_token().stop_requested());
   primary_state_overlay overlay(tag.base()); overlay.locate_primary({a, primary_query_kind::find, 1});
   const isolated_action_result candidate{overlay.seal(), {}, requests[1].execution_fingerprint};
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[1].transaction_id, candidate) == ordered_attempt_batch::candidate_status::reusable);
   BOOST_CHECK(batch.inspect_next<isolated_action_result>(tag, requests[1].transaction_id, candidate, [&] { batch.discard_remaining(); }) == ordered_attempt_batch::candidate_status::unavailable);
}

BOOST_FIXTURE_TEST_CASE(refreshed_secondary_reads_do_not_clear_old_tail_conflicts_in_any_index_family, secondary_batch_fixture) {
   using status = ordered_attempt_batch::candidate_status;
   for (unsigned family = 0; family < 5; ++family) {
      const secondary_query query{a, secondary_query_kind::lower_bound, batch_key(family, 11), 0};
      capture(query);
      ordered_attempt_batch batch(base, requests);
      primary_state_overlay prefix(base), old_reader(base);
      prefix.update_secondary(a, 3, name{}, batch_key(family, 15));
      prefix.update_secondary(a, 3, name{}, batch_key(family, 30)); // ABA remains dirty
      old_reader.read_secondary(query);
      const isolated_notification_result first{prefix.seal(), {}, requests[0].execution_fingerprint};
      const auto stale_state = old_reader.seal();
      const isolated_notification_result second{stale_state, {}, requests[1].execution_fingerprint};
      const isolated_notification_result third{stale_state, {}, requests[2].execution_fingerprint};
      batch.run_next<isolated_notification_result>(batch.get_ticket(0), requests[0].transaction_id, first, [](const auto&) {}, [] {});
      // Synthetic ABA has restored the same values. A NEW owner capture can
      // legitimately read them, but old tail snapshots remain conservative.
      const auto previous = batch.get_ticket(1), tail = batch.get_ticket(2);
      BOOST_CHECK(batch.inspect_next<isolated_notification_result>(previous, requests[1].transaction_id, second) == status::dependency_conflict);
      const auto fresh = batch.refresh_next(previous, requests[1].transaction_id, [&] {
         capture(query);
         return ordered_attempt_batch::refreshed_input{base, requests[1]};
      });
      primary_state_overlay reader(fresh.base()); reader.read_secondary(query);
      const isolated_notification_result current{reader.seal(), {}, requests[1].execution_fingerprint};
      BOOST_CHECK(batch.inspect_next<isolated_notification_result>(fresh, requests[1].transaction_id, current) == status::reusable);
      batch.run_next<isolated_notification_result>(fresh, requests[1].transaction_id, current, [](const auto&) {}, [] { BOOST_FAIL("fresh secondary read lost"); });
      BOOST_CHECK(batch.inspect_next<isolated_notification_result>(tail, requests[2].transaction_id, third) == status::dependency_conflict);
      batch.run_next<isolated_notification_result>(tail, requests[2].transaction_id, third,
         [](const auto&) { BOOST_FAIL("refresh cleared old secondary dependencies"); }, [] {});
      BOOST_CHECK(tail.stop_token().stop_requested());
   }
}

BOOST_FIXTURE_TEST_CASE(refreshed_structure_keeps_old_tail_barrier_and_legacy_secondary_guard, secondary_batch_fixture) {
   const secondary_query query{a, secondary_query_kind::find_primary, batch_key(0, 0), 1}; capture(query);
   for (bool legacy : {false, true}) {
      ordered_attempt_batch batch(base, requests);
      primary_state_overlay structural(base), old_reader(base);
      structural.remove(a, 1); structural.store(a, 1, "alice"_n, std::span<const char>("x", 1));
      old_reader.read_table(a);
      const isolated_action_result first{structural.seal(), {}, requests[0].execution_fingerprint};
      const isolated_action_result third{old_reader.seal(), {}, requests[2].execution_fingerprint};
      batch.run_next<isolated_action_result>(batch.get_ticket(0), requests[0].transaction_id, first, [](const auto&) {}, [] {});
      const auto tail = batch.get_ticket(2);
      const auto fresh = batch.refresh_next(batch.get_ticket(1), requests[1].transaction_id, [&] {
         capture(query); return ordered_attempt_batch::refreshed_input{base, requests[1]};
      });
      primary_state_overlay current(fresh.base()); current.read_secondary(query);
      if (legacy) {
         const isolated_action_result result{current.seal(), {}, requests[1].execution_fingerprint};
         BOOST_CHECK(batch.inspect_next<isolated_action_result>(fresh, requests[1].transaction_id, result) == ordered_attempt_batch::candidate_status::unavailable);
      } else {
         const isolated_notification_result result{current.seal(), {}, requests[1].execution_fingerprint};
         BOOST_CHECK(batch.inspect_next<isolated_notification_result>(fresh, requests[1].transaction_id, result) == ordered_attempt_batch::candidate_status::reusable);
         batch.run_next<isolated_notification_result>(fresh, requests[1].transaction_id, result, [](const auto&) {}, [] {});
         BOOST_CHECK(batch.inspect_next<isolated_action_result>(tail, requests[2].transaction_id, third) == ordered_attempt_batch::candidate_status::dependency_conflict);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
