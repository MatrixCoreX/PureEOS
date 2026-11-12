#include <eosio/chain/parallel_secondary_state.hpp>
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/parallel_secondary_journal.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/exceptions.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <future>
#include <limits>
#include <random>
#include <type_traits>

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
constexpr table_key secondary_rows{"token"_n,"alice"_n,"rows"_n};
constexpr uint64_t last_primary=std::numeric_limits<uint64_t>::max();
template<class Owned> struct test_mapping;
template<> struct test_mapping<uint64_t> {
   using object=index64_object;
   static uint64_t canonical(uint64_t v) {return v;}
   static uint64_t owned(uint64_t v) {return v;}
};
template<> struct test_mapping<uint128_t> {
   using object=index128_object;
   static uint128_t canonical(uint128_t v) {return v;}
   static uint128_t owned(uint128_t v) {return v;}
};
template<> struct test_mapping<std::array<uint128_t,2>> {
   using object=index256_object;
   static key256_t canonical(key256_t v) {return v;}
   static key256_t owned(key256_t v) {return v;}
};
template<> struct test_mapping<secondary_float64> {
   using object=index_double_object;
   static float64_t canonical(secondary_float64 v) {return {v.bits};}
   static secondary_float64 owned(float64_t v) {return {v.v};}
};
template<> struct test_mapping<secondary_float128> {
   using object=index_long_double_object;
   static float128_t canonical(secondary_float128 v) {
      float128_t out; fc::uint128_to_float128(v.bits,out); return out;
   }
   static secondary_float128 owned(float128_t v) {
      secondary_float128 out; fc::float128_to_uint128(v,out.bits); return out;
   }
};
secondary_key numbered(unsigned type,uint64_t value) {
   switch(type) {
      case 0:return value;
      case 1:return uint128_t{value};
      case 2:return key256_t{uint128_t{value},uint128_t{value+1}};
      case 3:return test_mapping<secondary_float64>::owned(ui64_to_f64(value));
      case 4:return test_mapping<secondary_float128>::owned(ui64_to_f128(value));
   }
   throw std::logic_error("invalid test key type");
}
secondary_query query(unsigned type,secondary_query_kind kind,uint64_t value=0,uint64_t primary=0,
                      table_key table=secondary_rows) {
   return {table,kind,numbered(type,value),primary};
}
template<class Canonical>
bool original_less(const Canonical& a,const Canonical& b) {
   if constexpr(std::is_same_v<Canonical,float64_t>) return soft_double_less{}(a,b);
   else if constexpr(std::is_same_v<Canonical,float128_t>) return soft_long_double_less{}(a,b);
   else return std::less<Canonical>{}(a,b);
}

// Independent selector: scan physical by_id objects, not the adapter's
// composite-key index lookup, then apply original key comparisons in a vector.
// This is state-layer coverage; it is NOT a WASM pointer/iterator/RAM oracle.
secondary_read_result reference_read(const chainbase::database& db,const secondary_query& q) {
   secondary_read_result out;
   const table_id_object* table=nullptr;
   for(const auto& t:db.get_index<table_id_multi_index,by_id>())
      if(t.code==q.table.code && t.scope==q.table.scope && t.table==q.table.table) {table=&t;break;}
   if(!table) return out;
   out.table=table_read{q.table,table->id._id,table->payer,table->count};
   std::visit([&](const auto& input) {
      using mapping=test_mapping<std::decay_t<decltype(input)>>;
      using object=typename mapping::object;
      using index=typename chainbase::get_index_type<object>::type;
      std::vector<const object*> rows;
      for(const auto& row:db.get_index<index,by_id>()) if(row.t_id==table->id) rows.push_back(&row);
      const auto less=[](const object* a,const object* b) {
         if(original_less(a->secondary_key,b->secondary_key)) return true;
         if(original_less(b->secondary_key,a->secondary_key)) return false;
         return a->primary_key<b->primary_key;
      };
      std::sort(rows.begin(),rows.end(),less);
      const auto value=mapping::canonical(input);
      const object* selected=nullptr;
      for(const auto* row:rows) {
         const bool before=original_less(row->secondary_key,value);
         const bool after=original_less(value,row->secondary_key);
         const bool equal=!before && !after;
         bool match=false;
         switch(q.kind) {
            case secondary_query_kind::find_primary:match=row->primary_key==q.primary_key;break;
            case secondary_query_kind::find_secondary:match=equal;break;
            case secondary_query_kind::lower_bound:match=!before;break;
            case secondary_query_kind::upper_bound:match=after;break;
            case secondary_query_kind::first:match=true;break;
            case secondary_query_kind::last:match=true;break;
            case secondary_query_kind::next:match=after || (equal && row->primary_key>q.primary_key);break;
            case secondary_query_kind::previous:match=before || (equal && row->primary_key<q.primary_key);break;
         }
         if(match) {
            selected=row;
            if(q.kind!=secondary_query_kind::last && q.kind!=secondary_query_kind::previous) break;
         }
      }
      if(selected) out.row=secondary_row_read{selected->id._id,selected->t_id._id,selected->primary_key,
                                             selected->payer,mapping::owned(selected->secondary_key)};
   },q.key);
   return out;
}
struct secondary_fixture {
   fc::temp_directory dir;
   chainbase::database db{dir.path(),chainbase::database::read_write,8*1024*1024};
   chainbase_transaction_state_view primary{db};
   chainbase_secondary_state_view view{db};
   secondary_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>();
      db.add_index<index64_index>(); db.add_index<index128_index>(); db.add_index<index256_index>();
      db.add_index<index_double_index>(); db.add_index<index_long_double_index>();
   }
   const table_id_object& table(const table_key& key=secondary_rows) {
      if(const auto* found=db.find<table_id_object,by_code_scope_table>(boost::make_tuple(key.code,key.scope,key.table)))
         return *found;
      return db.create<table_id_object>([&](auto& t) {
         t.code=key.code;t.scope=key.scope;t.table=key.table;t.payer="alice"_n;
      });
   }
   void put(uint64_t pk,const secondary_key& key,table_key table_name=secondary_rows,name payer="bob"_n) {
      const auto& t=table(table_name);
      std::visit([&](const auto& value) {
         using mapping=test_mapping<std::decay_t<decltype(value)>>;
         using object=typename mapping::object;
         if(const auto* found=db.find<object,by_primary>(boost::make_tuple(t.id,pk))) {
            db.modify(*found,[&](auto& r) {r.secondary_key=mapping::canonical(value);r.payer=payer;});
         } else {
            db.create<object>([&](auto& r) {r.t_id=t.id;r.primary_key=pk;r.secondary_key=mapping::canonical(value);r.payer=payer;});
            db.modify(t,[](auto& row){++row.count;});
         }
      },key);
   }
   void erase(uint64_t pk,const secondary_key& kind,table_key key=secondary_rows) {
      const auto* t=db.find<table_id_object,by_code_scope_table>(boost::make_tuple(key.code,key.scope,key.table));
      if(!t) return;
      std::visit([&](const auto& value) {
         using object=typename test_mapping<std::decay_t<decltype(value)>>::object;
         if(const auto* row=db.find<object,by_primary>(boost::make_tuple(t->id,pk))) {
            db.remove(*row);
            if(t->count==1) db.remove(*t);
            else db.modify(*t,[](auto& table){--table.count;});
         }
      },kind);
   }
   void compare(const secondary_query& q) {
      const auto result=view.read_secondary(q);
      BOOST_CHECK(result==reference_read(db,q));
      BOOST_CHECK(result.table==primary.read_table(q.table));
   }
};

// State-layer discovery only: the immutable source is never mutated by the
// callback. Unknown reads extend owned capture and restart the whole attempt.
// This is not a change to the controller's independent round/time budgets.
template<class F>
primary_overlay_result discover_overlay(const chainbase_transaction_state_view& source, F&& execute) {
   std::set<table_key,table_key_less> tables{secondary_rows};
   std::set<primary_query,primary_query_less> primary_queries;
   std::set<secondary_query,secondary_query_less> secondary_queries;
   for(unsigned round=0;round<512;++round) {
      const std::vector<table_key> keys(tables.begin(),tables.end());
      const std::vector<primary_query> pq(primary_queries.begin(),primary_queries.end());
      const std::vector<secondary_query> sq(secondary_queries.begin(),secondary_queries.end());
      const auto base=frozen_primary_state::capture_queries_with_secondary(source,keys,pq,sq);
      primary_state_overlay state(base);
      try { execute(state); return state.seal(); }
      catch(const primary_state_missing& missing) {
         bool extended=false;
         if(const auto* p=std::get_if<primary_capture_request>(&missing.request)) {
            extended=tables.insert(p->table).second;
            if(p->query) extended=primary_queries.insert(*p->query).second || extended;
         } else if(const auto* s=std::get_if<secondary_capture_request>(&missing.request))
            extended=secondary_queries.insert(s->query).second;
         if(!extended) throw std::logic_error("secondary test discovery made no progress");
      }
   }
   throw std::logic_error("secondary test discovery exceeded its test-only bound");
}

// A TEST-ONLY envelope for interleaving primary writes and typed secondary
// events. This helper is independent of production tree transcript validation.
using secondary_test_event = std::variant<size_t, isolated_secondary_cursor_event>;
struct secondary_journal_recorder {
   primary_state_overlay& state;
   secondary_cursor_group group;
   std::vector<secondary_test_event>& events;
   secondary_journal_recorder(primary_state_overlay& s, std::vector<secondary_test_event>& e)
      : state(s), group(s, "token"_n), events(e) {}
   template<class T> int save(T event) {
      const auto result = event.result;
      events.emplace_back(isolated_secondary_cursor_event{std::move(event)});
      return result;
   }
   int find_primary(table_key table, secondary_key key, uint64_t primary) {
      isolated_secondary_find_primary event{table, primary, key, key};
      event.result = group.get(secondary_kind(key)).find_primary(table, event.key_after, primary);
      return save(std::move(event));
   }
   int find_secondary(table_key table, secondary_key key, uint64_t before = 345) {
      isolated_secondary_find_secondary event{table, key, before, before};
      event.result = group.get(secondary_kind(key)).find_secondary(table, key, event.primary_after);
      return save(std::move(event));
   }
   int bound(table_key table, secondary_key key, bool upper = false, uint64_t before = 456) {
      isolated_secondary_bound event{table, upper, key, key, before, before};
      auto& cursor = group.get(secondary_kind(key));
      event.result = upper ? cursor.upper_bound(table, event.key_after, event.primary_after)
                           : cursor.lower_bound(table, event.key_after, event.primary_after);
      return save(std::move(event));
   }
   int end(table_key table, secondary_key_kind kind) {
      return save(isolated_secondary_end{table, kind, group.get(kind).end(table)});
   }
   int adjacent(secondary_key_kind kind, int iterator, bool previous = false, uint64_t before = 567) {
      isolated_secondary_adjacent event{kind, previous, iterator, -1, before, before};
      event.result = previous ? group.get(kind).previous(iterator, event.primary_after)
                              : group.get(kind).next(iterator, event.primary_after);
      return save(std::move(event));
   }
   int store(table_key table, uint64_t primary, name payer, secondary_key key) {
      const auto kind = secondary_kind(key);
      const auto index = state.write_count();
      const int iterator = group.get(kind).store(table, primary, payer, key);
      events.emplace_back(isolated_secondary_cursor_event{isolated_secondary_write{kind, index, iterator}});
      return iterator;
   }
   void update(secondary_key_kind kind, int iterator, name payer, secondary_key key) {
      const auto index = state.write_count();
      group.get(kind).update(iterator, payer, key);
      events.emplace_back(isolated_secondary_cursor_event{isolated_secondary_write{kind, index, iterator}});
   }
   void remove(secondary_key_kind kind, int iterator) {
      const auto index = state.write_count();
      group.get(kind).remove(iterator);
      events.emplace_back(isolated_secondary_cursor_event{isolated_secondary_write{kind, index, iterator}});
   }
};

