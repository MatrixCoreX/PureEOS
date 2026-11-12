#include <eosio/vm/backend.hpp>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

// Uniform block-reservation prototype, with positive entry/backedge overhead.
// Not a calibrated instruction/host cost schedule or active chain billing.
namespace {
using bytes = std::vector<uint8_t>;
struct metered_options : eosio::vm::default_options {
   static constexpr bool enable_execution_work_checkpoints = true;
   static constexpr uint32_t max_call_depth = 64;
};
struct disabled_metered_options : metered_options {
   static constexpr bool enable_execution_work_checkpoints = false;
};
#if defined(__x86_64__)
template<typename Host> struct guarded_context : eosio::vm::jit_execution_context<Host> {
   using eosio::vm::jit_execution_context<Host>::jit_execution_context;
   uint64_t checks=0;
   bool interrupted=false;
   void check_execution_budget() {
      ++checks;
      if(interrupted) throw std::runtime_error("independent native deadline interrupted");
   }
};
struct guarded_jit {
   template<typename Host> using context=guarded_context<Host>;
   template<typename Host,typename Options,typename DebugInfo>
   using parser=eosio::vm::binary_parser<eosio::vm::machine_code_writer<context<Host>>,Options,DebugInfo>;
   static constexpr bool is_jit=true;
};
#endif
static_assert(eosio::vm::opcode::variant_size() == 256);
void require(bool c, const char* message) { if (!c) throw std::runtime_error(message); }
template<typename E, typename F> void rejects(F&& f) {
   try { f(); } catch (const E&) { return; }
   throw std::runtime_error("expected runtime rejection");
}
void uleb(bytes& out, size_t value) {
   do { auto b = uint8_t(value & 127); value >>= 7; out.push_back(b | (value ? 128 : 0)); } while(value);
}
void section(bytes& out, uint8_t id, const bytes& value) {
   out.push_back(id); uleb(out,value.size()); out.insert(out.end(),value.begin(),value.end());
}
bytes module(const std::vector<bytes>& bodies, bool table=false, uint32_t start=UINT32_MAX) {
   bytes out{0,97,115,109,1,0,0,0};
   section(out,1,{1,0x60,0,0});
   bytes funcs; uleb(funcs,bodies.size()); for(size_t i=0;i<bodies.size();++i) funcs.push_back(0);
   section(out,3,funcs);
   if(table) section(out,4,{1,0x70,0,1});
   section(out,7,{1,3,'r','u','n',0,0});
   if(start!=UINT32_MAX) {bytes value; uleb(value,start); section(out,8,value);}
   if(table) section(out,9,{1,0,0x41,0,0x0b,1,1});
   bytes code; uleb(code,bodies.size());
   for(const auto& b:bodies) {uleb(code,b.size()); code.insert(code.end(),b.begin(),b.end());}
   section(out,10,code); return out;
}
template<typename Impl, typename Options=metered_options> struct fixture {
   bytes source;
   eosio::vm::wasm_code_ptr pointer;
   eosio::vm::wasm_allocator allocator;
   using backend = eosio::vm::backend<std::nullptr_t,Impl,Options>;
   backend vm;
   fixture(bytes raw, bool single=true) : source(std::move(raw)), pointer(source.data(),source.size()),
      vm(pointer,source.size(),&allocator,{},single,true) {}
   void run() { vm.call("env","run"); }
   auto bind(uint64_t& remaining, bool& failed) {
      return vm.get_context().bind_execution_work_budget({&remaining,&failed});
   }
   void succeeds(uint64_t budget, uint64_t expected_used) {
      bool failed=false; auto binding=bind(budget,failed); const auto before=budget;
      run(); require(!failed && before-budget==expected_used,"wrong executed checkpoint count");
   }
   void exhausts(uint64_t budget, uint64_t expected_remaining=0) {
      bool failed=false; auto binding=bind(budget,failed);
      rejects<eosio::vm::execution_work_exhausted>([&] { run(); });
      require(failed && budget==expected_remaining,"exhaustion did not preserve remaining budget");
      rejects<eosio::vm::execution_work_exhausted>([&] { run(); });
   }
};
template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}

