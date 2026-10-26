#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/parallel_sync_discovery.hpp>
#include <eosio/chain/thread_cpu_billing.hpp>
#include <eosio/chain/parallel_mixed_tree.hpp>
#include <eosio/chain/parallel_pure_hash.hpp>
#include <eosio/chain/parallel_pure_integer.hpp>
#include <eosio/chain/parallel_pure_float.hpp>
#include <eosio/chain/parallel_deadline_service.hpp>
#include <eosio/chain/parallel_jit_backend.hpp>
#include <eosio/chain/parallel_runtime_limits.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/vm/argument_proxy.hpp>
#include <eosio/vm/backend.hpp>
#include <eosio/vm/host_function.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/sha1.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/scoped_exit.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>

namespace eosio::chain::experimental {

// Only the tree and mixed executors supply this synchronous stack-local capability.
// Legacy module/transaction/notification APIs never receive it. No reference
// to it, the VM memory, or the caller's inputs survives execute_step.
struct isolated_tree_capture {
   std::function<void(const isolated_cursor_event&)> record;
   std::function<void(std::span<const char>, bool context_free)> send;
   name sender;
   bool context_free = false;
   const isolated_context_free_data* context_free_data = nullptr; // execute_step stack only
   isolated_console_budget* console_budget = nullptr;
   bool console_enabled = false;
};

// Stack-only capability supplied by the sync or mixed PRIVATE executor. A
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
   thread_cpu_clock::duration children_cpu{};
   bool children_cpu_valid = true;
   // Ordinary receivers reuse one apply_context throughout a notification
   // group. Its most recent sync return survives receiver changes, unlike
   // action_return_value and call ordinals. Borrow only during execute_step;
   // sync children and new root/inline groups NEVER share this buffer.
   std::vector<char>* group_call_return = nullptr;
   isolated_console_budget* console_budget = nullptr;
   bool console_enabled = false;
   uint32_t console_events = 0; // THIS frame only, not its children
};

namespace {

// Frame-local normal termination, distinct from a VM trap or cancellation.
// Catch only at execute_step's shared start/entry boundary, never around a
// whole transaction/tree: parents and queued actions must keep executing.
struct isolated_frame_exit {};

// Never attach a program request to generic is_account or root observations.
// Existence still precedes flags validation: an absent target must retain the
// original -1 result even for unsupported flags. Unknown is not absence.
bool read_sync_target_account(primary_state_overlay& state, name receiver, uint64_t flags) {
   try {
      return bool(state.read_account(receiver));
   } catch (const primary_state_missing& missing) {
      const auto* account = std::get_if<account_capture_request>(&missing.request);
      if (flags <= 1 && account && account->account == receiver)
         throw isolated_sync_target_missing(receiver);
      throw;
   }
}

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
   auto effective_end() const noexcept { return std::min(end_, external_.deadline); }
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
   static constexpr uint32_t max_function_section_elements = isolated_wasm_limits::max_local_functions;
   static constexpr uint32_t max_import_section_elements = isolated_wasm_limits::max_imports;
   static constexpr uint32_t max_code_bytes = isolated_wasm_limits::max_function_body_bytes;
   static constexpr bool forbid_export_mutable_globals = false;
   static constexpr bool allow_code_after_function_end = false;
};

template<secondary_key_kind Kind>
using secondary_wire_type = std::conditional_t<Kind == secondary_key_kind::uint64, uint64_t,
   std::conditional_t<Kind == secondary_key_kind::uint128 || Kind == secondary_key_kind::uint256, uint128_t,
   std::conditional_t<Kind == secondary_key_kind::float64, float64_t, float128_t>>>;

struct isolated_host {
   primary_overlay_cursor& cursor;
   secondary_cursor_group& secondary;
   primary_state_overlay& state;
   const isolated_action_input& input;
   std::stop_token stop;
   isolated_cancel external;
   const attempt_deadline& deadline; // execute_step stack only; never cached
   std::function<std::span<char>()> linear_memory;
   std::vector<char> return_value;
   std::vector<isolated_notification> recipients;
   std::set<name>* group_recipients = nullptr;
   std::vector<isolated_cursor_event>* cursor_events = nullptr;
   size_t max_cursor_events = 0;
   isolated_tree_capture* tree_capture = nullptr;
   isolated_sync_capture* sync_capture = nullptr;
   std::vector<char> last_call_return;