bool replay_secondary_test_script(const primary_overlay_result& expected,
      const std::vector<secondary_test_event>& events, bool read_only = false,
      const std::function<void()>& checkpoint = {}) {
   primary_state_overlay state(expected.base());
   secondary_cursor_group group(state, "token"_n);
   size_t next = 0;
   for (const auto& event : events) {
      if (checkpoint) checkpoint(); // never swallowed by a local fallback catch
      try {
         if (const auto* index = std::get_if<size_t>(&event)) {
            if (read_only || *index != next || next >= expected.writes().size()) return false;
            const auto& write = expected.primary_write(next++);
            switch (write.operation) {
               case primary_operation::store: state.store(write.table, write.primary_key, write.payer, write.value); break;
               case primary_operation::update: state.update_existing(write.table, write.primary_key, write.payer, write.value); break;
               case primary_operation::remove: state.remove(write.table, write.primary_key); break;
               default: return false;
            }
         } else if (!replay_isolated_secondary_cursor_event(group,
               std::get<isolated_secondary_cursor_event>(event), expected, next, expected.writes().size(), read_only)) return false;
      } catch (const std::exception&) { return false; }
   }
   if (checkpoint) checkpoint();
   const auto actual = state.seal();
   return next == expected.writes().size() && actual.writes() == expected.writes() &&
      actual.read_tables() == expected.read_tables() &&
      std::equal(actual.read_queries().begin(), actual.read_queries().end(),
                 expected.read_queries().begin(), expected.read_queries().end(), [](const auto& a, const auto& b) {
                    return !primary_query_less{}(a, b) && !primary_query_less{}(b, a);
                 }) &&
      actual.secondary_read_queries() == expected.secondary_read_queries() &&
      actual.read_accounts() == expected.read_accounts() && actual.primary_only() == expected.primary_only();
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_secondary_state_tests)
BOOST_AUTO_TEST_CASE(exact_owned_keys_and_original_order_are_different_contracts) {
   for(unsigned type=0;type<5;++type) {
      const auto a=numbered(type,5),b=numbered(type,9);
      BOOST_CHECK(static_cast<unsigned>(secondary_kind(a))==type);
      BOOST_CHECK(a==numbered(type,5)); BOOST_CHECK(a!=b);
      BOOST_CHECK(secondary_key_less(a,b)); BOOST_CHECK(!secondary_key_less(b,a));
      BOOST_CHECK(secondary_key_equivalent(a,a));
      BOOST_CHECK(!secondary_key_is_nan(a));
      for(unsigned other=0;other<5;++other) if(type!=other)
         BOOST_CHECK_THROW(secondary_key_less(a,numbered(other,5)),secondary_state_invalid_query);
   }
   const secondary_key z64=secondary_float64{0},n64=secondary_float64{uint64_t{1}<<63};
   const secondary_key z128=secondary_float128{0},n128=secondary_float128{uint128_t{1}<<127};
   BOOST_CHECK(z64!=n64); BOOST_CHECK(secondary_key_equivalent(z64,n64));
   BOOST_CHECK(z128!=n128); BOOST_CHECK(secondary_key_equivalent(z128,n128));
   BOOST_CHECK(secondary_key_less(key256_t{0,255},key256_t{0,256}));
   BOOST_CHECK(secondary_key_less(key256_t{0,~uint128_t{0}},key256_t{1,0}));
   BOOST_CHECK(secondary_key_less(uint128_t{1},uint128_t{1}<<127));
   BOOST_CHECK(secondary_key_less(secondary_float64{1},secondary_float64{0x0010000000000000ULL}));
   BOOST_CHECK(secondary_key_less(secondary_float64{0x000fffffffffffffULL},secondary_float64{0x0010000000000000ULL}));
   BOOST_CHECK(secondary_key_less(secondary_float128{1},secondary_float128{uint128_t{1}<<112}));
   BOOST_CHECK(secondary_key_less(secondary_float128{(uint128_t{1}<<112)-1},secondary_float128{uint128_t{1}<<112}));
}

BOOST_FIXTURE_TEST_CASE(all_five_indices_distinguish_missing_table_empty_index_and_typed_identity,secondary_fixture) {
   for(unsigned type=0;type<5;++type) {
      auto q=query(type,secondary_query_kind::find_primary,0,7);
      const auto result=view.read_secondary(q); BOOST_CHECK(!result.table); BOOST_CHECK(!result.row);
   }
   table();
   for(unsigned type=0;type<5;++type) {
      auto q=query(type,secondary_query_kind::find_primary,0,7);
      const auto result=view.read_secondary(q); BOOST_REQUIRE(result.table); BOOST_CHECK(!result.row); compare(q);
   }
   std::vector<secondary_read_result> captured;
   for(unsigned type=0;type<5;++type) put(7,numbered(type,11));
   for(unsigned type=0;type<5;++type) {
      auto q=query(type,secondary_query_kind::find_primary,0,7);
      auto result=view.read_secondary(q); BOOST_REQUIRE(result.row); BOOST_REQUIRE(result.table);
      BOOST_CHECK_EQUAL(result.table->count,5u); BOOST_CHECK_EQUAL(result.row->primary_key,7u);
      BOOST_CHECK(result.row->payer=="bob"_n); BOOST_CHECK(result.table->payer=="alice"_n);
      BOOST_CHECK(result.row->key==numbered(type,11)); compare(q); captured.push_back(std::move(result));
   }
   // Integer object IDs can coincide across index families. The type cannot.
   BOOST_CHECK_EQUAL(captured[0].row->object_id,captured[1].row->object_id);
   BOOST_CHECK(*captured[0].row!=*captured[1].row);
}

BOOST_FIXTURE_TEST_CASE(duplicate_keys_bounds_and_full_tuple_adjacency_match_physical_scan,secondary_fixture) {
   const table_key neighboring{"token"_n,"bob"_n,"rows"_n};
   for(unsigned type=0;type<5;++type) {
      for(auto pk:std::array<uint64_t,5>{99,last_primary,7,0,41}) put(pk,numbered(type,10));
      put(3,numbered(type,3)); put(4,numbered(type,17)); put(0,numbered(type,1),neighboring);
      const auto first=view.read_secondary(query(type,secondary_query_kind::find_secondary,10));
      BOOST_REQUIRE(first.row); BOOST_CHECK_EQUAL(first.row->primary_key,0u);
      for(auto kind:{secondary_query_kind::find_secondary,secondary_query_kind::lower_bound,
                     secondary_query_kind::upper_bound,secondary_query_kind::first,secondary_query_kind::last})
         for(auto value:{0u,3u,9u,10u,11u,17u,18u}) compare(query(type,kind,value));
      for(auto pk:std::array<uint64_t,8>{0,1,3,7,41,99,last_primary-1,last_primary}) {
         compare(query(type,secondary_query_kind::find_primary,0,pk));
         for(auto value:{0u,3u,9u,10u,17u,18u}) {
            compare(query(type,secondary_query_kind::next,value,pk));
            compare(query(type,secondary_query_kind::previous,value,pk));
         }
      }
      BOOST_CHECK(!view.read_secondary(query(type,secondary_query_kind::find_secondary,9)).row);
      BOOST_CHECK_EQUAL(view.read_secondary(query(type,secondary_query_kind::upper_bound,10)).row->primary_key,4u);
      BOOST_CHECK_EQUAL(view.read_secondary(query(type,secondary_query_kind::next,10,last_primary)).row->primary_key,4u);
      BOOST_CHECK(!view.read_secondary(query(type,secondary_query_kind::previous,3,3)).row);
      BOOST_CHECK(!view.read_secondary(query(type,secondary_query_kind::next,17,4)).row);
   }
}

BOOST_FIXTURE_TEST_CASE(primary_and_all_secondary_families_share_one_table_count_and_retirement,secondary_fixture) {
   const auto& t=table();
   const auto original_id=t.id._id;
   const auto& primary_row=db.create<key_value_object>([&](auto& row) {
      row.t_id=t.id;row.primary_key=7;row.payer="carol"_n;
   });
   db.modify(t,[](auto& row){++row.count;});
   for(unsigned type=0;type<5;++type) put(7,numbered(type,3));
   BOOST_REQUIRE(primary.read_table(secondary_rows)); BOOST_CHECK_EQUAL(primary.read_table(secondary_rows)->count,6u);
   for(unsigned type=0;type<5;++type) compare(query(type,secondary_query_kind::find_primary,0,7));
   db.remove(primary_row); db.modify(t,[](auto& row){--row.count;});
   BOOST_REQUIRE(primary.read_table(secondary_rows)); BOOST_CHECK_EQUAL(primary.read_table(secondary_rows)->count,5u);
   for(unsigned type=0;type<4;++type) {
      erase(7,numbered(type,0));
      const auto metadata=primary.read_table(secondary_rows); BOOST_REQUIRE(metadata);
      BOOST_CHECK_EQUAL(metadata->count,4-type);
      BOOST_REQUIRE(view.read_secondary(query(4,secondary_query_kind::find_primary,0,7)).row);
   }
   erase(7,numbered(4,0));
   BOOST_CHECK(!primary.read_table(secondary_rows));
   for(unsigned type=0;type<5;++type) compare(query(type,secondary_query_kind::find_primary,0,7));
   put(7,numbered(0,3));
   const auto recreated=view.read_secondary(query(0,secondary_query_kind::find_primary,0,7));
   BOOST_REQUIRE(recreated.table); BOOST_REQUIRE(recreated.row);
   BOOST_CHECK(recreated.table->object_id!=original_id);
   BOOST_CHECK_EQUAL(recreated.table->count,1u);
}

BOOST_FIXTURE_TEST_CASE(owned_reads_survive_update_remove_recreation_and_nested_undo,secondary_fixture) {
   for(unsigned type=0;type<5;++type) put(7,numbered(type,10));
   for(unsigned type=0;type<5;++type) {
      const auto q=query(type,secondary_query_kind::find_primary,0,7);
      const auto before=view.read_secondary(q); BOOST_REQUIRE(before.row);
      {
         auto outer=db.start_undo_session(true);
         put(7,numbered(type,20),secondary_rows,"carol"_n);
         const auto changed=view.read_secondary(q);
         BOOST_CHECK(changed!=before); BOOST_REQUIRE(changed.row);
         BOOST_CHECK_EQUAL(changed.row->object_id,before.row->object_id);
         BOOST_CHECK(before.row->key==numbered(type,10)); // the earlier read owns its key
         compare(q);
         {
            auto inner=db.start_undo_session(true);
            erase(7,numbered(type,0)); put(7,numbered(type,20));
            const auto replacement=view.read_secondary(q); BOOST_REQUIRE(replacement.row);
            BOOST_CHECK(replacement.row->object_id!=changed.row->object_id);
            compare(q);
         }
         BOOST_CHECK(view.read_secondary(q)==changed);
      }
      BOOST_CHECK(view.read_secondary(q)==before);
   }
}

BOOST_FIXTURE_TEST_CASE(nan_inputs_are_rejected_before_table_lookup_but_primary_outputs_are_not_inputs,secondary_fixture) {
   const std::array<secondary_key,6> invalid{
      secondary_float64{0x7ff8000000000001ULL},secondary_float64{0x7ff0000000000001ULL},
      secondary_float64{0xfff8000000000001ULL},
      secondary_float128{(uint128_t{0x7fff}<<112)|(uint128_t{1}<<111)|1},
      secondary_float128{(uint128_t{0x7fff}<<112)|1},
      secondary_float128{(uint128_t{0xffff}<<112)|(uint128_t{1}<<111)|1}};
   for(unsigned phase=0;phase<2;++phase) {
      if(phase) {put(7,numbered(3,11));put(7,numbered(4,11));}
      for(const auto& value:invalid) {
         BOOST_CHECK(secondary_key_is_nan(value)); BOOST_CHECK(value==value); // exact-bit equality, even for rejected NaN
         BOOST_CHECK_THROW(secondary_key_less(value,value),secondary_state_invalid_query);
         for(auto kind:{secondary_query_kind::find_secondary,secondary_query_kind::lower_bound,
                        secondary_query_kind::upper_bound,secondary_query_kind::next,secondary_query_kind::previous})
            BOOST_CHECK_THROW(view.read_secondary({secondary_rows,kind,value,7}),secondary_state_invalid_query);
         const auto output=view.read_secondary({secondary_rows,secondary_query_kind::find_primary,value,7});
         BOOST_CHECK(bool(output.row)==bool(phase));
         if(output.row) BOOST_CHECK(output.row->key==numbered(static_cast<unsigned>(secondary_kind(value)),11));
      }
      for(unsigned type=0;type<5;++type)
         BOOST_CHECK_THROW(view.read_secondary(query(type,static_cast<secondary_query_kind>(255))),secondary_state_invalid_query);
   }
}
BOOST_FIXTURE_TEST_CASE(signed_zero_infinities_and_duplicate_float_keys_preserve_exact_bits,secondary_fixture) {
   const auto f64=[](int64_t v)->secondary_key {return test_mapping<secondary_float64>::owned(i64_to_f64(v));};
   const auto f128=[](int64_t v)->secondary_key {return test_mapping<secondary_float128>::owned(i64_to_f128(v));};
   const std::array<secondary_key,6> keys64{secondary_float64{0xfff0000000000000ULL},f64(-4),
      secondary_float64{uint64_t{1}<<63},secondary_float64{0},f64(5),secondary_float64{0x7ff0000000000000ULL}};
   const std::array<secondary_key,6> keys128{secondary_float128{uint128_t{0xffff}<<112},f128(-4),
      secondary_float128{uint128_t{1}<<127},secondary_float128{0},f128(5),
      secondary_float128{uint128_t{0x7fff}<<112}};
   for(const auto& keys:{keys64,keys128}) {
      for(size_t i=0;i<keys.size();++i) put(3+2*i,keys[i]);
      for(const auto& key:keys) {
         BOOST_CHECK(!secondary_key_is_nan(key));
         for(auto kind:{secondary_query_kind::find_secondary,secondary_query_kind::lower_bound,
                        secondary_query_kind::upper_bound,secondary_query_kind::first,secondary_query_kind::last,
                        secondary_query_kind::next,secondary_query_kind::previous})
            for(uint64_t primary_key:std::array<uint64_t,4>{0,7,9,last_primary})
               compare({secondary_rows,kind,key,primary_key});
      }
      const auto zero=view.read_secondary({secondary_rows,secondary_query_kind::find_secondary,keys[3],0});
      BOOST_REQUIRE(zero.row); BOOST_CHECK_EQUAL(zero.row->primary_key,7u);
      BOOST_CHECK(zero.row->key==keys[2]); BOOST_CHECK(zero.row->key!=keys[3]);
      const auto next=view.read_secondary({secondary_rows,secondary_query_kind::next,keys[3],7});
      BOOST_REQUIRE(next.row); BOOST_CHECK_EQUAL(next.row->primary_key,9u); BOOST_CHECK(next.row->key==keys[3]);
      const auto previous=view.read_secondary({secondary_rows,secondary_query_kind::previous,keys[3],9});
      BOOST_REQUIRE(previous.row); BOOST_CHECK(previous.row->key==keys[2]);
      BOOST_CHECK(!view.read_secondary({secondary_rows,secondary_query_kind::upper_bound,keys.back(),0}).row);
      BOOST_CHECK(!view.read_secondary({secondary_rows,secondary_query_kind::previous,keys.front(),3}).row);
   }
}

