#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/account_object.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <barrier>
#include <future>
#include <limits>
#include <random>
#include <string>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key accounts{"token"_n, "alice"_n, "accounts"_n};
constexpr uint64_t max_key = std::numeric_limits<uint64_t>::max();

struct view_fixture {
   fc::temp_directory dir;
   chainbase::database db{dir.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};

   view_fixture() {
      db.add_index<table_id_multi_index>();
      db.add_index<key_value_index>();
      db.add_index<index64_index>();
      db.add_index<account_index>();
      db.add_index<account_metadata_index>();
   }

   const table_id_object& table(const table_key& key = accounts) {
      if (const auto* found = db.find<table_id_object, by_code_scope_table>(boost::make_tuple(key.code, key.scope, key.table)))
         return *found;
      return db.create<table_id_object>([&](auto& obj) {
         obj.code = key.code;
         obj.scope = key.scope;
         obj.table = key.table;
         obj.payer = "alice"_n;
      });
   }

   const key_value_object& put(uint64_t primary, const std::string& value = "value", const table_key& key = accounts) {
      const auto& t = table(key);
      if (const auto* found = db.find<key_value_object, by_scope_primary>(boost::make_tuple(t.id, primary))) {
         db.modify(*found, [&](auto& obj) { obj.value.assign(value.data(), value.size()); });
         return *found;
      }
      const auto& row = db.create<key_value_object>([&](auto& obj) {
         obj.t_id = t.id;
         obj.primary_key = primary;
         obj.payer = "bob"_n;
         obj.value.assign(value.data(), value.size());
      });
      db.modify(t, [](auto& obj) { ++obj.count; });
      return row;
   }

   void erase(uint64_t primary, const table_key& key = accounts) {
      const auto& t = table(key);
      if (const auto* row = db.find<key_value_object, by_scope_primary>(boost::make_tuple(t.id, primary))) {
         db.remove(*row);
         db.modify(t, [](auto& obj) { --obj.count; });
      }
   }

   primary_read_result read(primary_query_kind kind, uint64_t key = 0, size_t budget = 1024) {
      return view.read_primary({accounts, kind, key}, budget);
   }
};

// Independent oracle: walk physical by_id indices and select using plain
// comparisons. Deliberately NOT the adapter's composite find/bound lookups.
primary_read_result reference_read(const chainbase::database& db, const primary_query& query) {
   primary_read_result result;
   const table_id_object* table = nullptr;
   for (const auto& entry : db.get_index<table_id_multi_index, by_id>()) {
      if (entry.code == query.table.code && entry.scope == query.table.scope && entry.table == query.table.table) {
         table = &entry;
         break;
      }
   }
   if (!table) return result;
   result.table = table_read{query.table, table->id._id, table->payer, table->count};
   const key_value_object* best = nullptr;
   for (const auto& row : db.get_index<key_value_index, by_id>()) {
      if (row.t_id != table->id) continue;
      bool match = false;
      switch (query.kind) {
         case primary_query_kind::find: match = row.primary_key == query.primary_key; break;
         case primary_query_kind::lower_bound: match = row.primary_key >= query.primary_key; break;
         case primary_query_kind::upper_bound: match = row.primary_key > query.primary_key; break;
         case primary_query_kind::last: match = true; break;
         case primary_query_kind::previous: match = row.primary_key < query.primary_key; break;
      }
      if (match && (!best || (query.kind == primary_query_kind::last || query.kind == primary_query_kind::previous ? row.primary_key > best->primary_key
                                                                   : row.primary_key < best->primary_key)))
         best = &row;
   }
   if (best) {
      primary_row_read copy;
      copy.object_id = best->id._id;
      copy.table_object_id = best->t_id._id;
      copy.primary_key = best->primary_key;
      copy.payer = best->payer;
      for (size_t i = 0; i < best->value.size(); ++i) copy.value.push_back(best->value.data()[i]);
      result.row = std::move(copy);
   }
   return result;
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_state_view_tests)

BOOST_FIXTURE_TEST_CASE(missing_table_and_missing_row_are_distinct, view_fixture) {
   BOOST_CHECK(!view.read_table(accounts));
   auto absent = read(primary_query_kind::find, 7);
   BOOST_CHECK(!absent.table && !absent.row);
   const auto id = table().id._id;
   auto empty = read(primary_query_kind::find, 7);
   BOOST_REQUIRE(empty.table);
   BOOST_CHECK_EQUAL(empty.table->object_id, id);
   BOOST_CHECK(!empty.row);
   BOOST_CHECK(view.read_table(accounts) == empty.table);
}

BOOST_FIXTURE_TEST_CASE(point_read_preserves_binary_payload_and_metadata, view_fixture) {
   const std::string payload{"a\0b\xff", 4};
   const auto id = put(7, payload).id._id;
   const auto result = read(primary_query_kind::find, 7, 4);
   BOOST_REQUIRE(result.row);
   BOOST_REQUIRE(result.table);
   BOOST_CHECK_EQUAL(result.row->object_id, id);
   BOOST_CHECK_EQUAL(result.row->table_object_id, result.table->object_id);
   BOOST_CHECK_EQUAL(result.row->primary_key, 7u);
   BOOST_CHECK(result.row->payer == "bob"_n);
   BOOST_CHECK(result.table->payer == "alice"_n);
   BOOST_CHECK(result.table->key == accounts);
   BOOST_CHECK_EQUAL(result.table->count, 1u);
   BOOST_CHECK_EQUAL_COLLECTIONS(result.row->value.begin(), result.row->value.end(), payload.begin(), payload.end());
}

BOOST_FIXTURE_TEST_CASE(empty_value_is_a_present_row_with_zero_budget, view_fixture) {
   put(0, "");
   auto result = read(primary_query_kind::find, 0, 0);
   BOOST_REQUIRE(result.row);
   BOOST_CHECK(result.row->value.empty());
}

BOOST_FIXTURE_TEST_CASE(find_is_not_lower_bound, view_fixture) {
   put(10);
   BOOST_CHECK(!read(primary_query_kind::find, 9).row);
   BOOST_REQUIRE(read(primary_query_kind::lower_bound, 9).row);
   BOOST_CHECK_EQUAL(read(primary_query_kind::lower_bound, 9).row->primary_key, 10u);
   BOOST_CHECK(!read(primary_query_kind::upper_bound, 10).row);
}

BOOST_FIXTURE_TEST_CASE(bounds_do_not_overflow_uint64_or_cross_tables, view_fixture) {
   put(0); put(20); put(max_key);
   put(0, "other", {"token"_n, "bob"_n, "accounts"_n});
   BOOST_CHECK_EQUAL(read(primary_query_kind::lower_bound, 0).row->primary_key, 0u);
   BOOST_CHECK_EQUAL(read(primary_query_kind::upper_bound, 0).row->primary_key, 20u);
   BOOST_CHECK_EQUAL(read(primary_query_kind::lower_bound, max_key).row->primary_key, max_key);
   BOOST_CHECK(!read(primary_query_kind::upper_bound, max_key).row);
   BOOST_CHECK_EQUAL(read(primary_query_kind::last).row->primary_key, max_key);
}

BOOST_FIXTURE_TEST_CASE(logical_table_components_are_all_significant, view_fixture) {
   const std::array<table_key, 4> keys{accounts,
      table_key{"other"_n, "alice"_n, "accounts"_n},
      table_key{"token"_n, "bob"_n, "accounts"_n},
      table_key{"token"_n, "alice"_n, "other"_n}};
   for (size_t i = 0; i < keys.size(); ++i) put(1, std::string(i + 1, 'a'), keys[i]);
   for (size_t i = 0; i < keys.size(); ++i) {
      const auto result = view.read_primary({keys[i], primary_query_kind::find, 1}, 4);
      BOOST_REQUIRE(result.row);
      BOOST_CHECK_EQUAL(result.row->value.size(), i + 1);
      BOOST_CHECK(result.table->key == keys[i]);
   }
}

BOOST_FIXTURE_TEST_CASE(last_of_empty_table_does_not_use_neighbour, view_fixture) {
   put(99, "before", {"before"_n, "alice"_n, "accounts"_n});
   table();
   put(0, "after", {"after"_n, "alice"_n, "accounts"_n});
   for (auto kind : {primary_query_kind::last, primary_query_kind::lower_bound, primary_query_kind::upper_bound}) {
      const auto result = read(kind);
      BOOST_REQUIRE(result.table);
      BOOST_CHECK(!result.row);
   }
}

BOOST_FIXTURE_TEST_CASE(table_with_only_secondary_entries_has_no_primary_rows, view_fixture) {
   const auto& t = table();
   db.create<index64_object>([&](auto& row) {
      row.t_id = t.id; row.primary_key = 7; row.secondary_key = 9; row.payer = "alice"_n;
   });
   db.modify(t, [](auto& obj) { ++obj.count; });
   for (auto kind : {primary_query_kind::find, primary_query_kind::last, primary_query_kind::lower_bound}) {
      const auto result = read(kind, 7);
      BOOST_REQUIRE(result.table);
      BOOST_CHECK_EQUAL(result.table->count, 1u);
      BOOST_CHECK(!result.row);
   }
}

BOOST_FIXTURE_TEST_CASE(owned_result_survives_update_delete_and_table_removal, view_fixture) {
   put(5, "old");
   const auto saved = read(primary_query_kind::find, 5);
   put(5, "new data");
   BOOST_CHECK(saved.row->value == std::vector<char>({'o','l','d'}));
   erase(5);
   db.remove(table());
   BOOST_CHECK(!view.read_table(accounts));
   BOOST_CHECK_EQUAL(saved.table->count, 1u);
   BOOST_CHECK(saved.row->value == std::vector<char>({'o','l','d'}));
}

BOOST_FIXTURE_TEST_CASE(modifying_owned_copy_does_not_modify_database, view_fixture) {
   put(8, "abc");
   auto copy = read(primary_query_kind::find, 8);
   copy.row->value[0] = 'z';
   copy.row->payer = "nobody"_n;
   copy.table->count = 77;
   const auto current = read(primary_query_kind::find, 8);
   BOOST_CHECK_EQUAL(current.row->value[0], 'a');
   BOOST_CHECK(current.row->payer == "bob"_n);
   BOOST_CHECK_EQUAL(current.table->count, 1u);
}

BOOST_FIXTURE_TEST_CASE(copy_budget_is_local_and_does_not_mutate_state, view_fixture) {
   put(1, "12345");
   const auto before = read(primary_query_kind::find, 1, 5);
   for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                     primary_query_kind::upper_bound, primary_query_kind::last})
      BOOST_CHECK_THROW(read(kind, kind == primary_query_kind::upper_bound ? 0 : 1, 4), state_view_read_limit_exceeded);
   BOOST_CHECK(read(primary_query_kind::find, 1, 5) == before);
   BOOST_CHECK(!read(primary_query_kind::find, 99, 0).row);
}

BOOST_FIXTURE_TEST_CASE(invalid_query_fails_even_when_table_is_missing, view_fixture) {
   const auto invalid = static_cast<primary_query_kind>(255);
   BOOST_CHECK_THROW(read(invalid), state_view_unsupported_query);
   put(0);
   BOOST_CHECK_THROW(read(invalid), state_view_unsupported_query);
}

BOOST_FIXTURE_TEST_CASE(serial_adapter_rejects_worker_access, view_fixture) {
   put(1);
   auto worker = std::async(std::launch::async, [&]() {
      unsigned caught = 0;
      try { (void)view.read_table(accounts); } catch (const state_view_wrong_thread&) { ++caught; }
      try { (void)read(primary_query_kind::find, 1); } catch (const state_view_wrong_thread&) { ++caught; }
      return caught;
   });
   BOOST_CHECK_EQUAL(worker.get(), 2u);
   BOOST_REQUIRE(read(primary_query_kind::find, 1).row);
}

BOOST_FIXTURE_TEST_CASE(live_serial_reads_and_nested_undo, view_fixture) {
   put(3, "initial");
   const auto original = read(primary_query_kind::find, 3);
   {
      auto outer = db.start_undo_session(true);
      put(3, "outer");
      const auto middle = read(primary_query_kind::find, 3);
      BOOST_CHECK(!(middle == original));
      {
         auto inner = db.start_undo_session(true);
         erase(3);
         BOOST_CHECK(!read(primary_query_kind::find, 3).row);
         put(9, "inner");
      }
      BOOST_CHECK(read(primary_query_kind::find, 3) == middle);
      BOOST_CHECK(!read(primary_query_kind::find, 9).row);
   }
   BOOST_CHECK(read(primary_query_kind::find, 3) == original);
}

