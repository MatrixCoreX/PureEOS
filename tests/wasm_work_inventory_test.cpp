#include <eosio/vm/backend.hpp>
#include <eosio/vm/work_block_inventory.hpp>
#include <functional>
#include <iostream>
#include <vector>

namespace {
using bytes = std::vector<uint8_t>;
using inventory = eosio::vm::work_block_inventory;

void require(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}
template<typename Exception, typename F> void rejects(F&& operation) {
   try { operation(); } catch (const Exception&) { return; }
   throw std::runtime_error("expected inventory rejection");
}
void uleb(bytes& out, size_t value) {
   do { uint8_t b=value&127; value>>=7; out.push_back(b | (value ? 128 : 0)); } while(value);
}
void section(bytes& out, uint8_t id, const bytes& data) {
   out.push_back(id); uleb(out,data.size()); out.insert(out.end(),data.begin(),data.end());
}
bytes module(const std::vector<bytes>& bodies, bool table=false) {
   bytes out{0,97,115,109,1,0,0,0};
   section(out,1,{1,0x60,0,0}); // one ()->() type
   bytes funcs; uleb(funcs,bodies.size()); for(size_t i=0;i<bodies.size();++i) funcs.push_back(0);
   section(out,3,funcs);
   if(table) {
      section(out,4,{1,0x70,0,1});
      section(out,9,{1,0,0x41,0,0x0b,1,1});
   }
   bytes code; uleb(code,bodies.size());
   for(const auto& b:bodies) { uleb(code,b.size()); code.insert(code.end(),b.begin(),b.end()); }
   section(out,10,code); return out;
}
template<typename Impl> inventory::inventory parse(const bytes& raw, bool single=true) {
   eosio::vm::wasm_code_ptr ptr(const_cast<uint8_t*>(raw.data()),raw.size());
   using backend=eosio::vm::backend<std::nullptr_t,Impl,eosio::vm::default_options,inventory>;
   backend b(ptr,raw.size(),nullptr,{},single,false);
   return b.get_debug().functions();
}
void same_inventory(const bytes& raw, const std::vector<std::vector<uint32_t>>& expected) {
   const auto reference=parse<eosio::vm::null_backend>(raw);
   require(reference.size()==expected.size(),"defined function count changed");
   for(size_t f=0;f<reference.size();++f) {
      const auto& fn=reference[f];
      require(fn.blocks.size()==expected[f].size(),"wrong control-flow block partition");
      uint32_t previous=0;
      for(size_t i=0;i<fn.blocks.size();++i) {
         const auto& b=fn.blocks[i];
         require(b.instructions==expected[f][i],"wrong original instruction count");
         require(b.wasm_offset>=fn.wasm_offset && b.wasm_offset>previous && b.wasm_offset<raw.size(),"invalid source offset");
         previous=b.wasm_offset;
      }
   }
   require(parse<eosio::vm::interpreter>(raw)==reference,"interpreter partition differs");
   require(parse<eosio::vm::interpreter>(raw,false)==reference,"two-pass parsing changed metadata");
#if defined(__x86_64__)
   require(parse<eosio::vm::jit>(raw)==reference,"JIT partition differs");
#endif
}
template<typename Context> void work_binding() {
   Context context;
   uint64_t parent=10, child=20, fresh=3;
   bool parent_failed=false, child_failed=false, fresh_failed=false;
   rejects<std::logic_error>([&] {context.charge_execution_work(1);});
   {
      auto outer=context.bind_execution_work_budget({&parent,&parent_failed});
      context.charge_execution_work(3);
      {
         auto inner=context.bind_execution_work_budget({&child,&child_failed});
         context.charge_execution_work(2);
      }
      require(parent==7 && child==18,"nested context changed the wrong execution budget");
      rejects<eosio::vm::execution_work_exhausted>([&] {context.charge_execution_work(8);});
      require(parent==7 && parent_failed && !child_failed,"exhaustion crossed a context binding");
   }
   rejects<std::logic_error>([&] {context.charge_execution_work(0);});
   {
      auto bound=context.bind_execution_work_budget({&fresh,&fresh_failed});
      rejects<eosio::vm::execution_work_exhausted>([&] {
         auto invalid=context.bind_execution_work_budget({&parent,&parent_failed});
      });
      context.charge_execution_work(3);
      require(fresh==0 && !fresh_failed,"failed bind corrupted a reusable context");
   }
   rejects<std::logic_error>([&] {context.charge_execution_work(1);});
}
}