uint64_t random(uint64_t& state) {
   state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state;
}
struct program { bytes code; uint64_t work = 0; };
void append(bytes& out, const bytes& value) { out.insert(out.end(),value.begin(),value.end()); }
// Independent structured-program oracle. Counts are evaluated from the
// generated program tree, without calling the parser/instrumentation policy.
program generate(uint64_t& state, unsigned depth=0) {
   program out;
   for(unsigned i=0;i<4;++i) {
      const auto kind=random(state)%(depth<2?8:4);
      switch(kind) {
         case 0: {const auto n=random(state)%7+1; out.code.insert(out.code.end(),n,1); out.work+=n; break;}
         case 1: {append(out.code,{0x41,7,0x41,9,0x6a,0x1a}); out.work+=4; break;}
         case 2: {append(out.code,{0x10,1}); out.work+=7; break;} // call + entry + five nops
         case 3: {append(out.code,{0x41,0,0x11,0,0}); out.work+=8; break;}
         case 4: {
            const auto condition=uint8_t(random(state)%2); auto yes=generate(state,depth+1), no=generate(state,depth+1);
            append(out.code,{0x41,condition,0x04,0x40}); append(out.code,yes.code);
            out.code.push_back(0x05); append(out.code,no.code); out.code.push_back(0x0b);
            out.work+=2+(condition?yes.work:no.work); break;
         }
         case 5: {
            const auto n=uint8_t(random(state)%4+1), local=uint8_t(depth); auto body=generate(state,depth+1);
            append(out.code,{0x41,n,0x21,local,0x03,0x40}); append(out.code,body.code);
            append(out.code,{0x20,local,0x41,1,0x6b,0x22,local,0x0d,0,0x0b});
            out.work+=2+uint64_t(n)*(1+body.work+5); break;
         }
         case 6: {
            append(out.code,{0x02,0x40,0x41,1,0x0d,0}); out.code.insert(out.code.end(),13,1);
            out.code.push_back(0x0b); out.work+=2; break;
         }
         case 7: {
            append(out.code,{0x02,0x40,0x41,0,0x0e,1,0,0}); out.code.insert(out.code.end(),11,1);
            out.code.push_back(0x0b); out.work+=2; break;
         }
      }
   }
   return out;
}

template<typename Impl> void shared_execution() {
   using backend=typename fixture<Impl>::backend;
   using context=typename Impl::template context<std::nullptr_t>;
   backend a,b;
   {
      auto raw=module({{0,0x10,1,0x0b},{0,1,1,0x0b}});
      eosio::vm::wasm_code_ptr ptr(raw.data(),raw.size());
      backend compiled(ptr,raw.size(),nullptr,{},true,false);
      a.share(compiled); b.share(compiled);
   }
   context ca(a.get_module(),64), cb(b.get_module(),64);
   eosio::vm::wasm_allocator aa,ab;
   a.set_context(&ca); b.set_context(&cb); a.set_wasm_allocator(&aa); b.set_wasm_allocator(&ab);
   a.reset_max_pages(); b.reset_max_pages(); a.initialize(); b.initialize();
   // Both contexts execute the SAME immutable code on different native threads.
   // No active meter address may be embedded in that code or its metadata.
   std::exception_ptr errors[2];
   auto run=[&](backend& vm,unsigned index) {
      try {
         for(unsigned i=0;i<100;++i) {
            uint64_t left=5+index; bool failed=false;
            auto binding=vm.get_context().bind_execution_work_budget({&left,&failed});
            vm.call("env","run");
            require(left==index && !failed,"shared code used another context's work budget");
         }
      } catch (...) {errors[index]=std::current_exception();}
   };
   std::thread first(run,std::ref(a),0),second(run,std::ref(b),1);
   first.join(); second.join(); for(auto& error:errors) if(error) std::rethrow_exception(error);
}
template<typename Impl,typename F> void with_uninitialized_context(const bytes& raw,F&& operation) {
   using backend=typename fixture<Impl>::backend;
   using context=typename Impl::template context<std::nullptr_t>;
   eosio::vm::wasm_code_ptr ptr(const_cast<uint8_t*>(raw.data()),raw.size());
   backend compiled(ptr,raw.size(),nullptr,{},true,false),active;
   active.share(compiled); context ctx(active.get_module(),64); eosio::vm::wasm_allocator allocator;
   active.set_context(&ctx); active.set_wasm_allocator(&allocator); active.reset_max_pages(); active.reset_max_call_depth();
   operation(active,ctx);
}
}

