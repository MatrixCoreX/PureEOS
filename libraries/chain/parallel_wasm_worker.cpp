#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/vm/backend.hpp>
#include <eosio/vm/host_function.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>

namespace eosio::chain::experimental {

// Only the tree executor can supply this synchronous stack-local capability.
// Legacy module/transaction/notification APIs never receive it. No reference
// to it, the VM memory, or the caller's inputs survives execute_step.
struct isolated_tree_capture {
   std::function<void(const isolated_cursor_event&)> record;
   std::function<void(std::span<const char>, bool context_free)> send;
   name sender;
   bool context_free = false;
   const isolated_context_free_data* context_free_data = nullptr; // execute_step stack only
};

// Stack-only capability supplied exclusively by isolated_sync_executor. A
// recursive invocation owns another module and another one of these frames.
struct isolated_sync_capture {
   std::function<std::pair<int64_t, std::vector<char>>(name, uint64_t, std::span<const char>)> call;
   std::function<void(const isolated_cursor_event&)> record;
   bool is_sync = false;
   bool read_only = false;
   name sender;
   bool get_sender_fix = false;
   int64_t status = 0;
   std::chrono::steady_clock::duration children_time{};
};

namespace {

// Interpreter-only cooperative interruption: every opcode still goes through
// the original visitor. Polling bounds loops (including module start) without
// creating a timer thread or asynchronously changing executable page rights.
// This is a LOCAL attempt budget; timeout always selects serial fallback.
class attempt_deadline {
public:
   attempt_deadline(std::chrono::milliseconds duration, std::stop_token stop, isolated_cancel external)
      : end_(std::chrono::steady_clock::now() + duration), stop_(stop), external_(std::move(external)) {}
   void check() const {
      external_.check(stop_);
      if (std::chrono::steady_clock::now() >= end_) throw eosio::vm::timeout_exception{"isolated attempt timed out"};
   }
private:
   std::chrono::steady_clock::time_point end_;
   std::stop_token stop_;
   isolated_cancel external_;
};

template<typename Context> struct interruptible_visitor {
   eosio::vm::interpret_visitor<Context> original;
   const attempt_deadline& deadline;
   uint32_t max_pages;
   uint32_t remaining = 1024;
   // Preserve the original interpreter's always-inline opcode dispatch. An
   // out-of-line wrapper otherwise adds a call (and sanitizer stack frame) to
   // EVERY opcode, not just the one-in-1024 deadline checkpoint.
   template<typename Op> [[gnu::always_inline]] inline void operator()(const Op& op) {
      if (--remaining == 0) { deadline.check(); remaining = 1024; }
      original(op);
   }
   [[gnu::always_inline]] inline void operator()(const eosio::vm::grow_memory_t& op) {
      deadline.check();
      auto& context = original.get_context();
      const auto growth = context.peek_operand().to_ui32();
      const auto current = context.current_linear_memory();
      // Never expose a LOCAL cap as a successful contract-visible -1. That
      // could choose a different branch than the original execution. This
      // guard also runs in start functions. Conservatively reject the VM's
      // signed/negative growth extension rather than emulate it here.
      if (current < 0 || uint32_t(current) > max_pages || growth > max_pages - uint32_t(current))
         throw primary_state_unsupported();
      original(op); // retains module maximum, result, allocation and zeroing
   }
};

// Strict local subset; rejecting a module here only selects serial fallback.
struct isolated_vm_options : eosio::vm::eosio_options {
   uint32_t max_pages = 16;
   uint32_t max_call_depth = 64;
   static constexpr uint32_t max_function_section_elements = 128;
   static constexpr uint32_t max_import_section_elements = 32;
   static constexpr uint32_t max_code_bytes = 64 * 1024;
   static constexpr bool forbid_export_mutable_globals = false;
   static constexpr bool allow_code_after_function_end = false;
};

struct isolated_host {
   primary_overlay_cursor& cursor;
   primary_state_overlay& state;
   const isolated_action_input& input;
   std::stop_token stop;
   isolated_cancel external;
   std::function<std::span<char>()> linear_memory;
   std::vector<char> return_value;
   std::vector<isolated_notification> recipients;
   std::set<name>* group_recipients = nullptr;
   std::vector<isolated_cursor_event>* cursor_events = nullptr;
   size_t max_cursor_events = 0;
   isolated_tree_capture* tree_capture = nullptr;
   isolated_sync_capture* sync_capture = nullptr;
   std::vector<char> last_call_return;

   void record(isolated_cursor_event event) {
      if (sync_capture) { sync_capture->record(event); return; }
      if (!cursor_events) return; // unchanged root-only mode never allocates a transcript
      if (cursor_events->size() >= max_cursor_events) throw primary_state_budget_exceeded();
      if (tree_capture) tree_capture->record(event);
      cursor_events->push_back(std::move(event));
   }

   void check() const { external.check(stop); }
   void context_aware() const {
      check();
      if (tree_capture && tree_capture->context_free) throw primary_state_unsupported();
   }
   void action_only() const {
      check();
      if (sync_capture && sync_capture->is_sync) throw primary_state_unsupported();
   }
   void writable() const {
      context_aware();
      if (sync_capture && sync_capture->read_only) throw primary_state_unsupported();
   }
   std::span<char> memory(uint32_t offset, uint32_t size) {
      check();
      auto bytes = linear_memory();
      if (offset > bytes.size() || size > bytes.size() - offset) throw primary_state_unsupported();
      return bytes.subspan(offset, size);
   }
   uint64_t current_receiver() { check(); return input.receiver.to_uint64_t(); }
   uint32_t action_data_size() { action_only(); return input.data.size(); }
   uint32_t read_action_data(uint32_t offset, uint32_t size) {
      action_only();
      auto destination = memory(offset, size);
      const auto count = std::min(destination.size(), input.data.size());
      if (!count) return input.data.size();
      std::copy_n(input.data.begin(), count, destination.begin());
      return count;
   }
   int32_t has_auth(uint64_t actor) {
      action_only();
      context_aware();
      return std::any_of(input.authorizations.begin(), input.authorizations.end(),
                         [&](const auto& item) { return item.first.to_uint64_t() == actor; });
   }
   void require_auth(uint64_t actor) { if (!has_auth(actor)) throw primary_state_unsupported(); }
   void require_auth2(uint64_t actor, uint64_t permission) {
      action_only();
      context_aware();
      if (!std::any_of(input.authorizations.begin(), input.authorizations.end(), [&](const auto& item) {
            return item.first.to_uint64_t() == actor && item.second.to_uint64_t() == permission;
         })) throw primary_state_unsupported();
   }
   uint32_t memcpy(uint32_t destination, uint32_t source, uint32_t size) {
      auto dst = memory(destination, size);
      const auto src = memory(source, size);
      memory(destination, 1); // original converter checks this even for size=0
      const uint64_t distance = destination > source ? uint64_t(destination) - source : uint64_t(source) - destination;
      if (distance < size) throw primary_state_unsupported();
      std::memcpy(dst.data(), src.data(), size);
      return destination;
   }
   uint32_t memmove(uint32_t destination, uint32_t source, uint32_t size) {
      auto dst = memory(destination, size);
      const auto src = memory(source, size);
      memory(destination, 1);
      std::memmove(dst.data(), src.data(), size);
      return destination;
   }
   int32_t memcmp(uint32_t left, uint32_t right, uint32_t size) {
      const auto lhs = memory(left, size);
      const auto rhs = memory(right, size);
      const auto result = std::memcmp(lhs.data(), rhs.data(), size);
      return result < 0 ? -1 : result > 0 ? 1 : 0;
   }
   uint32_t memset(uint32_t destination, int32_t value, uint32_t size) {
      auto dst = memory(destination, size);
      memory(destination, 1);
      std::memset(dst.data(), value, size);
      return destination;
   }
   void eosio_assert(uint32_t condition, uint32_t message) {
      memory(message, 1);
      const auto bytes = linear_memory().subspan(message);
      // The original converter validates the entire NUL-terminated pointer
      // BEFORE calling the host, including when the condition is true.
      if (std::find(bytes.begin(), bytes.end(), '\0') == bytes.end() || !condition)
         throw primary_state_unsupported();
   }
   void eosio_assert_message(uint32_t condition, uint32_t message, uint32_t size) {
      memory(message, size);
      if (!condition) throw primary_state_unsupported();
   }
   void eosio_assert_code(uint32_t condition, uint64_t) {
      check();
      if (!condition) throw primary_state_unsupported();
   }
   [[noreturn]] void abort() { throw primary_state_unsupported(); }

