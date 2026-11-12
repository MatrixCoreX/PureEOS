// Compile-only contract check against the REAL sealed overlay. This does not
// execute Chainbase, authenticate a worker result or grant owner coverage.
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/parallel_work_footprint.hpp>

eosio::chain::canonical_work_footprint compile_real_overlay_work_adapter(
      const eosio::chain::experimental::primary_overlay_result& result,
      eosio::chain::canonical_work_coverage owner_coverage,
      std::span<const eosio::chain::owner_work_observation> owner_accesses) {
   return eosio::chain::make_overlay_work_footprint(result,owner_coverage,owner_accesses);
}
