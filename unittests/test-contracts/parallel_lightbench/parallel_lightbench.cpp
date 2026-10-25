#include <eosio/eosio.hpp>

using namespace eosio;
extern "C" __attribute__((eosio_wasm_import)) void set_action_return_value(const void*, uint32_t);

// Benchmark only. Existing same-sized rows, one distinct key per transaction.
// Shared authorization/table metadata are reads; resource settlement remains
// canonical. No token transfer, aggregate counter, sleep or arithmetic burn.
class [[eosio::contract("lightbench")]] lightbench : public contract {
public:
   using contract::contract;
   struct [[eosio::table]] row {
      uint64_t id;
      uint64_t value;
      uint64_t primary_key() const { return id; }
   };
   using rows = multi_index<"rows"_n, row>;

   [[eosio::action]] void init(uint64_t first, uint64_t count) {
      require_auth("alice"_n);
      check(first > 0 && count > 0 && count <= 1024 && first <= 131072 && count <= 131073 - first,
            "bounded preallocation only");
      rows table(get_self(), get_self().value);
      for (uint64_t id = first; id < first + count; ++id)
         table.emplace(get_self(), [&](auto& value) { value.id = id; value.value = 10; });
   }

   [[eosio::action]] void run(uint64_t key, uint64_t delta, uint64_t work, uint64_t nonce) {
      require_auth("alice"_n);
      check(key > 0 && key <= 131072 && delta == 1 && work == 0, "invalid light workload");
      rows table(get_self(), get_self().value);
      const auto& before = table.get(key, "missing preallocated row");
      check(before.value < uint64_t(-1), "counter overflow");
      table.modify(before, same_payer, [&](auto& value) { ++value.value; });
      set_action_return_value(&before.value, sizeof(before.value));
   }

   // Whole-corpus-independent profile: one distinct authorized caller AND row
   // per transaction. The ordinary chain still settles real CPU/NET/RAM.
   [[eosio::action]] void step(name actor, uint64_t key, uint64_t nonce) {
      require_auth(actor);
      check(key > 0 && key <= 131072, "invalid independent key");
      rows table(get_self(), get_self().value);
      const auto& before = table.get(key, "missing preallocated row");
      check(before.value < uint64_t(-1), "counter overflow");
      table.modify(before, same_payer, [&](auto& value) { ++value.value; });
      set_action_return_value(&before.value, sizeof(before.value));
   }
};
EOSIO_DISPATCH(lightbench, (init)(run)(step))
