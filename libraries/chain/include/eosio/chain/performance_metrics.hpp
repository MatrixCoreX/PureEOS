#pragma once
#include <cstddef>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace eosio::chain::performance {

// Process-local, opt-in diagnostics. Never consensus state, deadlines, billing
// inputs or a reason to skip work. Labels and bucket counts have a fixed bound.
enum class stage : size_t {
   signature_queue, signature_recovery, transaction_queue, transaction_execute,
   onblock, vote_queue, bls_verify, recovery_lock, recovery_pack,
   recovery_file_sync, directory_sync, recovery_store, recovery_prune,
   finalizer_file_sync, finalizer_save, parallel_capture, parallel_batch_wait,
   parallel_compile, parallel_execute, parallel_adopt, parallel_fallback, parallel_conflict, count
};
inline constexpr std::array names{
   "signature_queue", "signature_recovery", "transaction_queue", "transaction_execute",
   "onblock", "vote_queue", "bls_verify", "recovery_lock", "recovery_pack",
   "recovery_file_sync", "directory_sync", "recovery_store", "recovery_prune",
   "finalizer_file_sync", "finalizer_save", "parallel_capture", "parallel_batch_wait",
   "parallel_compile", "parallel_execute", "parallel_adopt", "parallel_fallback", "parallel_conflict"
};
inline constexpr std::array<uint64_t, 13> boundaries_ns{
   1'000, 5'000, 10'000, 50'000, 100'000, 500'000, 1'000'000,
   5'000'000, 10'000'000, 50'000'000, 100'000'000, 500'000'000, 1'000'000'000
};
static_assert(names.size() == static_cast<size_t>(stage::count));

struct sample {
   std::array<uint64_t, boundaries_ns.size() + 1> buckets{}; // non-cumulative; last is +Inf
   uint64_t count = 0;
   uint64_t nanoseconds = 0;
   uint64_t maximum_ns = 0;
};

class registry {
   struct entry { mutable std::mutex mutex; sample value; };
   std::array<entry, names.size()> entries;
public:
   std::atomic<bool> enabled{false};
   void record(stage name, uint64_t ns) {
      auto& e = entries[static_cast<size_t>(name)];
      std::lock_guard lock(e.mutex);
      size_t bucket = 0;
      while (bucket < boundaries_ns.size() && ns > boundaries_ns[bucket]) ++bucket;
      ++e.value.buckets[bucket];
      ++e.value.count;
      e.value.nanoseconds += ns;
      if (ns > e.value.maximum_ns) e.value.maximum_ns = ns;
   }
   sample snapshot(stage name) const {
      const auto& e = entries[static_cast<size_t>(name)];
      std::lock_guard lock(e.mutex);
      return e.value;
   }
};

inline registry& metrics() { static registry instance; return instance; }
using clock = std::chrono::steady_clock;
inline clock::time_point mark(bool active = true) {
   return active && metrics().enabled.load(std::memory_order_relaxed) ? clock::now() : clock::time_point{};
}
inline void since(stage name, clock::time_point start) {
   if (start != clock::time_point{}) {
      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
      metrics().record(name, static_cast<uint64_t>(elapsed < 0 ? 0 : elapsed));
   }
}
class scope {
   stage name;
   clock::time_point start;
public:
   explicit scope(stage name, bool active = true) : name(name), start(mark(active)) {}
   scope(const scope&) = delete;
   scope& operator=(const scope&) = delete;
   ~scope() { since(name, start); }
};
} // namespace eosio::chain::performance
