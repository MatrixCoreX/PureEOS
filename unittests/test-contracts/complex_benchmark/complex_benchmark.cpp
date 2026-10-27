#include <eosio/eosio.hpp>
#include <eosio/action.hpp>
#include <tuple>

using namespace eosio;
extern "C" __attribute__((eosio_wasm_import)) void set_action_return_value(const void*, uint32_t);

// Local benchmark only. Persistent data is disjoint BETWEEN transactions;
// actions WITHIN one transaction intentionally depend on their own prior writes.
// This is not a token, exchange, or production economic contract.
class [[eosio::contract("complexbench")]] complexbench : public contract {
public:
   using contract::contract;
   struct [[eosio::table]] row {
      uint64_t value;
      uint64_t primary_key() const { return 1; }
   };
   using rows = multi_index<"rows"_n, row>;

   [[eosio::action]] void run(uint64_t scope, uint64_t seed, uint64_t iterations, uint64_t nonce) {
      require_auth("alice"_n);
      check(iterations <= 10000000, "bounded benchmark work only");
      rows table(get_self(), scope);
      const auto& before = table.get(1, "missing preallocated job row");
      auto checksum = seed;
      for (uint64_t i = 0; i < iterations; ++i) {
         checksum ^= checksum >> 12;
         checksum ^= checksum << 25;
         checksum ^= checksum >> 27;
         checksum *= uint64_t{2685821657736338717};
      }
      const auto increment = 1 + (checksum & 65535);
      check(before.value <= uint64_t(-1) - increment, "result overflow");
      table.modify(before, same_payer, [&](auto& value) { value.value += increment; });
      set_action_return_value(&before.value, sizeof(before.value));
   }

   [[eosio::action]] void batch(uint64_t scope, uint64_t seed, uint64_t iterations, uint64_t nonce) {
      require_auth("alice"_n);
      for (uint64_t i = 0; i < 100; ++i)
         action(permission_level{"alice"_n, "active"_n}, get_self(), "run"_n,
                std::make_tuple(scope, seed, iterations, nonce + i)).send();
   }
};

EOSIO_DISPATCH(complexbench, (run)(batch))
