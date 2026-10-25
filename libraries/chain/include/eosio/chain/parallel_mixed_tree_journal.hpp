#pragma once

#include <eosio/chain/parallel_mixed_tree.hpp>

namespace eosio::chain::experimental {

// Frozen-state STRUCTURAL proof only. This does not execute the WASM again,
// authorize sends, validate live state or grant canonical replay capability.
// One overlay spans every queued action and immediate call. Ordinary receivers
// share their group's cursor; sync children and new groups have fresh cursors.
// Owner checkpoint exceptions propagate, including from nested call replay.
bool validate_isolated_mixed_tree_journal(const isolated_mixed_tree_result&,
   std::span<const isolated_action_input>, std::span<const isolated_sync_program>,
   const isolated_mixed_tree_context&, const std::function<void()>& checkpoint = {});

} // namespace eosio::chain::experimental
