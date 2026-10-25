#include <eosio/chain/parallel_work_footprint.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>

namespace allocation_control {inline long fail_after=-1;inline size_t count=0;inline bool counting=false;}
void* operator new(size_t size) {
   if(allocation_control::fail_after==0) throw std::bad_alloc();
   if(allocation_control::fail_after>0) --allocation_control::fail_after;
   if(allocation_control::counting) ++allocation_control::count;
   if(auto p=std::malloc(size?size:1)) return p;
   throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
void* operator new[](size_t size) {return ::operator new(size);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,size_t) noexcept {::operator delete(p);}

namespace {
using namespace eosio::chain;
using u128=unsigned __int128;
// Owned structural models of the real const accessors in
// parallel_state_overlay.hpp/transaction_state_view.hpp. No Chainbase database
// or canonical authorization is fabricated by this standalone component test.
struct name {uint64_t value=0;uint64_t to_uint64_t() const {return value;}auto operator<=>(const name&) const=default;};
struct table_key {name code,scope,table;auto operator<=>(const table_key&) const=default;};
struct table_read {table_key key;int64_t object_id=0;name payer;uint32_t count=0;bool operator==(const table_read&) const=default;};
enum class primary_query_kind:uint8_t {find,lower_bound,upper_bound,last,previous};
struct primary_query {table_key table;primary_query_kind kind=primary_query_kind::find;uint64_t primary_key=0;};
struct primary_metadata {int64_t object_id=0,table_object_id=0;uint64_t primary_key=0;name payer;size_t value_size=0;bool operator==(const primary_metadata&) const=default;};
struct primary_result {std::optional<table_read> table;std::optional<primary_metadata> row;};
struct f64 {uint64_t bits=0;bool operator==(const f64&) const=default;};
struct f128 {u128 bits=0;bool operator==(const f128&) const=default;};
using secondary_key=std::variant<uint64_t,u128,std::array<u128,2>,f64,f128>;
enum class secondary_query_kind:uint8_t {find_primary,find_secondary,lower_bound,upper_bound,first,last,next,previous};
struct secondary_query {table_key table;secondary_query_kind kind=secondary_query_kind::find_primary;secondary_key key=uint64_t{0};uint64_t primary_key=0;};
struct secondary_row {int64_t object_id=0,table_object_id=0;uint64_t primary_key=0;name payer;secondary_key key=uint64_t{0};bool operator==(const secondary_row&) const=default;};
struct secondary_result {std::optional<table_read> table;std::optional<secondary_row> row;};
enum class mutation_operation:uint8_t {update,store,remove};
struct primary_mutation {table_key table;uint64_t primary_key=0;name payer;std::vector<char> value;mutation_operation operation=mutation_operation::update;int64_t object_id=0,table_object_id=0;};
struct secondary_mutation {table_key table;uint64_t primary_key=0;name payer;secondary_key key=uint64_t{0};mutation_operation operation=mutation_operation::update;int64_t object_id=0,table_object_id=0;};
using mutation=std::variant<primary_mutation,secondary_mutation>;
struct frozen {
   std::map<table_key,table_read> tables;
   std::map<std::pair<table_key,uint64_t>,primary_metadata> rows;
   std::map<std::tuple<table_key,size_t,uint64_t>,secondary_row> secondary;
   mutable size_t reads=0;
   bool unavailable=false;
   std::optional<table_read> read_table(const table_key& key) const {
      ++reads;if(unavailable) throw std::runtime_error("missing frozen capture");auto p=tables.find(key);return p==tables.end()?std::nullopt:std::optional{p->second};
   }
   primary_result locate_primary(const primary_query& q) const {
      auto t=read_table(q.table);auto r=rows.find({q.table,q.primary_key});return {t,r==rows.end()?std::nullopt:std::optional{r->second}};
   }
   secondary_result read_secondary(const secondary_query& q) const {
      auto t=read_table(q.table);auto r=secondary.find({q.table,q.key.index(),q.primary_key});return {t,r==secondary.end()?std::nullopt:std::optional{r->second}};
   }
};
struct overlay {
   std::shared_ptr<frozen> snapshot=std::make_shared<frozen>();
   std::vector<table_key> tables;
   std::vector<primary_query> queries;
   std::vector<secondary_query> secondary_queries;
   std::vector<name> accounts;
   std::vector<mutation> mutations;
   const auto& base() const {return snapshot;}
   const auto& read_tables() const {return tables;}
   const auto& read_queries() const {return queries;}
   const auto& secondary_read_queries() const {return secondary_queries;}
   const auto& read_accounts() const {return accounts;}
   const auto& writes() const {return mutations;}
};
constexpr canonical_work_coverage full{canonical_work_coverage::all}; // synthetic trusted-owner fixture ONLY
void require(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected footprint exception");
}
bool intersects(const auto& a,const auto& b) {for(const auto& key:a) if(std::find(b.begin(),b.end(),key)!=b.end()) return true;return false;}
bool conflicts(const canonical_work_footprint& a,const canonical_work_footprint& b) {
   return !a.complete() || !b.complete() || intersects(a.writes(),b.reads()) || intersects(a.writes(),b.writes()) || intersects(a.reads(),b.writes());
}
execution_work_key ram(uint64_t payer) {return {uint8_t(owner_work_access_kind::ram),{payer,0,0,0,0}};}
table_key table(uint64_t scope,uint64_t code=700,uint64_t value=900) {return {{code},{scope},{value}};}
overlay update(table_key key,uint64_t primary=1,uint64_t old_payer=800,uint64_t new_payer=800,size_t old_size=8,size_t new_size=8) {
   overlay o;o.tables={key};o.queries={{key,primary_query_kind::find,primary}};
   o.snapshot->tables.emplace(key,table_read{key,10,{old_payer},1});
   o.snapshot->rows.emplace(std::pair{key,primary},primary_metadata{20,10,primary,{old_payer},old_size});
   o.mutations.emplace_back(primary_mutation{key,primary,{new_payer},std::vector<char>(new_size),mutation_operation::update,20,10});return o;
}
overlay read(table_key key,primary_query_kind kind=primary_query_kind::find,uint64_t primary=1) {
   overlay o;o.tables={key};o.queries={{key,kind,primary}};return o;
}
canonical_work_footprint build(const overlay& o,std::span<const owner_work_observation> owner={}) {
   return make_overlay_work_footprint(o,full,owner);
}
parallel_work_limits budget_limits() {
   parallel_work_limits l;l.total=l.parallel_path=l.serial=l.span=l.transaction=l.scheduled=100000;l.lanes=4;return l;
}
uint64_t random(uint64_t& seed) {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
}

int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"private_overlay_never_self_certifies_missing_owner_coverage",[] {
         auto o=update(table(1));o.snapshot->unavailable=true;
         for(uint16_t mask=0;mask<canonical_work_coverage::all;++mask) {
            auto f=make_overlay_work_footprint(o,{mask});require(!f.complete() && f.reads().empty() && f.writes().empty(),"partial coverage exposed private keys as complete");
         }
         require(!o.snapshot->reads,"unknown coverage read an unauthenticated frozen base");
         auto f=make_overlay_work_footprint(o,{511});require(!f.complete(),"unknown future coverage bits were silently accepted");
         parallel_work_budget budget(budget_limits());require(budget.try_add({.wasm=100},f.view()).serial==100,"unknown footprint failed to reserve whole-order serial barrier");
      }},
      {"negative_point_and_every_primary_range_kind_conflict_with_insert",[] {
         const auto key=table(1);
         overlay writer=read(key);writer.mutations.emplace_back(primary_mutation{key,1,{800},{'x'},mutation_operation::store,-2,-1});
         auto w=build(writer);require(w.complete(),"valid new-table store not represented");
         for(auto kind:{primary_query_kind::find,primary_query_kind::lower_bound,primary_query_kind::upper_bound,primary_query_kind::last,primary_query_kind::previous}) {
            auto r=build(read(key,kind,UINT64_MAX));require(r.complete() && conflicts(r,w),"point/range/negative lookup missed phantom insert");
         }
      }},
      {"all_secondary_families_and_full256bit_values_use_safe_shared_table_guard",[] {
         const auto key=table(1);auto primary=build(update(key));
         const std::vector<secondary_key> values{uint64_t{1},u128{1},std::array<u128,2>{u128{1}<<127,~u128{0}},f64{0x8000000000000000},f128{u128{1}<<127}};
         for(const auto& value:values) for(auto kind:{secondary_query_kind::find_primary,secondary_query_kind::find_secondary,secondary_query_kind::lower_bound,secondary_query_kind::upper_bound,secondary_query_kind::first,secondary_query_kind::last,secondary_query_kind::next,secondary_query_kind::previous}) {
            overlay r;r.tables={key};r.secondary_queries={{key,kind,value,UINT64_MAX}};auto f=build(r);
            require(f.complete() && conflicts(f,primary),"secondary query missed primary/shared-table lifecycle dependency");
         }
         overlay r;r.tables={key};r.secondary_queries={{key,secondary_query_kind::find_secondary,std::array<u128,2>{0,0},1}};
         auto zero=build(r);r.secondary_queries[0].key=std::array<u128,2>{~u128{0},~u128{0}};auto all=build(r);
         require(zero==all,"secondary values were partially hashed/truncated instead of deliberately whole-table guarded");
      }},
      {"independent_fixed_size_transfers_do_not_write_common_creation_payer_RAM",[] {
         // A->B and C->D: all tables were created by 800, but each balance row
         // retains payer800 and size8. Real db_update_i64 has no RAM delta.
         auto a=update(table(1)),b=update(table(2)),c=update(table(3)),d=update(table(4));
         a.tables.push_back(b.tables[0]);a.queries.push_back(b.queries[0]);a.mutations.push_back(b.mutations[0]);
         a.snapshot->tables.insert(b.snapshot->tables.begin(),b.snapshot->tables.end());a.snapshot->rows.insert(b.snapshot->rows.begin(),b.snapshot->rows.end());
         c.tables.push_back(d.tables[0]);c.queries.push_back(d.queries[0]);c.mutations.push_back(d.mutations[0]);
         c.snapshot->tables.insert(d.snapshot->tables.begin(),d.snapshot->tables.end());c.snapshot->rows.insert(d.snapshot->rows.begin(),d.snapshot->rows.end());
         const std::array<owner_work_observation,3> oa{{{owner_work_access_kind::permissions,101},{owner_work_access_kind::cpu,101,work_access_mode::write},{owner_work_access_kind::code,700}}};
         const std::array<owner_work_observation,3> oc{{{owner_work_access_kind::permissions,103},{owner_work_access_kind::cpu,103,work_access_mode::write},{owner_work_access_kind::code,700}}};
         auto first=build(a,oa),second=build(c,oc);
         require(first.complete() && second.complete() && !conflicts(first,second),"independent transfers falsely share RAM from creation payer");
         require(std::find(first.writes().begin(),first.writes().end(),ram(800))==first.writes().end(),"same-size update invented RAM write");
         parallel_work_budget budget(budget_limits());budget.try_add({.wasm=100},first.view());auto assessment=budget.try_add({.wasm=100},second.view());
         require(assessment.accepted() && assessment.parallel_path==100,"bridge failed to provide actual budget independence");
      }},
      {"payer_or_size_changes_have_real_shared_resource_dependencies",[] {
         const std::array<owner_work_observation,1> ram_read{{{owner_work_access_kind::ram,800}}};overlay no_db;
         auto owner=build(no_db,ram_read);
         for(auto o:{update(table(1),1,800,801),update(table(2),1,800,800,8,9),update(table(3),1,800,800,9,8)}) {
            auto f=build(o);require(f.complete() && conflicts(f,owner),"actual payer/size RAM delta was omitted");
         }
         auto payer=build(update(table(5),1,800,801));require(std::binary_search(payer.writes().begin(),payer.writes().end(),ram(801)),"new payer RAM charge omitted");
      }},
      {"payer_A_to_B_to_A_history_keeps_intermediate_resource_effects",[] {
         auto o=update(table(1),1,800,801);o.mutations.push_back(primary_mutation{table(1),1,{800},std::vector<char>(8),mutation_operation::update,20,10});auto f=build(o);
         require(f.complete() && std::binary_search(f.writes().begin(),f.writes().end(),ram(800)) && std::binary_search(f.writes().begin(),f.writes().end(),ram(801)),"journal net-state coalescing lost intermediate payer charge");
      }},
      {"table_lifecycle_payer_refund_and_recreation_are_not_lost",[] {
         auto o=update(table(1));o.snapshot->tables.at(table(1)).payer={900};o.mutations.clear();
         o.mutations.push_back(primary_mutation{table(1),1,{800},{},mutation_operation::remove,20,10});
         o.mutations.push_back(primary_mutation{table(1),1,{801},{'x'},mutation_operation::store,-2,-1});
         o.mutations.push_back(primary_mutation{table(1),1,{802},{'x','y'},mutation_operation::update,-2,-1});auto f=build(o);
         require(f.complete(),"valid remove/recreate/update journal rejected");
         for(auto payer:{800u,801u,802u,900u}) require(std::binary_search(f.writes().begin(),f.writes().end(),ram(payer)),"row or retired table payer RAM dependency lost");
      }},
      {"secondary_fixed_size_update_only_changes_RAM_when_payer_changes",[] {
         overlay o;const auto key=table(1);o.tables={key};o.snapshot->tables.emplace(key,table_read{key,10,{900},1});
         secondary_key value=std::array<u128,2>{~u128{0},u128{1}<<100};o.secondary_queries={{key,secondary_query_kind::find_primary,value,7}};
         o.snapshot->secondary.emplace(std::tuple{key,size_t{2},uint64_t{7}},secondary_row{20,10,7,{800},value});
         o.mutations.push_back(secondary_mutation{key,7,{800},std::array<u128,2>{0,1},mutation_operation::update,20,10});auto same=build(o);
         require(same.complete() && !std::binary_search(same.writes().begin(),same.writes().end(),ram(800)),"fixed secondary size invented a RAM charge");
         std::get<secondary_mutation>(o.mutations[0]).payer={801};auto changed=build(o);
         require(changed.complete() && std::binary_search(changed.writes().begin(),changed.writes().end(),ram(800)) && std::binary_search(changed.writes().begin(),changed.writes().end(),ram(801)),"secondary payer change omitted real RAM effects");
      }},
      {"missing_or_inconsistent_mutation_history_yields_empty_key_barrier",[] {
         auto original=update(table(1));
         const std::vector<std::function<void(overlay&)>> change{
            [](auto& o){o.queries.clear();},[](auto& o){o.tables.clear();},
            [](auto& o){std::get<primary_mutation>(o.mutations[0]).object_id=999;},
            [](auto& o){std::get<primary_mutation>(o.mutations[0]).table_object_id=999;},
            [](auto& o){std::get<primary_mutation>(o.mutations[0]).payer={0};},
            [](auto& o){o.queries[0].kind=primary_query_kind(255);},
            [](auto& o){std::get<primary_mutation>(o.mutations[0]).operation=mutation_operation(255);}
         };
         for(const auto& edit:change) {auto o=original;edit(o);auto f=build(o);require(!f.complete() && f.reads().empty() && f.writes().empty(),"incomplete journal leaked apparently complete partial keys");}
         original.snapshot->unavailable=true;rejects<std::runtime_error>([&] {build(original);});
      }},
      {"owner_account_permission_code_resource_and_global_guards_are_distinct",[] {
         overlay o;
         for(auto kind:{owner_work_access_kind::code,owner_work_access_kind::permissions,owner_work_access_kind::ram,owner_work_access_kind::cpu,owner_work_access_kind::net,owner_work_access_kind::account_sequences}) {
            const std::array<owner_work_observation,1> r{{{kind,100}}},w{{{kind,100,work_access_mode::write}}},other{{{kind,101,work_access_mode::write}}},life{{{owner_work_access_kind::account,100,work_access_mode::write}}};
            require(conflicts(build(o,r),build(o,w)) && !conflicts(build(o,r),build(o,other)) && conflicts(build(o,r),build(o,life)),"logical owner domain/account-lifecycle alias failed");
         }
         const std::array<owner_work_observation,1> invalid{{{owner_work_access_kind(255),100}}};auto f=build(o,invalid);require(!f.complete() && f.reads().empty(),"unknown owner domain accepted");
         const std::array<owner_work_observation,1> global_read{{{owner_work_access_kind::configuration,0}}},global_write{{{owner_work_access_kind::configuration,0,work_access_mode::write}}};
         require(conflicts(build(o,global_read),build(o,global_write)),"global configuration write did not order its readers");
      }},
      {"limits_validate_before_walking_or_publishing_partial_keys",[] {
         auto o=update(table(1));auto f=make_overlay_work_footprint(o,full,{},work_footprint_limits{2,3});require(f.complete(),"exact key/observation bound rejected");
         auto limited=make_overlay_work_footprint(o,full,{},work_footprint_limits{1,3});require(!limited.complete() && limited.reads().empty() && limited.writes().empty(),"key cap leaked partial keys");
         o.snapshot->reads=0;limited=make_overlay_work_footprint(o,full,{},work_footprint_limits{4096,2});require(!limited.complete() && !o.snapshot->reads,"observation cap read source before checking lengths");
         rejects<std::invalid_argument>([&] {make_overlay_work_footprint(o,full,{},work_footprint_limits{0,3});});
         rejects<std::invalid_argument>([&] {make_overlay_work_footprint(o,full,{},work_footprint_limits{4096,32769});});
      }},
      {"independent_resource_and_table_oracle_has_no_false_independence",[] {
         uint64_t seed=0x117abb33;size_t independent=0,dependent=0;
         for(unsigned i=0;i<1000;++i) {
            const auto s1=1+random(seed)%8,s2=1+random(seed)%8;const bool change1=random(seed)%2,change2=random(seed)%2;
            const auto payer1=800+random(seed)%8,payer2=800+random(seed)%8;
            auto a=update(table(s1),1,payer1,payer1,8,change1?9:8),b=update(table(s2),2,payer2,payer2,8,change2?9:8);
            // Independent oracle: different logical tables are disjoint;
            // RAM introduces a dependency only when both really change it.
            const bool true_dependency=s1==s2 || (change1 && change2 && payer1==payer2);
            auto fa=build(a),fb=build(b);require(fa.complete() && fb.complete(),"valid generated observation became an unknown barrier");
            if(true_dependency) {++dependent;require(conflicts(fa,fb),"oracle found a missed logical table/RAM dependency");}
            else {++independent;require(!conflicts(fa,fb),"different tables with no RAM dependency were falsely serialized");}
         }
         require(independent>500 && dependent>50,"oracle corpus lacked independent/dependent cases");
      }},
      {"allocation_failure_cannot_replace_a_previously_published_footprint",[] {
         auto o=update(table(1),1,800,801);const auto input_tables=o.snapshot->tables;const auto input_rows=o.snapshot->rows;
         allocation_control::count=0;allocation_control::counting=true;auto expected=build(o);allocation_control::counting=false;const auto points=allocation_control::count;
         require(points>=8,"fault test did not cover multiple staging allocations");
         for(size_t n=0;n<points;++n) {
            auto published=expected;allocation_control::fail_after=long(n);bool rejected=false;
            try {published=build(o);}catch(const std::bad_alloc&) {rejected=true;}catch(...) {allocation_control::fail_after=-1;throw;}
            allocation_control::fail_after=-1;require(rejected && published==expected,"allocation failure published partial dependency keys");
            require(o.snapshot->tables==input_tables && o.snapshot->rows==input_rows,"footprint extraction mutated canonical-state model");
         }
      }}
   };
   size_t failed=0;
   for(const auto& [name,test]:cases) {
      try {test();std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed;std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed?"failed":"passed")<<"\",\"cases\":"<<cases.size()<<",\"passed\":"<<cases.size()-failed<<",\"failed\":"<<failed<<"}\n";
   return failed?1:0;
}
