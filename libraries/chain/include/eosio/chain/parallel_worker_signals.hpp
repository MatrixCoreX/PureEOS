#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace eosio::chain::experimental {

// One persistent waiter per logical worker. Readiness and publication remain
// protected by the pool mutex. Signals own no jobs, VM state or callbacks.
template<size_t Capacity>
class parallel_worker_signals {
   std::array<std::condition_variable, Capacity> signals_;
public:
   template<class Predicate>
   void wait(size_t worker, std::unique_lock<std::mutex>& lock, Predicate&& ready) {
      if (worker >= Capacity) throw std::out_of_range("parallel worker signal index");
      signals_[worker].wait(lock, std::forward<Predicate>(ready));
   }
   template<class Predicate>
   size_t notify_ready(size_t workers, Predicate&& ready) {
      if (workers > Capacity) throw std::out_of_range("parallel worker signal count");
      size_t notified = 0;
      for (size_t i = 0; i < workers; ++i) if (ready(i)) {
         signals_[i].notify_one();
         ++notified;
      }
      return notified;
   }
   void notify_shutdown() {
      for (auto& signal : signals_) signal.notify_all();
   }
};
} // namespace eosio::chain::experimental
