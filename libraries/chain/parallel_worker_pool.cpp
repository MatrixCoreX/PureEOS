#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_sync_discovery.hpp>
#include <eosio/chain/parallel_worker_signals.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/thread_cpu_billing.hpp>
#include <eosio/chain/action.hpp>
#include <eosio/vm/exceptions.hpp>
#include <array>
#include <condition_variable>
#include <mutex>

namespace eosio::chain::experimental {

namespace {
void validate_worker_job(const isolated_worker_job& job) {
   if(job.prevalidate_tree && !job.tree)throw primary_state_budget_exceeded();
      if (unsigned(!job.actions.empty()) + unsigned(bool(job.notifications)) + unsigned(bool(job.tree)) +
          unsigned(bool(job.sync)) + unsigned(bool(job.sync_transaction)) + unsigned(bool(job.mixed)) != 1)
         throw primary_state_budget_exceeded();
      if (job.sync_transaction || job.mixed) {
         const auto& roots = job.mixed ? job.mixed->roots : job.sync_transaction->roots;
         const auto& programs = job.mixed ? job.mixed->programs : job.sync_transaction->programs;
         const auto max_roots = job.mixed ? isolated_mixed_tree_executor::max_roots : isolated_sync_executor::max_roots;
         const auto max_programs = job.mixed ? isolated_mixed_tree_executor::max_programs : isolated_sync_executor::max_programs;
         const auto max_code = job.mixed ? isolated_mixed_tree_executor::max_code_bytes : isolated_sync_executor::max_total_code_bytes;
         const auto max_data = job.mixed ? isolated_mixed_tree_executor::max_data_bytes : isolated_sync_executor::max_total_data_bytes;
         if (roots.empty() || roots.size() > max_roots || programs.empty() || programs.size() > max_programs)
            throw primary_state_budget_exceeded();
         size_t code_bytes = 0, data_bytes = 0;
         for (const auto& item : programs) {
            if (item.program.code.size() > isolated_wasm_limits::max_module_code_bytes ||
                item.program.code.size() > max_code - code_bytes)
               throw primary_state_budget_exceeded();
            code_bytes += item.program.code.size();
         }
         for (const auto& root : roots) {
            if (root.data.size() > 64 * 1024 || root.authorizations.size() > 64 || root.max_return_bytes > 4096 ||
                root.data.size() > max_data - data_bytes)
               throw primary_state_budget_exceeded();
            data_bytes += root.data.size();
         }
         return;
      }
      if (job.sync) {
         const auto& input = *job.sync;
         if (input.programs.empty() || input.programs.size() > isolated_sync_executor::max_programs ||
             input.root.data.size() > 64 * 1024 || input.root.authorizations.size() > 64 || input.root.max_return_bytes > 4096)
            throw primary_state_budget_exceeded();
         size_t code_bytes = 0;
         for (const auto& item : input.programs) {
            if (item.program.code.size() > isolated_wasm_limits::max_module_code_bytes ||
                item.program.code.size() > isolated_sync_executor::max_total_code_bytes - code_bytes)
               throw primary_state_budget_exceeded();
            code_bytes += item.program.code.size();
         }
         return;
      }
      if (job.notifications || job.tree) {
         if ((job.notifications && job.tree) || !job.actions.empty()) throw primary_state_budget_exceeded();
         const auto& roots = job.tree ? job.tree->roots : job.notifications->roots;
         const auto& programs = job.tree ? job.tree->programs : job.notifications->programs;
         const auto max_programs = job.tree ? isolated_action_tree_executor::max_programs : isolated_notification_executor::max_programs;
         if (roots.empty() || roots.size() > isolated_transaction_executor::max_actions ||
             programs.empty() || programs.size() > max_programs)
            throw primary_state_budget_exceeded();
         size_t code_bytes = 0, data_bytes = 0;
         for (const auto& program : programs) {
            if (program.code.size() > isolated_wasm_limits::max_module_code_bytes ||
                program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes)
               throw primary_state_budget_exceeded();
            code_bytes += program.code.size();
         }
         for (const auto& root : roots) {
            if (root.data.size() > 64 * 1024 || root.authorizations.size() > 64 || root.max_return_bytes > 4096 ||
                root.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes)
               throw primary_state_budget_exceeded();
            data_bytes += root.data.size();
         }
         return;
      }
      if (job.actions.empty() || job.actions.size() > isolated_transaction_executor::max_actions)
         throw primary_state_budget_exceeded();
      size_t code_bytes = 0, data_bytes = 0;
      for (const auto& action : job.actions) {
         if (action.code.size() > isolated_wasm_limits::max_module_code_bytes || action.input.data.size() > 64 * 1024 ||
             action.input.authorizations.size() > 64 ||
             action.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes ||
             action.input.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes)
            throw primary_state_budget_exceeded();
         code_bytes += action.code.size(); data_bytes += action.input.data.size();
      }
}
}


std::shared_ptr<const isolated_prevalidated_tree> isolated_prevalidated_tree::make(
      const immutable_worker_input<isolated_action_tree_job>& input,isolated_action_tree_result&& result,
      const ordered_attempt_batch::ticket& ticket,const isolated_cancel& cancel) {
   if(!input)throw primary_state_invalid_attempt();
   const auto check=[&]{cancel.check(ticket.stop_token());};
   check();
   const auto began=thread_cpu_clock::now();
   if(!validate_isolated_action_tree_journal(result,input->roots,input->programs,input->context,check))
      throw primary_state_invalid_attempt();
   const auto cpu=thread_cpu_clock::now()-began;
   check();
   if(cpu.count()<0 || cpu>isolated_transaction_executor::max_attempt_time)
      throw primary_state_invalid_attempt();
   return std::shared_ptr<const isolated_prevalidated_tree>(
      new isolated_prevalidated_tree(input,std::move(result),ticket,cpu));
}

bool isolated_prevalidated_tree::matches(const isolated_action_tree_result& result,
      std::span<const isolated_action_input> roots,std::span<const isolated_receiver_program> programs,
      const isolated_action_tree_context& context,const std::function<void()>& checkpoint)const {
   const auto check=[&]{if(checkpoint)checkpoint();};
   check();
   if(!result_ || &result!=&*result_ || !input_ || roots.empty() || programs.empty() ||
      roots.size()!=input_->roots.size() || programs.size()>isolated_action_tree_executor::max_programs ||
      context.max_inline_action_size!=input_->context.max_inline_action_size ||
      context.max_inline_action_depth!=input_->context.max_inline_action_depth ||
      context.console_enabled!=input_->context.console_enabled ||
      bool(context.context_free_data)!=bool(input_->context.context_free_data) ||
      (context.context_free_data && context.context_free_data->fingerprint()!=
                                    input_->context.context_free_data->fingerprint()))return false;
   for(size_t i=0;i<roots.size();++i) {
      check();const auto& a=roots[i];const auto& b=input_->roots[i];
      if(a.receiver!=b.receiver || a.account!=b.account || a.action!=b.action ||
         a.authorizations!=b.authorizations || a.data!=b.data || a.max_return_bytes!=b.max_return_bytes ||
         !same_isolated_environment(a.environment,b.environment))return false;
   }
   // The owner recaptures EVERY actually executed receiver and native pair.
   // Unused frozen prefetch entries grant no capability and need not be live.
   small_seen_keys<name> seen;
   for(const auto& current:programs) {
      check();if(!seen.insert(current.receiver))return false;
      const auto found=std::find_if(input_->programs.begin(),input_->programs.end(),
         [&](const auto& p){return p.receiver==current.receiver;});
      if(found==input_->programs.end() || found->limits!=current.limits ||
         found->serial_only!=current.serial_only || found->code!=current.code)return false;
   }
   return true;
}


bool isolated_prevalidated_tree::matches_current_roots(const isolated_action_tree_result& result,
      std::span<const action> roots,uint32_t max_return_bytes,
      const std::shared_ptr<const isolated_transaction_environment>& environment,
      std::span<const isolated_receiver_program> programs,const isolated_action_tree_context& context,
      const std::function<void()>& checkpoint)const {
   const auto check=[&]{if(checkpoint)checkpoint();};check();
   if(!result_ || &result!=&*result_ || !input_ || roots.empty() ||
      roots.size()!=input_->roots.size())return false;
   for(size_t i=0;i<roots.size();++i) {
      check();const auto& actual=roots[i];const auto& expected=input_->roots[i];
      if(expected.receiver!=actual.account || expected.account!=actual.account ||
         expected.action!=actual.name || expected.data!=actual.data ||
         expected.authorizations.size()!=actual.authorization.size() ||
         expected.max_return_bytes!=max_return_bytes ||
         !same_isolated_environment(expected.environment,environment))return false;
      for(size_t j=0;j<actual.authorization.size();++j) {
         check();if(expected.authorizations[j].first!=actual.authorization[j].actor ||
                    expected.authorizations[j].second!=actual.authorization[j].permission)return false;
      }
   }
   // Preserve the original identity,context and fresh program-catalogue
   // matcher. Its owned-root comparison remains a differential oracle.
   return matches(result,input_->roots,programs,context,checkpoint);
}

bool isolated_prevalidated_tree::binds_current_step(const isolated_action_tree_result& result,size_t index,
      name receiver,const fc::sha256& code_hash,const action& actual,const isolated_wasm_limits& limits,
      uint32_t max_return_bytes,const isolated_action_tree_context& context,
      const std::function<void()>& checkpoint)const {
   const auto check=[&]{if(checkpoint)checkpoint();};check();
   if(!result_ || &result!=&*result_ || !input_ || index>=result.steps.size() ||
      context.max_inline_action_size!=input_->context.max_inline_action_size ||
      context.max_inline_action_depth!=input_->context.max_inline_action_depth ||
      context.console_enabled!=input_->context.console_enabled ||
      bool(context.context_free_data)!=bool(input_->context.context_free_data) ||
      (context.context_free_data && context.context_free_data->fingerprint()!=
                                      input_->context.context_free_data->fingerprint()))return false;
   const auto& step=result.steps[index];
   if(step.group_index>=result.groups.size() || step.receiver!=receiver || step.code_hash!=code_hash)return false;
   const auto& expected=result.groups[step.group_index].input;
   if(actual.account!=expected.account || actual.name!=expected.action ||
      actual.data!=expected.data || actual.authorization.size()!=expected.authorizations.size() ||
      max_return_bytes!=expected.max_return_bytes)return false;
   for(size_t i=0;i<actual.authorization.size();++i) {
      check();if(actual.authorization[i].actor!=expected.authorizations[i].first ||
                 actual.authorization[i].permission!=expected.authorizations[i].second)return false;
   }
   const auto program=std::find_if(input_->programs.begin(),input_->programs.end(),
      [&](const auto& p){return p.receiver==receiver;});
   return program!=input_->programs.end() && program->limits==limits &&
      !program->serial_only && program->code.content_hash()==code_hash;
}

struct isolated_worker_pool::impl {
   static constexpr size_t max_threads = parallel_runtime_limits::max_workers;
   const std::thread::id owner = std::this_thread::get_id();
   struct batch {
      std::vector<isolated_worker_job> jobs;
      std::vector<std::optional<isolated_worker_job>> streamed_jobs;
      size_t published=0,started=0;
      std::vector<isolated_worker_outcome> outcomes;
      size_t next = 0, remaining = 0;
      const bool stable_small_batch;
      const bool streaming=false;
      std::array<bool, max_threads> claimed{}; // read/write only under pool mutex
      std::array<size_t, max_threads> assigned{};
      batch(std::vector<isolated_worker_job> input, size_t worker_count)
         : jobs(std::move(input)), outcomes(jobs.size()), remaining(jobs.size()),
           stable_small_batch(jobs.size() <= worker_count) {
         published=jobs.size();
         assigned.fill(jobs.size());
         if (!stable_small_batch) return;
         // A discovery retry is a sparse subset of the original slots. Keep
         // its original worker/cache, not its compacted position in this wave.
         bool unique_original_slots = true;
         for (size_t i = 0; i < jobs.size(); ++i) {
            const auto slot = jobs[i].ticket.index();
            if (slot >= worker_count || assigned[slot] != jobs.size()) {
               unique_original_slots = false; break;
            }
            assigned[slot] = i;
         }
         if (!unique_original_slots) {
            assigned.fill(jobs.size());
            for (size_t i = 0; i < jobs.size(); ++i) assigned[i] = i;
         }
      }

