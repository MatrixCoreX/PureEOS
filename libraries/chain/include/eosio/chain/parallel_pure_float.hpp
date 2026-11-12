#pragma once

#include <eosio/chain/parallel_pure_integer.hpp>
#include <cstring> // original softfloat.hpp uses unqualified memcpy helpers
#include <softfloat.hpp>
extern "C" {
#include <softfloat.h>
// Target-local symbol renames of the SAME original C sources. Never replace
// the canonical ___ symbols; sanitizer targets compile a separate copy too.
__int128 pureeos_private_fixtfti(float128_t);
__int128 pureeos_private_fixsfti(uint32_t);
__int128 pureeos_private_fixdfti(uint64_t);
unsigned __int128 pureeos_private_fixunstfti(float128_t);
unsigned __int128 pureeos_private_fixunssfti(uint32_t);
unsigned __int128 pureeos_private_fixunsdfti(uint64_t);
double pureeos_private_floattidf(__int128);
double pureeos_private_floatuntidf(unsigned __int128);
}

namespace eosio::chain::experimental {

enum class isolated_float_operation : uint8_t {
   addtf3, subtf3, multf3, divtf3, negtf2,
   extendsftf2, extenddftf2, trunctfdf2, trunctfsf2,
   fixtfsi, fixtfdi, fixtfti, fixunstfsi, fixunstfdi, fixunstfti,
   fixsfti, fixdfti, fixunssfti, fixunsdfti,
   floatsidf, floatsitf, floatditf, floatunsitf, floatunditf, floattidf, floatuntidf,
   eqtf2, netf2, getf2, gttf2, letf2, lttf2, cmptf2, unordtf2
};

// Actual production defaults from softfloat_state.c and 8086-SSE/specialize.h.
// TLS alone does NOT make an owner's non-default environment equivalent to a
// worker's. Do not reset anyone's environment: the private host and canonical
// adopter both reject a non-default mode, selecting unchanged serial execution.
// Sticky flags are not a rounding input and are never exposed by these hosts.
inline bool isolated_numeric_environment_compatible() noexcept {
   return softfloat_roundingMode == softfloat_round_near_even &&
          softfloat_detectTininess == softfloat_tininess_afterRounding &&
          extF80_roundingPrecision == 80;
}

inline float128_t isolated_quad(isolated_integer_bits bits) {
   return {{uint64_t(bits), uint64_t(bits >> 64)}};
}
inline isolated_integer_bits isolated_quad_bits(float128_t value) {
   return isolated_integer_words(value.v[0], value.v[1]);
}

// Pure numeric computation; low 32/64 bits encode scalar returns exactly.
// No native floating arithmetic or float->integer cast substitutes SoftFloat.
// An absent value discards the whole attempt, not a new consensus exception.
inline std::optional<isolated_integer_bits> isolated_float_compute(isolated_float_operation operation,
      isolated_integer_bits a, isolated_integer_bits b = 0) {
   if (!isolated_numeric_environment_compatible()) return {};
   using op = isolated_float_operation;
   using bits = isolated_integer_bits;
   const auto qa = isolated_quad(a), qb = isolated_quad(b);
   const auto signed32 = [](int32_t value) -> bits { return std::bit_cast<uint32_t>(value); };
   const auto signed64 = [](int64_t value) -> bits { return std::bit_cast<uint64_t>(value); };
   switch (operation) {
      case op::addtf3: return isolated_quad_bits(f128_add(qa, qb));
      case op::subtf3: return isolated_quad_bits(f128_sub(qa, qb));
      case op::multf3: return isolated_quad_bits(f128_mul(qa, qb));
      case op::divtf3: return isolated_quad_bits(f128_div(qa, qb));
      case op::negtf2: return a ^ (bits{1} << 127);
      case op::extendsftf2: return isolated_quad_bits(f32_to_f128({uint32_t(a)}));
      case op::extenddftf2: return isolated_quad_bits(f64_to_f128({uint64_t(a)}));
      case op::trunctfdf2: return f128_to_f64(qa).v;
      case op::trunctfsf2: return f128_to_f32(qa).v;
      // Preserve original rounding argument ZERO, not truncation inferred from
      // the name; exact=false still retains original saturation/NaN rules.
      case op::fixtfsi: return signed32(f128_to_i32(qa, 0, false));
      case op::fixtfdi: return signed64(f128_to_i64(qa, 0, false));
      case op::fixunstfsi: return uint32_t(f128_to_ui32(qa, 0, false));
      case op::fixunstfdi: return uint64_t(f128_to_ui64(qa, 0, false));
      case op::fixtfti:
         if (((qa.v[1] >> 48) & 0x7fff) == 16383 + 127) return {};
         return std::bit_cast<bits>(pureeos_private_fixtfti(qa));
      case op::fixsfti:
         if (((uint32_t(a) >> 23) & 0xff) == 127 + 127) return {};
         return std::bit_cast<bits>(pureeos_private_fixsfti(uint32_t(a)));
      case op::fixdfti:
         if (((uint64_t(a) >> 52) & 0x7ff) == 1023 + 127) return {};
         return std::bit_cast<bits>(pureeos_private_fixdfti(uint64_t(a)));
      case op::fixunstfti: return pureeos_private_fixunstfti(qa);
      case op::fixunssfti: return pureeos_private_fixunssfti(uint32_t(a));
      case op::fixunsdfti: return pureeos_private_fixunsdfti(uint64_t(a));
      case op::floatsidf: return i32_to_f64(std::bit_cast<int32_t>(uint32_t(a))).v;
      case op::floatsitf: return isolated_quad_bits(i32_to_f128(std::bit_cast<int32_t>(uint32_t(a))));
      case op::floatditf: return isolated_quad_bits(i64_to_f128(std::bit_cast<int64_t>(uint64_t(a))));
      case op::floatunsitf: return isolated_quad_bits(ui32_to_f128(uint32_t(a)));
      case op::floatunditf: return isolated_quad_bits(ui64_to_f128(uint64_t(a)));
      case op::floattidf:
         if (a == (bits{1} << 127)) return {}; // original C abs(INT128_MIN) UB
         return std::bit_cast<uint64_t>(pureeos_private_floattidf(std::bit_cast<__int128>(a)));
      case op::floatuntidf: return std::bit_cast<uint64_t>(pureeos_private_floatuntidf(a));
      case op::eqtf2: case op::netf2: case op::getf2: case op::gttf2:
      case op::letf2: case op::lttf2: case op::cmptf2: case op::unordtf2: {
         const bool unordered = f128_is_nan(qa) || f128_is_nan(qb);
         if (operation == op::unordtf2) return uint32_t(unordered);
         if (unordered) {
            if (operation == op::getf2) return signed32(-1);
            if (operation == op::gttf2 || operation == op::lttf2) return 0;
            return 1;
         }
         return signed32(f128_lt(qa, qb) ? -1 : f128_eq(qa, qb) ? 0 : 1);
      }
   }
   return {};
}

} // namespace eosio::chain::experimental
