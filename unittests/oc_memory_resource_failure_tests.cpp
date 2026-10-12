// Only the dedicated executable supplies this fake intrinsic directory and
// linker syscall wrappers. The normal unit_test glob must not define them.
#ifdef PUREEOS_OC_MEMORY_FAILURE_FIXTURE
#include <eosio/chain/webassembly/eos-vm-oc/memory.hpp>
#include <eosio/chain/webassembly/eos-vm-oc/intrinsic.hpp>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>

namespace {
struct mapping_observation {
   bool active = false, intrinsic_failure = false;
   unsigned fail_at = 0, maps = 0, unmaps = 0;
   void* base = nullptr;
   size_t length = 0;
   void* released = nullptr;
   size_t released_length = 0;
   int unmap_result = -1;
} observation;
size_t open_fds() {
   size_t count = 0;
   for (const auto& ignored : std::filesystem::directory_iterator("/proc/self/fd")) { (void)ignored; ++count; }
   return count;
}
} // namespace

extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t) noexcept;
extern "C" int __real_munmap(void*, size_t) noexcept;
extern "C" void* __wrap_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) noexcept {
   if (observation.active && ++observation.maps == observation.fail_at) { errno = ENOMEM; return MAP_FAILED; }
   auto* result = __real_mmap(address, length, protection, flags, fd, offset);
   if (observation.active && observation.maps == 1) { observation.base = result; observation.length = length; }
   return result;
}
extern "C" int __wrap_munmap(void* address, size_t length) noexcept {
   const int result = __real_munmap(address, length);
   if (observation.active) {
      ++observation.unmaps; observation.released = address; observation.released_length = length;
      observation.unmap_result = result;
   }
   return result;
}

namespace eosio::chain::eosvmoc {
const intrinsic_map_t& get_intrinsic_map() {
   if (observation.intrinsic_failure) throw std::runtime_error("injected intrinsic setup failure");
   static const intrinsic_map_t empty;
   return empty;
}
} // namespace eosio::chain::eosvmoc

BOOST_AUTO_TEST_SUITE(oc_memory_resource_failure_tests)

BOOST_AUTO_TEST_CASE(initial_or_later_mapping_failure_closes_fd_and_releases_the_whole_reservation) {
   for (unsigned fail_at : {1u, 2u, 3u}) {
      const auto before = open_fds();
      observation = {}; observation.active = true; observation.fail_at = fail_at;
      bool caught = false;
      try { eosio::chain::eosvmoc::memory value(1); }
      catch (const fc::exception&) { caught = true; }
      observation.active = false;
      BOOST_CHECK(caught); BOOST_CHECK_EQUAL(observation.maps, fail_at);
      BOOST_CHECK_EQUAL(observation.unmaps, fail_at == 1 ? 0u : 1u);
      if (fail_at != 1) {
         BOOST_CHECK(observation.released == observation.base);
         BOOST_CHECK_EQUAL(observation.released_length, observation.length);
         BOOST_CHECK_EQUAL(observation.unmap_result, 0);
      }
      BOOST_CHECK_EQUAL(open_fds(), before);
   }
}

BOOST_AUTO_TEST_CASE(failure_after_all_slices_are_mapped_also_releases_the_reservation) {
   const auto before = open_fds();
   observation = {}; observation.active = true; observation.intrinsic_failure = true;
   bool caught = false;
   try { eosio::chain::eosvmoc::memory value(1); }
   catch (const std::runtime_error&) { caught = true; }
   observation.active = false; observation.intrinsic_failure = false;
   BOOST_CHECK(caught); BOOST_CHECK_EQUAL(observation.maps, 3u); BOOST_CHECK_EQUAL(observation.unmaps, 1u);
   BOOST_CHECK(observation.released == observation.base);
   BOOST_CHECK_EQUAL(observation.released_length, observation.length);
   BOOST_CHECK_EQUAL(observation.unmap_result, 0);
   BOOST_CHECK_EQUAL(open_fds(), before);
}

BOOST_AUTO_TEST_CASE(success_transfers_mapping_ownership_to_destructor_exactly_once) {
   const auto before = open_fds();
   observation = {}; observation.active = true;
   auto value = std::make_unique<eosio::chain::eosvmoc::memory>(1);
   const auto* actual = value->start_of_memory_slices();
   const size_t size = value->size_of_memory_slice_mapping();
   const unsigned early_unmaps = observation.unmaps;
   value.reset();
   observation.active = false;
   BOOST_CHECK_EQUAL(observation.maps, 3u); BOOST_CHECK_EQUAL(early_unmaps, 0u);
   BOOST_CHECK_EQUAL(observation.unmaps, 1u);
   BOOST_CHECK(actual == observation.released); BOOST_CHECK_EQUAL(size, observation.released_length);
   BOOST_CHECK_EQUAL(observation.unmap_result, 0);
   BOOST_CHECK_EQUAL(open_fds(), before);
}

BOOST_AUTO_TEST_SUITE_END()
#endif // dedicated failure-injection executable only