      batch(std::span<const ordered_attempt_batch::ticket> tags,size_t worker_count)
         :streamed_jobs(tags.size()),outcomes(tags.size()),remaining(tags.size()),
          stable_small_batch(tags.size()<=worker_count),streaming(true) {
         assigned.fill(tags.size());
         if(!stable_small_batch)return;
         bool unique=true;
         for(size_t i=0;i<tags.size();++i) {
            const auto slot=tags[i].index();
            if(slot>=worker_count || assigned[slot]!=tags.size()) {unique=false;break;}
            assigned[slot]=i;
         }
         if(!unique) {
            assigned.fill(tags.size());
            for(size_t i=0;i<tags.size();++i)assigned[i]=i;
         }
      }
      const isolated_worker_job& get_job(size_t index)const {
         return streaming ? *streamed_jobs[index] : jobs[index];
      }
      bool has_job(size_t worker_index) const noexcept {
         return stable_small_batch ? assigned[worker_index] < published && !claimed[worker_index] : next < published;
      }
      size_t take_job(size_t worker_index) noexcept {
         if (!stable_small_batch) return next++;
         claimed[worker_index] = true;
         return assigned[worker_index];
      }
   };
   std::mutex mutex;
   parallel_worker_signals<max_threads> work;
   std::condition_variable done;
   std::shared_ptr<batch> active;
   std::optional<std::vector<isolated_worker_outcome>> retired;
   bool retiring=false;
   isolated_retirement_diagnostics retirement;
   bool stopping = false;
   // Outlives joined workers and their caches/active frames. No timer callback
   // can observe a released VM or borrowed stack after a worker returns.
   std::shared_ptr<const isolated_execution_runtime> runtime;
   std::vector<std::thread> threads;