int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"immediates_are_not_instructions",[] {
         same_inventory(module({{0,0x41,0x0b,0x1a,0x41,0x10,0x1a,0x0b}}),{{4}});
      }},
      {"if_else_and_merge_boundaries",[] {
         same_inventory(module({{0,0x41,0,0x04,0x40,0x41,7,0x1a,0x05,0x41,9,0x1a,0x0b,0x0b}}),{{2,2,2}});
      }},
      {"loop_backedge_body_boundary",[] {
         same_inventory(module({{0,0x02,0x40,0x03,0x40,0x41,0,0x0d,0,0x0b,0x0b,0x0b}}),{{2}});
      }},
      {"direct_indirect_calls_and_empty_callee",[] {
         same_inventory(module({{0,0x10,1,0x10,1,0x0b},{0,0x41,42,0x1a,0x0b}}),{{1,1},{2}});
         same_inventory(module({{0,0x41,0,0x11,0,0,0x0b},{0,0x0b}},true),{{2},{}});
      }},
      {"branch_table_return_trap_and_dead_blocks",[] {
         same_inventory(module({{0,0x02,0x40,0x03,0x40,0x41,0,0x0e,1,0,1,0x0b,0x0b,0x0b}}),{{2}});
         same_inventory(module({{0,0x41,0,0x1a,0x0f,0x41,1,0x1a,0x0b}}),{{3,2}});
         same_inventory(module({{0,0,1,0x0b}}),{{1,1}});
      }},
      {"forward_block_targets_split_at_end",[] {
         same_inventory(module({{0,0x41,0,0x1a,0x02,0x40,0x41,1,0x1a,0x0b,0x41,2,0x1a,0x0b}}),{{4,2}});
      }},
      {"metadata_lives_with_shared_compiled_module",[] {
         const auto raw=module({{0,0x41,0,0x1a,0x0b}});
         using backend=eosio::vm::backend<std::nullptr_t,eosio::vm::interpreter,eosio::vm::default_options,inventory>;
         backend shared;
         inventory::inventory expected;
         {
            eosio::vm::wasm_code_ptr ptr(const_cast<uint8_t*>(raw.data()),raw.size());
            backend compiled(ptr,raw.size(),nullptr,{},true,false);
            expected=compiled.get_debug().functions(); shared.share(compiled);
         }
         require(shared.get_debug().functions()==expected,"shared backend lost immutable work inventory");
         inventory copy=shared.get_debug(); copy.relocate(reinterpret_cast<const void*>(uintptr_t(1)));
         require(copy.functions()==expected,"native code relocation changed WASM identity");
      }},
      {"per_execution_budget_binding_restores_after_nested_failure",[] {
         work_binding<eosio::vm::execution_context<std::nullptr_t>>();
#if defined(__x86_64__)
         work_binding<eosio::vm::jit_execution_context<std::nullptr_t>>();
#endif
      }},
      {"malformed_observations_and_unfinished_publish_rejected",[] {
         uint8_t raw[]{0x41,0x01,0x0b,0xff};
         inventory::builder b; b.on_code_start(nullptr,raw); b.on_function_start(nullptr,raw);
         b.on_instr_start(nullptr,raw); b.on_instr_start(nullptr,raw+2); b.on_code_end(nullptr,raw+3);
         inventory stable; stable.set(std::move(b)); const auto before=stable.functions();
         inventory::builder bad; bad.on_code_start(nullptr,raw); bad.on_function_start(nullptr,raw);
         rejects<std::logic_error>([&] {stable.set(std::move(bad));});
         require(stable.functions()==before,"unfinished metadata replaced valid inventory");
         rejects<std::invalid_argument>([&] {bad.on_instr_start(nullptr,raw+3);});
         bad.on_instr_start(nullptr,raw);
         rejects<std::invalid_argument>([&] {bad.on_instr_start(nullptr,raw);});
         rejects<std::invalid_argument>([&] {bad.on_function_start(nullptr,raw);});
         bad.split_next=true; bad.blocks=bad.max_blocks;
         rejects<std::length_error>([&] {bad.on_instr_start(nullptr,raw+1);});
      }}
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
