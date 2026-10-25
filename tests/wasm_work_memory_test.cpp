#include <eosio/chain/execution_work_meter.hpp>
#include <eosio/vm/backend.hpp>
#include <eosio/vm/profile.hpp>
#include <algorithm>
#include <functional>
#include <iostream>

namespace {
using bytes=std::vector<uint8_t>;
using namespace eosio::chain;
using eosio::vm::execution_work_memory_prices;
constexpr execution_work_memory_prices prices{11,2}; // TEST units, no production rates
struct metered_options {static constexpr bool enable_execution_work_checkpoints=true;};
void require(bool v,const char* text) {if(!v) throw std::runtime_error(text);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected memory work rejection");
}
void uleb(bytes& out,uint64_t v) {do {auto b=uint8_t(v&127);v>>=7;out.push_back(b|(v?128:0));}while(v);}
void section(bytes& out,uint8_t id,const bytes& data) {out.push_back(id);uleb(out,data.size());out.insert(out.end(),data.begin(),data.end());}
bytes body(bytes instructions,unsigned locals=0) {
   bytes b;if(locals) {b={1};uleb(b,locals);b.push_back(0x7f);}else b={0};
   b.insert(b.end(),instructions.begin(),instructions.end());b.push_back(0x0b);return b;
}
bytes module(uint32_t initial=1,uint32_t maximum=8,bytes instructions={0x41,7},
             bytes data={},bool globals=false,unsigned locals=0,bool start=false) {
   bytes out{0,97,115,109,1,0,0,0};
   section(out,1,start?bytes{2,0x60,0,1,0x7f,0x60,0,0}:bytes{1,0x60,0,1,0x7f});
   section(out,3,start?bytes{2,0,1}:bytes{1,0});
   bytes memory{1,1};uleb(memory,initial);uleb(memory,maximum);section(out,5,memory);
   if(globals) section(out,6,{1,0x7f,0,0x41,9,0x0b});
   section(out,7,{1,3,'r','u','n',0,0});
   if(start) section(out,8,{1});
   bytes code{uint8_t(start?2:1)};auto first=body(instructions,locals);uleb(code,first.size());code.insert(code.end(),first.begin(),first.end());
   if(start) {auto b=body({0x41,0,0x41,42,0x36,2,0});uleb(code,b.size());code.insert(code.end(),b.begin(),b.end());}
   section(out,10,code);
   if(!data.empty()) {bytes d{1,0,0x41,0,0x0b};uleb(d,data.size());d.insert(d.end(),data.begin(),data.end());section(out,11,d);}
   return out;
}
template<typename Impl,typename Options=metered_options> struct fixture {
   using backend=eosio::vm::backend<std::nullptr_t,Impl,Options>;
   using context=typename Impl::template context<std::nullptr_t>;
   bytes source;
   eosio::vm::wasm_code_ptr pointer;
   backend compiled,active;
   context ctx;
   eosio::vm::wasm_allocator allocator;
   explicit fixture(bytes raw=module(),bool single=true)
     :source(std::move(raw)),pointer(source.data(),source.size()),
      compiled(pointer,source.size(),nullptr,{},single,false),ctx(compiled.get_module(),64) {
      active.share(compiled);active.set_context(&ctx);active.set_wasm_allocator(&allocator);
      active.reset_max_pages();active.reset_max_call_depth();
   }
   auto bind(execution_work_meter& meter,execution_work_memory_prices p=prices) {
      return ctx.bind_execution_work_budget(meter.wasm_budget(p));
   }
   void initialize() {active.initialize();}
   int32_t run() {return active.call_with_return("env","run")->to_i32();}
   int32_t pages() const {return allocator.get_current_page();}
   char* memory() {return allocator.template get_base_ptr<char>();}
};
template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}
bool zeros(const char* p,size_t n) {return std::all_of(p,p+n,[](char c){return c==0;});}
}
int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"memory_initialization_requires_binding_and_explicit_nonzero_prices",[] {both([]<typename Impl> {
         fixture<Impl> f(module(8,8));
         rejects<std::logic_error>([&] {f.initialize();});require(f.pages()==0,"unbound initialization allocated memory");
         execution_work_meter meter(1000);auto work=f.bind(meter,{});
         rejects<std::logic_error>([&] {f.initialize();});
         require(meter.exhausted() && meter.usage().total()==0 && f.pages()==0,"unpriced initialization allocated or charged partially");
      });}},
      {"initial_page_work_is_reserved_atomically_before_allocation",[] {both([]<typename Impl> {
         fixture<Impl> f(module(8,8));execution_work_meter small(87);
         {auto work=f.bind(small);rejects<execution_work_exhausted>([&] {f.initialize();});}
         require(f.pages()==0 && small.exhausted() && small.usage().total()==0,"insufficient initial budget changed allocator");
         execution_work_meter exact(88);auto work=f.bind(exact);f.initialize();
         require(f.pages()==8 && exact.remaining()==0 && !exact.exhausted() && exact.usage().wasm==88,"exact initial page budget was not accepted");
      });}},
      {"data_copy_and_globals_are_charged_before_any_initialization_effect",[] {both([]<typename Impl> {
         fixture<Impl> f(module(1,8,{0x41,7},{1,2,3,4,5},true));execution_work_meter small(22);
         {auto work=f.bind(small);rejects<execution_work_exhausted>([&] {f.initialize();});}
         require(f.pages()==0 && small.usage().total()==0,"initial data/globals caused partial allocation");
         execution_work_meter exact(23);auto work=f.bind(exact);f.initialize();
         require(exact.usage().wasm==23 && std::equal(f.memory(),f.memory()+5,bytes{1,2,3,4,5}.begin()),"initial data/global work or bytes changed");
      });}},
      {"memory_growth_charges_requested_successful_pages_and_keeps_result",[] {both([]<typename Impl> {
         fixture<Impl> f(module(1,8,{0x41,2,0x40,0}));execution_work_meter meter(36);auto work=f.bind(meter);
         f.initialize();require(f.run()==1 && f.pages()==3,"successful growth changed old-page result");
         require(meter.usage().wasm==36 && meter.remaining()==0 && !meter.exhausted(),"growth page/instruction work incorrect");
      });}},
      {"growth_exhaustion_is_bridged_before_memory_side_effect_and_context_recovers",[] {both([]<typename Impl> {
         fixture<Impl> f(module(1,8,{0x41,2,0x40,0}));execution_work_meter small(35);
         {auto work=f.bind(small);f.initialize();rejects<execution_work_exhausted>([&] {f.run();});}
         require(f.pages()==1 && small.exhausted() && small.usage().wasm==14,"growth failed after allocating or charged partial pages");
         execution_work_meter retry(36);auto work=f.bind(retry);f.initialize();
         require(f.run()==1 && f.pages()==3 && retry.remaining()==0,"failed growth poisoned context/binding");
      });}},
      {"failed_growth_keeps_minus_one_without_charging_unallocated_pages",[] {both([]<typename Impl> {
         fixture<Impl> f(module(1,1,{0x41,1,0x40,0}));execution_work_meter meter(14);auto work=f.bind(meter);
         f.initialize();require(f.run()==-1 && f.pages()==1 && meter.usage().wasm==14,"failed grow changed return or charged nonexistent pages");
      });}},
      {"existing_negative_growth_extension_and_impossible_shrink_keep_semantics",[] {both([]<typename Impl> {
         fixture<Impl> f(module(2,4,{0x41,0x7f,0x40,0}));execution_work_meter meter(36);auto work=f.bind(meter);
         f.initialize();require(f.run()==2 && f.pages()==1 && meter.usage().wasm==36,"upstream negative growth extension changed");
         fixture<Impl> empty(module(0,4,{0x41,0x7f,0x40,0}));execution_work_meter tiny(3);auto zero=empty.bind(tiny,{});
         empty.initialize();require(empty.run()==-1 && empty.pages()==0 && tiny.remaining()==0,"impossible shrink charged page cost or changed result");
      });}},
      {"start_memory_and_instruction_costs_share_the_prebound_budget",[] {both([]<typename Impl> {
         fixture<Impl> f(module(1,8,{0x41,7},{},false,0,true));execution_work_meter small(14);
         {auto work=f.bind(small);rejects<execution_work_exhausted>([&] {f.initialize();});}
         require(small.exhausted() && f.pages()==1 && f.memory()[0]==0,"start ran a store after its block budget failed");
         execution_work_meter exact(15);auto work=f.bind(exact);f.initialize();
         require(f.memory()[0]==42 && exact.remaining()==0,"start/init work not combined or start store changed");
      });}},
      {"reused_allocator_cost_depends_on_new_module_and_old_pages_cannot_leak",[] {both([]<typename Impl> {
         fixture<Impl> f(module(1,8,{0x41,2,0x40,0}));execution_work_meter first(36);
         {auto work=f.bind(first);f.initialize();f.run();std::memset(f.memory(),0x5a,3*65536);}
         execution_work_meter second(36);auto work=f.bind(second);f.initialize();
         require(f.pages()==1 && zeros(f.memory(),65536),"retained page was not reset");
         require(f.run()==1 && zeros(f.memory(),3*65536) && first.usage()==second.usage(),"old hidden bytes leaked on regrow or reused pages changed fee");
      });}},
      {"metered_allocator_handles_no_memory_zero_pages_and_regrow_without_leaks",[] {
         eosio::vm::wasm_allocator a;a.reset_for_metered_execution(4);
         std::memset(a.get_base_ptr<char>(),0x6b,4*65536);a.reset_for_metered_execution(2);
         require(zeros(a.get_base_ptr<char>(),2*65536),"retained allocator bytes leaked");
         a.alloc<char>(2);require(zeros(a.get_base_ptr<char>(),4*65536),"removed bytes leaked on alloc");
         std::memset(a.get_base_ptr<char>(),0x4b,4*65536);a.reset_for_metered_execution();require(a.get_current_page()==-1,"no-memory sentinel changed");
         a.reset_for_metered_execution(0);require(a.get_current_page()==0,"zero-page reset changed sentinel");
         a.alloc<char>(4);require(zeros(a.get_base_ptr<char>(),4*65536),"no-memory reuse leaked bytes");
      }},
      {"memory_price_products_overflow_before_allocating_or_debiting",[] {both([]<typename Impl> {
         fixture<Impl> f(module(2,2));execution_work_meter meter(UINT64_MAX);auto work=f.bind(meter,{UINT64_MAX,1});
         rejects<execution_work_exhausted>([&] {f.initialize();});
         require(meter.exhausted() && meter.usage().total()==0 && f.pages()==0,"page-price overflow changed memory/balance");
         fixture<Impl> bytes_f(module(1,1,{0x41,7},{1,2}));execution_work_meter byte_meter(UINT64_MAX);auto bw=bytes_f.bind(byte_meter,{1,UINT64_MAX});
         rejects<execution_work_exhausted>([&] {bytes_f.initialize();});
         require(byte_meter.exhausted() && byte_meter.usage().total()==0 && bytes_f.pages()==0,"data-price overflow caused partial debit/effects");
      });}},
      {"two_pass_interpreter_keeps_metered_initialization_metadata",[] {
         fixture<eosio::vm::interpreter> f(module(1,1,{0x41,7},{3,4}),false);execution_work_meter meter(16);auto work=f.bind(meter);
         f.initialize();require(meter.remaining()==0 && f.memory()[0]==3,"two-pass parse lost memory/data work metadata");
      }},
#if defined(__x86_64__)
      {"JIT_growth_resource_bridge_handles_both_stack_parities_and_backtraces",[] {
         auto check=[]<typename Impl>(unsigned locals) {
            fixture<Impl> f(module(1,8,{0x41,2,0x40,0},{},false,locals));execution_work_meter small(35+(locals?locals+1:0));auto work=f.bind(small);
            f.initialize();rejects<execution_work_exhausted>([&] {f.run();});
            require(f.pages()==1 && small.exhausted(),"unaligned growth bridge escaped or allocated memory");
         };
         for(unsigned n:{0u,1u,16u,17u}) {check.template operator()<eosio::vm::jit>(n);check.template operator()<eosio::vm::jit_profile>(n);}
      }},
#endif
      {"ordinary_unmetered_VM_preserves_initialization_growth_and_results",[] {both([]<typename Impl> {
         fixture<Impl,eosio::vm::default_options> f(module(1,8,{0x41,2,0x40,0}));
         f.initialize();require(f.run()==1 && f.pages()==3,"unmetered growth changed");
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