   std::vector<char>& call_return_buffer() {
      return sync_capture && sync_capture->group_call_return ?
         *sync_capture->group_call_return : last_call_return;
   }

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
   template<class Digest> void hash(uint32_t offset, uint32_t size, uint32_t destination) {
      const auto data = memory(offset, size);
      // Same non-null/alignment/writeback rules as ORIGINAL legacy_ptr. Hash
      // input is a char span (offset zero is legal), and may alias the output.
      auto out = scalar_pointer<Digest>(destination);
      const auto result = isolated_hash_with_checktime<Digest>(data, [&] { deadline.check(); });
      *out.get() = result; // compute completely BEFORE publishing to VM memory
   }
   template<class Digest> void assert_hash(uint32_t offset, uint32_t size, uint32_t expected) {
      const auto data = memory(offset, size);
      const auto value = scalar_pointer<const Digest>(expected);
      const auto result = isolated_hash_with_checktime<Digest>(data, [&] { deadline.check(); });
      if (!(result == *value.get())) throw primary_state_unsupported();
      // An error discards the whole private attempt. The ORIGINAL serial host
      // determines the consensus exception; do not publish a private error.
   }
   // All eight are ORIGINAL legacy CF hosts: no action-only/context-aware gate.
   void sha1(uint32_t data, uint32_t size, uint32_t out) { hash<fc::sha1>(data, size, out); }
   void sha256(uint32_t data, uint32_t size, uint32_t out) { hash<fc::sha256>(data, size, out); }
   void sha512(uint32_t data, uint32_t size, uint32_t out) { hash<fc::sha512>(data, size, out); }
   void ripemd160(uint32_t data, uint32_t size, uint32_t out) { hash<fc::ripemd160>(data, size, out); }
   void assert_sha1(uint32_t data, uint32_t size, uint32_t out) { assert_hash<fc::sha1>(data, size, out); }
   void assert_sha256(uint32_t data, uint32_t size, uint32_t out) { assert_hash<fc::sha256>(data, size, out); }
   void assert_sha512(uint32_t data, uint32_t size, uint32_t out) { assert_hash<fc::sha512>(data, size, out); }
   void assert_ripemd160(uint32_t data, uint32_t size, uint32_t out) { assert_hash<fc::ripemd160>(data, size, out); }
   template<isolated_integer_operation Operation, class Output>
   void integer128(uint32_t destination, uint64_t la, uint64_t ha, uint64_t lb = 0, uint64_t hb = 0, uint32_t shift = 0) {
      // Original legacy scalar pointer rules (including 16-byte alignment and
      // unaligned writeback) apply BEFORE arithmetic, also on rejected inputs.
      auto out = scalar_pointer<Output>(destination);
      deadline.check();
      const auto result = isolated_integer128(Operation, isolated_integer_words(la, ha), isolated_integer_words(lb, hb), shift);
      if (!result) throw primary_state_unsupported();
      deadline.check();
      *out.get() = std::bit_cast<Output>(*result);
   }
   // These nine are original CF hosts; do not require an ordinary action or
   // expose any ledger capability merely to compute an integer result.
   void __ashlti3(uint32_t out, uint64_t l, uint64_t h, uint32_t s) {
      integer128<isolated_integer_operation::arithmetic_left, __int128>(out, l, h, 0, 0, s);
   }
   void __ashrti3(uint32_t out, uint64_t l, uint64_t h, uint32_t s) {
      integer128<isolated_integer_operation::arithmetic_right, __int128>(out, l, h, 0, 0, s);
   }
   void __lshlti3(uint32_t out, uint64_t l, uint64_t h, uint32_t s) {
      integer128<isolated_integer_operation::logical_left, __int128>(out, l, h, 0, 0, s);
   }
   void __lshrti3(uint32_t out, uint64_t l, uint64_t h, uint32_t s) {
      integer128<isolated_integer_operation::logical_right, __int128>(out, l, h, 0, 0, s);
   }
   void __divti3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
      integer128<isolated_integer_operation::signed_divide, __int128>(out, la, ha, lb, hb);
   }
   void __udivti3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
      integer128<isolated_integer_operation::unsigned_divide, unsigned __int128>(out, la, ha, lb, hb);
   }
   void __multi3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
      integer128<isolated_integer_operation::multiply, unsigned __int128>(out, la, ha, lb, hb);
   }
   void __modti3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
      integer128<isolated_integer_operation::signed_modulo, __int128>(out, la, ha, lb, hb);
   }
   void __umodti3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
      integer128<isolated_integer_operation::unsigned_modulo, unsigned __int128>(out, la, ha, lb, hb);
   }
   template<isolated_float_operation Operation>
   isolated_integer_bits float_value(isolated_integer_bits a, isolated_integer_bits b = 0) {
      deadline.check();
      const auto value = isolated_float_compute(Operation, a, b);
      if (!value) throw primary_state_unsupported();
      deadline.check();
      return *value;
   }
   template<isolated_float_operation Operation, class Output>
   void float_output(uint32_t destination, isolated_integer_bits a, isolated_integer_bits b = 0) {
      // float128_t has ORIGINAL 8-byte alignment, integer outputs have 16.
      // Validate the legacy pointer before computation, including guard failures.
      auto out = scalar_pointer<Output>(destination);
      const auto value = float_value<Operation>(a, b);
      static_assert(std::endian::native == std::endian::little && sizeof(Output) == 16);
      *out.get() = std::bit_cast<Output>(value);
   }
   // Original CF interfaces: valid in normal/CF/sync/mixed and module start.
   void __addtf3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { float_output<isolated_float_operation::addtf3, float128_t>(out, isolated_integer_words(la, ha), isolated_integer_words(lb, hb)); }
   void __subtf3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { float_output<isolated_float_operation::subtf3, float128_t>(out, isolated_integer_words(la, ha), isolated_integer_words(lb, hb)); }
   void __multf3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { float_output<isolated_float_operation::multf3, float128_t>(out, isolated_integer_words(la, ha), isolated_integer_words(lb, hb)); }
   void __divtf3(uint32_t out, uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { float_output<isolated_float_operation::divtf3, float128_t>(out, isolated_integer_words(la, ha), isolated_integer_words(lb, hb)); }
   void __negtf2(uint32_t out, uint64_t la, uint64_t ha) { float_output<isolated_float_operation::negtf2, float128_t>(out, isolated_integer_words(la, ha)); }
   void __extendsftf2(uint32_t out, float a) { float_output<isolated_float_operation::extendsftf2, float128_t>(out, std::bit_cast<uint32_t>(a)); }
   void __extenddftf2(uint32_t out, double a) { float_output<isolated_float_operation::extenddftf2, float128_t>(out, std::bit_cast<uint64_t>(a)); }
   double __trunctfdf2(uint64_t la, uint64_t ha) { return std::bit_cast<double>(uint64_t(float_value<isolated_float_operation::trunctfdf2>(isolated_integer_words(la, ha)))); }
   float __trunctfsf2(uint64_t la, uint64_t ha) { return std::bit_cast<float>(uint32_t(float_value<isolated_float_operation::trunctfsf2>(isolated_integer_words(la, ha)))); }
   int32_t __fixtfsi(uint64_t la, uint64_t ha) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::fixtfsi>(isolated_integer_words(la, ha)))); }
   int64_t __fixtfdi(uint64_t la, uint64_t ha) { return std::bit_cast<int64_t>(uint64_t(float_value<isolated_float_operation::fixtfdi>(isolated_integer_words(la, ha)))); }
   void __fixtfti(uint32_t out, uint64_t la, uint64_t ha) { float_output<isolated_float_operation::fixtfti, __int128>(out, isolated_integer_words(la, ha)); }
   uint32_t __fixunstfsi(uint64_t la, uint64_t ha) { return uint32_t(float_value<isolated_float_operation::fixunstfsi>(isolated_integer_words(la, ha))); }
   uint64_t __fixunstfdi(uint64_t la, uint64_t ha) { return uint64_t(float_value<isolated_float_operation::fixunstfdi>(isolated_integer_words(la, ha))); }
   void __fixunstfti(uint32_t out, uint64_t la, uint64_t ha) { float_output<isolated_float_operation::fixunstfti, unsigned __int128>(out, isolated_integer_words(la, ha)); }
   void __fixsfti(uint32_t out, float a) { float_output<isolated_float_operation::fixsfti, __int128>(out, std::bit_cast<uint32_t>(a)); }
   void __fixdfti(uint32_t out, double a) { float_output<isolated_float_operation::fixdfti, __int128>(out, std::bit_cast<uint64_t>(a)); }
   void __fixunssfti(uint32_t out, float a) { float_output<isolated_float_operation::fixunssfti, unsigned __int128>(out, std::bit_cast<uint32_t>(a)); }
   void __fixunsdfti(uint32_t out, double a) { float_output<isolated_float_operation::fixunsdfti, unsigned __int128>(out, std::bit_cast<uint64_t>(a)); }
   double __floatsidf(int32_t a) { return std::bit_cast<double>(uint64_t(float_value<isolated_float_operation::floatsidf>(uint32_t(a)))); }
   void __floatsitf(uint32_t out, int32_t a) { float_output<isolated_float_operation::floatsitf, float128_t>(out, uint32_t(a)); }
   void __floatditf(uint32_t out, uint64_t a) { float_output<isolated_float_operation::floatditf, float128_t>(out, a); }
   void __floatunsitf(uint32_t out, uint32_t a) { float_output<isolated_float_operation::floatunsitf, float128_t>(out, a); }
   void __floatunditf(uint32_t out, uint64_t a) { float_output<isolated_float_operation::floatunditf, float128_t>(out, a); }
   double __floattidf(uint64_t la, uint64_t ha) { return std::bit_cast<double>(uint64_t(float_value<isolated_float_operation::floattidf>(isolated_integer_words(la, ha)))); }
   double __floatuntidf(uint64_t la, uint64_t ha) { return std::bit_cast<double>(uint64_t(float_value<isolated_float_operation::floatuntidf>(isolated_integer_words(la, ha)))); }
   int32_t __eqtf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::eqtf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __netf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::netf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __getf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::getf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __gttf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::gttf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __letf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::letf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __lttf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::lttf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __cmptf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::cmptf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
   int32_t __unordtf2(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) { return std::bit_cast<int32_t>(uint32_t(float_value<isolated_float_operation::unordtf2>(isolated_integer_words(la, ha), isolated_integer_words(lb, hb)))); }
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
   [[noreturn]] void eosio_exit(int32_t) {
      check();
      throw isolated_frame_exit{}; // original host ignores the numeric code
   }

   int32_t is_account(uint64_t account) { context_aware(); return bool(state.read_account(name{account})); }
   void require_recipient(uint64_t account) {
      context_aware();
      action_only();
      if (sync_capture && !tree_capture) throw isolated_mixed_mode_required();
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
   void print(isolated_console_operation operation, std::array<uint64_t, 2> bits,
              std::span<const char> bytes, size_t argument_bytes) {
      deadline.check();
      // Legacy root/notification APIs have no bound console configuration or
      // complete ordered transcript: never silently drop their output.
      auto* budget = sync_capture ? sync_capture->console_budget : tree_capture ? tree_capture->console_budget : nullptr;
      if (!budget) throw primary_state_unsupported();
      const bool enabled = sync_capture ? sync_capture->console_enabled : tree_capture->console_enabled;
      if (!enabled) return; // ABI validation has ALREADY happened in the caller
      if (!budget->consume(argument_bytes)) throw primary_state_budget_exceeded();
      isolated_console_event event{operation, bits, {}, state.write_count()};
      event.bytes.assign(bytes.begin(), bytes.end()); // bounded BEFORE allocation
      record(std::move(event));
      if (sync_capture) ++sync_capture->console_events;
      deadline.check();
   }
   void prints(uint32_t offset) {
      memory(offset, 1); // original NUL pointer accepts address zero, unlike legacy_ptr
      const auto bytes = linear_memory().subspan(offset);
      size_t scanned = 0;
      while (scanned < bytes.size()) {
         deadline.check();
         const auto chunk = bytes.subspan(scanned, std::min<size_t>(10240, bytes.size() - scanned));
         const auto end = std::find(chunk.begin(), chunk.end(), '\0');
         if (end != chunk.end()) {
            const auto size = scanned + static_cast<size_t>(end - chunk.begin()) + 1;
            print(isolated_console_operation::prints, {}, bytes.first(size), size);
            return;
         }
         scanned += chunk.size();
      }
      throw primary_state_unsupported();
   }
   void prints_l(uint32_t offset, uint32_t size) {
      print(isolated_console_operation::prints_l, {}, memory(offset, size), size);
   }
   void printhex(uint32_t offset, uint32_t size) {
      print(isolated_console_operation::printhex, {}, memory(offset, size), size);
   }
   void printi(int64_t value) { print(isolated_console_operation::printi, {std::bit_cast<uint64_t>(value), 0}, {}, 8); }
   void printui(uint64_t value) { print(isolated_console_operation::printui, {value, 0}, {}, 8); }
   void printn(uint64_t value) { print(isolated_console_operation::printn, {value, 0}, {}, 8); }
   void printsf(float value) { print(isolated_console_operation::printsf, {std::bit_cast<uint32_t>(value), 0}, {}, 4); }
   void printdf(double value) { print(isolated_console_operation::printdf, {std::bit_cast<uint64_t>(value), 0}, {}, 8); }
   template<class T> void print128(uint32_t offset, isolated_console_operation operation) {
      static_assert(std::endian::native == std::endian::little && sizeof(T) == 16);
      const auto value = scalar_pointer<const T>(offset); // includes non-null and historical alignment rules
      const auto bits = std::bit_cast<std::array<uint64_t, 2>>(*value.get());
      print(operation, bits, {}, 16);
   }
   void printi128(uint32_t offset) { print128<__int128>(offset, isolated_console_operation::printi128); }
   void printui128(uint32_t offset) { print128<unsigned __int128>(offset, isolated_console_operation::printui128); }
   void printqf(uint32_t offset) { print128<float128_t>(offset, isolated_console_operation::printqf); }
   void send_inline(uint32_t offset, uint32_t size) {
      context_aware();
      action_only();
      if (!tree_capture) {
         if (sync_capture) throw isolated_mixed_mode_required();
         throw primary_state_unsupported();
      }
      tree_capture->send(memory(offset, size), false);
   }
   void send_context_free_inline(uint32_t offset, uint32_t size) {
      context_aware(); // the SEND host is context-aware, unlike its child
      action_only();
      if (!tree_capture && sync_capture) throw isolated_mixed_mode_required();
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
   const isolated_transaction_environment& environment() const {
      check();
      if (!input.environment) throw isolated_transaction_environment_missing();
      return *input.environment;
   }
   uint64_t current_time() { context_aware(); return environment().current_time(); }
   uint64_t publication_time() { context_aware(); return environment().publication_time(); }
   int32_t transaction_size() { return environment().transaction_size(); }
   int32_t expiration() { return std::bit_cast<int32_t>(environment().expiration()); }
   int32_t tapos_block_num() { return static_cast<int32_t>(environment().tapos_block_num()); }
   int32_t tapos_block_prefix() { return std::bit_cast<int32_t>(environment().tapos_block_prefix()); }
   uint32_t get_block_num() { return environment().block_num(); }
   int32_t read_transaction(uint32_t output, uint32_t size) {
      auto destination = memory(output, size);
      const auto& captured = environment();
      deadline.check();
      const auto result = captured.read_transaction(destination);
      deadline.check();
      return result;
   }
   int32_t get_action(uint32_t type, uint32_t index, uint32_t output, uint32_t size) {
      action_only();
      auto destination = memory(output, size);
      const auto& captured = environment();
      deadline.check();
      const auto result = captured.get_action(type, index, destination);
      deadline.check();
      return result;
   }
   [[noreturn]] int32_t get_active_producers(uint32_t, uint32_t) { throw primary_state_unsupported(); }
   void set_action_return_value(uint32_t offset, uint32_t size) {
      action_only();
      context_aware();
      if (size > input.max_return_bytes) throw primary_state_unsupported();
      const auto source = memory(offset, size);
      return_value.assign(source.begin(), source.end());
   }
   [[noreturn]] void missing_sync_capture() const {
      if (tree_capture && !tree_capture->context_free) {
         if (tree_capture->sender == name{} && input.receiver == input.account)
            throw isolated_sync_mode_required();
         throw isolated_mixed_mode_required();
      }
      throw primary_state_unsupported();
   }
   int64_t call(uint64_t receiver, uint64_t flags, uint32_t offset, uint32_t size) {
      context_aware();
      if (!sync_capture) missing_sync_capture();
      const auto data = memory(offset, size); // validate before entering original call semantics
      auto& last_return = call_return_buffer();
      last_return.clear();
      auto [status, bytes] = [&] {
         const auto begin = std::chrono::steady_clock::now();
         const auto cpu_begin = thread_cpu_clock::now();
         // Failed descendants also consumed time. Close this interval before
         // parent return-buffer work, on BOTH return and exception unwind.
         auto account_child = fc::make_scoped_exit([&] {
            sync_capture->children_time += std::chrono::steady_clock::now() - begin;
            // Never throw from an unwinding cleanup. Invalidate this private
            // attempt if its CPU clock fails; canonical fallback remains intact.
            try {
               const auto delta = thread_cpu_clock::now() - cpu_begin;
               if (delta < thread_cpu_clock::duration::zero() ||
                   delta > thread_cpu_clock::duration::max() - sync_capture->children_cpu)
                  sync_capture->children_cpu_valid = false;
               else sync_capture->children_cpu += delta;
            } catch (...) { sync_capture->children_cpu_valid = false; }
         });
         return sync_capture->call(name{receiver}, flags, data);
      }();
      last_return = std::move(bytes);
      return status < 0 ? status : static_cast<int64_t>(last_return.size());
   }
   uint32_t get_call_return_value(uint32_t offset, uint32_t size) {
      context_aware();
      if (!sync_capture) missing_sync_capture();
      auto output = memory(offset, size);
      const auto& last_return = call_return_buffer();
      const auto count = std::min(output.size(), last_return.size());
      if (count) std::copy_n(last_return.begin(), count, output.begin());
      return last_return.size();
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

   // Reuse the ORIGINAL legacy alignment/writeback semantics, not an always-
   // copying wrapper. Scalar offset zero is invalid; idx256 is a span and may
   // start at zero. No proxy or reference to WASM memory escapes this call.
   template<class T> auto scalar_pointer(uint32_t offset) {
      if (!offset) throw primary_state_unsupported();
      return eosio::vm::argument_proxy<T*, alignof(T)>{memory(offset, sizeof(T)).data()};
   }
   template<secondary_key_kind Kind, bool Constant = false>
   auto secondary_pointer(uint32_t offset, uint32_t count) {
      using Wire = secondary_wire_type<Kind>;
      using T = std::conditional_t<Constant, const Wire, Wire>;
      if constexpr (Kind == secondary_key_kind::uint256) {
         // Original count is uint128 ELEMENTS, not bytes. Reject before any
         // private effect; invalid inputs are still decided by original WASM.
         if (count != 2) throw primary_state_unsupported();
         return eosio::vm::argument_proxy<eosio::vm::span<T>, alignof(T)>{
            memory(offset, 2 * sizeof(T)).data(), 2};
      } else return scalar_pointer<T>(offset);
   }
   template<secondary_key_kind Kind, class Proxy>
   static secondary_key read_secondary_key(const Proxy& p) {
      if constexpr (Kind == secondary_key_kind::uint256) return std::array<uint128_t, 2>{p[0], p[1]};
      else if constexpr (Kind == secondary_key_kind::float64) return secondary_float64{p->v};
      else if constexpr (Kind == secondary_key_kind::float128) {
         secondary_float128 out; fc::float128_to_uint128(*p.get(), out.bits); return out;
      } else return *p.get();
   }
   template<secondary_key_kind Kind, class Proxy>
   static void write_secondary_key(Proxy& p, const secondary_key& key) {
      if constexpr (Kind == secondary_key_kind::uint256) {
         const auto& value = std::get<std::array<uint128_t, 2>>(key); p[0] = value[0]; p[1] = value[1];
      } else if constexpr (Kind == secondary_key_kind::float64) p->v = std::get<secondary_float64>(key).bits;
      else if constexpr (Kind == secondary_key_kind::float128)
         fc::uint128_to_float128(std::get<secondary_float128>(key).bits, *p.get());
      else *p.get() = std::get<secondary_wire_type<Kind>>(key);
   }
   static void secondary_value_input(const secondary_key& key) {
      // Original registration applies is_nan_check BEFORE even a missing-table
      // lookup. find_primary has an OUTPUT key and intentionally skips this.
      if (secondary_key_is_nan(key)) throw primary_state_unsupported();
   }
   template<class Event> void record_secondary(Event event) {
      if (cursor_events || sync_capture) record(isolated_secondary_cursor_event{std::move(event)});
   }
   template<secondary_key_kind Kind>
   int32_t secondary_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary,
                           uint32_t offset, uint32_t count = 1) {
      writable(); auto p = secondary_pointer<Kind, true>(offset, count);
      const auto key = read_secondary_key<Kind>(p); secondary_value_input(key);
      const auto result = secondary.get(Kind).store({input.receiver, name(scope), name(table)}, primary, name(payer), key);
      record_secondary(isolated_secondary_write{Kind, state.write_count() - 1, result}); return result;
   }
   template<secondary_key_kind Kind>
   void secondary_update(int32_t iterator, uint64_t payer, uint32_t offset, uint32_t count = 1) {
      writable(); auto p = secondary_pointer<Kind, true>(offset, count);
      const auto key = read_secondary_key<Kind>(p); secondary_value_input(key);
      secondary.get(Kind).update(iterator, name(payer), key);
      record_secondary(isolated_secondary_write{Kind, state.write_count() - 1, iterator});
   }
   template<secondary_key_kind Kind> void secondary_remove(int32_t iterator) {
      writable(); secondary.get(Kind).remove(iterator);
      record_secondary(isolated_secondary_write{Kind, state.write_count() - 1, iterator});
   }
   template<secondary_key_kind Kind>
   int32_t secondary_find_primary(uint64_t code, uint64_t scope, uint64_t table,
                                 uint32_t offset, uint64_t primary, uint32_t count = 1) {
      context_aware(); auto p = secondary_pointer<Kind>(offset, count);
      auto key = read_secondary_key<Kind>(p); const auto before = key;
      const table_key t{name(code), name(scope), name(table)};
      const auto result = secondary.get(Kind).find_primary(t, key, primary);
      if (result >= 0) write_secondary_key<Kind>(p, key);
      record_secondary(isolated_secondary_find_primary{t, primary, before, key, result}); return result;
   }
   template<secondary_key_kind Kind>
   int32_t secondary_find_secondary(uint64_t code, uint64_t scope, uint64_t table,
                                   uint32_t offset, uint32_t output, uint32_t count = 1) {
      context_aware(); auto s = secondary_pointer<Kind, true>(offset, count);
      auto p = scalar_pointer<uint64_t>(output);
      const auto key = read_secondary_key<Kind>(s); secondary_value_input(key);
      uint64_t primary = *p.get(); const auto before = primary;
      const table_key t{name(code), name(scope), name(table)};
      const auto result = secondary.get(Kind).find_secondary(t, key, primary);
      if (result >= 0) *p.get() = primary;
      record_secondary(isolated_secondary_find_secondary{t, key, before, primary, result}); return result;
   }
   template<secondary_key_kind Kind>
   int32_t secondary_bound(uint64_t code, uint64_t scope, uint64_t table,
                          uint32_t offset, uint32_t output, bool upper, uint32_t count = 1) {
      context_aware(); auto s = secondary_pointer<Kind>(offset, count);
      auto p = scalar_pointer<uint64_t>(output);
      auto key = read_secondary_key<Kind>(s); secondary_value_input(key);
      const auto key_before = key; uint64_t primary = *p.get(); const auto primary_before = primary;
      const table_key t{name(code), name(scope), name(table)};
      const auto result = upper ? secondary.get(Kind).upper_bound(t, key, primary) :
                                  secondary.get(Kind).lower_bound(t, key, primary);
      // Match generic_index body order, including aligned aliases. Record the
      // independent semantic outputs, NEVER reread aliased memory as a row key.
      if (result >= 0) { *p.get() = primary; write_secondary_key<Kind>(s, key); }
      record_secondary(isolated_secondary_bound{t, upper, key_before, key, primary_before, primary, result});
      // database.cpp explicitly flushes secondary then primary temporaries,
      // even for a miss. Aligned proxies reference memory and do not copy back.
      (void)decltype(s)(std::move(s)); (void)decltype(p)(std::move(p));
      return result;
   }
   template<secondary_key_kind Kind>
   int32_t secondary_end(uint64_t code, uint64_t scope, uint64_t table) {
      context_aware(); const table_key t{name(code), name(scope), name(table)};
      const auto result = secondary.get(Kind).end(t);
      record_secondary(isolated_secondary_end{t, Kind, result}); return result;
   }
   template<secondary_key_kind Kind>
   int32_t secondary_adjacent(int32_t iterator, uint32_t output, bool previous) {
      context_aware(); auto p = scalar_pointer<uint64_t>(output);
      uint64_t primary = *p.get(); const auto before = primary;
      const auto result = previous ? secondary.get(Kind).previous(iterator, primary) : secondary.get(Kind).next(iterator, primary);
      if (result >= 0) *p.get() = primary;
      record_secondary(isolated_secondary_adjacent{Kind, previous, iterator, result, before, primary}); return result;
   }
   int32_t db_idx64_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset) { return secondary_store<secondary_key_kind::uint64>(scope, table, payer, primary, offset); }
   void db_idx64_update(int32_t iterator, uint64_t payer, uint32_t offset) { secondary_update<secondary_key_kind::uint64>(iterator, payer, offset); }
   void db_idx64_remove(int32_t iterator) { secondary_remove<secondary_key_kind::uint64>(iterator); }
   int32_t db_idx64_find_primary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint64_t primary) { return secondary_find_primary<secondary_key_kind::uint64>(code, scope, table, offset, primary); }
   int32_t db_idx64_find_secondary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_find_secondary<secondary_key_kind::uint64>(code, scope, table, offset, output); }
   int32_t db_idx64_lowerbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::uint64>(code, scope, table, offset, output, false); }
   int32_t db_idx64_upperbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::uint64>(code, scope, table, offset, output, true); }
   int32_t db_idx64_end(uint64_t code, uint64_t scope, uint64_t table) { return secondary_end<secondary_key_kind::uint64>(code, scope, table); }
   int32_t db_idx64_next(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::uint64>(iterator, output, false); }
   int32_t db_idx64_previous(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::uint64>(iterator, output, true); }
   int32_t db_idx128_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset) { return secondary_store<secondary_key_kind::uint128>(scope, table, payer, primary, offset); }
   void db_idx128_update(int32_t iterator, uint64_t payer, uint32_t offset) { secondary_update<secondary_key_kind::uint128>(iterator, payer, offset); }
   void db_idx128_remove(int32_t iterator) { secondary_remove<secondary_key_kind::uint128>(iterator); }
   int32_t db_idx128_find_primary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint64_t primary) { return secondary_find_primary<secondary_key_kind::uint128>(code, scope, table, offset, primary); }
   int32_t db_idx128_find_secondary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_find_secondary<secondary_key_kind::uint128>(code, scope, table, offset, output); }
   int32_t db_idx128_lowerbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::uint128>(code, scope, table, offset, output, false); }
   int32_t db_idx128_upperbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::uint128>(code, scope, table, offset, output, true); }
   int32_t db_idx128_end(uint64_t code, uint64_t scope, uint64_t table) { return secondary_end<secondary_key_kind::uint128>(code, scope, table); }
   int32_t db_idx128_next(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::uint128>(iterator, output, false); }
   int32_t db_idx128_previous(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::uint128>(iterator, output, true); }
   int32_t db_idx256_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset, uint32_t count) { return secondary_store<secondary_key_kind::uint256>(scope, table, payer, primary, offset, count); }
   void db_idx256_update(int32_t iterator, uint64_t payer, uint32_t offset, uint32_t count) { secondary_update<secondary_key_kind::uint256>(iterator, payer, offset, count); }
   void db_idx256_remove(int32_t iterator) { secondary_remove<secondary_key_kind::uint256>(iterator); }
   int32_t db_idx256_find_primary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t count, uint64_t primary) { return secondary_find_primary<secondary_key_kind::uint256>(code, scope, table, offset, primary, count); }
   int32_t db_idx256_find_secondary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t count, uint32_t output) { return secondary_find_secondary<secondary_key_kind::uint256>(code, scope, table, offset, output, count); }
   int32_t db_idx256_lowerbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t count, uint32_t output) { return secondary_bound<secondary_key_kind::uint256>(code, scope, table, offset, output, false, count); }
   int32_t db_idx256_upperbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t count, uint32_t output) { return secondary_bound<secondary_key_kind::uint256>(code, scope, table, offset, output, true, count); }
   int32_t db_idx256_end(uint64_t code, uint64_t scope, uint64_t table) { return secondary_end<secondary_key_kind::uint256>(code, scope, table); }
   int32_t db_idx256_next(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::uint256>(iterator, output, false); }
   int32_t db_idx256_previous(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::uint256>(iterator, output, true); }
   int32_t db_idx_double_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset) { return secondary_store<secondary_key_kind::float64>(scope, table, payer, primary, offset); }
   void db_idx_double_update(int32_t iterator, uint64_t payer, uint32_t offset) { secondary_update<secondary_key_kind::float64>(iterator, payer, offset); }
   void db_idx_double_remove(int32_t iterator) { secondary_remove<secondary_key_kind::float64>(iterator); }
   int32_t db_idx_double_find_primary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint64_t primary) { return secondary_find_primary<secondary_key_kind::float64>(code, scope, table, offset, primary); }
   int32_t db_idx_double_find_secondary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_find_secondary<secondary_key_kind::float64>(code, scope, table, offset, output); }
   int32_t db_idx_double_lowerbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::float64>(code, scope, table, offset, output, false); }
   int32_t db_idx_double_upperbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::float64>(code, scope, table, offset, output, true); }
   int32_t db_idx_double_end(uint64_t code, uint64_t scope, uint64_t table) { return secondary_end<secondary_key_kind::float64>(code, scope, table); }
   int32_t db_idx_double_next(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::float64>(iterator, output, false); }
   int32_t db_idx_double_previous(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::float64>(iterator, output, true); }
   int32_t db_idx_long_double_store(uint64_t scope, uint64_t table, uint64_t payer, uint64_t primary, uint32_t offset) { return secondary_store<secondary_key_kind::float128>(scope, table, payer, primary, offset); }
   void db_idx_long_double_update(int32_t iterator, uint64_t payer, uint32_t offset) { secondary_update<secondary_key_kind::float128>(iterator, payer, offset); }
   void db_idx_long_double_remove(int32_t iterator) { secondary_remove<secondary_key_kind::float128>(iterator); }
   int32_t db_idx_long_double_find_primary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint64_t primary) { return secondary_find_primary<secondary_key_kind::float128>(code, scope, table, offset, primary); }
   int32_t db_idx_long_double_find_secondary(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_find_secondary<secondary_key_kind::float128>(code, scope, table, offset, output); }
   int32_t db_idx_long_double_lowerbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::float128>(code, scope, table, offset, output, false); }
   int32_t db_idx_long_double_upperbound(uint64_t code, uint64_t scope, uint64_t table, uint32_t offset, uint32_t output) { return secondary_bound<secondary_key_kind::float128>(code, scope, table, offset, output, true); }
   int32_t db_idx_long_double_end(uint64_t code, uint64_t scope, uint64_t table) { return secondary_end<secondary_key_kind::float128>(code, scope, table); }
   int32_t db_idx_long_double_next(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::float128>(iterator, output, false); }
   int32_t db_idx_long_double_previous(int32_t iterator, uint32_t output) { return secondary_adjacent<secondary_key_kind::float128>(iterator, output, true); }
};

