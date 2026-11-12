#include <eosio/eosio.hpp>
#include <eosio/action.hpp>
using namespace eosio;

// Compile-only compatibility fixture, not a TPS or economic-model benchmark.
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
};
EOSIO_DISPATCH(cdtcompat, (boolfirst)(sendbool)(putrow))
