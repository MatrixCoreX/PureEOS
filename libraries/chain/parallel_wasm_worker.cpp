#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/vm/backend.hpp>
#include <eosio/vm/host_function.hpp>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>

namespace eosio::chain::experimental {
namespace {

// A bounded timer/cancellation guard. Destruction synchronizes with the callback
// before backend::timed_run restores code permissions or the module is destroyed.
class attempt_watchdog {
public:
   attempt_watchdog(std::chrono::milliseconds duration, std::stop_token stop) : duration_(duration), stop_(stop) {}
   template<typename F> class guard {
   public:
      guard(std::chrono::milliseconds duration, std::stop_token stop, F callback)
         : stop_(stop), callback_(std::move(callback)),
           stop_callback_(stop_, [this] { std::lock_guard lock(mutex_); condition_.notify_one(); }),
           timer_([this, duration] {
              std::unique_lock lock(mutex_);
              condition_.wait_for(lock, duration, [&] { return done_ || stop_.stop_requested(); });
              if (!done_) callback_();
           }) {}
      ~guard() {
         { std::lock_guard lock(mutex_); done_ = true; }
         condition_.notify_one();
         timer_.join();
      }
      guard(const guard&) = delete;
      guard& operator=(const guard&) = delete;
   private:
      std::stop_token stop_;
      F callback_;
      std::mutex mutex_;
      std::condition_variable condition_;
      bool done_ = false;
      std::stop_callback<std::function<void()>> stop_callback_;
      std::thread timer_;
   };
   template<typename F> auto scoped_run(F&& callback) {
      return guard<std::decay_t<F>>(duration_, stop_, std::forward<F>(callback));
   }
private:
   std::chrono::milliseconds duration_;
   std::stop_token stop_;
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
   const isolated_action_input& input;
   std::stop_token stop;
   std::function<std::span<char>()> linear_memory;
   std::vector<char> return_value;

   void check() const { if (stop.stop_requested()) throw primary_state_invalid_attempt(); }
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
   void set_action_return_value(uint32_t offset, uint32_t size) {
      if (size > input.max_return_bytes) throw primary_state_unsupported();
      const auto source = memory(offset, size);
      return_value.assign(source.begin(), source.end());
   }
   int32_t db_find_i64(uint64_t code, uint64_t scope, uint64_t table, uint64_t primary) {
      check(); return cursor.find({name(code), name(scope), name(table)}, primary);
   }
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
      ISOLATED_HOST(set_action_return_value);
      ISOLATED_HOST(db_find_i64);
      ISOLATED_HOST(db_lowerbound_i64);
      ISOLATED_HOST(db_upperbound_i64);
      ISOLATED_HOST(db_end_i64);
      ISOLATED_HOST(db_get_i64);
      ISOLATED_HOST(db_update_i64);
      ISOLATED_HOST(db_next_i64);
      ISOLATED_HOST(db_previous_i64);
#undef ISOLATED_HOST
   });
}
} // namespace

struct isolated_wasm_module::impl {
   const std::thread::id owner = std::this_thread::get_id();
   isolated_wasm_limits limits;
   isolated_vm_options options;
   std::vector<uint8_t> owned_code;
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
      // A local memory cap must not turn a successful canonical memory.grow
      // into -1 and let the contract take a different successful branch.
      // Require an explicit module maximum inside our local cap. The later
      // adopter must also check that this maximum fits the protocol limit.
      if (memories.size() > 1 || (memories.size() &&
          (!memories[0].limits.flags || memories[0].limits.maximum > limits.max_pages)))
         throw primary_state_unsupported();
      machine->set_wasm_allocator(&allocator);
   }
};

isolated_wasm_module::isolated_wasm_module(std::span<const char> code, isolated_wasm_limits limits)
   : impl_(std::make_unique<impl>(code, limits)) {}
isolated_wasm_module::~isolated_wasm_module() = default;

isolated_action_result isolated_wasm_module::execute(std::shared_ptr<const frozen_primary_state> base,
      const isolated_action_input& input, std::stop_token stop, primary_overlay_limits overlay_limits) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   if (stop.stop_requested()) throw primary_state_invalid_attempt();
   if (input.receiver != input.account || input.data.size() > impl_->limits.max_action_bytes ||
       input.authorizations.size() > impl_->limits.max_authorizations || input.max_return_bytes > impl_->limits.max_return_bytes)
      throw primary_state_unsupported();
   primary_state_overlay state(std::move(base), overlay_limits);
   primary_overlay_cursor cursor(state, input.receiver);
   auto& machine = *impl_->machine;
   isolated_host host{cursor, input, stop, [&]() -> std::span<char> {
      auto& context = machine.get_context();
      return {context.linear_memory(), static_cast<size_t>(context.current_linear_memory()) * 65536u};
   }, {}};
   machine.timed_run(attempt_watchdog(impl_->limits.max_attempt_time, stop), [&] {
      machine.initialize(&host, impl_->options);
      machine.call(host, "env", "apply", input.receiver.to_uint64_t(), input.account.to_uint64_t(), input.action.to_uint64_t());
   });
   if (stop.stop_requested()) throw primary_state_invalid_attempt();
   return {state.seal(), std::move(host.return_value)};
}

} // namespace eosio::chain::experimental