   int32_t is_account(uint64_t account) { context_aware(); return bool(state.read_account(name{account})); }
   void require_recipient(uint64_t account) {
      context_aware();
      action_only();
      if (sync_capture) throw primary_state_unsupported();
      const name recipient{account};
      if (group_recipients) {
         if (group_recipients->contains(recipient)) return;
         if (group_recipients->size() >= isolated_action_result::max_recipients + 1)
            throw primary_state_budget_exceeded();
         group_recipients->insert(recipient);
      } else {
         if (recipient == input.receiver || std::any_of(recipients.begin(), recipients.end(),
               [&](const auto& notification) { return notification.recipient == recipient; })) return;
         if (recipients.size() == isolated_action_result::max_recipients) throw primary_state_budget_exceeded();
      }
      recipients.push_back({recipient, state.write_count()});
      if (cursor_events) record(isolated_cursor_notify{recipients.size() - 1});
   }
   // Explicit link-only imports used by ordinary CDT modules. NONE is an
   // implementation: reaching any of these discards the WHOLE attempt, even
   // after private writes. No default result, output or canonical effect.
   [[noreturn]] void prints_l(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   void send_inline(uint32_t offset, uint32_t size) {
      context_aware();
      if (!tree_capture) throw primary_state_unsupported();
      tree_capture->send(memory(offset, size), false);
   }
   void send_context_free_inline(uint32_t offset, uint32_t size) {
      context_aware(); // the SEND host is context-aware, unlike its child
      if (!tree_capture || !tree_capture->context_free_data) throw primary_state_unsupported();
      tree_capture->send(memory(offset, size), true);
   }
   int32_t get_context_free_data(uint32_t index, uint32_t offset, uint32_t size) {
      if (!tree_capture || !tree_capture->context_free || !tree_capture->context_free_data)
         throw primary_state_unsupported();
      return tree_capture->context_free_data->read(index, memory(offset, size));
   }
   uint64_t get_sender() {
      context_aware();
      if (sync_capture)
         return (sync_capture->is_sync && !sync_capture->get_sender_fix ? input.receiver : sync_capture->sender).to_uint64_t();
      if (!tree_capture) throw primary_state_unsupported();
      return tree_capture->sender.to_uint64_t();
   }
   [[noreturn]] void set_blockchain_parameters_packed(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] uint32_t get_blockchain_parameters_packed(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] int64_t set_proposed_producers(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] uint64_t current_time() { throw primary_state_unsupported(); }
   [[noreturn]] int32_t get_active_producers(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   void set_action_return_value(uint32_t offset, uint32_t size) {
      action_only();
      context_aware();
      if (size > input.max_return_bytes) throw primary_state_unsupported();
      const auto source = memory(offset, size);
      return_value.assign(source.begin(), source.end());
   }
   int64_t call(uint64_t receiver, uint64_t flags, uint32_t offset, uint32_t size) {
      context_aware();
      if (!sync_capture) throw primary_state_unsupported();
      const auto data = memory(offset, size); // validate before entering original call semantics
      last_call_return.clear();
      const auto begin = std::chrono::steady_clock::now();
      auto [status, bytes] = sync_capture->call(name{receiver}, flags, data);
      sync_capture->children_time += std::chrono::steady_clock::now() - begin;
      last_call_return = std::move(bytes);
      return status < 0 ? status : static_cast<int64_t>(last_call_return.size());
   }
   uint32_t get_call_return_value(uint32_t offset, uint32_t size) {
      context_aware();
      if (!sync_capture) throw primary_state_unsupported();
      auto output = memory(offset, size);
      const auto count = std::min(output.size(), last_call_return.size());
      if (count) std::copy_n(last_call_return.begin(), count, output.begin());
      return last_call_return.size();
   }
   uint32_t get_call_data(uint32_t offset, uint32_t size) {
      context_aware();
      if (!sync_capture || !sync_capture->is_sync) throw primary_state_unsupported();
      auto output = memory(offset, size);
      const auto count = std::min(output.size(), input.data.size());
      if (count) std::copy_n(input.data.begin(), count, output.begin());
      return input.data.size();
   }
   void set_call_return_value(uint32_t offset, uint32_t size) {
      context_aware();
      if (!sync_capture || !sync_capture->is_sync || size > input.max_return_bytes)
         throw primary_state_unsupported();
      const auto bytes = memory(offset, size);
      return_value.assign(bytes.begin(), bytes.end());
   }
   int32_t db_find_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      context_aware();
      const table_key key{name(code), name(scope), name(table)};
      const auto result = cursor.find(key, primary);
      if (cursor_events || sync_capture) record(isolated_cursor_query{isolated_cursor_query_kind::find, key, primary, -1, result});
      return result;
   }
   int32_t db_store_i64(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset, uint32_t size) {
      writable();
      const auto result = cursor.store({input.receiver, name(scope), name(table)}, primary, name(payer), memory(offset, size));
      if (cursor_events || sync_capture) record(isolated_cursor_write{state.write_count() - 1, result});
      return result;
   }
   void db_remove_i64(int32_t iterator) {
      writable(); cursor.remove(iterator);
      if (cursor_events || sync_capture) record(isolated_cursor_write{state.write_count() - 1, iterator});
   }
   int32_t db_lowerbound_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      context_aware();
      const table_key key{name(code), name(scope), name(table)};
      const auto result = cursor.lower_bound(key, primary);
      if (cursor_events || sync_capture) record(isolated_cursor_query{isolated_cursor_query_kind::lower_bound, key, primary, -1, result});
      return result;
   }
   int32_t db_upperbound_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      context_aware();
      const table_key key{name(code), name(scope), name(table)};
      const auto result = cursor.upper_bound(key, primary);
      if (cursor_events || sync_capture) record(isolated_cursor_query{isolated_cursor_query_kind::upper_bound, key, primary, -1, result});
      return result;
   }
   int32_t db_end_i64(uint64_t code, uint64_t scope, uint64_t table) {
      context_aware();
      const table_key key{name(code), name(scope), name(table)};
      const auto result = cursor.end(key);
      if (cursor_events || sync_capture) record(isolated_cursor_query{isolated_cursor_query_kind::end, key, 0, -1, result});
      return result;
   }
   int32_t db_get_i64(int32_t iterator, uint32_t offset, uint32_t size) {
      context_aware();
      return cursor.get(iterator, memory(offset, size));
   }
   void db_update_i64(int32_t iterator, uint64_t payer, uint32_t offset, uint32_t size) {
      writable();
      cursor.update(iterator, name(payer), memory(offset, size));
      if (cursor_events || sync_capture) record(isolated_cursor_write{state.write_count() - 1, iterator});
   }
   int32_t adjacent(int32_t iterator, uint32_t offset, bool previous) {
      context_aware();
      // ORIGINAL legacy_ptr<uint64_t> rejects WASM offset zero before the DB
      // call, even though byte zero is inside linear memory. Cursor replay
      // bypasses that converter, so reject the whole private attempt here.
      if (!offset) throw primary_state_unsupported();
      auto destination = memory(offset, sizeof(uint64_t));
      uint64_t primary;
      std::memcpy(&primary, destination.data(), sizeof(primary));
      const auto before = primary;
      const auto result = previous ? cursor.previous(iterator, primary) : cursor.next(iterator, primary);
      std::memcpy(destination.data(), &primary, sizeof(primary)); // unaligned WASM pointers are legal
      if (cursor_events || sync_capture)
         record(isolated_cursor_query{previous ? isolated_cursor_query_kind::previous : isolated_cursor_query_kind::next,
                                      {}, 0, iterator, result, before, primary});
      return result;
   }
   int32_t db_next_i64(int32_t iterator, uint32_t offset) { return adjacent(iterator, offset, false); }
   int32_t db_previous_i64(int32_t iterator, uint32_t offset) { return adjacent(iterator, offset, true); }
};

using functions = eosio::vm::registered_host_functions<isolated_host>;
using backend = eosio::vm::backend<functions, eosio::vm::interpreter, isolated_vm_options>;

void register_isolated_functions() {
   static std::once_flag once;
   std::call_once(once, [] {
#define ISOLATED_HOST(method) functions::add<&isolated_host::method>("env", #method)
      ISOLATED_HOST(current_receiver);
      ISOLATED_HOST(action_data_size);
      ISOLATED_HOST(read_action_data);
      ISOLATED_HOST(has_auth);
      ISOLATED_HOST(require_auth);
      ISOLATED_HOST(require_auth2);
      ISOLATED_HOST(memcpy);
      ISOLATED_HOST(memmove);
      ISOLATED_HOST(memcmp);
      ISOLATED_HOST(memset);
      ISOLATED_HOST(eosio_assert);
      ISOLATED_HOST(eosio_assert_message);
      ISOLATED_HOST(eosio_assert_code);
      ISOLATED_HOST(abort);
      ISOLATED_HOST(is_account);
      ISOLATED_HOST(require_recipient);
      ISOLATED_HOST(set_action_return_value);
      ISOLATED_HOST(db_find_i64);
      ISOLATED_HOST(db_lowerbound_i64);
      ISOLATED_HOST(db_upperbound_i64);
      ISOLATED_HOST(db_end_i64);
      ISOLATED_HOST(db_get_i64);
      ISOLATED_HOST(db_update_i64);
      ISOLATED_HOST(db_next_i64);
      ISOLATED_HOST(db_previous_i64);
      // Retain registration order; the two lifecycle entries are now real hosts.
#define ISOLATED_FALLBACK(method) functions::add<&isolated_host::method>("env", #method)
#define ISOLATED_TREE_HOST(method) functions::add<&isolated_host::method>("env", #method)
      ISOLATED_FALLBACK(prints_l);
      ISOLATED_TREE_HOST(send_inline); // private tree only; existing adopters still fallback
      ISOLATED_HOST(db_store_i64);
      ISOLATED_HOST(db_remove_i64);
      ISOLATED_FALLBACK(set_blockchain_parameters_packed);
      ISOLATED_FALLBACK(get_blockchain_parameters_packed);
      ISOLATED_FALLBACK(set_proposed_producers);
      ISOLATED_FALLBACK(current_time);
      ISOLATED_FALLBACK(get_active_producers);
      ISOLATED_TREE_HOST(get_sender); // bound to creator receiver, never inferred from root account
      ISOLATED_TREE_HOST(send_context_free_inline);
      ISOLATED_TREE_HOST(get_context_free_data);
#define ISOLATED_SYNC_HOST(method) functions::add<&isolated_host::method>("env", #method)
      ISOLATED_SYNC_HOST(call);
      ISOLATED_SYNC_HOST(get_call_return_value);
      ISOLATED_SYNC_HOST(get_call_data);
      ISOLATED_SYNC_HOST(set_call_return_value);
#undef ISOLATED_SYNC_HOST
#undef ISOLATED_FALLBACK
#undef ISOLATED_TREE_HOST
#undef ISOLATED_HOST
   });
}
} // namespace

