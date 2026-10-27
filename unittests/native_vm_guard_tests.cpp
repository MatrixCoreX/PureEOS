// Syscall injection and deliberate faults belong only to this dedicated test
// executable, never to nodeos or the ordinary unit_test source glob.
#ifdef PUREEOS_NATIVE_VM_GUARD_FIXTURE
#include <eosio/vm/backend.hpp>
#include <eosio/vm/watchdog.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

namespace vm = eosio::vm;
namespace {
struct allocation_probe {
   bool active = false, fail_map = false, fail_protection = false;
   unsigned maps = 0, protects = 0, unmaps = 0;
   void* base = nullptr;
   size_t length = 0;
   int unmap_result = -1;
};
thread_local allocation_probe allocation;
}

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t) noexcept;
extern "C" int __real_mprotect(void*, size_t, int) noexcept;
extern "C" int __real_munmap(void*, size_t) noexcept;
extern "C" void* __wrap_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) noexcept {
   const bool target = allocation.active && length == vm::max_memory + 2 * size_t(::sysconf(_SC_PAGESIZE))
                       && protection == PROT_NONE && fd == -1 && offset == 0;
   if (target) {
      ++allocation.maps;
      if (allocation.fail_map) { errno = ENOMEM; return MAP_FAILED; }
   }
   auto* result = __real_mmap(address, length, protection, flags, fd, offset);
   if (target) { allocation.base = result; allocation.length = length; }
   return result;
}
extern "C" int __wrap_mprotect(void* address, size_t length, int protection) noexcept {
   if (allocation.active && address == allocation.base && length == size_t(::sysconf(_SC_PAGESIZE))
       && protection == PROT_READ) {
      ++allocation.protects;
      if (allocation.fail_protection) { errno = ENOMEM; return -1; }
   }
   return __real_mprotect(address, length, protection);
}
extern "C" int __wrap_munmap(void* address, size_t length) noexcept {
   const int result = __real_munmap(address, length);
   if (allocation.active && address == allocation.base && length == allocation.length) {
      ++allocation.unmaps; allocation.unmap_result = result;
   }
   return result;
}

