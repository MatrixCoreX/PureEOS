#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace eosio::detail {

// Local scheduling only: never changes acceptance, ordering, billing or a
// block deadline. Arm once for the FIRST arrival; later arrivals must not
// extend the wait. Near a block boundary use the original immediate post.
struct incoming_microbatch_policy {
   static constexpr int64_t max_wait_us = 2'000;
   static constexpr int64_t deadline_reserve_us = 5'000;
   static constexpr int64_t delay_us(int64_t remaining_us, bool parallel_available = true) noexcept {
      return !parallel_available || remaining_us <= deadline_reserve_us ? 0 :
         std::min(max_wait_us, remaining_us - deadline_reserve_us);
   }
   static constexpr size_t target(size_t configured) noexcept {
      return std::clamp<size_t>(configured, 2, 64);
   }
};

// Owner-thread-only holding area, NOT a transaction acceptance/priority queue.
// A refusal means flush/bypass the optimization, never reject the transaction.
// No timer, lock, chain state, deduplication or API/P2P reordering lives in the
// buffer. The owner may bound a short coalescing wait using the policy above.
template<typename T, size_t Capacity = 64>
class incoming_transaction_batch {
   static_assert(Capacity > 0 && std::is_nothrow_move_constructible_v<T>);
   std::vector<T> entries_;
   size_t bytes_ = 0;
   size_t limit_;
public:
   explicit incoming_transaction_batch(size_t limit) : limit_(limit) {}
   bool empty() const noexcept { return entries_.empty(); }
   size_t size() const noexcept { return entries_.size(); }
   size_t bytes() const noexcept { return bytes_; }
   static size_t preparation_count(size_t available, size_t recommended) noexcept {
      // A lone arrival stays on the original path. With a real backlog, a
      // recommendation of ONE is a recovery probe, not a disabled workload.
      // Suppressing that probe would prevent the admission policy from ever
      // observing a success and growing again after a temporary slowdown.
      return available > 1 ? std::min({available, recommended, Capacity}) : 0;
   }
   bool can_fit(size_t bytes) const noexcept {
      return entries_.size() < Capacity && bytes <= limit_ && bytes_ <= limit_ - bytes;
   }
   bool push(T&& entry, size_t bytes) {
      if (!can_fit(bytes)) return false;
      // Allocation happens before a noexcept move. Failure cannot change the
      // accounting or consume the caller's callback ownership.
      entries_.push_back(std::move(entry));
      bytes_ += bytes;
      return true;
   }
   std::vector<T> take() noexcept {
      std::vector<T> result;
      result.swap(entries_);
      bytes_ = 0;
      return result;
   }
   void clear() noexcept { entries_.clear(); bytes_ = 0; }
};

// Original handling can retain a callback in the retry queue or catch an
// exception thrown by the callback itself. All aliases consume ONE delivery,
// before calling user code, including reentrant/throwing callbacks. Owner only.
template<typename Result>
std::function<void(const Result&)> incoming_once(std::function<void(const Result&)> next) {
   auto delivery = std::make_shared<std::function<void(const Result&)>>(std::move(next));
   return [delivery = std::move(delivery)](const Result& result) {
      auto callback = std::move(*delivery);
      *delivery = {};
      if (callback) callback(result);
   };
}

} // namespace eosio::detail
