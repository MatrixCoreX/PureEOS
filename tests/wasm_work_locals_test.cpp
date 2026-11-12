#include <eosio/vm/backend.hpp>
#include <eosio/vm/profile.hpp>
#include <functional>
#include <iostream>

namespace {
using bytes=std::vector<uint8_t>;
using decls=std::vector<std::pair<uint32_t,uint8_t>>;
struct options {static constexpr bool enable_execution_work_checkpoints=true;};
void require(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected local-initialization work rejection");
}
void uleb(bytes& out,uint64_t v) {do {auto b=uint8_t(v&127);v>>=7;out.push_back(b|(v?128:0));}while(v);}
void section(bytes& out,uint8_t id,const bytes& data) {out.push_back(id);uleb(out,data.size());out.insert(out.end(),data.begin(),data.end());}
struct function {decls locals;bytes instructions;uint8_t result=0;};
bytes module(const std::vector<function>& functions,bool table=false) {
   bytes out{0,97,115,109,1,0,0,0},types,indices,code;
   uleb(types,functions.size());uleb(indices,functions.size());uleb(code,functions.size());
   for(unsigned i=0;i<functions.size();++i) {
      const auto& f=functions[i];types.insert(types.end(),{0x60,0,uint8_t(f.result?1:0)});if(f.result) types.push_back(f.result);
      uleb(indices,i);bytes body;uleb(body,f.locals.size());
      for(auto [count,type]:f.locals) {uleb(body,count);body.push_back(type);}
      body.insert(body.end(),f.instructions.begin(),f.instructions.end());body.push_back(0x0b);
      uleb(code,body.size());code.insert(code.end(),body.begin(),body.end());
   }
   section(out,1,types);section(out,3,indices);
   if(table) section(out,4,{1,0x70,0,1});
   section(out,7,{1,3,'r','u','n',0,0});
   if(table) section(out,9,{1,0,0x41,0,0x0b,1,1});
   section(out,10,code);return out;
}
template<typename Impl,typename Options=options> struct fixture {
   bytes source;
   eosio::vm::wasm_code_ptr pointer;
   eosio::vm::wasm_allocator allocator;
   eosio::vm::backend<std::nullptr_t,Impl,Options> vm;
   explicit fixture(bytes raw,bool single=true):source(std::move(raw)),pointer(source.data(),source.size()),
      vm(pointer,source.size(),&allocator,{},single,true) {}
   auto bind(uint64_t& remaining,bool& failed) {return vm.get_context().bind_execution_work_budget({&remaining,&failed});}
   auto run() {return vm.call_with_return("env","run");}
   void succeeds(uint64_t expected) {uint64_t left=expected;bool failed=false;auto b=bind(left,failed);run();require(!left&&!failed,"local initialization price differs from source declaration oracle");}
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
      {"all_scalar_local_types_are_zero_and_priced_before_access",[] {both([]<typename Impl> {
         for(uint8_t type:{uint8_t(0x7f),uint8_t(0x7e),uint8_t(0x7d),uint8_t(0x7c)}) {
            fixture<Impl> f(module({{{{1,type}},{0x20,0},type}}));uint64_t left=4;bool failed=false;auto b=f.bind(left,failed);auto v=f.run();
            require(v.has_value() && !left && !failed,"typed local entry/access price changed");
            const bool zero=type==0x7f?v->to_ui32()==0:type==0x7e?v->to_ui64()==0:type==0x7d?v->to_fui32()==0:v->to_fui64()==0;
            require(zero,"implicit local no longer initialized to zero");
         }
      });}},
      {"large_empty_body_pays_for_each_local_and_declaration",[] {both([]<typename Impl> {
         fixture<Impl> f(module({{{{1024,0x7f}},{}}}));f.succeeds(1026);
      });}},
      {"insufficient_entry_budget_rejects_before_operand_capacity_growth",[] {both([]<typename Impl> {
         fixture<Impl> f(module({{{{1024,0x7f}},{}}}));uint64_t left=1025;bool failed=false;
         auto& stack=f.vm.get_context().get_operand_stack();const auto capacity=stack.capacity();auto b=f.bind(left,failed);
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});
         require(failed && left==1025 && stack.capacity()==capacity,"entry failed after partial local charge/allocation");
      });}},
      {"zero_count_declaration_groups_are_not_free",[] {both([]<typename Impl> {
         decls locals(64,{0,0x7f});fixture<Impl> f(module({{locals,{}}}));f.succeeds(65);
      });}},
      {"nested_direct_call_initialization_is_charged_once_per_invocation",[] {both([]<typename Impl> {
         // root: entry1 + call1; child: entry1 + declaration1 + locals3.
         fixture<Impl> f(module({{{},{0x10,1}},{{{3,0x7e}},{}}}));f.succeeds(7);
      });}},
      {"indirect_call_reserves_callee_locals_before_initialization",[] {both([]<typename Impl> {
         fixture<Impl> f(module({{{},{0x41,0,0x11,1,0}},{{{1000,0x7f}},{}}},true));f.succeeds(1005);
         uint64_t left=1004;bool failed=false;auto b=f.bind(left,failed);
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});require(failed && left==1001,"indirect callee bypassed atomic entry reservation");
      });}},
      {"recursive_entry_costs_exhaust_before_unbounded_local_setup",[] {both([]<typename Impl> {
         fixture<Impl> f(module({{{{1,0x7f}},{0x10,0}}}));uint64_t left=10;bool failed=false;auto b=f.bind(left,failed);
         rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});require(failed && left==2,"recursive local entry cost was missing or double counted");
      });}},
      {"two_pass_interpreter_keeps_precomputed_entry_metadata",[] {
         fixture<eosio::vm::interpreter> f(module({{{{4,0x7e}},{0x20,0},0x7e}}),false);f.succeeds(7);
      }},
      {"ordinary_unmetered_locals_and_results_keep_original_behavior",[] {both([]<typename Impl> {
         fixture<Impl,eosio::vm::default_options> f(module({{{{1024,0x7f}},{0x20,0},0x7f}}));uint64_t left=0;bool failed=false;auto b=f.bind(left,failed);
         require(f.run()->to_ui32()==0 && !failed && !left,"unmetered local setup changed behavior or consumed work");
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
