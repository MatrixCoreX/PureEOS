// Dedicated native prototype tests; not an engine switch in the chain.
#ifdef PUREEOS_PARALLEL_JIT_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Parallel_JIT_Backend
#include <boost/test/included/unit_test.hpp>
#include <eosio/chain/parallel_jit_backend.hpp>
#include <eosio/chain/parallel_deadline_service.hpp>
#include <Inline/Serialization.h>
#include <IR/Module.h>
#include <WAST/WAST.h>
#include <WASM/WASM.h>
#include <array>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <sstream>
#include <xmmintrin.h>

namespace vm = eosio::vm;
using namespace eosio::chain::experimental;
using namespace std::chrono_literals;
namespace {
vm::wasm_code compile(const std::string& wat) {
   IR::Module module;
   std::vector<WAST::Error> errors;
   WAST::parseModule(wat.data(), wat.size(), module, errors);
   if (!errors.empty()) {
      std::ostringstream out;
      for (const auto& error : errors) out << error.locus.describe() << ": " << error.message << '\n';
      throw std::runtime_error(out.str());
   }
   // Match the chain's wast_to_wasm helper: this WAST parser adds a debug
   // name section, which the existing OutputStream user-section serializer
   // cannot emit. Names are not executable WASM state or JIT test semantics.
   const auto names = std::find_if(module.userSections.begin(), module.userSections.end(),
      [](const auto& section) { return section.name == "name"; });
   if (names != module.userSections.end()) module.userSections.erase(names);
   Serialization::ArrayOutputStream stream;
   WASM::serialize(stream, module);
   return stream.getBytes();
}
struct local_limit : std::runtime_error { local_limit() : std::runtime_error("local memory budget") {} };
struct host {
   std::function<void()> trip, nested;
   uint32_t trips = 0;
   void interrupt() { ++trips; if (trip) trip(); }
   void child() { if (nested) nested(); }
};
struct functions : vm::registered_host_functions<host> {
   static void resolve(vm::module& module) {
      static std::mutex mutex;
      std::lock_guard lock(mutex);
      vm::registered_host_functions<host>::resolve(module);
   }
};
void register_hosts() {
   static std::once_flag once;
   std::call_once(once, [] {
      functions::add<&host::interrupt>("test", "interrupt");
      functions::add<&host::child>("test", "child");
   });
}
struct options : vm::default_options {
   static constexpr uint32_t max_pages = 16, max_call_depth = 128;
};
template<typename Engine> using backend = vm::backend<functions, Engine, options>;
// Exercise the writer's larger backtrace encoding too. This is a TEST context,
// not an additional production engine or a shared mutable execution context.
template<typename Host> struct profile_context : vm::jit_execution_context<Host, true> {
   using base = vm::jit_execution_context<Host, true>;
   using base::base;
   uint32_t checks = 0;
   bool interrupted = false;
   void check_execution_budget() {
      ++checks;
      if (interrupted) throw vm::timeout_exception{"profile fixture"};
   }
};
struct profile_engine {
   template<typename Host> using context = profile_context<Host>;
   template<typename Host, typename Options, typename DebugInfo>
   using parser = vm::binary_parser<vm::machine_code_writer<context<Host>>, Options, DebugInfo>;
   static constexpr bool is_jit = true;
};
// Instrument the slow helper without adding work to the generated fast path.
template<typename Host> struct counted_context : parallel_jit_context<Host> {
   using base = parallel_jit_context<Host>;
   using base::base;
   uint32_t slow_checks = 0;
   void check_execution_budget() {
      ++slow_checks;
      base::check_execution_budget();
   }
};
struct counted_engine {
   template<typename Host> using context = counted_context<Host>;
   template<typename Host, typename Options, typename DebugInfo>
   using parser = vm::binary_parser<vm::machine_code_writer<context<Host>>, Options, DebugInfo>;
   static constexpr bool is_jit = true;
};
struct fixture {
   vm::wasm_allocator memory;
   backend<parallel_jit> machine;
   host receiver;
   uint32_t local_pages = 16, grows = 0;
   parallel_jit_frame* active = nullptr;
   explicit fixture(vm::wasm_code bytes) : machine(std::move(bytes), nullptr) {
      machine.set_wasm_allocator(&memory);
   }
   static void before_grow(void* state, int32_t current, int32_t pages) {
      auto& self = *static_cast<fixture*>(state);
      ++self.grows;
      if (pages < 0 || current < 0 || uint64_t(current) + uint32_t(pages) > self.local_pages)
         throw local_limit{};
   }
   template<typename F>
   auto run(parallel_deadline_service& timers, F&& fn,
            parallel_deadline_service::clock::time_point end = parallel_deadline_service::clock::now() + 5s,
            parallel_deadline_service::cancellation cancel = {}) {
      parallel_jit_frame frame;
      frame.state = this;
      frame.before_memory_grow = &before_grow;
      auto binding = machine.get_context().bind_frame(frame);
      auto timer = timers.watch(end, std::move(cancel), [&frame]() noexcept {
         frame.interrupt();
      });
      active = &frame;
      auto cleanup = vm::scope_guard([&] { active = nullptr; });
      auto& context = machine.get_context();
      context.reset();
      context.check_execution_budget();
      context.execute_start(&receiver, vm::jit_visitor{0});
      auto result = fn(*this);
      context.check_execution_budget();
      return result;
   }
   template<typename... Args> auto call(const char* name, Args... args) {
      return machine.call_with_return(receiver, "", name, std::move(args)...);
   }
};
const std::string imports = R"(
   (import "test" "interrupt" (func $trip))
   (import "test" "child" (func $child)))";
