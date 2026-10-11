#include <eosio/chain/parallel_inline_action.hpp>
#include <fc/io/raw.hpp>

namespace eosio::chain::experimental {
namespace {
bool read_length(fc::datastream<const char*>& stream, uint32_t& value) {
   value = 0;
   for (unsigned index = 0; index < 5; ++index) {
      if (!stream.remaining()) return false;
      char raw;
      stream.get(raw);
      const auto byte = static_cast<uint8_t>(raw);
      // The fifth byte cannot carry bits beyond uint32_t or continuation.
      if (index == 4 && (byte & 0xf0)) return false;
      value |= uint32_t(byte & 0x7f) << (index * 7);
      if (!(byte & 0x80)) return index == 0 || (byte & 0x7f) != 0;
   }
   return false;
}
} // namespace

std::optional<isolated_inline_action> decode_isolated_inline_action(
      std::span<const char> packed, uint32_t protocol_max_bytes) {
   if (packed.size() >= protocol_max_bytes || packed.size() > isolated_inline_action::max_packed_bytes ||
       packed.size() < 2 * sizeof(uint64_t) + 2) return {};
   fc::datastream<const char*> stream(packed.data(), packed.size());
   action decoded;
   // Fixed-width names use the ORIGINAL FC codec; the checked remaining byte
   // count above ensures these reads cannot run past the supplied span.
   fc::raw::unpack(stream, decoded.account);
   fc::raw::unpack(stream, decoded.name);
   uint32_t count;
   if (!read_length(stream, count) || count > isolated_inline_action::max_authorizations ||
       count > stream.remaining() / (2 * sizeof(uint64_t))) return {};
   decoded.authorization.reserve(count);
   for (uint32_t i = 0; i < count; ++i) {
      permission_level permission;
      fc::raw::unpack(stream, permission);
      decoded.authorization.push_back(permission);
   }
   uint32_t bytes;
   if (!read_length(stream, bytes) || bytes > stream.remaining()) return {};
   const auto offset = packed.size() - stream.remaining();
   decoded.data.assign(packed.begin() + offset, packed.begin() + offset + bytes);
   // Do NOT require EOF or repack decoded: original unpack permits a trailing
   // suffix. Its bytes still affect send_inline's original size check.
   return isolated_inline_action{{packed.begin(), packed.end()}, std::move(decoded), offset + bytes};
}

} // namespace eosio::chain::experimental
