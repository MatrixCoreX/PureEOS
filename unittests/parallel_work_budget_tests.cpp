#include <eosio/chain/parallel_work_budget.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <vector>

// Deterministic allocation-failure injection for the map staging guarantee.
// Only this standalone executable replaces new/delete; no node target does.
static thread_local int fail_after=-1;
void* operator new(std::size_t bytes) {
   if(fail_after==0) {fail_after=-1; throw std::bad_alloc();}
   if(fail_after>0) --fail_after;
   if(void* p=std::malloc(bytes?bytes:1)) return p;
   throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

using namespace eosio::chain;
namespace {
using key=execution_work_key;
using usage=execution_work_usage;
void require(bool c,const char* message) {if(!c) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();} catch(const E&) {return;}
   throw std::runtime_error("expected work budget rejection");
}
key k(uint64_t n) {return {1,{n,0,0,0,0}};}
parallel_work_limits generous() {return {1000000,1000000,1000000,1000000,1000000,1000000,4};}
struct tx {
   usage work;
   std::vector<key> reads,writes;
   bool complete=true;
   canonical_work_access access() const {return {reads,writes,complete};}
};
bool intersects(const std::vector<key>& a,const std::vector<key>& b) {
   for(const auto& x:a) for(const auto& y:b) if(x==y) return true;
   return false;
}
std::vector<uint64_t> quadratic_oracle(const std::vector<tx>& transactions) {
   std::vector<uint64_t> end,committed;
   for(size_t i=0;i<transactions.size();++i) {
      const auto& a=transactions[i]; uint64_t predecessor=0;
      for(size_t j=0;j<i;++j) {
         const auto& b=transactions[j];
         if(!a.complete || !b.complete || intersects(b.writes,a.reads) ||
            intersects(b.writes,a.writes) || intersects(b.reads,a.writes)) predecessor=std::max(predecessor,committed[j]);
      }
      end.push_back(predecessor+(a.complete?a.work.total()-a.work.serial:0));
      committed.push_back(std::max(committed.empty()?0:committed.back(),end.back()));
   }
   return end;
}
uint64_t random(uint64_t& s) {s^=s<<13; s^=s>>7; s^=s<<17; return s;}

// Explicit two-node-per-transaction scheduler: execute E_i then zero-duration
// commit C_i; C_(i-1) -> C_i, and conflict C_j -> E_i. No frontier data structure
// or closed-form bound is used by this oracle. Serial cost is added outside.
uint64_t greedy_oracle(const std::vector<tx>& transactions,unsigned lanes,bool reverse) {
   const auto n=transactions.size()*2;
   std::vector<std::vector<size_t>> parents(n);
   std::vector<uint64_t> duration(n);
   for(size_t i=0;i<transactions.size();++i) {
      const auto& a=transactions[i]; duration[2*i]=a.complete?a.work.total()-a.work.serial:0;
      parents[2*i+1].push_back(2*i); if(i) parents[2*i+1].push_back(2*i-1);
      for(size_t j=0;j<i;++j) {
         const auto& b=transactions[j];
         if(!a.complete || !b.complete || intersects(b.writes,a.reads) ||
            intersects(b.writes,a.writes) || intersects(b.reads,a.writes)) parents[2*i].push_back(2*j+1);
      }
   }
   std::vector<bool> done(n),started(n);
   std::vector<size_t> running(lanes,n);
   std::vector<uint64_t> finish(lanes);
   uint64_t time=0; size_t completed=0;
   const auto ready=[&](size_t i) {return std::all_of(parents[i].begin(),parents[i].end(),[&](size_t j) {return done[j];});};
   while(completed<n) {
      for(size_t p=0;p<lanes;++p) if(running[p]!=n && finish[p]<=time) {
         done[running[p]]=true; ++completed; running[p]=n;
      }
      bool changed=true;
      while(changed) {
         changed=false;
         for(size_t i=0;i<n;++i) if(!started[i] && !duration[i] && ready(i)) {
            started[i]=done[i]=true; ++completed; changed=true;
         }
      }
      for(size_t order=0;order<n;++order) {
         const auto i=reverse?n-1-order:order;
         if(started[i] || !ready(i)) continue;
         const auto free=std::find(running.begin(),running.end(),n); if(free==running.end()) break;
         const auto p=free-running.begin(); *free=i; started[i]=true; finish[p]=time+duration[i];
      }
      if(completed==n) break;
      uint64_t next=UINT64_MAX;
      for(size_t p=0;p<lanes;++p) if(running[p]!=n) next=std::min(next,finish[p]);
      require(next>time && next<UINT64_MAX,"explicit schedule stalled"); time=next;
   }
   return time;
}
}