int main(int argc, char** argv) {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"empty_function_charges_entry_and_requires_binding",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0x0b}}));
         rejects<std::logic_error>([&] { f.run(); });
         f.succeeds(1,1); f.exhausts(0); f.succeeds(UINT64_MAX,1);
      }); }},
      {"default_compilation_remains_unmetered",[] { both([]<typename Impl> {
         fixture<Impl,eosio::vm::default_options> f(module({{0,0x0b}})); f.run();
         f.succeeds(0,0);
         fixture<Impl,disabled_metered_options> disabled(module({{0,1,0x0b}}));
         disabled.run(); disabled.succeeds(0,0);
      }); }},
      {"start_function_is_bound_before_initialization_and_default_is_unchanged",[] {both([]<typename Impl> {
         const auto raw=module({{0,0x0b},{0,1,1,1,0x0b}},false,1);
         rejects<std::logic_error>([&] {fixture<Impl> unbound_start(raw);});
         fixture<Impl,eosio::vm::default_options> original(raw); original.run(); original.succeeds(0,0);
         with_uninitialized_context<Impl>(raw,[](auto& vm,auto& ctx) {
            uint64_t left=5; bool failed=false; auto binding=ctx.bind_execution_work_budget({&left,&failed});
            vm.initialize(); require(left==1 && !failed,"start function was free or counted twice");
            vm.call("env","run"); require(left==0 && !failed,"apply work did not share start budget");
         });
      });}},
      {"start_budget_failure_recovers_on_the_same_context",[] {both([]<typename Impl> {
         with_uninitialized_context<Impl>(module({{0,0x0b},{0,1,1,1,0x0b}},false,1),[](auto& vm,auto& ctx) {
            {
               uint64_t left=3; bool failed=false; auto binding=ctx.bind_execution_work_budget({&left,&failed});
               rejects<eosio::vm::execution_work_exhausted>([&] {vm.initialize();});
               require(left==2 && failed,"start block failed to reserve atomically");
            }
            uint64_t left=5; bool failed=false; auto binding=ctx.bind_execution_work_budget({&left,&failed});
            vm.initialize(); vm.call("env","run"); require(left==0 && !failed,"failed start poisoned reusable context");
         });
      });}},
      {"infinite_start_function_stops_at_the_same_shared_budget",[] {both([]<typename Impl> {
         with_uninitialized_context<Impl>(module({{0,0x0b},{0,0x03,0x40,0x0c,0,0x0b,0x0b}},false,1),[](auto& vm,auto& ctx) {
            uint64_t left=40; bool failed=false; auto binding=ctx.bind_execution_work_budget({&left,&failed});
            rejects<eosio::vm::execution_work_exhausted>([&] {vm.initialize();});
            require(failed && left==0,"module start bypassed loop work limit");
         });
      });}},
      {"every_loop_backedge_is_charged_and_failure_recovers",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{1,1,0x7f,0x41,3,0x21,0,0x03,0x40,0x20,0,0x41,1,
                                 0x6b,0x22,0,0x0d,0,0x0b,0x0b}}));
         f.succeeds(23,23); f.exhausts(22,4); f.succeeds(24,23);
      }); }},
      {"infinite_loop_stops_at_budget",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0x03,0x40,0x0c,0,0x0b,0x0b}})); f.exhausts(29);
      }); }},
      {"two_pass_compilation_executes_the_same_checkpoints",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0x10,1,0x0b},{0,0x0b}}),false);
         f.succeeds(3,3); f.exhausts(2); f.succeeds(3,3);
      }); }},
      {"direct_calls_preserve_stack_and_count_each_entry",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0x41,42,0x10,1,0x10,1,0x41,42,0x47,0x04,0x40,0,0x0b,0x0b},
                                 {0,0x0b}}));
         f.succeeds(9,9); f.exhausts(8,2); f.succeeds(9,9);
      }); }},
      {"indirect_calls_count_callee_entry",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0x41,0,0x11,0,0,0x0b},{0,0x0b}},true));
         f.succeeds(4,4); f.exhausts(3); f.succeeds(4,4);
      }); }},
      {"recursive_calls_stop_before_native_stack_limit",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0x10,0,0x0b}})); f.exhausts(20); f.exhausts(25);
      }); }},
      {"traps_do_not_consume_later_unexecuted_calls",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{0,0,0x10,1,0x0b},{0,0x0b}}));
         uint64_t left=10; bool failed=false; auto binding=f.bind(left,failed);
         rejects<eosio::vm::wasm_interpreter_exception>([&] { f.run(); });
         require(left==8 && !failed,"trap charged unreachable callee");
      }); }},
      {"separate_compilations_and_nested_bindings_are_isolated",[] { both([]<typename Impl> {
         fixture<Impl> a(module({{0,0x0b}})), b(module({{0,0x0b}}));
         uint64_t x=3,y=4; bool xf=false,yf=false;
         auto outer=a.bind(x,xf); a.run();
         {auto inner=a.bind(y,yf); a.run(); b.succeeds(1,1);}
         a.run(); require(x==1 && y==3 && !xf && !yf,"per-context budget leaked");
      }); }},
      {"internal_opcode_is_never_accepted_from_wasm",[] { both([]<typename Impl> {
         for(uint8_t op:{0xc0,0xc1,0xff}) {
            rejects<eosio::vm::wasm_parse_exception>([&] { fixture<Impl> f(module({{0,op,0x0b}})); });
            rejects<eosio::vm::wasm_parse_exception>([&] {
               fixture<Impl,eosio::vm::default_options> f(module({{0,op,0x0b}}));
            });
         }
      }); }},
      {"visitor_without_work_support_cannot_silently_skip_checkpoints",[] {
         rejects<eosio::vm::wasm_interpreter_exception>([] {
            eosio::vm::base_visitor{}(eosio::vm::execution_work_t{1});
         });
      }},
      {"long_straight_line_body_and_atomic_block_reservation",[] { both([]<typename Impl> {
         bytes body{0}; body.insert(body.end(),10000,1); body.push_back(0x0b);
         fixture<Impl> f(module({body})); f.succeeds(10001,10001); f.exhausts(10000,9999); f.succeeds(10001,10001);
      }); }},
      {"untaken_branches_have_no_work_reservation",[] { both([]<typename Impl> {
         for(uint8_t condition:{0,1}) {
            fixture<Impl> f(module({{0,0x41,condition,0x04,0x40,0x41,5,0x1a,0x05,
               1,1,1,1,0x41,7,0x1a,0x0b,0x41,9,0x1a,0x0b}}));
            const uint64_t expected=condition?7:11; f.succeeds(expected,expected);
         }
      }); }},
      {"branch_table_backedges_charge_each_iteration",[] { both([]<typename Impl> {
         fixture<Impl> f(module({{1,1,0x7f,0x41,3,0x21,0,0x02,0x40,0x03,0x40,0x20,0,
            0x41,1,0x6b,0x22,0,0x45,0x0e,1,0,1,0x0b,0x0b,0x0b}}));
         f.succeeds(26,26); f.exhausts(25,5); f.succeeds(26,26);
      }); }},
      {"native_trap_preserves_deterministic_block_reservation",[] { both([]<typename Impl> {
         const auto raw=module({{0,0x41,1,0x41,0,0x6d,0x1a,1,1,0x0b}});
         // Existing SIGFPE translation in the JIT uses wasm_memory_exception;
         // the interpreter performs a direct arithmetic check. Compare with
         // the uninstrumented engine, rather than changing trap semantics here.
         auto check_trap=[](auto& f) {
            if constexpr (Impl::is_jit)
               rejects<eosio::vm::wasm_memory_exception>([&] {f.run();});
            else
               rejects<eosio::vm::wasm_interpreter_exception>([&] {f.run();});
         };
         fixture<Impl,eosio::vm::default_options> original(raw); check_trap(original);
         fixture<Impl> f(raw);
         uint64_t left=10; bool failed=false; auto binding=f.bind(left,failed);
         check_trap(f);
         require(left==3 && !failed,"trapping block reservation differs by VM");
      }); }},
      {"shared_compiled_code_has_thread_private_execution_budgets",[] { both([]<typename Impl> {
         shared_execution<Impl>();
      }); }},