namespace {
struct code_fixture {
   vm::growable_allocator code;
   vm::wasm_allocator memory;
   code_fixture() {
      code.use_fixed_memory(size_t(::sysconf(_SC_PAGESIZE)));
      auto* start = code.start_code();
      *code.alloc<unsigned char>(1) = 0;
      code.end_code<false>(start);
      memory.reset(1);
   }
};
bool same_range(std::span<std::byte> a, std::span<std::byte> b) {
   return a.data() == b.data() && a.size() == b.size();
}
sigset_t signal_mask() {
   sigset_t out;
   if (pthread_sigmask(SIG_SETMASK, nullptr, &out)) throw std::runtime_error("read signal mask failed");
   return out;
}
bool same_mask(const sigset_t& a, const sigset_t& b) {
   for (int sig = 1; sig < NSIG; ++sig)
      if (sigismember(&a, sig) != sigismember(&b, sig)) return false;
   return true;
}
struct signal_snapshot {
   std::span<std::byte> code = vm::code_memory_range, memory = vm::memory_range;
   sigjmp_buf* dest = vm::signal_dest.load();
   sigset_t mask = signal_mask();
};
bool matches(const signal_snapshot& expected) {
   return same_range(vm::code_memory_range, expected.code) && same_range(vm::memory_range, expected.memory)
          && vm::signal_dest.load() == expected.dest && same_mask(signal_mask(), expected.mask);
}
bool owns(const code_fixture& frame) {
   return same_range(vm::code_memory_range, frame.code.get_code_span())
          && same_range(vm::memory_range, frame.memory.get_span()) && vm::signal_dest.load();
}
void leave_frame(unsigned mode) {
   if (mode == 1) throw std::runtime_error("guard C++ fixture");
   if (mode == 2) vm::throw_<vm::wasm_memory_exception>("guard longjmp fixture");
   if (mode == 3) siglongjmp(*vm::signal_dest.load(), SIGFPE);
}
struct signal_result {
   bool outer_owned = false, inner_owned = false, parent_restored = false, initial_restored = false;
   bool signals_unblocked = false, unrelated_block_preserved = false, exception_cleared = false;
   unsigned inner_exit = 0, outer_exit = 0;
};
signal_result signal_case(unsigned inner_mode, unsigned outer_mode) {
   code_fixture parent, child;
   const signal_snapshot original;
   sigset_t block;
   sigemptyset(&block);
   for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGPROF, SIGUSR1}) sigaddset(&block, sig);
   if (pthread_sigmask(SIG_BLOCK, &block, nullptr)) throw std::runtime_error("block fixture signals failed");
   const signal_snapshot incoming;
   signal_result result;
   try {
      vm::invoke_with_signal_handler([&] {
         result.outer_owned = owns(parent);
         const signal_snapshot active_parent;
         result.signals_unblocked = true;
         for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGPROF})
            result.signals_unblocked &= sigismember(&active_parent.mask, sig) == 0;
         result.unrelated_block_preserved = sigismember(&active_parent.mask, SIGUSR1) == 1;
         try {
            vm::invoke_with_signal_handler([&] {
               result.inner_owned = owns(child) && vm::signal_dest.load() != active_parent.dest;
               leave_frame(inner_mode);
            }, [&](int sig) { result.inner_exit = sig == SIGFPE ? 3 : 99; }, child.code, &child.memory);
         } catch (const vm::wasm_memory_exception&) { result.inner_exit = 2; }
           catch (const std::runtime_error&) { result.inner_exit = 1; }
         result.parent_restored = matches(active_parent);
         leave_frame(outer_mode);
      }, [&](int sig) { result.outer_exit = sig == SIGFPE ? 3 : 99; }, parent.code, &parent.memory);
   } catch (const vm::wasm_memory_exception&) { result.outer_exit = 2; }
     catch (const std::runtime_error&) { result.outer_exit = 1; }
   result.initial_restored = matches(incoming);
   result.exception_cleared = !vm::saved_exception;
   // Repair the known-bad baseline only AFTER recording its result. Otherwise
   // dangling ranges could contaminate later negative-control test cases.
   vm::code_memory_range = original.code; vm::memory_range = original.memory;
   vm::signal_dest.store(original.dest);
   pthread_sigmask(SIG_SETMASK, &original.mask, nullptr);
   return result;
}
void check_signal_result(const signal_result& r, unsigned inner, unsigned outer) {
   BOOST_CHECK(r.outer_owned); BOOST_CHECK(r.inner_owned);
   BOOST_CHECK(r.parent_restored); BOOST_CHECK(r.initial_restored);
   BOOST_CHECK(r.signals_unblocked); BOOST_CHECK(r.unrelated_block_preserved);
   BOOST_CHECK(r.exception_cleared);
   BOOST_CHECK_EQUAL(r.inner_exit, inner); BOOST_CHECK_EQUAL(r.outer_exit, outer);
}

