#pragma once

#include <eosio/chain/action.hpp>
#include <optional>
#include <span>

namespace eosio::chain::experimental {

// Private inline preparation ONLY. This is not permission to send an action:
// the canonical original send_inline/execute_inline path still owns target,
// permission, authorization, resource and recursion checks at the send point.
struct isolated_inline_action {
   static constexpr size_t max_packed_bytes = 64 * 1024;
   static constexpr size_t max_authorizations = 64;
   std::vector<char> packed; // exact original bytes, including any trailing bytes
   action decoded;
   size_t decoded_bytes = 0;
};

// The original host uses packed.size() < protocol_max_bytes, NOT <=. Local
// bounds and noncanonical/malformed encodings select whole-attempt fallback;
// they do not establish a new consensus rejection rule. Never allocate from an
// unchecked vector length. Allocation failure may propagate to the executor's
// ordinary whole-attempt fallback guard; no partial result escapes.
std::optional<isolated_inline_action> decode_isolated_inline_action(
   std::span<const char> packed, uint32_t protocol_max_bytes);

} // namespace eosio::chain::experimental