BOOST_FIXTURE_TEST_CASE(undo_and_recreate_use_current_table_and_row_ids, view_fixture) {
   primary_read_result transient;
   {
      auto session = db.start_undo_session(true);
      put(4, "discarded");
      transient = read(primary_query_kind::find, 4);
   }
   BOOST_CHECK(!view.read_table(accounts));
   put(4, "committed");
   const auto current = read(primary_query_kind::find, 4);
   BOOST_CHECK_EQUAL(current.table->object_id, transient.table->object_id);
   BOOST_CHECK_EQUAL(current.row->object_id, transient.row->object_id);
   BOOST_CHECK(current.row->value != transient.row->value);
}

BOOST_FIXTURE_TEST_CASE(reads_work_in_read_only_mode_without_db_allocations, view_fixture) {
   put(1);
   const auto revision = db.revision();
   const auto available = db.get_free_memory();
   db.set_read_only_mode();
   for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                     primary_query_kind::upper_bound, primary_query_kind::last})
      (void)read(kind, 1);
   BOOST_CHECK_EQUAL(db.revision(), revision);
   BOOST_CHECK_EQUAL(db.get_free_memory(), available);
}

BOOST_AUTO_TEST_CASE(owned_result_survives_database_close) {
   primary_read_result saved;
   {
      view_fixture fixture;
      fixture.put(1, "durable copy");
      saved = fixture.read(primary_query_kind::find, 1);
   }
   BOOST_REQUIRE(saved.row);
   BOOST_CHECK(saved.row->value == std::vector<char>({'d','u','r','a','b','l','e',' ','c','o','p','y'}));
}