// Deterministic stand-in for the platform_timer registration protocol, not a
// replacement implementation of timed_run. Removal joins an in-flight callback
// by holding the same mutex; expiration deliberately uses reverse stack order.
struct controlled_watchdog {
   std::mutex mutex;
   std::vector<std::function<void()>> callbacks;
   struct guard {
      controlled_watchdog& owner;
      template<typename F> guard(controlled_watchdog& w, F&& f) : owner(w) {
         std::lock_guard lock(owner.mutex); owner.callbacks.emplace_back(std::forward<F>(f));
      }
      guard(const guard&) = delete;
      ~guard() { std::lock_guard lock(owner.mutex); owner.callbacks.pop_back(); }
   };
   template<typename F> guard scoped_run(F&& f) { return guard(*this, std::forward<F>(f)); }
   void expire(unsigned mask, unsigned repetitions) {
      std::lock_guard lock(mutex);
      for (unsigned repeat = 0; repeat < repetitions; ++repeat)
         for (size_t i = callbacks.size(); i-- > 0;)
            if (mask & (1u << i)) callbacks[i]();
   }
};
bool code_accessible(const void* address, bool jit = false) {
   std::ifstream maps("/proc/self/maps");
   std::string line;
   const auto value = reinterpret_cast<uintptr_t>(address);
   while (std::getline(maps, line)) {
      unsigned long long first = 0, last = 0;
      char permissions[5] = {};
      if (std::sscanf(line.c_str(), "%llx-%llx %4s", &first, &last, permissions) == 3
          && value >= first && value < last) return jit ? permissions[2] == 'x' : permissions[0] == 'r';
   }
   throw std::runtime_error("code mapping missing from /proc/self/maps");
}
vm::wasm_code simple_code() {
   // (module (func (export "f0")) (func (export "f1") (param i32)))
   return {0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x60,
           0x00,0x00,0x60,0x01,0x7f,0x00,0x03,0x03,0x02,0x00,0x01,0x07,
           0x0b,0x02,0x02,0x66,0x30,0x00,0x00,0x02,0x66,0x31,0x00,0x01,
           0x0a,0x07,0x02,0x02,0x00,0x0b,0x02,0x00,0x0b};
}
using backend_type = vm::backend<std::nullptr_t, vm::interpreter>;
struct timeout_result {
   bool parent_ready = false, child_ready = false, flag_clear = false;
   bool parent_expiration_preserved = false, callbacks_empty = false, healthy_reuse = false;
   unsigned caught = 0;
};
template<typename Backend>
timeout_result timeout_case(Backend& parent, Backend& child, unsigned mask,
                            unsigned repetitions, bool callback_thread, bool memory_exception, bool jit = false) {
   controlled_watchdog watchdog;
   timeout_result result;
   try {
      parent.timed_run(watchdog, [&] {
         try {
            child.timed_run(watchdog, [&] {
               if (callback_thread) {
                  std::thread timer([&] { watchdog.expire(mask, repetitions); }); timer.join();
               } else watchdog.expire(mask, repetitions);
               if (memory_exception) throw vm::wasm_memory_exception("controlled fault");
            });
         } catch (const vm::timeout_exception&) { result.caught = 1; }
           catch (const vm::wasm_memory_exception&) { result.caught = 2; }
         result.parent_expiration_preserved = bool(vm::timed_run_has_timed_out.load()) == bool(mask & 1u);
      });
   } catch (...) { result.caught = 99; }
   result.parent_ready = code_accessible(parent.get_module().allocator.get_code_start(), jit);
   result.child_ready = code_accessible(child.get_module().allocator.get_code_start(), jit);
   result.flag_clear = !vm::timed_run_has_timed_out.load();
   result.callbacks_empty = watchdog.callbacks.empty();
   // A failing baseline must not hang the runner by executing PROT_NONE pages.
   // Readability failures above remain failures; the repair is test-only.
   if (!result.parent_ready) parent.get_module().allocator.enable_code(jit);
   if (!result.child_ready) child.get_module().allocator.enable_code(jit);
   result.healthy_reuse = parent.call("env", "f0") && child.call("env", "f1", uint32_t{7});
   return result;
}
void check_timeout_result(const timeout_result& result, unsigned mask, bool memory_exception) {
   BOOST_CHECK(result.parent_ready); BOOST_CHECK(result.child_ready);
   BOOST_CHECK(result.flag_clear); BOOST_CHECK(result.parent_expiration_preserved);
   BOOST_CHECK(result.callbacks_empty); BOOST_CHECK(result.healthy_reuse);
   BOOST_CHECK_EQUAL(result.caught, memory_exception ? (mask ? 1u : 2u) : 0u);
}
struct allocation_result {
   allocation_probe observed;
   bool threw = false, healthy = false;
   int fixture_cleanup = 0;
};
allocation_result allocation_case(unsigned mode) {
   allocation = {}; allocation.active = true;
   allocation.fail_map = mode == 1; allocation.fail_protection = mode == 2;
   allocation_result result;
   try { vm::wasm_allocator memory; memory.reset(1); result.healthy = memory.get_current_page() == 1; }
   catch (const vm::wasm_bad_alloc&) { result.threw = true; }
   allocation.active = false;
   result.observed = allocation;
   if (allocation.base && allocation.base != MAP_FAILED && allocation.unmaps == 0)
      result.fixture_cleanup = __real_munmap(allocation.base, allocation.length);
   return result;
}
void check_allocation_result(const allocation_result& result, unsigned mode) {
   const auto& r = result.observed;
   BOOST_CHECK_EQUAL(result.threw, mode != 0); BOOST_CHECK_EQUAL(result.healthy, mode == 0);
   BOOST_CHECK_EQUAL(r.maps, 1u); BOOST_CHECK_EQUAL(r.protects, mode == 1 ? 0u : 1u);
   BOOST_CHECK_EQUAL(r.unmaps, mode == 1 ? 0u : 1u);
   if (mode != 1) BOOST_CHECK_EQUAL(r.unmap_result, 0);
   BOOST_CHECK_EQUAL(result.fixture_cleanup, 0);
}
} // namespace

