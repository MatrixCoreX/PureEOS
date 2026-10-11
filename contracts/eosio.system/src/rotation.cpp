#include <eosio.system/rotation.hpp>

#include <algorithm>

namespace eosiosystem::rotation {
namespace {

bool contains(const std::vector<account_id>& accounts, account_id account) {
   return std::find(accounts.begin(), accounts.end(), account) != accounts.end();
}

bool valid_accounts(const std::vector<account_id>& accounts, uint32_t maximum, bool allow_empty) {
   if ((!allow_empty && accounts.empty()) || accounts.size() > maximum)
      return false;
   for (auto it = accounts.begin(); it != accounts.end(); ++it) {
      if (*it == 0 || std::find(accounts.begin(), it, *it) != it)
         return false;
   }
   return true;
}

bool same_members(const std::vector<account_id>& a, const std::vector<account_id>& b) {
   return a.size() == b.size() &&
          std::all_of(a.begin(), a.end(), [&](account_id n) { return contains(b, n); });
}

std::vector<account_id> target_members(const state& s) {
   auto result = s.active;
   if (s.outgoing != 0)
      result.erase(std::find(result.begin(), result.end(), s.outgoing));
   result.push_back(s.incoming);
   return result;
}

bool valid_checkpoint(const state& s) {
   if (!valid_accounts(s.active, committee_limit, false) ||
       !valid_accounts(s.confirmed, committee_limit, false) ||
       !valid_accounts(s.queue, candidate_limit, true) ||
       !valid_accounts(s.snapshot, candidate_limit, true) ||
       !valid_accounts(s.target, committee_limit, true) || s.scan_remaining > s.queue.size())
      return false;

   if (s.stage == phase::idle)
      return s.snapshot.empty() && s.target.empty() && s.cause == reason::none &&
             s.outgoing == 0 && s.incoming == 0 && s.scan_remaining == 0 &&
             s.proposed.proposer == 0 && s.proposed.finalizer == 0 &&
             !s.active_policies_match && s.active_since_slot == 0;

   if (s.cause != reason::growth && s.cause != reason::rotation &&
       s.cause != reason::fault && s.cause != reason::ineligible)
      return false;
   if (s.snapshot.empty() || !same_members(s.snapshot, s.queue))
      return false;
   if (s.cause == reason::growth) {
      if (s.outgoing != 0 || (s.stage != phase::observing && s.active.size() >= committee_limit))
         return false;
   } else if (s.outgoing == 0 ||
              (s.stage != phase::observing && !contains(s.active, s.outgoing))) {
      return false;
   }
   if (s.cause == reason::rotation &&
       (s.active.size() != committee_limit ||
        (s.stage != phase::observing && s.outgoing != s.active.front())))
      return false;
   if (s.cause == reason::ineligible && contains(s.snapshot, s.outgoing))
      return false;
   if (s.stage != phase::scanning &&
       (s.queue.back() != s.incoming || s.scan_remaining >= s.queue.size()))
      return false;

   switch (s.stage) {
   case phase::scanning:
      if (s.incoming != 0 || !s.target.empty() || s.scan_remaining == 0)
         return false;
      break;
   case phase::preflight:
   case phase::ready:
   case phase::submitted:
      if (!contains(s.snapshot, s.incoming) || contains(s.active, s.incoming))
         return false;
      if (s.stage == phase::preflight ? !s.target.empty() : s.target != target_members(s))
         return false;
      break;
   case phase::observing:
      if (s.target != s.active || !contains(s.snapshot, s.incoming) ||
          s.active.back() != s.incoming || contains(s.active, s.outgoing))
         return false;
      break;
   default:
      return false;
   }

   if (s.stage == phase::submitted || s.stage == phase::observing) {
      if (s.proposed.proposer == 0 || s.proposed.finalizer == 0)
         return false;
   } else if (s.proposed.proposer != 0 || s.proposed.finalizer != 0) {
      return false;
   }
   if (s.stage != phase::observing && s.active_policies_match)
      return false;
   return s.active_policies_match || s.active_since_slot == 0;
}

} // namespace

void scheduler::clear_round() {
   value.snapshot.clear();
   value.target.clear();
   value.stage = phase::idle;
   value.cause = reason::none;
   value.outgoing = value.incoming = 0;
   value.scan_remaining = 0;
   value.proposed = {};
   value.active_policies_match = false;
   value.active_since_slot = 0;
}

error scheduler::initialize(const std::vector<account_id>& initial_members) {
   if (!value.active.empty())
      return error::busy;
   if (!valid_accounts(initial_members, committee_limit, false))
      return error::invalid_members;
   value.active = value.confirmed = initial_members;
   return error::none;
}

error scheduler::begin_round(const std::vector<account_id>& ranked_candidates,
                             const std::vector<account_id>& confirmed_faults) {
   if (value.active.empty())
      return error::not_initialized;
   if (value.stage != phase::idle)
      return error::busy;
   if (!valid_accounts(ranked_candidates, candidate_limit, true))
      return error::invalid_snapshot;
   if (!valid_accounts(confirmed_faults, committee_limit, true) ||
       !std::all_of(confirmed_faults.begin(), confirmed_faults.end(),
                    [&](account_id n) { return contains(value.active, n); }))
      return error::invalid_faults;

   // Preserve the next-position order of all surviving candidates. Append only
   // genuinely new members, in deterministic rank order supplied by the caller.
   auto& queue = value.queue;
   queue.erase(std::remove_if(queue.begin(), queue.end(),
                             [&](account_id n) { return !contains(ranked_candidates, n); }), queue.end());
   for (account_id n : ranked_candidates) {
      if (!contains(queue, n))
         queue.push_back(n);
   }
   if (queue.empty())
      return error::none; // Never propose an empty committee.

   value.snapshot = ranked_candidates;
   value.scan_remaining = static_cast<uint32_t>(queue.size());
   value.stage = phase::scanning;
   // Faults and lost qualifications precede growth or ordinary FIFO rotation.
   // Among abnormal incumbents the earliest admitted is handled first.
   for (account_id n : value.active) {
      if (contains(confirmed_faults, n) || !contains(ranked_candidates, n)) {
         value.outgoing = n;
         value.cause = contains(confirmed_faults, n) ? reason::fault : reason::ineligible;
         return error::none;
      }
   }
   if (value.active.size() < committee_limit) {
      value.cause = reason::growth;
   } else {
      value.cause = reason::rotation;
      value.outgoing = value.active.front();
   }
   return error::none;
}

scan_result scheduler::scan(uint32_t budget) {
   if (value.stage != phase::scanning)
      return {error::wrong_phase, 0, 0};
   if (budget == 0 || budget > candidate_limit)
      return {error::invalid_budget, 0, 0};
   scan_result result;
   while (result.inspected < budget && value.scan_remaining > 0) {
      const auto candidate = value.queue.front();
      std::rotate(value.queue.begin(), value.queue.begin() + 1, value.queue.end());
      --value.scan_remaining;
      ++result.inspected;
      if (!contains(value.active, candidate)) {
         value.incoming = candidate;
         value.stage = phase::preflight;
         result.candidate = candidate;
         return result;
      }
   }
   if (value.scan_remaining == 0)
      clear_round();
   return result;
}

error scheduler::reject_candidate() {
   if (value.stage != phase::preflight && value.stage != phase::ready)
      return error::wrong_phase;
   value.incoming = 0;
   value.target.clear();
   value.stage = phase::scanning;
   if (value.scan_remaining == 0)
      clear_round();
   return error::none;
}

error scheduler::accept_candidate() {
   if (value.stage != phase::preflight)
      return error::wrong_phase;
   value.target = target_members(value);
   value.stage = phase::ready;
   return error::none;
}

error scheduler::mark_submitted(policy_versions versions) {
   if (value.stage != phase::ready)
      return error::wrong_phase;
   if (versions.proposer == 0 || versions.finalizer == 0)
      return error::invalid_policy;
   value.proposed = versions;
   value.stage = phase::submitted;
   return error::none;
}

error scheduler::observe_active(policy_versions versions,
                                const std::vector<account_id>& proposers,
                                const std::vector<account_id>& finalizers,
                                uint64_t consensus_slot) {
   if (value.stage != phase::submitted && value.stage != phase::observing)
      return error::wrong_phase;
   if (!valid_accounts(proposers, committee_limit, false) ||
       !valid_accounts(finalizers, committee_limit, false))
      return error::invalid_members;
   if (versions.proposer != value.proposed.proposer || versions.finalizer != value.proposed.finalizer ||
       !same_members(proposers, value.target) || !same_members(finalizers, value.target)) {
      // Never complete using a stale earlier match after a conflicting policy
      // observation. A later exact match must start a new observation interval.
      value.active_policies_match = false;
      value.active_since_slot = 0;
      return error::not_active;
   }
   if (value.stage == phase::observing && value.active_policies_match) {
      // Repeated observations must not restart the observation interval.
      return consensus_slot >= value.active_since_slot ? error::none : error::slot_regression;
   }
   value.active = value.target;
   value.active_policies_match = true;
   value.active_since_slot = consensus_slot;
   value.stage = phase::observing;
   return error::none;
}

error scheduler::complete_round(uint64_t consensus_slot, bool full_window_observed,
                                bool verified_health_passed) {
   if (value.stage != phase::observing)
      return error::wrong_phase;
   if (!value.active_policies_match)
      return error::not_active;
   if (consensus_slot < value.active_since_slot)
      return error::slot_regression;
   if (consensus_slot - value.active_since_slot < value.active.size() * producer_slots ||
       !full_window_observed || !verified_health_passed)
      return error::observation_incomplete;
   value.confirmed = value.active;
   clear_round();
   return error::none;
}

error scheduler::retry_failed_admission(bool verified_fault) {
   if (value.stage != phase::observing)
      return error::wrong_phase;
   if (!value.active_policies_match)
      return error::not_active;
   if (!verified_fault)
      return error::unverified_fault;
   // Both policies were observed active: do not roll back to the previous
   // membership. Keep the other k-1 members and replace only the failed entrant.
   value.outgoing = value.incoming;
   value.incoming = 0;
   value.target.clear();
   value.cause = reason::fault;
   value.proposed = {};
   value.active_policies_match = false;
   value.active_since_slot = 0;
   value.stage = phase::scanning;
   if (value.scan_remaining == 0)
      clear_round();
   return error::none;
}

error scheduler::cancel_before_submission() {
   if (value.stage != phase::scanning && value.stage != phase::preflight && value.stage != phase::ready)
      return error::wrong_phase;
   clear_round();
   return error::none;
}

error scheduler::restore(const state& checkpoint) {
   if (!valid_checkpoint(checkpoint))
      return error::invalid_checkpoint;
   value = checkpoint;
   return error::none;
}

} // namespace eosiosystem::rotation
