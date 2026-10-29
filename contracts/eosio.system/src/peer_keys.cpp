#include <eosio.system/eosio.system.hpp>
#include <eosio.system/peer_keys.hpp>
#include <algorithm>
#include <set>
namespace eosiosystem {
namespace {
rotation_context_v1 peer_rotation_context() {
   const uint32_t size = get_rotation_context_v1(nullptr, 0);
   eosio::check(size > 0, "BP peering requires the PureEOS native Savanna context");
   std::vector<char> bytes(size);
   eosio::check(get_rotation_context_v1(bytes.data(), size) == size, "rotation context changed");
   auto context = eosio::unpack<rotation_context_v1>(bytes.data(), bytes.size());
   eosio::check(context.version == 1, "unsupported native rotation context");
   return context;
}
bool current_producer(eosio::name account, const rotation_context_v1& context) {
   return std::any_of(context.producers.begin(), context.producers.end(),
                      [&](const auto& p) { return p.producer_name == account; });
}
}
void peer_keys::regpeerkey(const eosio::name& proposer_finalizer_name, const eosio::public_key& key) {
   const auto account = proposer_finalizer_name;
   require_auth(account);
   eosio::check(key.index() < 2, "WebAuthn keys are not supported for BP peer authentication");
   const auto nonzero = [](const auto& bytes) {
      return std::any_of(bytes.begin(), bytes.end(), [](char c) { return c != 0; });
   };
   eosio::check(key.index() == 0 ? nonzero(std::get<0>(key)) : nonzero(std::get<1>(key)),
                "empty peer key is not allowed");
   producers_table producers(get_self(), get_self().value);
   const auto producer = producers.find(account.value);
   eosio::check((producer != producers.end() && producer->active()) ||
                current_producer(account, peer_rotation_context()),
                "peer key owner must be a registered active producer or a current native producer");
   peer_keys_table keys(get_self(), get_self().value);
   const auto existing = keys.find(account.value);
   if (existing == keys.end()) {
      keys.emplace(account, [&](auto& row) { row.account = account; row.data = peer_key::v0_data{key}; });
   } else {
      eosio::check(existing->get_public_key() != std::optional<eosio::public_key>{key},
                   "provided peer key is already registered");
      keys.modify(existing, eosio::same_payer, [&](auto& row) { row.data = peer_key::v0_data{key}; });
   }
}
void peer_keys::delpeerkey(const eosio::name& proposer_finalizer_name, const eosio::public_key& key) {
   const auto account = proposer_finalizer_name;
   require_auth(account);
   peer_keys_table keys(get_self(), get_self().value);
   const auto existing = keys.find(account.value);
   eosio::check(existing != keys.end(), "peer key not found");
   eosio::check(existing->get_public_key() == std::optional<eosio::public_key>{key},
                "current peer key does not match the key to delete");
   keys.erase(existing);
}
peer_keys::getpeerkeys_res_t peer_keys::getpeerkeys() {
   peer_keys_table keys(get_self(), get_self().value);
   producers_table producers(get_self(), get_self().value);
   finalizers_table finalizers(get_self(), get_self().value);
   rotation_candidates candidates(get_self(), get_self().value);
   rotation_singleton rotation(get_self(), get_self().value);
   std::set<uint64_t> included;
   getpeerkeys_res_t result;
   const auto append = [&](eosio::name account) {
      if (!account.value || !included.insert(account.value).second) return;
      const auto key = keys.find(account.value);
      result.push_back(peerkeys_t{account, key == keys.end() ? std::optional<eosio::public_key>{} : key->get_public_key()});
   };
   // Current native authorities remain reachable throughout a handoff, even
   // when votes have moved. This action never changes either consensus policy.
   const auto context = peer_rotation_context();
   for (const auto& producer : context.producers) append(producer.producer_name);
   uint32_t ranked_limit = 210;
   if (rotation.exists()) {
      const auto state = rotation.get();
      ranked_limit = std::min<uint32_t>(ranked_limit, state.capacity);
      if (state.stage != 0) {
         for (const auto account : state.target) {
            const auto producer = producers.find(account.value);
            const auto finalizer = finalizers.find(account.value);
            const auto candidate = candidates.find(account.value);
            if (producer != producers.end() && producer->active() &&
                finalizer != finalizers.end() && !finalizer->active_key_binary.empty() &&
                (candidate == candidates.end() || !candidate->banned)) append(account);
         }
      }
   }
   // Use the existing PureEOS integer ranking and ban records. Bound discovery
   // reserve work independently of future rotation-pool growth; current and
   // pending authorities above are always included. Banned ranks are not backfilled.
   const auto ranked = candidates.get_index<"byrank"_n>();
   const uint128_t invalid_rank_floor = uint128_t(uint64_t(-1)) << 64;
   uint32_t inspected = 0;
   for (auto it = ranked.begin(); it != ranked.end() && inspected < ranked_limit &&
        it->rank < invalid_rank_floor; ++it, ++inspected) {
      if (it->banned) continue;
      const auto producer = producers.find(it->owner.value);
      const auto finalizer = finalizers.find(it->owner.value);
      if (producer != producers.end() && producer->active() &&
          finalizer != finalizers.end() && !finalizer->active_key_binary.empty()) append(it->owner);
   }
   return result;
}
}