   explicit impl(size_t count, isolated_vm_engine engine) {
      if (!parallel_runtime_limits::valid_workers(count)) throw primary_state_budget_exceeded();
      runtime = std::make_shared<isolated_execution_runtime>(engine,
         count * (isolated_sync_executor::max_depth + 1));
      threads.reserve(count);
      try {
         for (size_t i = 0; i < count; ++i) threads.emplace_back([this, i] { loop(i); });
      } catch (...) { shutdown(); throw; }
   }
   ~impl() { shutdown(); }
   void shutdown() {
      { std::lock_guard lock(mutex); stopping = true; }
      work.notify_shutdown();
      for (auto& thread : threads) if (thread.joinable()) thread.join();
   }
   void loop(size_t worker_index) {
      // Construct lazily inside the guarded job, so allocation failure cannot
      // escape this thread and terminate the process before returning fallback.
      // ONE thread-owned cache survives mode changes; no live module, overlay
      // or host capability is shared with another thread or retained by it.
      std::shared_ptr<isolated_module_cache> modules;
      std::unique_ptr<isolated_transaction_executor> executor;
      std::unique_ptr<isolated_notification_executor> notification_executor;
      std::unique_ptr<isolated_action_tree_executor> tree_executor;
      std::unique_ptr<isolated_sync_executor> sync_executor;
      std::unique_ptr<isolated_mixed_tree_executor> mixed_executor;
      while (true) {
         std::shared_ptr<batch> current;
         std::optional<std::vector<isolated_worker_outcome>> reclaim;
         size_t index=0;
         {
            std::unique_lock lock(mutex);
            // A small joined batch can give every job a distinct cache owner.
            // The same slot stays on that logical worker across discovery
            // waves, including sparse retries. Never move
            // a VM or pin an OS CPU. Larger batches keep the shared queue.
            work.wait(worker_index, lock, [&] {
               return stopping || (worker_index==0 && retired) || (active && active->has_job(worker_index));
            });
            if(worker_index==0 && retired) {
               reclaim.emplace(std::move(*retired));retired.reset();retiring=true;
            } else {
               if(stopping)return;
               current=active;index=current->take_job(worker_index);++current->started;
            }
         }
         if(reclaim) {
            // Only immutable owned bytes/state;no live DB,VM or callback.
            reclaim.reset();
            {
               std::lock_guard lock(mutex);
               retiring=false;++retirement.completed_batches;
               retirement.last_thread=std::this_thread::get_id();
            }
            done.notify_all();
            continue;
         }
         const auto& job = current->get_job(index);
         auto& outcome = current->outcomes[index];
         const auto attempt_began = std::chrono::steady_clock::now();
         try {
            job.cancel.check(job.ticket.stop_token());
            if (!modules) modules = std::make_shared<isolated_module_cache>(runtime);
            {
               if (job.mixed) {
                  executor.reset();
                  notification_executor.reset();
                  tree_executor.reset();
                  sync_executor.reset();
                  if (!mixed_executor) mixed_executor = std::make_unique<isolated_mixed_tree_executor>(modules);
                  const auto& input = *job.mixed;
                  outcome.mixed_result = mixed_executor->execute(job.ticket.base(), input.roots,
                     input.programs, input.context, job.ticket.stop_token(), {}, job.cancel);
                  bool first = true;
                  const auto add_elapsed = [&](std::chrono::microseconds elapsed) {
                     if (elapsed.count() < 0 || elapsed > isolated_mixed_tree_executor::max_attempt_time - outcome.elapsed)
                        throw primary_state_invalid_attempt();
                     outcome.elapsed += elapsed;
                  };
                  for (const auto& step : outcome.mixed_result->steps) {
                     // Empty receivers have no VM interval. In particular, a
                     // trailing notification must not erase the last real end.
                     if (step.has_code) {
                        if (first) { outcome.began = step.effects.began; first = false; }
                        outcome.ended = step.effects.ended;
                     }
                     add_elapsed(step.effects.elapsed);
                     for (const auto& call : step.calls) add_elapsed(call.exclusive_elapsed);
                  }
               } else if (job.sync_transaction) {
                  mixed_executor.reset();
                  executor.reset();
                  notification_executor.reset();
                  tree_executor.reset();
                  if (!sync_executor) sync_executor = std::make_unique<isolated_sync_executor>(modules);
                  const auto& input = *job.sync_transaction;
                  outcome.sync_transaction_result = sync_executor->execute_transaction(job.ticket.base(), input.roots,
                     input.programs, input.context, job.ticket.stop_token(), {}, job.cancel);
                  const auto& result = *outcome.sync_transaction_result;
                  outcome.began = result.roots.front().root.began;
                  outcome.ended = result.roots.back().root.ended;
                  const auto add_elapsed = [&](std::chrono::microseconds elapsed) {
                     if (elapsed.count() < 0 || elapsed > isolated_sync_executor::max_attempt_time - outcome.elapsed)
                        throw primary_state_invalid_attempt();
                     outcome.elapsed += elapsed;
                  };
                  // Bill the successful exclusive time once across ALL roots;
                  // inclusive root intervals remain local diagnostics only.
                  for (const auto& root : result.roots) {
                     add_elapsed(root.root.elapsed);
                     for (const auto& call : root.calls) add_elapsed(call.exclusive_elapsed);
                  }
               } else if (job.sync) {
                  mixed_executor.reset();
                  executor.reset();
                  notification_executor.reset();
                  tree_executor.reset();
                  if (!sync_executor) sync_executor = std::make_unique<isolated_sync_executor>(modules);
                  const auto& input = *job.sync;
                  outcome.sync_result = sync_executor->execute(job.ticket.base(), input.root, input.programs, input.context,
                                                               job.ticket.stop_token(), {}, job.cancel);
                  const auto& result = *outcome.sync_result;
                  // Root bounds are inclusive DIAGNOSTICS, not proof of actual
                  // leaf WASM overlap. Charge each exclusive frame exactly once.
                  outcome.began = result.root.began;
                  outcome.ended = result.root.ended;
                  const auto add_elapsed = [&](std::chrono::microseconds elapsed) {
                     if (elapsed.count() < 0 || elapsed > isolated_sync_executor::max_attempt_time - outcome.elapsed)
                        throw primary_state_invalid_attempt();
                     outcome.elapsed += elapsed;
                  };
                  add_elapsed(result.root.elapsed);
                  for (const auto& call : result.calls) add_elapsed(call.exclusive_elapsed);
               } else if (job.tree) {
                  mixed_executor.reset();
                  executor.reset();
                  notification_executor.reset();
                  sync_executor.reset();
                  if (!tree_executor) tree_executor = std::make_unique<isolated_action_tree_executor>(modules);
                  const auto& input = *job.tree;
                  outcome.tree_result = tree_executor->execute(job.ticket.base(), input.roots, input.programs, input.context,
                                                               job.ticket.stop_token(), {}, job.cancel);
                  bool first = true;
                  for (const auto& step : outcome.tree_result->steps) if (step.has_code) {
                     if (first) { outcome.began = step.effects.began; first = false; }
                     outcome.ended = step.effects.ended;
                     outcome.elapsed += step.effects.elapsed;
                  }
                  if(job.prevalidate_tree) {
                     outcome.prevalidated_tree=isolated_prevalidated_tree::make(
                        job.tree,std::move(*outcome.tree_result),job.ticket,job.cancel);
                     outcome.tree_result.reset();
                  }
               } else if (job.notifications) {
                  mixed_executor.reset();
                  executor.reset();
                  tree_executor.reset();
                  sync_executor.reset();
                  if (!notification_executor) notification_executor = std::make_unique<isolated_notification_executor>(modules);
                  const auto& input = *job.notifications;
                  outcome.notification_result = notification_executor->execute(job.ticket.base(), input.roots, input.programs,
                                                                                job.ticket.stop_token(), {}, job.cancel);
                  bool first = true;
                  for (const auto& step : outcome.notification_result->steps) if (step.has_code) {
                     if (first) { outcome.began = step.effects.began; first = false; }
                     outcome.ended = step.effects.ended;
                     outcome.elapsed += step.effects.elapsed;
                  }
               } else {
                  mixed_executor.reset();
                  notification_executor.reset();
                  tree_executor.reset();
                  sync_executor.reset();
                  if (!executor) executor = std::make_unique<isolated_transaction_executor>(modules);
                  outcome.result = executor->execute(job.ticket.base(), job.actions, job.ticket.stop_token(), {}, job.cancel);
                  outcome.began = outcome.result->actions.front().began;
                  outcome.ended = outcome.result->actions.back().ended;
                  for (const auto& action : outcome.result->actions) outcome.elapsed += action.elapsed;
               }
            }
         } catch (const isolated_sync_target_missing& missing) {
            // Both values are owned. No successful prefix or live state is
            // carried across the owner's join/capture/new-snapshot boundary.
            outcome.failure = isolated_worker_failure::missing_state;
            outcome.request = missing.request;
            outcome.receiver_request = missing.receiver;
         } catch (const primary_state_missing& missing) {
            // Owned request only. The joined owner may capture it and restart
            // with a NEW immutable preparation/base; never a live DB pointer.
            outcome.failure = isolated_worker_failure::missing_state;
            outcome.request = missing.request;
         } catch (const isolated_receiver_missing& missing) {
            outcome.failure = isolated_worker_failure::missing_receiver;
            outcome.receiver_request = missing.receiver;
         } catch (const isolated_execution_interrupted& interrupted) {
            outcome.failure = interrupted.reason == isolated_interrupt_reason::cancelled
               ? isolated_worker_failure::cancelled : isolated_worker_failure::deadline;
         } catch (const primary_state_budget_exceeded&) {
            outcome.failure = isolated_worker_failure::budget;
         } catch (const isolated_sync_mode_required&) {
            outcome.failure = isolated_worker_failure::sync_required;
         } catch (const isolated_mixed_mode_required&) {
            outcome.failure = isolated_worker_failure::mixed_required;
         } catch (const isolated_transaction_environment_missing&) {
            outcome.failure = isolated_worker_failure::environment_required;
         } catch (const primary_state_unsupported&) {
            outcome.failure = isolated_worker_failure::unsupported;
         } catch (const primary_state_invalid_attempt&) {
            outcome.failure = isolated_worker_failure::invalid_attempt;
         } catch (const eosio::vm::timeout_exception&) {
            outcome.failure = isolated_worker_failure::vm_timeout;
         } catch (const eosio::vm::wasm_bad_alloc&) {
            outcome.failure = isolated_worker_failure::allocation;
         } catch (const std::bad_alloc&) {
            outcome.failure = isolated_worker_failure::allocation;
         } catch (const eosio::vm::exception&) {
            outcome.failure = isolated_worker_failure::vm_error;
         } catch (...) {
            // Local capability/compile/timeout/allocation failure is NOT a
            // consensus rejection. Re-enter original execution on the owner.
            outcome.failure = isolated_worker_failure::unexpected;
         }
         if (outcome.failure != isolated_worker_failure::none) {
            outcome.result.reset();
            outcome.notification_result.reset();
            outcome.tree_result.reset();
            outcome.prevalidated_tree.reset();
            outcome.sync_result.reset();
            outcome.sync_transaction_result.reset();
            outcome.mixed_result.reset();
         }
         if (!outcome.result && !outcome.notification_result && !outcome.tree_attempt() && !outcome.sync_result &&
             !outcome.sync_transaction_result && !outcome.mixed_result) {
            outcome.elapsed = {};
            outcome.ended = std::chrono::steady_clock::now();
         }
         outcome.attempt_wall = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - attempt_began);
         {
            std::lock_guard lock(mutex);
            if (--current->remaining == 0) done.notify_one();
         }
      }
   }
};

