#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <test_contracts.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <limits>
#include <random>
#include <type_traits>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
using namespace eosio::testing;

namespace {
template<class Key>
Key original_secondary_number(uint64_t n) {
   if constexpr (std::is_same_v<Key, float64_t>) return ui64_to_f64(n);
   else if constexpr (std::is_same_v<Key, float128_t>) {
      float128_t result; ui64_to_f128M(n, &result); return result;
   }
   else if constexpr (std::is_same_v<Key, key256_t>) return {uint128_t{n}, uint128_t{n + 1}};
   else return Key{n};
}
template<class Key>
secondary_key owned_secondary_key(const Key& key) {
   if constexpr (std::is_same_v<Key, float64_t>) return secondary_float64{key.v};
   else if constexpr (std::is_same_v<Key, float128_t>) {
      uint128_t bits; fc::float128_to_uint128(key, bits); return secondary_float128{bits};
   } else return key;
}
template<class Key>
decltype(auto) original_secondary_argument(Key& key) {
   if constexpr (std::is_same_v<Key, key256_t>) return key.data();
   else return (key);
}

// Exercise the ACTUAL original generic_index, not the scan oracle or an
// independently rewritten cursor. The immutable snapshot is captured before
// either implementation changes rows. This is host-level semantics only;
// pointer conversion/aliasing and accepted WASM transactions are separate tests.
template<class Index>
void compare_secondary_cursor_with_original(apply_context& context, Index& original, Index& setup,
                                            const table_key& rows) {
   using key_type = typename Index::secondary_key_type;
   const auto number = [](uint64_t n) { return original_secondary_number<key_type>(n); };
   const auto owned = [&](uint64_t n) { return owned_secondary_key(number(n)); };
   const std::array primary{uint64_t{0}, uint64_t{7}, uint64_t{19}, std::numeric_limits<uint64_t>::max()};
   const std::array original_values{10u, 20u, 20u, 30u};
   for (size_t i = 0; i < primary.size(); ++i) {
      auto value = number(original_values[i]);
      setup.store(rows.scope.to_uint64_t(), rows.table.to_uint64_t(), "alice"_n,
                  primary[i], original_secondary_argument(value));
   }
   const table_key empty{rows.code, "empty"_n, rows.table}, missing{rows.code, "missing"_n, rows.table};
   // A primary row makes the shared table exist while this secondary index is empty.
   context.db_store_i64(empty.scope, empty.table, "alice"_n, 0, "x", 1);
   const std::array tables{rows, empty, missing};
   const std::array values{0u, 5u, 10u, 20u, 30u, 40u, 50u};
   const std::array query_primary{uint64_t{0}, uint64_t{7}, uint64_t{19}, uint64_t{99},
                                 std::numeric_limits<uint64_t>::max()};
   std::vector<secondary_query> queries;
   // Fixed, finite capture avoids learning a new dependency from the already
   // mutated original database. Unknown queries must still throw, not infer.
   for (const auto& table : tables) {
      for (auto p : query_primary) queries.push_back({table, secondary_query_kind::find_primary, owned(0), p});
      queries.push_back({table, secondary_query_kind::first, owned(0), 0});
      queries.push_back({table, secondary_query_kind::last, owned(0), 0});
      for (auto n : values) {
         for (auto kind : {secondary_query_kind::find_secondary, secondary_query_kind::lower_bound,
                           secondary_query_kind::upper_bound})
            queries.push_back({table, kind, owned(n), 0});
         for (auto p : query_primary) for (auto kind : {secondary_query_kind::next, secondary_query_kind::previous})
            queries.push_back({table, kind, owned(n), p});
      }
   }
   chainbase_transaction_state_view live(context.db);
   const auto base = frozen_primary_state::capture_queries_with_secondary(live, tables, {}, queries);
   primary_state_overlay state(base);
   secondary_overlay_cursor cursor(state, rows.code, secondary_kind(owned(0)));
   const auto find = [&](uint64_t p) {
      auto expected_key = number(50); secondary_key actual_key = owned_secondary_key(expected_key);
      const auto expected = original.find_primary(rows.code.to_uint64_t(), rows.scope.to_uint64_t(),
         rows.table.to_uint64_t(), original_secondary_argument(expected_key), p);
      const auto actual = cursor.find_primary(rows, actual_key, p);
      BOOST_CHECK_EQUAL(actual, expected); BOOST_CHECK(actual_key == owned_secondary_key(expected_key));
      return actual;
   };
   const auto adjacent = [&](int iterator) {
      uint64_t expected = 1234, actual = expected;
      BOOST_CHECK_EQUAL(cursor.next(iterator, actual), original.next_secondary(iterator, expected));
      BOOST_CHECK_EQUAL(actual, expected);
      if (iterator != -1) {
         expected = actual = 1234;
         BOOST_CHECK_EQUAL(cursor.previous(iterator, actual), original.previous_secondary(iterator, expected));
         BOOST_CHECK_EQUAL(actual, expected);
      }
   };
   const auto ranges = [&] {
      for (const auto& table : tables) {
         const auto end = cursor.end(table);
         BOOST_CHECK_EQUAL(end, original.end_secondary(table.code.to_uint64_t(), table.scope.to_uint64_t(),
                                                       table.table.to_uint64_t()));
         if (end < -1) adjacent(end);
         for (auto n : values) for (unsigned operation = 0; operation < 3; ++operation) {
            auto expected_key = number(n); secondary_key actual_key = owned_secondary_key(expected_key);
            uint64_t expected_primary = 4321, actual_primary = expected_primary;
            int expected = -1, actual = -1;
            const auto c = table.code.to_uint64_t(), s = table.scope.to_uint64_t(), t = table.table.to_uint64_t();
            if (operation == 0) {
               expected = original.find_secondary(c, s, t, original_secondary_argument(expected_key), expected_primary);
               actual = cursor.find_secondary(table, actual_key, actual_primary);
            } else if (operation == 1) {
               expected = original.lowerbound_secondary(c, s, t, original_secondary_argument(expected_key), expected_primary);
               actual = cursor.lower_bound(table, actual_key, actual_primary);
            } else {
               expected = original.upperbound_secondary(c, s, t, original_secondary_argument(expected_key), expected_primary);
               actual = cursor.upper_bound(table, actual_key, actual_primary);
            }
            BOOST_CHECK_EQUAL(actual, expected); BOOST_CHECK_EQUAL(actual_primary, expected_primary);
            BOOST_CHECK(actual_key == owned_secondary_key(expected_key));
            if (actual >= 0) adjacent(actual);
         }
      }
   };
   for (auto p : primary) adjacent(find(p));
   BOOST_CHECK_LT(find(99), 0);
   ranges();
   // Reuse the SAME handle after repeatedly moving its key, including ties.
   const auto moving = find(7);
   for (auto n : {40u, 5u, 20u, 0u, 50u, 10u}) {
      auto key = number(n);
      const name payer = n == 5 ? rows.code : name{};
      cursor.update(moving, payer, owned_secondary_key(key));
      original.update(moving, payer, original_secondary_argument(key));
      adjacent(moving); ranges();
   }
   // Another notification receiver shares caches but has no write authority;
   // reading another receiver's table must still preserve original handles.
   context.receiver = "alice"_n; cursor.set_receiver_for_notification("alice"_n);
   ranges();
   context.receiver = rows.code; cursor.set_receiver_for_notification(rows.code);
   const auto removed = find(19);
   cursor.remove(removed); original.remove(removed);
   BOOST_CHECK_LT(find(19), 0); ranges();
   auto recreated_key = number(5);
   const auto recreated = cursor.store(rows, 19, "alice"_n, owned_secondary_key(recreated_key));
   BOOST_CHECK_EQUAL(recreated, original.store(rows.scope.to_uint64_t(), rows.table.to_uint64_t(), "alice"_n,
                                               19, original_secondary_argument(recreated_key)));
   BOOST_CHECK_NE(recreated, removed); adjacent(recreated); ranges();
   // Absent -> secondary-only table -> absent -> new table identity.
   for (unsigned generation = 0; generation < 2; ++generation) {
      auto key = number(20);
      const auto handle = cursor.store(missing, 99, "alice"_n, owned_secondary_key(key));
      BOOST_CHECK_EQUAL(handle, original.store(missing.scope.to_uint64_t(), missing.table.to_uint64_t(), "alice"_n,
                                               99, original_secondary_argument(key)));
      ranges();
      cursor.remove(handle); original.remove(handle);
      BOOST_CHECK_EQUAL(cursor.end(missing), original.end_secondary(missing.code.to_uint64_t(),
                         missing.scope.to_uint64_t(), missing.table.to_uint64_t()));
   }
   // Compare every current row and the shared table metadata, not only cursors.
   for (auto p : query_primary) {
      const secondary_query q{rows, secondary_query_kind::find_primary, owned(0), p};
      const auto actual = state.read_secondary(q), expected = live.read_secondary(q);
      BOOST_REQUIRE(actual.table && expected.table);
      BOOST_CHECK(*actual.table == *expected.table);
      BOOST_REQUIRE_EQUAL(bool(actual.row), bool(expected.row));
      if (actual.row) {
         BOOST_CHECK_EQUAL(actual.row->primary_key, expected.row->primary_key);
         BOOST_CHECK(actual.row->key == expected.row->key); BOOST_CHECK(actual.row->payer == expected.row->payer);
         // New private object IDs are deliberately not canonical IDs.
         if (p != 19) BOOST_CHECK_EQUAL(actual.row->object_id, expected.row->object_id);
      }
   }
   const auto result = state.seal();
   BOOST_CHECK(!result.primary_only()); BOOST_CHECK_EQUAL(result.writes().size(), 12u);
}

struct trace_summary {
   std::vector<char> transaction_id;
   std::vector<char> receipt;
   std::vector<std::vector<char>> action_receipts;
   std::vector<std::vector<char>> return_values;
   std::vector<std::vector<char>> ram_deltas;
   bool operator==(const trace_summary&) const = default;
};

trace_summary summarize(const transaction_trace_ptr& trace) {
   trace_summary result;
   result.transaction_id = fc::raw::pack(trace->id);
   if (trace->receipt) result.receipt = fc::raw::pack(*trace->receipt);
   for (const auto& action : trace->action_traces) {
      result.action_receipts.push_back(action.receipt ? fc::raw::pack(*action.receipt) : std::vector<char>{});
      result.return_values.push_back(action.return_value);
      result.ram_deltas.push_back(fc::raw::pack(action.account_ram_deltas));
   }
   return result;
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_serial_reference_tests)

BOOST_AUTO_TEST_CASE(all_secondary_cursors_match_original_hosts_through_key_moves_payers_and_recreation) {
   tester chain;
   chain.create_accounts({"token"_n, "alice"_n});
   const auto before = chain.control->calculate_integrity_hash();
   signed_transaction trx;
   action act; act.account = "token"_n; act.name = "probe"_n;
   act.authorization = {{"alice"_n, config::active_name}};
   trx.actions.push_back(act);
   packed_transaction packed(trx, packed_transaction::compression_type::none);
   platform_timer timer;
   transaction_context transaction(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
      action_digests_t::store_which_t::both, fc::time_point::now(), transaction_metadata::trx_type::input);
   transaction.trace->action_traces.emplace_back(*transaction.trace, act, "token"_n, false, 1, 0, 0);
   apply_context setup(*chain.control, transaction, 1), original(*chain.control, transaction, 1);
   compare_secondary_cursor_with_original(original, original.idx64, setup.idx64,
                                          {"token"_n, "alice"_n, "sec.a"_n});
   compare_secondary_cursor_with_original(original, original.idx128, setup.idx128,
                                          {"token"_n, "alice"_n, "sec.b"_n});
   compare_secondary_cursor_with_original(original, original.idx256, setup.idx256,
                                          {"token"_n, "alice"_n, "sec.c"_n});
   compare_secondary_cursor_with_original(original, original.idx_double, setup.idx_double,
                                          {"token"_n, "alice"_n, "sec.d"_n});
   compare_secondary_cursor_with_original(original, original.idx_long_double, setup.idx_long_double,
                                          {"token"_n, "alice"_n, "sec.e"_n});
   transaction.undo();
   BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
}

// Exercise the original host implementation, not a second implementation of
// its expected values. No WASM execution or parallel adoption is claimed here.
BOOST_AUTO_TEST_CASE(primary_cursor_matches_original_host_read_and_update) {
   tester chain;
   chain.create_accounts({"token"_n, "alice"_n});
   const auto original_hash = chain.control->calculate_integrity_hash();
   signed_transaction signed_trx;
   action act;
   act.account = "token"_n; act.name = "probe"_n;
   act.authorization = {{"alice"_n, config::active_name}};
   signed_trx.actions.push_back(act);
   packed_transaction packed(signed_trx, packed_transaction::compression_type::none);
   platform_timer timer;
   transaction_context transaction(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
      action_digests_t::store_which_t::both, fc::time_point::now(), transaction_metadata::trx_type::input);
   // A host-only fixture, not an accepted transaction: populate its public trace
   // directly without relaxing private controller/transaction APIs for tests.
   transaction.trace->action_traces.emplace_back(*transaction.trace, act, "token"_n, false, 1, 0, 0);
   apply_context original(*chain.control, transaction, 1);
   auto& db = original.db;
   const std::array keys{
      table_key{"token"_n, "alice"_n, "accounts"_n},
      table_key{"token"_n, "bob"_n, "accounts"_n},
      table_key{"token"_n, "empty"_n, "accounts"_n},
      table_key{"token"_n, "missing"_n, "accounts"_n}};
   const std::array primary_keys{uint64_t{0}, uint64_t{7}, uint64_t{19}, std::numeric_limits<uint64_t>::max()};
   for (size_t t = 0; t < 3; ++t) {
      const auto& table = db.create<table_id_object>([&](auto& row) {
         row.code = keys[t].code; row.scope = keys[t].scope; row.table = keys[t].table;
         row.payer = "alice"_n;
      });
      if (t == 2) continue;
      for (auto primary : primary_keys) {
         db.create<key_value_object>([&](auto& row) {
            row.t_id = table.id; row.primary_key = primary; row.payer = "alice"_n;
            const std::array<char, 4> bytes{char(t), '\0', char(primary), '\x7f'};
            row.value.assign(bytes.data(), bytes.size());
         });
         db.modify(table, [](auto& row) { ++row.count; });
      }
   }
   chainbase_transaction_state_view live(db);
   primary_state_overlay isolated(frozen_primary_state::capture(live, keys));
   primary_overlay_cursor cursor(isolated, "token"_n);
   for (const auto& key : keys) {
      BOOST_CHECK_EQUAL(cursor.end(key), original.db_end_i64(key.code, key.scope, key.table));
      for (auto primary : primary_keys) {
         BOOST_CHECK_EQUAL(cursor.find(key, primary), original.db_find_i64(key.code, key.scope, key.table, primary));
         BOOST_CHECK_EQUAL(cursor.lower_bound(key, primary), original.db_lowerbound_i64(key.code, key.scope, key.table, primary));
         BOOST_CHECK_EQUAL(cursor.upper_bound(key, primary), original.db_upperbound_i64(key.code, key.scope, key.table, primary));
      }
      uint64_t a = 1234, b = 1234;
      const auto end = cursor.end(key);
      if (end < -1) {
         BOOST_CHECK_EQUAL(cursor.previous(end, a), original.db_previous_i64(end, b));
         BOOST_CHECK_EQUAL(a, b);
      }
   }
   std::mt19937_64 rng(0x5058325f484f5354ULL);
   for (unsigned i = 0; i < 500; ++i) {
      const auto& key = keys[rng() % 2];
      const auto primary = primary_keys[rng() % primary_keys.size()];
      const auto iterator = cursor.find(key, primary);
      BOOST_REQUIRE_EQUAL(iterator, original.db_find_i64(key.code, key.scope, key.table, primary));
      uint64_t a = 1234, b = 1234;
      BOOST_CHECK_EQUAL(cursor.next(iterator, a), original.db_next_i64(iterator, b));
      BOOST_CHECK_EQUAL(a, b);
      BOOST_CHECK_EQUAL(cursor.previous(iterator, a), original.db_previous_i64(iterator, b));
      BOOST_CHECK_EQUAL(a, b);
      std::array<char, 8> first, second;
      first.fill('z'); second.fill('z');
      const size_t size = rng() % 9;
      BOOST_CHECK_EQUAL(cursor.get(iterator, std::span(first).first(size)), original.db_get_i64(iterator, second.data(), size));
      BOOST_CHECK(first == second);
      const std::array<char, 4> bytes{char(rng()), char(rng()), '\0', char(rng())};
      const name payer = i % 2 ? "alice"_n : name{};
      cursor.update(iterator, payer, bytes);
      original.db_update_i64(iterator, payer, bytes.data(), bytes.size());
      BOOST_CHECK_EQUAL(cursor.get(iterator, first), original.db_get_i64(iterator, second.data(), second.size()));
      BOOST_CHECK(first == second);
   }
   for (const auto& key : keys) {
      for (auto primary : primary_keys) {
         const primary_query query{key, primary_query_kind::find, primary};
         BOOST_CHECK(isolated.read_primary(query, 4) == live.read_primary(query, 4));
      }
   }
   const auto result = isolated.seal();
   BOOST_CHECK_EQUAL(result.writes().size(), 500u);
   // Invalid handles keep original exception classes; each private attempt is
   // discarded on failure. INT_MIN is tested only in the new adapter because
   // the legacy end-handle conversion negates int and has an overflow hazard.
   for (int bad : {-1, -2, -99, 999}) {
      primary_state_overlay invalid(result.base());
      primary_overlay_cursor invalid_cursor(invalid, "token"_n);
      int64_t original_code = 0, isolated_code = 0;
      try { original.db_get_i64(bad, nullptr, 0); } catch (const fc::exception& e) { original_code = e.code(); }
      try { invalid_cursor.get(bad, {}); } catch (const fc::exception& e) { isolated_code = e.code(); }
      BOOST_CHECK_NE(original_code, 0);
      BOOST_CHECK_EQUAL(isolated_code, original_code);
      BOOST_CHECK_THROW(invalid.seal(), primary_state_invalid_attempt);
   }
   transaction.undo();
   BOOST_CHECK(chain.control->calculate_integrity_hash() == original_hash);
}

// Both controllers remain serial. The observer calls the experimental read
// adapter; the reference uses the existing tester/database APIs. Blocks are
// serialized once and independently decoded for each validator. This is a
// reusable prefix/state/output oracle, NOT a parallel-executor acceptance test.
BOOST_AUTO_TEST_CASE(native_savanna_token_corpus_preserves_state_and_receipts) {
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;

   std::vector<std::vector<char>> corpus;
   fc::temp_directory source_dir;
   {
      tester source(base_tester::default_config(source_dir).first, genesis);
      source.set_node_finalizers(std::array<account_name, 1>{"eosio"_n});
      auto seal = [&] { corpus.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"token"_n, "alice"_n, "bob"_n, "carol"_n});
      seal();
      source.set_code("token"_n, test_contracts::eosio_token_wasm());
      source.set_abi("token"_n, test_contracts::eosio_token_abi());
      seal();
      source.push_action("token"_n, "create"_n, "token"_n,
         fc::mutable_variant_object()("issuer", "token")("maximum_supply", "1000000.0000 PAR"));
      source.issue("token"_n, "1000.0000 PAR", "token"_n, "reference corpus");
      seal();
      source.transfer("token"_n, "alice"_n, "100.0000 PAR", "fund alice", "token"_n);
      source.transfer("token"_n, "bob"_n, "50.0000 PAR", "fund bob", "token"_n);
      seal();
      source.transfer("alice"_n, "bob"_n, "10.0000 PAR", "dependency one", "token"_n);
      source.transfer("bob"_n, "carol"_n, "5.0000 PAR", "dependency two", "token"_n);
      seal();
      source.transfer("bob"_n, "alice"_n, "55.0000 PAR", "empty bob", "token"_n);
      source.push_action("token"_n, "close"_n, "bob"_n,
         fc::mutable_variant_object()("owner", "bob")("symbol", "4,PAR"));
      seal();
      source.transfer("alice"_n, "bob"_n, "1.0000 PAR", "recreate row", "token"_n);
      seal();
      // Validate a rejected input restores the source pending state. It is not
      // inserted into the accepted-block corpus or counted as an accepted trx.
      const auto before_failure = source.control->calculate_integrity_hash();
      BOOST_CHECK_EXCEPTION(
         source.transfer("alice"_n, "carol"_n, "5000.0000 PAR", "overdraw", "token"_n),
         eosio_assert_message_exception,
         [](const auto& error) { return error.to_detail_string().find("overdrawn balance") != std::string::npos; });
      BOOST_CHECK(source.control->calculate_integrity_hash() == before_failure);
      for (unsigned i = 0; i < 12; ++i) seal();
   }

