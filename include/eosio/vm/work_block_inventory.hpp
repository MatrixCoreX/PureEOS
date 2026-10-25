#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace eosio::vm {

// Opt-in parser observation policy, using the existing DebugInfo callbacks.
// This records STATIC original-WASM blocks, not executed work or gas. It does
// not instrument either VM, price native/host operations or admit a block.
// Only WASM-relative offsets are retained; compiled addresses are not identity.
class work_block_inventory {
public:
   struct block {
      uint32_t wasm_offset = 0;
      uint32_t instructions = 0;
      bool operator==(const block&) const = default;
   };
   struct function {
      // Defined-function order; imported functions are not in the code section.
      uint32_t wasm_offset = 0;
      std::vector<block> blocks;
      bool operator==(const function&) const = default;
   };
   using inventory = std::vector<function>;

   struct builder {
      // Experimental metadata bounds, not active chain consensus limits.
      static constexpr std::size_t max_blocks = 262144, max_functions = 65536;
      inventory functions;
      uintptr_t wasm_base = 0;
      uint32_t last_instruction = 0;
      std::size_t blocks = 0;
      bool started = false, finished = false, has_instruction = false;
      bool split_next = true;

      uint32_t offset(const void* ptr) const {
         const auto value = reinterpret_cast<uintptr_t>(ptr);
         if (!started || value < wasm_base || value - wasm_base > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("invalid work inventory source offset");
         return static_cast<uint32_t>(value - wasm_base);
      }

      void on_code_start(const void*, const void* wasm_start) {
         if (!wasm_start) throw std::invalid_argument("missing work inventory source");
         functions.clear(); wasm_base = reinterpret_cast<uintptr_t>(wasm_start);
         last_instruction = 0; blocks = 0; started = true; finished = false;
         has_instruction = false; split_next = true;
      }

      void on_function_start(const void*, const void* wasm_start) {
         const auto pos = offset(wasm_start);
         if (finished || functions.size() >= max_functions ||
             (!functions.empty() && (pos <= last_instruction || pos <= functions.back().wasm_offset)))
            throw std::invalid_argument("invalid work inventory function order");
         functions.push_back({pos, {}});
         has_instruction = false; split_next = true;
      }

      static bool valid_instruction(uint8_t op) {
         return op <= 0x05 || (op >= 0x0b && op <= 0x11) || op == 0x1a || op == 0x1b ||
                (op >= 0x20 && op <= 0x24) || (op >= 0x28 && op <= 0xbf);
      }

      void on_instr_start(const void*, const void* wasm_instruction) {
         const auto pos = offset(wasm_instruction);
         if (finished || functions.empty() || pos < functions.back().wasm_offset ||
             (has_instruction && pos <= last_instruction))
            throw std::invalid_argument("invalid work inventory instruction order");
         const auto op = *static_cast<const uint8_t*>(wasm_instruction);
         if (!valid_instruction(op)) throw std::invalid_argument("unsupported work inventory opcode");
         last_instruction = pos; has_instruction = true;
         // Structural markers do not execute arithmetic. Loop backedges enter
         // the following body; else/end targets enter the following block.
         if (op == 0x03 || op == 0x05 || op == 0x0b) {
            split_next = true;
            return;
         }
         if (op == 0x02) return; // a block's forward target is handled at end
         auto& entries = functions.back().blocks;
         if (split_next) {
            if (blocks >= max_blocks) throw std::length_error("work inventory block bound exceeded");
            entries.push_back({pos, 0}); ++blocks; split_next = false;
         }
         if (entries.back().instructions == std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("work inventory instruction count overflow");
         ++entries.back().instructions;
         // Split after transfer/call boundaries. The original parser handles
         // immediates, labels and validation; no immediate byte is an opcode.
         if (op == 0x00 || op == 0x04 || (op >= 0x0c && op <= 0x11)) split_next = true;
      }

      void on_code_end(const void*, const void* wasm_end) {
         const auto end = offset(wasm_end);
         if (finished || (has_instruction && end <= last_instruction))
            throw std::invalid_argument("invalid work inventory end");
         finished = true;
      }
   };

   const inventory& functions() const {
      static const inventory empty;
      return data_ ? *data_ : empty;
   }
   void set(builder&& source) {
      if ((source.started && !source.finished) || (!source.started && !source.functions.empty()))
         throw std::logic_error("unfinished work inventory");
      auto next = std::make_shared<const inventory>(std::move(source.functions));
      data_ = std::move(next);
   }
   // Metadata addresses WASM source, so moving compiled code changes nothing.
   void relocate(const void*) {}
   void share_from(const work_block_inventory& other) noexcept { data_ = other.data_; }

private:
   std::shared_ptr<const inventory> data_;
};

} // namespace eosio::vm