BOOST_FIXTURE_TEST_CASE(deterministic_random_queries_match_independent_by_id_oracle, view_fixture) {
   std::mt19937_64 random(0x5058305f52454144ull);
   const std::array<table_key, 3> keys{accounts,
      table_key{"token"_n, "bob"_n, "accounts"_n},
      table_key{"token"_n, "alice"_n, "unused"_n}};
   for (unsigned step = 0; step < 600; ++step) {
      const auto& key = keys[step % 2]; // third table stays absent
      const uint64_t primary = step % 31 == 0 ? max_key : random() % 40;
      if (step % 4 == 0) erase(primary, key);
      else put(primary, std::string(random() % 20, char(random() & 255)), key);
      for (const auto& query_table : keys) {
         for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                           primary_query_kind::upper_bound, primary_query_kind::last}) {
            const primary_query query{query_table, kind, primary};
            BOOST_CHECK(view.read_primary(query, 20) == reference_read(db, query));
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_primary_cursor_tests)

BOOST_FIXTURE_TEST_CASE(predecessor_queries_have_strict_table_local_boundaries, view_fixture) {
   put(0, "zero"); put(20, "middle"); put(max_key, "last");
   const table_key other{"token"_n, "bob"_n, "accounts"_n};
   const table_key empty{"token"_n, "empty"_n, "accounts"_n};
   put(max_key, "other", other); table(empty);
   const std::array keys{accounts, other, empty};
   auto base = frozen_primary_state::capture(view, keys);
   for (auto key : keys) {
      for (uint64_t primary : {uint64_t{0}, uint64_t{1}, uint64_t{20}, uint64_t{21}, max_key}) {
         const primary_query query{key, primary_query_kind::previous, primary};
         const auto expected = reference_read(db, query);
         BOOST_CHECK(view.read_primary(query, 100) == expected);
         BOOST_CHECK(base->read_primary(query, 100) == expected);
      }
   }
   BOOST_CHECK(!view.locate_primary({accounts, primary_query_kind::previous, 0}).row);
   BOOST_CHECK_EQUAL(view.locate_primary({accounts, primary_query_kind::previous, max_key}).row->primary_key, 20u);
}

BOOST_FIXTURE_TEST_CASE(iterator_and_end_values_match_original_host_conventions, view_fixture) {
   put(1); put(3); put(max_key);
   const table_key other{"token"_n, "bob"_n, "accounts"_n};
   const table_key empty{"token"_n, "empty"_n, "accounts"_n};
   const table_key absent{"token"_n, "missing"_n, "accounts"_n};
   put(2, "other", other); table(empty);
   const std::array keys{accounts, other, empty, absent};
   primary_state_overlay state(frozen_primary_state::capture(view, keys));
   primary_overlay_cursor cursor(state, "token"_n);
   BOOST_CHECK_EQUAL(cursor.find(absent, 0), -1);
   BOOST_CHECK_EQUAL(cursor.end(absent), -1);
   BOOST_CHECK_EQUAL(cursor.end(accounts), -2);
   BOOST_CHECK_EQUAL(cursor.find(accounts, 2), -2);
   BOOST_CHECK_EQUAL(cursor.find(accounts, 1), 0);
   BOOST_CHECK_EQUAL(cursor.find(accounts, 3), 1);
   BOOST_CHECK_EQUAL(cursor.find(accounts, 1), 0);
   BOOST_CHECK_EQUAL(cursor.lower_bound(accounts, 2), 1);
   BOOST_CHECK_EQUAL(cursor.upper_bound(accounts, 3), 2);
   BOOST_CHECK_EQUAL(cursor.upper_bound(accounts, max_key), -2);
   BOOST_CHECK_EQUAL(cursor.end(other), -3);
   BOOST_CHECK_EQUAL(cursor.end(empty), -4);
   BOOST_CHECK_EQUAL(cursor.lower_bound(empty, 0), -4);
   uint64_t primary = 99;
   BOOST_CHECK_EQUAL(cursor.previous(-4, primary), -1);
   BOOST_CHECK_EQUAL(primary, 99u);
   BOOST_CHECK_EQUAL(cursor.next(0, primary), 1);
   BOOST_CHECK_EQUAL(primary, 3u);
   BOOST_CHECK_EQUAL(cursor.next(1, primary), 2);
   BOOST_CHECK_EQUAL(primary, max_key);
   primary = 99;
   BOOST_CHECK_EQUAL(cursor.next(2, primary), -2);
   BOOST_CHECK_EQUAL(primary, 99u);
   BOOST_CHECK_EQUAL(cursor.next(-2, primary), -1);
   BOOST_CHECK_EQUAL(cursor.next(std::numeric_limits<int>::min(), primary), -1);
   BOOST_CHECK_EQUAL(primary, 99u);
   BOOST_CHECK_EQUAL(cursor.previous(-2, primary), 2);
   BOOST_CHECK_EQUAL(primary, max_key);
   BOOST_CHECK_EQUAL(cursor.previous(2, primary), 1);
   BOOST_CHECK_EQUAL(primary, 3u);
   primary = 99;
   BOOST_CHECK_EQUAL(cursor.previous(0, primary), -1);
   BOOST_CHECK_EQUAL(primary, 99u);
   BOOST_CHECK_EQUAL(cursor.find(other, 2), 3);
}

BOOST_FIXTURE_TEST_CASE(get_size_partial_full_and_update_preserve_iterator, view_fixture) {
   put(1, "abcd");
   const std::array keys{accounts};
   primary_state_overlay state(frozen_primary_state::capture(view, keys));
   primary_overlay_cursor cursor(state, "token"_n);
   const auto iterator = cursor.find(accounts, 1);
   BOOST_CHECK_EQUAL(state.write_count(), 0u);
   BOOST_CHECK_EQUAL(cursor.get(iterator, {}), 4);
   std::array<char, 8> output; output.fill('z');
   BOOST_CHECK_EQUAL(cursor.get(iterator, std::span(output).first(2)), 2);
   BOOST_CHECK_EQUAL(output[0], 'a'); BOOST_CHECK_EQUAL(output[1], 'b'); BOOST_CHECK_EQUAL(output[2], 'z');
   BOOST_CHECK_EQUAL(cursor.get(iterator, output), 4);
   BOOST_CHECK_EQUAL(output[3], 'd'); BOOST_CHECK_EQUAL(output[4], 'z');
   cursor.update(iterator, name{}, std::string("efgh"));
   BOOST_CHECK_EQUAL(cursor.find(accounts, 1), iterator);
   BOOST_CHECK_EQUAL(cursor.get(iterator, {}), 4);
   BOOST_CHECK_EQUAL(cursor.get(iterator, output), 4);
   BOOST_CHECK_EQUAL(output[0], 'e'); BOOST_CHECK_EQUAL(output[3], 'h'); BOOST_CHECK_EQUAL(output[4], 'z');
   BOOST_CHECK_EQUAL(read(primary_query_kind::find, 1).row->value[0], 'a');
   BOOST_CHECK_EQUAL(state.write_count(), 1u);
   BOOST_CHECK(std::async(std::launch::async, [&] {
      try { (void)state.write_count(); } catch (const state_view_wrong_thread&) { return true; }
      return false;
   }).get());
   BOOST_CHECK_EQUAL(state.seal().writes().size(), 1u);
   BOOST_CHECK_THROW(state.write_count(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(zero_length_get_leaves_destination_untouched, view_fixture) {
   put(0, "");
   const std::array keys{accounts};
   primary_state_overlay state(frozen_primary_state::capture(view, keys));
   primary_overlay_cursor cursor(state, "token"_n);
   const auto iterator = cursor.find(accounts, 0);
   std::array<char, 4> output{'a', 'b', 'c', 'd'};
   BOOST_CHECK_EQUAL(cursor.get(iterator, output), 0);
   BOOST_CHECK_EQUAL(cursor.get(iterator, {}), 0);
   BOOST_CHECK(output == (std::array<char, 4>{'a', 'b', 'c', 'd'}));
   cursor.update(iterator, name{}, {});
   BOOST_CHECK_EQUAL(state.seal().writes().size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(invalid_handles_poison_entire_attempt, view_fixture) {
   put(1, "abc");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   for (int bad : {-1, 123, std::numeric_limits<int>::max()}) {
      primary_state_overlay state(base);
      primary_overlay_cursor cursor(state, "token"_n);
      cursor.update(cursor.find(accounts, 1), name{}, std::string("def"));
      BOOST_CHECK_THROW(cursor.get(bad, {}), invalid_table_iterator);
      BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
   }
   for (int end : {-2, std::numeric_limits<int>::min()}) {
      primary_state_overlay state(base);
      primary_overlay_cursor cursor(state, "token"_n);
      BOOST_CHECK_THROW(cursor.get(end, {}), table_operation_not_permitted);
      BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
   }
   primary_state_overlay state(base);
   primary_overlay_cursor cursor(state, "token"_n);
   uint64_t primary = 99;
   BOOST_CHECK_THROW(cursor.previous(std::numeric_limits<int>::min(), primary), invalid_table_iterator);
   BOOST_CHECK_EQUAL(primary, 99u);
   BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(foreign_code_may_be_read_but_cannot_be_updated, view_fixture) {
   const table_key foreign{"other"_n, "alice"_n, "accounts"_n};
   put(1, "abc", foreign);
   const std::array keys{foreign};
   primary_state_overlay state(frozen_primary_state::capture(view, keys));
   primary_overlay_cursor cursor(state, "token"_n);
   const auto iterator = cursor.find(foreign, 1);
   BOOST_CHECK_EQUAL(cursor.get(iterator, {}), 3);
   BOOST_CHECK_THROW(cursor.update(iterator, name{}, std::string("def")), table_access_violation);
   BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(existing_row_resize_and_payer_changes_refresh_old_iterators_and_retain_every_write, view_fixture) {
   put(1, "abc");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay state(base);
   primary_overlay_cursor cursor(state, "token"_n), other(state, "token"_n);
   const auto iterator = cursor.find(accounts, 1), other_iterator = other.find(accounts, 1);
   const std::array values{std::string("longer"), std::string(), std::string("z"), std::string("0123456789"), std::string("abc")};
   const std::array payers{"carol"_n, name{}, name{}, "bob"_n, name{}};
   name expected_payer = "bob"_n;
   for (unsigned step = 0; step < values.size(); ++step) {
      if (payers[step] != name{}) expected_payer = payers[step];
      cursor.update(iterator, payers[step], values[step]);
      BOOST_CHECK_EQUAL(cursor.get(iterator, {}), values[step].size());
      BOOST_CHECK_EQUAL(other.get(other_iterator, {}), values[step].size());
      std::array<char, 4> buffer{'!', '!', '!', '!'};
      const auto copied = std::min(buffer.size(), values[step].size());
      BOOST_CHECK_EQUAL(other.get(other_iterator, buffer), copied);
      BOOST_CHECK(std::equal(buffer.begin(), buffer.begin() + copied, values[step].begin()));
      for (size_t i = copied; i < buffer.size(); ++i) BOOST_CHECK_EQUAL(buffer[i], '!');
      for (const auto kind : {primary_query_kind::find, primary_query_kind::lower_bound, primary_query_kind::last}) {
         const primary_query query{accounts, kind, 1};
         const auto read = state.read_primary(query, values[step].size());
         BOOST_REQUIRE(read.row);
         BOOST_CHECK(read.row->payer == expected_payer);
         BOOST_CHECK(read.row->value == std::vector<char>(values[step].begin(), values[step].end()));
         BOOST_CHECK_EQUAL(state.locate_primary(query).row->value_size, values[step].size());
         BOOST_CHECK(state.read_primary_prefix(query, 0).metadata == state.locate_primary(query));
      }
   }
   const auto result = state.seal();
   BOOST_REQUIRE_EQUAL(result.writes().size(), values.size());
   BOOST_CHECK(result.primary_write(1).payer == "carol"_n); // same_payer follows the private switch
   BOOST_CHECK(result.primary_write(4).payer == "bob"_n);
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   BOOST_CHECK_EQUAL(view.locate_primary({accounts, primary_query_kind::find, 1}).row->value_size, 3u);
   BOOST_CHECK(view.locate_primary({accounts, primary_query_kind::find, 1}).row->payer == "bob"_n);
}

BOOST_FIXTURE_TEST_CASE(iterator_budgets_and_thread_ownership_are_enforced, view_fixture) {
   put(1); put(2);
   const table_key other{"token"_n, "bob"_n, "accounts"_n}; table(other);
   const std::array keys{accounts, other};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay state(base);
   primary_overlay_cursor cursor(state, "token"_n, 1, 1);
   auto worker = std::async(std::launch::async, [&] {
      try { (void)cursor.find(accounts, 1); } catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(worker.get());
   BOOST_CHECK_EQUAL(cursor.find(accounts, 1), 0);
   BOOST_CHECK_EQUAL(cursor.find(accounts, 1), 0);
   BOOST_CHECK_THROW(cursor.find(accounts, 2), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
   primary_state_overlay second(base);
   primary_overlay_cursor end_cursor(second, "token"_n, 1, 1);
   BOOST_CHECK_EQUAL(end_cursor.end(accounts), -2);
   BOOST_CHECK_THROW(end_cursor.end(other), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(second.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(cursor_handles_are_local_while_private_writes_are_shared_within_attempt, view_fixture) {
   put(1, "aaa"); put(2, "bbb");
   const std::array keys{accounts};
   primary_state_overlay state(frozen_primary_state::capture(view, keys));
   primary_overlay_cursor first(state, "token"_n), second(state, "token"_n);
   BOOST_CHECK_EQUAL(first.find(accounts, 1), 0);
   BOOST_CHECK_EQUAL(second.find(accounts, 2), 0);
   first.update(0, name{}, std::string("ccc"));
   std::array<char, 3> output{};
   BOOST_CHECK_EQUAL(second.get(0, output), 3);
   BOOST_CHECK(output == (std::array<char, 3>{'b', 'b', 'b'}));
   BOOST_CHECK_EQUAL(second.find(accounts, 1), 1);
   BOOST_CHECK_EQUAL(second.get(1, output), 3);
   BOOST_CHECK(output == (std::array<char, 3>{'c', 'c', 'c'}));
   (void)state.seal();
   BOOST_CHECK_THROW(first.get(0, output), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(second.end(accounts), primary_state_invalid_attempt);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_primary_isolation_tests)

BOOST_FIXTURE_TEST_CASE(metadata_and_prefix_do_not_require_full_value_copy, view_fixture) {
   const std::string large(1024 * 1024, 'x');
   put(1, large);
   const primary_query query{accounts, primary_query_kind::find, 1};
   const auto found = view.locate_primary(query);
   BOOST_REQUIRE(found.row);
   BOOST_CHECK_EQUAL(found.row->value_size, large.size());
   const auto zero = view.read_primary_prefix(query, 0);
   BOOST_CHECK(zero.metadata == found);
   BOOST_CHECK(zero.value.empty());
   const auto prefix = view.read_primary_prefix(query, 7);
   BOOST_CHECK(prefix.metadata == found);
   BOOST_CHECK(prefix.value == std::vector<char>(7, 'x'));
   BOOST_CHECK_THROW(view.read_primary(query, 7), state_view_read_limit_exceeded);
   const auto full = view.read_primary_prefix(query, std::numeric_limits<size_t>::max());
   BOOST_CHECK_EQUAL(full.value.size(), large.size());
}

BOOST_FIXTURE_TEST_CASE(prefix_distinguishes_absent_and_empty_and_owns_bytes, view_fixture) {
   const primary_query query{accounts, primary_query_kind::find, 0};
   BOOST_CHECK(!view.read_primary_prefix(query, 10).metadata.table);
   put(0, "");
   const auto empty = view.read_primary_prefix(query, 10);
   BOOST_REQUIRE(empty.metadata.row);
   BOOST_CHECK_EQUAL(empty.metadata.row->value_size, 0u);
   put(0, std::string("a\0b", 3));
   const auto prefix = view.read_primary_prefix(query, 2);
   erase(0);
   BOOST_CHECK(prefix.value == std::vector<char>({'a', '\0'}));
   BOOST_CHECK_EQUAL(prefix.metadata.row->value_size, 3u);
   const auto absent = view.read_primary_prefix(query, 10);
   BOOST_CHECK(absent.metadata.table && !absent.metadata.row && absent.value.empty());
}

BOOST_FIXTURE_TEST_CASE(metadata_and_prefix_preserve_thread_and_query_guards, view_fixture) {
   primary_query query{accounts, static_cast<primary_query_kind>(255), 0};
   BOOST_CHECK_THROW(view.locate_primary(query), state_view_unsupported_query);
   BOOST_CHECK_THROW(view.read_primary_prefix(query, 0), state_view_unsupported_query);
   query.kind = primary_query_kind::find;
   auto worker = std::async(std::launch::async, [&] {
      unsigned rejected = 0;
      try { (void)view.locate_primary(query); } catch (const state_view_wrong_thread&) { ++rejected; }
      try { (void)view.read_primary_prefix(query, 0); } catch (const state_view_wrong_thread&) { ++rejected; }
      return rejected;
   });
   BOOST_CHECK_EQUAL(worker.get(), 2u);
}

BOOST_FIXTURE_TEST_CASE(frozen_queries_match_live_and_independent_reference, view_fixture) {
   const table_key absent{"token"_n, "missing"_n, "accounts"_n};
   const table_key empty{"token"_n, "empty"_n, "accounts"_n};
   put(0, ""); put(17, "abc"); put(max_key, std::string("z\0z", 3)); table(empty);
   const std::array keys{accounts, absent, empty};
   auto frozen = frozen_primary_state::capture(view, keys);
   BOOST_CHECK_EQUAL(frozen->table_count(), 3u);
   BOOST_CHECK_EQUAL(frozen->row_count(), 3u);
   BOOST_CHECK_EQUAL(frozen->value_bytes(), 6u);
   for (auto key : keys) {
      BOOST_CHECK(frozen->read_table(key) == view.read_table(key));
      for (uint64_t primary : {uint64_t{0}, uint64_t{1}, uint64_t{17}, uint64_t{18}, max_key}) {
         for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                           primary_query_kind::upper_bound, primary_query_kind::last}) {
            const primary_query query{key, kind, primary};
            BOOST_CHECK(frozen->read_primary(query, 100) == reference_read(db, query));
            BOOST_CHECK(frozen->locate_primary(query) == view.locate_primary(query));
            for (size_t size : {size_t{0}, size_t{1}, size_t{3}, size_t{100}})
               BOOST_CHECK(frozen->read_primary_prefix(query, size) == view.read_primary_prefix(query, size));
         }
      }
   }
   BOOST_CHECK_THROW(frozen->read_primary({accounts, primary_query_kind::find, 17}, 2), state_view_read_limit_exceeded);
}

BOOST_FIXTURE_TEST_CASE(unknown_is_not_absent_and_capture_deduplicates, view_fixture) {
   const std::array keys{accounts, accounts};
   auto frozen = frozen_primary_state::capture(view, keys);
   BOOST_CHECK_EQUAL(frozen->table_count(), 1u);
   BOOST_CHECK(!frozen->read_table(accounts));
   const table_key other{"token"_n, "bob"_n, "accounts"_n};
   BOOST_CHECK_THROW(frozen->read_table(other), primary_state_unsupported);
   BOOST_CHECK_THROW(frozen->locate_primary({other}), primary_state_unsupported);
   BOOST_CHECK_THROW(frozen->read_primary_prefix({other}, 0), primary_state_unsupported);
   BOOST_CHECK_THROW(frozen->read_primary({other}, 0), primary_state_unsupported);
   BOOST_CHECK_THROW(frozen->locate_primary({accounts, static_cast<primary_query_kind>(255)}), state_view_unsupported_query);
}

BOOST_FIXTURE_TEST_CASE(capture_limits_are_checked_and_fail_without_state_changes, view_fixture) {
   const std::array keys{accounts};
   put(1, "abc"); put(2, "def");
   const auto before = read(primary_query_kind::find, 1);
   const auto free = db.get_free_memory();
   const auto revision = db.revision();
   BOOST_CHECK_THROW(frozen_primary_state::capture(view, keys, {0, 2, 6}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(frozen_primary_state::capture(view, keys, {1, 1, 6}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(frozen_primary_state::capture(view, keys, {1, 2, 5}), primary_state_budget_exceeded);
   auto exact = frozen_primary_state::capture(view, keys, {1, 2, 6});
   BOOST_CHECK_EQUAL(exact->value_bytes(), 6u);
   BOOST_CHECK(read(primary_query_kind::find, 1) == before);
   BOOST_CHECK_EQUAL(db.get_free_memory(), free);
   BOOST_CHECK_EQUAL(db.revision(), revision);
   const std::array<table_key, 0> none{};
   BOOST_CHECK_EQUAL(frozen_primary_state::capture(view, none, {0, 0, 0})->table_count(), 0u);
}

BOOST_AUTO_TEST_CASE(frozen_state_survives_database_destruction_and_absence_stays_absent) {
   std::shared_ptr<const frozen_primary_state> frozen;
   const table_key absent{"token"_n, "missing"_n, "accounts"_n};
   {
      view_fixture fixture;
      fixture.put(7, "old");
      const std::array keys{accounts, absent};
      frozen = frozen_primary_state::capture(fixture.view, keys);
      fixture.put(7, "new");
      fixture.put(1, "created later", absent);
      fixture.erase(7);
      fixture.db.remove(fixture.table());
   }
   BOOST_CHECK(!frozen->read_table(absent));
   const auto read = frozen->read_primary({accounts, primary_query_kind::find, 7}, 3);
   BOOST_REQUIRE(read.row);
   BOOST_CHECK(read.row->value == std::vector<char>({'o', 'l', 'd'}));
}

BOOST_FIXTURE_TEST_CASE(overlay_reads_own_writes_retains_order_and_does_not_write_base, view_fixture) {
   put(1, "aaa"); put(2, "bbb");
   const std::array keys{accounts};
   const auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay overlay(base);
   overlay.update_existing(accounts, 1, name{}, std::string("ccc"));
   overlay.update_existing(accounts, 2, "bob"_n, std::string("ddd"));
   overlay.update_existing(accounts, 1, "bob"_n, std::string("eee"));
   for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound, primary_query_kind::upper_bound}) {
      const primary_query query{accounts, kind, kind == primary_query_kind::upper_bound ? 0u : 1u};
      BOOST_CHECK(overlay.read_primary(query, 3).row->value == std::vector<char>(3, 'e'));
      BOOST_CHECK(overlay.read_primary_prefix(query, 2).value == std::vector<char>(2, 'e'));
      BOOST_CHECK(overlay.locate_primary(query) == base->locate_primary(query));
   }
   BOOST_CHECK(overlay.read_primary({accounts, primary_query_kind::last}, 3).row->value == std::vector<char>(3, 'd'));
   BOOST_CHECK(read(primary_query_kind::find, 1).row->value == std::vector<char>(3, 'a'));
   BOOST_CHECK(base->read_primary({accounts, primary_query_kind::find, 2}, 3).row->value == std::vector<char>(3, 'b'));
   const auto result = overlay.seal();
   BOOST_CHECK(result.base() == base);
   BOOST_CHECK_EQUAL(result.read_tables().size(), 1u);
   BOOST_REQUIRE_EQUAL(result.writes().size(), 3u);
   BOOST_CHECK_EQUAL(result.primary_write(0).primary_key, 1u);
   BOOST_CHECK_EQUAL(result.primary_write(1).primary_key, 2u);
   BOOST_CHECK_EQUAL(result.primary_write(2).primary_key, 1u);
   BOOST_CHECK(result.primary_write(0).value == std::vector<char>(3, 'c'));
   BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(overlay.read_table(accounts), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(overlay.update_existing(accounts, 1, name{}, std::string("fff")), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(lifecycle_create_erase_recreate_keeps_owned_generations_and_every_operation, view_fixture) {
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay state(base);
   primary_overlay_cursor cursor(state, "token"_n);
   BOOST_CHECK_EQUAL(cursor.end(accounts), -1);
   const auto first = cursor.store(accounts, max_key, "alice"_n, std::string("first"));
   const auto first_table = *state.read_table(accounts);
   const auto first_row = *state.locate_primary({accounts, primary_query_kind::find, max_key}).row;
   BOOST_CHECK_LT(first_table.object_id, 0);
   BOOST_CHECK_LT(first_row.object_id, 0);
   BOOST_CHECK_EQUAL(first_table.count, 1u);
   BOOST_CHECK(first_table.payer == "alice"_n);
   cursor.update(first, "bob"_n, {});
   BOOST_CHECK_EQUAL(cursor.get(first, {}), 0);
   cursor.remove(first);
   BOOST_CHECK(!state.read_table(accounts));
   BOOST_CHECK(!state.read_primary_prefix({accounts, primary_query_kind::last}, 5).metadata.row);
   const auto second = cursor.store(accounts, max_key, "carol"_n, std::string("again"));
   const auto second_table = *state.read_table(accounts);
   const auto second_row = *state.locate_primary({accounts, primary_query_kind::find, max_key}).row;
   BOOST_CHECK_NE(second, first);
   BOOST_CHECK_NE(second_table.object_id, first_table.object_id);
   BOOST_CHECK_NE(second_row.object_id, first_row.object_id);
   BOOST_CHECK(second_table.payer == "carol"_n);
   cursor.store(accounts, 0, "alice"_n, std::string("zero"));
   BOOST_CHECK_EQUAL(state.read_table(accounts)->count, 2u);
   uint64_t key = 77;
   BOOST_CHECK_EQUAL(cursor.previous(cursor.end(accounts), key), second);
   BOOST_CHECK_EQUAL(key, max_key);
   BOOST_CHECK_EQUAL(cursor.get(second, {}), 5);
   const auto result = state.seal();
   BOOST_REQUIRE_EQUAL(result.writes().size(), 5u);
   BOOST_CHECK(result.primary_write(0).operation == primary_operation::store);
   BOOST_CHECK(result.primary_write(1).operation == primary_operation::update);
   BOOST_CHECK(result.primary_write(2).operation == primary_operation::remove);
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   BOOST_CHECK(!view.read_table(accounts));
}

BOOST_FIXTURE_TEST_CASE(lifecycle_old_row_and_retired_end_handles_cannot_revive, view_fixture) {
   put(1, "one"); put(9, "nine");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   for (unsigned operation = 0; operation < 5; ++operation) {
      primary_state_overlay state(base);
      primary_overlay_cursor cursor(state, "token"_n);
      const auto old = cursor.find(accounts, 1);
      cursor.remove(old);
      const auto replacement = cursor.store(accounts, 1, "bob"_n, std::string("new"));
      BOOST_CHECK_NE(old, replacement);
      auto use_old = [&] {
         uint64_t key = 0;
         switch (operation) {
            case 0: (void)cursor.get(old, {}); break;
            case 1: cursor.update(old, {}, {}); break;
            case 2: (void)cursor.next(old, key); break;
            case 3: (void)cursor.previous(old, key); break;
            case 4: cursor.remove(old); break;
         }
      };
      BOOST_CHECK_THROW(use_old(), primary_state_unsupported);
      BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
   }
   primary_state_overlay state(base);
   primary_overlay_cursor cursor(state, "token"_n);
   const auto old_end = cursor.end(accounts);
   cursor.remove(cursor.find(accounts, 1)); cursor.remove(cursor.find(accounts, 9));
   cursor.store(accounts, 1, "alice"_n, std::string("replacement table"));
   BOOST_CHECK_NE(cursor.end(accounts), old_end);
   uint64_t key = 42;
   BOOST_CHECK_EQUAL(cursor.next(old_end, key), -1); // original next ignores any end pointer
   BOOST_CHECK_EQUAL(key, 42u);
   BOOST_CHECK_THROW(cursor.previous(old_end, key), primary_state_unsupported);
   BOOST_CHECK_THROW(state.seal(), primary_state_invalid_attempt);
   BOOST_CHECK_EQUAL(view.read_table(accounts)->count, 2u);
}

BOOST_FIXTURE_TEST_CASE(lifecycle_sparse_neighbors_are_discovered_not_guessed_after_erase, view_fixture) {
   put(3, "three"); put(7, "seven");
   const primary_query first{accounts, primary_query_kind::lower_bound, 0};
   std::vector<primary_query> queries{{accounts, primary_query_kind::find, 3}, first};
   auto base = frozen_primary_state::capture_queries(view, {}, queries);
   primary_state_overlay partial(base);
   partial.remove(accounts, 3);
   try { (void)partial.locate_primary(first); BOOST_FAIL("missing neighbor was guessed"); }
   catch (const primary_state_missing& e) {
      const auto& request = std::get<primary_capture_request>(e.request);
      BOOST_REQUIRE(request.query);
      BOOST_CHECK(request.query->kind == primary_query_kind::upper_bound);
      BOOST_CHECK_EQUAL(request.query->primary_key, 3u);
      queries.push_back(*request.query);
   }
   BOOST_CHECK_THROW(partial.seal(), primary_state_invalid_attempt);
   queries.push_back({accounts, primary_query_kind::find, 7});
   queries.push_back({accounts, primary_query_kind::find, 2});
   base = frozen_primary_state::capture_queries(view, {}, queries);
   primary_state_overlay complete(base);
   complete.remove(accounts, 3);
   BOOST_CHECK_EQUAL(complete.locate_primary(first).row->primary_key, 7u);
   BOOST_CHECK(complete.read_primary(first, 5).row->value == std::vector<char>({'s','e','v','e','n'}));
   complete.store(accounts, 2, "bob"_n, std::string("two"));
   BOOST_CHECK_EQUAL(complete.locate_primary(first).row->primary_key, 2u);
   const auto result = complete.seal();
   BOOST_CHECK(result.read_queries().contains({accounts, primary_query_kind::upper_bound, 3}));
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   put(1, "phantom");
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
}

BOOST_FIXTURE_TEST_CASE(lifecycle_mixed_operations_match_independent_ordered_map, view_fixture) {
   put(0, "zero"); put(3, "three"); put(max_key, "max");
   const std::array tables{accounts};
   auto base = frozen_primary_state::capture(view, tables);
   for (uint32_t seed : {7, 81, 1907}) {
      std::mt19937 random(seed);
      primary_state_overlay state(base);
      std::map<uint64_t, std::pair<std::string, name>> expected{{0, {"zero", "bob"_n}},
         {3, {"three", "bob"_n}}, {max_key, {"max", "bob"_n}}};
      for (unsigned step = 0; step < 128; ++step) {
         const uint64_t key = step % 17 == 0 ? max_key : random() % 13;
         if (const auto it = expected.find(key); it == expected.end()) {
            const auto value = std::to_string(step);
            state.store(accounts, key, "alice"_n, value); expected[key] = {value, "alice"_n};
         } else if (random() % 2) {
            state.remove(accounts, key); expected.erase(it);
         } else {
            const std::string value(step % 9, 'x');
            state.update_existing(accounts, key, "carol"_n, value); it->second = {value, "carol"_n};
         }
         const auto table = state.read_table(accounts);
         BOOST_CHECK_EQUAL(bool(table), !expected.empty());
         if (table) BOOST_CHECK_EQUAL(table->count, expected.size());
         for (const uint64_t sought : {uint64_t{0}, uint64_t{3}, uint64_t{12}, max_key}) {
            for (const auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                  primary_query_kind::upper_bound, primary_query_kind::previous, primary_query_kind::last}) {
               auto it = expected.end();
               switch (kind) {
                  case primary_query_kind::find: it = expected.find(sought); break;
                  case primary_query_kind::lower_bound: it = expected.lower_bound(sought); break;
                  case primary_query_kind::upper_bound: it = expected.upper_bound(sought); break;
                  case primary_query_kind::previous:
                     it = expected.lower_bound(sought); it = it == expected.begin() ? expected.end() : std::prev(it); break;
                  case primary_query_kind::last: if (!expected.empty()) it = std::prev(expected.end()); break;
               }
               const primary_query query{accounts, kind, sought};
               const auto actual = state.read_primary(query, 64);
               BOOST_REQUIRE_EQUAL(bool(actual.row), it != expected.end());
               BOOST_CHECK(actual.table == table);
               if (actual.row) {
                  BOOST_CHECK_EQUAL(actual.row->primary_key, it->first);
                  BOOST_CHECK(actual.row->payer == it->second.second);
                  BOOST_CHECK(actual.row->value == std::vector<char>(it->second.first.begin(), it->second.first.end()));
                  BOOST_CHECK_EQUAL(actual.row->table_object_id, table->object_id);
               }
               const auto prefix = state.read_primary_prefix(query, 2);
               BOOST_CHECK(prefix.metadata == state.locate_primary(query));
               if (actual.row) BOOST_CHECK(prefix.value == std::vector<char>(actual.row->value.begin(),
                  actual.row->value.begin() + std::min<size_t>(2, actual.row->value.size())));
            }
         }
      }
      const auto result = state.seal();
      BOOST_CHECK_EQUAL(result.writes().size(), 128u);
      BOOST_CHECK(result.matches_base_reads(view, [] {}));
   }
   BOOST_CHECK_EQUAL(view.read_table(accounts)->count, 3u);
}

BOOST_FIXTURE_TEST_CASE(overlay_tracks_absent_points_tables_and_empty_ranges, view_fixture) {
   put(1, "aaa");
   const table_key absent{"token"_n, "missing"_n, "accounts"_n};
   const std::array keys{accounts, absent};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay overlay(base);
   BOOST_CHECK(!overlay.read_table(absent));
   BOOST_CHECK(!overlay.locate_primary({accounts, primary_query_kind::find, 9}).row);
   BOOST_CHECK(!overlay.read_primary({accounts, primary_query_kind::upper_bound, max_key}, 0).row);
   BOOST_CHECK(!overlay.read_primary_prefix({absent, primary_query_kind::last}, 0).metadata.row);
   const auto result = overlay.seal();
   BOOST_CHECK_EQUAL(result.read_tables().size(), 2u);
   BOOST_CHECK(result.read_tables().contains(accounts));
   BOOST_CHECK(result.read_tables().contains(absent));
   BOOST_CHECK(result.writes().empty());
}

BOOST_FIXTURE_TEST_CASE(unsupported_update_discards_whole_attempt_not_just_last_write, view_fixture) {
   put(1, "abc");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   for (unsigned mode = 0; mode < 2; ++mode) {
      primary_state_overlay overlay(base);
      overlay.update_existing(accounts, 1, name{}, std::string("def"));
      const table_key key = mode == 1 ? table_key{"other"_n, "alice"_n, "accounts"_n} : accounts;
      BOOST_CHECK_THROW(overlay.update_existing(key, mode == 0 ? 9 : 1,
                        name{}, std::string("xyz")),
                        primary_state_unsupported);
      BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(overlay.read_table(accounts), primary_state_invalid_attempt);
   }
   BOOST_CHECK(read(primary_query_kind::find, 1).row->value == std::vector<char>({'a', 'b', 'c'}));
}

BOOST_FIXTURE_TEST_CASE(precise_read_guard_allows_unread_rows_but_checks_full_selected_value, view_fixture) {
   put(1, "abc"); put(2, "def");
   const std::array keys{accounts};
   primary_state_overlay overlay(frozen_primary_state::capture(view, keys));
   // Even a prefix or metadata lookup currently conservatively depends on the
   // entire selected row, not merely the bytes copied to WASM memory.
   BOOST_CHECK_EQUAL(overlay.read_primary_prefix({accounts, primary_query_kind::find, 1}, 1).value.size(), 1u);
   const auto result = overlay.seal();
   unsigned checkpoints = 0;
   const auto check = [&] { ++checkpoints; };
   BOOST_CHECK(result.matches_base_reads(view, check));
   BOOST_CHECK_EQUAL(checkpoints, 2u);
   put(2, "xyz");
   BOOST_CHECK(result.matches_base_reads(view, check));
   put(1, "abx");
   BOOST_CHECK(!result.matches_base_reads(view, check));
   put(1, "abc");
   BOOST_CHECK(result.matches_base_reads(view, check)); // coordinator separately tracks same-row ABA writes
   BOOST_CHECK_THROW(result.matches_base_reads(view, [] { throw std::runtime_error("deadline"); }), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(table_only_dependencies_ignore_values_not_metadata, view_fixture) {
   put(1, "abc");
   const std::array keys{accounts};
   primary_state_overlay overlay(frozen_primary_state::capture(view, keys));
   overlay.read_table(accounts);
   const auto result = overlay.seal();
   BOOST_CHECK(result.read_queries().empty());
   put(1, "xyz");
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   db.modify(table(), [](auto& obj) { obj.payer = "bob"_n; });
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
}

BOOST_FIXTURE_TEST_CASE(guard_rechecks_every_selection_but_copies_shared_payload_only_once, view_fixture) {
   put(9, std::string(65536, 'x'));
   const std::array tables{accounts};
   primary_state_overlay overlay(frozen_primary_state::capture(view, tables));
   for (uint64_t key = 0; key < 10; ++key)
      overlay.locate_primary({accounts, primary_query_kind::lower_bound, key});
   const auto result = overlay.seal();
   struct counted_view final : transaction_state_view {
      const transaction_state_view& source;
      mutable unsigned selections = 0, payloads = 0;
      explicit counted_view(const transaction_state_view& source) : source(source) {}
      std::optional<account_read> read_account(name account) const override { return source.read_account(account); }
      std::optional<table_read> read_table(const table_key& key) const override { return source.read_table(key); }
      primary_metadata_result locate_primary(const primary_query& query) const override {
         ++selections; return source.locate_primary(query);
      }
      primary_prefix_result read_primary_prefix(const primary_query& query, size_t size) const override {
         return source.read_primary_prefix(query, size);
      }
      primary_read_result read_primary(const primary_query& query, size_t size) const override {
         ++payloads; return source.read_primary(query, size);
      }
   } counted(view);
   BOOST_CHECK(result.matches_base_reads(counted, [] {}));
   BOOST_CHECK_EQUAL(counted.selections, 10u);
   BOOST_CHECK_EQUAL(counted.payloads, 1u);
}

BOOST_FIXTURE_TEST_CASE(precise_guard_rejects_absent_point_and_range_phantoms_with_unchanged_count, view_fixture) {
   for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                     primary_query_kind::upper_bound, primary_query_kind::previous, primary_query_kind::last}) {
      put(1, "aaa"); put(9, "bbb");
      const std::array keys{accounts};
      primary_state_overlay overlay(frozen_primary_state::capture(view, keys));
      // find/lower/upper query 5, previous queries 9, last ignores its key.
      overlay.locate_primary({accounts, kind, kind == primary_query_kind::previous ? 9u : 5u});
      const auto result = overlay.seal();
      BOOST_CHECK(result.matches_base_reads(view, [] {}));
      if (kind == primary_query_kind::last) { erase(1); put(11, "ccc"); }
      else { erase(1); put(6, "ccc"); }
      if (kind == primary_query_kind::find) { erase(6); put(5, "ccc"); }
      BOOST_CHECK_EQUAL(table().count, 2u);
      BOOST_CHECK(!result.matches_base_reads(view, [] {}));
      erase(5); erase(6); erase(9); erase(11);
   }
}

BOOST_FIXTURE_TEST_CASE(precise_guard_rejects_row_identity_payer_size_and_table_recreation, view_fixture) {
   const std::array keys{accounts};
   for (unsigned mode = 0; mode < 4; ++mode) {
      put(1, "abc");
      primary_state_overlay overlay(frozen_primary_state::capture(view, keys));
      overlay.locate_primary({accounts, primary_query_kind::find, 1});
      const auto result = overlay.seal();
      if (mode == 0) { erase(1); put(1, "abc"); }
      if (mode == 1) db.modify(put(1, "abc"), [](auto& obj) { obj.payer = "carol"_n; });
      if (mode == 2) put(1, "abcd");
      if (mode == 3) { erase(1); db.remove(table()); put(1, "abc"); }
      BOOST_CHECK(!result.matches_base_reads(view, [] {}));
      erase(1);
   }
}

BOOST_FIXTURE_TEST_CASE(query_budget_deduplicates_and_poisoning_cannot_be_sealed, view_fixture) {
   put(1, "abc");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay overlay(base, {1, 1, 3, 2});
   for (unsigned i = 0; i < 20; ++i) {
      overlay.locate_primary({accounts, primary_query_kind::last, i});
      overlay.locate_primary({accounts, primary_query_kind::find, 1});
   }
   BOOST_CHECK_EQUAL(overlay.seal().read_queries().size(), 2u);
   primary_state_overlay limited(base, {1, 1, 3, 1});
   limited.locate_primary({accounts, primary_query_kind::find, 1});
   BOOST_CHECK_THROW(limited.locate_primary({accounts, primary_query_kind::find, 2}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(limited.seal(), primary_state_invalid_attempt);
   primary_state_overlay zero(base, {1, 1, 3, 0});
   BOOST_CHECK(zero.read_table(accounts));
   BOOST_CHECK_THROW(zero.locate_primary({accounts, primary_query_kind::find, 1}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(zero.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(account_existence_is_owned_tracked_and_not_inferred_from_tables, view_fixture) {
   put(1); // alice has a scope, but no account object
   const auto& bob = db.create<account_object>([](auto& obj) { obj.name = "bob"_n; });
   const auto& metadata = db.create<account_metadata_object>([](auto& obj) { obj.name = "bob"_n; });
   const std::array names{"alice"_n, "bob"_n};
   auto base = frozen_primary_state::capture_queries(view, {}, {}, {}, {}, names);
   BOOST_CHECK_EQUAL(base->account_count(), 2u);
   BOOST_CHECK(!base->read_account("alice"_n));
   BOOST_REQUIRE(base->read_account("bob"_n));
   BOOST_CHECK_EQUAL(base->read_account("bob"_n)->object_id, bob.id._id);
   BOOST_CHECK_THROW(base->read_account("carol"_n), primary_state_missing);
   primary_state_overlay overlay(base);
   BOOST_CHECK(!overlay.read_account("alice"_n));
   BOOST_CHECK(overlay.read_account("bob"_n));
   BOOST_CHECK(overlay.read_account("bob"_n));
   const auto result = overlay.seal();
   BOOST_CHECK_EQUAL(result.read_accounts().size(), 2u);
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   db.modify(metadata, [](auto& obj) { ++obj.recv_sequence; ++obj.auth_sequence; ++obj.abi_sequence; });
   db.modify(bob, [](auto& obj) { obj.abi.assign("changed", 7); });
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   auto undo = db.start_undo_session(true);
   db.create<account_object>([](auto& obj) { obj.name = "alice"_n; });
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
   BOOST_CHECK(!base->read_account("alice"_n));
   undo.undo();
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   // Canonical accounts cannot be deleted. Exercise the identity guard anyway.
   db.remove(bob);
   db.create<account_object>([](auto& obj) { obj.name = "bob"_n; });
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
}

BOOST_FIXTURE_TEST_CASE(account_budgets_unknown_requests_and_thread_affinity_are_fail_closed, view_fixture) {
   const std::array names{"alice"_n, "bob"_n};
   primary_snapshot_limits snapshot_limits; snapshot_limits.max_accounts = 1;
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view, {}, {}, snapshot_limits, {}, names), primary_state_budget_exceeded);
   unsigned checkpoints = 0;
   auto base = frozen_primary_state::capture_queries(view, {}, {}, {}, [&] { ++checkpoints; }, names);
   BOOST_CHECK_EQUAL(checkpoints, 2u);
   primary_overlay_limits limits; limits.max_read_accounts = 1;
   primary_state_overlay overlay(base, limits);
   BOOST_CHECK(!overlay.read_account("alice"_n));
   BOOST_CHECK(!overlay.read_account("alice"_n));
   BOOST_CHECK_THROW(overlay.read_account("bob"_n), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
   primary_state_overlay unknown(base);
   try { unknown.read_account("carol"_n); BOOST_FAIL("unknown account was treated as absent"); }
   catch (const primary_state_missing& missing) {
      BOOST_REQUIRE(std::holds_alternative<account_capture_request>(missing.request));
      BOOST_CHECK(std::get<account_capture_request>(missing.request).account == "carol"_n);
   }
   BOOST_CHECK_THROW(unknown.seal(), primary_state_invalid_attempt);
   BOOST_CHECK(std::async(std::launch::async, [&] {
      try { view.read_account("alice"_n); return false; }
      catch (const state_view_wrong_thread&) { return true; }
   }).get());
   primary_state_overlay owned(base);
   BOOST_CHECK(std::async(std::launch::async, [&] {
      try { owned.read_account("alice"_n); return false; }
      catch (const state_view_wrong_thread&) { return true; }
   }).get());
   BOOST_CHECK(!owned.read_account("alice"_n)); // wrong-thread read did not poison owner
   BOOST_CHECK(!std::async(std::launch::async, [base] { return base->read_account("alice"_n).has_value(); }).get());
}

BOOST_FIXTURE_TEST_CASE(sparse_capture_only_copies_requested_rows_from_a_large_table, view_fixture) {
   for (uint64_t key = 0; key < 4100; ++key) put(key, "abc");
   const std::array tables{accounts};
   BOOST_CHECK_THROW(frozen_primary_state::capture(view, tables), primary_state_budget_exceeded);
   const std::array queries{
      primary_query{accounts, primary_query_kind::find, 17},
      primary_query{accounts, primary_query_kind::lower_bound, 17},
      primary_query{accounts, primary_query_kind::last, 123}};
   auto base = frozen_primary_state::capture_queries(view, tables, queries, {1, 2, 6, 3});
   BOOST_CHECK_EQUAL(base->row_count(), 2u);
   BOOST_CHECK_EQUAL(base->value_bytes(), 6u);
   BOOST_CHECK_EQUAL(base->read_table(accounts)->count, 4100u);
   for (const auto& query : queries) BOOST_CHECK(base->read_primary(query, 3) == reference_read(db, query));
   BOOST_CHECK(base->read_primary({accounts, primary_query_kind::last, 999}, 3) == reference_read(db, queries.back()));
   BOOST_CHECK_THROW(base->locate_primary({accounts, primary_query_kind::find, 18}), primary_state_missing);
   BOOST_CHECK_THROW(base->locate_primary({accounts, primary_query_kind::upper_bound, 17}), primary_state_missing);
}

BOOST_FIXTURE_TEST_CASE(sparse_capture_distinguishes_unknown_from_absent_tables_points_and_ranges, view_fixture) {
   put(1, "abc"); put(9, "def");
   const table_key absent{"token"_n, "missing"_n, "accounts"_n};
   const std::array queries{
      primary_query{accounts, primary_query_kind::find, 5},
      primary_query{accounts, primary_query_kind::upper_bound, 9},
      primary_query{accounts, primary_query_kind::previous, 1},
      primary_query{absent, primary_query_kind::last, 0}};
   const auto base = frozen_primary_state::capture_queries(view, {}, queries);
   BOOST_CHECK_EQUAL(base->table_count(), 2u);
   BOOST_CHECK_EQUAL(base->row_count(), 0u);
   for (const auto& query : queries) BOOST_CHECK(base->locate_primary(query) == view.locate_primary(query));
   BOOST_CHECK(!base->locate_primary({absent, primary_query_kind::find, 123}).table);
   BOOST_CHECK_THROW(base->locate_primary({accounts, primary_query_kind::find, 6}), primary_state_missing);
   BOOST_CHECK_THROW(base->locate_primary({accounts, primary_query_kind::lower_bound, 9}), primary_state_missing);
   primary_state_overlay overlay(base);
   overlay.locate_primary(queries[0]);
   const auto result = overlay.seal();
   erase(1); put(5, "ghi"); // same count, but the captured negative point is no longer absent
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
}

BOOST_FIXTURE_TEST_CASE(sparse_capture_limits_and_metadata_only_requests_are_fail_closed, view_fixture) {
   put(1, "abc");
   const std::array tables{accounts};
   const std::array queries{primary_query{accounts, primary_query_kind::find, 1}};
   const auto metadata_only = frozen_primary_state::capture_queries(view, tables, {});
   BOOST_CHECK_EQUAL(metadata_only->row_count(), 0u);
   BOOST_CHECK(metadata_only->read_table(accounts) == view.read_table(accounts));
   BOOST_CHECK_THROW(metadata_only->locate_primary(queries[0]), primary_state_missing);
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view, {}, queries, {0, 1, 3, 1}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view, {}, queries, {1, 0, 3, 1}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view, {}, queries, {1, 1, 2, 1}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view, {}, queries, {1, 1, 3, 0}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view, {}, queries, {}, [] {
      throw std::runtime_error("capture interrupted");
   }), std::runtime_error);
   auto worker = std::async(std::launch::async, [&] {
      try { frozen_primary_state::capture_queries(view, {}, queries); }
      catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(worker.get());
   BOOST_CHECK_EQUAL(read(primary_query_kind::find, 1).row->value.size(), 3u);
}

// Exercise the controller's joined-discovery capture contract, not a reusable
// cache across canonical writes. The old returned versions stay worker-safe.
BOOST_FIXTURE_TEST_CASE(joined_capture_reuses_reads_without_extending_old_versions, view_fixture) {
   put(1, std::string(4096, 'a')); put(9, std::string(4096, 'b'));
   const primary_query first{accounts, primary_query_kind::find, 1};
   const primary_query second{accounts, primary_query_kind::find, 9};
   frozen_primary_capture_session capture(view);
   const auto a = capture.capture({}, std::array{first}, {});
   const auto b = capture.capture({}, std::array{first, second}, {});
   BOOST_CHECK_THROW(a->locate_primary(second), primary_state_missing);
   BOOST_CHECK(b->read_primary(second, 4096) == reference_read(db, second));
   BOOST_CHECK(a->read_primary(first, 4096) == b->read_primary(first, 4096));
   const auto c = capture.capture({}, std::array{first, second}, {});
   BOOST_CHECK_EQUAL(c->row_count(), 2u);
   BOOST_CHECK_EQUAL(c->value_bytes(), 8192u);
   BOOST_CHECK_EQUAL(capture.stats().tables_read, 1u);
   BOOST_CHECK_EQUAL(capture.stats().primary_queries_read, 2u);
   BOOST_CHECK_EQUAL(capture.stats().primary_rows_copied, 2u); // not five copies/20 KiB
   BOOST_CHECK(std::async(std::launch::async, [a, b, first, second] {
      return a->read_primary(first, 4096) == b->read_primary(first, 4096) &&
             b->read_primary(second, 4096).row->value.size() == 4096;
   }).get());
}

BOOST_FIXTURE_TEST_CASE(joined_capture_preserves_negative_reads_and_unknown_queries, view_fixture) {
   put(1); put(9);
   const table_key absent{"token"_n, "missing"_n, "accounts"_n};
   const primary_query negative{accounts, primary_query_kind::find, 5};
   const primary_query range{accounts, primary_query_kind::lower_bound, 5};
   frozen_primary_capture_session capture(view);
   const auto a = capture.capture(std::array{absent}, std::array{negative}, {});
   const auto b = capture.capture({}, std::array{range}, {});
   BOOST_CHECK(!b->locate_primary(negative).row);
   BOOST_CHECK(!b->locate_primary({absent, primary_query_kind::last, 123}).table);
   BOOST_CHECK_EQUAL(b->locate_primary(range).row->primary_key, 9u);
   BOOST_CHECK_THROW(a->locate_primary(range), primary_state_missing);
   BOOST_CHECK_THROW(b->locate_primary({accounts, primary_query_kind::find, 6}), primary_state_missing);
   BOOST_CHECK_THROW(b->locate_primary({accounts, primary_query_kind::previous, 9}), primary_state_missing);
}

BOOST_FIXTURE_TEST_CASE(joined_capture_budgets_cover_the_union_not_only_each_extension, view_fixture) {
   put(1, "abc"); put(9, "def");
   const primary_query a{accounts, primary_query_kind::find, 1};
   const primary_query b{accounts, primary_query_kind::find, 9};
   const primary_query negative{accounts, primary_query_kind::find, 7};
   for (int boundary = 0; boundary != 3; ++boundary) {
      auto limits = primary_snapshot_limits{1, 2, 6, 2, 0};
      if (boundary == 0) limits.max_rows = 1;
      if (boundary == 1) limits.max_value_bytes = 5;
      if (boundary == 2) limits.max_queries = 1;
      frozen_primary_capture_session capture(view);
      const auto first = capture.capture({}, std::array{a}, {}, limits);
      BOOST_CHECK_THROW(capture.capture({}, std::array{b}, {}, limits), primary_state_budget_exceeded);
      BOOST_CHECK_EQUAL(first->row_count(), 1u);
      BOOST_CHECK_THROW(first->locate_primary(b), primary_state_missing);
      BOOST_CHECK(capture.capture({}, std::array{a}, {}, limits)->read_primary(a, 3) == reference_read(db, a));
   }
   frozen_primary_capture_session capture(view);
   capture.capture({}, std::array{a}, {}, {1, 2, 6, 2, 1}, {}, std::array{"alice"_n});
   BOOST_CHECK_THROW(capture.capture({}, {}, {}, {1, 2, 6, 2, 1}, {}, std::array{"bob"_n}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(capture.capture(std::array{table_key{"other"_n, "alice"_n, "accounts"_n}}, {}, {},
                                    {1, 2, 6, 2, 1}), primary_state_budget_exceeded);
   capture.capture({}, std::array{negative}, {}, {1, 2, 6, 2, 1});
   BOOST_CHECK_THROW(capture.capture({}, std::array{b}, {}, {1, 2, 6, 2, 1}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(capture.capture({}, {}, {}, {0, 2, 6, 2, 1}), primary_state_budget_exceeded);
}

BOOST_FIXTURE_TEST_CASE(joined_capture_cancellation_and_wrong_thread_do_not_publish_partial_state, view_fixture) {
   put(1); put(9);
   const primary_query a{accounts, primary_query_kind::find, 1};
   const primary_query b{accounts, primary_query_kind::find, 9};
   frozen_primary_capture_session capture(view);
   const auto first = capture.capture({}, std::array{a}, {});
   unsigned checkpoints = 0;
   BOOST_CHECK_THROW(capture.capture({}, std::array{b}, {}, {}, [&] {
      if (++checkpoints == 2) throw std::runtime_error("capture interrupted");
   }), std::runtime_error);
   const auto retained = capture.capture({}, {}, {});
   BOOST_CHECK_THROW(retained->locate_primary(b), primary_state_missing);
   BOOST_CHECK_EQUAL(retained->row_count(), 1u);
   BOOST_CHECK(std::async(std::launch::async, [&] {
      try { capture.capture({}, {}, {}); } catch (const state_view_wrong_thread&) { return true; }
      return false;
   }).get());
   BOOST_CHECK(std::async(std::launch::async, [&] {
      try { capture.stats(); } catch (const state_view_wrong_thread&) { return true; }
      return false;
   }).get());
   const auto second = capture.capture({}, std::array{b}, {});
   BOOST_CHECK_THROW(first->locate_primary(b), primary_state_missing);
   BOOST_CHECK(second->read_primary(b, 5) == reference_read(db, b));
}

BOOST_FIXTURE_TEST_CASE(joined_capture_secondary_and_primary_share_union_limits_and_immutable_versions, view_fixture) {
   const auto& t = table();
   put(1, "abc"); put(9, "def");
   for (uint64_t key : {1u, 9u}) db.create<index64_object>([&](auto& row) {
      row.t_id = t.id; row.primary_key = key; row.secondary_key = 12; row.payer = "alice"_n;
   });
   const primary_query primary{accounts, primary_query_kind::find, 1};
   const secondary_query first{accounts, secondary_query_kind::find_secondary, uint64_t{12}, 0};
   const secondary_query next{accounts, secondary_query_kind::next, uint64_t{12}, 1};
   const primary_snapshot_limits exact{1, 3, 19, 3, 0};
   frozen_primary_capture_session capture(view);
   const auto a = capture.capture({}, std::array{primary}, std::array{first}, exact);
   const auto b = capture.capture({}, std::array{primary}, std::array{first, next}, exact);
   BOOST_CHECK_EQUAL(b->read_secondary(first).row->primary_key, 1u);
   BOOST_CHECK_EQUAL(b->read_secondary(next).row->primary_key, 9u);
   BOOST_CHECK_THROW(a->read_secondary(next), primary_state_missing);
   BOOST_CHECK_EQUAL(b->row_count(), 3u);
   BOOST_CHECK_EQUAL(b->value_bytes(), 19u);
   BOOST_CHECK_EQUAL(capture.stats().primary_rows_copied, 1u);
   BOOST_CHECK_EQUAL(capture.stats().secondary_queries_read, 2u);
   BOOST_CHECK_EQUAL(capture.stats().tables_read, 1u);
   BOOST_CHECK_THROW(capture.capture({}, std::array{primary_query{accounts, primary_query_kind::find, 9}}, {}, exact),
                     primary_state_budget_exceeded);
}

BOOST_FIXTURE_TEST_CASE(new_capture_session_observes_current_prefix_and_undo_not_stale_balances, view_fixture) {
   const primary_query query{accounts, primary_query_kind::find, 1};
   put(1, "old");
   std::shared_ptr<const frozen_primary_state> old;
   { frozen_primary_capture_session capture(view); old = capture.capture({}, std::array{query}, {}); }
   auto undo = db.start_undo_session(true);
   put(1, "new");
   std::shared_ptr<const frozen_primary_state> changed;
   { frozen_primary_capture_session capture(view); changed = capture.capture({}, std::array{query}, {}); }
   BOOST_CHECK(changed->read_primary(query, 3) == reference_read(db, query));
   BOOST_CHECK(old->read_primary(query, 3) != changed->read_primary(query, 3));
   undo.undo();
   frozen_primary_capture_session after_undo(view);
   BOOST_CHECK(after_undo.capture({}, std::array{query}, {})->read_primary(query, 3) == old->read_primary(query, 3));
}

BOOST_FIXTURE_TEST_CASE(overlay_budget_failure_cannot_be_sealed, view_fixture) {
   put(1, "abc");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay no_reads(base, {0, 1, 3});
   BOOST_CHECK_THROW(no_reads.read_table(accounts), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(no_reads.seal(), primary_state_invalid_attempt);
   primary_state_overlay no_writes(base, {1, 0, 3});
   BOOST_CHECK_THROW(no_writes.update_existing(accounts, 1, name{}, std::string("abc")), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(no_writes.seal(), primary_state_invalid_attempt);
   primary_state_overlay small(base, {1, 2, 5});
   small.update_existing(accounts, 1, name{}, std::string("def"));
   BOOST_CHECK_THROW(small.update_existing(accounts, 1, name{}, std::string("ghi")), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(small.seal(), primary_state_invalid_attempt);
   primary_state_overlay read_limit(base);
   BOOST_CHECK_THROW(read_limit.read_primary({accounts, primary_query_kind::find, 1}, 2), state_view_read_limit_exceeded);
   BOOST_CHECK_THROW(read_limit.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(overlay_zero_length_rows_still_count_as_write_operations, view_fixture) {
   put(1, "");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys, {1, 1, 0});
   primary_state_overlay overlay(base, {1, 1, 0});
   overlay.update_existing(accounts, 1, name{}, {});
   BOOST_CHECK(overlay.read_primary({accounts, primary_query_kind::find, 1}, 0).row->value.empty());
   BOOST_CHECK_THROW(overlay.update_existing(accounts, 1, name{}, {}), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(overlay_misuse_and_unknown_reads_are_rejected, view_fixture) {
   BOOST_CHECK_THROW(primary_state_overlay(nullptr), primary_state_invalid_attempt);
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay overlay(base);
   auto worker = std::async(std::launch::async, [&] {
      try { (void)overlay.seal(); } catch (const state_view_wrong_thread&) { return true; }
      return false;
   });
   BOOST_CHECK(worker.get());
   BOOST_CHECK(!overlay.read_table(accounts));
   const table_key unknown{"other"_n, "alice"_n, "accounts"_n};
   BOOST_CHECK_THROW(overlay.read_table(unknown), primary_state_unsupported);
   BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
   primary_state_overlay invalid_query(base);
   BOOST_CHECK_THROW(invalid_query.locate_primary({accounts, static_cast<primary_query_kind>(255)}), state_view_unsupported_query);
   BOOST_CHECK_THROW(invalid_query.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(four_workers_use_private_overlays_while_source_changes, view_fixture) {
   put(1, "original");
   const std::array keys{accounts};
   auto base = frozen_primary_state::capture(view, keys);
   std::barrier start(5);
   std::vector<std::future<primary_overlay_result>> workers;
   for (unsigned i = 0; i < 4; ++i) {
      workers.push_back(std::async(std::launch::async, [&, base, i] {
         start.arrive_and_wait();
         primary_state_overlay overlay(base);
         for (unsigned pass = 0; pass < 100; ++pass) {
            overlay.update_existing(accounts, 1, name{}, std::string(8, char('a' + i)));
            if (overlay.read_primary({accounts, primary_query_kind::find, 1}, 8).row->value != std::vector<char>(8, char('a' + i)))
               throw std::runtime_error("private overlay leaked across workers");
         }
         return overlay.seal();
      }));
   }
   start.arrive_and_wait();
   // No worker retains even a const reference into this database.
   erase(1); db.remove(table()); put(1, "replacement");
   for (unsigned i = 0; i < workers.size(); ++i) {
      const auto result = workers[i].get();
      BOOST_REQUIRE_EQUAL(result.writes().size(), 100u);
      BOOST_CHECK(require_primary_mutation(result.writes().back()).value == std::vector<char>(8, char('a' + i)));
      BOOST_CHECK_EQUAL(result.read_tables().size(), 1u);
   }
   BOOST_CHECK_EQUAL(base->locate_primary({accounts, primary_query_kind::find, 1}).row->value_size, 8u);
   BOOST_CHECK_EQUAL(read(primary_query_kind::find, 1).row->value.size(), 11u);
}

BOOST_FIXTURE_TEST_CASE(random_private_updates_match_independent_serial_database, view_fixture) {
   const std::array keys{accounts, table_key{"token"_n, "bob"_n, "accounts"_n},
                        table_key{"token"_n, "missing"_n, "accounts"_n}};
   for (unsigned table = 0; table < 2; ++table)
      for (unsigned primary = 0; primary < 20; ++primary) put(primary, "old", keys[table]);
   auto base = frozen_primary_state::capture(view, keys);
   primary_state_overlay overlay(base);
   std::mt19937_64 random(0x5058325f57524954ull);
   for (unsigned step = 0; step < 800; ++step) {
      const auto& key = keys[random() % 2];
      const auto primary = random() % 20;
      const std::string value{char(random() & 255), char(random() & 255), char(random() & 255)};
      overlay.update_existing(key, primary, step % 2 ? "bob"_n : name{}, value);
      put(primary, value, key); // original serial database is the independent oracle
      const auto query_key = step % 41 == 0 ? max_key : random() % 25;
      for (auto table : keys) {
         for (auto kind : {primary_query_kind::find, primary_query_kind::lower_bound,
                           primary_query_kind::upper_bound, primary_query_kind::last}) {
            const primary_query query{table, kind, query_key};
            BOOST_CHECK(overlay.read_primary(query, 3) == reference_read(db, query));
            BOOST_CHECK(overlay.locate_primary(query) == view.locate_primary(query));
            BOOST_CHECK(overlay.read_primary_prefix(query, step % 5) == view.read_primary_prefix(query, step % 5));
         }
      }
   }
   const auto result = overlay.seal();
   BOOST_CHECK_EQUAL(result.writes().size(), 800u);
   BOOST_CHECK_EQUAL(result.read_tables().size(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_primary_isolation_tests)
BOOST_FIXTURE_TEST_CASE(owned_range_row_proves_only_its_positive_primary_point, view_fixture) {
   put(1, "one"); put(7, "abc"); put(9, "end");
   const primary_query range{accounts, primary_query_kind::lower_bound, 5};
   const primary_query point{accounts, primary_query_kind::find, 7};
   const auto base = frozen_primary_state::capture_queries(view, {}, std::array{range}, {1, 1, 3, 1});
   const auto expected = view.read_primary(point, 3);
   BOOST_CHECK_EQUAL(base->row_count(), 1u);
   BOOST_CHECK_EQUAL(base->value_bytes(), 3u);
   // Even a logically derivable negative/range answer is outside this candidate.
   for (const auto& unknown : std::array{
         primary_query{accounts, primary_query_kind::find, 6},
         primary_query{accounts, primary_query_kind::find, 9},
         primary_query{accounts, primary_query_kind::upper_bound, 7},
         primary_query{accounts, primary_query_kind::lower_bound, 7},
         primary_query{accounts, primary_query_kind::previous, 7},
         primary_query{table_key{"token"_n, "bob"_n, "accounts"_n}, primary_query_kind::find, 7}})
      BOOST_CHECK_THROW(base->locate_primary(unknown), primary_state_missing);
   BOOST_REQUIRE_NO_THROW(base->read_primary(point, 3));
   BOOST_CHECK(base->read_primary(point, 3) == expected);
   primary_state_overlay overlay(base);
   primary_overlay_cursor cursor(overlay, "token"_n);
   const auto iterator = cursor.lower_bound(accounts, 5);
   std::array<char, 3> bytes{};
   BOOST_CHECK_EQUAL(cursor.get(iterator, bytes), 3);
   BOOST_CHECK(std::string(bytes.begin(), bytes.end()) == "abc");
   const auto result = overlay.seal();
   BOOST_CHECK_EQUAL(result.read_queries().size(), 2u); // original range AND derived point
   BOOST_CHECK(result.read_queries().contains(point));
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   put(9, "new"); // unrelated same-sized row update is not a point conflict
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   put(7, "xyz");
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
   BOOST_CHECK(base->read_primary(point, 3) == expected);
   put(7, "abc");
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   erase(7); put(7, "abc"); // same key/value/count, different row generation
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
   BOOST_CHECK(base->read_primary(point, 3) == expected);
}

BOOST_FIXTURE_TEST_CASE(derived_positive_point_still_consumes_overlay_query_budget, view_fixture) {
   put(7, "abc");
   const primary_query range{accounts, primary_query_kind::lower_bound, 5};
   const auto base = frozen_primary_state::capture_queries(view, {}, std::array{range}, {1, 1, 3, 1});
   primary_overlay_limits limits; limits.max_read_queries = 1;
   primary_state_overlay overlay(base, limits);
   BOOST_REQUIRE_NO_THROW(overlay.locate_primary({accounts, primary_query_kind::find, 7}));
   BOOST_CHECK_THROW(overlay.locate_primary(range), primary_state_budget_exceeded);
   BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_primary_isolation_tests)
BOOST_FIXTURE_TEST_CASE(derived_primary_point_observes_private_update_remove_and_recreation, view_fixture) {
   put(7, "abc"); put(9, "tail"); // keep the original table alive after removal
   const primary_query range{accounts, primary_query_kind::lower_bound, 5};
   const primary_query point{accounts, primary_query_kind::find, 7};
   const auto base = frozen_primary_state::capture_queries(view, {}, std::array{range}, {1, 1, 3, 1});
   const auto original = view.read_primary(point, 3);
   BOOST_REQUIRE(original.row);
   primary_state_overlay overlay(base);

   // Only the range was captured; every write below must still observe and
   // record the derived exact point before consulting the private journal.
   BOOST_REQUIRE_NO_THROW(overlay.update_existing(accounts, 7, "carol"_n, std::string("longer")));
   auto found = overlay.read_primary(point, 6);
   BOOST_REQUIRE(found.row);
   BOOST_CHECK(found.row->value == std::vector<char>({'l', 'o', 'n', 'g', 'e', 'r'}));
   BOOST_CHECK(found.row->payer == "carol"_n);
   BOOST_CHECK_EQUAL(found.row->object_id, original.row->object_id);
   overlay.remove(accounts, 7);
   found = overlay.read_primary(point, 6);
   BOOST_REQUIRE(found.table);
   BOOST_CHECK(!found.row);
   BOOST_CHECK_EQUAL(found.table->count, original.table->count - 1);
   overlay.store(accounts, 7, "alice"_n, std::string("new"));
   found = overlay.read_primary(point, 3);
   BOOST_REQUIRE(found.row);
   BOOST_CHECK_LT(found.row->object_id, 0);
   BOOST_CHECK(found.row->payer == "alice"_n);
   BOOST_CHECK_EQUAL(found.table->count, original.table->count);
   const auto recreated_id = found.row->object_id;
   overlay.update_existing(accounts, 7, name{}, {});
   found = overlay.read_primary(point, 0);
   BOOST_REQUIRE(found.row);
   BOOST_CHECK(found.row->value.empty());
   BOOST_CHECK_EQUAL(found.row->object_id, recreated_id);
   BOOST_CHECK(found.row->payer == "alice"_n); // same_payer sees the recreated row

   const auto result = overlay.seal();
   BOOST_CHECK_EQUAL(result.writes().size(), 4u);
   BOOST_CHECK_EQUAL(result.read_queries().size(), 1u);
   BOOST_CHECK(result.read_queries().contains(point));
   BOOST_CHECK(result.matches_base_reads(view, [] {}));
   BOOST_CHECK(view.read_primary(point, 3) == original); // no canonical write
   BOOST_CHECK(base->read_primary(point, 3) == original); // no frozen-state mutation
   put(7, "xyz");
   BOOST_CHECK(!result.matches_base_reads(view, [] {}));
   BOOST_CHECK(base->read_primary(point, 3) == original);
}
BOOST_AUTO_TEST_SUITE_END()



BOOST_AUTO_TEST_SUITE(parallel_borrowed_primary_validation_tests)
BOOST_FIXTURE_TEST_CASE(borrowed_compare_matches_owned_metadata_and_complete_bytes, view_fixture) {
   const std::array queries{
      primary_query{accounts, primary_query_kind::find, 0},
      primary_query{accounts, primary_query_kind::find, 7},
      primary_query{accounts, primary_query_kind::find, 9},
      primary_query{accounts, primary_query_kind::lower_bound, 5},
      primary_query{accounts, primary_query_kind::upper_bound, 7},
      primary_query{accounts, primary_query_kind::previous, 9},
      primary_query{accounts, primary_query_kind::last, max_key},
      primary_query{accounts, primary_query_kind::find, max_key},
      primary_query{table_key{"token"_n,"absent"_n,"accounts"_n},primary_query_kind::find,7}};
   for (const size_t size : {size_t{0},size_t{1},size_t{32},size_t{33},size_t{65536}}) {
      put(7,std::string(size,'x')); put(9,"");
      for (const auto& query : queries) {
         const auto expected=view.locate_primary(query);
         const auto bytes=view.read_primary(query,65536);
         const auto payload=bytes.row ? std::span<const char>{bytes.row->value} : std::span<const char>{};
         BOOST_REQUIRE(view.matches_primary(query,expected,payload));
         auto bad=expected;
         if (bad.table) {
            ++bad.table->count; BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
            bad.table->payer="carol"_n;BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
            ++bad.table->object_id;BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
         }
         if (bad.row) {
            ++bad.row->object_id;BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
            ++bad.row->table_object_id;BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
            ++bad.row->primary_key;BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
            bad.row->payer="carol"_n;BOOST_CHECK(!view.matches_primary(query,bad,payload));bad=expected;
            ++bad.row->value_size;BOOST_CHECK(!view.matches_primary(query,bad,payload));
            auto wrong=bytes.row->value;wrong.push_back('x');
            BOOST_CHECK(!view.matches_primary(query,expected,wrong));
            if (!payload.empty()) {
               wrong=bytes.row->value;wrong.back()^=1;
               BOOST_CHECK(!view.matches_primary(query,expected,wrong));
               BOOST_CHECK(!view.matches_primary(query,expected,payload.first(payload.size()-1)));
            }
         }
         auto forbidden=std::async(std::launch::async,[&] {
            try {view.matches_primary(query,expected,payload);return false;}
            catch(const state_view_wrong_thread&) {return true;}
         });
         BOOST_CHECK(forbidden.get());
         // Fresh same-view checks across actual canonical mutations and undo.
         {
            auto undo=db.start_undo_session(true);
            put(7,"changed");
            const auto actual=view.read_primary(query,65536);
            BOOST_CHECK(view.matches_primary(query,expected,payload)==(actual==bytes));
         }
         BOOST_CHECK(view.matches_primary(query,expected,payload));
      }
   }
   primary_query unsupported{accounts,static_cast<primary_query_kind>(255),0};
   BOOST_CHECK_THROW(view.matches_primary(unsupported,{},{}),state_view_unsupported_query);
}

BOOST_FIXTURE_TEST_CASE(inline_and_overflow_dedup_preserve_old_validation_and_generic_protocol, view_fixture) {
   // Cross both sides of the eight-ID inline boundary and exercise repeated
   // selections for IDs stored inline AND in the unbounded tree fallback.
   for (const unsigned rows : {0u,1u,8u,9u,32u}) {
      for (unsigned i=0;i<rows;++i) put(2*i+1,std::string(33,char(i)));
      const std::array tables{accounts};
      primary_state_overlay overlay(frozen_primary_state::capture(view,tables));
      overlay.read_table(accounts);
      for (unsigned i=0;i<rows;++i) {
         overlay.locate_primary({accounts,primary_query_kind::find,2*i+1});
         overlay.locate_primary({accounts,primary_query_kind::lower_bound,2*i});
      }
      const auto result=overlay.seal();
      struct counted_view final:transaction_state_view {
         const transaction_state_view& source;mutable unsigned payloads=0,selections=0;
         explicit counted_view(const transaction_state_view& s):source(s){}
         std::optional<account_read> read_account(name a)const override{return source.read_account(a);}
         std::optional<table_read> read_table(const table_key& k)const override{return source.read_table(k);}
         primary_metadata_result locate_primary(const primary_query& q)const override{
            ++selections;return source.locate_primary(q);}
         primary_prefix_result read_primary_prefix(const primary_query& q,size_t n)const override{
            return source.read_primary_prefix(q,n);}
         primary_read_result read_primary(const primary_query& q,size_t n)const override{
            ++payloads;return source.read_primary(q,n);}
      } counted(view);
      unsigned checkpoints=0;
      BOOST_CHECK(result.matches_base_reads(view,[&]{++checkpoints;}));
      BOOST_CHECK_EQUAL(checkpoints,1+rows*2);
      BOOST_CHECK(result.matches_base_reads(counted,[]{}));
      BOOST_CHECK_EQUAL(counted.selections,rows*2);BOOST_CHECK_EQUAL(counted.payloads,rows);
      for (unsigned i=0;i<rows;++i) {
         {
            auto undo=db.start_undo_session(true);put(2*i+1,"mutated");
            BOOST_CHECK(!result.matches_base_reads(view,[]{}));
            BOOST_CHECK(!result.matches_base_reads(counted,[]{}));
         }
         BOOST_CHECK(result.matches_base_reads(view,[]{}));
      }
      BOOST_CHECK_THROW(result.matches_base_reads(view,[]{throw std::runtime_error("deadline");}),std::runtime_error);
      for(unsigned i=0;i<rows;++i)erase(2*i+1);
   }
}
BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(parallel_single_capture_read_tests)
BOOST_FIXTURE_TEST_CASE(single_read_capture_matches_physical_oracle_across_budgets,view_fixture) {
   constexpr table_key absent{"token"_n,"absent"_n,"accounts"_n};
   constexpr table_key empty{"token"_n,"empty"_n,"accounts"_n};
   table(empty);
   for(unsigned i=0;i<16;++i)put(3*i+1,std::string(std::array<size_t,5>{0,1,31,32,33}[i%5],char(i)));
   put(max_key,std::string(65536,'z'));
   const std::array keys{accounts,empty,absent};
   std::vector<primary_query> queries;
   for(const auto& key:keys)
      for(auto kind:{primary_query_kind::find,primary_query_kind::lower_bound,
                    primary_query_kind::upper_bound,primary_query_kind::previous,primary_query_kind::last})
         for(uint64_t value:{uint64_t{0},uint64_t{1},uint64_t{5},uint64_t{46},max_key-1,max_key})
            queries.push_back({key,kind,value});
   // Duplicate selections must remain legal at an exhausted byte/row budget.
   for(unsigned i=0;i<10;++i)queries.push_back({accounts,primary_query_kind::lower_bound,0});
   BOOST_REQUIRE_EQUAL(queries.size(),100u);
   for(size_t tables:{size_t{2},size_t{3}})
   for(size_t rows:{size_t{0},size_t{1},size_t{8},size_t{17},size_t{32}})
   for(size_t bytes:{size_t{0},size_t{1},size_t{32},size_t{128},size_t{65536},size_t{1048576}})
   for(size_t count:{size_t{0},size_t{99},size_t{100},size_t{256}}) {
      primary_snapshot_limits limits;limits.max_tables=tables;limits.max_rows=rows;
      limits.max_value_bytes=bytes;limits.max_queries=count;
      bool allowed=keys.size()<=tables && queries.size()<=count;
      size_t expected_bytes=0;std::set<int64_t> selected_rows;
      std::set<primary_query,primary_query_less> selected_queries;
      if(allowed)for(auto query:queries) {
         if(query.kind==primary_query_kind::last)query.primary_key=0;
         if(!selected_queries.insert(query).second)continue;
         const auto expected=reference_read(db,query); // physical by_id walk
         if(expected.row && !selected_rows.contains(expected.row->object_id)) {
            if(selected_rows.size()==rows || expected.row->value.size()>bytes-expected_bytes) {
               allowed=false;break;
            }
            selected_rows.insert(expected.row->object_id);expected_bytes+=expected.row->value.size();
         }
      }
      if(!allowed) {
         BOOST_CHECK_THROW(frozen_primary_state::capture_queries(view,keys,queries,limits),primary_state_budget_exceeded);
         continue;
      }
      auto captured=frozen_primary_state::capture_queries(view,keys,queries,limits);
      BOOST_CHECK_EQUAL(captured->row_count(),selected_rows.size());
      BOOST_CHECK_EQUAL(captured->value_bytes(),expected_bytes);
      for(const auto& query:queries)BOOST_CHECK(captured->read_primary(query,65536)==reference_read(db,query));
   }
}

BOOST_FIXTURE_TEST_CASE(single_capture_duplicate_reuse_keeps_stats_and_exhausted_budget,view_fixture) {
   put(1,"");
   frozen_primary_capture_session session(view);
   primary_snapshot_limits limits;limits.max_tables=1;limits.max_rows=1;
   limits.max_value_bytes=0;limits.max_queries=3;
   const primary_query first{accounts,primary_query_kind::find,1};
   const std::array more{primary_query{accounts,primary_query_kind::lower_bound,0},
                         primary_query{accounts,primary_query_kind::upper_bound,0}};
   auto a=session.capture({},std::array{first},{},limits,[]{});
   auto b=session.capture({},more,{},limits,[]{});
   BOOST_CHECK_EQUAL(a->row_count(),1u);BOOST_CHECK_EQUAL(b->row_count(),1u);
   BOOST_CHECK_EQUAL(b->value_bytes(),0u);
   for(const auto& q:more)BOOST_CHECK(b->read_primary(q,0)==reference_read(db,q));
   BOOST_CHECK(a->read_primary(first,0)==reference_read(db,first));
   BOOST_CHECK_EQUAL(session.stats().tables_read,1u);
   BOOST_CHECK_EQUAL(session.stats().primary_rows_copied,1u);
   BOOST_CHECK_EQUAL(session.stats().primary_queries_read,3u);
   BOOST_CHECK_THROW(a->locate_primary(more.front()),primary_state_missing);
}
BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(parallel_small_local_container_tests)
BOOST_AUTO_TEST_CASE(inline_name_membership_and_catalog_match_ordered_oracles) {
   const auto check=[]<size_t N>() {
      small_seen_keys<name,N> seen;std::set<name> expected;
      small_name_map<int64_t,N> values;std::map<name,int64_t> oracle;
      BOOST_CHECK(seen.insert(name{}));expected.insert(name{});
      BOOST_CHECK(values.emplace(name{},-1));oracle.emplace(name{},-1);
      const auto* stable=values.find(name{});BOOST_REQUIRE(stable);
      std::mt19937_64 random(0x9767a11+N);
      for(unsigned i=0;i<10000;++i) {
         const name key{random()%257};const int64_t value=static_cast<int64_t>(random());
         BOOST_CHECK(seen.insert(key)==expected.insert(key).second);
         BOOST_CHECK_EQUAL(seen.size(),expected.size());
         BOOST_CHECK(values.emplace(key,value)==oracle.emplace(key,value).second);
         const auto* found=values.find(key);BOOST_REQUIRE(found);
         BOOST_CHECK_EQUAL(*found,oracle.at(key));BOOST_CHECK_EQUAL(values.size(),oracle.size());
         BOOST_CHECK(values.find(name{})==stable);BOOST_CHECK_EQUAL(*stable,-1);
      }
      BOOST_CHECK(!values.find(name{std::numeric_limits<uint64_t>::max()}));
   };
   check.template operator()<1>();check.template operator()<8>();
}
BOOST_AUTO_TEST_CASE(inline_signed_identity_membership_matches_tree_for_all_boundaries) {
   small_seen_keys<int64_t> seen;std::set<int64_t> expected;
   for(int64_t value:{int64_t{0},int64_t{-1},int64_t{1},std::numeric_limits<int64_t>::min(),std::numeric_limits<int64_t>::max()})
      BOOST_CHECK(seen.insert(value)==expected.insert(value).second);
   std::mt19937_64 random(0x899112);
   for(unsigned i=0;i<10000;++i) {
      const auto value=static_cast<int64_t>(random()%1024)-512;
      BOOST_CHECK(seen.insert(value)==expected.insert(value).second);
      BOOST_CHECK_EQUAL(seen.size(),expected.size());
   }
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_primary_validation_optimization_tests)
BOOST_FIXTURE_TEST_CASE(full_payload_single_lookup_matches_prior_validation_and_rejects_stale_state, view_fixture) {
   put(7, std::string("a\0b\xff", 4)); put(9, ""); put(11, "tail");
   constexpr table_key absent{"token"_n, "carol"_n, "accounts"_n};
   const std::array queries{
      primary_query{accounts, primary_query_kind::find, 7},
      primary_query{accounts, primary_query_kind::lower_bound, 5},
      primary_query{accounts, primary_query_kind::upper_bound, 7},
      primary_query{accounts, primary_query_kind::last, 0},
      primary_query{accounts, primary_query_kind::previous, 11},
      primary_query{accounts, primary_query_kind::find, max_key},
      primary_query{absent, primary_query_kind::find, 7}};
   const auto base = frozen_primary_state::capture_queries(view, {}, queries, {});
   primary_state_overlay overlay(base);
   for (const auto& query : queries) overlay.locate_primary(query);
   const auto result = overlay.seal();
   // Preserve the former two-lookup algorithm as an independent equivalence
   // oracle. Mutate the live database and undo after EVERY case, using the
   // SAME view/result to catch accidental cache lifetime extension as well.
   const auto previous = [&] {
      std::set<int64_t> checked;
      for (const auto& key : result.read_tables())
         if (view.read_table(key) != base->read_table(key)) return false;
      for (const auto& query : result.read_queries()) {
         const auto expected = base->locate_primary(query);
         if (view.locate_primary(query) != expected) return false;
         if (expected.row && checked.insert(expected.row->object_id).second &&
             view.read_primary(query, expected.row->value_size) !=
             base->read_primary(query, expected.row->value_size)) return false;
      }
      return true;
   };
   BOOST_REQUIRE(previous());
   BOOST_REQUIRE(result.matches_base_reads(view, [] {}));
   for (unsigned mutation = 0; mutation < 12; ++mutation) {
      {
         auto undo = db.start_undo_session(true);
         switch (mutation) {
            case 0: put(7, "abcd"); break; // same-size content
            case 1: put(7, "a"); break; // truncated
            case 2: put(7, "abcdef"); break; // grew beyond requested prefix
            case 3: put(9, "x"); break; // empty row grew
            case 4: put(7, ""); break; // became empty
            case 5: erase(7); put(7, std::string("a\0b\xff", 4)); break; // generation
            case 6: db.modify(table(), [](auto& t) { t.payer = "bob"_n; }); break;
            case 7: {
               const auto& t = table();
               const auto& row = db.get<key_value_object, by_scope_primary>(boost::make_tuple(t.id, 7));
               db.modify(row, [](auto& r) { r.payer = "carol"_n; }); break;
            }
            case 8: put(max_key, "new"); break; // former absence
            case 9: put(7, "new", absent); break; // former missing table
            case 10: erase(9); break; // range selection and empty row
            case 11: put(6, "new"); break; // lower-bound phantom
         }
         BOOST_CHECK(!previous());
         BOOST_CHECK(result.matches_base_reads(view, [] {}) == previous());
      }
      BOOST_CHECK(previous());
      BOOST_CHECK(result.matches_base_reads(view, [] {}));
   }
}
BOOST_AUTO_TEST_SUITE_END()
