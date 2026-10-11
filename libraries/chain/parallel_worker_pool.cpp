#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/performance_metrics.hpp>
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
      // Construct lazily inside the guarded job, so allocation failure cannot
      // escape this thread and terminate the process before returning fallback.
      std::unique_ptr<isolated_transaction_executor> executor;
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
               if (!executor) executor = std::make_unique<isolated_transaction_executor>();
               outcome.result = executor->execute(job.ticket.base(), job.actions, job.ticket.stop_token(), {}, job.cancel);
               outcome.began = outcome.result->actions.front().began;
               outcome.ended = outcome.result->actions.back().ended;
               for (const auto& action : outcome.result->actions) outcome.elapsed += action.elapsed;
            }
         } catch (const primary_state_missing& missing) {
            // Owned request only. The joined owner may capture it and restart
            // with a NEW batch/base; no worker ever follows a live DB pointer.
            outcome.result.reset();
            outcome.request = missing.request;
         } catch (...) {
            // Local capability/compile/timeout/allocation failure is NOT a
            // consensus rejection. Re-enter original execution on the owner.
            outcome.result.reset();
         }
         if (!outcome.result) outcome.ended = std::chrono::steady_clock::now();
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
   for (const auto& job : jobs) {
      if (job.actions.empty() || job.actions.size() > isolated_transaction_executor::max_actions)
         throw primary_state_budget_exceeded();
      size_t code_bytes = 0, data_bytes = 0;
      for (const auto& action : job.actions) {
         if (action.code.size() > 64 * 1024 || action.input.data.size() > 64 * 1024 ||
             action.input.authorizations.size() > 64 ||
             action.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes ||
             action.input.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes)
            throw primary_state_budget_exceeded();
         code_bytes += action.code.size(); data_bytes += action.input.data.size();
      }
   }
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