BOOST_FIXTURE_TEST_CASE(unsigned_secondary_extremes_keep_numeric_word_order_without_overflow,secondary_fixture) {
   const std::array<secondary_key,4> keys64{uint64_t{0},uint64_t{255},uint64_t{256},last_primary};
   const std::array<secondary_key,4> keys128{uint128_t{0},uint128_t{1}<<64,uint128_t{1}<<127,~uint128_t{0}};
   const std::array<secondary_key,4> keys256{key256_t{0,255},key256_t{0,256},key256_t{1,0},
                                         key256_t{~uint128_t{0},~uint128_t{0}}};
   for(const auto& keys:{keys64,keys128,keys256}) {
      for(size_t i=0;i<keys.size();++i) put(i==3?last_primary:i,keys[i]);
      for(size_t i=0;i<keys.size();++i) {
         for(auto kind:{secondary_query_kind::find_secondary,secondary_query_kind::lower_bound,
                        secondary_query_kind::upper_bound,secondary_query_kind::first,secondary_query_kind::last})
            compare({secondary_rows,kind,keys[i],0});
         for(uint64_t pk:std::array<uint64_t,3>{0,2,last_primary}) {
            compare({secondary_rows,secondary_query_kind::next,keys[i],pk});
            compare({secondary_rows,secondary_query_kind::previous,keys[i],pk});
         }
      }
      const auto last=view.read_secondary({secondary_rows,secondary_query_kind::last,keys[0],0});
      BOOST_REQUIRE(last.row); BOOST_CHECK_EQUAL(last.row->primary_key,last_primary); BOOST_CHECK(last.row->key==keys.back());
      BOOST_CHECK(!view.read_secondary({secondary_rows,secondary_query_kind::upper_bound,keys.back(),0}).row);
      BOOST_CHECK(!view.read_secondary({secondary_rows,secondary_query_kind::next,keys.back(),last_primary}).row);
   }
}

BOOST_FIXTURE_TEST_CASE(seeded_interleaved_updates_erases_and_queries_match_independent_scan,secondary_fixture) {
   const std::array<table_key,3> tables{secondary_rows,table_key{"token"_n,"bob"_n,"rows"_n},
                                                   table_key{"other"_n,"bob"_n,"rows"_n}};
   std::mt19937_64 random(0x71b3096eULL);
   for(unsigned step=0;step<800;++step) {
      const unsigned type=random()%5;
      const auto& target=tables[random()%2]; // the third table intentionally stays absent
      const uint64_t pk=step%17==0?last_primary:random()%23;
      if(random()%4==0) erase(pk,numbered(type,0),target);
      else put(pk,numbered(type,random()%13),target,step%2?"bob"_n:"carol"_n);
      for(unsigned sample=0;sample<3;++sample) {
         const auto kind=static_cast<secondary_query_kind>(random()%8);
         const auto q=query(random()%5,kind,random()%15,random()%3==0?last_primary:random()%24,tables[random()%3]);
         compare(q);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(live_secondary_adapter_rejects_every_foreign_thread_before_database_access,secondary_fixture) {
   for(unsigned type=0;type<5;++type) put(7,numbered(type,10));
   for(unsigned type=0;type<5;++type) {
      const auto q=query(type,secondary_query_kind::find_primary,0,7);
      const auto before=view.read_secondary(q);
      auto task=std::async(std::launch::async,[&] {
         try {view.read_secondary(q);} catch(const state_view_wrong_thread&) {return true;} return false;
      });
      BOOST_CHECK(task.get()); BOOST_CHECK(view.read_secondary(q)==before);
      compare(q);
   }
   auto task=std::async(std::launch::async,[&] {
      try {view.read_secondary(query(0,static_cast<secondary_query_kind>(255)));}
      catch(const state_view_wrong_thread&) {return true;} return false;
   });
   BOOST_CHECK(task.get()); // reject foreign owner before even an invalid query
}
BOOST_FIXTURE_TEST_CASE(overlay_primary_and_all_secondary_families_share_lifetimes_and_chronology,secondary_fixture) {
   const std::array keys{secondary_rows};
   const auto base=frozen_primary_state::capture(primary,keys);
   primary_state_overlay state(base);
   const std::array<char,3> value{'a','b','c'};
   state.store(secondary_rows,7,"alice"_n,value);
   const auto first_table=state.read_table(secondary_rows);
   BOOST_REQUIRE(first_table); BOOST_CHECK_LT(first_table->object_id,0);
   const auto first_row=state.locate_primary({secondary_rows,primary_query_kind::find,7}).row;
   BOOST_REQUIRE(first_row);
   for(unsigned type=0;type<5;++type) state.store_secondary(secondary_rows,7,"bob"_n,numbered(type,10));
   BOOST_CHECK_EQUAL(state.read_table(secondary_rows)->count,6u);
   BOOST_CHECK(state.read_table(secondary_rows)->payer=="alice"_n);
   std::set<int64_t> ids{first_row->object_id};
   for(unsigned type=0;type<5;++type) {
      const auto before=state.read_secondary(query(type,secondary_query_kind::find_primary,0,7));
      BOOST_REQUIRE(before.row); BOOST_CHECK_LT(before.row->object_id,0);
      BOOST_CHECK(ids.insert(before.row->object_id).second);
      state.update_secondary(secondary_rows,7,{},numbered(type,20));
      const auto after=state.read_secondary(query(type,secondary_query_kind::find_primary,0,7));
      BOOST_REQUIRE(after.row); BOOST_CHECK(after.row->key==numbered(type,20));
      BOOST_CHECK(after.row->payer=="bob"_n); BOOST_CHECK_EQUAL(after.row->object_id,before.row->object_id);
   }
   state.remove(secondary_rows,7);
   BOOST_CHECK_EQUAL(state.read_table(secondary_rows)->count,5u);
   for(unsigned type=0;type<5;++type) {
      state.remove_secondary(secondary_rows,7,numbered(type,0));
      const auto table=state.read_table(secondary_rows);
      if(type<4) {BOOST_REQUIRE(table);BOOST_CHECK_EQUAL(table->count,4u-type);}
      else BOOST_CHECK(!table);
   }
   state.store(secondary_rows,7,"carol"_n,value);
   BOOST_CHECK(state.read_table(secondary_rows)->payer=="carol"_n);
   BOOST_CHECK(state.read_table(secondary_rows)->object_id!=first_table->object_id);
   BOOST_CHECK(state.locate_primary({secondary_rows,primary_query_kind::find,7}).row->object_id!=first_row->object_id);
   for(unsigned type=0;type<5;++type) BOOST_CHECK(!state.read_secondary(query(type,secondary_query_kind::first)).row);
   const auto sealed=state.seal();
   BOOST_CHECK(!sealed.primary_only()); BOOST_REQUIRE_EQUAL(sealed.writes().size(),18u);
   BOOST_CHECK(std::holds_alternative<primary_mutation>(sealed.writes()[0]));
   for(size_t i=1;i<=10;++i) BOOST_CHECK(std::holds_alternative<secondary_mutation>(sealed.writes()[i]));
   BOOST_CHECK(std::holds_alternative<primary_mutation>(sealed.writes()[11]));
   BOOST_CHECK_THROW(sealed.primary_write(1),primary_state_unsupported);
   BOOST_CHECK(sealed.matches_base_reads(primary,[]{})); BOOST_CHECK(!primary.read_table(secondary_rows));
}

BOOST_FIXTURE_TEST_CASE(overlay_moved_base_keys_duplicates_and_sparse_neighbors_keep_original_order,secondary_fixture) {
   for(unsigned type=0;type<5;++type) {
      put(1,numbered(type,10));put(2,numbered(type,20));put(3,numbered(type,20));put(4,numbered(type,30));
   }
   const auto result=discover_overlay(primary,[&](primary_state_overlay& state) {
      for(unsigned type=0;type<5;++type) {
         state.update_secondary(secondary_rows,2,"carol"_n,numbered(type,35));
         auto selected=state.read_secondary(query(type,secondary_query_kind::find_secondary,20));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,3u);
         state.update_secondary(secondary_rows,3,{},numbered(type,5));
         BOOST_CHECK(!state.read_secondary(query(type,secondary_query_kind::find_secondary,20)).row);
         selected=state.read_secondary(query(type,secondary_query_kind::lower_bound,20));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,4u);
         state.remove_secondary(secondary_rows,4,numbered(type,0));
         selected=state.read_secondary(query(type,secondary_query_kind::lower_bound,20));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,2u);
         BOOST_CHECK(selected.row->payer=="carol"_n);
         selected=state.read_secondary(query(type,secondary_query_kind::next,5,3));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,1u);
         selected=state.read_secondary(query(type,secondary_query_kind::previous,35,2));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,1u);
         state.store_secondary(secondary_rows,0,"bob"_n,numbered(type,35));
         selected=state.read_secondary(query(type,secondary_query_kind::find_secondary,35));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,0u);
         selected=state.read_secondary(query(type,secondary_query_kind::next,35,0));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,2u);
         BOOST_CHECK(!state.read_secondary(query(type,secondary_query_kind::upper_bound,35)).row);
         selected=state.read_secondary(query(type,secondary_query_kind::last));
         BOOST_REQUIRE(selected.row);BOOST_CHECK_EQUAL(selected.row->primary_key,2u);
      }
   });
   BOOST_CHECK_EQUAL(result.writes().size(),20u);BOOST_CHECK(result.matches_base_reads(primary,[]{}));
   BOOST_CHECK(!result.primary_only());BOOST_CHECK(!result.secondary_read_queries().empty());
   for(unsigned type=0;type<5;++type) BOOST_CHECK(view.read_secondary(query(type,secondary_query_kind::find_primary,0,2)).row->key==numbered(type,20));
}

BOOST_FIXTURE_TEST_CASE(overlay_secondary_dependencies_detect_reordering_without_metadata_change,secondary_fixture) {
   put(1,uint64_t{10});put(2,uint64_t{30});
   const auto result=discover_overlay(primary,[&](primary_state_overlay& state) {
      BOOST_CHECK_EQUAL(state.read_secondary(query(0,secondary_query_kind::lower_bound,15)).row->primary_key,2u);
   });
   const auto before=primary.read_table(secondary_rows);
   BOOST_CHECK(result.matches_base_reads(primary,[]{}));BOOST_CHECK(!result.primary_only());
   put(1,uint64_t{20});
   BOOST_CHECK(primary.read_table(secondary_rows)==before);
   BOOST_CHECK(!result.matches_base_reads(primary,[]{}));
   const transaction_state_view& generic=primary;
   BOOST_CHECK(generic.read_secondary(query(0,secondary_query_kind::lower_bound,15))==view.read_secondary(query(0,secondary_query_kind::lower_bound,15)));
}

BOOST_FIXTURE_TEST_CASE(overlay_shared_operation_byte_and_query_budgets_fail_closed,secondary_fixture) {
   const std::array keys{secondary_rows};const auto base=frozen_primary_state::capture(primary,keys);
   const std::array<char,3> value{'a','b','c'};
   for(unsigned mode=0;mode<3;++mode) {
      primary_overlay_limits limits;
      if(mode==0) limits.max_write_operations=6;
      if(mode==1) limits.max_write_value_bytes=83; // 3 primary + 8/16/32/8/16 secondary
      if(mode==2) limits.max_read_queries=6;
      primary_state_overlay state(base,limits);
      state.store(secondary_rows,7,"alice"_n,value);
      for(unsigned type=0;type<5;++type) state.store_secondary(secondary_rows,7,"bob"_n,numbered(type,10));
      BOOST_CHECK_EQUAL(state.write_count(),6u);
      if(mode<2) BOOST_CHECK_THROW(state.update_secondary(secondary_rows,7,{},uint64_t{11}),primary_state_budget_exceeded);
      else BOOST_CHECK_THROW(state.read_secondary(query(0,secondary_query_kind::first)),primary_state_budget_exceeded);
      BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
   }
   primary_state_overlay clean(base);
   clean.store_secondary(secondary_rows,1,"bob"_n,uint64_t{2});
   BOOST_CHECK_EQUAL(clean.seal().writes().size(),1u);BOOST_CHECK(!primary.read_table(secondary_rows));
}