isolated_worker_pool::isolated_worker_pool(size_t threads, isolated_vm_engine engine)
   : impl_(std::make_unique<impl>(threads, engine)) {}
isolated_worker_pool::~isolated_worker_pool() = default;

std::vector<isolated_worker_outcome> isolated_worker_pool::run(std::vector<isolated_worker_job> jobs) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   if (!parallel_runtime_limits::valid_batch(jobs.size())) throw primary_state_budget_exceeded();
   for(const auto& job:jobs)validate_worker_job(job);
   auto batch = std::make_shared<impl::batch>(std::move(jobs), impl_->threads.size());
   std::unique_lock lock(impl_->mutex);
   if (impl_->stopping || impl_->active) throw primary_state_invalid_attempt();
   impl_->active = batch;
   impl_->work.notify_ready(impl_->threads.size(), [&](size_t worker) { return batch->has_job(worker); });
   impl_->done.wait(lock, [&] { return batch->remaining == 0; });
   impl_->active.reset();
   return std::move(batch->outcomes);
}


// The owner constructs bounded immutable inputs while earlier chunks execute.
// Storage is fixed before publication; only distinct optional slots are filled.
// Failure joins every published job before rethrowing, so no work/result can
// escape into a later snapshot, canonical callback, undo or pool destruction.
std::vector<isolated_worker_outcome> isolated_worker_pool::run_streamed(
      std::span<const ordered_attempt_batch::ticket> tickets,
      const std::function<isolated_worker_job(size_t)>& produce) {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   if(!parallel_runtime_limits::valid_batch(tickets.size()))throw primary_state_budget_exceeded();
   if(!produce)throw primary_state_invalid_attempt();
   auto batch=std::make_shared<impl::batch>(tickets,impl_->threads.size());
   {
      std::lock_guard lock(impl_->mutex);
      if(impl_->stopping || impl_->active)throw primary_state_invalid_attempt();
      impl_->active=batch;
   }
   std::exception_ptr failed;
   try {
      for(size_t i=0;i<tickets.size();++i) {
         auto job=produce(i); // owner only, with no pool lock held
         if(!job.ticket.same_preparation(tickets[i]))throw primary_state_invalid_attempt();
         validate_worker_job(job);
         std::lock_guard lock(impl_->mutex);
         batch->streamed_jobs[i].emplace(std::move(job));
         ++batch->published;
         if(batch->published%impl_->threads.size()==0 || batch->published==tickets.size())
            impl_->work.notify_ready(impl_->threads.size(),[&](size_t worker){return batch->has_job(worker);});
      }
   } catch(...) {failed=std::current_exception();}
   {
      std::unique_lock lock(impl_->mutex);
      if(failed) {
         batch->remaining-=tickets.size()-batch->published;
         impl_->work.notify_ready(impl_->threads.size(),[&](size_t worker){return batch->has_job(worker);});
      }
      impl_->done.wait(lock,[&]{return batch->remaining==0;});
      impl_->active.reset();
   }
   if(failed)std::rethrow_exception(failed);
   return std::move(batch->outcomes);
}