const std::string healthy = R"((func (export "healthy") (result i32) (i32.const 42)))";
vm::wasm_code make_module(const std::string& body) { return compile("(module " + imports + body + healthy + ")"); }
uint32_t healthy_result(fixture& f, parallel_deadline_service& timers) {
   return f.run(timers, [](fixture& self) { return self.call("healthy")->to_ui32(); });
}
}

BOOST_AUTO_TEST_CASE(healthy_checkpoints_skip_helpers_but_unbound_and_interrupts_fail_closed) {
   register_hosts();
   const auto bytes = make_module(R"(
      (func (export "run") (result i32) (local $n i32)
         (set_local $n (i32.const 10000)) (loop $again
            (set_local $n (i32.sub (get_local $n) (i32.const 1)))
            (br_if $again (get_local $n))) (i32.const 77))
      (func (export "trip") (call $trip) (loop $again (br $again))))");
   vm::wasm_allocator memory;
   backend<counted_engine> machine(vm::wasm_code(bytes), nullptr);
   machine.set_wasm_allocator(&memory);
   host receiver;
   auto& context = machine.get_context();
   machine.initialize(receiver);
   const auto call = [&](const char* name) { return machine.call_with_return(receiver, "", name); };
   BOOST_CHECK_THROW(call("healthy"), vm::wasm_interpreter_exception);
   BOOST_CHECK_EQUAL(context.slow_checks, 1u);
   for (unsigned iteration = 0; iteration < 3; ++iteration) {
      parallel_jit_frame frame;
      frame.state = &receiver;
      frame.before_memory_grow = [](void*, int32_t, int32_t) {};
      const auto before = context.slow_checks;
      {
         auto binding = context.bind_frame(frame);
         BOOST_CHECK_EQUAL(call("run")->to_ui32(), 77u);
         BOOST_CHECK_EQUAL(context.slow_checks, before);
         receiver.trip = [&] { frame.interrupt(); };
         BOOST_CHECK_THROW(call("trip"), vm::timeout_exception);
         BOOST_CHECK_EQUAL(context.slow_checks, before + 1);
         BOOST_CHECK_THROW(call("healthy"), vm::timeout_exception);
         BOOST_CHECK_EQUAL(context.slow_checks, before + 2);
      }
      BOOST_CHECK_THROW(call("healthy"), vm::wasm_interpreter_exception);
      BOOST_CHECK_EQUAL(context.slow_checks, before + 3);
      BOOST_CHECK(vm::signal_dest.load() == nullptr);
      BOOST_CHECK(!vm::saved_exception);
   }
   static_assert(vm::machine_code_writer<counted_context<functions>>::has_execution_budget_flag);
   static_assert(vm::machine_code_writer<counted_context<functions>>::checkpoint_fast_path_size == 13);
   static_assert(!vm::machine_code_writer<profile_context<functions>>::has_execution_budget_flag);
   static_assert(!vm::machine_code_writer<vm::jit_execution_context<functions>>::has_execution_budget_checkpoint);
}