BOOST_FIXTURE_TEST_CASE(overlay_float_exact_bits_nan_and_wrong_thread_do_not_leak_mutations,secondary_fixture) {
   const std::array keys{secondary_rows};const auto absent=frozen_primary_state::capture(primary,keys);
   for(const secondary_key& nan:{secondary_key{secondary_float64{0x7ff8000000000001ULL}},
                               secondary_key{secondary_float128{(uint128_t{0x7fff}<<112)|1}}}) {
      for(unsigned mode=0;mode<3;++mode) {
         primary_state_overlay state(absent);
         if(mode==0) BOOST_CHECK_THROW(state.store_secondary(secondary_rows,7,"bob"_n,nan),secondary_state_invalid_query);
         if(mode==1) BOOST_CHECK_THROW(state.update_secondary(secondary_rows,7,{},nan),secondary_state_invalid_query);
         if(mode==2) BOOST_CHECK_THROW(state.read_secondary({secondary_rows,secondary_query_kind::lower_bound,nan,0}),secondary_state_invalid_query);
         BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
      }
   }
   for(unsigned type:{3u,4u}) {
      const secondary_key zero=type==3?secondary_key{secondary_float64{0}}:secondary_key{secondary_float128{0}};
      const secondary_key negative=type==3?secondary_key{secondary_float64{uint64_t{1}<<63}}:secondary_key{secondary_float128{uint128_t{1}<<127}};
      put(7,zero);put(9,negative);put(11,numbered(type,1));
      const auto result=discover_overlay(primary,[&](primary_state_overlay& state) {
         state.update_secondary(secondary_rows,7,{},negative);
         const auto first=state.read_secondary({secondary_rows,secondary_query_kind::find_secondary,zero,0});
         BOOST_REQUIRE(first.row);BOOST_CHECK_EQUAL(first.row->primary_key,7u);BOOST_CHECK(first.row->key==negative);
         state.remove_secondary(secondary_rows,7,zero);state.remove_secondary(secondary_rows,9,zero);
         BOOST_CHECK(!state.read_secondary({secondary_rows,secondary_query_kind::find_secondary,zero,0}).row);
      });
      BOOST_CHECK(result.matches_base_reads(primary,[]{}));
      put(7,negative);BOOST_CHECK(!result.matches_base_reads(primary,[]{})); // same ordering, different exact bits
   }
   primary_state_overlay state(absent);
   auto foreign=std::async(std::launch::async,[&] {
      try {state.store_secondary(secondary_rows,7,"bob"_n,uint64_t{1});return false;}
      catch(const state_view_wrong_thread&) {return true;}
   });
   BOOST_CHECK(foreign.get()); // owner remains usable; no foreign write to valid_
   state.store_secondary(secondary_rows,7,"bob"_n,uint64_t{1});
   BOOST_CHECK_EQUAL(state.seal().writes().size(),1u);
}

BOOST_FIXTURE_TEST_CASE(seeded_secondary_overlay_mutations_match_independent_database_oracle,secondary_fixture) {
   secondary_fixture oracle;
   for(unsigned type=0;type<5;++type) for(uint64_t pk=0;pk<3;++pk) {
      put(pk,numbered(type,pk+1));oracle.put(pk,numbered(type,pk+1));
   }
   const auto result=discover_overlay(primary,[&](primary_state_overlay& state) {
      auto undo=oracle.db.start_undo_session(true);
      std::mt19937_64 random(0x8b61d005ULL);
      for(unsigned step=0;step<80;++step) {
         const unsigned type=random()%5;const uint64_t pk=random()%8;const auto key=numbered(type,random()%13);
         const auto existing=oracle.view.read_secondary(query(type,secondary_query_kind::find_primary,0,pk));
         if(random()%3==0) {
            if(existing.row) {state.remove_secondary(secondary_rows,pk,key);oracle.erase(pk,key);}
         } else {
            if(existing.row) state.update_secondary(secondary_rows,pk,"carol"_n,key);
            else state.store_secondary(secondary_rows,pk,"carol"_n,key);
            oracle.put(pk,key,secondary_rows,"carol"_n);
         }
         for(unsigned sample=0;sample<2;++sample) {
            const auto q=query(type,static_cast<secondary_query_kind>(random()%8),random()%15,random()%9);
            const auto actual=state.read_secondary(q),expected=reference_read(oracle.db,q);
            BOOST_REQUIRE_EQUAL(bool(actual.table),bool(expected.table));
            if(actual.table) {BOOST_CHECK_EQUAL(actual.table->count,expected.table->count);BOOST_CHECK(actual.table->payer==expected.table->payer);}
            BOOST_REQUIRE_EQUAL(bool(actual.row),bool(expected.row));
            if(actual.row) {
               BOOST_CHECK_EQUAL(actual.row->primary_key,expected.row->primary_key);
               BOOST_CHECK(actual.row->key==expected.row->key);BOOST_CHECK(actual.row->payer==expected.row->payer);
               if(actual.row->object_id>=0) BOOST_CHECK_EQUAL(actual.row->object_id,expected.row->object_id);
            }
         }
      }
   });
   BOOST_CHECK(!result.primary_only());BOOST_CHECK(result.matches_base_reads(primary,[]{}));
   BOOST_CHECK_GT(result.writes().size(),40u);
   BOOST_CHECK_EQUAL(primary.read_table(secondary_rows)->count,15u);
}

BOOST_FIXTURE_TEST_CASE(overlay_retired_canonical_table_cannot_contribute_old_secondary_rows,secondary_fixture) {
   put(7,uint64_t{10});
   const auto old_table=primary.read_table(secondary_rows);
   const auto result=discover_overlay(primary,[&](primary_state_overlay& state) {
      state.remove_secondary(secondary_rows,7,uint64_t{0});
      BOOST_CHECK(!state.read_table(secondary_rows));
      state.store(secondary_rows,7,"carol"_n,std::array<char,1>{'x'});
      const auto current=state.read_table(secondary_rows);
      BOOST_REQUIRE(current);BOOST_CHECK(current->object_id!=old_table->object_id);
      BOOST_CHECK(current->payer=="carol"_n);
      BOOST_CHECK(!state.read_secondary(query(0,secondary_query_kind::first)).row);
      BOOST_CHECK(!state.read_secondary(query(0,secondary_query_kind::find_primary,0,7)).row);
      state.store_secondary(secondary_rows,7,"bob"_n,uint64_t{20});
      const auto row=state.read_secondary(query(0,secondary_query_kind::first)).row;
      BOOST_REQUIRE(row);BOOST_CHECK(row->key==secondary_key{uint64_t{20}});
      BOOST_CHECK_EQUAL(row->table_object_id,current->object_id);BOOST_CHECK_LT(row->object_id,0);
      BOOST_CHECK_EQUAL(state.read_table(secondary_rows)->count,2u);
   });
   BOOST_CHECK_EQUAL(result.writes().size(),3u);BOOST_CHECK(result.matches_base_reads(primary,[]{}));
   BOOST_CHECK(primary.read_table(secondary_rows)==old_table);
}

BOOST_FIXTURE_TEST_CASE(generic_secondary_read_is_owned_and_old_adapters_explicitly_decline,secondary_fixture) {
   struct legacy_adapter final:transaction_state_view {
      const transaction_state_view& source;
      explicit legacy_adapter(const transaction_state_view& source):source(source) {}
      std::optional<account_read> read_account(name n) const override {return source.read_account(n);}
      std::optional<table_read> read_table(const table_key& k) const override {return source.read_table(k);}
      primary_metadata_result locate_primary(const primary_query& q) const override {return source.locate_primary(q);}
      primary_prefix_result read_primary_prefix(const primary_query& q,size_t n) const override {return source.read_primary_prefix(q,n);}
      primary_read_result read_primary(const primary_query& q,size_t n) const override {return source.read_primary(q,n);}
   } legacy(primary);
   put(7,uint64_t{10});const auto q=query(0,secondary_query_kind::find_primary,0,7);
   BOOST_CHECK_THROW(legacy.read_secondary(q),state_view_unsupported_query);
   const auto base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},std::array{q});
   const transaction_state_view& frozen=*base;
   BOOST_CHECK(frozen.read_secondary(q)==view.read_secondary(q));
   auto foreign=std::async(std::launch::async,[&] {
      const transaction_state_view& live=primary;
      try {(void)live.read_secondary(q);return false;}
      catch(const state_view_wrong_thread&) {return frozen.read_secondary(q)==base->read_secondary(q);}
   });
   BOOST_CHECK(foreign.get());BOOST_CHECK(primary.read_secondary(q)==frozen.read_secondary(q));
}

BOOST_FIXTURE_TEST_CASE(cursor_all_families_follow_updated_keys_and_keep_index_local_handles,secondary_fixture) {
   for(unsigned type=0;type<5;++type) {put(7,numbered(type,10));put(9,numbered(type,20));put(11,numbered(type,20));}
   const auto result=discover_overlay(primary,[&](primary_state_overlay& state) {
      for(unsigned type=0;type<5;++type) {
         const auto kind=static_cast<secondary_key_kind>(type);
         secondary_overlay_cursor parent(state,secondary_rows.code,kind);
         auto key=numbered(type,99);uint64_t primary=777;
         const int middle=parent.find_primary(secondary_rows,key,9);
         BOOST_CHECK_EQUAL(middle,0);BOOST_CHECK(key==numbered(type,20));
         const int first=parent.find_primary(secondary_rows,key,7);
         const int last=parent.find_primary(secondary_rows,key,11);
         BOOST_CHECK_EQUAL(first,1);BOOST_CHECK_EQUAL(last,2);
         const auto end=parent.end(secondary_rows);BOOST_CHECK_EQUAL(end,-2);
         key=numbered(type,15);
         BOOST_CHECK_EQUAL(parent.lower_bound(secondary_rows,key,primary),middle);
         BOOST_CHECK_EQUAL(primary,9u);BOOST_CHECK(key==numbered(type,20));
         primary=777;
         BOOST_CHECK_EQUAL(parent.upper_bound(secondary_rows,key,primary),end);
         BOOST_CHECK_EQUAL(primary,777u);BOOST_CHECK(key==numbered(type,20));
         BOOST_CHECK_EQUAL(parent.find_secondary(secondary_rows,key,primary),middle);
         BOOST_CHECK_EQUAL(parent.next(middle,primary),last);BOOST_CHECK_EQUAL(primary,11u);
         primary=777;BOOST_CHECK_EQUAL(parent.next(last,primary),end);BOOST_CHECK_EQUAL(primary,777u);
         BOOST_CHECK_EQUAL(parent.previous(end,primary),last);BOOST_CHECK_EQUAL(primary,11u);
         primary=777;BOOST_CHECK_EQUAL(parent.previous(first,primary),-1);BOOST_CHECK_EQUAL(primary,777u);
         parent.update(middle,{},numbered(type,5));
         BOOST_CHECK_EQUAL(parent.next(middle,primary),first);BOOST_CHECK_EQUAL(primary,7u);
         BOOST_CHECK_EQUAL(parent.previous(middle,primary),-1);BOOST_CHECK_EQUAL(primary,7u);
         secondary_overlay_cursor child(state,secondary_rows.code,kind);
         key=numbered(type,0);
         BOOST_CHECK_EQUAL(child.find_primary(secondary_rows,key,7),0); // independent child cache
         const auto child_middle=child.find_primary(secondary_rows,key,9);
         BOOST_CHECK_EQUAL(child_middle,1);child.update(child_middle,{},numbered(type,30));
         BOOST_CHECK_EQUAL(parent.previous(middle,primary),last);BOOST_CHECK_EQUAL(primary,11u);
         BOOST_CHECK_EQUAL(parent.next(middle,primary),end);BOOST_CHECK_EQUAL(primary,11u);
         parent.set_receiver_for_notification("bob"_n);
         BOOST_CHECK_EQUAL(parent.find_primary(secondary_rows,key,9),middle);BOOST_CHECK(key==numbered(type,30));
         parent.set_receiver_for_notification(secondary_rows.code);
      }
   });
   BOOST_CHECK_EQUAL(result.writes().size(),10u);BOOST_CHECK(result.matches_base_reads(primary,[]{}));
}

BOOST_FIXTURE_TEST_CASE(cursor_deleted_objects_and_retired_ends_cannot_revive,secondary_fixture) {
   const std::array keys{secondary_rows};const auto base=frozen_primary_state::capture(primary,keys);
   for(unsigned type=0;type<5;++type) for(unsigned mode=0;mode<3;++mode) {
      primary_state_overlay state(base);const auto kind=static_cast<secondary_key_kind>(type);
      secondary_overlay_cursor parent(state,secondary_rows.code,kind),child(state,secondary_rows.code,kind);
      const auto old=parent.store(secondary_rows,7,"alice"_n,numbered(type,10));
      const auto end=parent.end(secondary_rows);auto key=numbered(type,0);uint64_t out=777;
      const auto child_row=child.find_primary(secondary_rows,key,7);child.remove(child_row);
      if(mode==0) {
         child.store(secondary_rows,7,"bob"_n,numbered(type,20));
         BOOST_CHECK_THROW(parent.next(old,out),primary_state_unsupported);
      } else {
         if(mode==2) {
            const auto other_type=(type+1)%5;
            secondary_overlay_cursor other(state,secondary_rows.code,static_cast<secondary_key_kind>(other_type));
            other.store(secondary_rows,9,"bob"_n,numbered(other_type,30));
         }
         BOOST_CHECK_THROW(parent.previous(end,out),primary_state_unsupported);
      }
      BOOST_CHECK_EQUAL(out,777u);BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
   }
   BOOST_CHECK(!primary.read_table(secondary_rows));
}

