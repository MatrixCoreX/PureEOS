#include <eosio/chain/parallel_worker_pool.hpp>
#include <condition_variable>
#include <mutex>

namespace eosio::chain::experimental {

struct isolated_worker_pool::impl {
   const std::thread::id owner = std::this_thread::get_id();
   struct batch {
      std::vector<isolated_worker_job> jobs;
      std::vector<isolated_worker_outcome> outcomes;
      size_t next = 0, remaining = 0;
      explicit batch(std::vector<isolated_worker_job> input)
         : jobs(std::move(input)), outcomes(jobs.size()), remaining(jobs.size()) {}
   };
   std::mutex mutex;
   std::condition_variable work, done;
   std::shared_ptr<batch> active;
   bool stopping = false;
   std::vector<std::thread> threads;

   explicit impl(size_t count) {
      if (!count || count > 16) throw primary_state_budget_exceeded();
      threads.reserve(count);
      try {
         for (size_t i = 0; i < count; ++i) threads.emplace_back([this] { loop(); });
      } catch (...) { shutdown(); throw; }
   }
   ~impl() { shutdown(); }
   void shutdown() {
      { std::lock_guard lock(mutex); stopping = true; }
      work.notify_all();
      for (auto& thread : threads) if (thread.joinable()) thread.join();
   }
   void loop() {
      std::unique_ptr<isolated_wasm_module> module;
      std::vector<char> cached_code;
      isolated_wasm_limits cached_limits;
      while (true) {
         std::shared_ptr<batch> current;
         size_t index;
         {
            std::unique_lock lock(mutex);
            work.wait(lock, [&] { return stopping || (active && active->next < active->jobs.size()); });
            if (stopping) return;
            current = active; index = current->next++;
         }
         const auto& job = current->jobs[index];
         auto& outcome = current->outcomes[index];
         try {
            if (!job.ticket.stop_token().stop_requested() && !job.cancel.stop_requested()) {
               if (!module || cached_code != job.code || cached_limits != job.limits) {
                  // Drop stale code BEFORE constructing a replacement, retaining
                  // at most one module per worker even on failure/large batches.
                  module.reset(); cached_code.clear();
                  module = std::make_unique<isolated_wasm_module>(job.code, job.limits);
                  cached_code = job.code; cached_limits = job.limits;
               }
               outcome.began = std::chrono::steady_clock::now();
               outcome.result = module->execute(job.ticket.base(), job.input, job.ticket.stop_token(), {}, job.cancel);
            }
         } catch (...) {
            // Local capability/compile/timeout/allocation failure is NOT a
            // consensus rejection. Re-enter original execution on the owner.
            outcome.result.reset();
         }
         outcome.ended = std::chrono::steady_clock::now();
         if (outcome.began != std::chrono::steady_clock::time_point{})
            outcome.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(outcome.ended - outcome.began);
         {
            std::lock_guard lock(mutex);
            if (--current->remaining == 0) done.notify_one();
         }
      }
   }
};

isolated_worker_pool::isolated_worker_pool(size_t threads) : impl_(std::make_unique<impl>(threads)) {}
isolated_worker_pool::~isolated_worker_pool() = default;

std::vector<isolated_worker_outcome> isolated_worker_pool::run(std::vector<isolated_worker_job> jobs) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   if (jobs.empty() || jobs.size() > 64) throw primary_state_budget_exceeded();
   // Bound all copied inputs too, including jobs that later fail linking.
   for (const auto& job : jobs)
      if (job.code.size() > 64 * 1024 || job.input.data.size() > 64 * 1024 || job.input.authorizations.size() > 64)
         throw primary_state_budget_exceeded();
   auto batch = std::make_shared<impl::batch>(std::move(jobs));
   std::unique_lock lock(impl_->mutex);
   if (impl_->stopping || impl_->active) throw primary_state_invalid_attempt();
   impl_->active = batch;
   impl_->work.notify_all();
   impl_->done.wait(lock, [&] { return batch->remaining == 0; });
   impl_->active.reset();
   return std::move(batch->outcomes);
}

} // namespace eosio::chain::experimental
