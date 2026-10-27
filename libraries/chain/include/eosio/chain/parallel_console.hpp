#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eosio::chain::experimental {

// Owned arguments, NOT formatted text. Only the canonical owner calls the
// original console intrinsics, in journal order. No VM pointer escapes a host.
enum class isolated_console_operation : uint8_t {
   prints, prints_l, printi, printui, printi128, printui128, printsf, printdf, printqf, printn, printhex
};
struct isolated_console_event {
   isolated_console_operation operation = isolated_console_operation::prints_l;
   std::array<uint64_t, 2> bits{}; // exact low/high numeric bits, not native FP formatting
   std::vector<char> bytes;       // prints includes its sole trailing NUL
   size_t after_writes = 0;
   bool operator==(const isolated_console_event&) const = default;
};

// Bounds span ALL roots, receivers and sync children of one attempt. These
// local caps select whole-attempt fallback; they are not consensus limits.
class isolated_console_budget {
public:
   static constexpr size_t max_event_bytes = 64 * 1024;
   static constexpr size_t max_total_bytes = 256 * 1024;
   static constexpr size_t max_events = 1024;
   bool consume(size_t bytes) {
      if (events_ >= max_events || bytes > max_event_bytes || bytes > max_total_bytes - bytes_) return false;
      ++events_; bytes_ += bytes;
      return true;
   }
   // Validate inert/unused fields too; never interpret an unknown enum as text.
   static std::optional<size_t> argument_bytes(const isolated_console_event& event) {
      using op = isolated_console_operation;
      if (event.bytes.size() > max_event_bytes) return {};
      switch (event.operation) {
         case op::prints:
            if (event.bytes.empty() || event.bytes.back() != '\0' ||
                std::find(event.bytes.begin(), event.bytes.end() - 1, '\0') != event.bytes.end() - 1) return {};
            [[fallthrough]];
         case op::prints_l: case op::printhex:
            if (event.bits != std::array<uint64_t, 2>{}) return {};
            return event.bytes.size();
         case op::printsf:
            if (event.bits[0] > UINT32_MAX) return {};
            [[fallthrough]];
         case op::printi: case op::printui: case op::printdf: case op::printn:
            if (event.bits[1] || !event.bytes.empty()) return {};
            return event.operation == op::printsf ? 4 : 8;
         case op::printi128: case op::printui128: case op::printqf:
            if (!event.bytes.empty()) return {};
            return 16;
      }
      return {};
   }
   bool accept(const isolated_console_event& event, size_t writes, bool enabled) {
      if (!enabled || event.after_writes != writes) return false;
      const auto size = argument_bytes(event);
      return size && consume(*size);
   }
private:
   size_t bytes_ = 0, events_ = 0;
};

// Private sync markers count preceding console EVENTS in that frame (including
// empty prints), never formatted bytes. The frozen validator checks this count;
// canonical replay checks the original byte markers at the corresponding call.
inline bool accept_isolated_console_marker(std::span<const uint32_t> markers, size_t& next,
                                           uint32_t console_events, bool enabled) {
   if (!enabled) return true;
   if (next >= markers.size() || markers[next] != console_events) return false;
   ++next;
   return true;
}

} // namespace eosio::chain::experimental
