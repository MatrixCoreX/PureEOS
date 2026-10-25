#pragma once

#include <eosio/vm/execution_work_budget.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace eosio::chain {

// Integer work units, not measured microseconds. Until both VMs and all native
// operations have deterministic costs, this component must not admit blocks or
// replace account CPU charges. No clock, worker count or machine quota is input.
// One resource-error category across native code, VM checkpoints and nested
// exception bridges. A completion fence must preserve an existing budget error.
using execution_work_exhausted = eosio::vm::execution_work_exhausted;

struct execution_work_usage {
   uint64_t wasm = 0;
   uint64_t host = 0;
   uint64_t signature = 0;
   uint64_t native = 0;
   // A subset of host + native, not an additional charge. Native owner work is
   // always serial; a host cost may also include mandatory canonical commit.
   uint64_t serial = 0;

   static uint64_t add(uint64_t a, uint64_t b) {
      if (b > std::numeric_limits<uint64_t>::max() - a)
         throw std::overflow_error("execution work counter overflow");
      return a + b;
   }

   uint64_t total() const { return add(add(wasm, host), add(signature, native)); }

   void validate() const {
      (void)total();
      if (serial < native || serial > add(host, native))
         throw std::invalid_argument("invalid serial execution work subset");
   }

   // Pricing is independent of execution capacity. Preserve the floor only on
   // the account invoice. Signature discount is an explicit policy argument;
   // a future deterministic protocol must obtain it from common chain state.
   uint64_t billable_units(uint64_t minimum, uint32_t signature_bill_pct) const {
      constexpr uint32_t whole = 10000;
      if (signature_bill_pct > whole) throw std::invalid_argument("invalid signature work bill percentage");
      validate();
      // Avoid signature * percentage overflowing even at UINT64_MAX. Both
      // terms and their sum are bounded above by the original signature work.
      const auto discounted = (signature / whole) * signature_bill_pct +
                              ((signature % whole) * signature_bill_pct) / whole;
      const auto invoice = add(total() - signature, discounted);
      return invoice < minimum ? minimum : invoice;
   }

   bool operator==(const execution_work_usage&) const = default;
};

// One stable, owner-held budget per semantic execution. Trial workers use their
// own instances; adopting a validated result merges its usage once. The caller
// controls adoption/undo; this counter does not authenticate a private result.
class execution_work_meter {
   uint64_t limit_;
   uint64_t remaining_;
   execution_work_usage non_wasm_{};
   bool exhausted_ = false;

public:
   explicit execution_work_meter(uint64_t limit) : limit_(limit), remaining_(limit) {
      if (!limit) throw std::invalid_argument("execution work limit must be positive");
   }
   execution_work_meter(const execution_work_meter&) = delete;
   execution_work_meter& operator=(const execution_work_meter&) = delete;
   execution_work_meter(execution_work_meter&&) = delete;
   execution_work_meter& operator=(execution_work_meter&&) = delete;

   execution_work_usage usage() const {
      const auto other = non_wasm_.total();
      if (remaining_ > limit_ || other > limit_ - remaining_)
         throw std::logic_error("invalid execution work counter state");
      auto result = non_wasm_;
      result.wasm = limit_ - remaining_ - other;
      result.validate();
      return result;
   }
   bool exhausted() const { return exhausted_; }
   // Trusted native infrastructure must invalidate this semantic execution if
   // it cannot determine a cost. A caught policy error cannot revive a budget.
   void invalidate() noexcept { exhausted_ = true; }
   uint64_t remaining() const { (void)usage(); return exhausted_ ? 0 : remaining_; }

   // Trusted native VM binding only. No clock or per-block lookup is needed
   // when charging an executed WASM block. All host/native consumers debit the
   // same remaining counter, including nested calls on this semantic execution.
   // The owning meter must outlive the binding; it cannot be copied or moved.
   using wasm_budget_view = eosio::vm::execution_work_budget_view;
   wasm_budget_view wasm_budget(eosio::vm::execution_work_memory_prices memory_prices={}) {
      if (exhausted_) throw execution_work_exhausted();
      (void)usage();
      return {&remaining_, &exhausted_, memory_prices};
   }

   void consume(const execution_work_usage& part) {
      if (exhausted_) throw execution_work_exhausted();
      try {
         part.validate();
         if (part.total() > remaining()) throw execution_work_exhausted();
         const auto used = usage();
         execution_work_usage next{
            execution_work_usage::add(used.wasm, part.wasm),
            execution_work_usage::add(used.host, part.host),
            execution_work_usage::add(used.signature, part.signature),
            execution_work_usage::add(used.native, part.native),
            execution_work_usage::add(used.serial, part.serial)};
         next.validate();
         remaining_ -= part.total();
         next.wasm = 0; // derived from the remaining budget, including VM debits
         non_wasm_ = next; // no partial charge if a preceding validation failed
      } catch (...) {
         exhausted_ = true; // a caught failure cannot revive this execution
         throw;
      }
   }
};

} // namespace eosio::chain
