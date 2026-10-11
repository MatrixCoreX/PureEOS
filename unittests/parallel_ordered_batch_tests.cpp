#include <eosio/chain/parallel_ordered_batch.hpp>
#include <eosio/chain/contract_table_objects.hpp>
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
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_ordered_batch_tests)

BOOST_FIXTURE_TEST_CASE(independent_tables_adopt_in_original_order_and_own_result, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   std::vector<size_t> seen;
   for (size_t i = 0; i < 3; ++i) {
      const auto result = output(i, i == 0 ? a : (i == 1 ? b : absent), i != 2);
      auto adopted = batch.run_next(batch.get_ticket(i), requests[i].transaction_id, result,
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
      batch.run_next(batch.get_ticket(i), requests[i].transaction_id, result,
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
      batch.run_next(batch.get_ticket(0), requests[0].transaction_id, first_result, [](const auto&) {}, [] {});
      bool serial = false;
      batch.run_next(batch.get_ticket(1), requests[1].transaction_id, second_result,
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
      batch.run_next(batch.get_ticket(0), requests[0].transaction_id, output(0, a), [](const auto&) {}, [] {});
      bool adopted = false;
      batch.run_next(batch.get_ticket(1), requests[1].transaction_id, result,
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
      batch.run_next(batch.get_ticket(0), requests[0].transaction_id, first_result, [](const auto&) {}, [] { BOOST_FAIL("first"); });
      const auto used = batch.run_next(batch.get_ticket(1), requests[1].transaction_id, second_result,
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
      batch.run_next(batch.get_ticket(i), requests[i].transaction_id, result,
         [&](const auto&) { ++adopted; }, [&] { ++serial; });
   }
   BOOST_CHECK_EQUAL(adopted, 2u); BOOST_CHECK_EQUAL(serial, 1u);
}

BOOST_FIXTURE_TEST_CASE(no_attempt_is_a_whole_batch_serial_barrier, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   size_t serial = 0;
   batch.run_next(batch.get_ticket(0), requests[0].transaction_id, std::nullopt,
      [](const auto&) { BOOST_FAIL("no result must not be adopted"); }, [&] { ++serial; });
   for (size_t i = 1; i < requests.size(); ++i)
      batch.run_next(batch.get_ticket(i), requests[i].transaction_id, output(i, b),
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
   BOOST_CHECK_THROW(batch.run_next(other.get_ticket(0), requests[0].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.run_next(batch.get_ticket(1), requests[1].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(batch.run_next(first, requests[1].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(calls, 0u); BOOST_CHECK_EQUAL(batch.next_index(), 0u);
   batch.run_next(first, requests[0].transaction_id, attempt, adopt, serial);
   BOOST_CHECK_THROW(batch.run_next(first, requests[0].transaction_id, attempt, adopt, serial), primary_state_invalid_attempt);
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
      BOOST_CHECK_THROW(batch.run_next(ticket, requests[0].transaction_id, attempt,
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
      BOOST_CHECK_THROW(batch.run_next(ticket, requests[0].transaction_id, attempt,
         [&](const auto&) { ++mutations; throw std::runtime_error("adopt failure"); },
         [&] { ++mutations; throw std::runtime_error("serial failure"); }), std::runtime_error);
      BOOST_CHECK_EQUAL(mutations, 1u); BOOST_CHECK_EQUAL(batch.next_index(), 0u);
      BOOST_CHECK(ticket.stop_token().stop_requested());
      BOOST_CHECK_THROW(batch.get_ticket(0), primary_state_invalid_attempt);
   }
}

BOOST_FIXTURE_TEST_CASE(reentrancy_and_foreign_thread_cannot_advance_or_close_batch, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   auto ticket = batch.get_ticket(1);
   auto result = output(1, b);
   batch.run_next(batch.get_ticket(0), requests[0].transaction_id, output(0, a), [&](const auto&) {
      BOOST_CHECK_THROW(batch.close(), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(batch.run_next(ticket, requests[1].transaction_id, result, [](const auto&) {}, [] {}), primary_state_invalid_attempt);
   }, [] { BOOST_FAIL("unexpected fallback"); });
   auto worker = std::async(std::launch::async, [&] {
      unsigned rejected = 0;
      try { (void)batch.next_index(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { batch.close(); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { batch.run_next(ticket, requests[1].transaction_id, result, [](const auto&) {}, [] {}); }
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
   BOOST_CHECK_THROW(replacement.run_next(old, requests[0].transaction_id, output(0, a),
      [](const auto&) {}, [] {}), primary_state_invalid_attempt);
   replacement.close();
   BOOST_CHECK(!replacement.complete());
   BOOST_CHECK_THROW(replacement.get_ticket(0), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(batch_size_and_identity_budgets_fail_before_publication, batch_fixture) {
   BOOST_CHECK_THROW(ordered_attempt_batch(nullptr, requests), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(ordered_attempt_batch(base, {}), primary_state_budget_exceeded);
   std::vector<ordered_attempt_batch::request> large(65, requests[0]);
   BOOST_CHECK_THROW(ordered_attempt_batch(base, large), primary_state_budget_exceeded);
   auto invalid = requests; invalid[0].transaction_id = {};
   BOOST_CHECK_THROW(ordered_attempt_batch(base, invalid), primary_state_invalid_attempt);
   invalid = requests; invalid[0].execution_fingerprint = {};
   BOOST_CHECK_THROW(ordered_attempt_batch(base, invalid), primary_state_invalid_attempt);
   ordered_attempt_batch batch(base, requests);
   BOOST_CHECK_THROW(batch.get_ticket(requests.size()), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(adopter_can_report_serial_fallback_and_moved_ticket_fails_safely, batch_fixture) {
   ordered_attempt_batch batch(base, requests);
   auto ticket = batch.get_ticket(0);
   auto moved = std::move(ticket);
   BOOST_CHECK_THROW(ticket.base(), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(ticket.stop_token(), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(ticket.transaction_id(), primary_state_invalid_attempt);
   batch.run_next(moved, requests[0].transaction_id, output(0, a), [&](const auto&) { batch.discard_remaining(); }, [] {});
   bool serial = false;
   batch.run_next(batch.get_ticket(1), requests[1].transaction_id, output(1, b),
      [](const auto&) { BOOST_FAIL("canonical fallback must disable later adoption"); }, [&] { serial = true; });
   BOOST_CHECK(serial);
}

BOOST_AUTO_TEST_SUITE_END()
