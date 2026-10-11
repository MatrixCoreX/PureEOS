#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
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
      }
      if (match && (!best || (query.kind == primary_query_kind::last ? row.primary_key > best->primary_key
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
