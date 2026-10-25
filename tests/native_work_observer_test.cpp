#include <eosio/vm/backend.hpp>
#include <eosio/vm/observed_host_functions.hpp>
#include <eosio/chain/native_work_pricing.hpp>
#include <bit>
#include <barrier>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

// Actual VM -> original registered-host-function bridge. These are ABI/call
// observations, NOT calibrated native prices, access proofs or active fees.
namespace {
using bytes=std::vector<uint8_t>;
using argument=eosio::vm::native_work_argument;
void require(bool c,const char* msg) {if(!c) throw std::runtime_error(msg);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();} catch(const E&) {return;}
   throw std::runtime_error("expected observer rejection");
}
void uleb(bytes& out,size_t value) {
   do {auto b=uint8_t(value&127); value>>=7; out.push_back(b|(value?128:0));} while(value);
}
void section(bytes& out,uint8_t id,const bytes& data) {
   out.push_back(id); uleb(out,data.size()); out.insert(out.end(),data.begin(),data.end());
}
bytes module(unsigned calls=1) {
   bytes out{0,97,115,109,1,0,0,0};
   section(out,1,{2,0x60,4,0x7f,0x7e,0x7d,0x7c,1,0x7e,0x60,0,0});
   section(out,2,{1,3,'e','n','v',5,'p','r','o','b','e',0,0});
   section(out,3,{1,1}); section(out,7,{1,3,'r','u','n',0,1});
   bytes body{0};
   for(unsigned i=0;i<calls;++i) {
      const bytes call{0x41,0x7f,0x42,0x7f,0x43,0x34,0x12,0xc0,0x7f,
         0x44,0,0,0,0,0,0,0,0x80,0x10,0,0x1a};
      body.insert(body.end(),call.begin(),call.end());
   }
   body.push_back(0x0b); bytes code{1}; uleb(code,body.size()); code.insert(code.end(),body.begin(),body.end());
   section(out,10,code); return out;
}
struct host {
   unsigned calls=0;
   bool fail=false;
   std::function<void()> child;
   uint64_t probe(int32_t a,uint64_t b,float c,double d) {
      require(a==-1 && b==UINT64_MAX && std::bit_cast<uint32_t>(c)==0x7fc01234 &&
         std::bit_cast<uint64_t>(d)==0x8000000000000000,"original host received different ABI values");
      ++calls; if(fail) throw std::runtime_error("original native failure"); if(child) child(); return 42;
   }
};
using raw_registry=eosio::vm::registered_host_functions<host>;
using registry=eosio::vm::observed_host_functions<raw_registry>;
void registration() {
   static std::once_flag flag; std::call_once(flag,[] {raw_registry::add<&host::probe>("env","probe");});
}
struct record {
   std::string module,function;
   std::vector<argument> args;
   size_t parameters,returns;
   bool complete;
   bool operator==(const record&) const = default;
};
struct recorder {
   std::vector<record> events;
   bool reject=false;
   static void collect(void* opaque,const eosio::vm::native_work_call& e) {
      auto& self=*static_cast<recorder*>(opaque);
      self.events.push_back({std::string(e.module),std::string(e.function),{e.arguments.begin(),e.arguments.end()},
         e.signature_params.size(),e.signature_returns.size(),e.arguments_complete});
      if(self.reject) throw eosio::vm::execution_work_exhausted();
   }
};
template<typename Impl,typename Options=eosio::vm::default_options> struct fixture {
   host native;
   eosio::vm::wasm_allocator allocator;
   eosio::vm::backend<registry,Impl,Options> vm;
   fixture(unsigned calls=1) : vm((registration(),module(calls)),&allocator) {}
   void run(recorder& r) {auto observe=registry::observe(&r,&recorder::collect); vm.call(native,"env","run");}
};
template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}
std::vector<argument> expected_arguments() {
   return {{eosio::vm::i32,UINT32_MAX},{eosio::vm::i64,UINT64_MAX},
      {eosio::vm::f32,0x7fc01234},{eosio::vm::f64,0x8000000000000000}};
}
template<typename Impl> void shared_code_observation() {
   registration();
   using backend=eosio::vm::backend<registry,Impl>;
   using context=typename Impl::template context<registry>;
   backend a,b;
   {
      auto raw=module(7); eosio::vm::wasm_code_ptr ptr(raw.data(),raw.size());
      backend compiled(ptr,raw.size(),nullptr,{},true,false);
      a.share(compiled); b.share(compiled);
   }
   context ca(a.get_module(),64),cb(b.get_module(),64);
   eosio::vm::wasm_allocator aa,ab;
   a.set_context(&ca); b.set_context(&cb); a.set_wasm_allocator(&aa); b.set_wasm_allocator(&ab);
   a.reset_max_pages(); b.reset_max_pages(); a.initialize(); b.initialize();
   std::barrier start(2); std::exception_ptr errors[2];
   auto run=[&](backend& vm,unsigned index) {
      start.arrive_and_wait();
      try {
         host native; recorder r; auto observation=registry::observe(&r,&recorder::collect);
         for(unsigned i=0;i<100;++i) {
            const bool reject=index && i%2; r.reject=reject; const auto before=r.events.size();
            if(reject) rejects<eosio::vm::execution_work_exhausted>([&] {vm.call(native,"env","run");});
            else vm.call(native,"env","run");
            require(r.events.size()-before==(reject?1:7),"shared code used another thread's observer/rejection policy");
         }
         require(native.calls==(index?350:700),"shared code native dispatch count changed");
         for(const auto& event:r.events) require(event.args==expected_arguments(),"shared code corrupted ABI observation");
      } catch(...) {errors[index]=std::current_exception();}
   };
   std::thread first(run,std::ref(a),0),second(run,std::ref(b),1);
   first.join(); second.join(); for(auto error:errors) if(error) std::rethrow_exception(error);
}
// Adapter boundary tests only: this registry deliberately does not convert or
// execute a 64/65-argument native ABI. Real ABI delegation is tested above.
struct boundary_registry {
   struct native {unsigned calls=0;};
   using host_type_t=native;
   using execution_interface_t=eosio::vm::execution_interface;
   struct mappings {
      std::unordered_map<eosio::vm::host_func_pair,uint32_t,eosio::vm::host_func_pair_hash> named_mapping;
      std::vector<int> functions;
      std::vector<eosio::vm::host_function> host_functions;
      mappings() : functions(3),host_functions(3) {
         named_mapping={{ {"env","bounded"},0 },{{"env","opaque"},1},{{"env","small"},2}};
         host_functions[0].params.assign(64,eosio::vm::i32);
         host_functions[1].params.assign(65,eosio::vm::i32);
         host_functions[2].params={eosio::vm::i64};
      }
      static mappings& get() {static mappings m; return m;}
   };
   void operator()(native* host,execution_interface_t,uint32_t) {++host->calls;}
};
using boundary_observer=eosio::vm::observed_host_functions<boundary_registry>;
struct priced_options {static constexpr bool enable_execution_work_checkpoints=true;};
eosio::chain::native_work_rule probe_price() {
   using namespace eosio::vm;
   // Test-only rates. The first i32 is UINT32_MAX; price its raw ABI value in
   // one chunk to exercise extraction without dereferencing native pointers.
   return {"env","probe",{i32,i64,f32,f64},{i64},3,1,{{0,UINT32_MAX,2,1,UINT32_MAX}}};
}
}