struct isolated_wasm_module::impl {
   const std::thread::id owner = std::this_thread::get_id();
   isolated_wasm_limits limits;
   isolated_vm_options options;
   std::vector<uint8_t> owned_code;
   fc::sha256 code_hash;
   eosio::vm::wasm_allocator allocator;
   std::unique_ptr<backend> machine;

   impl(std::span<const char> code, isolated_wasm_limits bounds) : limits(bounds) {
      if (code.empty() || code.size() > limits.max_code_bytes || limits.max_code_bytes > 64 * 1024 ||
          !limits.max_pages || limits.max_pages > 16 || !limits.max_call_depth || limits.max_call_depth > 64 ||
          limits.max_action_bytes > 64 * 1024 || limits.max_authorizations > 64 || limits.max_return_bytes > 4096 ||
          limits.max_attempt_time.count() <= 0 || limits.max_attempt_time > std::chrono::seconds(1))
         throw primary_state_budget_exceeded();
      options.max_pages = limits.max_pages;
      options.max_call_depth = limits.max_call_depth;
      register_isolated_functions();
      owned_code.assign(code.begin(), code.end());
      code_hash = fc::sha256::hash(code.data(), code.size());
      eosio::vm::wasm_code_ptr pointer(owned_code.data(), owned_code.size());
      // Do not run the module's start function before the attempt/timeout exist.
      // Interpreter two-pass parsing limits virtual mappings. No backend::share.
      // Compilation/linking is deliberately serialized for this first subset;
      // the registry's upstream resolver uses a non-const map accessor. Runtime
      // dispatch only reads the immutable mappings populated by call_once.
      static std::mutex compilation_mutex;
      {
         std::lock_guard lock(compilation_mutex);
         machine = std::make_unique<backend>(pointer, code.size(), nullptr, options, false);
      }
      const auto& memories = machine->get_module().memories;
      // Initial memory is bounded by the parser. An absent/larger module
      // maximum is safe only because EVERY memory.grow is guarded above.
      if (memories.size() > 1) throw primary_state_unsupported();
      machine->set_wasm_allocator(&allocator);
   }
};

isolated_wasm_module::isolated_wasm_module(std::span<const char> code, isolated_wasm_limits limits)
   : impl_(std::make_unique<impl>(code, limits)) {}
isolated_wasm_module::~isolated_wasm_module() = default;

fc::sha256 isolated_execution_fingerprint(const fc::sha256& code_hash, const isolated_wasm_limits& limits,
                                          const isolated_action_input& input) {
   fc::sha256::encoder encoder;
   constexpr char domain[] = "pureeos/isolated-action/v1";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, code_hash);
   fc::raw::pack(encoder, limits.max_pages);
   fc::raw::pack(encoder, limits.max_call_depth);
   fc::raw::pack(encoder, input.receiver);
   fc::raw::pack(encoder, input.account);
   fc::raw::pack(encoder, input.action);
   fc::raw::pack(encoder, input.authorizations);
   fc::raw::pack(encoder, input.data);
   fc::raw::pack(encoder, input.max_return_bytes);
   return encoder.result();
}