BOOST_FIXTURE_TEST_CASE(cursor_missing_rows_leave_outputs_untouched_and_validate_bad_handles,secondary_fixture) {
   const std::array keys{secondary_rows};const auto base=frozen_primary_state::capture(primary,keys);
   for(unsigned type=0;type<5;++type) {
      for(bool end_only:{false,true}) {
         primary_state_overlay probe(base);
         secondary_overlay_cursor no_rows(probe,secondary_rows.code,static_cast<secondary_key_kind>(type));
         uint64_t unchanged=777;
         BOOST_CHECK_EQUAL(end_only?no_rows.end(secondary_rows):no_rows.next(-2,unchanged),-1);
         const auto result=probe.seal();
         BOOST_CHECK(!result.primary_only());BOOST_CHECK(result.secondary_read_queries().empty());
         BOOST_CHECK_EQUAL(unchanged,777u);
      }
      primary_state_overlay state(base);
      secondary_overlay_cursor cursor(state,secondary_rows.code,static_cast<secondary_key_kind>(type));
      auto key=numbered(type,99);const auto original=key;uint64_t out=777;
      BOOST_CHECK_EQUAL(cursor.find_primary(secondary_rows,key,7),-1);BOOST_CHECK(key==original);
      BOOST_CHECK_EQUAL(cursor.find_secondary(secondary_rows,key,out),-1);BOOST_CHECK_EQUAL(out,777u);
      BOOST_CHECK_EQUAL(cursor.lower_bound(secondary_rows,key,out),-1);BOOST_CHECK(key==original);BOOST_CHECK_EQUAL(out,777u);
      BOOST_CHECK_EQUAL(cursor.upper_bound(secondary_rows,key,out),-1);BOOST_CHECK(key==original);BOOST_CHECK_EQUAL(out,777u);
      BOOST_CHECK_EQUAL(cursor.end(secondary_rows),-1);
      BOOST_CHECK_EQUAL(cursor.next(std::numeric_limits<int>::min(),out),-1);BOOST_CHECK_EQUAL(out,777u);
      if(type>=3) {
         key=type==3?secondary_key{secondary_float64{0x7ff8000000000001ULL}}:
            secondary_key{secondary_float128{(uint128_t{0x7fff}<<112)|1}};
         const auto nan=key;
         BOOST_CHECK_EQUAL(cursor.find_primary(secondary_rows,key,7),-1);BOOST_CHECK(key==nan);
      }
      BOOST_CHECK_THROW(cursor.previous(std::numeric_limits<int>::min(),out),invalid_table_iterator);
      BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
      for(int bad:{-1,0}) {
         primary_state_overlay fresh(base);
         secondary_overlay_cursor empty(fresh,secondary_rows.code,static_cast<secondary_key_kind>(type));
         BOOST_CHECK_THROW(empty.next(bad,out),invalid_table_iterator);
         BOOST_CHECK_THROW(fresh.seal(),primary_state_invalid_attempt);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(cursor_limits_foreign_threads_and_wrong_types_fail_closed,secondary_fixture) {
   const std::array keys{secondary_rows};const auto base=frozen_primary_state::capture(primary,keys);
   for(unsigned type=0;type<5;++type) {
      const auto kind=static_cast<secondary_key_kind>(type);
      for(unsigned mode=0;mode<2;++mode) {
         primary_state_overlay state(base);
         secondary_overlay_cursor cursor(state,secondary_rows.code,kind,mode==0?1:4096,mode==1?0:64);
         if(mode==0) cursor.store(secondary_rows,7,"bob"_n,numbered(type,10));
         BOOST_CHECK_THROW(cursor.store(secondary_rows,9,"bob"_n,numbered(type,20)),primary_state_budget_exceeded);
         BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
      }
      primary_state_overlay state(base);secondary_overlay_cursor cursor(state,secondary_rows.code,kind);
      auto foreign=std::async(std::launch::async,[&] {
         try {uint64_t out=0;(void)cursor.next(-2,out);return false;}
         catch(const state_view_wrong_thread&) {return true;}
      });
      BOOST_CHECK(foreign.get());BOOST_CHECK_EQUAL(cursor.store(secondary_rows,7,"bob"_n,numbered(type,10)),0);
      uint64_t out=777;
      BOOST_CHECK_THROW(cursor.find_secondary(secondary_rows,numbered((type+1)%5,10),out),secondary_state_invalid_query);
      BOOST_CHECK_EQUAL(out,777u);BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
   }
   BOOST_CHECK(!primary.read_table(secondary_rows));
}

BOOST_FIXTURE_TEST_CASE(cursor_receiver_switch_never_authorizes_foreign_writes,secondary_fixture) {
   std::vector<secondary_query> reads;
   for(unsigned type=0;type<5;++type) {put(7,numbered(type,10));reads.push_back(query(type,secondary_query_kind::find_primary,0,7));}
   const auto base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},reads);
   for(unsigned type=0;type<5;++type) {
      primary_state_overlay state(base);
      secondary_overlay_cursor cursor(state,"alice"_n,static_cast<secondary_key_kind>(type));
      auto key=numbered(type,0);const auto handle=cursor.find_primary(secondary_rows,key,7);
      BOOST_CHECK_EQUAL(handle,0); // foreign reads are allowed
      cursor.set_receiver_for_notification(secondary_rows.code);cursor.update(handle,{},numbered(type,20));
      cursor.set_receiver_for_notification("alice"_n);
      BOOST_CHECK_THROW(cursor.remove(handle),table_access_violation);
      BOOST_CHECK_THROW(state.seal(),primary_state_invalid_attempt);
      BOOST_CHECK(view.read_secondary(reads[type]).row->key==numbered(type,10));
   }
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
std::vector<secondary_query> capture_primary_queries() {
   std::vector<secondary_query> out;
   for(unsigned type=0;type<5;++type) out.push_back(query(type,secondary_query_kind::find_primary,0,7));
   return out;
}
void put_primary(secondary_fixture& fixture) {
   const auto& table=fixture.table();
   fixture.db.create<key_value_object>([&](auto& row) {
      row.t_id=table.id;row.primary_key=7;row.payer="carol"_n;row.value.assign("abc",3);
   });
   fixture.db.modify(table,[](auto& row){++row.count;});
}
void check_secondary_request(const frozen_primary_state& base,const secondary_query& q) {
   try {base.read_secondary(q);BOOST_FAIL("uncaptured secondary query was answered");}
   catch(const primary_state_missing& missing) {
      const auto* request=std::get_if<secondary_capture_request>(&missing.request);
      BOOST_REQUIRE(request);BOOST_CHECK(request->query==normalize_secondary_query(q));
   }
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_secondary_journal_tests)

BOOST_FIXTURE_TEST_CASE(typed_events_preserve_all_families_outputs_and_shared_write_chronology, secondary_fixture) {
   put_primary(*this);
   for (unsigned type = 0; type < 5; ++type) {
      put(7, numbered(type, 20)); put(19, numbered(type, 20));
   }
   std::vector<secondary_test_event> events;
   const auto expected = discover_overlay(primary, [&](primary_state_overlay& state) {
      events.clear(); secondary_journal_recorder record(state, events);
      for (unsigned type = 0; type < 5; ++type) {
         const auto kind = static_cast<secondary_key_kind>(type);
         events.emplace_back(state.write_count());
         const std::array<char, 2> bytes{'x', char(type)};
         state.update_existing(secondary_rows, 7, "bob"_n, bytes);
         const auto first = record.find_primary(secondary_rows, numbered(type, 0), 7);
         const auto second = record.find_primary(secondary_rows, numbered(type, 0), 19);
         BOOST_CHECK_EQUAL(record.find_secondary(secondary_rows, numbered(type, 20)), first);
         BOOST_CHECK_EQUAL(record.bound(secondary_rows, numbered(type, 20)), first);
         const auto end = record.end(secondary_rows, kind);
         BOOST_CHECK_EQUAL(record.bound(secondary_rows, numbered(type, 20), true), end);
         BOOST_CHECK_EQUAL(record.find_primary(secondary_rows, numbered(type, 99), 99), end);
         BOOST_CHECK_EQUAL(record.adjacent(kind, first), second);
         BOOST_CHECK_EQUAL(record.adjacent(kind, second, true), first);
         BOOST_CHECK_EQUAL(record.adjacent(kind, end, true), second);
         BOOST_CHECK_EQUAL(record.adjacent(kind, end), -1);
         const auto added = record.store(secondary_rows, 99, "alice"_n, numbered(type, 10));
         record.update(kind, first, "carol"_n, numbered(type, 30));
         BOOST_CHECK_EQUAL(record.adjacent(kind, first, true), second);
         BOOST_CHECK_EQUAL(record.find_primary(secondary_rows, numbered(type, 0), 7), first);
         record.remove(kind, added);
         const auto recreated = record.store(secondary_rows, 99, "bob"_n, numbered(type, 40));
         BOOST_CHECK_NE(recreated, added);
         BOOST_CHECK_EQUAL(record.adjacent(kind, first), recreated);
      }
   });
   BOOST_CHECK_EQUAL(expected.writes().size(), 25u);
   BOOST_REQUIRE(replay_secondary_test_script(expected, events));
   BOOST_CHECK(expected.matches_base_reads(primary, [] {}));
   BOOST_CHECK(primary.read_primary({secondary_rows, primary_query_kind::find, 7}, 3).row->value ==
               std::vector<char>({'a', 'b', 'c'}));
   // Every outer owner checkpoint must propagate, not become false/success.
   struct cancelled {};
   for (size_t stop = 1; stop <= events.size() + 1; ++stop) {
      size_t count = 0;
      BOOST_CHECK_THROW(replay_secondary_test_script(expected, events, false, [&] {
         if (++count == stop) throw cancelled{};
      }), cancelled);
      BOOST_CHECK_EQUAL(count, stop);
   }
}

BOOST_FIXTURE_TEST_CASE(typed_recreation_and_missing_outputs_keep_exact_float_bits, secondary_fixture) {
   const std::array tables{secondary_rows};
   const auto base = frozen_primary_state::capture(primary, tables);
   for (unsigned type = 0; type < 5; ++type) {
      const auto kind = static_cast<secondary_key_kind>(type);
      auto zero = numbered(type, 0), stored = zero, sentinel = numbered(type, 99);
      if (type == 3) {
         stored = secondary_float64{uint64_t{1} << 63};
         sentinel = secondary_float64{0x7ff8000000000001ULL};
      } else if (type == 4) {
         stored = secondary_float128{uint128_t{1} << 127};
         sentinel = secondary_float128{(uint128_t{0x7fff} << 112) | 1};
      }
      std::vector<secondary_test_event> events;
      primary_state_overlay state(base); secondary_journal_recorder record(state, events);
      BOOST_CHECK_EQUAL(record.end(secondary_rows, kind), -1);
      BOOST_CHECK_EQUAL(record.find_primary(secondary_rows, sentinel, 7), -1);
      BOOST_CHECK_EQUAL(record.find_secondary(secondary_rows, zero), -1);
      BOOST_CHECK_EQUAL(record.bound(secondary_rows, zero), -1);
      BOOST_CHECK_EQUAL(record.bound(secondary_rows, zero, true), -1);
      for (unsigned generation = 0; generation < 2; ++generation) {
         const auto handle = record.store(secondary_rows, 7, "alice"_n, stored);
         BOOST_CHECK_EQUAL(record.find_primary(secondary_rows, sentinel, 7), handle);
         BOOST_CHECK_EQUAL(record.bound(secondary_rows, zero), handle);
         BOOST_CHECK_EQUAL(record.find_secondary(secondary_rows, zero), handle);
         events.emplace_back(state.write_count());
         state.store(secondary_rows, 7, "bob"_n, std::span<const char>("x", 1));
         record.remove(kind, handle);
         const auto end = record.end(secondary_rows, kind);
         BOOST_CHECK_LT(end, -1); BOOST_CHECK_EQUAL(record.adjacent(kind, end, true), -1);
         events.emplace_back(state.write_count()); state.remove(secondary_rows, 7);
         BOOST_CHECK_EQUAL(record.end(secondary_rows, kind), -1);
      }
      const auto expected = state.seal();
      BOOST_CHECK_EQUAL(expected.writes().size(), 8u);
      BOOST_CHECK(replay_secondary_test_script(expected, events));
      BOOST_CHECK(!primary.read_table(secondary_rows));
   }
}

BOOST_FIXTURE_TEST_CASE(unused_bundles_and_independent_frames_preserve_cursor_lifetimes, secondary_fixture) {
   const std::array tables{secondary_rows};
   const auto base = frozen_primary_state::capture(primary, tables);
   {
      primary_state_overlay state(base); secondary_cursor_group group(state, "token"_n);
      group.set_receiver_for_notification("bob"_n); group.set_receiver_for_notification("token"_n);
      BOOST_CHECK_THROW(group.get(static_cast<secondary_key_kind>(255)), secondary_state_invalid_query);
      BOOST_CHECK(state.seal().primary_only());
   }
   for (unsigned type = 0; type < 5; ++type) {
      const auto kind = static_cast<secondary_key_kind>(type);
      primary_state_overlay state(base); secondary_cursor_group parent(state, "token"_n), child(state, "token"_n);
      const auto first = parent.get(kind).store(secondary_rows, 7, "alice"_n, numbered(type, 10));
      const auto second = parent.get(kind).store(secondary_rows, 19, "alice"_n, numbered(type, 20));
      auto key = numbered(type, 0);
      const auto child_second = child.get(kind).find_primary(secondary_rows, key, 19);
      BOOST_CHECK_EQUAL(first, 0); BOOST_CHECK_EQUAL(second, 1); BOOST_CHECK_EQUAL(child_second, 0);
      child.get(kind).update(child_second, {}, numbered(type, 5));
      parent.set_receiver_for_notification("bob"_n);
      uint64_t out = 999;
      BOOST_CHECK_EQUAL(parent.get(kind).previous(first, out), second); BOOST_CHECK_EQUAL(out, 19u);
      parent.set_receiver_for_notification("token"_n);
      child.get(kind).remove(child_second);
      BOOST_CHECK_THROW(parent.get(kind).next(second, out), primary_state_unsupported);
   }
   {
      primary_state_overlay state(base); secondary_cursor_group group(state, "token"_n);
      const auto handle = group.get(secondary_key_kind::uint64).store(secondary_rows, 7, "alice"_n, uint64_t{10});
      group.set_receiver_for_notification("bob"_n);
      BOOST_CHECK_THROW(group.get(secondary_key_kind::uint64).update(handle, {}, uint64_t{20}), table_access_violation);
   }
}

BOOST_FIXTURE_TEST_CASE(typed_replay_rejects_tampered_outputs_order_and_missing_effects, secondary_fixture) {
   std::vector<secondary_test_event> events;
   const auto expected = discover_overlay(primary, [&](primary_state_overlay& state) {
      events.clear(); secondary_journal_recorder record(state, events);
      for (unsigned type = 0; type < 5; ++type) {
         const auto kind = static_cast<secondary_key_kind>(type);
         record.end(secondary_rows, kind);
         const auto a = record.store(secondary_rows, 7, "alice"_n, numbered(type, 10));
         const auto b = record.store(secondary_rows, 19, "bob"_n, numbered(type, 20));
         record.find_primary(secondary_rows, numbered(type, 99), 7);
         record.find_secondary(secondary_rows, numbered(type, 10));
         record.bound(secondary_rows, numbered(type, 11));
         record.bound(secondary_rows, numbered(type, 10), true);
         record.adjacent(kind, a); record.adjacent(kind, b, true);
         record.update(kind, a, {}, numbered(type, 30));
         record.remove(kind, b);
      }
   });
   BOOST_REQUIRE(replay_secondary_test_script(expected, events));
   BOOST_CHECK(!replay_secondary_test_script(expected, events, true));
   for (size_t index = 0; index < events.size(); ++index) {
      auto bad = events;
      auto& event = std::get<isolated_secondary_cursor_event>(bad[index]);
      std::visit([](auto& item) {
         using T = std::decay_t<decltype(item)>;
         if constexpr (std::is_same_v<T, isolated_secondary_write>) ++item.write_index;
         else item.result = item.result >= 0 ? item.result + 100 : 100;
      }, event);
      BOOST_CHECK(!replay_secondary_test_script(expected, bad));
      if (const auto* original = std::get_if<isolated_secondary_write>(&std::get<isolated_secondary_cursor_event>(events[index]))) {
         bad = events;
         std::get<isolated_secondary_write>(std::get<isolated_secondary_cursor_event>(bad[index])).kind =
            static_cast<secondary_key_kind>((static_cast<unsigned>(original->kind) + 1) % 5);
         BOOST_CHECK(!replay_secondary_test_script(expected, bad));
         bad = events; bad.erase(bad.begin() + index);
         BOOST_CHECK(!replay_secondary_test_script(expected, bad));
      } else {
         bad = events;
         std::visit([](auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, isolated_secondary_find_primary> || std::is_same_v<T, isolated_secondary_bound>)
               item.key_after = numbered(static_cast<unsigned>(secondary_kind(item.key_after)), 999);
            else if constexpr (std::is_same_v<T, isolated_secondary_find_secondary> || std::is_same_v<T, isolated_secondary_adjacent>)
               item.primary_after = 999;
            else if constexpr (std::is_same_v<T, isolated_secondary_end>) item.kind = static_cast<secondary_key_kind>(255);
         }, std::get<isolated_secondary_cursor_event>(bad[index]));
         BOOST_CHECK(!replay_secondary_test_script(expected, bad));
      }
   }
   auto prefix = events; prefix.pop_back(); BOOST_CHECK(!replay_secondary_test_script(expected, prefix));
   auto repeated = events; repeated.push_back(events.back()); BOOST_CHECK(!replay_secondary_test_script(expected, repeated));
   auto reordered = events; std::swap(reordered[1], reordered[2]); BOOST_CHECK(!replay_secondary_test_script(expected, reordered));
}

BOOST_FIXTURE_TEST_CASE(typed_helper_checks_ranges_readonly_wrong_variants_and_owner, secondary_fixture) {
   const std::array tables{secondary_rows}; const auto base = frozen_primary_state::capture(primary, tables);
   primary_state_overlay original(base); original.store_secondary(secondary_rows, 7, "alice"_n, uint64_t{10});
   const auto expected = original.seal();
   const isolated_secondary_cursor_event event = isolated_secondary_write{secondary_key_kind::uint64, 0, 0};
   for (unsigned mode = 0; mode < 6; ++mode) {
      primary_state_overlay state(base); secondary_cursor_group group(state, "token"_n);
      size_t next = mode == 0 ? 2 : 0, end = mode == 1 ? 2 : mode == 2 ? 0 : 1;
      if (mode == 3) state.store(secondary_rows, 99, "alice"_n, std::span<const char>("x", 1));
      if (mode == 4) group.set_receiver_for_notification("bob"_n);
      BOOST_CHECK(!replay_isolated_secondary_cursor_event(group, event, expected, next, end, mode == 5));
      BOOST_CHECK_EQUAL(state.write_count(), mode == 3 ? 1u : 0u);
   }
   primary_state_overlay primary_original(base);
   primary_original.store(secondary_rows, 7, "alice"_n, std::span<const char>("x", 1));
   const auto primary_expected = primary_original.seal();
   primary_state_overlay replay(base); secondary_cursor_group group(replay, "token"_n); size_t next = 0;
   BOOST_CHECK(!replay_isolated_secondary_cursor_event(group, event, primary_expected, next, 1, false));
   auto foreign = std::async(std::launch::async, [&] {
      size_t foreign_next = 0;
      return replay_isolated_secondary_cursor_event(group, event, expected, foreign_next, 1, false);
   });
   BOOST_CHECK(!foreign.get()); BOOST_CHECK_EQUAL(replay.write_count(), 0u);
   BOOST_CHECK(replay_isolated_secondary_cursor_event(group, event, expected, next, 1, false));
   BOOST_CHECK_EQUAL(next, 1u); BOOST_CHECK(replay.seal().writes() == expected.writes());
   primary_state_overlay empty(base); const auto empty_expected = empty.seal();
   primary_state_overlay ends(base); secondary_cursor_group end_group(ends, "token"_n); next = 0;
   const isolated_secondary_cursor_event end_event = isolated_secondary_adjacent{
      secondary_key_kind::uint64, false, std::numeric_limits<int32_t>::min(), -1, 777, 777};
   BOOST_CHECK(replay_isolated_secondary_cursor_event(end_group, end_event, empty_expected, next, 0, true));
   const auto used = ends.seal(); BOOST_CHECK(!used.primary_only());
   BOOST_CHECK(used.secondary_read_queries().empty());
}

BOOST_FIXTURE_TEST_CASE(whole_log_comparison_is_required_and_readonly_queries_remain_valid, secondary_fixture) {
   const std::array tables{secondary_rows}; const auto base = frozen_primary_state::capture(primary, tables);
   std::vector<secondary_test_event> events;
   primary_state_overlay original(base); secondary_journal_recorder record(original, events);
   const auto a = record.store(secondary_rows, 7, "alice"_n, uint64_t{10});
   const auto b = record.store(secondary_rows, 19, "bob"_n, uint64_t{20});
   record.update(secondary_key_kind::uint64, a, {}, uint64_t{30});
   const auto expected = original.seal();
   BOOST_REQUIRE(replay_secondary_test_script(expected, events));
   auto bad = events;
   std::get<isolated_secondary_write>(std::get<isolated_secondary_cursor_event>(bad.back())).iterator = b;
   BOOST_CHECK(!replay_secondary_test_script(expected, bad));
   // A structurally valid handle may name the WRONG row. The local helper is
   // deliberately not the whole proof; final exact typed-log equality catches it.
   primary_state_overlay replay(base); secondary_cursor_group group(replay, "token"_n); size_t next = 0;
   for (const auto& event : bad)
      BOOST_REQUIRE(replay_isolated_secondary_cursor_event(group,
         std::get<isolated_secondary_cursor_event>(event), expected, next, expected.writes().size(), false));
   BOOST_CHECK(replay.seal().writes() != expected.writes());
   put(7, uint64_t{10}); put(19, uint64_t{20});
   const auto reads = discover_overlay(primary, [&](primary_state_overlay& state) {
      events.clear(); secondary_journal_recorder reader(state, events);
      const auto handle = reader.find_primary(secondary_rows, uint64_t{999}, 7);
      reader.find_secondary(secondary_rows, uint64_t{10});
      reader.bound(secondary_rows, uint64_t{11}); reader.bound(secondary_rows, uint64_t{10}, true);
      reader.adjacent(secondary_key_kind::uint64, handle);
      reader.end(secondary_rows, secondary_key_kind::uint64);
   });
   BOOST_REQUIRE(reads.writes().empty()); BOOST_CHECK(replay_secondary_test_script(reads, events, true));
   primary_state_overlay limited(reads.base()); secondary_cursor_group no_cache(limited, "token"_n, 0);
   next = 0;
   BOOST_CHECK(!replay_isolated_secondary_cursor_event(no_cache,
      std::get<isolated_secondary_cursor_event>(events.front()), reads, next, 0, true));
   BOOST_CHECK_THROW(limited.seal(), primary_state_invalid_attempt);
}

BOOST_FIXTURE_TEST_CASE(complete_replay_keeps_primary_dependency_coverage_and_requires_exact_secondary_effects, secondary_fixture) {
   db.add_index<account_index>();
   db.create<account_object>([](auto& account) { account.name = "alice"_n; });
   const std::array tables{secondary_rows}; const std::array accounts{"alice"_n};
   const auto base = frozen_primary_state::capture_queries_with_secondary(primary, tables, {}, {}, {}, {}, accounts);
   primary_state_overlay empty_state(base); const auto empty = empty_state.seal();
   primary_state_overlay primary_state(base);
   primary_state.read_account("alice"_n);
   primary_state.locate_primary({secondary_rows, primary_query_kind::find, 7});
   const auto primary_dependencies = primary_state.seal();
   // db_get / is_account are not cursor events. Their extra dependencies must
   // survive in the expected result for live owner checks, not be discarded.
   BOOST_CHECK(validate_isolated_replay_state(empty, primary_dependencies));
   BOOST_CHECK(!validate_isolated_replay_state(primary_dependencies, empty));
   primary_state_overlay end_state(base); secondary_cursor_group ends(end_state, "token"_n);
   BOOST_CHECK_EQUAL(ends.get(secondary_key_kind::uint64).end(secondary_rows), -1);
   const auto end_only = end_state.seal();
   BOOST_CHECK(end_only.secondary_read_queries().empty());
   BOOST_CHECK(!validate_isolated_replay_state(empty, end_only));
   BOOST_CHECK(!validate_isolated_replay_state(end_only, empty));
   primary_state_overlay query_state(base);
   query_state.read_secondary({secondary_rows, secondary_query_kind::find_primary, uint64_t{0}, 7});
   const auto query_only = query_state.seal();
   BOOST_CHECK(!validate_isolated_replay_state(end_only, query_only));
   BOOST_CHECK(!validate_isolated_replay_state(query_only, end_only));
   primary_state_overlay extra_query_state(base);
   extra_query_state.read_secondary({secondary_rows, secondary_query_kind::find_primary, uint64_t{0}, 7});
   extra_query_state.read_secondary({secondary_rows, secondary_query_kind::find_primary, uint64_t{0}, 19});
   const auto extra_query = extra_query_state.seal();
   BOOST_CHECK(!validate_isolated_replay_state(query_only, extra_query));
   const auto other_base = frozen_primary_state::capture_queries_with_secondary(primary, tables, {}, {}, {}, {}, accounts);
   primary_state_overlay other_state(other_base);
   BOOST_CHECK(!validate_isolated_replay_state(other_state.seal(), empty));
   size_t checkpoints = 0;
   BOOST_REQUIRE(validate_isolated_replay_state(extra_query, extra_query, [&] { ++checkpoints; }));
   BOOST_CHECK_GE(checkpoints, 5u);
   struct cancelled {};
   for (size_t target = 1; target <= checkpoints; ++target) {
      size_t visited = 0;
      BOOST_CHECK_THROW(validate_isolated_replay_state(extra_query, extra_query, [&] {
         if (++visited == target) throw cancelled{};
      }), cancelled);
   }
   BOOST_CHECK_THROW(validate_isolated_replay_state(empty, end_only, [] { throw cancelled{}; }), cancelled);
}

BOOST_FIXTURE_TEST_CASE(typed_write_bytes_and_full_replay_match_primary_secondary_and_removed_keys, secondary_fixture) {
   const std::array tables{secondary_rows}; const auto base = frozen_primary_state::capture(primary, tables);
   for (unsigned type = 0; type < 5; ++type) {
      const auto make = [&](name payer, bool reverse) {
         primary_state_overlay state(base);
         const std::array<char, 3> value{'a', 'b', 'c'};
         state.store(secondary_rows, 7, "alice"_n, value);
         state.store_secondary(secondary_rows, reverse ? 19 : 7, payer, numbered(type, 10));
         state.store_secondary(secondary_rows, reverse ? 7 : 19, payer, numbered(type, 20));
         state.update_secondary(secondary_rows, 7, {}, numbered(type, 30));
         state.remove_secondary(secondary_rows, 7, numbered(type, 0));
         state.remove(secondary_rows, 7);
         return state.seal();
      };
      const auto expected = make("alice"_n, false);
      BOOST_REQUIRE_EQUAL(expected.writes().size(), 6u);
      BOOST_CHECK_EQUAL(isolated_state_write_bytes(expected.writes()[0]), 3u);
      for (size_t i = 1; i < 5; ++i)
         BOOST_CHECK_EQUAL(isolated_state_write_bytes(expected.writes()[i]), secondary_key_bytes(numbered(type, 0)));
      BOOST_CHECK_EQUAL(isolated_state_write_bytes(expected.writes()[5]), 0u);
      BOOST_CHECK(validate_isolated_replay_state(make("alice"_n, false), expected));
      BOOST_CHECK(!validate_isolated_replay_state(make("bob"_n, false), expected));
      BOOST_CHECK(!validate_isolated_replay_state(make("alice"_n, true), expected));
      BOOST_CHECK(!primary.read_table(secondary_rows));
   }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_secondary_capture_tests)

BOOST_AUTO_TEST_CASE(normalization_preserves_input_bits_and_exact_dictionary_order) {
   static_assert(std::is_nothrow_copy_assignable_v<std::optional<state_capture_request>>);
   for(unsigned type=0;type<5;++type) {
      for(unsigned kind=0;kind<8;++kind) {
         const auto q=query(type,static_cast<secondary_query_kind>(kind),13,19);
         const auto normalized=normalize_secondary_query(q);
         BOOST_CHECK(normalized==normalize_secondary_query(normalized));
         BOOST_CHECK(secondary_kind(normalized.key)==secondary_kind(q.key));
         if(kind==0 || kind==4 || kind==5)
            BOOST_CHECK(normalized.key==(type==2?secondary_key{key256_t{}}:numbered(type,0)));
         else BOOST_CHECK(normalized.key==q.key);
         BOOST_CHECK_EQUAL(normalized.primary_key,(kind==0 || kind>=6)?19u:0u);
      }
   }
   const secondary_query positive{secondary_rows,secondary_query_kind::lower_bound,secondary_float64{0},0};
   auto negative=positive;negative.key=secondary_float64{uint64_t{1}<<63};
   BOOST_CHECK(secondary_key_equivalent(positive.key,negative.key));
   std::set<secondary_query,secondary_query_less> distinct{positive,negative};
   BOOST_CHECK_EQUAL(distinct.size(),2u);
   for(const auto& left:distinct) for(const auto& right:distinct) {
      BOOST_CHECK(!(secondary_query_less{}(left,right) && secondary_query_less{}(right,left)));
      BOOST_CHECK_EQUAL(!secondary_query_less{}(left,right) && !secondary_query_less{}(right,left),left==right);
   }
   BOOST_CHECK_EQUAL(secondary_key_bytes(uint64_t{}),8u);
   BOOST_CHECK_EQUAL(secondary_key_bytes(uint128_t{}),16u);
   BOOST_CHECK_EQUAL(secondary_key_bytes(key256_t{}),32u);
   BOOST_CHECK_EQUAL(secondary_key_bytes(secondary_float64{}),8u);
   BOOST_CHECK_EQUAL(secondary_key_bytes(secondary_float128{}),16u);
}

BOOST_FIXTURE_TEST_CASE(primary_and_all_five_families_share_metadata_and_one_capture_budget,secondary_fixture) {
   put_primary(*this);
   for(unsigned type=0;type<5;++type) put(7,numbered(type,11));
   const std::vector<primary_query> pq{{secondary_rows,primary_query_kind::find,7}};
   const auto sq=capture_primary_queries();
   const primary_snapshot_limits exact{1,6,83,6,0};
   const auto base=frozen_primary_state::capture_queries_with_secondary(primary,{},pq,sq,exact);
   BOOST_CHECK_EQUAL(base->table_count(),1u);BOOST_CHECK_EQUAL(base->row_count(),6u);
   BOOST_CHECK_EQUAL(base->value_bytes(),83u);BOOST_CHECK_EQUAL(base->account_count(),0u);
   BOOST_CHECK(base->read_primary(pq[0],3)==primary.read_primary(pq[0],3));
   for(const auto& q:sq) {
      BOOST_CHECK(base->read_secondary(q)==view.read_secondary(q));
      BOOST_REQUIRE(base->read_secondary(q).table);
      BOOST_CHECK_EQUAL(base->read_secondary(q).table->count,6u);
      BOOST_CHECK(base->read_secondary(q).table==base->read_table(secondary_rows));
   }
   for(unsigned boundary=0;boundary<4;++boundary) {
      auto limits=exact;
      if(boundary==0) --limits.max_tables;
      if(boundary==1) --limits.max_rows;
      if(boundary==2) --limits.max_value_bytes;
      if(boundary==3) --limits.max_queries;
      BOOST_CHECK_THROW(frozen_primary_state::capture_queries_with_secondary(primary,{},pq,sq,limits),primary_state_budget_exceeded);
   }
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries_with_secondary(primary,{},pq,{},
      primary_snapshot_limits{1,6,83,0,0}),primary_state_budget_exceeded);
   const std::vector<secondary_query> duplicate{
      query(0,secondary_query_kind::find_primary,123,7),query(0,secondary_query_kind::find_primary,456,7),
      query(0,secondary_query_kind::lower_bound,11),query(0,secondary_query_kind::first)};
   const auto deduplicated=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},duplicate,{1,1,8,4,0});
   BOOST_CHECK_EQUAL(deduplicated->row_count(),1u);BOOST_CHECK_EQUAL(deduplicated->value_bytes(),8u);
   for(const auto& q:duplicate) BOOST_CHECK(deduplicated->read_secondary(q)==view.read_secondary(q));
   const std::vector<secondary_query> absent{query(0,secondary_query_kind::find_primary,0,999)};
   const auto zero=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},absent,{1,0,0,1,0});
   BOOST_CHECK_EQUAL(zero->row_count(),0u);BOOST_CHECK_EQUAL(zero->value_bytes(),0u);
   BOOST_CHECK(!zero->read_secondary(absent[0]).row);
}

