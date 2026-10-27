#include <eosio/eosio.hpp>

using namespace eosio;
extern "C" __attribute__((eosio_wasm_import)) void set_action_return_value(const void*, uint32_t);

// Test-only deployed application: real authorization and persistent writes.
class [[eosio::contract("statebench")]] statebench : public contract {
public:
   using contract::contract;
   struct [[eosio::table]] row {
      uint64_t value;
      uint64_t primary_key() const { return 1; }
   };
   using rows = multi_index<"rows"_n, row>;
   [[eosio::action]] void run(uint64_t scope, uint64_t delta, uint64_t work, uint64_t nonce) {
      require_auth("alice"_n);
      check(work == 0 && delta > 0, "invalid state benchmark input");
      rows table(get_self(), scope);
      const auto& before = table.get(1, "missing preallocated row");
      check(before.value <= uint64_t(-1) - delta, "counter overflow");
      table.modify(before, same_payer, [&](auto& value) { value.value += delta; });
      set_action_return_value(&before.value, sizeof(before.value));
   }
};
EOSIO_DISPATCH(statebench, (run))
