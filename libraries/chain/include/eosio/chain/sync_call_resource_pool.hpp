#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace eosio::chain {

template <typename T>
class call_resource_pool {
public:
   // The factory transfers a fresh resource to the pool. It must not recursively
   // resize this same pool. Borrow/return during construction is allowed.
   using resource_creator = std::function<T* ()>;

   explicit call_resource_pool(resource_creator creator) {
      available.reserve(1);
      std::unique_ptr<T> value{creator()};
      if (!value) throw std::invalid_argument("sync call resource factory returned null");
      auto* pointer = value.get();
      auto [it, inserted] = owned.emplace(pointer, entry{std::move(value)});
      available.push_back(&it->second);
   }

   call_resource_pool(const call_resource_pool&) = delete;
   call_resource_pool& operator=(const call_resource_pool&) = delete;
   // Like the original pool, destruction requires all users to have stopped.
   // Ownership does not depend on whether every checkout was returned.
   ~call_resource_pool() = default;

   // This short critical section protects registration ONLY. The caller owns
   // the returned resource exclusively; WASM execution holds no pool lock.
   T* acquire() {
      std::lock_guard lock(borrow_mutex);
      if (available.empty()) throw std::runtime_error("sync call resource pool exhausted");
      auto* slot = available.back();
      available.pop_back();
      slot->checked_out = true;
      return slot->value.get();
   }

   // Safe in scope-exit cleanup: capacity is reserved BEFORE publication, so a
   // valid return never allocates. Reject misuse without changing ownership or
   // publishing the same mutable VM to two callers. Existing void callers may
   // ignore the result; composed pools can use it to locate the actual owner.
   bool release(T* value) noexcept {
      std::lock_guard lock(borrow_mutex);
      const auto it = owned.find(value);
      if (it == owned.end() || !it->second.checked_out || available.size() == available.capacity()) return false;
      it->second.checked_out = false;
      available.push_back(&it->second);
      return true;
   }

   // Normally called by the owner at startup / configuration changes. Serialize
   // dimensions as well, so simultaneous growth cannot publish a stale product.
   void set_num_threads(uint32_t count, resource_creator creator) {
      if (!count) throw std::invalid_argument("sync call resource thread count must be positive");
      std::lock_guard growth(growth_mutex);
      if (count > num_threads) resize(count, max_call_depth, creator);
   }

   void set_max_call_depth(uint32_t depth, resource_creator creator) {
      if (!depth) throw std::invalid_argument("sync call resource depth must be positive");
      std::lock_guard growth(growth_mutex);
      if (depth > max_call_depth) resize(num_threads, depth, creator);
   }

private:
   struct entry { std::unique_ptr<T> value; bool checked_out = false; };
   using owned_map = std::map<T*, entry, std::less<T*>>;

   // growth_mutex is held, but resource factories never hold borrow_mutex.
   void resize(uint32_t threads, uint32_t depth, const resource_creator& creator) {
      const uint64_t target = uint64_t{threads} * depth;
      // The original capacity arithmetic is uint32_t. Reject overflow rather
      // than wrap into a smaller pool or attempt an unbounded creation loop.
      if (target > std::numeric_limits<uint32_t>::max() || target > available.max_size() || target > owned.max_size())
         throw std::length_error("sync call resource capacity overflow");
      const auto count = static_cast<size_t>(target - uint64_t{num_threads} * max_call_depth);
      std::vector<entry*> additions;
      additions.reserve(count);
      {
         std::lock_guard lock(borrow_mutex);
         available.reserve(static_cast<size_t>(target));
      }
      owned_map staged;
      for (size_t i = 0; i < count; ++i) {
         auto* pointer = creator();
         if (!pointer) throw std::invalid_argument("sync call resource factory returned null");
         // A duplicate pointer is already owned: do not give it a second
         // unique_ptr, even on the failure path.
         {
            std::lock_guard lock(borrow_mutex);
            if (owned.contains(pointer)) throw std::invalid_argument("sync call resource factory reused an owned object");
         }
         if (staged.contains(pointer)) throw std::invalid_argument("sync call resource factory repeated an object");
         std::unique_ptr<T> value{pointer};
         auto [it, inserted] = staged.emplace(pointer, entry{std::move(value)});
         additions.push_back(&it->second);
      }
      // Same allocator, nonthrowing pointer comparator: transfer existing map
      // nodes without allocation; their entry addresses remain stable. All
      // potentially failing allocations happened before this publication.
      std::lock_guard lock(borrow_mutex);
      owned.merge(staged);
      for (auto* slot : additions) available.push_back(slot);
      num_threads = threads;
      max_call_depth = depth;
   }

   std::mutex borrow_mutex, growth_mutex;
   owned_map owned;
   std::vector<entry*> available;
   uint32_t num_threads = 1; // main thread
   uint32_t max_call_depth = 1; // before activation; the pool never shrinks
};

}  /// namespace eosio::chain
