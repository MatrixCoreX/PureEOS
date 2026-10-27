#pragma once

#include <eosio/chain/parallel_runtime_limits.hpp>
#include <eosio/chain/parallel_wasm_worker.hpp>
#include <thread>
#include <utility>

namespace eosio::chain::experimental {

// One owner-held budget for the entire initial discovery AND future prefix
// refreshes. Workers receive only a copy of cancellation(), never this object.
// The caller first clamps the inherited absolute deadline to its phase limit;
// checkpoint/worker checks enforce that deadline. No operation here renews it.
class parallel_preparation_budget {
public:
   explicit parallel_preparation_budget(isolated_cancel bounded_cancel)
      : cancel_(std::move(bounded_cancel)) {
      if (cancel_.deadline == std::chrono::steady_clock::time_point::max())
         throw primary_state_invalid_attempt();
   }
   parallel_preparation_budget(const parallel_preparation_budget&) = delete;
   parallel_preparation_budget& operator=(const parallel_preparation_budget&) = delete;
   parallel_preparation_budget(parallel_preparation_budget&&) = delete;
   parallel_preparation_budget& operator=(parallel_preparation_budget&&) = delete;

   const isolated_cancel& cancellation() const { check_owner(); return cancel_; }
   size_t remaining_rounds() const { check_owner(); return rounds_; }
   bool try_begin_round() {
      check_owner();
      if (!rounds_) return false;
      --rounds_;
      return true;
   }
   size_t remaining_environment_wire_bytes() const { check_owner(); return environment_bytes_; }
   void consume_environment_wire_bytes(size_t bytes) {
      check_owner();
      if (bytes > environment_bytes_) throw primary_state_budget_exceeded();
      environment_bytes_ -= bytes; // subtraction after comparison: no overflow
   }

private:
   void check_owner() const {
      if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
   }
   const std::thread::id owner_ = std::this_thread::get_id();
   const isolated_cancel cancel_;
   size_t rounds_ = parallel_runtime_limits::max_discovery_rounds;
   size_t environment_bytes_ = parallel_runtime_limits::max_environment_wire_bytes;
};

} // namespace eosio::chain::experimental