BOOST_AUTO_TEST_SUITE(native_vm_guard_tests)

BOOST_AUTO_TEST_CASE(nested_signal_ranges_destination_and_mask_restore_on_every_exit) {
   for (unsigned round = 0; round < 3; ++round)
      for (unsigned inner = 0; inner < 4; ++inner)
         for (unsigned outer = 0; outer < 4; ++outer) {
            BOOST_TEST_CONTEXT("round=" << round << " inner=" << inner << " outer=" << outer) {
               check_signal_result(signal_case(inner, outer), inner, outer);
            }
         }
}

BOOST_AUTO_TEST_CASE(signal_frames_are_thread_local_and_failed_calls_can_be_reused) {
   vm::setup_signal_handler();
   std::array<std::array<signal_result, 16>, 4> results;
   std::barrier start(4);
   std::vector<std::thread> threads;
   for (unsigned worker = 0; worker < 4; ++worker) threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 16; ++i) results[worker][i] = signal_case(i % 4, i / 4);
   });
   for (auto& thread : threads) thread.join();
   for (const auto& worker : results)
      for (unsigned i = 0; i < 16; ++i) check_signal_result(worker[i], i % 4, i / 4);
}

BOOST_AUTO_TEST_CASE(nested_timeout_restores_each_module_and_preserves_expired_ancestors) {
   vm::wasm_allocator parent_memory, child_memory;
   backend_type parent(simple_code(), &parent_memory), child(simple_code(), &child_memory);
   BOOST_REQUIRE(!parent.get_module().allocator.get_code_span().empty());
   BOOST_REQUIRE(!child.get_module().allocator.get_code_span().empty());
   for (unsigned round = 0; round < 3; ++round)
      for (unsigned mask = 0; mask < 4; ++mask)
         for (bool threaded : {false, true})
            for (bool exception : {false, true}) {
               BOOST_TEST_CONTEXT("round=" << round << " mask=" << mask << " threaded=" << threaded << " exception=" << exception) {
                  check_timeout_result(timeout_case(parent, child, mask, 2, threaded, exception), mask, exception);
               }
            }
   // timed_run scopes may share compiled code. This checks their cleanup state,
   // not concurrent or recursive execution of the same mutable VM context.
   for (unsigned round = 0; round < 3; ++round)
      for (unsigned mask = 0; mask < 4; ++mask)
         check_timeout_result(timeout_case(parent, parent, mask, 2, true, true), mask, true);
}

BOOST_AUTO_TEST_CASE(timeout_state_is_thread_local_with_concurrent_nested_backends) {
   struct pair {
      vm::wasm_allocator parent_memory, child_memory;
      backend_type parent{simple_code(), &parent_memory}, child{simple_code(), &child_memory};
   };
   // Parsing is owner-side and sequential here, just as in the original chain.
   // Only execution and timer callbacks overlap; no parser race is assumed away.
   std::array<std::unique_ptr<pair>, 4> modules;
   for (auto& value : modules) value = std::make_unique<pair>();
   std::array<std::array<timeout_result, 16>, 4> results;
   std::barrier start(4);
   std::vector<std::thread> threads;
   for (unsigned worker = 0; worker < 4; ++worker) threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 16; ++i)
         results[worker][i] = timeout_case(modules[worker]->parent, modules[worker]->child,
                                          i % 4, 2, true, i >= 8);
   });
   for (auto& thread : threads) thread.join();
   for (const auto& worker : results)
      for (unsigned i = 0; i < 16; ++i) check_timeout_result(worker[i], i % 4, i >= 8);
}

#if defined(__x86_64__) && !defined(PUREEOS_NATIVE_VM_SANITIZER)
BOOST_AUTO_TEST_CASE(jit_timeout_cleanup_restores_executable_pages_and_reuses_both_modules) {
   vm::wasm_allocator parent_memory, child_memory;
   vm::backend<std::nullptr_t, vm::jit> parent(simple_code(), &parent_memory), child(simple_code(), &child_memory);
   for (unsigned round = 0; round < 3; ++round)
      for (unsigned mask = 0; mask < 4; ++mask)
         for (bool threaded : {false, true})
            for (bool exception : {false, true}) {
               BOOST_TEST_CONTEXT("JIT round=" << round << " mask=" << mask << " threaded=" << threaded << " exception=" << exception) {
                  check_timeout_result(timeout_case(parent, child, mask, 2, threaded, exception, true), mask, exception);
               }
            }
}
#endif

