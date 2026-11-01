#include <eosio.system/eosio.system.hpp>
#include <eosio.system/upgrade_governance.hpp>
#include <algorithm>
#include <cmath>

namespace eosiosystem {
void system_contract::refreshgov() {
#ifdef PUREEOS_SINGLE_CHOICE_VOTING
   require_direct_vote_policy();
#endif
   const name developer{get_upgrade_developer()};
   check(developer.value != 0, "developer upgrade governance must first be initialized");
   // Read-only ranking selection: positive votes, active signing authority and
   // a registered active BLS key. Equal vote totals use the name primary key.
   std::vector<uint64_t> voters;
   voters.reserve(210);
#ifdef PUREEOS_SEQUENTIAL_ROTATION
   // The production profile already maintains a LIVE qualification-filtered
   // rank on every vote, registration and BLS-key mutation. Use that index,
   // not the frozen rotation snapshot, so arbitrarily many incomplete producer
   // registrations cannot consume scan work or prevent an upgrade review.
   rotation_singleton state(get_self(), get_self().value);
   check(state.exists() && state.get().version == 2, "rotation governance index is not initialized");
   rotation_candidates candidates(get_self(), get_self().value);
   const auto ranked = candidates.get_index<"byrank"_n>();
   constexpr uint128_t invalid_rank_floor = uint128_t(uint64_t(-1)) << 64;
   // New bans have invalid keys. Bound any legacy banned prefix until the
   // rotation scanner has normalized it; never publish a truncated electorate.
   constexpr uint32_t max_scanned_candidates = 1024;
   uint32_t scanned = 0;
   for (auto it = ranked.begin(); it != ranked.end() && it->rank < invalid_rank_floor && voters.size() < 210; ++it) {
      check(scanned++ < max_scanned_candidates, "governance candidate scan limit exceeded");
      if (!it->banned && it->owner != developer) voters.push_back(it->owner.value);
   }
#else
   const auto ranked = _producers.get_index<"prototalvote"_n>();
   // Bound *visited rows*, not just eligible voters. Missing BLS keys must not
   // allow an unbounded scan. Fail closed instead of publishing a truncated
   // electorate with an artificially lower veto threshold.
   constexpr uint32_t max_scanned_candidates = 1024;
   uint32_t scanned = 0;
   for (auto it = ranked.begin(); it != ranked.end() && voters.size() < 210; ++it) {
      check(scanned++ < max_scanned_candidates, "governance candidate scan limit exceeded");
      if (!it->active() || it->total_votes <= 0) break;
#ifdef PUREEOS_SINGLE_CHOICE_VOTING
      if (it->owner == developer) continue;
#else
      if (!std::isfinite(it->total_votes) || it->owner == developer) continue;
#endif
      const auto finalizer = _finalizers.find(it->owner.value);
      if (finalizer == _finalizers.end() || finalizer->finalizer_key_count == 0 || finalizer->active_key_binary.size() != 96) continue;
      const bool usable_authority = std::visit([](const auto& authority) {
         uint64_t total = 0;
         for (const auto& key : authority.keys) {
            if (key.key == eosio::public_key{}) return false;
            total += key.weight;
         }
         return authority.threshold > 0 && total >= authority.threshold;
      }, it->get_producer_authority());
      if (usable_authority) voters.push_back(it->owner.value);
   }
#endif
   if (voters.empty()) {
      // Before the first eligible registration, the native genesis producer is
      // the sole node. Never silently fall back to the active 21 on a mature chain.
      const auto active = eosio::get_active_producers();
      check(active.size() == 1 && active.front() == "eosio"_n,
            "no eligible candidates: cannot publish an upgrade with an empty electorate");
      voters.push_back("eosio"_n.value);
   }
   std::sort(voters.begin(), voters.end());
   set_upgrade_voters(reinterpret_cast<const char*>(voters.data()), voters.size() * sizeof(uint64_t));
}
} // namespace eosiosystem