BOOST_AUTO_TEST_CASE(inline_interrupt_preserves_early_and_racing_cancellation) {
   register_hosts();
   fixture f(make_module(""));
   auto& context = f.machine.get_context();
   context.reset();
   for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      parallel_jit_frame frame;
      frame.state = &f;
      frame.before_memory_grow = &fixture::before_grow;
      if (iteration % 2 == 0) frame.interrupt(); // cancellation before binding
      std::promise<void> start;
      auto ready = start.get_future();
      std::jthread interrupter([&] { ready.wait(); frame.interrupt(); });
      start.set_value(); // may run before, during or after publication
      {
         auto binding = context.bind_frame(frame);
         interrupter.join();
         // Test native entry, not just the C++ helper: a lost inline update
         // would incorrectly skip the helper and return a healthy result.
         BOOST_CHECK_THROW(f.call("healthy"), vm::timeout_exception);
         BOOST_CHECK_THROW(context.check_execution_budget(), vm::timeout_exception);
      }
      BOOST_CHECK_THROW(f.call("healthy"), vm::wasm_interpreter_exception);
   }
   parallel_deadline_service timers(1);
   BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
}

BOOST_AUTO_TEST_CASE(frame_alias_rejection_and_detached_interrupt_do_not_poison_reuse) {
   register_hosts();
   fixture first(make_module("")), second(make_module(""));
   auto& context = first.machine.get_context();
   auto& other = second.machine.get_context();
   context.reset(); other.reset();
   parallel_jit_frame old;
   old.state = &first; old.before_memory_grow = &fixture::before_grow;
   {
      auto binding = context.bind_frame(old);
      BOOST_CHECK_THROW((void)other.bind_frame(old), std::logic_error);
      BOOST_CHECK_EQUAL(first.call("healthy")->to_ui32(), 42u);
      BOOST_CHECK_THROW(second.call("healthy"), vm::wasm_interpreter_exception);
   }
   old.interrupt();
   parallel_jit_frame current;
   current.state = &first; current.before_memory_grow = &fixture::before_grow;
   {
      auto binding = context.bind_frame(current);
      old.interrupt(); // detached old frames cannot signal this new binding
      BOOST_CHECK_EQUAL(first.call("healthy")->to_ui32(), 42u);
      current.interrupt();
      BOOST_CHECK_THROW(first.call("healthy"), vm::timeout_exception);
   }
   parallel_deadline_service timers(1);
   BOOST_CHECK_EQUAL(healthy_result(first, timers), 42u);
   BOOST_CHECK_EQUAL(healthy_result(second, timers), 42u);
}

