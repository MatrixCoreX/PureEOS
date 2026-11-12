#pragma once

#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_sync_call.hpp>

namespace eosio::chain::experimental {

struct isolated_mixed_tree_context {
   isolated_action_tree_context tree;
   isolated_sync_context sync;
};

// Complete, owned worker input. This is not a canonical replay capability.
struct isolated_mixed_tree_job {
   std::vector<isolated_action_input> roots;
   std::vector<isolated_sync_program> programs;
   isolated_mixed_tree_context context;
};

// A distinct event/result type: legacy tree/sync adopters cannot consume it.
// Queued sends and immediate calls retain their positions among cursor events.
using isolated_mixed_tree_event = std::variant<isolated_cursor_event,
   isolated_tree_inline_event, isolated_sync_call_event>;

struct isolated_mixed_tree_step {
   size_t group_index = 0;
   uint32_t action_ordinal = 0, creator_action_ordinal = 0, closest_unnotified_ancestor = 0;
   name receiver, sender;
   fc::sha256 code_hash;
   bool has_code = false;
   isolated_action_effects effects;
   std::vector<isolated_mixed_tree_event> events;
   std::vector<isolated_sync_trace> calls; // local to THIS action trace
   std::vector<uint32_t> console_markers;
};

struct isolated_mixed_tree_result {
   primary_overlay_result state;
   std::vector<isolated_action_tree_group> groups;
   std::vector<isolated_mixed_tree_step> steps;
   fc::sha256 execution_fingerprint;
   std::shared_ptr<const isolated_context_free_data> context_free_data;
};

fc::sha256 isolated_mixed_tree_fingerprint(std::span<const isolated_action_input>,
   std::span<const isolated_sync_program>, const isolated_mixed_tree_context&);

// Shared by private execution, frozen proof and canonical preflight. Preserve
// the existing encoding; receiver/code/input are bound by action_fingerprint.
fc::sha256 isolated_mixed_tree_step_fingerprint(const fc::sha256& action_fingerprint,
   const fc::sha256& transaction_fingerprint, const isolated_mixed_tree_context&,
   const isolated_action_tree_group&, const isolated_mixed_tree_step&);

// Private execution entry. Canonical adoption needs its own current-state
// proof and stack-scoped replay capability; this executor grants neither.
// Private sends have NOT run original permission/RAM checks. A successful
// result is NOT authority, a frozen structural proof, or permission to commit.
// Each executor owns at most nine live+dormant modules in total, not nine per
// inline group. Idle exact-code/limits instances may move between depths; live
// ancestors are never reused. The overlay, deadline and budgets span the tx.
class isolated_mixed_tree_executor {
public:
   static constexpr size_t max_roots = isolated_sync_executor::max_roots;
   static constexpr size_t max_programs = isolated_sync_executor::max_programs;
   static constexpr size_t max_steps = isolated_action_tree_executor::max_steps;
   static constexpr size_t max_events = isolated_action_tree_executor::max_events;
   static constexpr size_t max_calls = isolated_sync_executor::max_calls;
   static constexpr size_t max_data_bytes = isolated_sync_executor::max_total_data_bytes;
   static constexpr size_t max_code_bytes = isolated_sync_executor::max_total_code_bytes;
   static constexpr auto max_attempt_time = isolated_sync_executor::max_attempt_time;
   isolated_mixed_tree_executor();
   explicit isolated_mixed_tree_executor(std::shared_ptr<isolated_module_cache>);
   ~isolated_mixed_tree_executor();
   isolated_mixed_tree_executor(const isolated_mixed_tree_executor&) = delete;
   isolated_mixed_tree_executor& operator=(const isolated_mixed_tree_executor&) = delete;
   isolated_mixed_tree_result execute(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_input>, std::span<const isolated_sync_program>,
      isolated_mixed_tree_context, std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
