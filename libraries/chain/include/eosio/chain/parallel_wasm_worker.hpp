#pragma once
#include <eosio/chain/parallel_immutable_code.hpp>

#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/parallel_secondary_journal.hpp>
#include <eosio/chain/parallel_console.hpp>
#include <eosio/chain/parallel_transaction_environment.hpp>
#include <eosio/chain/parallel_execution_runtime.hpp>
#include <fc/crypto/sha256.hpp>
#include <chrono>
#include <atomic>
#include <set>
#include <variant>
#include <stop_token>

namespace eosio::chain::experimental {

struct isolated_tree_capture; // private definition; no standalone host capability
struct isolated_sync_capture;

enum class isolated_interrupt_reason : uint8_t { cancelled, deadline };
// Preserve the existing invalid-attempt catch contract, but retain the reason
// at the checkpoint itself. Inspecting the clock later in a catch is ambiguous.
struct isolated_execution_interrupted : primary_state_invalid_attempt {
   const isolated_interrupt_reason reason;
   explicit isolated_execution_interrupted(isolated_interrupt_reason why) : reason(why) {}
};

// A local discovery hint only when an ordinary ROOT actually calls a sync
// host. It is not inferred from a callee export and carries no live pointers.
struct isolated_sync_mode_required : primary_state_unsupported {};

// Another owned discovery hint, never permission to run a forbidden host.
// Ordinary nested tree frames need this mode for synchronous calls; ordinary
// sync roots need it for queued sends/notifications. CF/sync-child violations
// remain unsupported, not requests to upgrade their authority.
struct isolated_mixed_mode_required : primary_state_unsupported {};

// Request one immutable transaction environment from the joined owner. Never
// carries a controller pointer, authorization or any partial successful result.
struct isolated_transaction_environment_missing : primary_state_unsupported {};

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
   // Borrow only for this check; the caller retains ownership of its token.
   // Per-host-call copies contend on the shared stop-state reference count.
   void check(const std::stop_token& stop = {}) const {
      if (stop.stop_requested() || (generation && generation->load(std::memory_order_relaxed) != observed))
         throw isolated_execution_interrupted(isolated_interrupt_reason::cancelled);
      if (deadline != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= deadline)
         throw isolated_execution_interrupted(isolated_interrupt_reason::deadline);
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
   std::shared_ptr<const isolated_transaction_environment> environment;
};

struct isolated_wasm_limits {
   // Private admission only: exceeding these bounds selects original serial
   // execution, never changes the chain's WASM validation/consensus limits.
   // Module bytes and the parser's SINGLE function-body size are distinct.
   static constexpr size_t max_module_code_bytes = 128 * 1024;
   static constexpr uint32_t max_imports = 128;
   static constexpr uint32_t max_local_functions = 256;
   static constexpr uint32_t max_function_body_bytes = 64 * 1024;
   uint32_t max_pages = 16;
   uint32_t max_call_depth = 64;
   size_t max_code_bytes = max_module_code_bytes;
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
using isolated_cursor_event = std::variant<isolated_cursor_query, isolated_cursor_write, isolated_cursor_notify,
                                           isolated_secondary_cursor_event, isolated_console_event>;

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
   std::chrono::microseconds elapsed{}; // exclusive thread CPU; excludes compilation and descheduling
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
// state. Interpreter polling or private JIT frame timers bound execution. This type
// is neither copyable nor movable and is thread-affine.
// Unknown imports fail linking. Explicit link-only imports always throw when
// called (they are NOT implemented hosts). All errors require WHOLE-transaction fallback,
// never a consensus rejection based on these local capability/timeout limits.
class isolated_wasm_module {
public:
   explicit isolated_wasm_module(std::span<const char> code, isolated_wasm_limits = {},
                                 std::shared_ptr<const isolated_execution_runtime> = {});
   ~isolated_wasm_module();
   isolated_wasm_module(const isolated_wasm_module&) = delete;
   isolated_wasm_module& operator=(const isolated_wasm_module&) = delete;
   isolated_action_result execute(std::shared_ptr<const frozen_primary_state>, const isolated_action_input&,
                                  std::stop_token = {}, primary_overlay_limits = {}, isolated_cancel = {});
private:
   friend class isolated_transaction_executor;
   friend class isolated_notification_executor;
   friend class isolated_action_tree_executor;
   friend class isolated_sync_executor;
   friend class isolated_mixed_tree_executor;
   // Only the owning execution entry points may call this. An exception
   // destroys their entire local overlay; no caller can seal a partial prefix.
   isolated_action_effects execute_step(primary_state_overlay&, const isolated_action_input&,
                                       std::stop_token, isolated_cancel,
                                       primary_overlay_cursor* group_cursor = nullptr,
                                       std::set<name>* group_recipients = nullptr,
                                       std::vector<isolated_cursor_event>* cursor_events = nullptr,
                                       size_t max_cursor_events = 0,
                                       isolated_tree_capture* tree_capture = nullptr,
                                       isolated_sync_capture* sync_capture = nullptr,
                                       secondary_cursor_group* group_secondary = nullptr);
   struct impl;
   std::unique_ptr<impl> impl_;
};

// Thread-owned module storage, NOT a host/state/adoption capability. A worker
// shares this one nine-slot bound across mutually exclusive execution modes.
// Every borrow is exclusive, exact-code/limits keyed and reset before execution.
class isolated_module_cache {
public:
   explicit isolated_module_cache(std::shared_ptr<const isolated_execution_runtime> = {});
   ~isolated_module_cache();
   isolated_module_cache(const isolated_module_cache&) = delete;
   isolated_module_cache& operator=(const isolated_module_cache&) = delete;
private:
   friend class isolated_transaction_executor;
   friend class isolated_notification_executor;
   friend class isolated_action_tree_executor;
   friend class isolated_sync_executor;
   friend class isolated_mixed_tree_executor;
   isolated_module_cache(size_t capacity, bool fixed_depth, std::shared_ptr<const isolated_execution_runtime> = {});
   struct impl;
   std::unique_ptr<impl> impl_;
};

// Private execution does not itself authorize canonical adoption. A standalone
// executor keeps its original four-entry limit; a worker supplies its common
// bounded cache. One owned overlay per call, sealed after ALL actions
// finish. No partial result is returned on missing state, cancellation or error.
class isolated_transaction_executor {
public:
   static constexpr size_t max_actions = 16;
   static constexpr size_t max_total_code_bytes = 256 * 1024;
   static constexpr size_t max_total_action_bytes = 256 * 1024;
   static constexpr std::chrono::milliseconds max_attempt_time{500};
   isolated_transaction_executor();
   explicit isolated_transaction_executor(std::shared_ptr<isolated_module_cache>);
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
   immutable_code code;
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
   explicit isolated_notification_executor(std::shared_ptr<isolated_module_cache>);
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