// Only import resolution uses the upstream registry's mutable map accessor.
// Parsing, globals and allocator state belong to EACH backend, so they do not
// need to wait for another worker's independent module construction. Keep the
// original resolver and all its type/error checks, under a narrow shared lock;
// registration still completes via call_once before any backend is constructed.
struct functions : eosio::vm::registered_host_functions<isolated_host> {
   static void resolve(eosio::vm::module& module) {
      static std::mutex resolution_mutex;
      std::lock_guard lock(resolution_mutex);
      eosio::vm::registered_host_functions<isolated_host>::resolve(module);
   }
};
using backend = eosio::vm::backend<functions, eosio::vm::interpreter, isolated_vm_options>;
#ifdef __x86_64__
using jit_backend = eosio::vm::backend<functions, parallel_jit, isolated_vm_options>;
#endif

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
      ISOLATED_HOST(sha1);
      ISOLATED_HOST(sha256);
      ISOLATED_HOST(sha512);
      ISOLATED_HOST(ripemd160);
      ISOLATED_HOST(assert_sha1);
      ISOLATED_HOST(assert_sha256);
      ISOLATED_HOST(assert_sha512);
      ISOLATED_HOST(assert_ripemd160);
      ISOLATED_HOST(__ashlti3);
      ISOLATED_HOST(__ashrti3);
      ISOLATED_HOST(__lshlti3);
      ISOLATED_HOST(__lshrti3);
      ISOLATED_HOST(__divti3);
      ISOLATED_HOST(__udivti3);
      ISOLATED_HOST(__multi3);
      ISOLATED_HOST(__modti3);
      ISOLATED_HOST(__umodti3);
      ISOLATED_HOST(__addtf3);
      ISOLATED_HOST(__subtf3);
      ISOLATED_HOST(__multf3);
      ISOLATED_HOST(__divtf3);
      ISOLATED_HOST(__negtf2);
      ISOLATED_HOST(__extendsftf2);
      ISOLATED_HOST(__extenddftf2);
      ISOLATED_HOST(__trunctfdf2);
      ISOLATED_HOST(__trunctfsf2);
      ISOLATED_HOST(__fixtfsi);
      ISOLATED_HOST(__fixtfdi);
      ISOLATED_HOST(__fixtfti);
      ISOLATED_HOST(__fixunstfsi);
      ISOLATED_HOST(__fixunstfdi);
      ISOLATED_HOST(__fixunstfti);
      ISOLATED_HOST(__fixsfti);
      ISOLATED_HOST(__fixdfti);
      ISOLATED_HOST(__fixunssfti);
      ISOLATED_HOST(__fixunsdfti);
      ISOLATED_HOST(__floatsidf);
      ISOLATED_HOST(__floatsitf);
      ISOLATED_HOST(__floatditf);
      ISOLATED_HOST(__floatunsitf);
      ISOLATED_HOST(__floatunditf);
      ISOLATED_HOST(__floattidf);
      ISOLATED_HOST(__floatuntidf);
      ISOLATED_HOST(__eqtf2);
      ISOLATED_HOST(__netf2);
      ISOLATED_HOST(__getf2);
      ISOLATED_HOST(__gttf2);
      ISOLATED_HOST(__letf2);
      ISOLATED_HOST(__lttf2);
      ISOLATED_HOST(__cmptf2);
      ISOLATED_HOST(__unordtf2);
      ISOLATED_HOST(eosio_assert);
      ISOLATED_HOST(eosio_assert_message);
      ISOLATED_HOST(eosio_assert_code);
      ISOLATED_HOST(eosio_exit);
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
      ISOLATED_TREE_HOST(prints);
      ISOLATED_TREE_HOST(prints_l);
      ISOLATED_TREE_HOST(printi);
      ISOLATED_TREE_HOST(printui);
      ISOLATED_TREE_HOST(printi128);
      ISOLATED_TREE_HOST(printui128);
      ISOLATED_TREE_HOST(printsf);
      ISOLATED_TREE_HOST(printdf);
      ISOLATED_TREE_HOST(printqf);
      ISOLATED_TREE_HOST(printn);
      ISOLATED_TREE_HOST(printhex);
      ISOLATED_TREE_HOST(send_inline); // private tree only; existing adopters still fallback
      ISOLATED_HOST(db_store_i64);
      ISOLATED_HOST(db_remove_i64);
      ISOLATED_FALLBACK(set_blockchain_parameters_packed);
      ISOLATED_FALLBACK(get_blockchain_parameters_packed);
      ISOLATED_FALLBACK(set_proposed_producers);
      ISOLATED_HOST(current_time);
      ISOLATED_HOST(publication_time);
      ISOLATED_HOST(read_transaction);
      ISOLATED_HOST(transaction_size);
      ISOLATED_HOST(expiration);
      ISOLATED_HOST(tapos_block_num);
      ISOLATED_HOST(tapos_block_prefix);
      ISOLATED_HOST(get_action);
      ISOLATED_HOST(get_block_num);
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
      // Private execution only: all canonical adopters remain primary-only
      // until original host replay and secondary conflict checks are accepted.
