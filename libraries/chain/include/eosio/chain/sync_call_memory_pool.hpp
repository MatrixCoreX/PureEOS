#pragma once

#include <eosio/chain/sync_call_resource_pool.hpp>
#include <algorithm>
#include <array>
#include <atomic>

namespace eosio::chain {

// The actual OC depth/slice allocation algorithm, separated from mmap/LLVM so
// ownership, growth and concurrency can be tested with ordinary resources too.
// FirstSlices shallow depths have separate pools. All deeper calls share the
// last (one-slice) pool. Factories transfer fresh objects; no recursive growth.
template <typename T, uint32_t FirstSlices = 5>
class depth_resource_pools {
   static_assert(FirstSlices > 0);
   using pool = call_resource_pool<T>;
public:
   using resource_creator = std::function<T*(uint32_t slices)>;
   explicit depth_resource_pools(resource_creator creator) : create(std::move(creator)) {
      pools[0] = make_pool(FirstSlices);
   }

   T* acquire(uint32_t depth) {
      if (!depth) throw std::invalid_argument("sync call memory depth must be positive");
      const auto count = published.load(std::memory_order_acquire);
      return pools[std::min(depth, count) - 1]->acquire();
   }

   bool release(uint32_t depth, T* value) noexcept {
      if (!depth || !value) return false;
      // Return to the OWNER, not a pool recomputed from mutable layout or the
      // supplied depth. At most FirstSlices nonallocating ownership lookups.
      const auto count = published.load(std::memory_order_acquire);
      for (uint32_t i = 0; i < count; ++i) if (pools[i]->release(value)) return true;
      return false;
   }

   void set_num_threads(uint32_t threads) {
      if (!threads) throw std::invalid_argument("sync call memory threads must be positive");
      std::lock_guard growth(growth_mutex);
      if (threads <= num_threads) return;
      check_capacity(threads, max_depth);
      const auto count = published.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < count; ++i)
         pools[i]->set_num_threads(threads, factory(FirstSlices - i));
      // If an earlier pool grew before a later factory failed, it safely keeps
      // extra capacity. Logical dimensions remain old; a retry completes all
      // pools. Shrinking after a failed configuration is intentionally avoided.
      num_threads = threads;
   }

   void set_max_call_depth(uint32_t depth) {
      if (!depth) throw std::invalid_argument("sync call memory depth must be positive");
      std::lock_guard growth(growth_mutex);
      if (depth <= max_depth) return;
      check_capacity(num_threads, depth);
      const auto old_count = published.load(std::memory_order_relaxed);
      const auto new_count = std::min(depth, FirstSlices);
      std::array<std::unique_ptr<pool>, FirstSlices> staged;
      for (uint32_t i = old_count; i < new_count; ++i) {
         staged[i] = make_pool(FirstSlices - i);
         // Configure the NEW pool, not pools[i - old_count].
         staged[i]->set_num_threads(num_threads, factory(FirstSlices - i));
      }
      if (depth > FirstSlices) {
         auto& last = old_count == FirstSlices ? pools[FirstSlices - 1] : staged[FirstSlices - 1];
         last->set_max_call_depth(depth - FirstSlices + 1, factory(1));
      }
      // Existing slots never move/change. Initialize all new slots before the
      // release-store publishes their count to concurrent borrowers/returners.
      for (uint32_t i = old_count; i < new_count; ++i) pools[i] = std::move(staged[i]);
      published.store(new_count, std::memory_order_release);
      max_depth = depth;
   }

private:
   static void check_capacity(uint32_t threads, uint32_t depth) {
      if (uint64_t{threads} * depth > std::numeric_limits<uint32_t>::max())
         throw std::length_error("sync call memory capacity overflow");
   }
   typename pool::resource_creator factory(uint32_t slices) {
      return [this, slices] { return create(slices); };
   }
   std::unique_ptr<pool> make_pool(uint32_t slices) {
      return std::make_unique<pool>(factory(slices));
   }

   resource_creator create;
   std::mutex growth_mutex;
   std::array<std::unique_ptr<pool>, FirstSlices> pools;
   std::atomic<uint32_t> published{1};
   uint32_t num_threads = 1, max_depth = 1;
};

} // namespace eosio::chain
