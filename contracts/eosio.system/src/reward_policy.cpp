#include <eosio.system/eosio.system.hpp>

namespace eosiosystem {
void system_contract::sync_reward_policy() {
   reward_policy_singleton policy(get_self(), get_self().value);
   const bool exists = policy.exists();
   auto state = policy.get_or_default();
   check(state.version == 1 && state.halving_seconds == rewards::halving_usecs / 1000000 &&
         state.initial_continuous_rate == rewards::initial_continuous_rate, "unsupported reward policy");
   bool changed = !exists;
   const auto now = eosio::current_time_point();
   // No fixed-supply/15% threshold for integer-v2 chains. A funded direct vote
   // is required: registration, liquid balance and zero-weight selections do
   // not activate rewards. This is a one-time clock, not a recurring turnout
   // requirement, so vote removal or rank changes cannot restart halving.
   if (_gstate.thresh_activated_stake_time == time_point() && _gstate.total_producer_vote_weight > 0)
      _gstate.thresh_activated_stake_time = now;
   if (state.start_time == time_point() && _gstate.thresh_activated_stake_time != time_point()) {
      // Existing integer-v2 chains retain their original activation clock.
      // Do not reset either the clock or historical funded reward buckets on upgrade.
      state.start_time = _gstate.thresh_activated_stake_time;
      changed = true;
   }
   check(state.start_time <= now, "reward activation time is in the future");
   const uint32_t era = state.start_time == time_point() ? 0 : rewards::era_at(
      uint64_t(state.start_time.time_since_epoch().count()), uint64_t(now.time_since_epoch().count()));
   if (state.current_era != era) {
      state.current_era = era;
      changed = true;
   }
   if (changed) policy.set(state, get_self());
   // global4 remains a useful RPC view, but is not a mutable policy override.
   _gstate4.continuous_rate = rewards::rate_at(era);
   _gstate4.inflation_pay_factor = pay_factor_precision; // all NEW issuance goes to nodes
   _gstate4.votepay_factor = rewards::block_pay_factor; // compatibility view: 100% block pay
}

double system_contract::accrue_halving_rewards(int64_t supply, time_point from, time_point to) const {
   reward_policy_singleton policy(get_self(), get_self().value);
   const auto state = policy.get();
   check(state.version == 1 && state.start_time != time_point(), "rewards are not activated");
   check(supply >= 0 && from >= time_point() && to >= from, "invalid reward accrual interval");
   return rewards::accrued(supply, uint64_t(state.start_time.time_since_epoch().count()),
                          uint64_t(from.time_since_epoch().count()), uint64_t(to.time_since_epoch().count()));
}
} // namespace eosiosystem