isolated_action_result isolated_wasm_module::execute(std::shared_ptr<const frozen_primary_state> base,
      const isolated_action_input& input, std::stop_token stop, primary_overlay_limits overlay_limits, isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   primary_state_overlay state(std::move(base), overlay_limits);
   auto effects = execute_step(state, input, stop, std::move(external));
   return {state.seal(), std::move(effects.return_value), effects.execution_fingerprint, std::move(effects.recipients)};
}

isolated_action_effects isolated_wasm_module::execute_step(primary_state_overlay& state,
      const isolated_action_input& input, std::stop_token stop, isolated_cancel external,
      primary_overlay_cursor* group_cursor, std::set<name>* group_recipients,
      std::vector<isolated_cursor_event>* cursor_events, size_t max_cursor_events,
      isolated_tree_capture* tree_capture, isolated_sync_capture* sync_capture) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   const auto began = std::chrono::steady_clock::now();
   external.check(stop);
   if (bool(group_cursor) != bool(group_recipients) ||
       (sync_capture && (tree_capture || group_cursor)) ||
       bool(group_cursor) != bool(cursor_events) || (cursor_events && !cursor_events->empty()) ||
       (tree_capture && !group_cursor) ||
       max_cursor_events > isolated_notification_executor::max_cursor_events ||
       (group_recipients && !group_recipients->contains(input.receiver)) ||
       (!group_cursor && input.receiver != input.account) || input.data.size() > impl_->limits.max_action_bytes ||
       input.authorizations.size() > impl_->limits.max_authorizations || input.max_return_bytes > impl_->limits.max_return_bytes)
      throw primary_state_unsupported();
   const auto fingerprint = isolated_execution_fingerprint(impl_->code_hash, impl_->limits, input);
   const auto writes_begin = state.write_count();
   primary_overlay_cursor local_cursor(state, input.receiver);
   auto& cursor = group_cursor ? *group_cursor : local_cursor;
   cursor.set_receiver_for_notification(input.receiver);
   auto& machine = *impl_->machine;
   isolated_host host{cursor, state, input, stop, external, [&]() -> std::span<char> {
      auto& context = machine.get_context();
      const int32_t pages = context.current_linear_memory();
      // EOS VM uses -1 for modules with NO memory. Never convert that sentinel
      // into a huge unsigned span. Zero-page memory calls conservatively fall
      // back too; non-memory hosts remain usable without linear memory.
      if (pages <= 0) throw primary_state_unsupported();
      return {context.linear_memory(), static_cast<size_t>(pages) * 65536u};
   }, {}, {}, group_recipients, cursor_events, max_cursor_events, tree_capture, sync_capture, {}};
   const attempt_deadline deadline(impl_->limits.max_attempt_time, stop, external);
   auto& context = machine.get_context();
   // Same reset/start sequence as backend::initialize, with a bounded visitor
   // for BOTH start and apply. No shared VM module/context is introduced.
   context.set_max_call_depth(impl_->options.max_call_depth);
   context.set_max_pages(impl_->options.max_pages);
   context.reset();
   interruptible_visitor<std::remove_reference_t<decltype(context)>> visitor{{context}, deadline, impl_->limits.max_pages};
   deadline.check();
   if (sync_capture && sync_capture->is_sync) {
      const auto index = machine.get_module().get_exported_function("sync_call");
      if (index == std::numeric_limits<uint32_t>::max() ||
          !(machine.get_module().get_function_type(index) ==
            eosio::vm::host_function{{eosio::vm::i64, eosio::vm::i64, eosio::vm::i32}, {eosio::vm::i64}}))
         throw primary_state_unsupported(); // captured metadata/code mismatch, never invent -2
   }
   context.execute_start(&host, visitor);
   if (sync_capture && sync_capture->is_sync) {
      auto result = context.execute(&host, visitor, "sync_call", sync_capture->sender.to_uint64_t(),
                                    input.receiver.to_uint64_t(), static_cast<uint32_t>(input.data.size()));
      if (!result) throw primary_state_unsupported();
      sync_capture->status = result->to_i64();
   } else {
      context.execute(&host, visitor, "apply", input.receiver.to_uint64_t(), input.account.to_uint64_t(), input.action.to_uint64_t());
   }
   deadline.check();
   const auto ended = std::chrono::steady_clock::now();
   const auto children_time = sync_capture ? sync_capture->children_time : std::chrono::steady_clock::duration{};
   if (children_time > ended - began) throw primary_state_invalid_attempt();
   return {std::move(host.return_value), fingerprint, std::move(host.recipients), writes_begin, state.write_count(),
           std::chrono::duration_cast<std::chrono::microseconds>(ended - began - children_time), began, ended};
}