BOOST_AUTO_TEST_CASE(native_allocator_constructor_failure_releases_mapping_and_allows_retry) {
   for (unsigned round = 0; round < 3; ++round)
      for (unsigned mode : {1u, 0u, 2u, 0u}) check_allocation_result(allocation_case(mode), mode);
}

BOOST_AUTO_TEST_CASE(native_allocator_injection_is_thread_local_and_never_leaks_a_failed_mapping) {
   std::array<std::array<allocation_result, 12>, 4> results;
   std::barrier start(4);
   std::vector<std::thread> threads;
   for (unsigned worker = 0; worker < 4; ++worker) threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 12; ++i) results[worker][i] = allocation_case(i % 3);
   });
   for (auto& thread : threads) thread.join();
   for (const auto& worker : results)
      for (unsigned i = 0; i < 12; ++i) check_allocation_result(worker[i], i % 3);
}

// Real synchronous faults are isolated in a bounded child process. No signal is
// sent to the test runner, nodeos, or user processes. ASan/TSan deliberately keep
// their own handlers; this OS signal-chain probe runs only in the normal target.
#ifndef PUREEOS_NATIVE_VM_SANITIZER
BOOST_AUTO_TEST_CASE(parent_memory_fault_after_child_return_is_caught_in_isolated_process) {
   vm::setup_signal_handler();
   const int parent_dumpable = prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L);
   BOOST_REQUIRE(parent_dumpable >= 0);
   for (unsigned mode = 0; mode < 3; ++mode) {
      const pid_t child_pid = fork();
      BOOST_REQUIRE(child_pid >= 0);
      if (child_pid == 0) {
         const rlimit no_core{0, 0};
         // RLIMIT_CORE alone is ignored by a pipe-style core collector. Disable
         // dumpability in this child too; do not alter any system setting.
         if (setrlimit(RLIMIT_CORE, &no_core) || prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L)) _exit(91);
         if (prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L) != 0) _exit(90);
         // setup_signal_handler's once flag was initialized before fork. Replace
         // inherited Boost handlers only in the child, then chain the defaults.
         struct sigaction defaults{}; defaults.sa_handler = SIG_DFL; sigemptyset(&defaults.sa_mask);
         if (sigaction(SIGSEGV, &defaults, nullptr) || sigaction(SIGFPE, &defaults, nullptr)) _exit(92);
         vm::setup_signal_handler_impl();
         code_fixture parent, nested;
         vm::invoke_with_signal_handler([&] {
            try {
               vm::invoke_with_signal_handler([&] { leave_frame(mode); }, [](int) { _exit(93); }, nested.code, &nested.memory);
            } catch (const std::exception&) {}
            const volatile unsigned char* guard = parent.memory.get_base_ptr<unsigned char>() + vm::page_size;
            const unsigned char value = *guard; (void)value;
            _exit(94);
         }, [](int sig) { _exit(sig == SIGSEGV ? 0 : 95); }, parent.code, &parent.memory);
         _exit(96);
      }
      int status = 0, wait_error = 0; bool reaped = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
         const auto observed = waitpid(child_pid, &status, WNOHANG);
         if (observed == child_pid) { reaped = true; break; }
         if (observed < 0 && errno != EINTR) { wait_error = errno; break; }
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      // With no wait error the unreaped child still owns this PID. On ECHILD
      // ownership is lost: never risk signalling a reused PID.
      if (!reaped && !wait_error) { kill(child_pid, SIGKILL); while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {} }
      BOOST_CHECK_EQUAL(wait_error, 0);
      BOOST_CHECK(reaped);
      BOOST_CHECK(!WCOREDUMP(status));
      BOOST_CHECK_MESSAGE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                          "child mode=" << mode << " wait status=" << status);
   }
   BOOST_CHECK_EQUAL(prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L), parent_dumpable);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
#endif // PUREEOS_NATIVE_VM_GUARD_FIXTURE