#define ISOLATED_SECONDARY_HOST(method) functions::add<&isolated_host::method>("env", #method)
      ISOLATED_SECONDARY_HOST(db_idx64_store);
      ISOLATED_SECONDARY_HOST(db_idx64_update);
      ISOLATED_SECONDARY_HOST(db_idx64_remove);
      ISOLATED_SECONDARY_HOST(db_idx64_find_primary);
      ISOLATED_SECONDARY_HOST(db_idx64_find_secondary);
      ISOLATED_SECONDARY_HOST(db_idx64_lowerbound);
      ISOLATED_SECONDARY_HOST(db_idx64_upperbound);
      ISOLATED_SECONDARY_HOST(db_idx64_end);
      ISOLATED_SECONDARY_HOST(db_idx64_next);
      ISOLATED_SECONDARY_HOST(db_idx64_previous);
      ISOLATED_SECONDARY_HOST(db_idx128_store);
      ISOLATED_SECONDARY_HOST(db_idx128_update);
      ISOLATED_SECONDARY_HOST(db_idx128_remove);
      ISOLATED_SECONDARY_HOST(db_idx128_find_primary);
      ISOLATED_SECONDARY_HOST(db_idx128_find_secondary);
      ISOLATED_SECONDARY_HOST(db_idx128_lowerbound);
      ISOLATED_SECONDARY_HOST(db_idx128_upperbound);
      ISOLATED_SECONDARY_HOST(db_idx128_end);
      ISOLATED_SECONDARY_HOST(db_idx128_next);
      ISOLATED_SECONDARY_HOST(db_idx128_previous);
      ISOLATED_SECONDARY_HOST(db_idx256_store);
      ISOLATED_SECONDARY_HOST(db_idx256_update);
      ISOLATED_SECONDARY_HOST(db_idx256_remove);
      ISOLATED_SECONDARY_HOST(db_idx256_find_primary);
      ISOLATED_SECONDARY_HOST(db_idx256_find_secondary);
      ISOLATED_SECONDARY_HOST(db_idx256_lowerbound);
      ISOLATED_SECONDARY_HOST(db_idx256_upperbound);
      ISOLATED_SECONDARY_HOST(db_idx256_end);
      ISOLATED_SECONDARY_HOST(db_idx256_next);
      ISOLATED_SECONDARY_HOST(db_idx256_previous);
      ISOLATED_SECONDARY_HOST(db_idx_double_store);
      ISOLATED_SECONDARY_HOST(db_idx_double_update);
      ISOLATED_SECONDARY_HOST(db_idx_double_remove);
      ISOLATED_SECONDARY_HOST(db_idx_double_find_primary);
      ISOLATED_SECONDARY_HOST(db_idx_double_find_secondary);
      ISOLATED_SECONDARY_HOST(db_idx_double_lowerbound);
      ISOLATED_SECONDARY_HOST(db_idx_double_upperbound);
      ISOLATED_SECONDARY_HOST(db_idx_double_end);
      ISOLATED_SECONDARY_HOST(db_idx_double_next);
      ISOLATED_SECONDARY_HOST(db_idx_double_previous);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_store);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_update);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_remove);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_find_primary);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_find_secondary);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_lowerbound);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_upperbound);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_end);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_next);
      ISOLATED_SECONDARY_HOST(db_idx_long_double_previous);
