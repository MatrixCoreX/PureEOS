#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/vm/backend.hpp>
#include <eosio/vm/host_function.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>

namespace eosio::chain::experimental {
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
      if (stop_.stop_requested() || external_.stop_requested()) throw primary_state_invalid_attempt();
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
   template<typename Op> void operator()(const Op& op) {
      if (--remaining == 0) { deadline.check(); remaining = 1024; }
      original(op);
   }
   void operator()(const eosio::vm::grow_memory_t& op) {
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

   void check() const { if (stop.stop_requested() || external.stop_requested()) throw primary_state_invalid_attempt(); }
   std::span<char> memory(uint32_t offset, uint32_t size) {
      check();
      auto bytes = linear_memory();
      if (offset > bytes.size() || size > bytes.size() - offset) throw primary_state_unsupported();
      return bytes.subspan(offset, size);
   }
   uint64_t current_receiver() { check(); return input.receiver.to_uint64_t(); }
   uint32_t action_data_size() { check(); return input.data.size(); }
   uint32_t read_action_data(uint32_t offset, uint32_t size) {
      auto destination = memory(offset, size);
      const auto count = std::min(destination.size(), input.data.size());
      if (!count) return input.data.size();
      std::copy_n(input.data.begin(), count, destination.begin());
      return count;
   }
   int32_t has_auth(uint64_t actor) {
      check();
      return std::any_of(input.authorizations.begin(), input.authorizations.end(),
                         [&](const auto& item) { return item.first.to_uint64_t() == actor; });
   }
   void require_auth(uint64_t actor) { if (!has_auth(actor)) throw primary_state_unsupported(); }
   void require_auth2(uint64_t actor, uint64_t permission) {
      check();
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

   int32_t is_account(uint64_t account) { check(); return bool(state.read_account(name{account})); }
   void require_recipient(uint64_t account) {
      check();
      const name recipient{account};
      if (recipient == input.receiver || std::any_of(recipients.begin(), recipients.end(),
            [&](const auto& notification) { return notification.recipient == recipient; })) return;
      if (recipients.size() == isolated_action_result::max_recipients) throw primary_state_budget_exceeded();
      recipients.push_back({recipient, state.write_count()});
   }
   // Explicit link-only imports used by ordinary CDT modules. NONE is an
   // implementation: reaching any of these discards the WHOLE attempt, even
   // after private writes. No default result, output or canonical effect.
   [[noreturn]] void prints_l(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] void send_inline(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] void set_blockchain_parameters_packed(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] uint32_t get_blockchain_parameters_packed(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] int64_t set_proposed_producers(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   [[noreturn]] uint64_t current_time() { throw primary_state_unsupported(); }
   [[noreturn]] int32_t get_active_producers(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   void set_action_return_value(uint32_t offset, uint32_t size) {
      if (size > input.max_return_bytes) throw primary_state_unsupported();
      const auto source = memory(offset, size);
      return_value.assign(source.begin(), source.end());
   }
   int32_t db_find_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      check(); return cursor.find({name(code), name(scope), name(table)}, primary);
   }
   int32_t db_store_i64(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset, uint32_t size) {
      return cursor.store({input.receiver, name(scope), name(table)}, primary, name(payer), memory(offset, size));
   }
   void db_remove_i64(int32_t iterator) { check(); cursor.remove(iterator); }
   int32_t db_lowerbound_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      check(); return cursor.lower_bound({name(code), name(scope), name(table)}, primary);
   }
   int32_t db_upperbound_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      check(); return cursor.upper_bound({name(code), name(scope), name(table)}, primary);
   }
   int32_t db_end_i64(uint64_t code, uint64_t scope, uint64_t table) {
      check(); return cursor.end({name(code), name(scope), name(table)});
   }
   int32_t db_get_i64(int32_t iterator, uint32_t offset, uint32_t size) {
      return cursor.get(iterator, memory(offset, size));
   }
   void db_update_i64(int32_t iterator, uint64_t payer, uint32_t offset, uint32_t size) {
      cursor.update(iterator, name(payer), memory(offset, size));
   }
   int32_t adjacent(int32_t iterator, uint32_t offset, bool previous) {
      auto destination = memory(offset, sizeof(uint64_t));
      uint64_t primary;
      std::memcpy(&primary, destination.data(), sizeof(primary));
      const auto result = previous ? cursor.previous(iterator, primary) : cursor.next(iterator, primary);
      std::memcpy(destination.data(), &primary, sizeof(primary)); // unaligned WASM pointers are legal
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
      ISOLATED_FALLBACK(prints_l);
      ISOLATED_FALLBACK(send_inline);
      ISOLATED_HOST(db_store_i64);
      ISOLATED_HOST(db_remove_i64);
      ISOLATED_FALLBACK(set_blockchain_parameters_packed);
      ISOLATED_FALLBACK(get_blockchain_parameters_packed);
      ISOLATED_FALLBACK(set_proposed_producers);
      ISOLATED_FALLBACK(current_time);
      ISOLATED_FALLBACK(get_active_producers);
#undef ISOLATED_FALLBACK
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
      const isolated_action_input& input, std::stop_token stop, isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   const auto began = std::chrono::steady_clock::now();
   if (stop.stop_requested() || external.stop_requested()) throw primary_state_invalid_attempt();
   if (input.receiver != input.account || input.data.size() > impl_->limits.max_action_bytes ||
       input.authorizations.size() > impl_->limits.max_authorizations || input.max_return_bytes > impl_->limits.max_return_bytes)
      throw primary_state_unsupported();
   const auto fingerprint = isolated_execution_fingerprint(impl_->code_hash, impl_->limits, input);
   const auto writes_begin = state.write_count();
   primary_overlay_cursor cursor(state, input.receiver);
   auto& machine = *impl_->machine;
   isolated_host host{cursor, state, input, stop, external, [&]() -> std::span<char> {
      auto& context = machine.get_context();
      const int32_t pages = context.current_linear_memory();
      // EOS VM uses -1 for modules with NO memory. Never convert that sentinel
      // into a huge unsigned span. Zero-page memory calls conservatively fall
      // back too; non-memory hosts remain usable without linear memory.
      if (pages <= 0) throw primary_state_unsupported();
      return {context.linear_memory(), static_cast<size_t>(pages) * 65536u};
   }, {}, {}};
   const attempt_deadline deadline(impl_->limits.max_attempt_time, stop, external);
   auto& context = machine.get_context();
   // Same reset/start sequence as backend::initialize, with a bounded visitor
   // for BOTH start and apply. No shared VM module/context is introduced.
   context.set_max_call_depth(impl_->options.max_call_depth);
   context.set_max_pages(impl_->options.max_pages);
   context.reset();
   interruptible_visitor<std::remove_reference_t<decltype(context)>> visitor{{context}, deadline, impl_->limits.max_pages};
   deadline.check();
   context.execute_start(&host, visitor);
   context.execute(&host, visitor, "apply", input.receiver.to_uint64_t(), input.account.to_uint64_t(), input.action.to_uint64_t());
   deadline.check();
   const auto ended = std::chrono::steady_clock::now();
   return {std::move(host.return_value), fingerprint, std::move(host.recipients), writes_begin, state.write_count(),
           std::chrono::duration_cast<std::chrono::microseconds>(ended - began), began, ended};
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

struct isolated_transaction_executor::impl {
   const std::thread::id owner = std::this_thread::get_id();
   std::unique_ptr<isolated_wasm_module> module;
   std::vector<char> cached_code;
   isolated_wasm_limits cached_limits;
};

isolated_transaction_executor::isolated_transaction_executor() : impl_(std::make_unique<impl>()) {}
isolated_transaction_executor::~isolated_transaction_executor() = default;

isolated_transaction_result isolated_transaction_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      std::span<const isolated_action_invocation> actions, std::stop_token stop,
      primary_overlay_limits overlay_limits, isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() + max_attempt_time);
   auto checkpoint = [&] {
      if (stop.stop_requested() || external.stop_requested()) throw primary_state_invalid_attempt();
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
      if (!impl_->module || impl_->cached_code != action.code || impl_->cached_limits != action.limits) {
         performance::scope compile_timer(performance::stage::parallel_compile);
         // Never hold two compiled modules, including when construction fails.
         impl_->module.reset(); impl_->cached_code.clear();
         impl_->module = std::make_unique<isolated_wasm_module>(action.code, action.limits);
         impl_->cached_code = action.code; impl_->cached_limits = action.limits;
      }
      checkpoint();
      {
         performance::scope execute_timer(performance::stage::parallel_execute);
         results.push_back(impl_->module->execute_step(state, action.input, stop, external));
      }
      fingerprints.push_back(results.back().execution_fingerprint);
   }
   checkpoint();
   const auto fingerprint = isolated_transaction_fingerprint(fingerprints);
   // State/VM/cursor exceptions propagate to the caller. The local state and
   // ALL step outputs die together; no earlier step can escape as a success.
   return {state.seal(), std::move(results), fingerprint};
}

} // namespace eosio::chain::experimental
