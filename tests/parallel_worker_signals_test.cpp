#include <eosio/chain/parallel_worker_signals.hpp>
#include <eosio/chain/parallel_runtime_limits.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using eosio::chain::experimental::parallel_worker_signals;

static size_t assertions = 0;
static void require(bool ok) {
   ++assertions;
   if (!ok) throw std::runtime_error("worker signal assertion failed");
}

static void run(size_t count) {
   parallel_worker_signals<eosio::chain::experimental::parallel_runtime_limits::max_workers> signals;
   std::mutex mutex;
   std::condition_variable done;
   std::vector<size_t> jobs(count), consumed(count);
   bool stopping = false;
   size_t remaining = 0;
   std::vector<std::thread> threads;
   auto stop = [&] {
      { std::lock_guard lock(mutex); stopping = true; }
      signals.notify_shutdown();
      for (auto& thread : threads) if (thread.joinable()) thread.join();
   };
   try {
      for (size_t i = 0; i < count; ++i) threads.emplace_back([&, i] {
         std::unique_lock lock(mutex);
         while (true) {
            signals.wait(i, lock, [&] { return stopping || jobs[i] != 0; });
            if (stopping) return;
            --jobs[i]; ++consumed[i]; --remaining;
            done.notify_one();
         }
      });
      for (size_t round = 0; round < 128; ++round) {
         std::unique_lock lock(mutex);
         size_t selected = 0;
         const auto before = consumed;
         for (size_t i = 0; i < count; ++i)
            if ((i + round) % 3 == 0 || round % 7 == 0) { jobs[i] = 1; ++selected; }
         remaining = selected;
         require(signals.notify_ready(count, [&](size_t i) { return jobs[i] != 0; }) == selected);
         require(done.wait_for(lock, std::chrono::seconds(2), [&]{ return remaining == 0; }));
         for (size_t i = 0; i < count; ++i)
            require(consumed[i] == before[i] + size_t((i + round) % 3 == 0 || round % 7 == 0));
         require(signals.notify_ready(count, [](size_t) { return false; }) == 0);
      }
      stop();
   } catch (...) { stop(); throw; }
}

int main() {
   try {
      for (size_t count : {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u}) run(count);
      // Partially constructed pools and shutdown before any work.
      parallel_worker_signals<eosio::chain::experimental::parallel_runtime_limits::max_workers> empty;
      empty.notify_shutdown();
      require(empty.notify_ready(0, [](size_t) { return true; }) == 0);
      bool rejected = false;
      try { empty.notify_ready(eosio::chain::experimental::parallel_runtime_limits::max_workers+1, [](size_t) { return true; }); }
      catch (const std::out_of_range&) { rejected = true; }
      require(rejected);
      std::cout << "worker signal assertions=" << assertions << " passed\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
