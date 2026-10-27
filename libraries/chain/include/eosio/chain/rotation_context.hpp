#pragma once

#include <eosio/chain/producer_schedule.hpp>
#include <eosio/chain/finalizer_authority.hpp>

namespace eosio::chain {
// Wire ABI shared with eosio.system/rotation_state.hpp. All evidence is from
// the executed parent block, never the local vote processor/fork DB LIB.
struct rotation_context_v1 {
   uint32_t version = 1;
   uint32_t block_num = 0;
   uint32_t slot = 0;
   uint32_t final_block_num = 0;
   uint32_t final_slot = 0;
   uint32_t proposer_version = 0;
   uint32_t finalizer_generation = 0;
   uint32_t latest_finalizer_generation = 0;
   bool transitioning = false;
   std::vector<producer_authority> producers;
   uint64_t threshold = 0;
   std::vector<finalizer_authority> finalizers;
};
}
FC_REFLECT(eosio::chain::rotation_context_v1,
   (version)(block_num)(slot)(final_block_num)(final_slot)(proposer_version)
   (finalizer_generation)(latest_finalizer_generation)(transitioning)(producers)(threshold)(finalizers))
