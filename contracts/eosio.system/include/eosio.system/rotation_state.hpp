#pragma once
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>
#include <eosio/producer_schedule.hpp>
#include <eosio/instant_finality.hpp>
#include <eosio/binary_extension.hpp>
#include <array>
#include <eosio.system/rotation_growth.hpp>

extern "C" {
   __attribute__((eosio_wasm_import)) uint32_t get_rotation_context_v1(char*, uint32_t);
}

namespace eosiosystem {
struct rotation_context_v1 {
   uint32_t version, block_num, slot, final_block_num, final_slot;
   uint32_t proposer_version, finalizer_generation, latest_finalizer_generation;
   bool transitioning;
   std::vector<eosio::producer_authority> producers;
   uint64_t threshold;
   std::vector<eosio::finalizer_authority> finalizers;
   EOSLIB_SERIALIZE(rotation_context_v1,
      (version)(block_num)(slot)(final_block_num)(final_slot)(proposer_version)
      (finalizer_generation)(latest_finalizer_generation)(transitioning)(producers)(threshold)(finalizers))
};

struct rotation_window {
   eosio::name owner;
   uint64_t start = 0;
   uint32_t policy_version = 0;
   bool produced = false;
   EOSLIB_SERIALIZE(rotation_window, (owner)(start)(policy_version)(produced))
};

// Candidate-funded fixed-size records survive unregister/key changes forever.
// previous_rank is an immutable scan index while a snapshot is building.
// Changed rows are synchronized in bounded batches before the next snapshot.
struct [[eosio::table("rotcands"), eosio::contract("eosio.system")]] rotation_candidate {
   eosio::name owner;
   uint128_t rank = ~uint128_t(0), previous_rank = ~uint128_t(0);
   uint64_t frozen_epoch = 0, seen_epoch = 0, ticket = 0;
   uint64_t penalty_epoch = 0;
   uint8_t zero_windows = 0;
   bool banned = false;
   std::array<uint64_t, 3> evidence_slots{};
   std::array<uint32_t, 3> evidence_policies{};
   uint32_t ban_final_block = 0;
   uint64_t primary_key() const { return owner.value; }
   uint128_t by_rank() const { return rank; }
   uint128_t by_previous() const { return previous_rank; }
   uint64_t by_ticket() const { return ticket; }
   uint64_t by_dirty() const { return frozen_epoch; }
   EOSLIB_SERIALIZE(rotation_candidate, (owner)(rank)(previous_rank)(frozen_epoch)(seen_epoch)(ticket)
      (penalty_epoch)(zero_windows)(banned)(evidence_slots)(evidence_policies)(ban_final_block))
};
using rotation_candidates = eosio::multi_index<"rotcands"_n, rotation_candidate,
   eosio::indexed_by<"byrank"_n, eosio::const_mem_fun<rotation_candidate, uint128_t, &rotation_candidate::by_rank>>,
   eosio::indexed_by<"byprevious"_n, eosio::const_mem_fun<rotation_candidate, uint128_t, &rotation_candidate::by_previous>>,
   eosio::indexed_by<"byticket"_n, eosio::const_mem_fun<rotation_candidate, uint64_t, &rotation_candidate::by_ticket>>,
   eosio::indexed_by<"bydirty"_n, eosio::const_mem_fun<rotation_candidate, uint64_t, &rotation_candidate::by_dirty>>>;

// Fresh-chain v2 schema. stage: 0 idle, 1 submitted, 2 observing, 3 key-only.
// Submitted with incoming==0 and outgoing!=0 is a banned-member eviction:
// complete only after BOTH native policies match; no entrant observation.
struct [[eosio::table("rotation"), eosio::contract("eosio.system")]] rotation_info {
   uint32_t version = 2;
   uint8_t stage = 0;
   std::vector<eosio::name> active; // tenure order, not physical proposer order
   std::vector<eosio::name> target;
   eosio::name incoming, outgoing;
   uint32_t proposer_version = 0, finalizer_generation = 0;
   eosio::checksum256 proposer_digest, finalizer_digest;
   uint64_t next_attempt_slot = 0;
   uint64_t observation_start = 0, observation_end = 0;
   uint32_t produced_mask = 0;
   uint32_t observation_block = 0;
   uint64_t completed = 0;
   uint64_t stalled_windows = 0; // diagnostic only; NOT a blacklist
   uint64_t growth_start_slot = 0;
   uint32_t capacity = 210, snapshot_capacity = 210, ranked_count = 0;
   uint64_t snapshot_epoch = 0, published_epoch = 0, queue_tail = 0, scan_end_ticket = 0;
   bool ranking = false, rank_started = false, selecting = false;
   uint128_t rank_cursor = 0;
   uint64_t penalty_epoch = 1, last_slot = 0, window_start = 0;
   bool penalty_paused = true, window_produced = false;
   uint32_t window_policy = 0;
   std::vector<eosio::name> window_members;
   std::vector<rotation_window> pending_windows;
   uint64_t failed_admissions = 0, total_banned = 0;
   uint8_t blocked = 0; // 0 none, 1 global/finality pause, 2 no replacement, 3 zero-output observation
   // Under-21 admission timeout: replace this seat, but never blacklist it.
   // A trailing extension keeps existing v2 rows readable across an upgrade.
   eosio::binary_extension<eosio::name> retry_outgoing;
   // CDT's serialization macro supports at most 32 fields. Keep ABI field order
   // explicit rather than silently dropping the window/penalty tail.
   template<typename Stream> friend Stream& operator<<(Stream& ds, const rotation_info& t) {
      return ds << t.version << t.stage << t.active << t.target << t.incoming << t.outgoing
         << t.proposer_version << t.finalizer_generation << t.proposer_digest << t.finalizer_digest
         << t.next_attempt_slot << t.observation_start << t.observation_end << t.produced_mask
         << t.observation_block << t.completed << t.stalled_windows
         << t.growth_start_slot << t.capacity << t.snapshot_capacity << t.ranked_count << t.snapshot_epoch << t.published_epoch
         << t.queue_tail << t.scan_end_ticket << t.ranking << t.rank_started << t.selecting << t.rank_cursor
         << t.penalty_epoch << t.last_slot << t.window_start << t.penalty_paused << t.window_produced << t.window_policy
         << t.window_members << t.pending_windows << t.failed_admissions << t.total_banned << t.blocked
         << t.retry_outgoing;
   }
   template<typename Stream> friend Stream& operator>>(Stream& ds, rotation_info& t) {
      return ds >> t.version >> t.stage >> t.active >> t.target >> t.incoming >> t.outgoing
         >> t.proposer_version >> t.finalizer_generation >> t.proposer_digest >> t.finalizer_digest
         >> t.next_attempt_slot >> t.observation_start >> t.observation_end >> t.produced_mask
         >> t.observation_block >> t.completed >> t.stalled_windows
         >> t.growth_start_slot >> t.capacity >> t.snapshot_capacity >> t.ranked_count >> t.snapshot_epoch >> t.published_epoch
         >> t.queue_tail >> t.scan_end_ticket >> t.ranking >> t.rank_started >> t.selecting >> t.rank_cursor
         >> t.penalty_epoch >> t.last_slot >> t.window_start >> t.penalty_paused >> t.window_produced >> t.window_policy
         >> t.window_members >> t.pending_windows >> t.failed_admissions >> t.total_banned >> t.blocked
         >> t.retry_outgoing;
   }
};
using rotation_singleton = eosio::singleton<"rotation"_n, rotation_info>;
}
