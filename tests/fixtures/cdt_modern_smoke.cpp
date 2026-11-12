#include <eosio/eosio.hpp>
#include <eosio/action.hpp>
#include <cstdlib>
#include <cstring>
using namespace eosio;

// SDK correctness fixture, not a TPS or economic-model benchmark.
class [[eosio::contract("cdtcompat")]] cdtcompat : public contract {
public:
   using contract::contract;
   [[eosio::action]] void boolfirst(bool enabled, name actor) {
      require_auth(actor);
      check(enabled, "expected enabled");
   }
   using bool_action = action_wrapper<"boolfirst"_n, &cdtcompat::boolfirst>;
   [[eosio::action]] void sendbool(name actor) {
      require_auth(actor);
      bool_action(get_self(), {actor, "active"_n}).send(true, actor);
   }
   struct [[eosio::table]] row {
      uint64_t id;
      uint64_t value;
      uint64_t primary_key() const { return id; }
      uint64_t by_value() const { return value; }
   };
   using rows = multi_index<"rows"_n, row,
      indexed_by<"byvalue"_n, const_mem_fun<row, uint64_t, &row::by_value>>>;
   [[eosio::action]] void putrow(name actor, uint64_t key, uint64_t value) {
      require_auth(actor);
      rows table(get_self(), actor.value);
      auto prior = table.find(key);
      if (prior == table.end())
         table.emplace(actor, [&](auto& record) { record.id = key; record.value = value; });
      else table.modify(prior, same_payer, [&](auto& record) { record.value = value; });
   }
   [[eosio::action]] void checkmem(name actor) {
      require_auth(actor);
      auto* first = static_cast<char*>(std::malloc(33));
      check(first != nullptr && (uintptr_t(first) % 16) == 0, "allocation alignment");
      for (unsigned i = 0; i < 33; ++i) first[i] = char(i + 1);
      auto* next = static_cast<char*>(std::realloc(first, 70000));
      check(next != nullptr, "reallocation failed");
      for (unsigned i = 0; i < 33; ++i) check(next[i] == char(i + 1), "reallocation lost bytes");
      auto* shrunk = static_cast<char*>(std::realloc(next, 17));
      check(shrunk != nullptr, "shrink failed");
      for (unsigned i = 0; i < 17; ++i) check(shrunk[i] == char(i + 1), "shrink lost bytes");
      auto* zero = static_cast<char*>(std::calloc(32, 4));
      check(zero != nullptr, "calloc failed");
      for (unsigned i = 0; i < 128; ++i) check(zero[i] == 0, "calloc not zero");
      std::free(first); std::free(shrunk); std::free(zero);
   }
   [[eosio::action]] void badalloc(name actor) {
      require_auth(actor);
      putrow(actor, 9999, 1); // must be undone when the allocator rejects
      volatile size_t count = size_t(-1);
      auto* ptr = std::calloc(count, 2);
      check(ptr != nullptr, "unexpected null allocation");
      std::free(ptr);
   }
   [[eosio::action]] void badvar(name actor) {
      require_auth(actor);
      putrow(actor, 9999, 2);
      const char bytes[] = {char(0x80), char(0x80), char(0x80), char(0x80), char(0x10)};
      datastream<const char*> stream(bytes, sizeof(bytes));
      unsigned_int value;
      stream >> value;
      check(value.value == 0, "unexpected value");
   }
   [[eosio::action]] void badstream(name actor) {
      require_auth(actor);
      putrow(actor, 9999, 3);
      const char bytes[8]{};
      datastream<const char*> stream(bytes, 4);
      stream.skip(5);
   }
   [[eosio::action]] void badvector(name actor) {
      require_auth(actor);
      putrow(actor, 9999, 4);
      const char bytes[] = {char(0x80), char(0x80), char(0x04)};
      datastream<const char*> stream(bytes, sizeof(bytes));
      std::vector<char> value;
      stream >> value;
   }
};
EOSIO_DISPATCH(cdtcompat, (boolfirst)(sendbool)(putrow)(checkmem)(badalloc)(badvar)(badstream)(badvector))
