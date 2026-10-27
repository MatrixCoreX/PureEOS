#pragma once
#include <algorithm>
#include <map>
#include <deque>
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
   using entry_map = std::map<Key, entry>;
   entry_map values;
   std::deque<typename entry_map::iterator> insertion_order;
   void check_owner() const {
      if (owner != std::this_thread::get_id()) throw std::logic_error("access hints used by non-owner");
   }
public:
   // The queue contains this map's stable iterators; owner-local caches must
   // never be copied/moved to another map or execution thread.
   parallel_access_hints() = default;
   parallel_access_hints(const parallel_access_hints&) = delete;
   parallel_access_hints& operator=(const parallel_access_hints&) = delete;
   parallel_access_hints(parallel_access_hints&&) = delete;
   parallel_access_hints& operator=(parallel_access_hints&&) = delete;
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
         // FIFO churn must not shift every retained key for each new shape.
         // If enqueue allocation fails below, remove the newly published map
         // entry so the bounded eviction queue and map remain consistent.
         if (values.size() == Entries) {
            const auto oldest = insertion_order.front();
            insertion_order.pop_front();
            values.erase(oldest);
         }
         found = values.emplace(key, entry{}).first;
         try { insertion_order.push_back(found); }
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
      auto queued = std::find(insertion_order.begin(), insertion_order.end(), found);
      if (queued == insertion_order.end()) throw std::logic_error("access hint eviction queue mismatch");
      insertion_order.erase(queued);
      values.erase(found);
      // A temporary preparation timeout must not permanently disable a hot
      // shape after cold JIT work. The NEXT batch may learn afresh; this grants
      // no extra retry/deadline to the batch which is already falling back.
   }
   size_t size() const { check_owner(); return values.size(); }
};
} // namespace eosio::chain::experimental
