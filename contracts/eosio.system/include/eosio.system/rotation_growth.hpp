#pragma once
#include <cstdint>
#include <limits>

namespace eosiosystem::rotation {
constexpr uint64_t growth_period_slots = uint64_t(1461) * 86400 * 2;
constexpr uint32_t rank_work_budget = 16;
constexpr uint32_t queue_work_budget = 16;
constexpr uint32_t window_settle_budget = 8;
// A full 21-member schedule round without chain/finality progress is a
// global interruption, before three synthetic personal misses could accrue.
constexpr uint32_t pause_guard_slots = 21 * 12;
constexpr uint32_t pending_window_limit = 192;
inline uint32_t candidate_capacity(uint64_t origin, uint64_t slot) {
   const auto elapsed = slot > origin ? slot - origin : 0;
   const auto epoch = elapsed / growth_period_slots;
   // Saturation prevents integer wrap; today's block-slot horizon arrives first.
   if (epoch >= 25) return std::numeric_limits<uint32_t>::max();
   const uint64_t base = uint64_t(210) << epoch;
   const auto capacity = base + base * (elapsed % growth_period_slots) / growth_period_slots;
   return capacity > std::numeric_limits<uint32_t>::max()
      ? std::numeric_limits<uint32_t>::max() : uint32_t(capacity);
}
}