BOOST_AUTO_TEST_CASE(arithmetic_stack_locals_indirect_and_recursion_match_both_engines) {
   register_hosts();
   const auto bytes = make_module(R"(
      (type $unary (func (param i32) (result i32)))
      (table 1 anyfunc) (elem (i32.const 0) $sum)
      (memory 1 4) (global $g (mut i64) (i64.const 7))
      (func $sum (type $unary) (param $n i32) (result i32) (local $s i32)
         (block $done (loop $again
            (br_if $done (i32.eqz (get_local $n)))
            (set_local $s (i32.add (get_local $s) (get_local $n)))
            (set_local $n (i32.sub (get_local $n) (i32.const 1))) (br $again)))
         (get_local $s))
      (func $recursive (param $n i32) (result i32)
         (if (result i32) (i32.eqz (get_local $n)) (then (i32.const 1))
            (else (i32.add (get_local $n) (call $recursive (i32.sub (get_local $n) (i32.const 1)))))))
      (func (export "calculate") (param $a i32) (param $b i64) (result i64)
         (local i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
         (i64.store (i32.const 8) (get_local $b))
         (set_global $g (i64.add (get_global $g) (i64.const 9)))
         (i64.add (i64.add (i64.load (i32.const 8)) (get_global $g))
            (i64.extend_u/i32 (i32.add (call_indirect (type $unary) (get_local $a) (i32.const 0))
               (call $recursive (get_local $a))))))
      (func (export "float") (param f64) (result f64) (f64.mul (get_local 0) (f64.const 1.5))))");
   backend<vm::interpreter> serial(vm::wasm_code(bytes), nullptr);
   backend<vm::jit> native(vm::wasm_code(bytes), nullptr);
   vm::wasm_allocator sm, nm;
   serial.set_wasm_allocator(&sm); native.set_wasm_allocator(&nm);
   host sh, nh;
   fixture private_jit(bytes);
   parallel_deadline_service timers(1);
   for (uint32_t n : {0u, 1u, 7u, 31u}) {
      serial.initialize(sh); native.initialize(nh);
      const auto expected = serial.call_with_return(sh, "", "calculate", uint32_t(n), uint64_t(91))->to_ui64();
      BOOST_CHECK_EQUAL(native.call_with_return(nh, "", "calculate", uint32_t(n), uint64_t(91))->to_ui64(), expected);
      BOOST_CHECK_EQUAL(private_jit.run(timers, [&](fixture& f) { return f.call("calculate", n, uint64_t(91))->to_ui64(); }), expected);
   }
   for (double value : {0.0, -0.0, 1.25, -99.5, std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      serial.initialize(sh); native.initialize(nh);
      const auto expected = serial.call_with_return(sh, "", "float", double(value))->to_fui64();
      BOOST_CHECK_EQUAL(native.call_with_return(nh, "", "float", double(value))->to_fui64(), expected);
      BOOST_CHECK_EQUAL(private_jit.run(timers, [&](fixture& f) { return f.call("float", value)->to_fui64(); }), expected);
   }
}

BOOST_AUTO_TEST_CASE(all_loop_back_edges_check_the_frame_without_wall_clock_races) {
   register_hosts();
   parallel_deadline_service timers(1);
   for (const auto& branch : {"(br $again)", "(br_if $again (i32.const 1))", "(br_table $again $again (i32.const 0))"}) {
      fixture f(make_module("(func (export \"run\") (loop $again (call $trip) " + std::string(branch) + "))"));
      f.receiver.trip = [&] { f.active->interrupt(); };
      BOOST_CHECK_THROW(f.run(timers, [](fixture& self) { self.call("run"); return 0; }), vm::timeout_exception);
      // Flag is set INSIDE the loop: only the next back edge can stop it.
      BOOST_CHECK_EQUAL(f.receiver.trips, 1u);
      BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
      BOOST_CHECK(vm::signal_dest.load() == nullptr);
      BOOST_CHECK(!vm::saved_exception);
   }
}

BOOST_AUTO_TEST_CASE(entry_direct_indirect_start_and_recursive_cancellation) {
   register_hosts();
   parallel_deadline_service timers(1);
   const std::array bodies{
      std::string("(func $target (call $trip)) (func (export \"run\") (call $trip) (call $target))"),
      std::string("(type $t (func)) (table 1 anyfunc) (elem (i32.const 0) $target) (func $target (call $trip)) (func (export \"run\") (call $trip) (call_indirect (type $t) (i32.const 0)))"),
      std::string("(func $recur (call $trip) (call $recur)) (func (export \"run\") (call $recur))"),
      std::string("(func $start (loop $again (call $trip) (br $again))) (start $start)")
   };
   for (size_t index = 0; index < bodies.size(); ++index) {
      fixture f(make_module(bodies[index]));
      f.receiver.trip = [&] { f.active->interrupt(); };
      BOOST_CHECK_THROW(f.run(timers, [](fixture& self) { self.call("run"); return 0; }), vm::timeout_exception);
      BOOST_CHECK_EQUAL(f.receiver.trips, 1u);
      BOOST_CHECK(vm::signal_dest.load() == nullptr);
      if (index != 3) BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
   }
   fixture f(make_module(""));
   BOOST_CHECK_THROW(f.run(timers, [](fixture& self) {
      self.active->interrupt();
      self.call("healthy"); return 0;
   }), vm::timeout_exception);
   BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
}

BOOST_AUTO_TEST_CASE(memory_growth_guards_bridge_exceptions_and_allow_reuse) {
   register_hosts();
   parallel_deadline_service timers(1);
   fixture f(make_module(R"((memory 1 4)
      (func (export "grow") (param i32) (result i32) (grow_memory (get_local 0)))
      (func (export "tripgrow") (call $trip) (drop (grow_memory (i32.const 0)))))"));
   f.local_pages = 2;
   BOOST_CHECK_EQUAL(f.run(timers, [](fixture& self) { return self.call("grow", uint32_t(1))->to_i32(); }), 1);
   BOOST_CHECK_EQUAL(f.grows, 1u);
   BOOST_CHECK_THROW(f.run(timers, [](fixture& self) { return self.call("grow", uint32_t(2))->to_i32(); }), local_limit);
   BOOST_CHECK_THROW(f.run(timers, [](fixture& self) { return self.call("grow", uint32_t(-1))->to_i32(); }), local_limit);
   BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
   f.local_pages = 16;
   BOOST_CHECK_EQUAL(f.run(timers, [](fixture& self) { return self.call("grow", uint32_t(4))->to_i32(); }), -1);
   f.receiver.trip = [&] { f.active->interrupt(); };
   const auto before = f.grows;
   BOOST_CHECK_THROW(f.run(timers, [](fixture& self) { self.call("tripgrow"); return 0; }), vm::timeout_exception);
   BOOST_CHECK_EQUAL(f.grows, before);
   BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
}

BOOST_AUTO_TEST_CASE(memory_growth_preserves_operands_and_bridges_both_stack_parities) {
   register_hosts();
   parallel_deadline_service timers(1);
   for (unsigned parameters = 0; parameters <= 2; ++parameters) {
      for (unsigned locals : {0u, 1u, 2u, 15u, 16u}) {
         for (unsigned operands = 0; operands <= 2; ++operands) {
            BOOST_TEST_CONTEXT("parameters=" << parameters << " locals=" << locals << " operands=" << operands) {
               std::string body = "(memory 1 4) (func (export \"run\")";
               for (unsigned i = 0; i < parameters; ++i) body += " (param i32)";
               body += " (result i32)";
               for (unsigned i = 0; i < locals; ++i) body += " (local i32)";
               if (locals) body += " (set_local " + std::to_string(parameters) + " (i32.const 123))";
               body += " (call $trip) ";
               std::string expression = "(grow_memory (i32.const 1))";
               uint32_t expected = 1;
               for (unsigned i = 0; i < operands; ++i) {
                  expression = "(i32.add (i32.const " + std::to_string(256 + i) + ") " + expression + ")";
                  expected += 256 + i;
               }
               if (locals) {
                  expression = "(i32.add " + expression + " (get_local " + std::to_string(parameters) + "))";
                  expected += 123;
               }
               for (unsigned i = 0; i < parameters; ++i) {
                  expression = "(i32.add " + expression + " (get_local " + std::to_string(i) + "))";
                  expected += 17 + i;
               }
               fixture f(make_module(body + expression + ")"));
               for (unsigned mode = 0; mode < 3; ++mode) {
                  BOOST_TEST_CONTEXT("mode=" << mode) {
                     f.local_pages = mode == 1 ? 1 : 16;
                     f.receiver.trip = [&] {
                        if (mode == 2) f.active->interrupt();
                     };
                     const auto before = f.grows;
                     auto run = [&] { return f.run(timers, [&](fixture& self) {
                        if (parameters == 0) return self.call("run")->to_ui32();
                        if (parameters == 1) return self.call("run", uint32_t(17))->to_ui32();
                        return self.call("run", uint32_t(17), uint32_t(18))->to_ui32();
                     }); };
                     if (mode == 0) BOOST_CHECK_EQUAL(run(), expected);
                     else if (mode == 1) BOOST_CHECK_THROW(run(), local_limit);
                     else BOOST_CHECK_THROW(run(), vm::timeout_exception);
                     BOOST_CHECK_EQUAL(f.grows, before + (mode == 2 ? 0 : 1));
                     BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
                     BOOST_CHECK(vm::signal_dest.load() == nullptr);
                  }
               }
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(timer_stop_generation_and_timeout_interrupt_real_infinite_jit) {
   register_hosts();
   parallel_deadline_service timers(1);
   const auto bytes = make_module("(func (export \"run\") (call $trip) (loop $again (br $again)))");
   for (unsigned mode = 0; mode < 3; ++mode) {
      fixture f(bytes);
      std::stop_source stop;
      auto generation = std::make_shared<std::atomic<uint64_t>>(9);
      f.receiver.trip = [&] {
         if (mode == 1) stop.request_stop();
         if (mode == 2) generation->fetch_add(1, std::memory_order_relaxed);
      };
      const auto end = parallel_deadline_service::clock::now() + (mode == 0 ? 30ms : 5s);
      BOOST_CHECK_THROW(f.run(timers, [](fixture& self) { self.call("run"); return 0; }, end,
         {stop.get_token(), generation, 9}), vm::timeout_exception);
      BOOST_CHECK_EQUAL(f.receiver.trips, 1u);
      BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
      BOOST_CHECK(vm::signal_dest.load() == nullptr);
   }
}

BOOST_AUTO_TEST_CASE(nested_frames_keep_independent_interrupts_and_restore_parent) {
   register_hosts();
   parallel_deadline_service timers(2);
   const auto bytes = make_module(R"(
      (func (export "parent") (call $child) (loop $again (call $trip) (br $again)))
      (func (export "finitechild") (call $trip))
      (func (export "child") (loop $again (call $trip) (br $again))))");
   for (unsigned mask : {1u, 2u, 3u}) {
      fixture parent(bytes), child(bytes);
      unsigned child_timeouts = 0;
      bool signal_restored = false, child_healthy = false;
      parent.receiver.nested = [&] {
         auto* previous_signal = vm::signal_dest.load();
         child.receiver.trip = [&] {
            if (mask & 1) parent.active->interrupt();
            if (mask & 2) child.active->interrupt();
         };
         try { child.run(timers, [&](fixture& f) { f.call((mask & 2) ? "child" : "finitechild"); return 0; }); }
         catch (const vm::timeout_exception&) { ++child_timeouts; }
         signal_restored = vm::signal_dest.load() == previous_signal;
         child_healthy = healthy_result(child, timers) == 42;
      };
      parent.receiver.trip = [&] { parent.active->interrupt(); };
      BOOST_CHECK_THROW(parent.run(timers, [](fixture& f) { f.call("parent"); return 0; }), vm::timeout_exception);
      BOOST_CHECK_EQUAL(child_timeouts, (mask & 2) ? 1u : 0u);
      BOOST_CHECK(signal_restored); BOOST_CHECK(child_healthy);
      BOOST_CHECK_EQUAL(parent.receiver.trips, (mask & 1) ? 0u : 1u);
      BOOST_CHECK_EQUAL(healthy_result(parent, timers), 42u);
      BOOST_CHECK_EQUAL(healthy_result(child, timers), 42u);
      BOOST_CHECK(vm::signal_dest.load() == nullptr);
   }
}

BOOST_AUTO_TEST_CASE(concurrent_private_modules_own_their_context_and_timer_flag) {
   register_hosts();
   const auto bytes = make_module(R"((memory 1 2)
      (func (export "run") (param i32) (result i32)
         (i32.store (i32.const 0) (get_local 0))
         (call $trip) (loop $again (br $again)) (i32.load (i32.const 0))))");
   for (unsigned workers : {1u, 2u, 4u, 8u, 12u, 16u}) {
      parallel_deadline_service timers(workers);
      std::vector<std::future<bool>> futures;
      for (unsigned worker = 0; worker < workers; ++worker) futures.emplace_back(std::async(std::launch::async, [&, worker] {
         fixture f(bytes);
         f.receiver.trip = [&] { f.active->interrupt(); };
         unsigned cancelled = 0;
         for (unsigned iteration = 0; iteration < 8; ++iteration) {
            try { f.run(timers, [&](fixture& self) { return self.call("run", uint32_t(worker))->to_ui32(); }); }
            catch (const vm::timeout_exception&) { ++cancelled; }
            if (healthy_result(f, timers) != 42 || vm::signal_dest.load() || vm::saved_exception) return false;
         }
         return cancelled == 8 && f.receiver.trips == 8;
      }));
      for (auto& future : futures) BOOST_CHECK(future.get());
   }
}

BOOST_AUTO_TEST_CASE(binding_capacity_and_unbound_entry_fail_closed_then_recover) {
   register_hosts();
   fixture f(make_module(""));
   parallel_deadline_service timers(1);
   auto& context = f.machine.get_context();
   context.reset();
   BOOST_CHECK_THROW(f.call("healthy"), vm::wasm_interpreter_exception);
   parallel_jit_frame frame;
   frame.state = &f; frame.before_memory_grow = &fixture::before_grow;
   {
      auto binding = context.bind_frame(frame);
      BOOST_CHECK_THROW((void)context.bind_frame(frame), std::logic_error);
      BOOST_CHECK(std::async(std::launch::async, [&] {
         try { auto illegal = context.bind_frame(frame); } catch (const std::logic_error&) { return true; }
         return false;
      }).get());
   }
   {
      auto occupied = timers.watch(parallel_deadline_service::clock::now() + 5s, {}, []() noexcept {});
      BOOST_CHECK_THROW(healthy_result(f, timers), std::length_error);
   }
   BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
   BOOST_CHECK(vm::signal_dest.load() == nullptr);
}

BOOST_AUTO_TEST_CASE(code_capacity_accounts_for_dense_loop_and_function_checkpoints) {
   register_hosts();
   std::string dense = "(func (export \"run\") (result i32) ";
   for (unsigned i = 0; i < 2048; ++i) dense += "(loop (nop)) ";
   dense += "(i32.const 99)) ";
   for (unsigned i = 0; i < 256; ++i) dense += "(func (nop)) ";
   fixture f(make_module(dense));
   parallel_deadline_service timers(1);
   BOOST_CHECK_EQUAL(f.run(timers, [](fixture& self) { return self.call("run")->to_ui32(); }), 99u);
   BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
   static_assert(vm::machine_code_writer<vm::jit_execution_context<functions>>::max_prologue_size == 21);
   static_assert(vm::machine_code_writer<parallel_jit_context<functions>>::max_prologue_size > 21);
}

BOOST_AUTO_TEST_CASE(optional_profile_checkpoint_preserves_the_backtrace_stack_encoding) {
   register_hosts();
   const auto bytes = make_module(R"((memory 1 4)
      (func (export "grow0") (result i32) (grow_memory (i32.const 1)))
      (func (export "grow1") (result i32) (local i32)
         (i32.add (i32.const 77) (grow_memory (i32.const 1))))
      (func (export "run") (result i32) (local $n i32)
      (set_local $n (i32.const 10)) (loop $again
         (set_local $n (i32.sub (get_local $n) (i32.const 1))) (br_if $again (get_local $n)))
      (i32.const 77)))");
   backend<profile_engine> machine(vm::wasm_code(bytes), nullptr);
   vm::wasm_allocator memory;
   machine.set_wasm_allocator(&memory);
   host receiver;
   auto& context = machine.get_context();
   BOOST_REQUIRE(static_cast<void*>(&context) == static_cast<void*>(static_cast<vm::jit_execution_context<functions, true>*>(&context)));
   machine.initialize(receiver);
   BOOST_CHECK_EQUAL(machine.call_with_return(receiver, "", "run")->to_ui32(), 77u);
   BOOST_CHECK_EQUAL(context.checks, 11u); // entry + ten visits to the loop target
   BOOST_CHECK_EQUAL(machine.call_with_return(receiver, "", "grow0")->to_i32(), 1);
   BOOST_CHECK_EQUAL(machine.call_with_return(receiver, "", "grow1")->to_i32(), 79);
   context.interrupted = true;
   BOOST_CHECK_THROW(machine.call_with_return(receiver, "", "healthy"), vm::timeout_exception);
   context.interrupted = false;
   machine.initialize(receiver);
   BOOST_CHECK_EQUAL(machine.call_with_return(receiver, "", "healthy")->to_ui32(), 42u);
   BOOST_CHECK(vm::signal_dest.load() == nullptr);
}

BOOST_AUTO_TEST_CASE(jit_owned_metadata_survives_parser_storage_release) {
   register_hosts();
   parallel_deadline_service timers(1);
   for (const auto& memory : {"", "(memory 0 0)", "(memory 1 4)", "(memory 1)"}) {
      fixture f(make_module(std::string(memory) + R"(
         (func (export "sync_call") (param i64 i64 i32) (result i64) (i64.const 123)))"));
      auto& module = f.machine.get_module();
      BOOST_REQUIRE(module.jit_mod);
      const auto index = module.get_exported_function("sync_call");
      BOOST_REQUIRE(index != std::numeric_limits<uint32_t>::max());
      const auto& type = module.jit_mod->get_function_type(index);
      BOOST_CHECK(type == vm::host_function({vm::i64, vm::i64, vm::i32}, {vm::i64}));
      BOOST_CHECK_EQUAL(module.jit_mod->memories.size(), std::string(memory).empty() ? 0u : 1u);
      BOOST_CHECK_EQUAL(f.run(timers, [](fixture& self) {
         return self.call("sync_call", uint64_t(0), uint64_t(0), uint32_t(0))->to_ui64();
      }), 123u);
   }
}

BOOST_AUTO_TEST_CASE(normal_and_exceptional_frames_restore_the_callers_native_float_control) {
   register_hosts();
   parallel_deadline_service timers(1);
   fixture f(make_module(R"((memory 1 4)
      (func (export "trip") (call $trip) (loop $again (br $again)))
      (func (export "grow") (drop (grow_memory (i32.const 1))))
      (func (export "trap") (unreachable)))"));
   f.local_pages = 1;
   const uint32_t original = _mm_getcsr();
   auto restore = vm::scope_guard([&] { _mm_setcsr(original); });
   const uint32_t requested = (original & ~uint32_t(0x6000)) | uint32_t(0x4000);
   for (unsigned mode = 0; mode < 5; ++mode) {
      _mm_setcsr(requested);
      f.receiver.trip = [&] {
         if (mode == 1) f.active->interrupt();
         else throw std::runtime_error("injected host failure");
      };
      unsigned observed = 0;
      try {
         f.run(timers, [&](fixture& self) {
            self.call(mode == 0 ? "healthy" : mode == 3 ? "grow" : mode == 4 ? "trap" : "trip");
            return 0;
         });
      } catch (const vm::timeout_exception&) { observed = 1; }
        catch (const local_limit&) { observed = 3; }
        catch (const vm::exception&) { observed = 4; }
        catch (const std::runtime_error&) { observed = 2; }
      const uint32_t after = _mm_getcsr();
      BOOST_CHECK_EQUAL(observed, mode);
      BOOST_CHECK_EQUAL(after, requested);
      BOOST_CHECK_EQUAL(healthy_result(f, timers), 42u);
      BOOST_CHECK_EQUAL(_mm_getcsr(), requested);
   }
}
#endif
