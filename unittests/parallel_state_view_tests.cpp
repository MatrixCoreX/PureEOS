#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/contract_table_objects.hpp>
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
   BOOST_CHECK_EQUAL(result.writes()[0].primary_key, 1u);
   BOOST_CHECK_EQUAL(result.writes()[1].primary_key, 2u);
   BOOST_CHECK_EQUAL(result.writes()[2].primary_key, 1u);
   BOOST_CHECK(result.writes()[0].value == std::vector<char>(3, 'c'));
   BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(overlay.read_table(accounts), primary_state_invalid_attempt);
   BOOST_CHECK_THROW(overlay.update_existing(accounts, 1, name{}, std::string("fff")), primary_state_invalid_attempt);
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
   for (unsigned mode = 0; mode < 4; ++mode) {
      primary_state_overlay overlay(base);
      overlay.update_existing(accounts, 1, name{}, std::string("def"));
      const table_key key = mode == 3 ? table_key{"other"_n, "alice"_n, "accounts"_n} : accounts;
      BOOST_CHECK_THROW(overlay.update_existing(key, mode == 0 ? 9 : 1,
                        mode == 1 ? "carol"_n : name{}, mode == 2 ? std::string("long") : std::string("xyz")),
                        primary_state_unsupported);
      BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
      BOOST_CHECK_THROW(overlay.read_table(accounts), primary_state_invalid_attempt);
   }
   BOOST_CHECK(read(primary_query_kind::find, 1).row->value == std::vector<char>({'a', 'b', 'c'}));
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
      BOOST_CHECK(result.writes().back().value == std::vector<char>(8, char('a' + i)));
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