   fc::temp_directory reference_dir, observed_dir;
   tester reference(base_tester::default_config(reference_dir).first, genesis);
   tester observed(base_tester::default_config(observed_dir).first, genesis);
   chainbase_transaction_state_view view(observed.control->db());
   std::vector<trace_summary> reference_traces, observed_traces;
   auto reference_connection = reference.control->applied_transaction().connect([&](const auto& event) {
      reference_traces.push_back(summarize(std::get<0>(event)));
   });
   auto observed_connection = observed.control->applied_transaction().connect([&](const auto& event) {
      observed_traces.push_back(summarize(std::get<0>(event)));
   });

   const auto token_symbol = symbol::from_string("4,PAR");
   const auto primary = token_symbol.to_symbol_code().value;
   size_t compared_traces = 0;
   size_t compared_actions = 0;
   for (size_t block_index = 0; block_index < corpus.size(); ++block_index) {
      reference_traces.clear();
      observed_traces.clear();
      reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[block_index])));
      observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[block_index])));
      reference.control->abort_block();
      observed.control->abort_block();
      BOOST_CHECK(reference.control->head().id() == observed.control->head().id());
      BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
      BOOST_CHECK(reference_traces == observed_traces);
      compared_traces += reference_traces.size();
      for (const auto& trace : reference_traces) compared_actions += trace.action_receipts.size();
      const auto state_before_reads = observed.control->calculate_integrity_hash();
      BOOST_CHECK(reference.control->calculate_integrity_hash() == state_before_reads);

      for (name owner : {"token"_n, "alice"_n, "bob"_n, "carol"_n, "missing"_n}) {
         const table_key key{"token"_n, owner, "accounts"_n};
         const auto result = view.read_primary({key, primary_query_kind::find, primary}, 1024);
         const auto* table = reference.control->db().find<table_id_object, by_code_scope_table>(
            boost::make_tuple(key.code, key.scope, key.table));
         BOOST_CHECK_EQUAL(bool(result.table), table != nullptr);
         if (table) {
            BOOST_CHECK_EQUAL(result.table->object_id, table->id._id);
            BOOST_CHECK_EQUAL(result.table->count, table->count);
            BOOST_CHECK(result.table->payer == table->payer);
         }
         const auto old_api_value = reference.get_row_by_account(key.code, key.scope, key.table, name(primary));
         BOOST_CHECK_EQUAL(bool(result.row), !old_api_value.empty()); // token rows are non-empty assets
         if (result.row) BOOST_CHECK(result.row->value == old_api_value);
         if (block_index == 5 && owner == "bob"_n) BOOST_CHECK(!result.table && !result.row);
         if (block_index >= 6 && owner == "bob"_n) BOOST_REQUIRE(result.row);
      }
      BOOST_CHECK(observed.control->calculate_integrity_hash() == state_before_reads);
   }
   BOOST_CHECK_EQUAL(corpus.size(), 19u);
   BOOST_CHECK_GE(compared_traces, 15u); // guard against two disconnected/empty observers passing
   BOOST_CHECK_GE(compared_actions, compared_traces);
   BOOST_CHECK_GT(observed.last_irreversible_block_num(), 0u);
   for (const auto& [owner, expected] : std::array<std::pair<name, int64_t>, 4>{
         std::pair{"token"_n, 8500000}, {"alice"_n, 1440000}, {"bob"_n, 10000}, {"carol"_n, 50000}}) {
      const auto result = view.read_primary({{"token"_n, owner, "accounts"_n}, primary_query_kind::find, primary}, 1024);
      BOOST_REQUIRE(result.row);
      const auto balance = fc::raw::unpack<asset>(result.row->value);
      BOOST_CHECK_EQUAL(balance.get_amount(), expected);
      BOOST_CHECK(balance.get_symbol() == token_symbol);
   }
}

BOOST_AUTO_TEST_SUITE_END()
