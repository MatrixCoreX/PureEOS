#include <eosio/vm/backend.hpp>
#include <functional>
#include <iostream>
#include <mutex>

// Binding/failed-state guards for direct imported exports/start, with the real
// original registry. Native prices are deliberately a separate tested layer.
namespace {
struct options {static constexpr bool enable_execution_work_checkpoints=true;};
struct host {unsigned calls=0;void ping() {++calls;}};
using registry=eosio::vm::registered_host_functions<host>;
void registration() {static std::once_flag once;std::call_once(once,[] {registry::add<&host::ping>("env","ping");});}
void require(bool v,const char* text) {if(!v) throw std::runtime_error(text);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected execution entry rejection");
}
std::vector<uint8_t> module(bool start=false) {
   std::vector<uint8_t> code{0,97,115,109,1,0,0,0,1,4,1,0x60,0,0,
      2,12,1,3,'e','n','v',4,'p','i','n','g',0,0,
      7,7,1,3,'r','u','n',0,0};
   if(start) code.insert(code.end(),{8,1,0});return code;
}
template<typename Impl,typename Options=options> struct fixture {
   using backend=eosio::vm::backend<registry,Impl,Options>;
   using context=typename Impl::template context<registry>;
   host native;
   std::vector<uint8_t> source;
   eosio::vm::wasm_code_ptr pointer;
   backend compiled,active;
   context ctx;
   eosio::vm::wasm_allocator allocator;
   explicit fixture(bool start=false):source((registration(),module(start))),pointer(source.data(),source.size()),
      compiled(pointer,source.size(),nullptr,{},true,false),ctx(compiled.get_module(),64) {
      active.share(compiled);active.set_context(&ctx);active.set_wasm_allocator(&allocator);
      active.reset_max_pages();active.reset_max_call_depth();
   }
   void initialize() {active.initialize(native);}
   void run() {active.call(native,"env","run");}
};
template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}
}
int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"unbound_direct_import_export_rejects_before_original_native_effect",[] {both([]<typename Impl> {
         fixture<Impl> f;f.initialize();rejects<std::logic_error>([&] {f.run();});require(!f.native.calls,"unbound imported export executed native code");
      });}},
      {"failed_bound_budget_rejects_before_direct_native_dispatch",[] {both([]<typename Impl> {
         fixture<Impl> f;f.initialize();uint64_t left=5;bool failed=false;auto work=f.ctx.bind_execution_work_budget({&left,&failed});
         failed=true;rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});
         require(!f.native.calls && left==5,"failed budget allowed direct native effects before completion check");
      });}},
      {"explicit_failed_binding_blocks_native_calls_even_on_unmetered_code",[] {both([]<typename Impl> {
         fixture<Impl,eosio::vm::default_options> f;f.initialize();uint64_t left=5;bool failed=false;
         auto work=f.ctx.bind_execution_work_budget({&left,&failed});failed=true;
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});
         require(!f.native.calls && left==5,"explicit failed binding allowed native effects on unmetered module");
      });}},
      {"imported_start_requires_binding_before_initialize",[] {both([]<typename Impl> {
         fixture<Impl> f(true);rejects<std::logic_error>([&] {f.initialize();});require(!f.native.calls,"unbound imported start executed native code");
      });}},
      {"healthy_import_uses_separate_native_pricing_layer_and_binding_recovers",[] {both([]<typename Impl> {
         fixture<Impl> f;f.initialize();uint64_t left=5;bool failed=false;
         {auto work=f.ctx.bind_execution_work_budget({&left,&failed});f.run();}
         require(f.native.calls==1 && left==5 && !failed,"direct imported call invented WASM-body work");
         rejects<std::logic_error>([&] {f.run();});require(f.native.calls==1,"scope exit retained a stale execution binding");
      });}},
      {"healthy_imported_start_runs_once_under_explicit_binding",[] {both([]<typename Impl> {
         fixture<Impl> f(true);uint64_t left=5;bool failed=false;auto work=f.ctx.bind_execution_work_budget({&left,&failed});
         f.initialize();require(f.native.calls==1 && left==5,"bound imported start changed call count");
      });}},
      {"owned_metered_start_cannot_run_before_caller_can_bind",[] {both([]<typename Impl> {
         registration();host native;eosio::vm::wasm_allocator allocator;auto code=module(true);
         using backend=typename fixture<Impl>::backend;
         rejects<std::logic_error>([&] {backend vm(code,native,&allocator);});
         require(!native.calls,"owning constructor ran imported start before a budget existed");
      });}},
      {"ordinary_unmetered_imported_export_and_start_keep_existing_behavior",[] {both([]<typename Impl> {
         fixture<Impl,eosio::vm::default_options> f(true);f.initialize();f.run();require(f.native.calls==2,"unmetered direct native calls changed");
      });}},
      {"missing_exports_and_invalid_indices_reject_without_native_effects",[] {both([]<typename Impl> {
         fixture<Impl> f;f.initialize();uint64_t left=5;bool failed=false;auto work=f.ctx.bind_execution_work_budget({&left,&failed});
         rejects<eosio::vm::wasm_interpreter_exception>([&] {f.active.call(f.native,"env","missing");});
         for(uint32_t index:{1u,2u,UINT32_MAX-1,UINT32_MAX})
            rejects<eosio::vm::wasm_interpreter_exception>([&] {f.active.call(&f.native,index);});
         require(!f.native.calls && left==5 && !failed,"invalid function changed host/budget");f.run();
         require(f.native.calls==1,"invalid index poisoned valid import execution");
      });}},
      {"ordinary_unmetered_JIT_missing_export_is_an_exception_not_a_crash",[] {both([]<typename Impl> {
         fixture<Impl,eosio::vm::default_options> f;f.initialize();
         rejects<eosio::vm::wasm_interpreter_exception>([&] {f.active.call(f.native,"env","missing");});
         rejects<eosio::vm::wasm_interpreter_exception>([&] {f.active.call(&f.native,UINT32_MAX-1);});
         require(!f.native.calls,"invalid unmetered function dispatched native code");f.run();require(f.native.calls==1,"unmetered context did not recover");
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