#if defined(__x86_64__)
      {"profile_JIT_preserves_backtrace_and_stack_alignment",[] {
         fixture<eosio::vm::jit_profile> f(module({{0,0x41,42,0x10,1,0x41,42,0x47,
            0x04,0x40,0,0x0b,0x0b},{1,17,0x7f,0x10,2,0x0b},{0,0x0b}}));
         f.succeeds(27,27); f.exhausts(26,2); f.succeeds(27,27);
      }},
      {"work_meter_does_not_replace_native_deadline_checkpoints",[] {
         fixture<guarded_jit> f(module({{1,1,0x7f,0x41,3,0x21,0,0x03,0x40,0x20,0,0x41,1,
            0x6b,0x22,0,0x0d,0,0x0b,0x0b}}));
         f.succeeds(23,23); require(f.vm.get_context().checks==4,"lost native entry/backedge deadlines");
         f.vm.get_context().interrupted=true;
         uint64_t left=100; bool failed=false;
         {
            auto bound=f.bind(left,failed);
            rejects<std::runtime_error>([&] {f.run();});
            require(left==100 && !failed,"native deadline changed deterministic work before execution");
         }
         f.vm.get_context().interrupted=false; f.succeeds(23,23);
      }},
#endif
      {"128_structured_programs_match_independent_tree_oracle",[] {
         uint64_t state=0xa1c10242026;
         for(unsigned i=0;i<128;++i) {
            auto p=generate(state); bytes body{1,3,0x7f}; append(body,p.code); body.push_back(0x0b);
            // Entry1 + declaration1 + three implicit locals + generated body.
            const auto raw=module({body,{0,1,1,1,1,1,0x0b}},true); const auto expected=5+p.work;
            both([&]<typename Impl> {
               fixture<Impl> f(raw,i%2==0); f.succeeds(expected,expected);
               {
                  uint64_t left=expected-1; bool failed=false; auto binding=f.bind(left,failed);
                  rejects<eosio::vm::execution_work_exhausted>([&] {f.run();});
                  require(failed && left<expected,"one-unit underbudget accepted structured program");
               }
               f.succeeds(expected+19,expected);
            });
         }
      }}
   };
   if(argc>2) return 2;
   const std::string_view filter=argc==2?argv[1]:"";
   size_t failed=0, executed=0;
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
