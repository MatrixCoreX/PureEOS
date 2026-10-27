#pragma once
#include <algorithm>
#include <map>
#include <thread>
#include <vector>
#include <stdexcept>

namespace eosio::chain::experimental {

// Owner-local, bounded PATHS only. Values, existence, ranges and account/code
// eligibility MUST be recaptured from the current prefix. A hint is neither a
// read proof nor an authorization. Keys bind code and transaction shape, not
// payload values: different nonces must not defeat path learning. Wrong paths
// are harmless speculative prefetch; actual state/dependencies stay checked.
// FIFO bounds avoid an unbounded recency clock and work with nonnumeric keys.
template<typename Key, typename Request, size_t Entries = 256, size_t Paths = 32>
class parallel_access_hints {
   static_assert(Entries > 0 && Paths > 0);
   struct entry { std::vector<Request> paths; bool saturated = false; };
   const std::thread::id owner = std::this_thread::get_id();
   std::map<Key, entry> values;
   std::vector<Key> insertion_order;
   void check_owner() const {
      if (owner != std::this_thread::get_id()) throw std::logic_error("access hints used by non-owner");
   }
public:
   // The visitor must finish before any learn/invalidate call. No worker may
   // retain these references. This deliberately returns no container pointer.
   template<typename Visitor> void visit(const Key& key, Visitor&& visitor) const {
      check_owner();
      const auto found = values.find(key);
      if (found == values.end() || found->second.saturated) return;
      for (const auto& path : found->second.paths) visitor(path);
   }
   template<typename Equal> void learn(const Key& key, const Request& request, Equal&& equal) {
      check_owner();
      auto found = values.find(key);
      if (found == values.end()) {
         // Reserve insertion bookkeeping first. Allocation failure cannot
         // publish an entry absent from the bounded eviction queue.
         insertion_order.reserve(Entries);
         if (values.size() == Entries) {
            values.erase(insertion_order.front());
            insertion_order.erase(insertion_order.begin());
         }
         found = values.emplace(key, entry{}).first;
         try { insertion_order.push_back(key); }
         catch (...) { values.erase(found); throw; }
      }
      auto& item = found->second;
      if (item.saturated) return;
      for (const auto& path : item.paths) if (equal(path, request)) return;
      if (item.paths.size() == Paths) {
         // High-entropy/dynamic access shapes get the original discovery path,
         // rather than forever replaying a misleading partial prefetch list.
         item.paths.clear(); item.saturated = true; return;
      }
      item.paths.push_back(request);
   }
   void invalidate(const Key& key) {
      check_owner();
      auto found = values.find(key);
      if (found == values.end()) return;
      const auto compare = values.key_comp();
      auto queued = std::find_if(insertion_order.begin(), insertion_order.end(), [&](const auto& item) {
         return !compare(item, key) && !compare(key, item);
      });
      if (queued == insertion_order.end()) throw std::logic_error("access hint eviction queue mismatch");
      values.erase(found);
      insertion_order.erase(queued);
      // A temporary preparation timeout must not permanently disable a hot
      // shape after cold JIT work. The NEXT batch may learn afresh; this grants
      // no extra retry/deadline to the batch which is already falling back.
   }
   size_t size() const { check_owner(); return values.size(); }
};
} // namespace eosio::chain::experimental
