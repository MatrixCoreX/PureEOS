#pragma once

#include <cstdint>
#include <vector>

namespace eosiosystem::rotation {

// Standalone reference model for the original 210-entry FIFO and handoff tests.
// Production rotation v2 uses the paginated rotcands queue in rotation_state.cpp
// and candidate_capacity() in rotation_growth.hpp; its historical preflight
// states below are NOT an online probe/approval path in the deployed contract.
using account_id = uint64_t;
constexpr uint32_t candidate_limit = 210;
constexpr uint32_t committee_limit = 21;
constexpr uint32_t producer_slots = 12;
constexpr uint32_t normal_epoch_slots = committee_limit * producer_slots;

// Zero means an invalid committee size, not a quorum permitting empty policies.
constexpr uint32_t finalizer_threshold(uint32_t size) {
   return size >= 1 && size <= committee_limit ? size * 2 / 3 + 1 : 0;
}

enum class phase : uint8_t { idle, scanning, preflight, ready, submitted, observing };
enum class reason : uint8_t { none, growth, rotation, fault, ineligible };
enum class error : uint8_t {
   none, not_initialized, busy, wrong_phase, invalid_members, invalid_snapshot,
   invalid_faults, invalid_budget, invalid_policy, not_active,
   observation_incomplete, slot_regression, unverified_fault, invalid_checkpoint
};

struct policy_versions {
   uint32_t proposer = 0;
   uint32_t finalizer = 0;
};

struct state {
   // queue.front() is the next position. Rotating the vector preserves the
   // cursor even when ranked membership changes between election snapshots.
   std::vector<account_id> queue;
   std::vector<account_id> snapshot;
   // Tenure order, NOT the name/key ordering of the underlying policies.
   // active is the last observed COMMON proposer/finalizer membership. While
   // submitted, neither this nor confirmed may replace host policy queries.
   std::vector<account_id> active;
   std::vector<account_id> confirmed;
   std::vector<account_id> target;
   phase stage = phase::idle;
   reason cause = reason::none;
   account_id outgoing = 0;
   account_id incoming = 0;
   uint32_t scan_remaining = 0;
   policy_versions proposed;
   bool active_policies_match = false;
   uint64_t active_since_slot = 0;
};

struct scan_result {
   error status = error::none;
   account_id candidate = 0;
   uint32_t inspected = 0;
};

class scheduler {
public:
   const state& get_state() const { return value; }

   // Genesis membership may have no votes yet. Empty/duplicate/null or >21
   // memberships are rejected. Initialization can only happen once.
   error initialize(const std::vector<account_id>& initial_members);

   // Input is an already qualified, integer-vote-ranked top-210 snapshot;
   // ties are resolved by account ID by the ranking adapter, NOT here.
   // confirmed_faults must come from verified evidence, never a caller's ping.
   error begin_round(const std::vector<account_id>& ranked_candidates,
                     const std::vector<account_id>& confirmed_faults = {});
   scan_result scan(uint32_t budget);
   error reject_candidate(); // Also permits revalidation failure before submit.
   error accept_candidate(); // Readiness must already have been verified.
   error mark_submitted(policy_versions versions);

   // Call only with actual active policies from the deterministic host view.
   // Matching account sets without matching policy versions is insufficient.
   error observe_active(policy_versions versions,
                        const std::vector<account_id>& proposers,
                        const std::vector<account_id>& finalizers,
                        uint64_t consensus_slot);
   error complete_round(uint64_t consensus_slot, bool full_window_observed,
                        bool verified_health_passed);
   error retry_failed_admission(bool verified_fault);
   error cancel_before_submission();

   // Checkpoint fields have no EOSIO ABI yet. This validates model restoration;
   // the future table adapter must add versioned serialization and chain tests.
   error restore(const state& checkpoint);

private:
   state value;
   void clear_round();
};

} // namespace eosiosystem::rotation
