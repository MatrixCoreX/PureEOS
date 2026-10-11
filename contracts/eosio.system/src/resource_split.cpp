#include <eosio.system/eosio.system.hpp>
#include <algorithm>
#include <limits>

namespace eosiosystem {
void update_weight(time_point_sec now, powerup_state_resource& res, int64_t& delta_available);
void update_utilization(time_point_sec now, powerup_state_resource& res);

resource_policy system_contract::require_resource_policy() const {
   resource_policy_singleton policy(get_self(), get_self().value);
   check(policy.exists(), "resource split requires fresh-chain initialization; migration is not supported");
   auto state = policy.get();
   check(state.version == 1, "unsupported resource split policy");
   return state;
}

void system_contract::update_longterm_resources(name receiver, int64_t net_delta, int64_t cpu_delta) {
   auto policy = require_resource_policy();
   check(receiver != reserve_account, "resource reserve cannot receive staking or REX loans");
   auto voter = _voters.find(receiver.value);
   if (voter != _voters.end()) {
      check(!has_field(voter->flags1, voter_info::flags1_fields::net_managed) &&
            !has_field(voter->flags1, voter_info::flags1_fields::cpu_managed),
            "managed CPU/NET is incompatible with resource split");
   }
   auto add = [](int64_t value, int64_t delta) {
      const int128_t next = int128_t(value) + delta;
      // Bound L + 9L as well as the individual PowerUp market weight.
      check(next >= 0 && next <= std::numeric_limits<int64_t>::max() / 10,
            "long-term resource weight out of range");
      return int64_t(next);
   };
   policy.net_weight = add(policy.net_weight, net_delta);
   policy.cpu_weight = add(policy.cpu_weight, cpu_delta);
   resource_policy_singleton(get_self(), get_self().value).set(policy, get_self());

   powerup_state_singleton market(get_self(), 0);
   if (!market.exists()) return; // Bootstrap must cfgpowerup before governance sealing.
   auto state = market.get();
   check(state.version == 1, "PowerUp resource split version mismatch");
   const time_point_sec now = eosio::current_time_point();
   auto sync = [&](powerup_state_resource& resource, int64_t weight) {
      update_utilization(now, resource);
      // A one-unit floor keeps an empty market usable without dividing by zero.
      resource.assumed_stake_weight = std::max(weight, int64_t{1});
      int64_t delta = 0;
      update_weight(now, resource, delta);
      return delta;
   };
   const auto net = sync(state.net, policy.net_weight);
   const auto cpu = sync(state.cpu, policy.cpu_weight);
   adjust_resources(get_self(), reserve_account, get_core_symbol(), net, cpu, true);
   market.set(state, get_self());
}
} // namespace eosiosystem
