#include <eosio/eosio.hpp>
#include <eosio/crypto.hpp>
#include <array>
#include <cstring>

using namespace eosio;
extern "C" __attribute__((eosio_wasm_import)) void set_action_return_value(const void*, uint32_t);

// A second real CDT fixture: bounded 1-KiB SHA-256 chain plus persistent state.
// The Python oracle computes the same digest chain independently with hashlib.
class [[eosio::contract("complexhash")]] complexhash : public contract {
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
         const auto digest = sha256(reinterpret_cast<const char*>(payload.data()), sizeof(payload)).extract_as_byte_array();
         std::memcpy(payload.data(), digest.data(), digest.size());
      }
      const auto increment = 1 + (payload[0] & 65535);
      check(before.value <= uint64_t(-1) - increment, "result overflow");
      table.modify(before, same_payer, [&](auto& value) { value.value += increment; });
      set_action_return_value(&before.value, sizeof(before.value));
   }
};

EOSIO_DISPATCH(complexhash, (run))