BOOST_FIXTURE_TEST_CASE(uncaptured_queries_are_requests_not_absent_rows_or_inferred_neighbors,secondary_fixture) {
   const std::array<table_key,1> known{secondary_rows};
   const auto absent=frozen_primary_state::capture_queries_with_secondary(primary,known,{},{});
   for(unsigned type=0;type<5;++type) for(unsigned kind=0;kind<8;++kind) {
      const auto q=query(type,static_cast<secondary_query_kind>(kind),13,7);
      BOOST_CHECK(!absent->read_secondary(q).table);BOOST_CHECK(!absent->read_secondary(q).row);
   }
   table();
   const auto empty=frozen_primary_state::capture_queries_with_secondary(primary,known,{},{});
   for(unsigned type=0;type<5;++type) check_secondary_request(*empty,query(type,secondary_query_kind::find_primary,99,7));
   put(7,numbered(0,11));put(19,numbered(0,30));
   const std::array<secondary_query,1> queries{query(0,secondary_query_kind::find_primary,99,7)};
   const auto sparse=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},queries);
   BOOST_CHECK(sparse->read_secondary(queries[0])==view.read_secondary(queries[0]));
   check_secondary_request(*sparse,query(0,secondary_query_kind::find_primary,99,19));
   check_secondary_request(*sparse,query(0,secondary_query_kind::next,11,7));
   check_secondary_request(*sparse,query(0,secondary_query_kind::lower_bound,11));
   check_secondary_request(*sparse,query(1,secondary_query_kind::find_primary,99,7));
   auto unknown=queries[0];unknown.table.scope="carol"_n;check_secondary_request(*sparse,unknown);
   // A full PRIMARY capture is not a full SECONDARY capture.
   const auto primary_only=frozen_primary_state::capture(primary,known);
   check_secondary_request(*primary_only,queries[0]);
   BOOST_CHECK(!absent->read_secondary(queries[0]).row); // old table absence remains an owned fact
}