isolated_sync_result isolated_sync_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      const isolated_action_input& root, std::span<const isolated_sync_program> programs,
      isolated_sync_context protocol, std::stop_token stop, primary_overlay_limits bounds, isolated_cancel external) {
   if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() + max_attempt_time);
   external.check(stop);
   if (programs.empty() || programs.size() > max_programs || root.receiver != root.account ||
       root.data.size() > max_total_data_bytes)
      throw primary_state_budget_exceeded();
   size_t total_code = 0, total_data = root.data.size();
   std::map<name, const isolated_sync_program*> catalog;
   for (const auto& item : programs) {
      external.check(stop);
      const auto& program = item.program;
      if (program.code.size() > max_total_code_bytes - total_code ||
          !catalog.emplace(program.receiver, &item).second) throw primary_state_budget_exceeded();
      total_code += program.code.size();
   }
   auto find_program = [&](name receiver) -> const isolated_sync_program& {
      const auto it = catalog.find(receiver);
      if (it == catalog.end()) throw isolated_receiver_missing(receiver);
      return *it->second;
   };
   primary_state_overlay state(std::move(base), bounds);
   if (!state.read_account(root.receiver)) throw primary_state_unsupported();
   std::vector<isolated_sync_trace> traces;
   std::vector<uint32_t> root_markers;
   std::vector<isolated_sync_event> root_events;
   size_t event_count = 0;
   auto record = [&](uint32_t ordinal, isolated_sync_event event) {
      if (event_count >= max_events) throw primary_state_budget_exceeded();
      ++event_count;
      (ordinal ? traces.at(ordinal - 1).events : root_events).push_back(std::move(event));
   };
   traces.reserve(max_calls);
   using call_result = std::pair<int64_t, std::vector<char>>;
   std::function<isolated_action_effects(const isolated_sync_program&, const isolated_action_input&,
      name, uint32_t, uint32_t, bool, int64_t&)> run;
   run = [&](const isolated_sync_program& captured, const isolated_action_input& input,
             name sender, uint32_t ordinal, uint32_t depth, bool read_only, int64_t& status) {
      external.check(stop);
      const auto& program = captured.program;
      if (program.serial_only || program.code.empty()) throw primary_state_unsupported();
      // Never consult the dormant action cache while an ancestor VM is live.
      // Construction, execution, destruction all finish before returning.
      isolated_wasm_module module(program.code, program.limits);
      isolated_sync_capture frame;
      frame.is_sync = ordinal != 0;
      frame.read_only = read_only;
      frame.sender = sender;
      frame.get_sender_fix = protocol.get_sender_fix;
      frame.record = [&](const isolated_cursor_event& event) { record(ordinal, event); };
      frame.call = [&](name receiver, uint64_t flags, std::span<const char> data) -> call_result {
         external.check(stop);
         if (traces.size() >= max_calls || data.size() > max_total_data_bytes - total_data)
            throw primary_state_budget_exceeded();
         total_data += data.size();
         const uint32_t child_ordinal = traces.size() + 1;
         const bool child_read_only = read_only || bool(flags & 1);
         // Keep ordinals, not references: nested calls can grow the vector.
         isolated_sync_trace trace;
         trace.call_ordinal = child_ordinal; trace.sender_ordinal = ordinal;
         trace.sender = input.receiver; trace.receiver = receiver;
         trace.flags = flags; trace.read_only = child_read_only;
         trace.data.assign(data.begin(), data.end());
         trace.writes_begin = trace.writes_end = state.write_count();
         traces.push_back(std::move(trace));
         record(ordinal, isolated_sync_call_event{child_ordinal, state.write_count()});
         if (protocol.console_enabled) {
            if (ordinal) traces[ordinal - 1].console_markers.push_back(0);
            else root_markers.push_back(0);
         }
         if (depth >= protocol.max_depth || depth >= max_depth || data.size() > protocol.max_data_bytes)
            throw primary_state_unsupported();
         auto failed = [&](int64_t error) -> call_result {
            traces[child_ordinal - 1].status = error;
            external.check(stop);
            return {error, {}};
         };
         // Account existence is a frozen dependency. Unknown is a capture
         // request, NOT -1. Preserve the original flags check ordering.
         if (!state.read_account(receiver)) return failed(-1);
         if (flags > 1) throw primary_state_unsupported();
         const auto& child_program = find_program(receiver);
         if (child_program.program.serial_only) throw primary_state_unsupported();
         if (child_program.program.code.empty()) return failed(-1);
         if (!child_program.sync_call_supported) return failed(-2);
         isolated_action_input child;
         child.receiver = child.account = receiver;
         child.data = traces[child_ordinal - 1].data;
         child.max_return_bytes = std::min(protocol.max_data_bytes, child_program.program.limits.max_return_bytes);
         int64_t child_status = 0;
         auto effects = run(child_program, child, input.receiver, child_ordinal, depth + 1, child_read_only, child_status);
         if (child_status != 0 && child_status > -10000) child_status = -3;
         auto& completed = traces[child_ordinal - 1]; // reacquire after recursion
         completed.exclusive_elapsed = effects.elapsed;
         completed.executed = true;
         completed.writes_begin = effects.writes_begin; completed.writes_end = effects.writes_end;
         completed.execution_fingerprint = effects.execution_fingerprint;
         completed.began = effects.began; completed.ended = effects.ended;
         if (child_status < 0) return failed(child_status);
         completed.return_value = std::move(effects.return_value);
         external.check(stop);
         return {0, completed.return_value};
      };
      auto effects = module.execute_step(state, input, stop, external, nullptr, nullptr, nullptr, 0, nullptr, &frame);
      status = frame.status;
      return effects;
   };
   int64_t status = 0;
   auto effects = run(find_program(root.receiver), root, name{}, 0, 0, protocol.root_read_only, status);
   external.check(stop);
   if (!effects.recipients.empty()) throw primary_state_invalid_attempt();
   const auto fingerprint = isolated_sync_fingerprint(root, programs, protocol);
   external.check(stop);
   return {state.seal(), std::move(effects), std::move(traces), std::move(root_markers), fingerprint,
           std::move(root_events)};
}

fc::sha256 isolated_transaction_fingerprint(std::span<const fc::sha256> action_fingerprints) {
   if (action_fingerprints.empty() || action_fingerprints.size() > isolated_transaction_executor::max_actions)
      throw primary_state_budget_exceeded();
   fc::sha256::encoder encoder;
   constexpr char domain[] = "pureeos/isolated-transaction/v1";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, static_cast<uint32_t>(action_fingerprints.size()));
   for (const auto& fingerprint : action_fingerprints) fc::raw::pack(encoder, fingerprint);
   return encoder.result();
}

namespace {
// A worker owns at most four dormant modules. Exact bytes AND all limits form
// the key: receiver names/hashes alone must not reuse stale code or bounds.
// This is not a pool for nested live frames: get() may evict a previous module,
// and execute_step() always resets that module's memory/globals/start function.
class bounded_module_cache {
   static constexpr size_t capacity = 4;
   struct entry {
      std::vector<char> code;
      isolated_wasm_limits limits;
      std::unique_ptr<isolated_wasm_module> module;
   };
   std::vector<entry> entries_;
public:
   bounded_module_cache() { entries_.reserve(capacity); }
   isolated_wasm_module& get(const std::vector<char>& code, const isolated_wasm_limits& limits) {
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
         if (it->code == code && it->limits == limits) {
            std::rotate(it, it + 1, entries_.end()); // most recently used at back
            return *entries_.back().module;
         }
      }
      performance::scope compile_timer(performance::stage::parallel_compile);
      // Evict BEFORE construction, including the throwing path. No transient
      // fifth allocator/module, no partially initialized cache entry on error.
      if (entries_.size() == capacity) entries_.erase(entries_.begin());
      entry next{code, limits, std::make_unique<isolated_wasm_module>(code, limits)};
      entries_.push_back(std::move(next));
      return *entries_.back().module;
   }
};
} // namespace

struct isolated_transaction_executor::impl {
   const std::thread::id owner = std::this_thread::get_id();
   bounded_module_cache modules;
};

isolated_transaction_executor::isolated_transaction_executor() : impl_(std::make_unique<impl>()) {}
isolated_transaction_executor::~isolated_transaction_executor() = default;

isolated_transaction_result isolated_transaction_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      std::span<const isolated_action_invocation> actions, std::stop_token stop,
      primary_overlay_limits overlay_limits, isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() + max_attempt_time);
   auto checkpoint = [&] {
      external.check(stop);
   };
   checkpoint();
   if (actions.empty() || actions.size() > max_actions) throw primary_state_budget_exceeded();
   size_t code_bytes = 0, data_bytes = 0;
   for (const auto& action : actions) {
      if (action.code.size() > 64 * 1024 || action.code.size() > max_total_code_bytes - code_bytes ||
          action.input.data.size() > 64 * 1024 || action.input.data.size() > max_total_action_bytes - data_bytes ||
          action.input.authorizations.size() > 64) throw primary_state_budget_exceeded();
      code_bytes += action.code.size(); data_bytes += action.input.data.size();
   }
   primary_state_overlay state(std::move(base), overlay_limits);
   std::vector<isolated_action_effects> results;
   std::vector<fc::sha256> fingerprints;
   results.reserve(actions.size()); fingerprints.reserve(actions.size());
   for (const auto& action : actions) {
      checkpoint();
      auto& module = impl_->modules.get(action.code, action.limits);
      checkpoint();
      {
         performance::scope execute_timer(performance::stage::parallel_execute);
         results.push_back(module.execute_step(state, action.input, stop, external));
      }
      fingerprints.push_back(results.back().execution_fingerprint);
   }
   checkpoint();
   const auto fingerprint = isolated_transaction_fingerprint(fingerprints);
   // State/VM/cursor exceptions propagate to the caller. The local state and
   // ALL step outputs die together; no earlier step can escape as a success.
   return {state.seal(), std::move(results), fingerprint};
}

struct isolated_notification_executor::impl {
   const std::thread::id owner = std::this_thread::get_id();
   bounded_module_cache modules;
};

isolated_notification_executor::isolated_notification_executor() : impl_(std::make_unique<impl>()) {}
isolated_notification_executor::~isolated_notification_executor() = default;