#undef ISOLATED_SECONDARY_HOST
#undef ISOLATED_FALLBACK
#undef ISOLATED_TREE_HOST
#undef ISOLATED_HOST
   });
}
} // namespace

isolated_execution_runtime::isolated_execution_runtime(isolated_vm_engine engine, size_t maximum_frames)
   : engine_(engine) {
   if (!isolated_vm_engine_supported(engine) || !maximum_frames ||
       maximum_frames > parallel_runtime_limits::max_workers * (isolated_sync_executor::max_depth + 1))
      throw primary_state_unsupported();
   if (engine == isolated_vm_engine::jit)
      deadlines_ = std::make_unique<parallel_deadline_service>(maximum_frames);
}
isolated_execution_runtime::~isolated_execution_runtime() = default;
parallel_deadline_service& isolated_execution_runtime::deadlines() const {
   if (!deadlines_) throw primary_state_invalid_attempt();
   return *deadlines_;
}

struct isolated_wasm_module::impl {
   const std::thread::id owner = std::this_thread::get_id();
   isolated_wasm_limits limits;
   isolated_vm_options options;
   std::vector<uint8_t> owned_code;
   fc::sha256 code_hash;
   eosio::vm::wasm_allocator allocator;
   const std::shared_ptr<const isolated_execution_runtime> runtime;
   std::unique_ptr<backend> machine;
#ifdef __x86_64__
   std::unique_ptr<jit_backend> jit_machine;
#endif

   template<typename Function> decltype(auto) visit(Function&& function) {
#ifdef __x86_64__
      if (jit_machine) return function(*jit_machine);
#endif
      return function(*machine);
   }

   impl(std::span<const char> code, isolated_wasm_limits bounds,
        std::shared_ptr<const isolated_execution_runtime> execution_runtime)
      : limits(bounds), runtime(std::move(execution_runtime)) {
      if (code.empty() || code.size() > limits.max_code_bytes || limits.max_code_bytes > isolated_wasm_limits::max_module_code_bytes ||
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
      // The private resolver above serializes ONLY the shared registry lookup.
      // Both parsing passes and construction use this module's owned storage.
      // No start function runs here: the backend has no linear-memory allocator
      // until the original bounded execution/reset path below supplies it.
#ifdef __x86_64__
      if (runtime && runtime->engine() == isolated_vm_engine::jit) {
         // JIT uses single parsing and owns relocated metadata. Never read
         // memories/types from the released parser arena, or execute start here.
         jit_machine = std::make_unique<jit_backend>(pointer, code.size(), nullptr, options);
         if (jit_machine->get_module().jit_mod->memories.size() > 1)
            throw primary_state_unsupported();
         // Account executable bytes separately from the 128 KiB WASM input.
         // Each cache holds at most nine modules; no shared executable mappings.
         if (jit_machine->get_module().allocator.get_code_span().size() > 16 * 1024 * 1024)
            throw primary_state_budget_exceeded();
         jit_machine->set_wasm_allocator(&allocator);
      } else
#endif
      {
         machine = std::make_unique<backend>(pointer, code.size(), nullptr, options, false);
         // Initial memory is parser-bounded; every growth is guarded at runtime.
         if (machine->get_module().memories.size() > 1) throw primary_state_unsupported();
         machine->set_wasm_allocator(&allocator);
      }
   }
};

isolated_wasm_module::isolated_wasm_module(std::span<const char> code, isolated_wasm_limits limits,
      std::shared_ptr<const isolated_execution_runtime> runtime)
   : impl_(std::make_unique<impl>(code, limits, std::move(runtime))) {}
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
   if (input.environment) {
      constexpr char extension[] = "pureeos/isolated-action-environment/v1";
      encoder.write(extension, sizeof(extension));
      fc::raw::pack(encoder, input.environment->fingerprint());
   }
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
      isolated_tree_capture* tree_capture, isolated_sync_capture* sync_capture,
      secondary_cursor_group* group_secondary) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   const auto began = std::chrono::steady_clock::now();
   const auto cpu_began = thread_cpu_clock::now();
   const bool sample_sync = sync_capture && performance::metrics().enabled.load(std::memory_order_relaxed);
   auto sync_metric = fc::make_scoped_exit([&] {
      if (sample_sync) {
         const auto exclusive = std::chrono::steady_clock::now() - began - sync_capture->children_time;
         const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(exclusive).count();
         performance::metrics().record(performance::stage::parallel_execute,static_cast<uint64_t>(std::max<int64_t>(0,ns)));
      }
   }); // diagnostic only, including failures; NEVER a billing/deadline input
   external.check(stop);
   if (bool(group_cursor) != bool(group_recipients) || bool(group_cursor) != bool(group_secondary) ||
       (sync_capture && sync_capture->group_call_return &&
          (sync_capture->is_sync || !tree_capture || !group_cursor)) ||
       (sync_capture && (tree_capture || group_cursor) &&
          !(tree_capture && group_cursor && !sync_capture->is_sync)) ||
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
   secondary_cursor_group local_secondary(state, input.receiver);
   auto& cursor = group_cursor ? *group_cursor : local_cursor;
   auto& secondary = group_secondary ? *group_secondary : local_secondary;
   cursor.set_receiver_for_notification(input.receiver);
   secondary.set_receiver_for_notification(input.receiver);
   const attempt_deadline deadline(impl_->limits.max_attempt_time, stop, external);
   return impl_->visit([&](auto& machine) -> isolated_action_effects {
      isolated_host host{cursor, secondary, state, input, stop, external, deadline, [&]() -> std::span<char> {
         auto& context = machine.get_context();
         const int32_t pages = context.current_linear_memory();
         // EOS VM uses -1 for modules with NO memory. Never convert that sentinel
         // into a huge unsigned span. Zero-page memory calls conservatively fall
         // back too; non-memory hosts remain usable without linear memory.
         if (pages <= 0) throw primary_state_unsupported();
         return {context.linear_memory(), static_cast<size_t>(pages) * 65536u};
      }, {}, {}, group_recipients, cursor_events, max_cursor_events, tree_capture, sync_capture, {}};
      auto& context = machine.get_context();
      constexpr bool interpreted = std::is_same_v<std::remove_reference_t<decltype(machine)>, backend>;
      const auto execute = [&](auto& visitor) {
         // Same reset/start sequence as backend::initialize, with a bounded visitor
         // for BOTH start and apply. No shared VM module/context is introduced.
         context.set_max_call_depth(impl_->options.max_call_depth);
         context.set_max_pages(impl_->options.max_pages);
         context.reset();
         deadline.check();
         if (sync_capture && sync_capture->is_sync) {
            const auto index = machine.get_module().get_exported_function("sync_call");
            if (index == std::numeric_limits<uint32_t>::max()) throw primary_state_unsupported();
            const auto& type = [&]() -> const auto& {
               if constexpr (interpreted) return machine.get_module().get_function_type(index);
               else return machine.get_module().jit_mod->get_function_type(index);
            }();
            if (!(type == eosio::vm::host_function{{eosio::vm::i64, eosio::vm::i64, eosio::vm::i32}, {eosio::vm::i64}}))
               throw primary_state_unsupported(); // captured metadata/code mismatch, never invent -2
         }
         try {
            context.execute_start(&host, visitor);
            if (sync_capture && sync_capture->is_sync) {
               auto result = context.execute(&host, visitor, "sync_call", sync_capture->sender.to_uint64_t(),
                                             input.receiver.to_uint64_t(), static_cast<uint32_t>(input.data.size()));
               if (!result) throw primary_state_unsupported();
               sync_capture->status = result->to_i64();
            } else {
               context.execute(&host, visitor, "apply", input.receiver.to_uint64_t(), input.account.to_uint64_t(), input.action.to_uint64_t());
            }
         } catch (const isolated_frame_exit&) {
            // Original sync exit is successful and retains any set return bytes.
            // An exit in start skips the entry too. Keep the outer deadline check
            // and the normal effects/billing path; do not catch other exceptions.
            if (sync_capture && sync_capture->is_sync) sync_capture->status = 0;
         }
      };
      if constexpr (interpreted) {
         interruptible_visitor<std::remove_reference_t<decltype(context)>> visitor{{context}, deadline, impl_->limits.max_pages};
         execute(visitor);
      }
#ifdef __x86_64__
      else {
         struct growth_guard { const attempt_deadline& deadline; uint32_t maximum; } growth{deadline, impl_->limits.max_pages};
         parallel_jit_frame frame;
         frame.state = &growth;
         frame.before_memory_grow = [](void* opaque, int32_t current, int32_t requested) {
            const auto& guard = *static_cast<const growth_guard*>(opaque);
            guard.deadline.check();
            if (current < 0 || requested < 0 || uint32_t(current) > guard.maximum ||
                uint32_t(requested) > guard.maximum - uint32_t(current)) throw primary_state_unsupported();
         };
         // Bind outside the VM signal frame: exception bridges must still restore
         // the caller's floating control state. The timer unregisters BEFORE the
         // binding/frame/guard go out of scope, including nested-call failures.
         auto binding = context.bind_frame(frame);
         auto timer = impl_->runtime->deadlines().watch(deadline.effective_end(),
            {stop, external.generation, external.observed}, [&frame]() noexcept {
               frame.interrupt();
            });
         eosio::vm::jit_visitor visitor{0};
         try { execute(visitor); }
         catch (const eosio::vm::timeout_exception&) {
            deadline.check(); // retain external cancellation/deadline diagnostics
            throw;
         }
      }
#endif
      deadline.check();
      const auto cpu_ended = thread_cpu_clock::now();
      const auto ended = std::chrono::steady_clock::now();
      const auto children_time = sync_capture ? sync_capture->children_time : std::chrono::steady_clock::duration{};
      const auto children_cpu = sync_capture ? sync_capture->children_cpu : thread_cpu_clock::duration{};
      if (children_time > ended - began) throw primary_state_invalid_attempt();
      if (cpu_ended < cpu_began || children_cpu > cpu_ended - cpu_began ||
          (sync_capture && !sync_capture->children_cpu_valid)) throw primary_state_invalid_attempt();
      return {std::move(host.return_value), fingerprint, std::move(host.recipients), writes_begin, state.write_count(),
              std::chrono::duration_cast<std::chrono::microseconds>(cpu_ended - cpu_began - children_cpu), began, ended};
   });
}

