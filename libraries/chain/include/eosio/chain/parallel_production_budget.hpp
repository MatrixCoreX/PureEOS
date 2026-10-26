#pragma once
#include <eosio/chain/parallel_runtime_limits.hpp>
#include <algorithm>
#include <cstdint>

namespace eosio::chain::experimental {
// Local scheduling only. The original execution limit and real block deadline
// remain authoritative. The caller supplies its ACTUAL subjective time limit.
inline int64_t production_preparation_budget_us(int64_t remaining_us,
      uint64_t chain_transaction_cpu_us,uint64_t block_cpu_remaining_us,
      int64_t subjective_transaction_us) noexcept {
   const auto serial_limit=std::min<uint64_t>({chain_transaction_cpu_us,
      block_cpu_remaining_us,static_cast<uint64_t>(std::max<int64_t>(0,subjective_transaction_us))});
   const auto reserve=static_cast<int64_t>(serial_limit)+5'000;
   if(remaining_us<=reserve) return 0;
   return std::min({remaining_us/4,
      std::chrono::duration_cast<std::chrono::microseconds>(parallel_runtime_limits::production_timeout).count(),
      remaining_us-reserve});
}
} // namespace eosio::chain::experimental