BOOST_FIXTURE_TEST_CASE(frozen_rows_survive_owner_changes_and_are_shared_without_live_database_access,secondary_fixture) {
   const auto queries=capture_primary_queries();
   std::vector<secondary_read_result> expected;
   for(unsigned type=0;type<5;++type) put(7,numbered(type,11));
   for(const auto& q:queries) expected.push_back(view.read_secondary(q));
   const auto base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},queries);
   for(unsigned type=0;type<5;++type) put(7,numbered(type,19),secondary_rows,"carol"_n);
   for(size_t i=0;i<queries.size();++i) {
      BOOST_CHECK(base->read_secondary(queries[i])==expected[i]);
      BOOST_CHECK(view.read_secondary(queries[i])!=expected[i]);
   }
   for(unsigned type=0;type<5;++type) erase(7,numbered(type,0));
   BOOST_CHECK(!primary.read_table(secondary_rows));
   for(unsigned type=0;type<5;++type) put(7,numbered(type,29));
   BOOST_REQUIRE(primary.read_table(secondary_rows));
   BOOST_REQUIRE(expected[0].table);
   BOOST_CHECK(primary.read_table(secondary_rows)->object_id!=expected[0].table->object_id);
   std::vector<std::future<bool>> readers;
   for(unsigned worker=0;worker<4;++worker) readers.push_back(std::async(std::launch::async,[base,queries,expected] {
      for(unsigned round=0;round<100;++round) for(size_t i=0;i<queries.size();++i)
         if(base->read_secondary(queries[i])!=expected[i]) return false;
      return true;
   }));
   // Only the owner touches live DB; readers use an independent immutable base.
   for(unsigned round=0;round<30;++round) for(unsigned type=0;type<5;++type) put(7,numbered(type,round));
   for(auto& reader:readers) BOOST_CHECK(reader.get());
   for(size_t i=0;i<queries.size();++i) BOOST_CHECK(base->read_secondary(queries[i])==expected[i]);
}

BOOST_FIXTURE_TEST_CASE(exact_zero_queries_and_nan_preconditions_survive_sparse_capture,secondary_fixture) {
   const secondary_query plus{secondary_rows,secondary_query_kind::find_secondary,secondary_float64{0},0};
   auto minus=plus;minus.key=secondary_float64{uint64_t{1}<<63};
   put(7,minus.key);put(9,plus.key);
   const std::array<secondary_query,1> one{plus};
   const auto base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},one);
   BOOST_CHECK(base->read_secondary(plus)==view.read_secondary(plus));
   check_secondary_request(*base,minus);
   const std::array<secondary_query,2> both{plus,minus};
   const auto both_base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},both,{1,1,8,2,0});
   BOOST_CHECK_EQUAL(both_base->row_count(),1u);
   BOOST_CHECK(both_base->read_secondary(plus)==both_base->read_secondary(minus));
   auto nan=plus;nan.key=secondary_float64{0x7ff0000000000001ULL};
   for(const auto& table:std::array<table_key,2>{secondary_rows,table_key{"token"_n,"carol"_n,"rows"_n}}) {
      nan.table=table;
      const std::array<table_key,1> tables{table};
      const auto metadata=frozen_primary_state::capture_queries_with_secondary(primary,tables,{},{});
      BOOST_CHECK_THROW(metadata->read_secondary(nan),secondary_state_invalid_query);
      const std::array<secondary_query,1> invalid{nan};
      BOOST_CHECK_THROW(frozen_primary_state::capture_queries_with_secondary(primary,{}, {},invalid),secondary_state_invalid_query);
      auto wrong=nan;wrong.kind=static_cast<secondary_query_kind>(255);
      BOOST_CHECK_THROW(metadata->read_secondary(wrong),secondary_state_invalid_query);
   }
   nan.table=secondary_rows;nan.kind=secondary_query_kind::find_primary;nan.primary_key=7;
   const std::array<secondary_query,1> output_only{nan};
   const auto output_base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},output_only);
   BOOST_CHECK(output_base->read_secondary(nan)==view.read_secondary(nan));
}

