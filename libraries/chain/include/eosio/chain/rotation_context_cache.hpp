#pragma once

#include <eosio/chain/types.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace eosio::chain {

struct rotation_context_cache_metrics {
   uint64_t hits = 0;
   uint64_t builds = 0;
   uint64_t build_nanoseconds = 0;
   uint64_t bytes = 0;
};

// One entry per controller, for wire version 1 only. The key is the executed
// parent's full ID, NOT its height, the speculative block or the local LIB.
// Readers retain immutable bytes across replacement. Nothing is persisted and
// failed builders never publish a new key. A read window may have concurrent
// readers; the controller's existing read/write-window rules protect head access.
class rotation_context_cache {
public:
   using bytes_ptr = std::shared_ptr<const std::vector<char>>;

   template<typename Builder>
   bytes_ptr get(const block_id_type& parent, Builder&& build) {
      std::lock_guard guard(mutex);
      if (bytes && parent == parent_id) {
         ++metrics.hits;
         return bytes;
      }
      const auto start = std::chrono::steady_clock::now();
      auto next = std::make_shared<const std::vector<char>>(build());
      metrics.build_nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
         std::chrono::steady_clock::now() - start).count();
      ++metrics.builds;
      metrics.bytes = next->size();
      parent_id = parent;
      bytes = std::move(next);
      return bytes;
   }

   rotation_context_cache_metrics snapshot() const {
      std::lock_guard guard(mutex);
      return metrics;
   }

private:
   mutable std::mutex mutex;
   block_id_type parent_id;
   bytes_ptr bytes;
   rotation_context_cache_metrics metrics;
};
} // namespace eosio::chain
