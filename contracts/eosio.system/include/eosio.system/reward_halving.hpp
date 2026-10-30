#pragma once

#include <algorithm>
#include <cstdint>

namespace eosiosystem::rewards {
// Accrue block rewards at exactly 1% of issued supply per contract year.
// No additional savings issuance; future eras halve this coefficient.
inline constexpr double initial_continuous_rate = 0.01;
inline constexpr int64_t block_pay_factor = 10000; // 100% block pay; no vote pay
inline constexpr uint64_t year_usecs = uint64_t(364) * 86400 * 1000000;
// Same definition of four years as the candidate-capacity growth policy.
inline constexpr uint64_t halving_usecs = uint64_t(1461) * 86400 * 1000000;
inline constexpr uint32_t max_eras = 64;

inline uint32_t era_at(uint64_t origin, uint64_t now) {
   return now <= origin ? 0 : uint32_t(std::min<uint64_t>((now - origin) / halving_usecs, max_eras));
}

inline double rate_at(uint32_t era) {
   return era >= max_eras ? 0.0 : initial_continuous_rate / double(uint64_t{1} << era);
}

// Integrate each era separately. Applying only the rate at claim time would
// retroactively halve the pre-boundary interval. Work is bounded even after
// a long pause; after 64 eras the remaining lifetime amount is below one core
// token smallest unit for any legal asset supply (including compounding).
// Exact integer token units: 1/100 of supply per contract year, halved by era.
// Each segment contributes a quotient and remainder. Align remainders when
// crossing eras and round only once at the end of this accrual interval.
inline int64_t accrued(int64_t supply, uint64_t origin, uint64_t from, uint64_t to) {
   if (to <= from || to <= origin || supply <= 0) return 0;
   uint64_t cursor = std::max(from, origin) - origin;
   const uint64_t end = to - origin;
   uint64_t era = cursor / halving_usecs;
   if (era >= max_eras) return 0;
   using wide = unsigned __int128;
   const wide base_denominator = wide(year_usecs) * 100;
   wide whole = 0, remainder = 0;
   for (; cursor < end && era < max_eras; ++era) {
      const uint64_t boundary = std::min(end, (era + 1) * halving_usecs);
      const wide denominator = base_denominator << era;
      const wide numerator = wide(uint64_t(supply)) * (boundary - cursor);
      whole += numerator / denominator;
      remainder += numerator % denominator;
      whole += remainder / denominator;
      remainder %= denominator;
      cursor = boundary;
      // The next denominator doubles. Remainder remains below its denominator.
      remainder <<= 1;
   }
   return int64_t(whole);
}
} // namespace eosiosystem::rewards
