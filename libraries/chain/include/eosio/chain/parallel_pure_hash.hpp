#pragma once

#include <eosio/chain/config.hpp>
#include <span>

namespace eosio::chain::experimental {

// Pure byte computation, with no ledger/context capability or retained state.
// Keep the ORIGINAL transaction_context::hash_with_checktime chunking and FC
// encoder. The additional entry/final checks are private-attempt cancellation:
// a cancelled digest is never published. The caller borrows its existing
// absolute deadline; this helper must never create/renew a time allowance.
template<class Digest, class Checkpoint>
Digest isolated_hash_with_checktime(std::span<const char> data, Checkpoint&& checkpoint) {
   constexpr size_t chunk = config::hashing_checktime_block_size;
   static_assert(chunk > 0);
   checkpoint();
   typename Digest::encoder encoder;
   while (data.size() > chunk) {
      encoder.write(data.data(), chunk);
      data = data.subspan(chunk);
      checkpoint();
   }
   encoder.write(data.data(), data.size());
   auto result = encoder.result();
   checkpoint();
   return result;
}

} // namespace eosio::chain::experimental