isolated_notification_result isolated_notification_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
      std::stop_token stop, primary_overlay_limits overlay_limits, isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() +
                                                isolated_transaction_executor::max_attempt_time);
   auto checkpoint = [&] {
      external.check(stop);
   };
   checkpoint();
   if (roots.empty() || roots.size() > isolated_transaction_executor::max_actions ||
       programs.empty() || programs.size() > max_programs) throw primary_state_budget_exceeded();
   size_t code_bytes = 0, data_bytes = 0;
   // These borrowed pointers exist only during this synchronous call. Neither
   // the cache nor any returned result retains catalog/input references.
   struct program_entry { const isolated_receiver_program* program; fc::sha256 hash; };
   std::map<name, program_entry> catalog;
   for (const auto& program : programs) {
      checkpoint();
      if (program.code.size() > 64 * 1024 ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes)
         throw primary_state_budget_exceeded();
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, program_entry{&program,
            program.code.empty() ? fc::sha256{} : fc::sha256::hash(program.code.data(), program.code.size())}).second)
         throw primary_state_unsupported();
   }
   auto lookup = [&](name receiver) -> const program_entry& {
      const auto it = catalog.find(receiver);
      if (it == catalog.end()) throw isolated_receiver_missing(receiver);
      if (it->second.program->serial_only) throw primary_state_unsupported();
      return it->second;
   };
   std::vector<fc::sha256> root_fingerprints;
   root_fingerprints.reserve(roots.size());
   for (const auto& root : roots) {
      if (root.data.size() > 64 * 1024 ||
          root.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes ||
          root.authorizations.size() > 64 || root.max_return_bytes > 4096) throw primary_state_budget_exceeded();
      data_bytes += root.data.size();
      if (root.receiver != root.account) throw primary_state_unsupported();
      const auto& entry = lookup(root.receiver);
      if (entry.program->code.empty()) throw primary_state_unsupported();
      root_fingerprints.push_back(isolated_execution_fingerprint(entry.hash, entry.program->limits, root));
   }
   primary_state_overlay state(std::move(base), overlay_limits);
   std::vector<isolated_receiver_effects> results;
   size_t event_count = 0;
   // The original transaction reserves ALL top-level ordinals first. A root's
   // notification ordinal can therefore exceed the next executed root's.
   uint32_t next_ordinal = roots.size() + 1;
   for (size_t root_index = 0; root_index < roots.size(); ++root_index) {
      const auto& root = roots[root_index];
      primary_overlay_cursor cursor(state, root.receiver);
      std::set<name> notified{root.receiver};
      struct pending_receiver { name receiver; uint32_t ordinal, creator; };
      std::vector<pending_receiver> pending{{root.receiver, uint32_t(root_index + 1), 0}};
      for (size_t index = 0; index < pending.size(); ++index) {
         checkpoint();
         const auto current = pending[index]; // appending below may reallocate
         const auto& entry = lookup(current.receiver);
         const auto& program = *entry.program;
         auto arguments = root;
         arguments.receiver = current.receiver; // original action stays intact
         isolated_action_effects effects;
         std::vector<isolated_cursor_event> cursor_events;
         if (!program.code.empty()) {
            auto& module = impl_->modules.get(program.code, program.limits);
            checkpoint();
            performance::scope execute_timer(performance::stage::parallel_execute);
            effects = module.execute_step(state, arguments, stop, external, &cursor, &notified,
                                                  &cursor_events, max_cursor_events - event_count);
         } else {
            effects.writes_begin = effects.writes_end = state.write_count();
            effects.execution_fingerprint = isolated_execution_fingerprint(entry.hash, program.limits, arguments);
         }
         for (const auto& notification : effects.recipients) {
            if (next_ordinal > max_steps) throw primary_state_budget_exceeded();
            pending.push_back({notification.recipient, next_ordinal++, current.ordinal});
         }
         if (results.size() >= max_steps) throw primary_state_budget_exceeded();
         event_count += cursor_events.size();
         results.push_back({root_index, current.ordinal, current.creator, current.receiver, entry.hash,
                            !program.code.empty(), std::move(effects), std::move(cursor_events)});
      }
   }
   checkpoint();
   // The owned result still requires whole-tree canonical code/state preflight;
   // the input catalog is not that proof, and no successful prefix may escape.
   return {state.seal(), std::move(results), isolated_transaction_fingerprint(root_fingerprints)};
}

fc::sha256 isolated_action_tree_fingerprint(std::span<const fc::sha256> roots,
                                           const isolated_action_tree_context& context) {
   fc::sha256::encoder encoder;
   constexpr char domain[] = "pureeos/isolated-action-tree/v2";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, isolated_transaction_fingerprint(roots));
   fc::raw::pack(encoder, context.max_inline_action_size);
   fc::raw::pack(encoder, context.max_inline_action_depth);
   fc::raw::pack(encoder, bool(context.context_free_data));
   if (context.context_free_data) fc::raw::pack(encoder, context.context_free_data->fingerprint());
   return encoder.result();
}

fc::sha256 isolated_action_tree_step_fingerprint(const fc::sha256& action_fingerprint,
      const isolated_action_tree_context& context, const isolated_action_tree_group& group,
      const isolated_action_tree_step& step) {
   fc::sha256::encoder encoder;
   constexpr char domain[] = "pureeos/isolated-action-tree-step/v2";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, action_fingerprint);
   fc::raw::pack(encoder, context.max_inline_action_size);
   fc::raw::pack(encoder, context.max_inline_action_depth);
   fc::raw::pack(encoder, bool(context.context_free_data));
   if (context.context_free_data) fc::raw::pack(encoder, context.context_free_data->fingerprint());
   fc::raw::pack(encoder, uint32_t(group.root_index));
   fc::raw::pack(encoder, group.action_ordinal);
   fc::raw::pack(encoder, group.creator_action_ordinal);
   fc::raw::pack(encoder, group.closest_unnotified_ancestor);
   fc::raw::pack(encoder, group.depth);
   fc::raw::pack(encoder, group.sender);
   fc::raw::pack(encoder, group.context_free);
   fc::raw::pack(encoder, uint32_t(step.group_index));
   fc::raw::pack(encoder, step.action_ordinal);
   fc::raw::pack(encoder, step.creator_action_ordinal);
   fc::raw::pack(encoder, step.closest_unnotified_ancestor);
   fc::raw::pack(encoder, step.sender);
   return encoder.result();
}

struct isolated_action_tree_executor::impl {
   const std::thread::id owner = std::this_thread::get_id();
   bounded_module_cache modules;
};

isolated_action_tree_executor::isolated_action_tree_executor() : impl_(std::make_unique<impl>()) {}
isolated_action_tree_executor::~isolated_action_tree_executor() = default;