int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"original_native_ABI_is_preserved_for_integer_and_float_bits",[] {both([]<typename Impl> {
         fixture<Impl> f; recorder r; f.run(r);
         require(f.native.calls==1 && r.events.size()==1,"wrong actual native call count");
         const auto& e=r.events.front();
         require(e.module=="env" && e.function=="probe" && e.complete && e.parameters==4 && e.returns==1 &&
            e.args==expected_arguments(),"observer changed argument order/types/bits");
      });}},
      {"repeated_identical_calls_are_not_deduplicated",[] {both([]<typename Impl> {
         fixture<Impl> f(7); recorder r; f.run(r);
         require(f.native.calls==7 && r.events.size()==7,"native billing observation reused unique-query count");
         for(const auto& e:r.events) require(e==r.events.front(),"repeated ABI arguments changed");
      });}},
      {"unbound_or_rejecting_observer_never_delegates_and_context_recovers",[] {both([]<typename Impl> {
         fixture<Impl> f;
         rejects<std::logic_error>([&] {f.vm.call(f.native,"env","run");});
         require(f.native.calls==0,"unbound observer delegated native function");
         recorder r; r.reject=true;
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run(r);});
         require(f.native.calls==0 && r.events.size()==1,"observer rejection occurred after native effects");
         r.reject=false; f.run(r); require(f.native.calls==1 && r.events.size()==2,"failed observer poisoned context/binding");
      });}},
      {"original_native_exception_keeps_type_and_attempt_is_recorded",[] {both([]<typename Impl> {
         fixture<Impl> f; recorder r; f.native.fail=true;
         try {f.run(r); throw std::logic_error("missing native exception");}
         catch(const std::runtime_error& e) {require(std::string_view(e.what())=="original native failure","native exception changed");}
         require(f.native.calls==1 && r.events.size()==1,"throwing native attempt was lost or duplicated");
         f.native.fail=false; f.run(r); require(f.native.calls==2 && r.events.size()==2,"native exception poisoned subsequent execution");
      });}},
      {"nested_observation_binding_restores_the_parent",[] {both([]<typename Impl> {
         fixture<Impl> parent(2),child; recorder outer,inner;
         parent.native.child=[&] {child.run(inner);}; parent.run(outer);
         require(outer.events.size()==2 && inner.events.size()==2 && parent.native.calls==2 && child.native.calls==2,
            "nested native observations used the wrong recorder");
         rejects<std::logic_error>([&] {parent.vm.call(parent.native,"env","run");});
      });}},
      {"invalid_binding_cannot_replace_an_existing_observer",[] {both([]<typename Impl> {
         fixture<Impl> f; recorder r;
         auto binding=registry::observe(&r,&recorder::collect);
         rejects<std::invalid_argument>([] {auto invalid=registry::observe(nullptr,nullptr);});
         f.vm.call(f.native,"env","run"); require(r.events.size()==1,"invalid nested binding replaced active recorder");
      });}},
      {"shared_compiled_code_keeps_thread_observers_and_failures_isolated",[] {both([]<typename Impl> {
         shared_code_observation<Impl>();
      });}},
      {"capture_limit_is_exact_and_oversized_signature_is_explicitly_opaque",[] {
         boundary_registry::native host; recorder r; auto binding=boundary_observer::observe(&r,&recorder::collect);
         eosio::vm::operand_stack stack;
         for(unsigned i=0;i<64;++i) stack.push(eosio::vm::i32_const_t{i});
         boundary_observer{}(&host,{nullptr,&stack},0);
         require(r.events.size()==1 && r.events[0].complete && r.events[0].args.size()==64,"exact capture limit lost arguments");
         for(unsigned i=0;i<64;++i) require(r.events[0].args[i].bits==i,"bounded capture reversed arguments");
         boundary_observer{}(&host,{nullptr,nullptr},1);
         require(host.calls==2 && r.events.size()==2 && !r.events[1].complete && r.events[1].parameters==65 &&
            r.events[1].args.empty(),"oversized native signature was silently partly captured");
      }},
      {"bad_stack_type_underflow_and_index_reject_before_observation_or_dispatch",[] {
         boundary_registry::native host; recorder r; auto binding=boundary_observer::observe(&r,&recorder::collect);
         eosio::vm::operand_stack stack;
         rejects<eosio::vm::wasm_interpreter_exception>([&] {boundary_observer{}(&host,{nullptr,&stack},2);});
         rejects<eosio::vm::wasm_interpreter_exception>([&] {boundary_observer{}(&host,{nullptr,nullptr},2);});
         stack.push(eosio::vm::i32_const_t{1});
         rejects<eosio::vm::wasm_interpreter_exception>([&] {boundary_observer{}(&host,{nullptr,&stack},2);});
         rejects<std::logic_error>([&] {boundary_observer{}(&host,{nullptr,&stack},UINT32_MAX);});
         require(host.calls==0 && r.events.empty(),"invalid ABI reached observer/native dispatch");
      }},
      {"registry_growth_is_rejected_before_delegation",[] {
         boundary_registry::native host; recorder r; auto binding=boundary_observer::observe(&r,&recorder::collect);
         auto& functions=boundary_registry::mappings::get().functions;
         struct restore {std::vector<int>& v; ~restore() {v.pop_back();}};
         functions.push_back(0); restore cleanup{functions};
         rejects<std::logic_error>([&] {boundary_observer{}(&host,{nullptr,nullptr},1);});
         require(host.calls==0 && r.events.empty(),"registry mutation reached native dispatch");
      }},
      {"actual_VM_and_priced_original_native_calls_share_one_budget",[] {both([]<typename Impl> {
         using namespace eosio::chain;
         fixture<Impl,priced_options> f(2);execution_work_meter meter(1000);
         const native_work_schedule schedule(1,{probe_price()});native_work_charge charge{meter,schedule};
         auto work=f.vm.get_context().bind_execution_work_budget(meter.wasm_budget());
         auto observation=registry::observe(&charge,&native_work_charge::before_call);
         f.vm.call(f.native,"env","run");
         require(f.native.calls==2 && meter.usage()==execution_work_usage{.wasm=13,.host=10,.serial=4},
            "original native call price was lost, duplicated or charged to a separate counter");
      });}},
      {"priced_native_exhaustion_prevents_original_native_effects",[] {both([]<typename Impl> {
         using namespace eosio::chain;
         fixture<Impl,priced_options> f;execution_work_meter meter(10);
         const native_work_schedule schedule(1,{probe_price()});native_work_charge charge{meter,schedule};
         auto work=f.vm.get_context().bind_execution_work_budget(meter.wasm_budget());
         auto observation=registry::observe(&charge,&native_work_charge::before_call);
         rejects<execution_work_exhausted>([&] {f.vm.call(f.native,"env","run");});
         require(!f.native.calls && meter.exhausted() && meter.usage().host==0,
            "native call executed or partially charged after price exceeded shared budget");
      });}},
      {"unpriced_native_attempt_cannot_escape_completion_or_delegate",[] {both([]<typename Impl> {
         using namespace eosio::chain;
         fixture<Impl,priced_options> f;execution_work_meter meter(1000);
         auto r=probe_price();r.function="different";const native_work_schedule schedule(1,{r});native_work_charge charge{meter,schedule};
         auto work=f.vm.get_context().bind_execution_work_budget(meter.wasm_budget());
         auto observation=registry::observe(&charge,&native_work_charge::before_call);
         rejects<execution_work_exhausted>([&] {f.vm.call(f.native,"env","run");});
         require(!f.native.calls && meter.exhausted() && meter.usage().host==0,"unpriced native call bypassed budget invalidation");
      });}}
   };
   size_t failed=0;
   for(const auto& [name,run]:cases) {
      try {run(); std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed; std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed?"failed":"passed")<<"\",\"cases\":"<<cases.size()
            <<",\"passed\":"<<cases.size()-failed<<",\"failed\":"<<failed<<"}\n";
   return failed?1:0;
}