// One exclusive bounded cache for all worker modes. Standalone legacy APIs
// retain their four-entry LRU or nine fixed-depth slots. Protocol observations
// are checked anew by the caller, never inferred from a cached backend/export.
namespace {
struct leased_modules {
   static constexpr size_t capacity = isolated_sync_executor::max_depth + 1;
   struct entry {
      std::vector<char> code;
      fc::sha256 code_hash;
      isolated_wasm_limits limits;
      std::unique_ptr<isolated_wasm_module> module;
      // Only deterministic construction-time link failures are remembered.
      // The private import registry and runtime engine are immutable for this
      // cache's lifetime. Code AND every limit still participate in the key.
      bool link_failed = false;
      bool leased = false;
   };
   const std::thread::id owner = std::this_thread::get_id();
   const size_t limit;
   const bool fixed_depth;
   const std::shared_ptr<const isolated_execution_runtime> runtime;
   std::array<entry, capacity> modules;
   std::array<bool, capacity> active_depth{};
   // A bounded permutation, most recently acquired first; no growing clock.
   std::array<size_t, capacity> recency{};

   leased_modules(size_t bound, bool depth_slots, std::shared_ptr<const isolated_execution_runtime> execution_runtime)
      : limit(bound), fixed_depth(depth_slots), runtime(std::move(execution_runtime)) {
      if (!limit || limit > capacity) throw primary_state_budget_exceeded();
      for (size_t i = 0; i < capacity; ++i) recency[i] = i;
   }
   void check_owner() const {
      if (std::this_thread::get_id() != owner) throw state_view_wrong_thread();
   }
   void touch(size_t selected) noexcept {
      size_t position = 0;
      while (recency[position] != selected) ++position;
      for (; position > 0; --position) recency[position] = recency[position - 1];
      recency[0] = selected;
   }
   class lease {
      entry& slot;
      bool& depth_active;
      lease(entry& value, bool& active) noexcept : slot(value), depth_active(active) {
         slot.leased = true;
         depth_active = true;
      }
      friend struct leased_modules;
   public:
      lease(const lease&) = delete;
      lease& operator=(const lease&) = delete;
      ~lease() { slot.leased = false; depth_active = false; }
      isolated_wasm_module& get() const { return *slot.module; }
   };

   lease acquire(size_t depth, const isolated_receiver_program& program) {
      return acquire(depth, program.code.view(), program.limits, &program.code.content_hash());
   }
   lease acquire(size_t depth, const std::vector<char>& code, const isolated_wasm_limits& limits, const fc::sha256* identity=nullptr) {
      check_owner(); // before any slot access, including a cache miss
      if (depth >= limit) throw primary_state_budget_exceeded();
      if (active_depth[depth]) throw primary_state_invalid_attempt();
      const auto same_code=[&](const entry& slot) { return identity ? slot.code_hash==*identity : slot.code==code; };
      size_t selected = fixed_depth ? depth : capacity;
      if (!fixed_depth) {
         for (const auto i : recency) {
            if (i >= limit) continue;
            const auto& slot = modules[i];
            if (!slot.leased && (slot.module || slot.link_failed) && same_code(slot) && slot.limits == limits) {
               selected = i;
               break;
            }
         }
      }
      if (selected == capacity) {
         for (size_t i = 0; i < limit; ++i) {
            if (!modules[i].leased && !modules[i].module && !modules[i].link_failed) { selected = i; break; }
         }
      }
      if (selected == capacity) {
         for (size_t position = capacity; position > 0; --position) {
            const auto i = recency[position - 1];
            if (i < limit && !modules[i].leased) { selected = i; break; }
         }
      }
      if (selected == capacity) throw primary_state_budget_exceeded();
      auto& slot = modules[selected];
      if (slot.leased) throw primary_state_invalid_attempt();
      if (slot.link_failed && same_code(slot) && slot.limits == limits) {
         touch(selected);
         throw eosio::vm::wasm_link_exception{"cached isolated module link failure"};
      }
      if (!slot.module || !same_code(slot) || slot.limits != limits) {
         // Never create a transient tenth module. A throwing copy/compiler
         // cannot publish a usable old key. Negative entries share the SAME
         // bounded LRU; they never retain a backend, exception or VM state.
         slot.module.reset();
         slot.link_failed = false;
         performance::scope compile_timer(performance::stage::parallel_compile);
         auto owned_code = code;
         const auto owned_hash=identity ? *identity : fc::sha256::hash(owned_code.data(),owned_code.size());
         std::unique_ptr<isolated_wasm_module> module;
         try {
            module = std::make_unique<isolated_wasm_module>(owned_code, limits, runtime);
         } catch (const eosio::vm::wasm_link_exception&) {
            slot.code = std::move(owned_code); slot.code_hash=owned_hash;
            slot.limits = limits;
            slot.link_failed = true;
            touch(selected);
            throw; // preserve the original first failure's diagnostic
         }
         // Parse/budget/allocation/cancellation failures are NOT cached. Nor
         // are any failures from execute_step, including missing state/hosts.
         slot.code = std::move(owned_code); slot.code_hash=owned_hash;
         slot.limits = limits;
         slot.module = std::move(module);
      }
      // Construction has succeeded. Updating the fixed permutation and
      // acquiring/releasing the lease cannot allocate or throw during unwind.
      touch(selected);
      return lease(slot, active_depth[depth]);
   }
};
} // namespace

struct isolated_module_cache::impl : leased_modules {
   impl(size_t capacity, bool fixed_depth, std::shared_ptr<const isolated_execution_runtime> runtime)
      : leased_modules(capacity, fixed_depth, std::move(runtime)) {}
};
isolated_module_cache::isolated_module_cache(std::shared_ptr<const isolated_execution_runtime> runtime)
   : isolated_module_cache(leased_modules::capacity, false, std::move(runtime)) {}
isolated_module_cache::isolated_module_cache(size_t capacity, bool fixed_depth,
      std::shared_ptr<const isolated_execution_runtime> runtime)
   : impl_(std::make_unique<impl>(capacity, fixed_depth, std::move(runtime))) {}
isolated_module_cache::~isolated_module_cache() = default;

struct isolated_sync_executor::impl {
   std::shared_ptr<isolated_module_cache> modules;
   explicit impl(std::shared_ptr<isolated_module_cache> cache) : modules(std::move(cache)) {
      if (!modules) throw primary_state_invalid_attempt();
      modules->impl_->check_owner();
   }
};

isolated_sync_executor::isolated_sync_executor()
   : isolated_sync_executor(std::shared_ptr<isolated_module_cache>(new isolated_module_cache(max_depth + 1, true))) {}
isolated_sync_executor::isolated_sync_executor(std::shared_ptr<isolated_module_cache> cache)
   : impl_(std::make_unique<impl>(std::move(cache))) {}
isolated_sync_executor::~isolated_sync_executor() = default;

isolated_sync_result isolated_sync_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      const isolated_action_input& root, std::span<const isolated_sync_program> programs,
      isolated_sync_context protocol, std::stop_token stop, primary_overlay_limits bounds, isolated_cancel external) {
   auto transaction = execute_roots(std::move(base), {&root, 1}, programs, protocol, stop, bounds, external, true);
   auto result = std::move(transaction.roots.front());
   const auto fingerprint = transaction.execution_fingerprint;
   return {std::move(transaction.state), std::move(result.root), std::move(result.calls),
           std::move(result.root_console_markers), fingerprint, std::move(result.root_events), std::move(transaction.environment)};
}

isolated_sync_transaction_result isolated_sync_executor::execute_transaction(
      std::shared_ptr<const frozen_primary_state> base, std::span<const isolated_action_input> roots,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol,
      std::stop_token stop, primary_overlay_limits bounds, isolated_cancel external) {
   return execute_roots(std::move(base), roots, programs, protocol, stop, bounds, external, false);
}

isolated_sync_transaction_result isolated_sync_executor::execute_roots(
      std::shared_ptr<const frozen_primary_state> base, std::span<const isolated_action_input> roots,
      std::span<const isolated_sync_program> programs, isolated_sync_context protocol,
      std::stop_token stop, primary_overlay_limits bounds, isolated_cancel external, bool single_root_fingerprint) {
   if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() + max_attempt_time);
   external.check(stop);
   if (programs.empty() || programs.size() > max_programs || roots.empty() || roots.size() > max_roots ||
       (single_root_fingerprint && roots.size() != 1))
      throw primary_state_budget_exceeded();
   size_t total_code = 0, total_data = 0, total_calls = 0;
   for (const auto& root : roots) {
      external.check(stop);
      if (root.receiver != root.account || root.data.size() > max_total_data_bytes - total_data)
         throw primary_state_budget_exceeded();
      if (!same_isolated_environment(root.environment, roots.front().environment)) throw primary_state_invalid_attempt();
      total_data += root.data.size();
   }
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
   std::vector<isolated_sync_root_result> results;
   results.reserve(roots.size());
   std::vector<isolated_sync_trace> traces;
   std::vector<uint32_t> root_markers;
   std::vector<isolated_sync_event> root_events;
   isolated_console_budget console_budget;
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
      auto module = impl_->modules->impl_->acquire(depth, program);
      external.check(stop); // compilation/cache lookup shares the original deadline
      isolated_sync_capture frame;
      frame.is_sync = ordinal != 0;
      frame.read_only = read_only;
      frame.sender = sender;
      frame.get_sender_fix = protocol.get_sender_fix;
      frame.console_budget = &console_budget;
      frame.console_enabled = protocol.console_enabled;
      frame.record = [&](const isolated_cursor_event& event) { record(ordinal, event); };
      frame.call = [&](name receiver, uint64_t flags, std::span<const char> data) -> call_result {
         external.check(stop);
         if (total_calls >= max_calls || data.size() > max_total_data_bytes - total_data)
            throw primary_state_budget_exceeded();
         ++total_calls;
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
            if (ordinal) traces[ordinal - 1].console_markers.push_back(frame.console_events);
            else root_markers.push_back(frame.console_events);
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
         if (!read_sync_target_account(state, receiver, flags)) return failed(-1);
         if (flags > 1) throw primary_state_unsupported();
         const auto& child_program = find_program(receiver);
         if (child_program.program.serial_only) throw primary_state_unsupported();
         if (child_program.program.code.empty()) return failed(-1);
         if (!child_program.sync_call_supported) return failed(-2);
         isolated_action_input child;
         child.receiver = child.account = receiver;
         child.data = traces[child_ordinal - 1].data;
         child.environment = input.environment;
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
      auto effects = module.get().execute_step(state, input, stop, external, nullptr, nullptr, nullptr, 0, nullptr, &frame);
      status = frame.status;
      return effects;
   };
   for (const auto& root : roots) {
      external.check(stop);
      if (!state.read_account(root.receiver)) throw primary_state_unsupported();
      // Call ordinals belong to THIS action trace. The overlay and all attempt
      // budgets above survive; memory, cursors and returned bytes do not.
      traces.clear(); root_markers.clear(); root_events.clear();
      traces.reserve(max_calls - total_calls);
      int64_t status = 0;
      auto effects = run(find_program(root.receiver), root, name{}, 0, 0, protocol.root_read_only, status);
      external.check(stop);
      if (!effects.recipients.empty()) throw primary_state_invalid_attempt();
      results.push_back({std::move(effects), std::move(traces), std::move(root_markers), std::move(root_events)});
   }
   const auto fingerprint = single_root_fingerprint ? isolated_sync_fingerprint(roots.front(), programs, protocol) :
                                                       isolated_sync_transaction_fingerprint(roots, programs, protocol);
   external.check(stop);
   return {state.seal(), std::move(results), fingerprint, roots.front().environment};
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
   std::shared_ptr<isolated_module_cache> modules;
   explicit impl(std::shared_ptr<isolated_module_cache> cache) : modules(std::move(cache)) {
      if (!modules) throw primary_state_invalid_attempt();
      modules->impl_->check_owner();
   }
};