int main(int argc,char** argv) {
   const auto max=UINT64_MAX;
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"independent_transactions_share_path_but_not_total_or_invoice",[] {
         parallel_work_budget b(generous());
         for(unsigned i=0;i<4;++i) {const std::array writes{k(i)}; require(b.try_add({.wasm=10},{{},writes,true}).accepted(),"independent work rejected");}
         require(b.state().total==40 && b.state().parallel_path==10 && b.state().span==10 && b.state().scheduled==18,"wrong finite-lane independent work bound");
         require(usage{.wasm=10}.billable_units(100,5000)==100,"floor leaked into raw budget");
      }},
      {"RAW_WAR_WAW_and_cross_key_bridges_match_explicit_DAG",[] {
         const std::vector<tx> transactions{
            {{.wasm=5},{},{k(1)}},{{.wasm=7},{k(1)},{}},{{.wasm=11},{k(1)},{}},
            {{.wasm=13},{},{k(1)}},{{.wasm=3},{k(1)},{k(2)}},{{.wasm=4},{k(2)},{k(3)}},{{.wasm=2},{},{k(3)}}};
         const auto expected=std::vector<uint64_t>{5,12,16,29,32,36,38};
         require(quadratic_oracle(transactions)==expected,"independent DAG fixture is wrong");
         parallel_work_budget b(generous());
         for(size_t i=0;i<transactions.size();++i)
            require(b.try_add(transactions[i].work,transactions[i].access()).completion==expected[i],"lost read/write dependency");
         require(b.state().keys==3 && b.state().parallel_path==38,"wrong frontier accounting");
      }},
      {"canonical_commit_prefix_delays_later_dependent_execution",[] {
         const std::vector<tx> transactions{
            {{.wasm=100},{},{k(1)}},{{.wasm=1},{},{k(2)}},{{.wasm=100},{k(2)},{}}};
         const auto expected=std::vector<uint64_t>{100,1,200};
         require(quadratic_oracle(transactions)==expected && greedy_oracle(transactions,4,false)==200,"commit-prefix oracle is wrong");
         auto limits=generous(); limits.parallel_path=150; parallel_work_budget b(limits);
         for(size_t i=0;i<2;++i) require(b.try_add(transactions[i].work,transactions[i].access()).accepted(),"valid prefix rejected");
         const auto before=b.state(); const auto result=b.try_add(transactions[2].work,transactions[2].access());
         require(result.violations==parallel_work_assessment::path_limit && result.parallel_path==200,"used VM finish before canonical commit availability");
         require(b.state()==before,"rejected commit-dependent work modified state");
      }},
      {"finite_lanes_need_more_than_total_average_or_critical_path",[] {
         const auto t=tx{{.wasm=4},{},{}};
         const std::vector<tx> five(5,t); require(greedy_oracle(five,4,false)==8,"finite-lane fixture is wrong");
         auto limits=generous(); limits.scheduled=7; parallel_work_budget b(limits);
         for(unsigned i=0;i<4;++i) require(b.try_add(t.work,t.access()).accepted(),"four-lane prefix rejected early");
         const auto before=b.state(),result=b.try_add(t.work,t.access());
         require(result.violations==parallel_work_assessment::scheduled_limit && result.total==20 &&
            result.parallel_path==4 && result.scheduled==8,"finite-lane tail disappeared");
         require(b.state()==before,"finite-lane rejection changed state");
      }},
      {"global_serial_cost_plus_parallel_path_bounds_span",[] {
         auto limits=generous(); limits.span=34; limits.parallel_path=30; limits.serial=30;
         parallel_work_budget b(limits);
         for(unsigned i=0;i<3;++i) {const std::array writes{k(i)}; require(b.try_add({.wasm=10,.host=10,.serial=5},{{},writes,true}).accepted(),"valid mixed budget rejected");}
         const auto before=b.state(); const std::array writes{k(3)};
         const auto result=b.try_add({.wasm=10,.host=10,.serial=5},{{},writes,true});
         require(result.violations==parallel_work_assessment::span_limit && result.span==35,"separate maxima hid combined serial span");
         require(b.state()==before,"rejected span changed canonical frontier");
         require(b.try_add({.wasm=1},{{},{},true}).accepted() && b.state().span==30,"rejected candidate blocked next transaction");
      }},
      {"unknown_access_is_full_serial_barrier_even_for_new_keys",[] {
         const std::vector<tx> transactions{
            {{.wasm=10},{},{k(1)}},{{.wasm=7},{},{},false},{{.wasm=13},{},{k(99)}}};
         parallel_work_budget b(generous());
         for(const auto& t:transactions) require(b.try_add(t.work,t.access()).accepted(),"unknown barrier rejected");
         require(b.state().parallel_path==23 && b.state().serial==7 && b.state().span==30,"unknown effects did not constrain later new keys");
         const auto before=b.state(); const std::array reads{k(1)};
         rejects<std::invalid_argument>([&] {b.try_add({.wasm=1},{reads,{},false});});
         require(b.state()==before,"malformed unknown footprint changed state");
      }},
      {"each_capacity_limit_rejects_without_consuming_block_state",[] {
         for(unsigned which=0;which<6;++which) {
            auto limits=generous(); const auto expected=which==5?uint32_t(parallel_work_assessment::scheduled_limit):uint32_t(1u<<which);
            if(which==0) limits.total=1,limits.transaction=1;
            if(which==1) limits.parallel_path=1;
            if(which==2) limits.serial=1;
            if(which==3) limits.span=1;
            if(which==4) limits.transaction=1;
            if(which==5) limits.scheduled=1;
            parallel_work_budget b(limits); const auto before=b.state();
            const usage work=which==2?usage{.host=2,.serial=2}:usage{.wasm=2};
            const auto result=b.try_add(work,{{},{},true});
            require((result.violations&expected)!=0 && b.state()==before,"capacity failure partially committed");
         }
      }},
      {"metadata_bounds_duplicates_and_key_union_are_checked",[] {
         auto limits=generous(); limits.max_transactions=2; limits.max_keys=2; limits.max_transaction_keys=2;
         parallel_work_budget b(limits); const std::array both{k(1),k(2)};
         require(b.try_add({.wasm=1},{both,both,true}).accepted() && b.state().keys==2,"read/write overlap counted twice");
         const auto before=b.state(); const std::array extra{k(3)};
         require(b.try_add({.wasm=1},{extra,{},true}).violations&parallel_work_assessment::key_count_limit,"key bound missing");
         require(b.state()==before,"key bound changed map");
         const std::array duplicate{k(1),k(1)}, reversed{k(2),k(1)};
         for(const auto& bad:{duplicate,reversed}) rejects<std::invalid_argument>([&] {b.try_add({.wasm=1},{bad,{},true});});
         const std::array huge{k(1),k(2),k(3)};
         require(b.try_add({.wasm=1},{huge,{},true}).violations&parallel_work_assessment::transaction_key_limit,"transaction metadata bound missing");
         require(b.try_add({.wasm=1},{{},{},true}).accepted(),"second transaction rejected");
         require(b.try_add({.wasm=1},{{},{},true}).violations&parallel_work_assessment::transaction_count_limit,"transaction count bound missing");
      }},
      {"overflow_and_invalid_usage_cannot_mutate_committed_state",[max] {
         parallel_work_limits unset;
         rejects<std::invalid_argument>([&] {parallel_work_budget invalid(unset);});
         parallel_work_budget b({max,max,max,max,max,max,4});
         require(b.try_add({.signature=max},{{},{},true}).accepted(),"exact uint64 boundary rejected");
         const auto before=b.state(); rejects<std::overflow_error>([&] {b.try_add({.wasm=1},{{},{},true});});
         require(b.state()==before,"overflow partially committed");
         rejects<std::invalid_argument>([&] {b.try_add({},{{},{},true});});
         rejects<std::invalid_argument>([&] {b.try_add({.native=1},{{},{},true});});
         require(b.state()==before,"malformed usage changed frontier");
         auto bad=generous(); bad.max_keys=parallel_work_limits::hard_max_keys+1;
         rejects<std::invalid_argument>([&] {parallel_work_budget invalid(bad);});
      }},
      {"finite_bound_rounding_matches_128_bit_oracle_at_uint64_limit",[max] {
         for(unsigned lanes:{1u,4u,8u,12u,16u,65536u}) {
            parallel_work_budget b({max,max,max,max,max,max,lanes});
            b.try_add({.signature=max/2},{{},{},true}); b.try_add({.signature=max/2},{{},{},true});
            const auto expected=uint64_t((static_cast<unsigned __int128>(max-1)+
               static_cast<unsigned __int128>(lanes-1)*(max/2)+lanes-1)/lanes);
            require(b.state().scheduled==expected,"finite-lane multiply/ceil overflowed");
            require(b.state().scheduled<=b.state().total,"scheduled upper bound exceeds serial execution");
         }
         for(unsigned lanes:{0u,65537u}) {
            auto bad=generous(); bad.lanes=lanes;
            rejects<std::invalid_argument>([&] {parallel_work_budget invalid(bad);});
         }
      }},
      {"allocation_failure_leaves_no_hidden_frontier_nodes",[] {
         const std::array original{k(0)}; std::vector<key> new_keys; for(unsigned i=1;i<=8;++i) new_keys.push_back(k(i));
         for(int failure=0;failure<8;++failure) {
            parallel_work_budget b(generous()); b.try_add({.wasm=3},{{},original,true}); const auto before=b.state();
            bool caught=false;
            try {fail_after=failure; b.try_add({.wasm=7},{new_keys,{},true}); fail_after=-1;}
            catch(const std::bad_alloc&) {fail_after=-1; caught=true;}
            catch(...) {fail_after=-1; throw;}
            require(caught && b.state()==before,"allocation failure changed block counters");
            require(b.try_add({.wasm=7},{new_keys,{},true}).accepted() && b.state().keys==9,"failed staging left hidden map nodes");
         }
      }},
      {"hard_metadata_bounds_are_reached_without_unbounded_growth",[] {
         parallel_work_budget count(generous());
         for(size_t i=0;i<parallel_work_limits::hard_max_transactions;++i)
            require(count.try_add({.wasm=1},{{},{},true}).accepted(),"hard transaction boundary rejected early");
         const auto before=count.state();
         require(count.try_add({.wasm=1},{{},{},true}).violations&parallel_work_assessment::transaction_count_limit,"hard transaction bound missing");
         require(count.state()==before,"hard transaction bound changed state");
         parallel_work_budget keys(generous());
         for(size_t begin=0;begin<parallel_work_limits::hard_max_keys;begin+=parallel_work_limits::hard_max_transaction_keys) {
            std::vector<key> writes;
            for(size_t i=0;i<parallel_work_limits::hard_max_transaction_keys;++i) writes.push_back(k(begin+i));
            require(keys.try_add({.wasm=1},{{},writes,true}).accepted(),"hard key boundary rejected early");
         }
         const std::array extra{k(parallel_work_limits::hard_max_keys)},existing{k(0)};
         const auto full=keys.state();
         require(keys.try_add({.wasm=1},{{},extra,true}).violations&parallel_work_assessment::key_count_limit,"hard key bound missing");
         require(keys.state()==full,"hard key bound changed map");
         require(keys.try_add({.wasm=1},{existing,{},true}).accepted() && keys.state().keys==parallel_work_limits::hard_max_keys,
                 "full key map cannot reuse an existing key");
      }},
      {"1000_random_ordered_footprints_match_quadratic_DAG",[] {
         uint64_t random_state=0x1025a1c2026;
         for(unsigned round=0;round<1000;++round) {
            std::vector<tx> transactions;
            for(unsigned i=0;i<20;++i) {
               tx t{{random(random_state)%50+1,random(random_state)%30+1,random(random_state)%20,0,0}};
               t.work.serial=random(random_state)%(t.work.host+1); t.complete=random(random_state)%9!=0;
               if(t.complete) for(unsigned j=0;j<7;++j) {
                  if(random(random_state)%4==0) t.reads.push_back(k(j));
                  if(random(random_state)%4==0) t.writes.push_back(k(j));
               }
               transactions.push_back(std::move(t));
            }
            const auto expected=quadratic_oracle(transactions);
            uint64_t expected_total=0,expected_serial=0;
            for(const auto& t:transactions) {expected_total+=t.work.total(); expected_serial+=t.complete?t.work.serial:t.work.total();}
            for(unsigned grouping:{1u,4u,8u,12u,16u}) {
               parallel_work_budget b(generous());
               std::vector<uint64_t> actual;
               for(size_t begin=0;begin<transactions.size();begin+=grouping)
                  for(size_t i=begin;i<std::min(transactions.size(),begin+grouping);++i) {
                     const auto result=b.try_add(transactions[i].work,transactions[i].access());
                     require(result.accepted(),"small random work rejected"); actual.push_back(result.completion);
                  }
               require(actual==expected,"frontier differs from independent complete DAG");
               const auto path=*std::max_element(expected.begin(),expected.end());
               const auto nonserial=expected_total-expected_serial;
               const auto scheduled=expected_serial+uint64_t((static_cast<unsigned __int128>(nonserial)+3*static_cast<unsigned __int128>(path)+3)/4);
               require(b.state().total==expected_total && b.state().serial==expected_serial && b.state().parallel_path==path &&
                       b.state().span==path+expected_serial && b.state().scheduled==scheduled,"local grouping changed common-policy block budget");
            }
         }
      }},
      {"256_explicit_greedy_schedules_respect_common_lane_bound",[] {
         uint64_t state=0x1025a1c4;
         for(unsigned round=0;round<256;++round) {
            std::vector<tx> transactions;
            for(unsigned i=0;i<16;++i) {
               tx t{{random(state)%15+1,random(state)%9+1,0,0,0}};
               t.work.serial=random(state)%(t.work.host+1); t.complete=random(state)%13!=0;
               if(t.complete) for(unsigned j=0;j<5;++j) {
                  if(random(state)%4==0) t.reads.push_back(k(j));
                  if(random(state)%4==0) t.writes.push_back(k(j));
               }
               transactions.push_back(std::move(t));
            }
            for(unsigned lanes:{1u,4u,8u,12u,16u}) {
               auto limits=generous(); limits.lanes=lanes; parallel_work_budget b(limits);
               for(const auto& t:transactions) require(b.try_add(t.work,t.access()).accepted(),"small explicit schedule rejected");
               for(bool reverse:{false,true})
                  require(greedy_oracle(transactions,lanes,reverse)+b.state().serial<=b.state().scheduled,
                          "computed finite-lane bound underestimates explicit ordered-commit schedule");
               if(lanes==1) require(b.state().scheduled==b.state().total,"one common lane is not exactly serial work");
            }
         }
      }}
   };
   if(argc>2) return 2;
   const std::string_view filter=argc==2?argv[1]:"";
   size_t failed=0,executed=0;
   for(const auto& [name,run]:cases) {
      if(!filter.empty() && filter!=name) continue;
      ++executed;
      try {run(); std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {fail_after=-1; ++failed; std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed || !executed?"failed":"passed")<<"\",\"cases\":"<<executed
            <<",\"passed\":"<<executed-failed<<",\"failed\":"<<failed<<"}\n";
   return !executed?2:(failed?1:0);
}
