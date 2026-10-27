#include <eosio/chain/parallel_ordered_batch.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <algorithm>
#include <array>

namespace eosio::chain::experimental {

ordered_attempt_batch::ordered_attempt_batch(std::shared_ptr<const frozen_primary_state> base,
      std::span<const request> transactions) {
   if (!base || !parallel_runtime_limits::valid_batch(transactions.size())) throw primary_state_budget_exceeded();
   for (const auto& item : transactions)
      if (item.transaction_id == fc::sha256{} || item.execution_fingerprint == fc::sha256{})
         throw primary_state_invalid_attempt();
   identity_ = std::make_shared<identity>();
   identity_->slots.reserve(transactions.size());
   for (const auto& item : transactions)
      identity_->slots.push_back(std::make_shared<const preparation>(preparation{base, item}));
}

ordered_attempt_batch::~ordered_attempt_batch() {
   // No worker refers to this coordinator itself, only its owned identity/base.
   // The outer executor must still join its pool before process/runtime shutdown.
   identity_->stop.request_stop();
}

void ordered_attempt_batch::check_owner() const {
   if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
}

ordered_attempt_batch::ticket ordered_attempt_batch::get_ticket(size_t index) const {
   check_owner();
   if (closed_ || index >= identity_->slots.size()) throw primary_state_invalid_attempt();
   return {identity_, index};
}

void ordered_attempt_batch::prepare_retries(std::shared_ptr<const frozen_primary_state> base,
      std::span<const retry_request> retries) {
   check_owner();
   if (closed_ || in_callback_ || next_ != 0 || identity_->stop.stop_requested() || retries.empty())
      throw primary_state_invalid_attempt();
   if (!base || retries.size() > identity_->slots.size()) throw primary_state_budget_exceeded();
   auto next = identity_->slots;
   std::array<bool, parallel_runtime_limits::max_batch_transactions> changed{};
   for (const auto& retry : retries) {
      if (retry.index >= next.size() || changed[retry.index] ||
          retry.transaction.transaction_id != next[retry.index]->transaction.transaction_id ||
          retry.transaction.execution_fingerprint == fc::sha256{}) throw primary_state_invalid_attempt();
      changed[retry.index] = true;
      next[retry.index] = std::make_shared<const preparation>(preparation{base, retry.transaction});
   }
   check_snapshot_budget(next);
   identity_->slots.swap(next); // publish only after all validation/allocation
}

void ordered_attempt_batch::check_snapshot_budget(const std::vector<std::shared_ptr<const preparation>>& slots) {
   // Count a shared snapshot once. Do not accidentally retain 64 independent
   // 4 MiB snapshots in the coordinator. Joined worker outcomes/tickets must
   // also be released by the owner when replaced; they own their own lifetimes.
   constexpr size_t max_retained_value_bytes = parallel_runtime_limits::max_retained_snapshot_bytes;
   std::array<const frozen_primary_state*, parallel_runtime_limits::max_batch_transactions> counted{};
   size_t count = 0, bytes = 0;
   for (const auto& slot : slots) {
      if (std::find(counted.begin(), counted.begin() + count, slot->base.get()) != counted.begin() + count) continue;
      if (slot->base->value_bytes() > max_retained_value_bytes - bytes) throw primary_state_budget_exceeded();
      bytes += slot->base->value_bytes();
      counted[count++] = slot->base.get();
   }
}

ordered_attempt_batch::ticket ordered_attempt_batch::refresh_next(const ticket& previous,
      const fc::sha256& expected_transaction, const std::function<refreshed_input()>& capture) {
   check_next(previous, expected_transaction);
   if (!next_ || identity_->stop.stop_requested() || !capture) throw primary_state_invalid_attempt();
   // This bounds descriptor replacement even for a mistaken owner loop. The
   // future executor must additionally debit its shared discovery/time budget.
   if (refreshes_ >= parallel_runtime_limits::max_discovery_rounds) throw primary_state_budget_exceeded();
   auto input = [&] {
      in_callback_ = true;
      try {
         auto result = capture();
         in_callback_ = false;
         return result;
      } catch (...) {
         in_callback_ = false;
         closed_ = true;
         disable_speculation();
         throw;
      }
   }();
   if (identity_->stop.stop_requested() || input.transaction.transaction_id != expected_transaction ||
       input.transaction.execution_fingerprint == fc::sha256{}) throw primary_state_invalid_attempt();
   if (!input.base) throw primary_state_budget_exceeded();
   // A capture callback must not return any retained old snapshot. Pointer
   // freshness is necessary, not sufficient: current code/context/state still
   // require the original canonical adopter checks before any ledger writes.
   for (const auto& slot : identity_->slots)
      if (slot->base == input.base) throw primary_state_invalid_attempt();
   auto next = identity_->slots;
   next[next_] = std::make_shared<const preparation>(preparation{std::move(input.base), input.transaction, next_});
   check_snapshot_budget(next);
   identity_->slots.swap(next);
   ++refreshes_;
   return {identity_, next_};
}

size_t ordered_attempt_batch::next_index() const { check_owner(); return next_; }
bool ordered_attempt_batch::complete() const { check_owner(); return next_ == identity_->slots.size(); }

void ordered_attempt_batch::disable_speculation() { identity_->stop.request_stop(); }

void ordered_attempt_batch::discard_remaining() { check_owner(); disable_speculation(); }

void ordered_attempt_batch::close() {
   check_owner();
   if (in_callback_) throw primary_state_invalid_attempt();
   closed_ = true;
   disable_speculation();
}

void ordered_attempt_batch::check_next(const ticket& tag, const fc::sha256& expected_transaction) const {
   check_owner();
   if (closed_ || in_callback_ || tag.identity_ != identity_ || tag.index_ != next_ ||
       next_ >= identity_->slots.size() || tag.preparation_ != identity_->slots[next_] ||
       expected_transaction != identity_->slots[next_]->transaction.transaction_id)
      throw primary_state_invalid_attempt();
}

void ordered_attempt_batch::record_conflict() {
   if (performance::metrics().enabled.load(std::memory_order_relaxed))
      performance::metrics().record(performance::stage::parallel_conflict, 0);
}

ordered_attempt_batch::candidate_status ordered_attempt_batch::classify(const primary_overlay_result& state, const fc::sha256& fingerprint,
      bool secondary_allowed, const std::function<void()>& checkpoint) const {
   using status = candidate_status;
   if (identity_->stop.stop_requested()) return status::unavailable;
   if (state.base() != identity_->slots[next_]->base || fingerprint != identity_->slots[next_]->transaction.execution_fingerprint)
      throw primary_state_invalid_attempt();
   // Legacy root-only results have no complete cursor transcript to justify
   // secondary adoption, even if their dependency sets happen to be current.
   if (!secondary_allowed && !state.primary_only()) return status::unavailable;
   if (state.writes().size() > max_operations_per_result || state.read_tables().size() > max_operations_per_result ||
       state.read_queries().size() > max_operations_per_result ||
       state.secondary_read_queries().size() > max_operations_per_result - state.read_queries().size()) return status::unavailable;
   // Only refresh_next can publish a nonzero prefix, and only for next_. Its
   // fresh snapshot already includes every preceding commit. Older tail slots
   // still have prefix zero and must check the entire accumulated dirty set.
   if (identity_->slots[next_]->committed_prefix != 0) {
      if (identity_->slots[next_]->committed_prefix != next_) throw primary_state_invalid_attempt();
      if (checkpoint) checkpoint();
      return identity_->stop.stop_requested() ? status::unavailable : status::reusable;
   }
   for (const auto& table : state.read_tables()) {
      if (checkpoint) checkpoint();
      if (identity_->stop.stop_requested()) return status::unavailable;
      if (structural_dirty_.contains(table)) return status::dependency_conflict;
   }
   for (const auto& query : state.read_queries()) {
      if (checkpoint) checkpoint();
      if (identity_->stop.stop_requested()) return status::unavailable;
      const auto dirty = dirty_.find(query.table);
      if (dirty == dirty_.end()) continue;
      const auto selected = state.base()->locate_primary(query);
      // Structural changes were already ruled out above. Remaining
      // size/payer/value updates do not change absence or range selection;
      // only a selected row may be stale. Canonical adoption rechecks both
      // query selection and table/row identities against the actual database.
      if (!selected.row || !dirty->second.contains(selected.row->primary_key)) continue;
      // Exact dependency-conflict events only; the already-disabled serial
      // tail is not counted repeatedly. A zero-duration sample is a counter.
      return status::dependency_conflict;
   }
   for (const auto& query : state.secondary_read_queries()) {
      if (checkpoint) checkpoint();
      if (identity_->stop.stop_requested()) return status::unavailable;
      const auto table = secondary_dirty_.find(query.table);
      if (table == secondary_dirty_.end()) continue;
      const auto& changes = table->second[static_cast<size_t>(secondary_kind(query.key))];
      if (changes.primary_keys.empty()) continue;
      const auto selected = state.base()->read_secondary(query);
      if (selected.row && changes.primary_keys.contains(selected.row->primary_key)) return status::dependency_conflict;
      using kind = secondary_query_kind;
      if (query.kind == kind::find_primary) {
         if (changes.primary_keys.contains(query.primary_key)) return status::dependency_conflict;
         continue;
      }
      // Adjacency also depends on the anchor, not only its selected neighbor.
      if ((query.kind == kind::next || query.kind == kind::previous) &&
          changes.primary_keys.contains(query.primary_key)) return status::dependency_conflict;
      const auto& positions = changes.positions;
      auto candidate = positions.end();
      bool reverse = false;
      switch (query.kind) {
         case kind::find_secondary:
            candidate = positions.lower_bound({query.key, 0});
            if (candidate != positions.end() && !secondary_key_equivalent(candidate->key, query.key)) candidate = positions.end();
            break;
         case kind::lower_bound: candidate = positions.lower_bound({query.key, 0}); break;
         case kind::upper_bound: candidate = positions.upper_bound({query.key, UINT64_MAX}); break;
         case kind::first: candidate = positions.begin(); break;
         case kind::last:
            reverse = true;
            if (!positions.empty()) candidate = std::prev(positions.end());
            break;
         case kind::next: candidate = positions.upper_bound({query.key, query.primary_key}); break;
         case kind::previous: {
            reverse = true;
            const auto bound = positions.lower_bound({query.key, query.primary_key});
            if (bound != positions.begin()) candidate = std::prev(bound);
            break;
         }
         default: throw primary_state_invalid_attempt();
      }
      if (candidate == positions.end()) continue;
      if (!selected.row) return status::dependency_conflict; // a prefix update filled an observed absence
      const secondary_position original{selected.row->key, selected.row->primary_key};
      if (reverse ? !position_less{}(*candidate, original) : !position_less{}(original, *candidate)) return status::dependency_conflict;
   }
   return status::reusable;
}

bool ordered_attempt_batch::remember_writes(const primary_overlay_result& state,
      const std::function<void()>& checkpoint) {
   if (state.writes().size() > max_batch_write_operations - write_operations_) return false;
   for (const auto& mutation : state.writes()) {
      // Caller interruption (including bad_alloc) is NOT a local bookkeeping
      // failure. Keep this outside the allocation catch and before any effects.
      if (checkpoint) checkpoint();
      if (identity_->stop.stop_requested()) return false;
      try {
         std::visit([&](const auto& write) {
            if (write.operation != primary_operation::update) structural_dirty_.insert(write.table);
            if constexpr (std::is_same_v<std::decay_t<decltype(write)>, primary_mutation>) {
               dirty_[write.table].insert(write.primary_key);
            } else {
               auto& changes = secondary_dirty_[write.table][static_cast<size_t>(secondary_kind(write.key))];
               changes.primary_keys.insert(write.primary_key);
               if (write.operation == primary_operation::update) changes.positions.insert({write.key, write.primary_key});
            }
         }, mutation);
      } catch (const std::bad_alloc&) {
         // Partial bookkeeping is harmless: the caller disables this whole
         // speculative tail before entering the original serial callback.
         return false;
      }
      ++write_operations_;
   }
   return true;
}

} // namespace eosio::chain::experimental
