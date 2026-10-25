// Fault injection is linked only into this dedicated executable.
#ifdef PUREEOS_PARALLEL_JIT_FINALIZATION_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Parallel_JIT_Finalization
#include <boost/test/included/unit_test.hpp>
#include <eosio/chain/parallel_jit_backend.hpp>
#include <cerrno>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vm = eosio::vm;
using eosio::chain::experimental::parallel_jit;
namespace {
struct probe {
   bool active = false;
   int fail_protection = 0;
   unsigned writable = 0, executable = 0;
   void* failed = nullptr;
   void* latest_writable = nullptr;
};
thread_local probe observed;
}
extern "C" int __real_mprotect(void*, size_t, int) noexcept;
extern "C" int __wrap_mprotect(void* address, size_t length, int protection) noexcept {
   if (observed.active) {
      if (protection == (PROT_READ | PROT_WRITE)) {
         ++observed.writable; observed.latest_writable = address;
      }
      if (protection == PROT_EXEC) ++observed.executable;
      if (protection == observed.fail_protection) {
         observed.failed = address; errno = EACCES; return -1;
      }
   }
   return __real_mprotect(address, length, protection);
}
namespace {
vm::wasm_code simple_code() {
   // (module (func (export "f0")) (func (export "f1") (param i32)))
   return {0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x60,
           0x00,0x00,0x60,0x01,0x7f,0x00,0x03,0x03,0x02,0x00,0x01,0x07,
           0x0b,0x02,0x02,0x66,0x30,0x00,0x00,0x02,0x66,0x31,0x00,0x01,
           0x0a,0x07,0x02,0x02,0x00,0x0b,0x02,0x00,0x0b};
}
template<typename Engine> int failure_and_reuse(int protection) {
   observed = {};
   observed.active = true; observed.fail_protection = protection;
   bool caught = false;
   try { vm::backend<std::nullptr_t, Engine> rejected(simple_code(), nullptr); }
   catch (const vm::wasm_bad_alloc&) { caught = true; }
   catch (...) { return 31; }
   observed.active = false;
   if (!caught) return 32;
   if (!observed.failed || observed.writable != 1 || observed.executable != unsigned(protection == PROT_EXEC)) return 33;
   void* failed = observed.failed;
   observed = {};
   observed.active = true;
   vm::backend<std::nullptr_t, vm::jit> recovered(simple_code(), nullptr);
   observed.active = false;
   if (observed.writable != 1 || observed.executable != 1 || observed.latest_writable != failed) return 34;
   vm::wasm_allocator memory;
   recovered.set_wasm_allocator(&memory);
   recovered.initialize();
   recovered.call("", "f0");
   recovered.call("", "f1", uint32_t(19));
   return vm::signal_dest.load() ? 35 : 0;
}
template<typename Engine> int invalid_body_does_not_publish() {
   auto malformed = simple_code();
   malformed.at(malformed.size() - 8) = 8; // code section grows by one byte
   malformed.at(malformed.size() - 3) = 3; // second body grows by one byte
   malformed.insert(malformed.end() - 1, 0xff); // invalid opcode, but retain final end
   observed = {};
   observed.active = true; observed.fail_protection = PROT_READ | PROT_WRITE;
   bool caught = false;
   try { vm::backend<std::nullptr_t, Engine> rejected(std::move(malformed), nullptr); }
   catch (const vm::wasm_parse_exception&) { caught = true; }
   catch (...) { return 41; }
   observed.active = false;
   if (!caught || observed.writable || observed.executable) return 42;
   vm::wasm_allocator memory;
   vm::backend<std::nullptr_t, vm::jit> recovered(simple_code(), &memory);
   recovered.call("", "f0");
   return vm::signal_dest.load() ? 43 : 0;
}
int child_status(bool private_jit, int protection) {
   const pid_t child = fork();
   if (child < 0) throw std::runtime_error("fork failed");
   if (!child) {
      const rlimit no_core{0, 0};
      setrlimit(RLIMIT_CORE, &no_core);
      alarm(5); // bound ONLY this test-owned child, never an unrelated process
      try {
         if (!protection) _exit(private_jit ? invalid_body_does_not_publish<parallel_jit>() : invalid_body_does_not_publish<vm::jit>());
         _exit(private_jit ? failure_and_reuse<parallel_jit>(protection) : failure_and_reuse<vm::jit>(protection));
      }
      catch (...) { _exit(99); }
   }
   int status = 0;
   while (waitpid(child, &status, 0) == -1) {
      if (errno != EINTR) throw std::runtime_error("waitpid failed");
   }
   return status;
}
}
BOOST_AUTO_TEST_CASE(code_publication_failure_is_catchable_releases_storage_and_allows_healthy_reuse) {
   for (bool private_jit : {false, true}) for (int protection : {PROT_READ | PROT_WRITE, PROT_EXEC}) {
      BOOST_TEST_CONTEXT("private=" << private_jit << " mprotect=" << protection) {
         const int status = child_status(private_jit, protection);
         BOOST_CHECK_MESSAGE(WIFEXITED(status), "child terminated by signal; wait status=" << status);
         if (WIFEXITED(status)) BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
      }
   }
}
BOOST_AUTO_TEST_CASE(invalid_body_unwinding_does_not_finalize_or_hide_the_original_parse_error) {
   for (bool private_jit : {false, true}) {
      BOOST_TEST_CONTEXT("private=" << private_jit) {
         const int status = child_status(private_jit, 0);
         BOOST_CHECK_MESSAGE(WIFEXITED(status), "invalid-body child terminated by signal; wait status=" << status);
         if (WIFEXITED(status)) BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
      }
   }
}
#endif
