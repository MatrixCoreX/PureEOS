#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace eosio::vm {

// Common parser policy: reserve one uniform work unit per original
// non-structural opcode at BASIC-BLOCK ENTRY. A trapping block keeps that
// reservation. Function entry and loop entry/backedges have separate positive
// overhead, including empty functions/loops. This is a VM prototype, not a
// calibrated chain cost schedule (host/native costs are separate).
template<typename Writer, typename Options,
         bool EnabledOption = requires(Options o) { o.enable_execution_work_checkpoints; }>
class work_instrumentation {
public:
   work_instrumentation(Writer&, const Options&) {}
   void before(uint8_t) {}
   void after(uint8_t) {}
   void finish() {}
};

template<typename Writer, typename Options>
class work_instrumentation<Writer,Options,true> {
   using patch_type = decltype(std::declval<Writer&>().emit_execution_work_block());
   Writer& writer_;
   bool enabled_, active_ = false;
   patch_type patch_{};
   uint64_t units_ = 0;
public:
   work_instrumentation(Writer& writer, const Options& options)
      : writer_(writer), enabled_(options.enable_execution_work_checkpoints) {}
   void finish() {
      if (!active_) return;
      writer_.set_execution_work_block_units(patch_,units_);
      active_ = false; units_ = 0;
   }
   void before(uint8_t op) {
      if (!enabled_) return;
      if (op == 0x03 || op == 0x05 || op == 0x0b) { finish(); return; }
      if (op == 0x02) return;
      if (!active_) { patch_ = writer_.emit_execution_work_block(); active_ = true; }
      if (units_ == std::numeric_limits<uint64_t>::max())
         throw std::overflow_error("WASM block work overflow");
      ++units_;
   }
   void after(uint8_t op) {
      if (op == 0x00 || op == 0x04 || (op >= 0x0c && op <= 0x11)) finish();
   }
};

} // namespace eosio::vm
