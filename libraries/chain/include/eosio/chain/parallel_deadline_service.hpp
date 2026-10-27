#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace eosio::chain::experimental {

// Process-local watchdog infrastructure, not a consensus timer. One service
// thread watches a fixed number of live frames; no thread per action or guard.
// The pool must size this for its bounded worker count and synchronous depth.
// An expired callback MUST be noexcept, short, and non-reentrant into this
// service (e.g. set a live frame's atomic interrupt flag). It executes under
// the registry lock so destroying a guard synchronizes with an in-flight call.
// Every object referenced by a callback must outlive its guard.
// Shutdown interrupts still-live guards once; it never silently loses a timer.
class parallel_deadline_service {
public:
   using clock = std::chrono::steady_clock;
   struct cancellation {
      std::stop_token stop;
      std::shared_ptr<const std::atomic<uint64_t>> generation;
      uint64_t observed = 0;
      bool requested() const noexcept {
         return stop.stop_requested() ||
            (generation && generation->load(std::memory_order_relaxed) != observed);
      }
      bool pollable() const noexcept { return stop.stop_possible() || bool(generation); }
   };
private:
   struct entry {
      clock::time_point deadline;
      cancellation cancel;
      std::function<void()> callback;
      bool fired = false;
   };
   struct shared_state {
      explicit shared_state(size_t capacity) : slots(capacity) {
         if (!capacity) throw std::invalid_argument("empty parallel deadline service");
      }
      std::mutex mutex;
      std::condition_variable changed;
      std::vector<std::unique_ptr<entry>> slots;
      bool closing = false;
      // Protected by mutex, including the transition into condition-variable
      // sleep. A stale earlier wake is harmless; a later one is never allowed.
      clock::time_point next_wakeup = clock::time_point::max();
   };
public:
   class guard {
   public:
      guard(const guard&) = delete;
      guard& operator=(const guard&) = delete;
      guard(guard&&) = delete;
      guard& operator=(guard&&) = delete;
      ~guard() {
         // Move the callback and its captures out before destroying them.
         // Arbitrary capture destructors must not run under the registry lock.
         std::unique_ptr<entry> released;
         {
            std::lock_guard lock(state_->mutex);
            released = std::move(state_->slots[index_]);
         }
         // Removing a registration cannot introduce an earlier deadline or
         // cancellation poll. Keep a possible early wake instead of waking
         // the watchdog for every short-lived successful JIT frame.
      }
   private:
      friend class parallel_deadline_service;
      guard(std::shared_ptr<shared_state> state, size_t index) noexcept
         : state_(std::move(state)), index_(index) {}
      const std::shared_ptr<shared_state> state_;
      const size_t index_;
   };

   explicit parallel_deadline_service(size_t capacity)
      : state_(std::make_shared<shared_state>(capacity)), timer_([state = state_] { run(*state); }) {}
   parallel_deadline_service(const parallel_deadline_service&) = delete;
   parallel_deadline_service& operator=(const parallel_deadline_service&) = delete;
   ~parallel_deadline_service() {
      {
         std::lock_guard lock(state_->mutex);
         state_->closing = true;
      }
      state_->changed.notify_one();
      timer_.join();
   }

   template<typename Callback>
   [[nodiscard]] guard watch(clock::time_point deadline, cancellation cancel, Callback&& callback) {
      // Construct allocating user state before locking or publishing a slot.
      auto value = std::make_unique<entry>(entry{deadline, std::move(cancel), std::forward<Callback>(callback)});
      if (!value->callback) throw std::invalid_argument("empty parallel deadline callback");
      std::unique_lock lock(state_->mutex);
      if (state_->closing) throw std::logic_error("closed parallel deadline service");
      for (size_t index = 0; index < state_->slots.size(); ++index) {
         if (state_->slots[index]) continue;
         auto wake = value->deadline;
         if (value->cancel.pollable()) {
            const auto now = clock::now();
            wake = std::min(wake, value->cancel.requested() ? now : now + std::chrono::milliseconds(1));
         }
         const bool earlier = wake < state_->next_wakeup;
         state_->slots[index] = std::move(value);
         lock.unlock();
         // Existing registrations already arrange an equal or earlier wake.
         // New earlier deadlines/polls and shutdown still notify explicitly.
         if (earlier) state_->changed.notify_one();
         return guard(state_, index);
      }
      // This is a local admission failure, never an on-chain rejection.
      throw std::length_error("parallel deadline registry exhausted");
   }

private:
   static void fire(entry& value) noexcept {
      if (std::exchange(value.fired, true)) return;
      value.callback(); // throwing from a timeout callback is a programming error
   }
   static void run(shared_state& state) noexcept {
      std::unique_lock lock(state.mutex);
      for (;;) {
         if (state.closing) {
            for (auto& slot : state.slots) if (slot) fire(*slot);
            return;
         }
         const auto now = clock::now();
         auto next = clock::time_point::max();
         bool poll = false;
         for (auto& slot : state.slots) {
            if (!slot || slot->fired) continue;
            if (now >= slot->deadline || slot->cancel.requested()) { fire(*slot); continue; }
            next = std::min(next, slot->deadline);
            poll |= slot->cancel.pollable();
         }
         // Atomic generations/stop tokens may change without notifying us.
         // Poll at a bounded cadence, not a busy spin; no hard real-time claim.
         if (poll) next = std::min(next, now + std::chrono::milliseconds(1));
         state.next_wakeup = next;
         if (next == clock::time_point::max()) state.changed.wait(lock);
         else state.changed.wait_until(lock, next);
      }
   }
   const std::shared_ptr<shared_state> state_;
   std::thread timer_;
};

} // namespace eosio::chain::experimental
