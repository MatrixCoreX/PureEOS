#include <eosio/chain/execution_work_meter.hpp>
#include <eosio/vm/backend.hpp>
#include <functional>
#include <iostream>
#include <mutex>
#include <vector>

// End-to-end COUNTER bridge only. Host=10 and signature=5 below are test
// constants, not proposed chain prices. No production host registry is metered
// by this fixture, and it does not provide transaction/database undo.
namespace {
using bytes=std::vector<uint8_t>;
using meter=eosio::chain::execution_work_meter;
using usage=eosio::chain::execution_work_usage;
struct options : eosio::vm::default_options {
   static constexpr bool enable_execution_work_checkpoints=true;
};
void require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& operation) {
   try {operation();} catch(const E&) {return;}
   throw std::runtime_error("expected host bridge rejection");
}
void section(bytes& out,uint8_t id,const bytes& data) {
   require(data.size()<128,"fixture section exceeds short encoding");
   out.push_back(id); out.push_back(uint8_t(data.size())); out.insert(out.end(),data.begin(),data.end());
}
bytes module(bool host,bool continuation=true) {
   bytes out{0,97,115,109,1,0,0,0}; section(out,1,{1,0x60,0,0});
   if(host) section(out,2,{1,3,'e','n','v',6,'c','h','a','r','g','e',0,0});
   section(out,3,{1,0}); section(out,7,{1,3,'r','u','n',0,uint8_t(host?1:0)});
   if(host && continuation) section(out,10,{1,6,0,0x10,0,1,1,0x0b}); // entry + call + two nops = four WASM work
   else if(host) section(out,10,{1,4,0,0x10,0,0x0b}); // host call followed immediately by return
   else section(out,10,{1,5,0,1,1,1,0x0b}); // child entry + three nops = four WASM work
   return out;
}
struct host {
   meter* work;
   unsigned calls=0;
   bool catch_exhaustion=false;
   std::function<void()> after_caught;
   std::function<void()> child;
   void charge() {
      try {work->consume({.host=10,.serial=4});}
      catch(const eosio::chain::execution_work_exhausted&) {
         if(catch_exhaustion) {if(after_caught) after_caught(); return;}
         throw;
      }
      ++calls; // only after the debit succeeds; real transaction undo is external
      if(child) child();
   }
};
using functions=eosio::vm::registered_host_functions<host>;
void register_host() {
   static std::once_flag flag;
   std::call_once(flag,[] {functions::add<&host::charge>("env","charge");});
}
template<typename Impl> struct fixture {
   host native;
   eosio::vm::wasm_allocator allocator;
   eosio::vm::backend<functions,Impl,options> vm;
   fixture(meter& work,bool continuation=true) : native{&work}, vm((register_host(),module(true,continuation)),native,&allocator) {}
   void run() {
      auto view=native.work->wasm_budget();
      auto binding=vm.get_context().bind_execution_work_budget({view.remaining,view.exhausted});
      vm.call(native,"env","run");
   }
};
template<typename Impl> void run_child(meter& work) {
   eosio::vm::wasm_allocator allocator;
   eosio::vm::backend<std::nullptr_t,Impl,options> vm(module(false),&allocator);
   auto view=work.wasm_budget(); auto binding=vm.get_context().bind_execution_work_budget({view.remaining,view.exhausted});
   vm.call("env","run");
}
template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}
struct frame_exit {};
struct detailed_budget_error : eosio::vm::execution_work_exhausted {};
template<typename Impl> void caught_exhaustion(unsigned exit_kind=0) {
   meter work(10); fixture<Impl> f(work,false); f.native.catch_exhaustion=true;
   if(exit_kind==1) f.native.after_caught=[&] {f.vm.exit({});};
   if(exit_kind==2) f.native.after_caught=[] {throw frame_exit{};};
   try {rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});}
   catch(const frame_exit&) {throw std::runtime_error("failed work escaped as ordinary frame termination");}
   require(work.exhausted() && work.usage()==usage{.wasm=2},"caught native failure changed debit accounting");
}
}

