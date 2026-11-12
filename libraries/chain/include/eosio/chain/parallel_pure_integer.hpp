#pragma once

#include <bit>
#include <cstdint>
#include <optional>

namespace eosio::chain::experimental {

enum class isolated_integer_operation : uint8_t {
   arithmetic_left, arithmetic_right, logical_left, logical_right,
   signed_divide, unsigned_divide, multiply, signed_modulo, unsigned_modulo
};

using isolated_integer_bits = unsigned __int128;

constexpr isolated_integer_bits isolated_integer_words(uint64_t low, uint64_t high) {
   return (isolated_integer_bits{high} << 64) | low;
}

// Pure arithmetic only. An absent result asks the caller to discard the WHOLE
// private attempt; it is not a new consensus exception or numeric rule. Keep
// the original FC zero result for logical/left shifts >=128, but do not enter
// the original undefined oversized arithmetic-right-shift path privately.
inline std::optional<isolated_integer_bits> isolated_integer128(isolated_integer_operation operation,
      isolated_integer_bits a, isolated_integer_bits b = 0, uint32_t shift = 0) {
   using op = isolated_integer_operation;
   constexpr auto all = ~isolated_integer_bits{0};
   constexpr auto minimum = isolated_integer_bits{1} << 127;
   switch (operation) {
      case op::arithmetic_left:
      case op::logical_left: return shift < 128 ? a << shift : 0;
      case op::logical_right: return shift < 128 ? a >> shift : 0;
      case op::arithmetic_right:
         if (shift >= 128) return {};
         if (!shift) return a;
         return (a >> shift) | ((a & minimum) ? all << (128 - shift) : 0);
      case op::multiply: return a * b; // original unsigned modulo-2^128 product
      case op::unsigned_divide: return b ? std::optional<isolated_integer_bits>{a / b} : std::nullopt;
      case op::unsigned_modulo: return b ? std::optional<isolated_integer_bits>{a % b} : std::nullopt;
      case op::signed_divide:
      case op::signed_modulo: {
         if (!b) return {};
         // Preserve the original explicit overflow exceptions to arithmetic:
         // min / -1 returns min; min % -1 returns zero, never native UB.
         if (a == minimum && b == all) return operation == op::signed_divide ? a : 0;
         const auto lhs = std::bit_cast<__int128>(a), rhs = std::bit_cast<__int128>(b);
         const __int128 result = operation == op::signed_divide ? lhs / rhs : lhs % rhs;
         return std::bit_cast<isolated_integer_bits>(result);
      }
   }
   return {}; // invalid local operation never publishes a value
}

} // namespace eosio::chain::experimental
