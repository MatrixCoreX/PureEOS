#include <eosio/eosio.hpp>
#include <cstring>
#include <memory>

using namespace eosio;

extern "C" __attribute__((eosio_wasm_import)) void set_action_return_value(const void*, uint32_t);

// A real CDT dispatcher/multi_index/allocator fixture, NOT a production token.
// init exercises the serial creation boundary; run updates an existing row.
class [[eosio::contract("parallelcdt")]] parallelcdt : public contract {
public:
   using contract::contract;

   struct [[eosio::table]] record {
      uint64_t key;
      int64_t value;
      uint64_t primary_key() const { return key; }
   };
   using records = multi_index<"rows"_n, record>;

   [[eosio::action]] void init(uint64_t scope, int64_t value, uint64_t bytes, uint64_t nonce) {
      require_auth("alice"_n);
      records table(get_self(), scope);
      table.emplace("alice"_n, [&](auto& row) { row.key = 1; row.value = value; });
   }

   [[eosio::action]] void run(uint64_t scope, int64_t delta, uint64_t bytes, uint64_t nonce) {
      require_auth("alice"_n);
      records table(get_self(), scope);
      const auto& row = table.get(1, "missing balance");
      check(delta >= -1000000 && delta <= 1000000, "delta outside fixture range");
      check(row.value + delta >= 0, "insufficient balance");
      table.modify(row, same_payer, [&](auto& item) { item.value += delta; });
      // Allocation happens AFTER the write: crossing a local worker limit
      // must discard that private write and execute the original transaction.
      check(bytes <= 2 * 1024 * 1024, "fixture allocation too large");
      if (bytes) {
         std::unique_ptr<char[]> source(new char[bytes]), target(new char[bytes]);
         // Exercise allocation size without turning a memory-limit test into
         // millions of interpreted byte comparisons on the SERIAL fallback.
         const auto touched = bytes < 4096 ? bytes : 4096;
         std::memset(source.get(), 0xe7, touched);
         std::memcpy(target.get(), source.get(), touched);
         check(std::memcmp(target.get(), source.get(), touched) == 0, "memory mismatch");
      }
      set_action_return_value(&row.value, sizeof(row.value));
   }
};

EOSIO_DISPATCH(parallelcdt, (init)(run))
