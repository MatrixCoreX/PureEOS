#pragma once

// Private JIT context selected explicitly by the worker runtime. Checkpoints
// never revoke shared code pages; module/cache/frame ownership remains private.
#include <eosio/vm/backend.hpp>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef __x86_64__
#include <xmmintrin.h>
#endif

namespace eosio::chain::experimental {
#ifdef __x86_64__

struct parallel_jit_frame {
   template<typename> friend class parallel_jit_context;
private:
   std::atomic<bool> interrupted_{false};
   std::atomic<std::atomic<bool>*> bound_flag_{nullptr};
public:
   // Publish to both the frame and its context-owned inline byte. The SC
   // handshake also preserves an interrupt racing the initial binding: either
   // binding observes interrupted_, or interrupt() observes bound_flag_. This
   // work happens only on cancellation, not at healthy JIT checkpoints.
   void interrupt() noexcept {
      interrupted_.store(true, std::memory_order_seq_cst);
      if (auto* flag = bound_flag_.load(std::memory_order_seq_cst))
         flag->store(true, std::memory_order_release);
   }
   void* state = nullptr;
   void (*before_memory_grow)(void*, int32_t current_pages, int32_t requested_pages) = nullptr;
};

template<typename Host>
class parallel_jit_context : public eosio::vm::jit_execution_context<Host> {
   using original = eosio::vm::jit_execution_context<Host>;
   const std::thread::id owner_ = std::this_thread::get_id();
   parallel_jit_frame* frame_ = nullptr;
   // Fail closed while unbound. A healthy checkpoint loads this byte directly,
   // without loading/dereferencing a frame pointer. Deadline callbacks must be
   // unregistered before unbinding and reusing the context, as before.
   std::atomic<bool> execution_budget_flag_{true};
   int32_t flag_offset() const {
      const auto begin = reinterpret_cast<uintptr_t>(this);
      const auto member = reinterpret_cast<uintptr_t>(&execution_budget_flag_);
      if (member < begin || member - begin > uint32_t(std::numeric_limits<int32_t>::max()))
         throw std::logic_error("unsupported private JIT flag layout");
      return static_cast<int32_t>(member - begin);
   }
public:
   using original::original;
   parallel_jit_context() = default;
   parallel_jit_context(const parallel_jit_context&) = delete;
   parallel_jit_context& operator=(const parallel_jit_context&) = delete;
   parallel_jit_context(parallel_jit_context&&) = delete;
   parallel_jit_context& operator=(parallel_jit_context&&) = delete;
   // Explicit x86-64 code-generation ABI: a lock-free one-byte atomic at this
   // displacement from the context in RDI. Compute it from a
   // LIVE object, not offsetof on a non-standard-layout class or a fake/null
   // object. Check every real binding against it before entering native code.
   // x86's ordinary byte load provides acquire ordering; no fence is needed.
   static int32_t execution_budget_flag_offset() {
      static_assert(std::atomic<bool>::is_always_lock_free && sizeof(std::atomic<bool>) == 1);
      static const int32_t offset = [] {
         parallel_jit_context probe;
         if (static_cast<void*>(static_cast<original*>(&probe)) != static_cast<void*>(&probe))
            throw std::logic_error("unsupported private JIT context layout");
         return probe.flag_offset();
      }();
      return offset;
   }
   // The inherited entry trampoline passes its BASE context address. The
   // private machine-code writer calls helpers using this derived type. Reject
   // a platform/ABI layout where those addresses differ BEFORE entering JIT.
   class binding {
      friend class parallel_jit_context;
      parallel_jit_context& context_;
      // The native trampoline restores MXCSR on its normal return, but a
      // longjmp from a checkpoint/host skips that instruction. This guard
      // lives OUTSIDE the JIT signal frame and restores the caller on both
      // paths. Do not modify the separate SoftFloat TLS rounding state.
      const uint32_t saved_mxcsr_ = _mm_getcsr();
      explicit binding(parallel_jit_context& context, parallel_jit_frame& frame)
         : context_(context) {
         // Clear before publishing the destination, then check the sticky
         // frame bit. Together with interrupt()'s SC operations this cannot
         // lose an early/racing interruption. A frame cannot bind twice.
         context_.execution_budget_flag_.store(false, std::memory_order_release);
         std::atomic<bool>* expected = nullptr;
         if (!frame.bound_flag_.compare_exchange_strong(expected,
               &context_.execution_budget_flag_, std::memory_order_seq_cst)) {
            context_.execution_budget_flag_.store(true, std::memory_order_release);
            throw std::logic_error("private JIT frame already bound");
         }
         context_.frame_ = &frame;
         if (frame.interrupted_.load(std::memory_order_seq_cst))
            context_.execution_budget_flag_.store(true, std::memory_order_release);
      }
   public:
      binding(const binding&) = delete;
      binding& operator=(const binding&) = delete;
      ~binding() {
         if (std::this_thread::get_id() != context_.owner_) std::terminate();
         context_.execution_budget_flag_.store(true, std::memory_order_release);
         context_.frame_->bound_flag_.store(nullptr, std::memory_order_seq_cst);
         context_.frame_ = nullptr;
         _mm_setcsr(saved_mxcsr_);
      }
   };
   [[nodiscard]] binding bind_frame(parallel_jit_frame& frame) {
      if (std::this_thread::get_id() != owner_ || frame_ ||
          !frame.state || !frame.before_memory_grow ||
          flag_offset() != execution_budget_flag_offset() ||
          static_cast<void*>(static_cast<original*>(this)) != static_cast<void*>(this))
         throw std::logic_error("invalid private JIT frame binding");
      return binding(*this, frame);
   }

   // Called by C++ entry/growth guards and the generated slow-path exception
   // bridge. The generated healthy path loads the same flag without a call.
   void check_execution_budget() const {
      if (!frame_) throw eosio::vm::wasm_interpreter_exception{"unbound private JIT frame"};
      if (frame_->interrupted_.load(std::memory_order_acquire))
         throw eosio::vm::timeout_exception{"private JIT execution interrupted"};
   }

   // The upstream grow helper does not bridge C++ exceptions. Do it here,
   // inside the active VM signal frame, not during parsing/reset/construction.
   // The owner-supplied guard enforces cancellation and local memory bounds
   // before the original module maximum/allocation/result handling.
   int32_t grow_linear_memory(int32_t pages) {
      int32_t result = -1;
      eosio::vm::longjmp_on_exception([&] {
         check_execution_budget();
         frame_->before_memory_grow(frame_->state, original::current_linear_memory(), pages);
         result = original::grow_linear_memory(pages);
      });
      return result;
   }
};

struct parallel_jit {
   template<typename Host> using context = parallel_jit_context<Host>;
   template<typename Host, typename Options, typename DebugInfo>
   using parser = eosio::vm::binary_parser<eosio::vm::machine_code_writer<context<Host>>, Options, DebugInfo>;
   static constexpr bool is_jit = true;
};

#endif
} // namespace eosio::chain::experimental
