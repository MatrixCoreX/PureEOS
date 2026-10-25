#pragma once

#include <eosio/chain/parallel_runtime_limits.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace eosio::chain::experimental {

// Owner-only, bounded local scheduling hints. Never shared with workers, saved
// in chain state, or used to validate/bill a transaction. Estimates may be wrong:
// the actual preparation deadline and canonical checks remain authoritative.
template<typename Key, size_t Capacity = 32>
class parallel_production_admission {
   static_assert(Capacity > 0);
   using clock = std::chrono::steady_clock;
   static constexpr auto retry_delay = std::chrono::seconds(1);
   struct entry {
      std::optional<Key> key;
      size_t target = 1;
      uint64_t wave_us = 0;
      unsigned successes = 0;
      std::optional<clock::time_point> retry_after = {};
      uint64_t receipt_cpu_us = 0;
      std::optional<clock::time_point> receipt_until = {};
   };
   // Most recently observed first; bounded moves avoid a wrapping age counter.
   std::array<entry, Capacity> entries_{};

   entry& observe_key(const Key& key, size_t workers, size_t configured) {
      size_t i = 0;
      while (i < Capacity && entries_[i].key != key) ++i;
      entry value = i < Capacity ? entries_[i] : entry{key, cold_batch(workers, configured)};
      if (i == Capacity) --i;
      for (; i > 0; --i) entries_[i] = std::move(entries_[i - 1]);
      entries_[0] = std::move(value);
      return entries_[0];
   }

public:
   enum class outcome { ready, deadline, other_failure };

   static size_t cold_batch(size_t workers, size_t configured) noexcept {
      return parallel_runtime_limits::valid_workers(workers) &&
             parallel_runtime_limits::valid_batch(configured) ? std::min(workers, configured) : 0;
   }

   size_t recommend(const Key& key, size_t workers, size_t configured, uint64_t budget_us,
                    clock::time_point now = clock::now()) const {
      return recommend_for_block(key, workers, configured, budget_us,
                                 std::numeric_limits<uint64_t>::max(), now);
   }

   size_t recommend_for_block(const Key& key, size_t workers, size_t configured, uint64_t budget_us,
                              uint64_t remaining_cpu_us, clock::time_point now = clock::now()) const {
      if (!budget_us || !remaining_cpu_us || !cold_batch(workers, configured)) return 0;
      for (const auto& e : entries_) if (e.key == key) {
         if (e.retry_after && now < *e.retry_after) return 0;
         size_t target = std::min(e.target, configured);
         if (e.wave_us) {
            // 25% estimation margin, without changing the actual deadline. A
            // one-item probe remains possible after a slow sample; otherwise a
            // transient load spike would permanently disable this workload.
            const auto safe_us = budget_us - budget_us / 4;
            const auto waves = std::min<uint64_t>(configured, safe_us / e.wave_us);
            target = std::min(target, std::max<size_t>(1, waves * workers));
         }
         // Predict capacity only from successful canonical receipts, never
         // speculative elapsed time. Leave 25% headroom without changing a
         // bill or block limit. Zero means original serial handling, not wait.
         // Expire hints so one expensive branch or a changed chain limit cannot
         // disable a shape forever when the bypass itself generates no sample.
         if (e.receipt_until && now < *e.receipt_until) {
            const auto estimated = e.receipt_cpu_us + (e.receipt_cpu_us + 3) / 4;
            target = std::min<uint64_t>(target, remaining_cpu_us / estimated);
         }
         return target;
      }
      return cold_batch(workers, configured);
   }

   void observe_receipt(const Key& key, size_t workers, size_t configured, uint64_t cpu_us,
                        clock::time_point now = clock::now()) {
      if (!cold_batch(workers, configured) || !cpu_us || cpu_us > std::numeric_limits<uint32_t>::max()) return;
      auto& e = observe_key(key, workers, configured);
      if (!e.receipt_until || now >= *e.receipt_until || cpu_us >= e.receipt_cpu_us)
         e.receipt_cpu_us = cpu_us;
      else e.receipt_cpu_us -= (e.receipt_cpu_us - cpu_us + 3) / 4;
      e.receipt_until = std::min(now, clock::time_point::max() - retry_delay) + retry_delay;
      // Receipt learning must not clear a preparation deadline backoff, grow a
      // batch, or pretend that a serially executed transaction was parallel.
   }

   void observe(const Key& key, size_t workers, size_t configured, size_t count,
                uint64_t elapsed_us, uint64_t budget_us, outcome result,
                clock::time_point now = clock::now()) {
      if (!cold_batch(workers, configured) || !count || count > configured || !budget_us) return;
      // Unsupported imports, traps, memory/capture limits, cancellation and
      // heterogeneous batches are not evidence that a smaller batch fits.
      if (result == outcome::other_failure) return;
      auto& e = observe_key(key, workers, configured);
      if (result == outcome::deadline) {
         e.target = std::max<size_t>(1, std::min(e.target, count) / 2);
         e.successes = 0;
         // Shrinking cannot help when even ONE attempt exceeds the cap. Do not
         // waste another 50ms before every serial transaction of this shape.
         // Briefly use the original serial path, then automatically probe again
         // (cold caches / background contention can recover). Local steady time
         // controls only optimization admission, never validity or CPU billing.
         if (count == 1)
            e.retry_after = std::min(now, clock::time_point::max() - retry_delay) + retry_delay;
         return;
      }
      e.retry_after.reset();
      const auto waves = (count + workers - 1) / workers;
      const uint64_t measured = std::clamp<uint64_t>(elapsed_us / waves, 1, 1'000'000);
      // React immediately to slower observations, recover gradually from cold
      // compilation/background load. Workload identity excludes data values,
      // so different branches can legitimately have different costs.
      e.wave_us = measured >= e.wave_us ? measured : e.wave_us - (e.wave_us - measured + 3) / 4;
      if (count >= e.target && elapsed_us <= budget_us - budget_us / 4) {
         if (++e.successes == 4) {
            e.target = std::min(configured, e.target + std::min(workers, e.target));
            e.successes = 0;
         }
      } else e.successes = 0;
   }
};

} // namespace eosio::chain::experimental