isolated_action_tree_result isolated_action_tree_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
      isolated_action_tree_context tree_context, std::stop_token stop,
      primary_overlay_limits overlay_limits, isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() +
                                                isolated_transaction_executor::max_attempt_time);
   const auto checkpoint = [&] {
      external.check(stop);
   };
   checkpoint();
   if (roots.empty() || roots.size() > isolated_transaction_executor::max_actions ||
       programs.empty() || programs.size() > max_programs) throw primary_state_budget_exceeded();
   struct entry { const isolated_receiver_program* program; fc::sha256 hash; };
   std::map<name, entry> catalog;
   size_t code_bytes = 0, data_bytes = 0;
   for (const auto& program : programs) {
      checkpoint();
      if (program.code.size() > 64 * 1024 ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes)
         throw primary_state_budget_exceeded();
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, entry{&program,
            program.code.empty() ? fc::sha256{} : fc::sha256::hash(program.code.data(), program.code.size())}).second)
         throw primary_state_unsupported();
   }
   const auto lookup = [&](name receiver) -> const entry& {
      const auto it = catalog.find(receiver);
      if (it == catalog.end()) throw isolated_receiver_missing(receiver);
      if (it->second.program->serial_only) throw primary_state_unsupported();
      return it->second;
   };
   std::vector<isolated_action_tree_group> groups;
   std::vector<isolated_action_tree_step> results;
   std::vector<fc::sha256> fingerprints;
   // Stable group storage throughout recursive scheduling. Every append below
   // checks max_steps BEFORE insertion; no reference survives a reallocation.
   groups.reserve(max_steps); results.reserve(max_steps); fingerprints.reserve(roots.size());
   for (size_t i = 0; i < roots.size(); ++i) {
      checkpoint();
      const auto& root = roots[i];
      if (root.data.size() > 64 * 1024 ||
          root.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes ||
          root.authorizations.size() > 64 || root.max_return_bytes > 4096) throw primary_state_budget_exceeded();
      data_bytes += root.data.size();
      if (root.receiver != root.account) throw primary_state_unsupported();
      const auto& program = lookup(root.receiver);
      if (program.program->code.empty()) throw primary_state_unsupported();
      fingerprints.push_back(isolated_execution_fingerprint(program.hash, program.program->limits, root));
      groups.push_back({i, uint32_t(i + 1), 0, 0, 0, {}, root});
   }
   const auto fingerprint = isolated_action_tree_fingerprint(fingerprints, tree_context);
   primary_state_overlay state(std::move(base), overlay_limits);
   uint32_t next_ordinal = roots.size() + 1;
   size_t event_count = 0, inline_count = 0, inline_bytes = 0;
   struct pending_receiver { name receiver; uint32_t ordinal, creator, ancestor; name sender; };
   std::function<void(size_t)> execute_group = [&](size_t group_index) {
      checkpoint();
      const auto& group = groups[group_index];
      primary_overlay_cursor cursor(state, group.input.receiver);
      std::set<name> notified{group.input.receiver};
      std::vector<pending_receiver> pending{{group.input.receiver, group.action_ordinal,
         group.creator_action_ordinal, group.closest_unnotified_ancestor, group.sender}};
      std::vector<size_t> children, context_free_children;
      for (size_t i = 0; i < pending.size(); ++i) {
         checkpoint();
         const auto current = pending[i];
         const auto& entry = lookup(current.receiver);
         const auto& program = *entry.program;
         auto arguments = group.input;
         arguments.receiver = current.receiver;
         isolated_action_tree_step step;
         step.group_index = group_index; step.action_ordinal = current.ordinal;
         step.creator_action_ordinal = current.creator; step.closest_unnotified_ancestor = current.ancestor;
         step.receiver = current.receiver; step.sender = current.sender;
         step.code_hash = entry.hash; step.has_code = !program.code.empty();
         if (step.has_code) {
            auto& module = impl_->modules.get(program.code, program.limits);
            checkpoint();
            isolated_tree_capture capture{
               [&](const isolated_cursor_event& event) {
                  if (event_count >= max_events) throw primary_state_budget_exceeded();
                  ++event_count;
                  step.events.emplace_back(event);
               },
               [&](std::span<const char> bytes, bool context_free) {
                  checkpoint();
                  if (event_count >= max_events || inline_count >= max_steps ||
                      bytes.size() > max_total_inline_bytes - inline_bytes ||
                      bytes.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes)
                     throw primary_state_budget_exceeded();
                  auto sent = decode_isolated_inline_action(bytes, tree_context.max_inline_action_size);
                  if (!sent || (context_free && (!tree_context.context_free_data || !sent->decoded.authorization.empty())))
                     throw primary_state_unsupported();
                  inline_bytes += bytes.size(); data_bytes += bytes.size(); ++inline_count; ++event_count;
                  step.events.emplace_back(isolated_tree_inline_event{std::move(*sent), state.write_count(), 0, context_free});
               }, step.sender, group.context_free, tree_context.context_free_data.get()};
            std::vector<isolated_cursor_event> cursor_events;
            performance::scope execute_timer(performance::stage::parallel_execute);
            step.effects = module.execute_step(state, arguments, stop, external, &cursor, &notified,
               &cursor_events, max_events - event_count, &capture);
         } else {
            step.effects.writes_begin = step.effects.writes_end = state.write_count();
            step.effects.execution_fingerprint = isolated_execution_fingerprint(entry.hash, program.limits, arguments);
         }
         step.effects.execution_fingerprint = isolated_action_tree_step_fingerprint(
            step.effects.execution_fingerprint, tree_context, group, step);
         // Assign ordinals in the host stream's exact interleaving. Scheduling
         // an inline does NOT execute it and does NOT join notification dedup.
         for (auto& event : step.events) {
            if (const auto* cursor_event = std::get_if<isolated_cursor_event>(&event)) {
               if (const auto* notification = std::get_if<isolated_cursor_notify>(cursor_event)) {
                  if (next_ordinal > max_steps) throw primary_state_budget_exceeded();
                  const auto recipient = step.effects.recipients.at(notification->notification_index).recipient;
                  pending.push_back({recipient, next_ordinal++, current.ordinal, group.action_ordinal, current.receiver});
               }
            } else {
               auto& send = std::get<isolated_tree_inline_event>(event);
               if (next_ordinal > max_steps || groups.size() >= max_steps) throw primary_state_budget_exceeded();
               send.action_ordinal = next_ordinal++;
               const auto& act = send.sent.decoded;
               isolated_action_input child;
               child.receiver = child.account = act.account; child.action = act.name;
               child.data = act.data; child.max_return_bytes = group.input.max_return_bytes;
               for (const auto& permission : act.authorization)
                  child.authorizations.emplace_back(permission.actor, permission.permission);
               (send.context_free ? context_free_children : children).push_back(groups.size());
               groups.push_back({group.root_index, send.action_ordinal, current.ordinal, group.action_ordinal,
                  group.depth + 1, current.receiver, std::move(child), send.context_free});
            }
         }
         if (results.size() >= max_steps) throw primary_state_budget_exceeded();
         results.push_back(std::move(step));
      }
      // Original exec(): finish the WHOLE notification group, then context-free
      // children, then ordinary inline groups, in each queue's send order.
      // Each child gets fresh cursors
      // and recipient dedup, but shares transaction state and absolute budgets.
      if ((!children.empty() || !context_free_children.empty()) &&
          (group.depth >= tree_context.max_inline_action_depth || group.depth >= max_depth))
         throw primary_state_unsupported();
      for (const auto child : context_free_children) execute_group(child);
      for (const auto child : children) execute_group(child);
   };
   for (size_t i = 0; i < roots.size(); ++i) execute_group(i);
   checkpoint();
   return {state.seal(), std::move(groups), std::move(results), fingerprint, std::move(tree_context.context_free_data)};
}

