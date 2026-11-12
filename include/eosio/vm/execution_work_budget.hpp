#pragma once

#include <cstdint>
#include <stdexcept>

namespace eosio::vm {

class execution_work_exhausted : public std::runtime_error {
public:
   execution_work_exhausted() : std::runtime_error("execution work budget exhausted") {}
};

// Explicit experimental prices supplied by trusted common-policy code. Zero
// means unconfigured and rejects a positive quantity; it never means free.
struct execution_work_memory_prices {
   uint64_t page_units = 0;
   uint64_t initialization_byte_units = 0;
};

// Non-owning, per-execution capability supplied by trusted native context code.
// It must not be retained in a shared module or exposed to WASM linear memory.
// This component alone does not insert or execute work checkpoints.
struct execution_work_budget_view {
   uint64_t* remaining = nullptr;
   bool* exhausted = nullptr;
   execution_work_memory_prices memory_prices{};

   void charge(uint64_t units) const {
      if (!remaining || !exhausted) throw std::logic_error("VM execution work budget is unbound");
      if (*exhausted || units > *remaining) {
         *exhausted = true;
         throw execution_work_exhausted();
      }
      *remaining -= units;
   }

   void charge_memory(uint64_t pages,uint64_t initialization_bytes=0,uint64_t fixed_items=0) const {
      charge(0);
      if ((pages && !memory_prices.page_units) ||
          (initialization_bytes && !memory_prices.initialization_byte_units)) {
         *exhausted=true;
         throw std::logic_error("execution memory work policy is unconfigured");
      }
      auto units=fixed_items;
      const auto add_product=[&](uint64_t count,uint64_t price) {
         if (price && count>(UINT64_MAX-units)/price) {
            *exhausted=true;
            throw execution_work_exhausted();
         }
         units+=count*price;
      };
      add_product(pages,memory_prices.page_units);
      add_product(initialization_bytes,memory_prices.initialization_byte_units);
      charge(units); // one atomic reservation before any memory side effect
   }
};

} // namespace eosio::vm
