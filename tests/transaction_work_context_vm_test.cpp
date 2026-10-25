#include <eosio/chain/transaction_work_context.hpp>
#include <eosio/vm/backend.hpp>
#include <functional>
#include <iostream>
#include <mutex>

namespace {
using namespace eosio::chain;
using bytes=std::vector<uint8_t>;
struct options {static constexpr bool enable_execution_work_checkpoints=true;};
void require(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected VM/context work rejection");
}
void uleb(bytes& out,uint64_t n) {do {auto b=uint8_t(n&127);n>>=7;out.push_back(b|(n?128:0));}while(n);}
void section(bytes& out,uint8_t id,const bytes& value) {out.push_back(id);uleb(out,value.size());out.insert(out.end(),value.begin(),value.end());}
bytes module(unsigned nops=0,bool memory=false,bool native=false) {
   bytes out{0,97,115,109,1,0,0,0};section(out,1,{1,0x60,0,0});
   if(native) section(out,2,{1,3,'e','n','v',4,'p','i','n','g',0,0});
   section(out,3,{1,0});if(memory) section(out,5,{1,1,1,1});
   section(out,7,{1,3,'r','u','n',0,uint8_t(native?1:0)});
   bytes body{0};if(native) body.insert(body.end(),{0x10,0});body.insert(body.end(),nops,1);body.push_back(0x0b);
   bytes code{1};uleb(code,body.size());code.insert(code.end(),body.begin(),body.end());section(out,10,code);return out;
}
struct host {std::function<void()> child;unsigned calls=0;void ping() {++calls;if(child) child();}};
using registry=eosio::vm::registered_host_functions<host>;
void registration() {static std::once_flag flag;std::call_once(flag,[] {registry::add<&host::ping>("env","ping");});}
template<typename Impl,typename Options=options> struct fixture {
   using backend=eosio::vm::backend<registry,Impl,Options>;
   using context=typename Impl::template context<registry>;
   bytes source;
   eosio::vm::wasm_code_ptr pointer;
   backend compiled,active;
   context ctx;
   eosio::vm::wasm_allocator allocator;
   host native;
   explicit fixture(bytes raw=module()):source((registration(),std::move(raw))),pointer(source.data(),source.size()),
      compiled(pointer,source.size(),nullptr,{},true,false),ctx(compiled.get_module(),64) {
      active.share(compiled);active.set_context(&ctx);active.set_wasm_allocator(&allocator);active.reset_max_pages();active.reset_max_call_depth();
   }
   void run() {active.initialize(native);active.call(native,"env","run");}
};
template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}
transaction_work_policy policy() {return {7,100,{11,2}};}
transaction_work_local_limits local() {return {200,4,1};}
}
int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"real_VM_initialization_and_execution_keep_seed_once",[] {both([]<typename Impl> {
         transaction_work_context c(policy(),local(),{3,2});auto a=c.begin_final_attempt();fixture<Impl> vm(module(0,true));
         {auto binding=a.bind_vm(vm.ctx);vm.run();}
         require(a.finish().usage==execution_work_usage{.wasm=12,.signature=3,.native=2,.serial=2},"real VM/init work or seed changed");
      });}},
      {"real_trial_limit_falls_back_to_same_canonical_program_without_double_invoice",[] {both([]<typename Impl> {
         transaction_work_context c(policy(),local(),{3,2});fixture<Impl> vm(module(10));auto ticket=c.reserve_trial(5);auto trial=ticket.start();
         {auto binding=trial.bind_vm(vm.ctx);rejects<execution_work_exhausted>([&] {vm.run();});}
         trial.discard();require(c.inspect().local_spent==5 && !c.inspect().terminal,"private VM exhaustion poisoned semantic context or refunded failed trial");
         auto final=c.begin_final_attempt();{auto binding=final.bind_vm(vm.ctx);vm.run();}
         require(final.finish().usage==execution_work_usage{.wasm=11,.signature=3,.native=2,.serial=2},"canonical fallback retained trial bill or context was not reusable");
      });}},
      {"real_successful_trial_is_imported_once_without_VM_reexecution",[] {both([]<typename Impl> {
         transaction_work_context c(policy(),local(),{3,2});fixture<Impl> vm(module(10));auto ticket=c.reserve_trial(20);auto trial=ticket.start();
         {auto binding=trial.bind_vm(vm.ctx);vm.run();}auto receipt=trial.seal_success();
         auto final=c.begin_final_attempt();final.adopt(std::move(receipt));final.consume({.native=4,.serial=4});
         require(final.finish().usage==execution_work_usage{.wasm=11,.signature=3,.native=6,.serial=6} && c.inspect().local_spent==11,"adoption double charged VM or signature");
      });}},
      {"real_native_to_child_VM_sync_uses_one_semantic_counter",[] {both([]<typename Impl> {
         transaction_work_context c(policy(),local(),{3,2});auto a=c.begin_final_attempt();fixture<Impl> parent(module(0,false,true)),child(module(3));
         parent.native.child=[&] {a.consume({.host=7});auto child_binding=a.bind_vm(child.ctx);child.run();};
         {auto root_binding=a.bind_vm(parent.ctx);parent.run();}
         require(parent.native.calls==1 && a.finish().usage==execution_work_usage{.wasm=6,.host=7,.signature=3,.native=2,.serial=2},
            "nested real VM/native work used a different meter or charged signature twice");
      });}},
      {"real_VM_completion_cannot_swallow_premature_semantic_finish",[] {both([]<typename Impl> {
         transaction_work_context c(policy(),local());auto a=c.begin_final_attempt();fixture<Impl> vm(module(0,false,true));
         vm.native.child=[&] {rejects<std::logic_error>([&] {a.finish();});};
         {auto binding=a.bind_vm(vm.ctx);rejects<execution_work_exhausted>([&] {vm.run();});}
         rejects<execution_work_exhausted>([&] {a.finish();});require(c.inspect().terminal,"native caught a premature finish and escaped resource failure");
      });}},
      {"real_uninstrumented_VM_is_rejected_at_the_context_binding_boundary",[] {both([]<typename Impl> {
         transaction_work_context c(policy(),local());auto a=c.begin_final_attempt();fixture<Impl,eosio::vm::default_options> vm;
         rejects<std::logic_error>([&] {a.bind_vm(vm.ctx);});rejects<execution_work_exhausted>([&] {a.finish();});
         require(c.inspect().terminal,"unmetered real module produced an empty valid semantic receipt");
      });}}
   };
   unsigned failed=0;
   for(const auto& [name,test]:cases) {
      try {test();std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed;std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed?"failed":"passed")<<"\",\"cases\":"<<cases.size()<<",\"passed\":"<<cases.size()-failed<<",\"failed\":"<<failed<<"}\n";
   return failed?1:0;
}
