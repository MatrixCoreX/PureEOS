#include <eosio/chain/parallel_ordered_batch.hpp>
#include <eosio/chain/performance_metrics.hpp>

namespace eosio::chain::experimental {

ordered_attempt_batch::ordered_attempt_batch(std::shared_ptr<const frozen_primary_state> base,
      std::span<const request> transactions) {
   if (!base || transactions.empty() || transactions.size() > 64) throw primary_state_budget_exceeded();
   for (const auto& item : transactions)
      if (item.transaction_id == fc::sha256{} || item.execution_fingerprint == fc::sha256{})
         throw primary_state_invalid_attempt();
   identity_ = std::make_shared<identity>();
   identity_->base = std::move(base);
   identity_->transactions.assign(transactions.begin(), transactions.end());
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
   if (closed_ || index >= identity_->transactions.size()) throw primary_state_invalid_attempt();
   return {identity_, index};
}

size_t ordered_attempt_batch::next_index() const { check_owner(); return next_; }
bool ordered_attempt_batch::complete() const { check_owner(); return next_ == identity_->transactions.size(); }

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
       next_ >= identity_->transactions.size() || expected_transaction != identity_->transactions[next_].transaction_id)
      throw primary_state_invalid_attempt();
}

bool ordered_attempt_batch::can_use(const primary_overlay_result& state, const fc::sha256& fingerprint) const {
   if (identity_->stop.stop_requested()) return false;
   if (state.base() != identity_->base || fingerprint != identity_->transactions[next_].execution_fingerprint)
      throw primary_state_invalid_attempt();
   auto conflict = [] {
      if (performance::metrics().enabled.load(std::memory_order_relaxed))
         performance::metrics().record(performance::stage::parallel_conflict, 0);
      return false;
   };
   for (const auto& table : state.read_tables())
      if (structural_dirty_.contains(table)) return conflict();
   for (const auto& query : state.read_queries()) {
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
      return conflict();
   }
   return true;
}

} // namespace eosio::chain::experimental
