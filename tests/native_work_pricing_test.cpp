#include <eosio/chain/native_work_pricing.hpp>
#include <array>
#include <functional>
#include <iostream>
#include <thread>
#include <type_traits>

namespace {
using namespace eosio::chain;
using namespace eosio::vm;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();} catch(const E&) {return;}
   throw std::runtime_error("expected native work rejection");
}
native_work_rule rule() {
   return {"env","sized",{i32,i32,i32},{i32},17,5,{{2,32,3,1,UINT32_MAX}}};
}
struct event {
   std::array<value_type,3> types{i32,i32,i32};
   std::array<value_type,1> returns{i32};
   std::array<native_work_argument,3> args{{{i32,123},{i32,456},{i32,0}}};
   native_work_call call() const {return {"env","sized",999,types,returns,args,true};}
};
void failed_charge(const native_work_schedule& schedule,const native_work_call& call) {
   execution_work_meter meter(UINT64_MAX); meter.consume({.wasm=5});
   const auto before=meter.usage(); native_work_charge charge{meter,schedule};
   rejects<std::exception>([&] {native_work_charge::before_call(&charge,call);});
   require(meter.exhausted() && meter.usage()==before && meter.remaining()==0,"failed quote partially charged or revived meter");
   rejects<execution_work_exhausted>([&] {meter.wasm_budget();});
}
uint64_t random(uint64_t& seed) {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
}
int main() {
   static_assert(!std::is_copy_assignable_v<native_work_schedule> && !std::is_move_assignable_v<native_work_schedule>);
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"byte_costs_round_up_without_counting_serial_twice",[] {
         native_work_schedule schedule(7,{rule()}); require(schedule.version()==7,"schedule version changed");
         event e;
         for(uint64_t bytes:{0ULL,1ULL,31ULL,32ULL,33ULL,63ULL,64ULL,65ULL,0xffffffffULL}) {
            e.args[2].bits=bytes; const auto cost=schedule.quote(e.call()); const auto chunks=bytes/32+(bytes%32!=0);
            require(cost==execution_work_usage{.host=17+3*chunks,.serial=5+chunks},"native byte cost/serial subset incorrect");
            require(cost.total()==cost.host,"serial subset was added to total charge");
         }
      }},
      {"local_registry_indices_and_schedule_order_do_not_change_prices",[] {
         auto other=rule(); other.function="aaa"; other.base_units=900;
         native_work_schedule a(1,{rule(),other}),b(1,{other,rule()}); event e;
         for(uint32_t index:{0u,1u,UINT32_MAX}) {
            auto c=e.call(); c.registry_index=index;
            require(a.quote(c)==b.quote(c) && a.quote(c).host==17,"process-local registration order affected work price");
         }
      }},
      {"unknown_opaque_or_mismatched_ABI_invalidates_without_partial_charge",[] {
         native_work_schedule schedule(1,{rule()}); event e; auto c=e.call();
         c.function="unknown"; failed_charge(schedule,c);
         c=e.call(); c.module="other"; failed_charge(schedule,c);
         c=e.call(); c.arguments_complete=false; failed_charge(schedule,c);
         c=e.call(); c.arguments=c.arguments.first(2); failed_charge(schedule,c);
         e.types[1]=i64; failed_charge(schedule,e.call()); e.types[1]=i32;
         e.returns[0]=i64; failed_charge(schedule,e.call()); e.returns[0]=i32;
         e.args[0].type=f32; failed_charge(schedule,e.call()); e.args[0].type=i32;
         e.args[2].bits=uint64_t(UINT32_MAX)+1; failed_charge(schedule,e.call());
      }},
      {"malformed_or_unbounded_policy_metadata_is_rejected",[] {
         rejects<std::invalid_argument>([] {native_work_schedule s(0,{rule()});});
         rejects<std::invalid_argument>([] {native_work_schedule s(1,{});});
         rejects<std::invalid_argument>([] {native_work_schedule s(1,{rule(),rule()});});
         const std::vector<std::function<void(native_work_rule&)>> changes{
            [](auto& r){r.module.clear();},[](auto& r){r.function.assign(257,'x');},
            [](auto& r){r.base_units=0;},[](auto& r){r.serial_base_units=18;},
            [](auto& r){r.parameters.assign(65,i32);},[](auto& r){r.returns.assign(2,i32);},
            [](auto& r){r.parameters[0]=value_type(0);},[](auto& r){r.sizes[0].argument=3;},
            [](auto& r){r.parameters[2]=f32;},[](auto& r){r.sizes[0].bytes_per_chunk=0;},
            [](auto& r){r.sizes[0].units_per_chunk=0;},[](auto& r){r.sizes[0].serial_units_per_chunk=4;},
            [](auto& r){r.sizes[0].maximum_argument=uint64_t(UINT32_MAX)+1;},
            [](auto& r){r.sizes.assign(17,r.sizes[0]);}
         };
         for(const auto& change:changes) {auto r=rule(); change(r); rejects<std::invalid_argument>([&] {native_work_schedule s(1,{r});});}
         std::vector<native_work_rule> rules;
         for(unsigned i=0;i<1024;++i) {auto r=rule();r.function=std::to_string(i);rules.push_back(r);}
         native_work_schedule accepted(1,rules);
         auto r=rule();r.function="extra";rules.push_back(r);
         rejects<std::invalid_argument>([&] {native_work_schedule s(1,rules);});
      }},
      {"maximum_size_is_enforced_before_native_delegation",[] {
         auto r=rule();r.sizes[0].maximum_argument=32;native_work_schedule schedule(1,{r});event e;
         e.args[2].bits=32;require(schedule.quote(e.call()).host==20,"inclusive size boundary rejected");
         e.args[2].bits=33;failed_charge(schedule,e.call());
      }},
      {"cost_overflow_is_atomic_and_failed_state_is_sticky",[] {
         auto r=rule();r.base_units=UINT64_MAX;r.serial_base_units=0;r.sizes[0].serial_units_per_chunk=0;
         native_work_schedule addition(1,{r});event e;e.args[2].bits=1;failed_charge(addition,e.call());
         r.base_units=1;r.sizes[0].units_per_chunk=UINT64_MAX;
         native_work_schedule product(1,{r});e.args[2].bits=33;failed_charge(product,e.call());
      }},
      {"uint64_size_rounding_matches_independent_wide_integer_oracle",[] {
         uint64_t seed=0x915de3122;
         const std::array<value_type,1> type{i64};
         for(unsigned i=0;i<1000;++i) {
            const uint64_t n=i?random(seed):UINT64_MAX,chunk=1+random(seed)%100000,unit=1+random(seed)%10000;
            native_work_rule r{"env","wide",{i64},{},1,0,{{0,chunk,unit,0,UINT64_MAX}}};
            native_work_schedule schedule(1,{r});const std::array<native_work_argument,1> args{{{i64,n}}};
            native_work_call c{"env","wide",0,type,{},args,true};
            const auto expected=1+((static_cast<unsigned __int128>(n)+chunk-1)/chunk)*unit;
            if(expected>UINT64_MAX) rejects<std::overflow_error>([&] {schedule.quote(c);});
            else require(schedule.quote(c).host==uint64_t(expected),"uint64 ceil/multiply/add differs from wide oracle");
         }
      }},
      {"every_attempt_uses_the_same_shared_VM_and_host_budget",[] {
         native_work_schedule schedule(1,{rule()});event e;
         execution_work_meter meter(17*7+5);native_work_charge charge{meter,schedule};auto wasm=meter.wasm_budget();
         wasm.charge(5);
         for(unsigned i=0;i<7;++i) native_work_charge::before_call(&charge,e.call());
         require(meter.usage()==execution_work_usage{.wasm=5,.host=119,.serial=35} && !meter.exhausted() && meter.remaining()==0,
            "identical calls deduplicated or VM/native capacity split");
         rejects<execution_work_exhausted>([&] {native_work_charge::before_call(&charge,e.call());});
         require(meter.usage().total()==124,"failed attempt charged a partial cost");
      }},
      {"one_immutable_schedule_is_safe_with_distinct_thread_budgets",[] {
         const native_work_schedule schedule(1,{rule()});std::exception_ptr errors[2];
         auto run=[&](unsigned index) {
            try {
               event e;e.args[2].bits=index*32;const auto cost=17+index*3;
               execution_work_meter meter(cost*1000);native_work_charge charge{meter,schedule};
               for(unsigned i=0;i<1000;++i) native_work_charge::before_call(&charge,e.call());
               require(!meter.exhausted() && meter.remaining()==0 && meter.usage().host==cost*1000,"thread budget or policy changed");
            }catch(...) {errors[index]=std::current_exception();}
         };
         std::thread a(run,0),b(run,1);a.join();b.join();for(auto e:errors) if(e) std::rethrow_exception(e);
      }}
   };
   unsigned failed=0;
   for(const auto& [name,test]:cases) {
      try {test();std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed;std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed?"failed":"passed")<<"\",\"cases\":"<<cases.size()<<",\"passed\":"<<cases.size()-failed<<",\"failed\":"<<failed<<"}\n";
   return failed?1:0;
}