isolated_transaction_executor::isolated_transaction_executor()
   : isolated_transaction_executor(std::shared_ptr<isolated_module_cache>(new isolated_module_cache(4, false))) {}
isolated_transaction_executor::isolated_transaction_executor(std::shared_ptr<isolated_module_cache> cache)
   : impl_(std::make_unique<impl>(std::move(cache))) {}
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
      if (action.code.size() > isolated_wasm_limits::max_module_code_bytes || action.code.size() > max_total_code_bytes - code_bytes ||
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
      auto module = impl_->modules->impl_->acquire(0, action.code, action.limits);
      checkpoint();
      {
         performance::scope execute_timer(performance::stage::parallel_execute);
         results.push_back(module.get().execute_step(state, action.input, stop, external));
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
   std::shared_ptr<isolated_module_cache> modules;
   explicit impl(std::shared_ptr<isolated_module_cache> cache) : modules(std::move(cache)) {
      if (!modules) throw primary_state_invalid_attempt();
      modules->impl_->check_owner();
   }
};

isolated_notification_executor::isolated_notification_executor()
   : isolated_notification_executor(std::shared_ptr<isolated_module_cache>(new isolated_module_cache(4, false))) {}
isolated_notification_executor::isolated_notification_executor(std::shared_ptr<isolated_module_cache> cache)
   : impl_(std::make_unique<impl>(std::move(cache))) {}
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
      if (program.code.size() > isolated_wasm_limits::max_module_code_bytes ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes)
         throw primary_state_budget_exceeded();
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, program_entry{&program,
            program.code.empty() ? fc::sha256{} : program.code.content_hash()}).second)
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
      secondary_cursor_group secondary(state, root.receiver);
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
            auto module = impl_->modules->impl_->acquire(0, program);
            checkpoint();
            performance::scope execute_timer(performance::stage::parallel_execute);
            effects = module.get().execute_step(state, arguments, stop, external, &cursor, &notified,
               &cursor_events, max_cursor_events - event_count, nullptr, nullptr, &secondary);
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
   constexpr char domain[] = "pureeos/isolated-action-tree/v3";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, isolated_transaction_fingerprint(roots));
   fc::raw::pack(encoder, context.max_inline_action_size);
   fc::raw::pack(encoder, context.max_inline_action_depth);
   fc::raw::pack(encoder, context.console_enabled);
   fc::raw::pack(encoder, bool(context.context_free_data));
   if (context.context_free_data) fc::raw::pack(encoder, context.context_free_data->fingerprint());
   return encoder.result();
}

fc::sha256 isolated_action_tree_step_fingerprint(const fc::sha256& action_fingerprint,
      const isolated_action_tree_context& context, const isolated_action_tree_group& group,
      const isolated_action_tree_step& step) {
   fc::sha256::encoder encoder;
   constexpr char domain[] = "pureeos/isolated-action-tree-step/v3";
   encoder.write(domain, sizeof(domain));
   fc::raw::pack(encoder, action_fingerprint);
   fc::raw::pack(encoder, context.max_inline_action_size);
   fc::raw::pack(encoder, context.max_inline_action_depth);
   fc::raw::pack(encoder, context.console_enabled);
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
   std::shared_ptr<isolated_module_cache> modules;
   explicit impl(std::shared_ptr<isolated_module_cache> cache) : modules(std::move(cache)) {
      if (!modules) throw primary_state_invalid_attempt();
      modules->impl_->check_owner();
   }
};

isolated_action_tree_executor::isolated_action_tree_executor()
   : isolated_action_tree_executor(std::shared_ptr<isolated_module_cache>(new isolated_module_cache(4, false))) {}
isolated_action_tree_executor::isolated_action_tree_executor(std::shared_ptr<isolated_module_cache> cache)
   : impl_(std::make_unique<impl>(std::move(cache))) {}
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
      if (program.code.size() > isolated_wasm_limits::max_module_code_bytes ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes)
         throw primary_state_budget_exceeded();
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, entry{&program,
            program.code.empty() ? fc::sha256{} : program.code.content_hash()}).second)
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
   isolated_console_budget console_budget;
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
      if (!same_isolated_environment(root.environment, roots.front().environment)) throw primary_state_invalid_attempt();
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
      secondary_cursor_group secondary(state, group.input.receiver);
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
            auto module = impl_->modules->impl_->acquire(0, program);
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
               }, step.sender, group.context_free, tree_context.context_free_data.get(), &console_budget, tree_context.console_enabled};
            std::vector<isolated_cursor_event> cursor_events;
            performance::scope execute_timer(performance::stage::parallel_execute);
            step.effects = module.get().execute_step(state, arguments, stop, external, &cursor, &notified,
               &cursor_events, max_events - event_count, &capture, nullptr, &secondary);
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
               child.environment = group.input.environment;
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

fc::sha256 isolated_mixed_tree_fingerprint(std::span<const isolated_action_input> roots,
      std::span<const isolated_sync_program> programs, const isolated_mixed_tree_context& context) {
   fc::sha256::encoder hash;
   constexpr char domain[] = "pureeos/isolated-mixed-tree/v2";
   hash.write(domain, sizeof(domain));
   // This binds the FULL catalog, all VM limits, real sync-supported bits,
   // ordered root inputs and sync protocol, including unused captured entries.
   fc::raw::pack(hash, isolated_sync_transaction_fingerprint(roots, programs, context.sync));
   fc::raw::pack(hash, context.tree.max_inline_action_size);
   fc::raw::pack(hash, context.tree.max_inline_action_depth);
   fc::raw::pack(hash, context.tree.console_enabled);
   fc::raw::pack(hash, bool(context.tree.context_free_data));
   if (context.tree.context_free_data) fc::raw::pack(hash, context.tree.context_free_data->fingerprint());
   return hash.result();
}

fc::sha256 isolated_mixed_tree_step_fingerprint(const fc::sha256& action_fingerprint,
      const fc::sha256& transaction_fingerprint, const isolated_mixed_tree_context& context,
      const isolated_action_tree_group& group, const isolated_mixed_tree_step& step) {
   isolated_action_tree_step identity;
   identity.group_index = step.group_index; identity.action_ordinal = step.action_ordinal;
   identity.creator_action_ordinal = step.creator_action_ordinal;
   identity.closest_unnotified_ancestor = step.closest_unnotified_ancestor; identity.sender = step.sender;
   fc::sha256::encoder hash;
   fc::raw::pack(hash, transaction_fingerprint);
   fc::raw::pack(hash, isolated_action_tree_step_fingerprint(action_fingerprint, context.tree, group, identity));
   return hash.result();
}

struct isolated_mixed_tree_executor::impl {
   const std::thread::id owner = std::this_thread::get_id();
   std::shared_ptr<isolated_module_cache> modules;
   explicit impl(std::shared_ptr<isolated_module_cache> cache) : modules(std::move(cache)) {
      if (!modules) throw primary_state_invalid_attempt();
      modules->impl_->check_owner();
   }
};

isolated_mixed_tree_executor::isolated_mixed_tree_executor()
   : isolated_mixed_tree_executor(std::make_shared<isolated_module_cache>()) {}
isolated_mixed_tree_executor::isolated_mixed_tree_executor(std::shared_ptr<isolated_module_cache> cache)
   : impl_(std::make_unique<impl>(std::move(cache))) {}
isolated_mixed_tree_executor::~isolated_mixed_tree_executor() = default;