int main(int argc,char** argv) {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"live_VM_and_actual_native_callback_share_one_budget",[] {both([]<typename Impl> {
         meter work(14); fixture<Impl> f(work); f.run();
         require(work.usage()==usage{4,10,0,0,4} && work.remaining()==0 && !work.exhausted(),"native/VM debit lost or doubled");
         require(f.native.calls==1,"native callback did not execute exactly once");
         require(work.usage().billable_units(100,5000)==100,"minimum invoice changed raw work");
      });}},
      {"native_debit_prevents_later_VM_block_before_it_executes",[] {both([]<typename Impl> {
         meter work(13); fixture<Impl> f(work);
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});
         require(work.exhausted() && work.usage()==usage{2,10,0,0,4},"VM continued using stale pre-host remaining work");
         require(f.native.calls==1,"native callback was duplicated on exhaustion");
         rejects<eosio::chain::execution_work_exhausted>([&] {f.run();});
      });}},
      {"native_exhaustion_propagates_through_JIT_without_partial_debit",[] {both([]<typename Impl> {
         meter work(10); fixture<Impl> f(work);
         rejects<eosio::chain::execution_work_exhausted>([&] {f.run();});
         require(work.exhausted() && work.usage()==usage{.wasm=2} && f.native.calls==0,"failed host debit changed native counters");
      });}},
      {"nested_VM_called_from_native_charges_same_semantic_execution_once",[] {both([]<typename Impl> {
         meter work(18); fixture<Impl> f(work); f.native.child=[&] {run_child<Impl>(work);}; f.run();
         require(work.usage()==usage{8,10,0,0,4} && work.remaining()==0,"nested VM was lost or double charged");
      });}},
      {"nested_failure_restores_bindings_and_new_execution_is_reusable",[] {both([]<typename Impl> {
         meter work(15); fixture<Impl> f(work); f.native.child=[&] {run_child<Impl>(work);};
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});
         require(work.exhausted() && work.usage()==usage{3,10,0,0,4},"child exhaustion changed the wrong shared budget");
         rejects<std::logic_error>([&] {f.vm.get_context().charge_execution_work(0);});
         meter fresh(18); f.native.work=&fresh; f.native.child=[&] {run_child<Impl>(fresh);};
         f.run(); require(fresh.usage()==usage{8,10,0,0,4},"nested failure poisoned the reused context");
      });}},
      {"signature_price_discount_does_not_discount_VM_capacity",[] {both([]<typename Impl> {
         meter work(19); work.consume({.signature=5}); fixture<Impl> f(work); f.run();
         require(work.usage()==usage{4,10,5,0,4} && work.remaining()==0,"signature work was lost");
         require(work.usage().billable_units(0,5000)==16,"test signature discount rounding is inconsistent");
         require(work.usage().total()==19,"discount leaked into raw execution capacity");
      });}},
      {"discarded_real_VM_attempt_does_not_change_canonical_counter",[] {both([]<typename Impl> {
         meter canonical(100);
         {meter trial(13); fixture<Impl> f(trial); rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});}
         require(canonical.usage().total()==0,"discarded VM attempt was billed");
         meter accepted(14); fixture<Impl> f(accepted); f.run(); canonical.consume(accepted.usage());
         require(canonical.usage()==usage{4,10,0,0,4},"successful trial work import differs");
      });}},
      {"caught_exhaustion_cannot_escape_interpreter_completion",[] {caught_exhaustion<eosio::vm::interpreter>();}},
      {"caught_exhaustion_cannot_escape_interpreter_exit",[] {caught_exhaustion<eosio::vm::interpreter>(1);}},
      {"caught_exhaustion_cannot_escape_interpreter_custom_frame_exit",[] {caught_exhaustion<eosio::vm::interpreter>(2);}},
#if defined(__x86_64__)
      {"caught_exhaustion_cannot_escape_JIT_completion",[] {caught_exhaustion<eosio::vm::jit>();}},
      {"caught_exhaustion_cannot_escape_JIT_exit",[] {caught_exhaustion<eosio::vm::jit>(1);}},
      {"caught_exhaustion_cannot_escape_JIT_custom_frame_exit",[] {caught_exhaustion<eosio::vm::jit>(2);}},
#endif
      {"healthy_native_exit_and_same_context_reuse_keep_their_semantics",[] {both([]<typename Impl> {
         meter work(12); fixture<Impl> f(work); f.native.child=[&] {f.vm.exit({});}; f.run();
         require(work.usage()==usage{2,10,0,0,4} && !work.exhausted(),"valid exit charged unreachable continuation");
         meter fresh(14); f.native.work=&fresh; f.native.child={}; f.run();
         require(fresh.usage()==usage{4,10,0,0,4} && !fresh.exhausted(),"normal exit poisoned a reused context");
      });}},
      {"healthy_custom_frame_exit_preserves_tag_and_context_reuse",[] {both([]<typename Impl> {
         meter work(14); fixture<Impl> f(work); f.native.child=[] {throw frame_exit{};}; bool caught=false;
         try {f.run();} catch(const frame_exit&) {caught=true;}
         require(caught && work.usage()==usage{2,10,0,0,4} && work.remaining()==2 && !work.exhausted(),
                 "healthy custom termination was converted or overbilled");
         meter fresh(14); f.native.work=&fresh; f.native.child={}; f.run();
         require(fresh.usage()==usage{4,10,0,0,4},"custom termination poisoned the reused context");
      });}},
      {"existing_resource_error_keeps_its_dynamic_type",[] {both([]<typename Impl> {
         meter work(10); fixture<Impl> f(work,false); f.native.catch_exhaustion=true;
         f.native.after_caught=[] {throw detailed_budget_error{};};
         rejects<detailed_budget_error>([&] {f.run();});
         require(work.exhausted() && work.usage()==usage{.wasm=2},"resource exception propagation changed consumed work");
      });}}
   };
   if(argc>2) return 2;
   const std::string_view filter=argc==2?argv[1]:"";
   size_t failed=0,executed=0;
   for(const auto& [name,run]:cases) {
      if(!filter.empty() && filter!=name) continue;
      ++executed;
      try {run(); std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed; std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed || !executed?"failed":"passed")<<"\",\"cases\":"<<executed
            <<",\"passed\":"<<executed-failed<<",\"failed\":"<<failed<<"}\n";
   return !executed?2:(failed?1:0);
}
