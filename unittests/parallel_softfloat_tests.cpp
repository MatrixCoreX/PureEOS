extern "C" {
#include <softfloat.h>
}
#include <boost/test/unit_test.hpp>
#include <array>
#include <barrier>
#include <future>
#include <random>
#include <vector>

namespace {
struct saved_float_state {
   uint_fast8_t rounding = softfloat_roundingMode;
   uint_fast8_t tininess = softfloat_detectTininess;
   uint_fast8_t flags = softfloat_exceptionFlags;
   uint_fast8_t precision = extF80_roundingPrecision;
   ~saved_float_state() {
      softfloat_roundingMode = rounding;
      softfloat_detectTininess = tininess;
      softfloat_exceptionFlags = flags;
      extF80_roundingPrecision = precision;
   }
};

std::array<uintptr_t, 4> float_addresses() {
   return {reinterpret_cast<uintptr_t>(&softfloat_roundingMode), reinterpret_cast<uintptr_t>(&softfloat_detectTininess),
           reinterpret_cast<uintptr_t>(&softfloat_exceptionFlags), reinterpret_cast<uintptr_t>(&extF80_roundingPrecision)};
}

bool has_separate_state() {
   const auto owner = float_addresses();
   const auto other = std::async(std::launch::async, float_addresses).get();
   for (size_t i = 0; i < owner.size(); ++i) if (owner[i] == other[i]) return false;
   return true;
}

// Stable bit-pattern corpus, not native hardware floating-point as an oracle.
// Save the digest from the pre-TLS library before making the build change.
uint64_t serial_fingerprint() {
   saved_float_state restore;
   softfloat_roundingMode = softfloat_round_near_even;
   softfloat_detectTininess = softfloat_tininess_afterRounding;
   extF80_roundingPrecision = 80;
   std::mt19937_64 random(0x5058305f464c4f41ull);
   uint64_t digest = 14695981039346656037ull;
   auto mix = [&](uint64_t value) {
      for (unsigned byte = 0; byte < 8; ++byte) {
         digest ^= (value >> (8 * byte)) & 255;
         digest *= 1099511628211ull;
      }
   };
   for (unsigned i = 0; i < 10000; ++i) {
      softfloat_exceptionFlags = 0;
      const float32_t a{static_cast<uint32_t>(random())}, b{static_cast<uint32_t>(random())};
      const float64_t c{random()}, d{random()};
      mix(f32_add(a, b).v); mix(f32_sub(a, b).v); mix(f32_mul(a, b).v); mix(f32_div(a, b).v); mix(f32_sqrt(a).v);
      mix(f64_add(c, d).v); mix(f64_sub(c, d).v); mix(f64_mul(c, d).v); mix(f64_div(c, d).v); mix(f64_sqrt(c).v);
      mix(f32_to_f64(a).v); mix(f64_to_f32(c).v);
      const float128_t e = f64_to_f128(c), f = f64_to_f128(d);
      const float128_t g = f128_add(e, f), h = f128_mul(e, f);
      mix(g.v[0]); mix(g.v[1]); mix(h.v[0]); mix(h.v[1]);
      mix(softfloat_exceptionFlags);
   }
   return digest;
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_softfloat_tests)

BOOST_AUTO_TEST_CASE(serial_results_match_saved_baseline) {
   const auto digest = serial_fingerprint();
   BOOST_TEST_MESSAGE("pre-TLS-compatible SoftFloat corpus digest: " << digest);
   // Captured from unchanged upstream 19cbea6, before the TLS build definition.
   // See build/local-tests/parallel-overlay-20261011-q42k0b/softfloat-serial-before.log.
   BOOST_CHECK_EQUAL(digest, 4281633724421848196ull);
}

BOOST_AUTO_TEST_CASE(known_ieee_results_and_default_state) {
   saved_float_state restore;
   softfloat_roundingMode = softfloat_round_near_even;
   softfloat_exceptionFlags = 0;
   BOOST_CHECK_EQUAL(f32_add({0x3f800000}, {0x40000000}).v, 0x40400000u);
   BOOST_CHECK_EQUAL(f32_div({0x3f800000}, {0x40400000}).v, 0x3eaaaaabu);
   BOOST_CHECK_EQUAL(f64_div({0x3ff0000000000000ull}, {0x4008000000000000ull}).v, 0x3fd5555555555555ull);
   BOOST_CHECK_EQUAL(f32_mul({0x80000000}, {0x40000000}).v, 0x80000000u);
   softfloat_exceptionFlags = 0;
   (void)f32_div({0}, {0});
   BOOST_CHECK_EQUAL(softfloat_exceptionFlags, softfloat_flag_invalid);
}

BOOST_AUTO_TEST_CASE(all_four_state_variables_are_thread_local) {
   const auto owner = float_addresses();
   std::barrier start(5);
   std::vector<std::future<std::array<uintptr_t, 4>>> workers;
   for (unsigned i = 0; i < 4; ++i) workers.push_back(std::async(std::launch::async, [&] {
      const auto pointers = float_addresses();
      start.arrive_and_wait();
      return pointers;
   }));
   start.arrive_and_wait();
   std::vector<std::array<uintptr_t, 4>> addresses{owner};
   for (auto& worker : workers) addresses.push_back(worker.get());
   for (size_t i = 0; i < addresses.size(); ++i)
      for (size_t j = i + 1; j < addresses.size(); ++j)
         for (size_t k = 0; k < owner.size(); ++k) BOOST_CHECK_NE(addresses[i][k], addresses[j][k]);
}

BOOST_AUTO_TEST_CASE(rounding_and_exception_flags_do_not_cross_threads) {
   // Fail without starting concurrent writes when run against the old library.
   BOOST_REQUIRE(has_separate_state());
   saved_float_state restore;
   softfloat_exceptionFlags = softfloat_flag_overflow;
   std::barrier start(5);
   std::vector<std::future<std::array<uint64_t, 4>>> workers;
   for (unsigned i = 0; i < 4; ++i) workers.push_back(std::async(std::launch::async, [&, i] {
      saved_float_state restore_worker;
      const auto initial = softfloat_exceptionFlags;
      softfloat_roundingMode = i % 2 ? softfloat_round_max : softfloat_round_min;
      softfloat_detectTininess = i % 2;
      extF80_roundingPrecision = i % 2 ? 32 : 64;
      softfloat_exceptionFlags = 0;
      start.arrive_and_wait();
      uint64_t bad = 0;
      for (unsigned pass = 0; pass < 10000; ++pass) {
         if (f32_add({0x3f800000}, {0x33800000}).v != (i % 2 ? 0x3f800001u : 0x3f800000u)) ++bad;
         if (softfloat_detectTininess != i % 2 || extF80_roundingPrecision != (i % 2 ? 32 : 64)) ++bad;
      }
      return std::array<uint64_t, 4>{initial, bad, softfloat_exceptionFlags, softfloat_roundingMode};
   }));
   start.arrive_and_wait();
   for (unsigned i = 0; i < workers.size(); ++i) {
      const auto result = workers[i].get();
      BOOST_CHECK_EQUAL(result[0], 0u);
      BOOST_CHECK_EQUAL(result[1], 0u);
      BOOST_CHECK_EQUAL(result[2], softfloat_flag_inexact);
      BOOST_CHECK_EQUAL(result[3], i % 2 ? softfloat_round_max : softfloat_round_min);
   }
   BOOST_CHECK_EQUAL(softfloat_exceptionFlags, softfloat_flag_overflow);
}

BOOST_AUTO_TEST_CASE(fresh_workers_preserve_default_initialization) {
   BOOST_REQUIRE(has_separate_state());
   saved_float_state restore;
   softfloat_roundingMode = softfloat_round_max;
   softfloat_detectTininess = softfloat_tininess_beforeRounding;
   softfloat_exceptionFlags = softfloat_flag_invalid;
   extF80_roundingPrecision = 32;
   for (unsigned i = 0; i < 4; ++i) {
      const auto initial = std::async(std::launch::async, [] {
         return std::array<uint_fast8_t, 4>{softfloat_roundingMode, softfloat_detectTininess,
                                          softfloat_exceptionFlags, extF80_roundingPrecision};
      }).get();
      BOOST_CHECK_EQUAL(initial[0], softfloat_round_near_even);
      BOOST_CHECK_EQUAL(initial[1], softfloat_tininess_afterRounding);
      BOOST_CHECK_EQUAL(initial[2], 0u);
      BOOST_CHECK_EQUAL(initial[3], 80u);
   }
}

BOOST_AUTO_TEST_CASE(one_two_four_workers_match_serial_bit_patterns) {
   BOOST_REQUIRE(has_separate_state());
   const auto expected = serial_fingerprint();
   for (unsigned count : {1, 2, 4}) {
      std::barrier start(count);
      std::vector<std::future<uint64_t>> workers;
      for (unsigned i = 0; i < count; ++i) workers.push_back(std::async(std::launch::async, [&] {
         start.arrive_and_wait();
         return serial_fingerprint();
      }));
      for (auto& worker : workers) BOOST_CHECK_EQUAL(worker.get(), expected);
   }
}

BOOST_AUTO_TEST_SUITE_END()
