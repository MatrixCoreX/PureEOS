#include <eosio/vm/backend.hpp>
#include <functional>
#include <iostream>
#include <mutex>

namespace {
struct host { unsigned calls = 0; void ping() { ++calls; } };
using registry = eosio::vm::registered_host_functions<host>;

void require(bool value, const char* message) {
   if (!value) throw std::runtime_error(message);
}

template<typename F> void rejects(F&& f) {
   try { f(); }
   catch (const eosio::vm::wasm_interpreter_exception&) { return; }
   throw std::runtime_error("invalid VM entry did not throw the expected exception");
}

std::vector<uint8_t> module(bool start) {
   std::vector<uint8_t> code{0,97,115,109,1,0,0,0,1,4,1,0x60,0,0,
      2,12,1,3,'e','n','v',4,'p','i','n','g',0,0,
      7,7,1,3,'r','u','n',0,0};
   if (start) code.insert(code.end(), {8,1,0});
   return code;
}

template<typename Impl> struct fixture {
   using backend = eosio::vm::backend<registry, Impl, eosio::vm::default_options>;
   using context = typename Impl::template context<registry>;
   host native;
   std::vector<uint8_t> source;
   eosio::vm::wasm_code_ptr pointer;
   backend compiled, active;
   context ctx;
   eosio::vm::wasm_allocator allocator;
   explicit fixture(bool start = false)
      : source(module(start)), pointer(source.data(), source.size()),
        compiled(pointer, source.size(), nullptr, {}, true, false),
        ctx(compiled.get_module(), 64) {
      active.share(compiled);
      active.set_context(&ctx);
      active.set_wasm_allocator(&allocator);
      active.reset_max_pages();
      active.reset_max_call_depth();
      active.initialize(native);
   }
   void run() { active.call(native, "env", "run"); }
};

template<typename F> void both(F&& f) {
   f.template operator()<eosio::vm::interpreter>();
#if defined(__x86_64__)
   f.template operator()<eosio::vm::jit>();
#endif
}
}

int main() {
   registry::add<&host::ping>("env", "ping");
   const std::vector<std::pair<const char*, std::function<void()>>> cases{
      {"ordinary_imported_start_and_export_keep_their_effects", [] { both([]<typename Impl> {
         fixture<Impl> f(true);
         require(f.native.calls == 1, "imported start did not run exactly once");
         f.run();
         require(f.native.calls == 2, "valid imported export changed behavior");
      }); }},
      {"missing_export_is_an_exception_and_context_remains_usable", [] { both([]<typename Impl> {
         fixture<Impl> f;
         rejects([&] { f.active.call(f.native, "env", "missing"); });
         require(f.native.calls == 0, "missing export dispatched a native effect");
         f.run();
         require(f.native.calls == 1, "valid call failed after a missing export");
      }); }},
      {"every_out_of_range_index_rejects_before_native_dispatch", [] { both([]<typename Impl> {
         fixture<Impl> f;
         for (uint32_t index : {1u, 2u, UINT32_MAX - 1, UINT32_MAX})
            rejects([&] { f.active.call(&f.native, index); });
         require(f.native.calls == 0, "invalid index dispatched a native effect");
         f.run();
         require(f.native.calls == 1, "invalid index damaged valid execution");
      }); }},
      {"failed_lookup_does_not_repeat_an_imported_start", [] { both([]<typename Impl> {
         fixture<Impl> f(true);
         rejects([&] { f.active.call(f.native, "env", "missing"); });
         require(f.native.calls == 1, "failed lookup reran an imported start");
         f.run();
         require(f.native.calls == 2, "valid execution after imported start changed");
      }); }}
   };
   unsigned failed = 0;
   for (const auto& [name, test] : cases) {
      try { test(); std::cerr << name << ": passed\n"; }
      catch (const std::exception& e) {
         ++failed;
         std::cerr << name << ": FAILED: " << e.what() << '\n';
      }
   }
   std::cout << "{\"status\":\"" << (failed ? "failed" : "passed")
             << "\",\"cases\":" << cases.size() << ",\"passed\":" << cases.size() - failed
             << ",\"failed\":" << failed << "}\n";
   return failed ? 1 : 0;
}
