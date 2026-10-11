#include <eosio.system/eosio.system.hpp>

namespace eosiosystem {

void system_contract::require_direct_vote_policy() const {
   direct_vote_policy_singleton policy(get_self(), get_self().value);
   check(policy.exists() && policy.get().version == 2,
         "single-choice voting requires fresh-chain initialization; migration is not supported");
}

void system_contract::adjust_direct_producer(const name& producer, int64_t delta) {
   if (delta == 0) return;
   const auto itr = _producers.require_find(producer.value, "integer vote producer record missing");
   _producers.modify(itr, same_payer, [&](auto& row) {
      row.total_votes = checked_vote_add(row.total_votes, delta);
   });
   _gstate.total_producer_vote_weight = checked_vote_add(_gstate.total_producer_vote_weight, delta);
#ifdef PUREEOS_SEQUENTIAL_ROTATION
   refresh_rotation_candidate(producer);
#endif

   // Voting only updates eligibility/ranking. No vote-pay shares are accrued.
}

void system_contract::apply_direct_vote(const name& owner, const std::vector<name>& selected) {
   const auto itr = _voters.require_find(owner.value, "voter record missing");
   check_direct_vote_state(*itr);
   check(itr->staked >= 0 && itr->last_vote_amount >= 0, "negative integer vote amount");
   const name old_target = itr->producers.empty() ? name{} : itr->producers.front();
   const name new_target = selected.empty() ? name{} : selected.front();
   const int64_t old_amount = itr->last_vote_amount;
   const int64_t new_amount = new_target ? itr->staked : 0;
   check(old_target || old_amount == 0, "unselected voter has allocated votes");
   if (old_target == new_target) {
      if (new_target) adjust_direct_producer(new_target, new_amount - old_amount);
   } else {
      if (old_target) adjust_direct_producer(old_target, -old_amount);
      if (new_target) adjust_direct_producer(new_target, new_amount);
   }
   _voters.modify(itr, same_payer, [&](auto& row) {
      row.producers = selected;
      row.last_vote_amount = new_amount;
   });
   // A separate, exact activation measure; no repeat-vote accumulation and no
   // balance is activated merely by making a zero-token selection.
   _gstate.total_activated_stake = _gstate.total_producer_vote_weight;
   // The first positive allocation starts rewards, even at one smallest unit.
   // Use the same path as onblock so a reviewed upgrade also activates an
   // already-voted, previously sub-threshold chain without another user vote.
   sync_reward_policy();
}

void system_contract::check_direct_vote_state(const voter_info& voter) const {
   check(!voter.proxy && !voter.is_proxy && voter.proxied_vote_weight == 0,
         "proxy voting is disabled");
   check(voter.producers.size() <= 1, "legacy multi-vote state requires migration");
}

void system_contract::require_direct_vote_target(const name& producer) const {
   const auto itr = _producers.find(producer.value);
   check(itr != _producers.end() && itr->active(), "vote target must be an active registered producer");
   const bool usable = std::visit([](const auto& auth) {
      uint64_t weight = 0;
      for (const auto& key : auth.keys) {
         if (key.key == eosio::public_key{}) return false;
         weight += key.weight;
      }
      return auth.threshold > 0 && weight >= auth.threshold;
   }, itr->get_producer_authority());
   check(usable, "vote target must have a usable block signing authority");
}

bool system_contract::has_rex_shares(const name& owner) const {
   const auto itr = _rexbalance.find(owner.value);
   // Includes immature, savings and queued-to-sell shares. Neither vote_stake
   // rounding nor submitting a sell order can make a holding disappear.
   return itr != _rexbalance.end() && itr->rex_balance.amount > 0;
}

void system_contract::require_rex_vote(const name& owner) const {
   require_direct_vote_policy();
   const auto voter = _voters.find(owner.value);
   check(voter != _voters.end(), "select one producer before buying REX");
   check_direct_vote_state(*voter);
   check(voter->producers.size() == 1, "select one producer before buying REX");
   require_direct_vote_target(voter->producers.front());
}

} // namespace eosiosystem
