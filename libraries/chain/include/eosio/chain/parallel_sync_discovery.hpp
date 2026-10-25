#pragma once

#include <eosio/chain/parallel_state_overlay.hpp>

namespace eosio::chain::experimental {

// Internal discovery signal, not a capability to execute or adopt code. Only
// an actual synchronous call may request its program alongside an UNKNOWN
// account. Keep the base exception contract for standalone executor callers.
// The joined owner must still check existence, eligibility and all budgets.
struct isolated_sync_target_missing final : primary_state_missing {
   const name receiver;
   explicit isolated_sync_target_missing(name target)
      : primary_state_missing(target), receiver(target) {}
};

} // namespace eosio::chain::experimental
