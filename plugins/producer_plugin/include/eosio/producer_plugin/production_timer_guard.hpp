#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace eosio {

// A timer completion and its posted owner-thread callback are two different
// scheduling boundaries. Cancellation must be checked at BOTH. This token
// protects freshness only; it does not authorize concurrent chain access.
class production_timer_guard {
public:
   using token = uint64_t;
   token next() noexcept { return generation_.fetch_add(1, std::memory_order_relaxed) + 1; }
   void invalidate() noexcept { next(); }
   bool current(token value) const noexcept {
      return generation_.load(std::memory_order_relaxed) == value;
   }
   template<typename Callback>
   auto on_owner(token value, Callback&& callback) {
      return [this, value, callback = std::forward<Callback>(callback)]() mutable {
         if (current(value)) callback();
      };
   }
private:
   std::atomic<token> generation_{0};
};

} // namespace eosio
