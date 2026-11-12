#include <eosio/eosio.hpp>
#include <array>
#include <cstring>

using namespace eosio;
extern "C" __attribute__((eosio_wasm_import)) void set_action_return_value(const void*, uint32_t);
extern "C" __attribute__((eosio_wasm_import)) void sha256(const char*, uint32_t, void*);

// Diagnostic companion to complex_hash_benchmark.cpp, NOT its replacement.
// The full CDT crypto wrapper retains recover_key/assert_recover_key imports
// that this private executor cannot link, even on an uncalled path. Keep that
// failing fixture. This one isolates the supported SHA-256 intrinsic without
// changing the algorithm, per-transaction workload, or independent oracle.
class [[eosio::contract("complexhashc")]] complexhashc : public contract {
public:
   using contract::contract;
   struct [[eosio::table]] row {
      uint64_t value;
      uint64_t primary_key() const { return 1; }
   };
   using rows = multi_index<"rows"_n, row>;
   [[eosio::action]] void run(uint64_t scope, uint64_t seed, uint64_t iterations, uint64_t nonce) {
      require_auth("alice"_n);
      check(iterations <= 100000, "bounded benchmark work only");
      rows table(get_self(), scope);
      const auto& before = table.get(1, "missing preallocated job row");
      std::array<uint64_t, 128> payload;
      for (uint64_t i = 0; i < payload.size(); ++i) payload[i] = seed + i;
      for (uint64_t i = 0; i < iterations; ++i) {
         alignas(16) std::array<uint8_t, 32> digest;
         ::sha256(reinterpret_cast<const char*>(payload.data()), sizeof(payload), digest.data());
         std::memcpy(payload.data(), digest.data(), digest.size());
      }
      const auto increment = 1 + (payload[0] & 65535);
      check(before.value <= uint64_t(-1) - increment, "result overflow");
      table.modify(before, same_payer, [&](auto& value) { value.value += increment; });
      set_action_return_value(&before.value, sizeof(before.value));
   }
};

EOSIO_DISPATCH(complexhashc, (run))