bool validate_isolated_notification_journal(const isolated_notification_result& result,
      std::span<const isolated_action_input> roots, std::span<const isolated_receiver_program> programs,
      const std::function<void()>& checkpoint) {
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   const primary_overlay_limits bounds;
   if (!result.state.base() || roots.empty() || roots.size() > isolated_transaction_executor::max_actions ||
       programs.empty() || programs.size() > isolated_notification_executor::max_programs ||
       result.steps.size() < roots.size() || result.steps.size() > isolated_notification_executor::max_steps ||
       result.state.writes().size() > bounds.max_write_operations ||
       result.state.read_tables().size() > bounds.max_read_tables ||
       result.state.read_queries().size() > bounds.max_read_queries ||
       result.state.read_accounts().size() > bounds.max_read_accounts) return false;

   struct entry { const isolated_receiver_program* program; fc::sha256 hash; };
   std::map<name, entry> catalog;
   size_t code_bytes = 0, data_bytes = 0, write_bytes = 0;
   for (const auto& program : programs) {
      check();
      if (program.code.size() > 64 * 1024 ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return false;
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, entry{&program,
            program.code.empty() ? fc::sha256{} : fc::sha256::hash(program.code.data(), program.code.size())}).second)
         return false;
   }
   std::vector<fc::sha256> fingerprints;
   for (const auto& root : roots) {
      check();
      if (root.receiver != root.account || root.data.size() > 64 * 1024 ||
          root.data.size() > isolated_transaction_executor::max_total_action_bytes - data_bytes ||
          root.authorizations.size() > 64 || root.max_return_bytes > 4096) return false;
      data_bytes += root.data.size();
      const auto it = catalog.find(root.receiver);
      if (it == catalog.end() || it->second.program->serial_only || it->second.program->code.empty()) return false;
      fingerprints.push_back(isolated_execution_fingerprint(it->second.hash, it->second.program->limits, root));
   }
   if (isolated_transaction_fingerprint(fingerprints) != result.root_fingerprint) return false;
   for (const auto& write : result.state.writes()) {
      check();
      if (write.value.size() > bounds.max_write_value_bytes - write_bytes) return false;
      write_bytes += write.value.size();
   }

   primary_state_overlay replay(result.state.base());
   size_t next_step = 0, next_write = 0, event_count = 0;
   uint32_t next_ordinal = roots.size() + 1;
   std::chrono::microseconds elapsed{};
   for (size_t root_index = 0; root_index < roots.size(); ++root_index) {
      const auto& root = roots[root_index];
      primary_overlay_cursor cursor(replay, root.receiver);
      struct pending_receiver { name receiver; uint32_t ordinal, creator; };
      std::vector<pending_receiver> pending{{root.receiver, uint32_t(root_index + 1), 0}};
      std::set<name> notified{root.receiver};
      for (size_t index = 0; index < pending.size(); ++index) {
         check();
         if (next_step == result.steps.size()) return false;
         const auto expected = pending[index];
         const auto& step = result.steps[next_step++];
         const auto& effects = step.effects;
         if (step.root_index != root_index || step.receiver != expected.receiver ||
             step.action_ordinal != expected.ordinal || step.creator_action_ordinal != expected.creator ||
             effects.writes_begin != next_write || effects.writes_end < next_write ||
             effects.writes_end > result.state.writes().size() ||
             effects.recipients.size() > isolated_action_result::max_recipients ||
             effects.return_value.size() > root.max_return_bytes || effects.elapsed.count() < 0 ||
             effects.elapsed > isolated_transaction_executor::max_attempt_time - elapsed ||
             step.cursor_events.size() > isolated_notification_executor::max_cursor_events - event_count) return false;
         elapsed += effects.elapsed;
         event_count += step.cursor_events.size();
         const auto found = catalog.find(step.receiver);
         if (found == catalog.end()) return false;
         const auto& program = *found->second.program;
         if (program.serial_only || step.code_hash != found->second.hash || step.has_code != !program.code.empty()) return false;
         auto arguments = root;
         arguments.receiver = step.receiver;
         if (effects.execution_fingerprint != isolated_execution_fingerprint(found->second.hash, program.limits, arguments))
            return false;
         if (!step.has_code && (!step.cursor_events.empty() || !effects.recipients.empty() ||
               !effects.return_value.empty() || effects.writes_end != next_write || effects.elapsed.count() != 0)) return false;
         cursor.set_receiver_for_notification(step.receiver);
         size_t next_notification = 0;
         for (const auto& event : step.cursor_events) {
            // Never catch a deadline/interrupt from the original owner. Only
            // errors from the PRIVATE reconstruction below select fallback.
            check();
            try {
               if (const auto* query = std::get_if<isolated_cursor_query>(&event)) {
                  using kind = isolated_cursor_query_kind;
                  int32_t actual;
                  if (query->kind == kind::next || query->kind == kind::previous) {
                     if (query->table != table_key{} || query->key != 0) return false;
                     auto primary = query->primary_before;
                     actual = query->kind == kind::next ? cursor.next(query->iterator, primary) :
                                                         cursor.previous(query->iterator, primary);
                     if (primary != query->primary_after) return false;
                  } else {
                     if (query->iterator != -1 || query->primary_before != 0 || query->primary_after != 0) return false;
                     switch (query->kind) {
                        case kind::find: actual = cursor.find(query->table, query->key); break;
                        case kind::lower_bound: actual = cursor.lower_bound(query->table, query->key); break;
                        case kind::upper_bound: actual = cursor.upper_bound(query->table, query->key); break;
                        case kind::end:
                           if (query->key != 0) return false;
                           actual = cursor.end(query->table); break;
                        default: return false;
                     }
                  }
                  if (actual != query->result) return false;
               } else if (const auto* write_event = std::get_if<isolated_cursor_write>(&event)) {
                  if (write_event->write_index != next_write || next_write >= effects.writes_end) return false;
                  const auto& write = result.state.writes()[next_write++];
                  if (write.table.code != step.receiver) return false;
                  switch (write.operation) {
                     case primary_operation::store:
                        if (cursor.store(write.table, write.primary_key, write.payer, write.value) != write_event->iterator)
                           return false;
                        break;
                     case primary_operation::update: cursor.update(write_event->iterator, write.payer, write.value); break;
                     case primary_operation::remove: cursor.remove(write_event->iterator); break;
                     default: return false;
                  }
               } else if (const auto* notification = std::get_if<isolated_cursor_notify>(&event)) {
                  if (notification->notification_index != next_notification || next_notification >= effects.recipients.size())
                     return false;
                  const auto& recipient = effects.recipients[next_notification++];
                  if (recipient.after_writes != next_write || !recipient.recipient ||
                      notified.size() >= isolated_action_result::max_recipients + 1 ||
                      !notified.insert(recipient.recipient).second || next_ordinal > isolated_notification_executor::max_steps)
                     return false;
                  pending.push_back({recipient.recipient, next_ordinal++, step.action_ordinal});
               } else return false;
            } catch (const std::exception&) {
               // Invalid/retired handles, absent capture, foreign writes and
               // local reconstruction budgets never mutate the original state.
               return false;
            }
         }
         if (next_write != effects.writes_end || next_notification != effects.recipients.size()) return false;
      }
   }
   if (next_step != result.steps.size() || next_write != result.state.writes().size()) return false;
   const auto reconstructed = replay.seal();
   if (reconstructed.writes().size() != result.state.writes().size()) return false;
   for (size_t i = 0; i < reconstructed.writes().size(); ++i) {
      check();
      const auto& actual = reconstructed.writes()[i];
      const auto& expected = result.state.writes()[i];
      if (actual.table != expected.table || actual.primary_key != expected.primary_key ||
          actual.payer != expected.payer || actual.value != expected.value || actual.operation != expected.operation ||
          actual.object_id != expected.object_id || actual.table_object_id != expected.table_object_id) return false;
   }
   // db_get/is_account need not create cursor events, so the original proof
   // may be larger. It may NEVER be smaller: otherwise a forged event could
   // read a batch-captured row which was not revalidated against current state.
   for (const auto& table : reconstructed.read_tables()) {
      check(); if (!result.state.read_tables().contains(table)) return false;
   }
   for (const auto& query : reconstructed.read_queries()) {
      check(); if (!result.state.read_queries().contains(query)) return false;
   }
   check();
   return true;
}

} // namespace eosio::chain::experimental
