#pragma once

#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/parallel_inline_action.hpp>
#include <eosio/chain/config.hpp>

namespace eosio::chain::experimental {

// Owned, immutable, bounded transaction input. Never retains packed_transaction
// or controller pointers. Copy and hash ONCE; all steps bind the same digest.
class isolated_context_free_data {
public:
   static constexpr size_t max_entries = 1024, max_bytes = 256 * 1024;
   static std::shared_ptr<const isolated_context_free_data> capture(std::span<const std::vector<char>>);
   const fc::sha256& fingerprint() const { return hash_; }
   int32_t read(uint32_t index, std::span<char> destination) const;
private:
   explicit isolated_context_free_data(std::span<const std::vector<char>>);
   const std::vector<std::vector<char>> data_;
   const fc::sha256 hash_;
};

// Caller-owned protocol observations, NOT permission to bypass original checks.
// The adopter must compare these to the current original configuration.
struct isolated_action_tree_context {
   uint32_t max_inline_action_size = config::default_max_inline_action_size;
   uint32_t max_inline_action_depth = config::default_max_inline_action_depth;
   // Null keeps the legacy private capability boundary. The canonical owner
   // captures packed data; adoption must independently verify the same input.
   std::shared_ptr<const isolated_context_free_data> context_free_data;
   bool console_enabled = false;
   bool operator==(const isolated_action_tree_context&) const = default;
};

struct isolated_tree_inline_event {
   isolated_inline_action sent;
   size_t after_writes = 0;
   uint32_t action_ordinal = 0; // assigned in host-event order, NOT execution order
   bool context_free = false;
};
// Do not extend the existing notification journal: its adopter cannot accept
// inline events. This separate result type requires its own canonical adopter.
using isolated_action_tree_event = std::variant<isolated_cursor_event, isolated_tree_inline_event>;

struct isolated_action_tree_group {
   size_t root_index = 0;
   uint32_t action_ordinal = 0, creator_action_ordinal = 0;
   uint32_t closest_unnotified_ancestor = 0, depth = 0;
   name sender;
   isolated_action_input input;
   bool context_free = false;
};

struct isolated_action_tree_step {
   size_t group_index = 0;
   uint32_t action_ordinal = 0, creator_action_ordinal = 0, closest_unnotified_ancestor = 0;
   name receiver, sender;
   fc::sha256 code_hash;
   bool has_code = false;
   isolated_action_effects effects;
   std::vector<isolated_action_tree_event> events;
};

struct isolated_action_tree_result {
   primary_overlay_result state;
   std::vector<isolated_action_tree_group> groups; // scheduling order
   std::vector<isolated_action_tree_step> steps;   // original execution order
   fc::sha256 root_fingerprint;
   std::shared_ptr<const isolated_context_free_data> context_free_data;
};

fc::sha256 isolated_action_tree_fingerprint(std::span<const fc::sha256> roots,
                                          const isolated_action_tree_context&);
fc::sha256 isolated_action_tree_step_fingerprint(const fc::sha256& action_fingerprint,
   const isolated_action_tree_context&, const isolated_action_tree_group&, const isolated_action_tree_step&);

// Structural frozen-state preflight only, NOT authorization or current-state
// validation and NOT a second execution of the contract business logic. Rebuild
// scheduling and cursor/write journals, bind raw sends to child inputs, and
// require every replay dependency to be covered by the original private proof.
// The owner must separately check live code/native handlers, current protocol
// context and matches_base_reads, and replay ORIGINAL send-time authorization.
// Owner checkpoint exceptions propagate; invalid private replay returns false.
bool validate_isolated_action_tree_journal(const isolated_action_tree_result&,
   std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
   isolated_action_tree_context, const std::function<void()>& checkpoint = {});

// PRIVATE ONLY. No original DB/permission manager/controller is reachable here.
// send_inline logs the exact owned bytes; account/permission/eosio.code checks
// have NOT run. Success is NOT an authorized transaction or permission to adopt.
// Original send_inline must eventually be replayed at the exact send position.
class isolated_action_tree_executor {
public:
   static constexpr size_t max_programs = 256, max_steps = 256;
   static constexpr size_t max_events = 4096, max_depth = 16;
   static constexpr size_t max_total_inline_bytes = 256 * 1024;
   isolated_action_tree_executor();
   explicit isolated_action_tree_executor(std::shared_ptr<isolated_module_cache>);
   ~isolated_action_tree_executor();
   isolated_action_tree_executor(const isolated_action_tree_executor&) = delete;
   isolated_action_tree_executor& operator=(const isolated_action_tree_executor&) = delete;
   isolated_action_tree_result execute(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
      isolated_action_tree_context, std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