size_t isolated_worker_pool::active_started_jobs()const {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   std::lock_guard lock(impl_->mutex);
   return impl_->active ? impl_->active->started : 0;
}


bool isolated_worker_pool::retire_outcomes(std::vector<isolated_worker_outcome>&& outcomes) {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   if(outcomes.empty())return true;
   if(outcomes.size()>parallel_runtime_limits::max_batch_transactions)throw primary_state_budget_exceeded();
   std::lock_guard lock(impl_->mutex);
   if(impl_->stopping || impl_->active || impl_->retired || impl_->retiring)return false;
   const auto count=outcomes.size();
   impl_->retired.emplace(std::move(outcomes));
   ++impl_->retirement.accepted_batches;
   impl_->retirement.peak_outcomes=std::max(impl_->retirement.peak_outcomes,count);
   impl_->work.notify_ready(impl_->threads.size(),[](size_t worker){return worker==0;});
   return true;
}
void isolated_worker_pool::drain_retired(const std::function<void()>& checkpoint) {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   if(checkpoint)checkpoint();
   std::unique_lock lock(impl_->mutex);
   while(impl_->retired || impl_->retiring) {
      impl_->done.wait_for(lock,std::chrono::microseconds(100));
      lock.unlock();if(checkpoint)checkpoint();lock.lock();
   }
}
isolated_retirement_diagnostics isolated_worker_pool::retirement_diagnostics()const {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   std::lock_guard lock(impl_->mutex);return impl_->retirement;
}

} // namespace eosio::chain::experimental