isolated_mixed_tree_result isolated_mixed_tree_executor::execute(std::shared_ptr<const frozen_primary_state> base,
      std::span<const isolated_action_input> roots, std::span<const isolated_sync_program> programs,
      isolated_mixed_tree_context context, std::stop_token stop, primary_overlay_limits bounds,
      isolated_cancel external) {
   if (std::this_thread::get_id() != impl_->owner) throw state_view_wrong_thread();
   external.deadline = std::min(external.deadline, std::chrono::steady_clock::now() +
                                                isolated_sync_executor::max_attempt_time);
   const auto checkpoint = [&] { external.check(stop); };
   checkpoint();
   if (roots.empty() || roots.size() > isolated_sync_executor::max_roots ||
       programs.empty() || programs.size() > max_programs) throw primary_state_budget_exceeded();
   struct entry { const isolated_sync_program* captured; fc::sha256 hash; };
   std::map<name, entry> catalog;
   size_t code_bytes = 0, data_bytes = 0, event_count = 0, total_calls = 0;
   isolated_console_budget console_budget;
   if (context.tree.console_enabled != context.sync.console_enabled) throw primary_state_unsupported();
   for (const auto& captured : programs) {
      checkpoint();
      const auto& p = captured.program;
      if (p.code.size() > isolated_wasm_limits::max_module_code_bytes || p.code.size() > isolated_sync_executor::max_total_code_bytes - code_bytes)
         throw primary_state_budget_exceeded();
      if (!p.receiver || !catalog.emplace(p.receiver, entry{&captured,
            p.code.empty() ? fc::sha256{} : p.code.content_hash()}).second)
         throw primary_state_unsupported();
      code_bytes += p.code.size();
   }
   const auto lookup = [&](name receiver) -> const entry& {
      const auto found = catalog.find(receiver);
      if (found == catalog.end()) throw isolated_receiver_missing(receiver);
      if (found->second.captured->program.serial_only) throw primary_state_unsupported();
      return found->second;
   };
   std::vector<isolated_action_tree_group> groups;
   std::vector<isolated_mixed_tree_step> results;
   groups.reserve(max_steps); results.reserve(max_steps);
   for (size_t i = 0; i < roots.size(); ++i) {
      checkpoint();
      const auto& root = roots[i];
      if (root.receiver != root.account || !root.receiver) throw primary_state_unsupported();
      if (!same_isolated_environment(root.environment, roots.front().environment)) throw primary_state_invalid_attempt();
      if (root.data.size() > 64 * 1024 || root.data.size() > max_data_bytes - data_bytes ||
          root.authorizations.size() > 64 || root.max_return_bytes > 4096) throw primary_state_budget_exceeded();
      if (lookup(root.receiver).captured->program.code.empty()) throw primary_state_unsupported();
      data_bytes += root.data.size();
      groups.push_back({i, uint32_t(i + 1), 0, 0, 0, {}, root});
   }
   const auto fingerprint = isolated_mixed_tree_fingerprint(roots, programs, context);
   primary_state_overlay state(std::move(base), bounds);
   uint32_t next_ordinal = roots.size() + 1;
   const auto count_event = [&] {
      checkpoint();
      if (event_count >= max_events) throw primary_state_budget_exceeded();
      ++event_count;
   };
   struct pending_receiver { name receiver; uint32_t ordinal, creator, ancestor; name sender; };
   std::function<void(size_t)> execute_group = [&](size_t group_index) {
      checkpoint();
      // Reserve/check max_steps before every insertion. No group reference is
      // invalidated by descendants, and no step reference survives results growth.
      const auto& group = groups[group_index];
      primary_overlay_cursor cursor(state, group.input.receiver);
      secondary_cursor_group secondary(state, group.input.receiver);
      std::vector<char> group_call_return;
      std::set<name> notified{group.input.receiver};
      std::vector<pending_receiver> pending{{group.input.receiver, group.action_ordinal,
         group.creator_action_ordinal, group.closest_unnotified_ancestor, group.sender}};
      std::vector<size_t> children, cf_children;
      for (size_t i = 0; i < pending.size(); ++i) {
         checkpoint();
         const auto current = pending[i];
         if (!state.read_account(current.receiver)) throw primary_state_unsupported();
         const auto& found = lookup(current.receiver);
         auto arguments = group.input;
         arguments.receiver = current.receiver;
         isolated_mixed_tree_step step;
         step.group_index = group_index; step.action_ordinal = current.ordinal;
         step.creator_action_ordinal = current.creator; step.closest_unnotified_ancestor = current.ancestor;
         step.receiver = current.receiver; step.sender = current.sender;
         step.code_hash = found.hash; step.has_code = !found.captured->program.code.empty();
         isolated_tree_capture tree{
            [&](const isolated_cursor_event&) { throw primary_state_invalid_attempt(); },
            [&](std::span<const char> bytes, bool cf) {
               count_event();
               if (bytes.size() > max_data_bytes - data_bytes) throw primary_state_budget_exceeded();
               auto sent = decode_isolated_inline_action(bytes, context.tree.max_inline_action_size);
               if (!sent || (cf && (!context.tree.context_free_data || !sent->decoded.authorization.empty())))
                  throw primary_state_unsupported();
               data_bytes += bytes.size();
               step.events.emplace_back(isolated_tree_inline_event{std::move(*sent), state.write_count(), 0, cf});
            }, step.sender, group.context_free, context.tree.context_free_data.get(), &console_budget, context.tree.console_enabled};
         using call_result = std::pair<int64_t, std::vector<char>>;
         std::function<isolated_action_effects(const entry&, const isolated_action_input&,
            name, uint32_t, uint32_t, bool, int64_t&)> run;
         run = [&](const entry& program, const isolated_action_input& input, name sender,
                   uint32_t ordinal, uint32_t depth, bool read_only, int64_t& status) {
            checkpoint();
            auto module = impl_->modules->impl_->acquire(depth, program.captured->program);
            checkpoint();
            isolated_sync_capture frame;
            frame.is_sync = ordinal != 0; frame.read_only = read_only;
            if (!ordinal) frame.group_call_return = &group_call_return;
            frame.sender = sender; frame.get_sender_fix = context.sync.get_sender_fix;
            frame.console_budget = &console_budget;
            frame.console_enabled = context.sync.console_enabled;
            frame.record = [&](const isolated_cursor_event& event) {
               count_event();
               if (ordinal) step.calls.at(ordinal - 1).events.emplace_back(event);
               else step.events.emplace_back(event);
            };
            frame.call = [&](name receiver, uint64_t flags, std::span<const char> data) -> call_result {
               checkpoint();
               if (total_calls >= max_calls || data.size() > max_data_bytes - data_bytes)
                  throw primary_state_budget_exceeded();
               ++total_calls; data_bytes += data.size();
               const uint32_t child_ordinal = step.calls.size() + 1;
               isolated_sync_trace trace;
               trace.call_ordinal = child_ordinal; trace.sender_ordinal = ordinal;
               trace.sender = input.receiver; trace.receiver = receiver; trace.flags = flags;
               trace.read_only = read_only || bool(flags & 1);
               trace.data.assign(data.begin(), data.end());
               trace.writes_begin = trace.writes_end = state.write_count();
               step.calls.push_back(std::move(trace));
               count_event();
               const isolated_sync_call_event event{child_ordinal, state.write_count()};
               if (ordinal) step.calls.at(ordinal - 1).events.emplace_back(event);
               else step.events.emplace_back(event);
               if (context.sync.console_enabled) {
                  if (ordinal) step.calls.at(ordinal - 1).console_markers.push_back(frame.console_events);
                  else step.console_markers.push_back(frame.console_events);
               }
               if (depth >= context.sync.max_depth || depth >= isolated_sync_executor::max_depth ||
                   data.size() > context.sync.max_data_bytes) throw primary_state_unsupported();
               const auto failed = [&](int64_t error) -> call_result {
                  step.calls.at(child_ordinal - 1).status = error;
                  checkpoint();
                  return {error, {}};
               };
               if (!read_sync_target_account(state, receiver, flags)) return failed(-1); // precedes flags, as in original
               if (flags > 1) throw primary_state_unsupported();
               const auto& child_program = lookup(receiver);
               if (child_program.captured->program.code.empty()) return failed(-1);
               if (!child_program.captured->sync_call_supported) return failed(-2);
               isolated_action_input child;
               child.receiver = child.account = receiver;
               child.data = step.calls.at(child_ordinal - 1).data;
               child.environment = input.environment;
               child.max_return_bytes = std::min(context.sync.max_data_bytes,
                                                child_program.captured->program.limits.max_return_bytes);
               int64_t child_status = 0;
               auto effects = run(child_program, child, input.receiver, child_ordinal, depth + 1,
                                  read_only || bool(flags & 1), child_status);
               if (child_status != 0 && child_status > -10000) child_status = -3;
               auto& completed = step.calls.at(child_ordinal - 1); // reacquire AFTER nested vector growth
               completed.executed = true; completed.exclusive_elapsed = effects.elapsed;
               completed.writes_begin = effects.writes_begin; completed.writes_end = effects.writes_end;
               completed.execution_fingerprint = effects.execution_fingerprint;
               completed.began = effects.began; completed.ended = effects.ended;
               if (child_status < 0) return failed(child_status);
               completed.return_value = std::move(effects.return_value);
               checkpoint();
               return {0, completed.return_value};
            };
            // Only ordinary steps borrow the notification group's cursor and
            // send queue. Synchronous descendants get fresh cursors/capabilities.
            std::vector<isolated_cursor_event> cursor_events;
            auto effects = ordinal ?
               module.get().execute_step(state, input, stop, external, nullptr, nullptr, nullptr, 0, nullptr, &frame) :
               module.get().execute_step(state, input, stop, external, &cursor, &notified,
                  &cursor_events, max_events - event_count, &tree, &frame, &secondary);
            status = frame.status;
            return effects;
         };
         if (step.has_code) {
            int64_t status = 0;
            step.effects = run(found, arguments, step.sender, 0, 0, context.sync.root_read_only, status);
         } else {
            step.effects.writes_begin = step.effects.writes_end = state.write_count();
            step.effects.execution_fingerprint = isolated_execution_fingerprint(found.hash,
               found.captured->program.limits, arguments);
         }
         step.effects.execution_fingerprint = isolated_mixed_tree_step_fingerprint(
            step.effects.execution_fingerprint, fingerprint, context, group, step);
         for (auto& event : step.events) {
            checkpoint();
            if (const auto* ce = std::get_if<isolated_cursor_event>(&event)) {
               if (const auto* notification = std::get_if<isolated_cursor_notify>(ce)) {
                  if (next_ordinal > max_steps) throw primary_state_budget_exceeded();
                  const auto recipient = step.effects.recipients.at(notification->notification_index).recipient;
                  pending.push_back({recipient, next_ordinal++, current.ordinal, group.action_ordinal, current.receiver});
               }
            } else if (auto* send = std::get_if<isolated_tree_inline_event>(&event)) {
               if (next_ordinal > max_steps || groups.size() >= max_steps) throw primary_state_budget_exceeded();
               send->action_ordinal = next_ordinal++;
               const auto& act = send->sent.decoded;
               isolated_action_input child;
               child.receiver = child.account = act.account; child.action = act.name;
               child.data = act.data; child.max_return_bytes = group.input.max_return_bytes;
               child.environment = group.input.environment;
               for (const auto& p : act.authorization) child.authorizations.emplace_back(p.actor, p.permission);
               (send->context_free ? cf_children : children).push_back(groups.size());
               groups.push_back({group.root_index, send->action_ordinal, current.ordinal, group.action_ordinal,
                  group.depth + 1, current.receiver, std::move(child), send->context_free});
            } // synchronous calls never allocate action ordinals or queued groups
         }
         if (results.size() >= max_steps) throw primary_state_budget_exceeded();
         results.push_back(std::move(step));
      }
      if ((!children.empty() || !cf_children.empty()) &&
          (group.depth >= context.tree.max_inline_action_depth || group.depth >= isolated_action_tree_executor::max_depth))
         throw primary_state_unsupported();
      for (const auto child : cf_children) execute_group(child);
      for (const auto child : children) execute_group(child);
   };
   for (size_t i = 0; i < roots.size(); ++i) execute_group(i);
   checkpoint();
   return {state.seal(), std::move(groups), std::move(results), fingerprint, std::move(context.tree.context_free_data)};
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
       result.state.secondary_read_queries().size() > bounds.max_read_queries - result.state.read_queries().size() ||
       result.state.read_accounts().size() > bounds.max_read_accounts) return false;

   struct entry { const isolated_receiver_program* program; fc::sha256 hash; };
   std::map<name, entry> catalog;
   size_t code_bytes = 0, data_bytes = 0, write_bytes = 0;
   for (const auto& program : programs) {
      check();
      if (program.code.size() > isolated_wasm_limits::max_module_code_bytes ||
          program.code.size() > isolated_transaction_executor::max_total_code_bytes - code_bytes) return false;
      code_bytes += program.code.size();
      if (!program.receiver || !catalog.emplace(program.receiver, entry{&program,
            program.code.empty() ? fc::sha256{} : program.code.content_hash()}).second)
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
   for (const auto& mutation : result.state.writes()) {
      check();
      const auto bytes = isolated_state_write_bytes(mutation);
      if (bytes > bounds.max_write_value_bytes - write_bytes) return false;
      write_bytes += bytes;
   }

   primary_state_overlay replay(result.state.base());
   size_t next_step = 0, next_write = 0, event_count = 0;
   uint32_t next_ordinal = roots.size() + 1;
   std::chrono::microseconds elapsed{};
   for (size_t root_index = 0; root_index < roots.size(); ++root_index) {
      const auto& root = roots[root_index];
      primary_overlay_cursor cursor(replay, root.receiver);
      secondary_cursor_group secondary(replay, root.receiver);
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
         secondary.set_receiver_for_notification(step.receiver);
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
                  const auto& write = result.state.primary_write(next_write++);
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
               } else if (const auto* secondary_event = std::get_if<isolated_secondary_cursor_event>(&event)) {
                  if (!replay_isolated_secondary_cursor_event(secondary, *secondary_event, result.state,
                        next_write, effects.writes_end, false)) return false;
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
   return validate_isolated_replay_state(reconstructed, result.state, checkpoint);
}

} // namespace eosio::chain::experimental
