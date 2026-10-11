#pragma once

#include <eosio/chain/parallel_state_overlay.hpp>
#include <fc/crypto/sha256.hpp>
#include <chrono>
#include <atomic>
#include <set>
#include <variant>
#include <stop_token>

namespace eosio::chain::experimental {

// An owned atomic generation bridges the existing controller interrupt route
// without sending signals to workers or retaining any controller/DB pointer.
struct isolated_cancel {
   std::shared_ptr<const std::atomic<uint64_t>> generation;
   uint64_t observed = 0;
   // One absolute budget for the WHOLE production batch, including queued jobs.
   // Validation leaves it unlimited and uses the per-attempt watchdog instead.
   std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
   bool stop_requested() const {
      return (generation && generation->load(std::memory_order_relaxed) != observed) ||
             (deadline != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= deadline);
   }
};

// Restricted capability. No controller, database, permission manager, receipts,
// resources, signals or native action handlers are accessible to this host.
// A caller must later validate code/context/dependencies before ordered adoption.
struct isolated_action_input {
   name receiver;
   name account;
   name action;
   std::vector<std::pair<name, name>> authorizations;
   std::vector<char> data;
   uint32_t max_return_bytes = 256;
};

struct isolated_wasm_limits {
   uint32_t max_pages = 16;
   uint32_t max_call_depth = 64;
   size_t max_code_bytes = 64 * 1024;
   size_t max_action_bytes = 64 * 1024;
   size_t max_authorizations = 64;
   uint32_t max_return_bytes = 4096;
   std::chrono::milliseconds max_attempt_time{500};
   bool operator==(const isolated_wasm_limits&) const = default;
};

struct isolated_notification {
   name recipient;
   // Schedule at the same point relative to canonical writes. A later RAM
   // failure must preserve the original already-scheduled notification traces.
   size_t after_writes = 0;
   bool operator==(const isolated_notification&) const = default;
};

// PRIVATE notification replay preparation. Record every host operation which
// can change the shared primary iterator cache, in the same stream as writes
// and recipient scheduling. db_get does not allocate/change cache entries and
// stays covered by the ordinary state dependencies, not a cursor event.
enum class isolated_cursor_query_kind : uint8_t { find, lower_bound, upper_bound, end, next, previous };
struct isolated_cursor_query {
   isolated_cursor_query_kind kind = isolated_cursor_query_kind::find;
   table_key table{};              // find/lower/upper/end only
   uint64_t key = 0;               // find/lower/upper only
   int32_t iterator = -1;          // next/previous input handle
   int32_t result = -1;            // exact returned handle, including sentinels
   uint64_t primary_before = 0, primary_after = 0; // adjacent in/out argument
   bool operator==(const isolated_cursor_query&) const = default;
};
struct isolated_cursor_write {
   size_t write_index = 0;         // absolute index in the transaction journal
   int32_t iterator = -1;          // store result OR update/remove input handle
   bool operator==(const isolated_cursor_write&) const = default;
};
struct isolated_cursor_notify {
   size_t notification_index = 0;  // index in THIS receiver's new recipients
   bool operator==(const isolated_cursor_notify&) const = default;
};
using isolated_cursor_event = std::variant<isolated_cursor_query, isolated_cursor_write, isolated_cursor_notify>;

struct isolated_action_result {
   primary_overlay_result state;
   std::vector<char> return_value;
   fc::sha256 execution_fingerprint;
   // Root notification order; root itself is implicitly already notified.
   // The owner must preflight every recipient and replay the original scheduler.
   static constexpr size_t max_recipients = 64;
   std::vector<isolated_notification> recipients;
};

// One action's owned effects within a TRANSACTION-wide journal. Notifications
// use absolute write positions, including writes by earlier actions.
// VM memory/start and return bytes are local to this receiver. Root-only
// execution uses a fresh cursor; notification execution shares cursors and
// recipient deduplication within each root group. State budgets span all steps.
struct isolated_action_effects {
   std::vector<char> return_value;
   fc::sha256 execution_fingerprint;
   std::vector<isolated_notification> recipients;
   size_t writes_begin = 0;
   size_t writes_end = 0;
   std::chrono::microseconds elapsed{}; // private execution only, not compilation
   std::chrono::steady_clock::time_point began, ended; // local overlap diagnostics, never fingerprinted
};

struct isolated_action_invocation {
   std::vector<char> code;
   isolated_action_input input;
   isolated_wasm_limits limits;
};

struct isolated_transaction_result {
   primary_overlay_result state;
   std::vector<isolated_action_effects> actions;
   fc::sha256 execution_fingerprint;
};

// Binds successful output to the actual code, fixed action inputs and observable
// runtime limits. This is NOT authorization or canonical state validation.
fc::sha256 isolated_execution_fingerprint(const fc::sha256& code_hash, const isolated_wasm_limits&,
                                          const isolated_action_input&);
fc::sha256 isolated_transaction_fingerprint(std::span<const fc::sha256> action_fingerprints);

// Each worker OWNS its compiled module, linear memory and execution context.
// Modules remain unshared, including parsing/initialization and mutable VM
// state. Local interpreter timeouts use cooperative opcode polling. This type
// is neither copyable nor movable and is thread-affine.
// Unknown imports fail linking. Explicit link-only imports always throw when
// called (they are NOT implemented hosts). All errors require WHOLE-transaction fallback,
// never a consensus rejection based on these local capability/timeout limits.
class isolated_wasm_module {
public:
   explicit isolated_wasm_module(std::span<const char> code, isolated_wasm_limits = {});
   ~isolated_wasm_module();
   isolated_wasm_module(const isolated_wasm_module&) = delete;
   isolated_wasm_module& operator=(const isolated_wasm_module&) = delete;
   isolated_action_result execute(std::shared_ptr<const frozen_primary_state>, const isolated_action_input&,
                                  std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   friend class isolated_transaction_executor;
   friend class isolated_notification_executor;
   // Only the owning execution entry points may call this. An exception
   // destroys their entire local overlay; no caller can seal a partial prefix.
   isolated_action_effects execute_step(primary_state_overlay&, const isolated_action_input&,
                                       std::stop_token, isolated_cancel,
                                       primary_overlay_cursor* group_cursor = nullptr,
                                       std::set<name>* group_recipients = nullptr,
                                       std::vector<isolated_cursor_event>* cursor_events = nullptr,
                                       size_t max_cursor_events = 0);
   struct impl;
   std::unique_ptr<impl> impl_;
};

// Private execution does not itself authorize canonical adoption. One bounded
// module cache per worker thread; one owned overlay per call, sealed after ALL actions
// finish. No partial result is returned on missing state, cancellation or error.
class isolated_transaction_executor {
public:
   static constexpr size_t max_actions = 16;
   static constexpr size_t max_total_code_bytes = 256 * 1024;
   static constexpr size_t max_total_action_bytes = 256 * 1024;
   static constexpr std::chrono::milliseconds max_attempt_time{500};
   isolated_transaction_executor();
   ~isolated_transaction_executor();
   isolated_transaction_executor(const isolated_transaction_executor&) = delete;
   isolated_transaction_executor& operator=(const isolated_transaction_executor&) = delete;
   isolated_transaction_result execute(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_invocation>, std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

// Owned notification execution input. This catalog is not a canonical code
// dependency proof: its caller must own every byte for the synchronous call.
// Empty code explicitly represents an existing no-code receiver. Missing names
// request an owned capture; serial_only marks native/privileged/unsupported code.
struct isolated_receiver_program {
   name receiver;
   std::vector<char> code;
   isolated_wasm_limits limits;
   bool serial_only = false;
};

struct isolated_receiver_missing : std::runtime_error {
   name receiver;
   explicit isolated_receiver_missing(name account)
      : std::runtime_error("receiver program requires owner capture"), receiver(account) {}
};

struct isolated_receiver_effects {
   size_t root_index = 0;
   uint32_t action_ordinal = 0, creator_action_ordinal = 0;
   name receiver;
   fc::sha256 code_hash;
   bool has_code = false;
   isolated_action_effects effects;
   std::vector<isolated_cursor_event> cursor_events;
};

struct isolated_notification_result {
   primary_overlay_result state;
   std::vector<isolated_receiver_effects> steps; // execution order, NOT ordinal order
   fc::sha256 root_fingerprint;
};

// Read-only PRECONDITION for a future canonical adopter, not an authorization.
// Binds the complete receiver queue to the supplied owned inputs/programs and
// replays cursor events on a NEW private overlay of result.state.base(). Every
// mutation must reproduce the reported table/key/object generation/payer/value;
// every replay read must already belong to the original dependency proof.
// False means whole-attempt fallback BEFORE any canonical mutation. Checkpoint
// exceptions propagate (including interrupts); local replay errors return false.
// The owner must ALSO revalidate current code/capabilities and matches_base_reads,
// and retain original transaction authorization/resources/receipts/undo checks.
bool validate_isolated_notification_journal(const isolated_notification_result&,
   std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
   const std::function<void()>& checkpoint = {});

// A result is not itself permission to mutate canonical state. One root and
// its notification queue share iterator handles and deduplication. The next
// root starts a fresh group, while retaining the transaction-wide private state.
class isolated_notification_executor {
public:
   static constexpr size_t max_programs = 256, max_steps = 256;
   static constexpr size_t max_cursor_events = 4096; // whole attempt, not per receiver
   isolated_notification_executor();
   ~isolated_notification_executor();
   isolated_notification_executor(const isolated_notification_executor&) = delete;
   isolated_notification_executor& operator=(const isolated_notification_executor&) = delete;
   isolated_notification_result execute(std::shared_ptr<const frozen_primary_state>,
      std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
      std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace eosio::chain::experimental
