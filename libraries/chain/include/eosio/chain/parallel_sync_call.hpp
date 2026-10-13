#pragma once

#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/config.hpp>
#include <thread>

namespace eosio::chain::experimental {

// Owner-captured protocol observations, not permission to adopt a result.
struct isolated_sync_context {
   uint32_t max_depth = config::default_max_sync_call_depth;
   uint32_t max_data_bytes = config::default_max_sync_call_data_size;
   bool root_read_only = false;
   bool get_sender_fix = false;
   bool console_enabled = false;
};

// The owner must capture the actual code_object bit. A private parser finding
// an export is NOT evidence that the canonical code object supports calls.
struct isolated_sync_program {
   isolated_receiver_program program;
   bool sync_call_supported = false;
};

// Owned input/catalog/protocol bundle. A worker job and its serial owner keep
// separate copies; this bundle alone is never a canonical replay capability.
struct isolated_sync_job {
   isolated_action_input root;
   std::vector<isolated_sync_program> programs;
   isolated_sync_context context;
};

struct isolated_sync_call_event {
   uint32_t call_ordinal = 0;
   size_t after_writes = 0;
   bool operator==(const isolated_sync_call_event&) const = default;
};
// Unlike queued inline actions, this child is replayed immediately in this
// stream, before the parent's next cursor operation or mutation.
using isolated_sync_event = std::variant<isolated_cursor_event, isolated_sync_call_event>;

struct isolated_sync_trace {
   uint32_t call_ordinal = 0, sender_ordinal = 0;
   name sender, receiver;
   uint64_t flags = 0;
   bool read_only = false;
   std::vector<char> data, return_value;
   int64_t status = 0; // normalized entry status, NOT call()'s returned byte count
   std::vector<uint32_t> console_markers;
   std::chrono::microseconds exclusive_elapsed{};
   bool executed = false; // missing account/code/export never enters a VM
   size_t writes_begin = 0, writes_end = 0; // includes descendant writes
   fc::sha256 execution_fingerprint;
   std::vector<isolated_sync_event> events;
   std::chrono::steady_clock::time_point began, ended; // local diagnostics only
};

// Separate type: existing action/notification adopters cannot consume this.
// Exceptions/unsupported hosts discard the WHOLE attempt; no partial trace or
// success prefix is returned. Formal replay needs a separately preflighted,
// stack-scoped whole-root capability; the result alone cannot authorize it.
struct isolated_sync_result {
   primary_overlay_result state;
   isolated_action_effects root;
   std::vector<isolated_sync_trace> calls;
   std::vector<uint32_t> root_console_markers;
   fc::sha256 execution_fingerprint;
   std::vector<isolated_sync_event> root_events;
};

// One ordinary root in a PRIVATE transaction-wide attempt. Call ordinals and
// cursors start afresh for each root; write positions refer to the shared
// transaction journal. No canonical adopter or worker job accepts this type.
struct isolated_sync_root_result {
   isolated_action_effects root;
   std::vector<isolated_sync_trace> calls;
   std::vector<uint32_t> root_console_markers;
   std::vector<isolated_sync_event> root_events;
};
struct isolated_sync_transaction_result {
   primary_overlay_result state;
   std::vector<isolated_sync_root_result> roots;
   fc::sha256 execution_fingerprint;
};

fc::sha256 isolated_sync_fingerprint(const isolated_action_input&, std::span<const isolated_sync_program>,
                                    isolated_sync_context);
fc::sha256 isolated_sync_transaction_fingerprint(std::span<const isolated_action_input>,
   std::span<const isolated_sync_program>, isolated_sync_context);
// Frozen-state structural proof only: NOT live-state, authorization, RAM or
// permission to adopt. Rebuilds immediate call order and independent cursors.
bool validate_isolated_sync_journal(const isolated_sync_result&, const isolated_action_input&,
   std::span<const isolated_sync_program>, isolated_sync_context, const std::function<void()>& checkpoint = {});
bool validate_isolated_sync_transaction_journal(const isolated_sync_transaction_result&,
   std::span<const isolated_action_input>, std::span<const isolated_sync_program>, isolated_sync_context,
   const std::function<void()>& checkpoint = {});

// Restricted subset: ordinary roots, immediate nested calls with primary
// reads/writes and per-frame cursors. Only the one-root result is connected to
// canonical adoption; the multi-root result remains private. Notifications, inline, printing and
// eosio_exit require whole-attempt fallback. Each LIVE frame owns a different backend,
// context and allocator, including recursive calls to the same contract.
// At most nine live modules / 9 MiB linear memory (VM metadata/mappings extra).
class isolated_sync_executor {
public:
   static constexpr size_t max_programs = 64, max_calls = 64, max_depth = 8;
   static constexpr size_t max_roots = 16;
   static constexpr size_t max_events = 4096;
   static constexpr size_t max_total_code_bytes = 256 * 1024, max_total_data_bytes = 256 * 1024;
   static constexpr std::chrono::milliseconds max_attempt_time{500};
   isolated_sync_executor() = default;
   isolated_sync_executor(const isolated_sync_executor&) = delete;
   isolated_sync_executor& operator=(const isolated_sync_executor&) = delete;
   isolated_sync_result execute(std::shared_ptr<const frozen_primary_state>, const isolated_action_input&,
      std::span<const isolated_sync_program>, isolated_sync_context,
      std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
   // Not connected to controller or formal adoption. All roots share state,
   // budgets and one deadline; any failure discards the ENTIRE attempt.
   isolated_sync_transaction_result execute_transaction(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_input>, std::span<const isolated_sync_program>, isolated_sync_context,
      std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   isolated_sync_transaction_result execute_roots(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_input>, std::span<const isolated_sync_program>, isolated_sync_context,
      std::stop_token, primary_overlay_limits, isolated_cancel, bool single_root_fingerprint);
   const std::thread::id owner_ = std::this_thread::get_id();
};

} // namespace eosio::chain::experimental