BOOST_FIXTURE_TEST_CASE(secondary_adapter_inherits_original_database_and_owner_identity,secondary_fixture) {
   const auto q=query(0,secondary_query_kind::find_primary,0,7);
   put(7,numbered(0,11));
   chainbase_secondary_state_view paired(primary);
   BOOST_CHECK(paired.read_secondary(q)==view.read_secondary(q));
   secondary_fixture other;other.put(7,numbered(0,88));
   chainbase_secondary_state_view other_paired(other.primary);
   BOOST_CHECK(paired.read_secondary(q)!=other_paired.read_secondary(q));
   auto foreign=std::async(std::launch::async,[&] {
      try {chainbase_secondary_state_view rejected(primary);}
      catch(const state_view_wrong_thread&) {return true;}return false;
   });
   BOOST_CHECK(foreign.get());
   auto capture=std::async(std::launch::async,[&] {
      try {frozen_primary_state::capture_queries_with_secondary(primary,{}, {},{});}
      catch(const state_view_wrong_thread&) {return true;}return false;
   });
   BOOST_CHECK(capture.get());
   BOOST_CHECK(paired.read_secondary(q)==view.read_secondary(q));
}

BOOST_FIXTURE_TEST_CASE(account_capture_and_all_checkpoint_cancellations_publish_no_partial_snapshot,secondary_fixture) {
   db.add_index<account_index>();
   db.create<account_object>([](auto& row){row.name="alice"_n;});
   const std::array<name,2> accounts{"alice"_n,"carol"_n};
   const auto sq=capture_primary_queries();
   for(unsigned type=0;type<5;++type) put(7,numbered(type,11));
   put_primary(*this);
   const std::array<table_key,1> keys{secondary_rows};
   const std::array<primary_query,1> pq{primary_query{secondary_rows,primary_query_kind::find,7}};
   size_t checkpoints=0;
   const auto complete=frozen_primary_state::capture_queries_with_secondary(primary,keys,pq,sq,{},[&]{++checkpoints;},accounts);
   BOOST_REQUIRE(checkpoints>=8u);BOOST_CHECK_EQUAL(complete->account_count(),2u);
   BOOST_CHECK(complete->read_account("alice"_n)==primary.read_account("alice"_n));
   BOOST_CHECK(!complete->read_account("carol"_n));
   auto limits=primary_snapshot_limits{};limits.max_accounts=1;
   BOOST_CHECK_THROW(frozen_primary_state::capture_queries_with_secondary(primary,keys,pq,sq,limits,{},accounts),primary_state_budget_exceeded);
   struct cancelled {};
   for(size_t stop=1;stop<=checkpoints;++stop) {
      size_t visited=0;
      std::shared_ptr<const frozen_primary_state> published;
      BOOST_CHECK_THROW(published=frozen_primary_state::capture_queries_with_secondary(primary,keys,pq,sq,{},[&] {
         if(++visited==stop) throw cancelled{};
      },accounts),cancelled);
      BOOST_CHECK(!published);BOOST_CHECK_EQUAL(visited,stop);
      const auto healthy=frozen_primary_state::capture_queries_with_secondary(primary,keys,pq,sq,{}, {},accounts);
      for(const auto& q:sq) BOOST_CHECK(healthy->read_secondary(q)==complete->read_secondary(q));
      BOOST_CHECK(healthy->read_primary(pq[0],3)==complete->read_primary(pq[0],3));
   }
}

BOOST_FIXTURE_TEST_CASE(seeded_capture_rounds_match_live_owner_and_preserve_previous_snapshots,secondary_fixture) {
   std::mt19937_64 random(0x39dba610ULL);
   std::shared_ptr<const frozen_primary_state> previous;
   std::vector<secondary_query> previous_queries;
   std::vector<secondary_read_result> previous_results;
   for(unsigned round=0;round<100;++round) {
      for(unsigned mutation=0;mutation<5;++mutation) {
         const unsigned type=random()%5;const uint64_t pk=random()%16;
         if(random()%3==0) erase(pk,numbered(type,0));else put(pk,numbered(type,random()%13));
      }
      if(previous) for(size_t i=0;i<previous_queries.size();++i)
         BOOST_CHECK(previous->read_secondary(previous_queries[i])==previous_results[i]);
      std::vector<secondary_query> queries;
      for(unsigned sample=0;sample<20;++sample)
         queries.push_back(query(random()%5,static_cast<secondary_query_kind>(random()%8),random()%15,random()%18));
      const auto base=frozen_primary_state::capture_queries_with_secondary(primary,{}, {},queries);
      previous_results.clear();
      for(const auto& q:queries) {
         const auto expected=view.read_secondary(q);
         BOOST_CHECK(base->read_secondary(q)==expected);previous_results.push_back(expected);
      }
      previous=base;previous_queries=std::move(queries);
   }
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_secondary_capture_tests)
BOOST_FIXTURE_TEST_CASE(owned_secondary_range_row_proves_only_its_typed_primary_point, secondary_fixture) {
   for (unsigned type = 0; type < 5; ++type) BOOST_TEST_CONTEXT("type=" << type) {
      put(7, numbered(type, 11)); put(3, numbered(type, 11)); // duplicate, lowest primary is 3
      const auto range = query(type, secondary_query_kind::lower_bound, 11);
      const auto point = query(type, secondary_query_kind::find_primary, 99, 3);
      const auto base = frozen_primary_state::capture_queries_with_secondary(primary, {}, {}, std::array{range});
      const auto expected = view.read_secondary(point);
      BOOST_REQUIRE(expected.row); BOOST_CHECK_EQUAL(expected.row->primary_key, 3u);
      BOOST_CHECK_EQUAL(base->row_count(), 1u);
      check_secondary_request(*base, query(type, secondary_query_kind::find_primary, 99, 7));
      check_secondary_request(*base, query((type + 1) % 5, secondary_query_kind::find_primary, 99, 3));
      check_secondary_request(*base, query(type, secondary_query_kind::find_secondary, 11));
      check_secondary_request(*base, query(type, secondary_query_kind::next, 11, 3));
      check_secondary_request(*base, query(type, secondary_query_kind::previous, 11, 3));
      BOOST_CHECK_THROW(base->locate_primary({secondary_rows, primary_query_kind::find, 3}), primary_state_missing);
      BOOST_REQUIRE_NO_THROW(base->read_secondary(point));
      BOOST_CHECK(base->read_secondary(point) == expected);
      primary_state_overlay overlay(base);
      BOOST_CHECK(overlay.read_secondary(point) == expected);
      const auto result = overlay.seal();
      BOOST_REQUIRE_EQUAL(result.secondary_read_queries().size(), 1u);
      BOOST_CHECK(*result.secondary_read_queries().begin() == normalize_secondary_query(point));
      BOOST_CHECK(result.matches_base_reads(primary, [] {}));
      put(7, numbered(type, 21)); // unrelated key position changed, but our query is by primary
      BOOST_CHECK(result.matches_base_reads(primary, [] {}));
      put(3, numbered(type, 11), secondary_rows, "carol"_n);
      BOOST_CHECK(!result.matches_base_reads(primary, [] {}));
      put(3, numbered(type, 11));
      BOOST_CHECK(result.matches_base_reads(primary, [] {}));
      erase(3, numbered(type, 0)); put(3, numbered(type, 11));
      BOOST_CHECK(!result.matches_base_reads(primary, [] {}));
      BOOST_CHECK(base->read_secondary(point) == expected);
   }
}

BOOST_FIXTURE_TEST_CASE(derived_secondary_point_keeps_exact_float_bits_and_original_nan_preconditions, secondary_fixture) {
   const std::array<secondary_key, 2> negative_zero{
      secondary_float64{uint64_t{1} << 63}, secondary_float128{uint128_t{1} << 127}};
   const std::array<secondary_key, 2> nan{
      secondary_float64{0x7ff0000000000001ULL},
      secondary_float128{(uint128_t{0x7fff000000000000ULL} << 64) | 1}};
   for (unsigned index = 0; index < 2; ++index) {
      const unsigned type = index + 3;
      put(3, negative_zero[index]); put(7, numbered(type, 0));
      const auto range = query(type, secondary_query_kind::lower_bound, 0);
      const auto base = frozen_primary_state::capture_queries_with_secondary(primary, {}, {}, std::array{range});
      auto point = query(type, secondary_query_kind::find_primary, 0, 3);
      point.key = nan[index]; // ignored value, but retained type: valid original output-only query
      BOOST_REQUIRE_NO_THROW(base->read_secondary(point));
      BOOST_REQUIRE(base->read_secondary(point).row);
      BOOST_CHECK(base->read_secondary(point) == view.read_secondary(point));
      BOOST_CHECK(base->read_secondary(point).row->key == negative_zero[index]);
      auto invalid = range; invalid.key = nan[index];
      BOOST_CHECK_THROW(base->read_secondary(invalid), secondary_state_invalid_query);
      auto other_zero = range; other_zero.key = negative_zero[index];
      check_secondary_request(*base, other_zero); // do not merge signed-zero query identities
      primary_overlay_limits limits; limits.max_read_queries = 1;
      primary_state_overlay overlay(base, limits);
      BOOST_REQUIRE_NO_THROW(overlay.read_secondary(point));
      BOOST_CHECK_THROW(overlay.read_secondary(range), primary_state_budget_exceeded);
      BOOST_CHECK_THROW(overlay.seal(), primary_state_invalid_attempt);
   }
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_secondary_capture_tests)
BOOST_FIXTURE_TEST_CASE(derived_secondary_point_observes_private_update_remove_and_recreation, secondary_fixture) {
   for (unsigned type = 0; type < 5; ++type) BOOST_TEST_CONTEXT("type=" << type) {
      put(7, numbered(type, 11)); put(9, numbered(type, 99));
      const auto range = query(type, secondary_query_kind::lower_bound, 11);
      const auto point = query(type, secondary_query_kind::find_primary, 0, 7);
      const auto base = frozen_primary_state::capture_queries_with_secondary(primary, {}, {}, std::array{range});
      const auto original = view.read_secondary(point);
      BOOST_REQUIRE(original.row);
      primary_state_overlay overlay(base);

      BOOST_REQUIRE_NO_THROW(overlay.update_secondary(secondary_rows, 7, "carol"_n, numbered(type, 31)));
      auto found = overlay.read_secondary(point);
      BOOST_REQUIRE(found.row);
      BOOST_CHECK(found.row->key == numbered(type, 31));
      BOOST_CHECK(found.row->payer == "carol"_n);
      BOOST_CHECK_EQUAL(found.row->object_id, original.row->object_id);
      overlay.remove_secondary(secondary_rows, 7, numbered(type, 0));
      found = overlay.read_secondary(point);
      BOOST_REQUIRE(found.table);
      BOOST_CHECK(!found.row);
      BOOST_CHECK_EQUAL(found.table->count, original.table->count - 1);
      overlay.store_secondary(secondary_rows, 7, "alice"_n, numbered(type, 41));
      found = overlay.read_secondary(point);
      BOOST_REQUIRE(found.row);
      BOOST_CHECK_LT(found.row->object_id, 0);
      BOOST_CHECK(found.row->key == numbered(type, 41));
      BOOST_CHECK(found.row->payer == "alice"_n);
      BOOST_CHECK_EQUAL(found.table->count, original.table->count);
      const auto recreated_id = found.row->object_id;
      overlay.update_secondary(secondary_rows, 7, name{}, numbered(type, 51));
      found = overlay.read_secondary(point);
      BOOST_REQUIRE(found.row);
      BOOST_CHECK(found.row->key == numbered(type, 51));
      BOOST_CHECK_EQUAL(found.row->object_id, recreated_id);
      BOOST_CHECK(found.row->payer == "alice"_n);

      const auto result = overlay.seal();
      BOOST_CHECK_EQUAL(result.writes().size(), 4u);
      BOOST_CHECK_EQUAL(result.secondary_read_queries().size(), 1u);
      BOOST_CHECK(result.secondary_read_queries().contains(normalize_secondary_query(point)));
      BOOST_CHECK(result.matches_base_reads(primary, [] {}));
      BOOST_CHECK(view.read_secondary(point) == original);
      BOOST_CHECK(base->read_secondary(point) == original);
      put(7, numbered(type, 61));
      BOOST_CHECK(!result.matches_base_reads(primary, [] {}));
      BOOST_CHECK(base->read_secondary(point) == original);
   }
}
BOOST_AUTO_TEST_SUITE_END()
