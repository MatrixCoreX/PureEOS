#include <eosio.system/eosio.system.hpp>
#include <eosio.system/rotation.hpp>
#include <eosio.system/upgrade_governance.hpp>
#include <algorithm>

namespace eosiosystem {
namespace {
constexpr uint128_t invalid_rank_floor = uint128_t(uint64_t(-1)) << 64;
uint64_t next_ticket(rotation_info& s) {
   check(s.queue_tail != uint64_t(-1), "rotation ticket exhausted");
   return ++s.queue_tail;
}

bool sync_ranking(rotation_candidates& candidates) {
   auto dirty = candidates.get_index<"bydirty"_n>();
   for (uint32_t work = 0; work < rotation::rank_work_budget; ++work) {
      auto c = dirty.lower_bound(1);
      if (c == dirty.end()) return work == 0;
      dirty.modify(c, same_payer, [](auto& r) {
         r.previous_rank = r.rank;
         r.frozen_epoch = 0;
      });
   }
   return false; // begin the next snapshot on a later block: total rank work <= 16
}

void advance_ranking(rotation_info& s, rotation_candidates& candidates) {
   auto previous = candidates.get_index<"byprevious"_n>();
   for (uint32_t work = 0; work < rotation::rank_work_budget; ++work) {
      auto b = s.rank_started ? previous.upper_bound(s.rank_cursor) : previous.begin();
      const auto key = b == previous.end() ? ~uint128_t(0) : b->previous_rank;
      if (s.ranked_count >= s.snapshot_capacity || key >= invalid_rank_floor) {
         s.ranking = false;
         s.selecting = true;
         s.published_epoch = s.snapshot_epoch;
         s.scan_end_ticket = s.queue_tail;
         return;
      }
      s.rank_cursor = key;
      s.rank_started = true;
      auto c = candidates.find(b->owner.value);
      ++s.ranked_count; // banned accounts still occupy their rank; never backfill C+1
      candidates.modify(c, same_payer, [&](auto& r) {
         if (!r.banned && (r.ticket == 0 || r.seen_epoch != s.published_epoch)) r.ticket = next_ticket(s);
         r.seen_epoch = s.snapshot_epoch;
      });
   }
}

void pause_penalties(rotation_info& s) {
   if (!s.penalty_paused) {
      check(s.penalty_epoch != uint64_t(-1), "penalty epoch exhausted");
      ++s.penalty_epoch;
   }
   s.penalty_paused = true;
   s.window_start = 0;
   s.window_members.clear();
   s.pending_windows.clear();
   s.blocked = 1;
}

void observe_windows(rotation_info& s, rotation_candidates& candidates,
                     const rotation_context_v1& ctx, name producer, bool full_size, bool coherent) {
   const bool gap = s.last_slot && uint64_t(ctx.slot) - s.last_slot > rotation::pause_guard_slots;
   s.last_slot = ctx.slot;
   if (!full_size || gap || uint64_t(ctx.slot) - ctx.final_slot > rotation::pause_guard_slots) {
      pause_penalties(s);
      return;
   }
   if (!coherent) {
      // During a 21->21 joint-policy handoff discard the partial window, not
      // already completed evidence or previous personal-window streaks.
      s.window_start = 0;
      s.window_members.clear();
      return;
   }
   s.penalty_paused = false;
   std::vector<name> members;
   for (const auto& p : ctx.producers) members.push_back(p.producer_name);
   if (members != s.window_members || !s.window_start) {
      // Preserve finalized streaks across a 21->21 handoff, but discard only
      // the incomplete old window. A key-only change has the SAME slot owners.
      s.window_members = members;
      s.window_start = (uint64_t(ctx.slot) / rotation::producer_slots + 1) * rotation::producer_slots;
      s.window_produced = false;
      s.window_policy = ctx.proposer_version;
   }
   while (uint64_t(ctx.slot) >= s.window_start + rotation::producer_slots) {
      if (s.pending_windows.size() >= rotation::pending_window_limit) {
         pause_penalties(s); // bounded fail-safe; no synthetic punishment
         return;
      }
      const auto owner = members[(s.window_start / rotation::producer_slots) % members.size()];
      s.pending_windows.push_back({owner, s.window_start, s.window_policy, s.window_produced});
      s.window_start += rotation::producer_slots;
      s.window_produced = false;
      s.window_policy = ctx.proposer_version;
   }
   if (uint64_t(ctx.slot) >= s.window_start) {
      check(producer == members[(ctx.slot / rotation::producer_slots) % members.size()], "invalid window producer");
      s.window_produced = true;
      s.window_policy = ctx.proposer_version;
   }
   size_t consumed = 0;
   for (const auto& w : s.pending_windows) {
      if (consumed == rotation::window_settle_budget || uint64_t(ctx.final_slot) < w.start + rotation::producer_slots - 1) break;
      ++consumed;
      auto c = candidates.find(w.owner.value);
      if (c == candidates.end() || c->banned) continue;
      candidates.modify(c, same_payer, [&](auto& r) {
         if (r.penalty_epoch != s.penalty_epoch) {
            r.penalty_epoch = s.penalty_epoch;
            r.zero_windows = 0;
         }
         if (w.produced) {
            r.zero_windows = 0;
            r.evidence_slots = {};
            r.evidence_policies = {};
         } else {
            check(r.zero_windows < 3, "invalid zero-window count");
            r.evidence_slots[r.zero_windows] = w.start;
            r.evidence_policies[r.zero_windows] = w.policy_version;
            if (++r.zero_windows == 3) {
               r.banned = true;
               r.ban_final_block = ctx.final_block_num;
               ++s.total_banned;
            }
         }
      });
   }
   s.pending_windows.erase(s.pending_windows.begin(), s.pending_windows.begin() + consumed);
}

template<typename T> eosio::checksum256 digest(const T& value) {
   auto bytes = eosio::pack(value);
   return eosio::sha256(bytes.data(), bytes.size());
}
template<typename T> bool contains(const std::vector<T>& values, const T& value) {
   return std::find(values.begin(), values.end(), value) != values.end();
}
bool usable_producer(const producer_info& p) {
   if (!p.active()) return false;
   return std::visit([](const auto& auth) {
      return auth.is_valid() && std::all_of(auth.keys.begin(), auth.keys.end(), [](const auto& kw) {
         return kw.key != eosio::public_key{};
      });
   }, p.get_producer_authority());
}
rotation_context_v1 read_context() {
   const auto size = get_rotation_context_v1(nullptr, 0);
   // Producer authorities may contain multiple keys. Do not let an arbitrary
   // 64 KiB read cap strand an otherwise accepted policy on the next block.
   // The host supplies the exact size and the normal WASM memory limit applies.
   check(size > 0, "native rotation context requires Savanna");
   std::vector<char> data(size);
   check(get_rotation_context_v1(data.data(), size) == size, "rotation context changed");
   auto ctx = eosio::unpack<rotation_context_v1>(data.data(), data.size());
   check(ctx.version == 1, "unsupported native rotation context");
   return ctx;
}
bool common_members(const rotation_context_v1& ctx, const std::vector<name>& members) {
   if (members.empty() || members.size() > rotation::committee_limit ||
       ctx.producers.size() != members.size() || ctx.finalizers.size() != members.size() ||
       ctx.threshold != rotation::finalizer_threshold(members.size())) return false;
   std::vector<name> finalizers;
   for (const auto& f : ctx.finalizers) {
      // Description is an account identifier in this fresh-chain profile.
      const auto n = name{f.description};
      if (f.weight != 1 || !contains(members, n) || contains(finalizers, n)) return false;
      finalizers.push_back(n);
   }
   for (const auto& p : ctx.producers)
      if (!contains(members, p.producer_name)) return false;
   return true;
}
eosio::finalizer_policy final_policy(const rotation_context_v1& ctx) {
   return {ctx.threshold, ctx.finalizers};
}
bool exact_target(const rotation_context_v1& ctx, const rotation_info& s) {
   return !ctx.transitioning && ctx.proposer_version == s.proposer_version &&
      ctx.finalizer_generation == s.finalizer_generation && common_members(ctx, s.target) &&
      digest(ctx.producers) == s.proposer_digest && digest(final_policy(ctx)) == s.finalizer_digest;
}
void begin_observation(rotation_info& s, uint64_t slot) {
   const uint64_t span = s.active.size() * rotation::producer_slots;
   // A complete, globally aligned schedule round strictly after activation.
   // Partial windows at activation and wall-clock jumps do not count as proof.
   s.observation_start = (slot / span + 1) * span;
   s.observation_end = s.observation_start + span - 1;
   s.produced_mask = 0;
   s.observation_block = 0;
}
}

void system_contract::refresh_rotation_candidate(name owner, bool reserve) {
   rotation_singleton table(get_self(), get_self().value);
   if (!table.exists()) return;
   const auto s = table.get();
   check(s.version == 2, "rotation v2 requires fresh-chain initialization");
   rotation_candidates candidates(get_self(), get_self().value);
   auto c = candidates.find(owner.value);
   if (c == candidates.end() && !reserve) return;
   const auto p = _producers.find(owner.value);
   const auto f = _finalizers.find(owner.value);
   auto key = invalid_rank_floor | owner.value;
   if (p != _producers.end() && p->total_votes > 0 && usable_producer(*p) &&
       f != _finalizers.end() && f->active_key_binary.size() == 96)
      key = (uint128_t(uint64_t(-1) - uint64_t(p->total_votes)) << 64) | owner.value;
   if (c == candidates.end()) {
      require_auth(owner);
      candidates.emplace(owner, [&](auto& r) {
         r.owner = owner;
         r.rank = key;
         if (s.ranking) r.frozen_epoch = s.snapshot_epoch; // frozen index excludes new registrations
         else r.previous_rank = key;
      });
   } else if (c->rank != key) {
      candidates.modify(c, same_payer, [&](auto& r) {
         if (s.ranking) {
            // Never insert a moved live key ahead of the snapshot cursor.
            // Otherwise repeated changes could indefinitely consume its budget.
            r.frozen_epoch = s.snapshot_epoch;
         } else {
            r.previous_rank = key;
            r.frozen_epoch = 0;
         }
         r.rank = key;
      });
   }
}

void system_contract::advance_rotation(const block_timestamp& timestamp, name producer) {
   rotation_singleton table(get_self(), get_self().value);
   // Bootstrap/seal is independent of stake activation. Until governance is
   // sealed the explicitly configured native genesis signer remains in charge.
   if (!table.exists() || get_upgrade_developer() == 0) return;
   auto s = table.get();
   check(s.version == 2, "rotation v2 requires fresh-chain initialization");
   const auto ctx = read_context();
   check(ctx.slot == timestamp.slot, "onblock must describe executed parent block");
   s.capacity = rotation::candidate_capacity(s.growth_start_slot, ctx.slot);
   rotation_candidates candidates(get_self(), get_self().value);
   const bool observation_gap = s.last_slot && uint64_t(ctx.slot) - s.last_slot > rotation::pause_guard_slots;
   // Outside a snapshot, user mutations update both indexes directly and do
   // not add dirty rows. Thus this drain has a finite, monotonically shrinking
   // workload even under continuous voting; policy handoffs need not wait.
   const bool rank_ready = s.ranking || sync_ranking(candidates);
   s.blocked = 0;
   std::vector<name> actual;
   for (const auto& p : ctx.producers) actual.push_back(p.producer_name);
   observe_windows(s, candidates, ctx, producer,
      actual.size() == rotation::committee_limit && ctx.finalizers.size() == rotation::committee_limit,
      common_members(ctx, actual));
   const auto banned = [&](name n) {
      auto c = candidates.find(n.value);
      return c != candidates.end() && c->banned;
   };

   if (s.active.empty()) {
      for (const auto& p : ctx.producers) s.active.push_back(p.producer_name);
      if (ctx.transitioning || !common_members(ctx, s.active)) return;
   }

   if (s.stage != 0) {
      if (!exact_target(ctx, s)) {
         // No new policy is emitted while either half of the handoff is pending.
         if (s.stage == 2) {
            s.stage = 1;
            s.produced_mask = 0;
         }
         table.set(s, get_self());
         return;
      }
      if (s.stage == 3) { // key-only update; membership/cursor never changes
         s.stage = 0;
         s.target.clear();
         s.next_attempt_slot = uint64_t(ctx.slot) + rotation::normal_epoch_slots;
      } else if (s.stage == 1) {
         s.active = s.target;
         s.retry_outgoing = name{};
         if (!s.incoming) {
            // Removal-only: BOTH native policies have completed their normal
            // finalized handoff (exact_target above). There is no entrant to
            // observe. Do not count this as a successful admission or retain
            // the banned seat merely because no replacement is registered.
            s.stage = 0;
            s.target.clear();
            s.outgoing = name{};
            s.next_attempt_slot = ctx.slot;
         } else {
            s.stage = 2;
            begin_observation(s, ctx.slot);
         }
      } else if (s.stage == 2) {
         if (observation_gap || uint64_t(ctx.slot) - ctx.final_slot > rotation::pause_guard_slots) {
            // A whole-chain/finality pause is not evidence against the entrant.
            begin_observation(s, ctx.slot);
            s.blocked = 1;
            table.set(s, get_self());
            return;
         }
         if (banned(s.incoming)) {
            ++s.failed_admissions;
            s.stage = 0;
            s.target.clear();
            s.next_attempt_slot = ctx.slot; // prioritized single-seat fault replacement
            table.set(s, get_self());
            return;
         }
         if (ctx.slot >= s.observation_start && ctx.slot <= s.observation_end) {
            const auto scheduled = ctx.producers[(ctx.slot / rotation::producer_slots) % ctx.producers.size()].producer_name;
            check(producer == scheduled, "onblock producer does not match native active schedule");
            auto it = std::find(s.active.begin(), s.active.end(), producer);
            check(it != s.active.end(), "observed producer not in active rotation");
            s.produced_mask |= uint32_t(1) << (it - s.active.begin());
            s.observation_block = ctx.block_num;
         }
         if (uint64_t(ctx.final_slot) >= s.observation_end &&
             ctx.final_block_num >= s.observation_block) {
            const auto entrant = std::find(s.active.begin(), s.active.end(), s.incoming);
            check(entrant != s.active.end(), "missing observed entrant");
            const uint32_t entrant_bit = uint32_t(1) << (entrant - s.active.begin());
            // Native finality needs a quorum, NOT unanimous incumbent output.
            // Requiring every incumbent here would give one withholding member
            // a new veto over rotations. Only the entrant's assigned window is
            // the admission observation; the separate finalized-window
            // monitor above handles failures of ALL actual members.
            if ((s.produced_mask & entrant_bit) != 0) {
               ++s.completed;
               s.stage = 0;
               s.target.clear();
               s.next_attempt_slot = uint64_t(ctx.slot) + rotation::normal_epoch_slots;
            } else {
               ++s.stalled_windows;
               s.blocked = 3;
               if (s.active.size() < rotation::committee_limit) {
                  // A fully finalized assigned window with zero output ends
                  // this admission attempt, NOT the account's eligibility.
                  // Keep this seat until a one-for-one replacement is available;
                  // do not accumulate additional untested seats or shrink quorum.
                  s.retry_outgoing = s.incoming;
                  ++s.failed_admissions;
                  s.stage = 0;
                  s.target.clear();
                  s.next_attempt_slot = ctx.slot;
               } else {
                  begin_observation(s, ctx.slot); // full committee: existing three-window ban rule
               }
            }
         }
      }
      table.set(s, get_self());
      return;
   }

   const bool fault = std::any_of(s.active.begin(), s.active.end(), banned);
   if (actual.size() == rotation::committee_limit && s.penalty_paused) {
      table.set(s, get_self());
      return; // no new admissions during a full-committee global/finality pause
   }
   if (ctx.transitioning || !common_members(ctx, s.active) || (!fault && !s.ranking && !s.selecting && ctx.slot < s.next_attempt_slot)) {
      table.set(s, get_self());
      return;
   }
   if (!s.ranking && !s.selecting) {
      if (!rank_ready) { table.set(s, get_self()); return; }
      check(s.snapshot_epoch != uint64_t(-1), "ranking epoch exhausted");
      ++s.snapshot_epoch;
      s.snapshot_capacity = s.capacity;
      s.ranked_count = 0;
      s.rank_started = false;
      s.ranking = true;
   }
   if (s.ranking) advance_ranking(s, candidates);
   if (s.ranking) { table.set(s, get_self()); return; }
   name incoming, outgoing;
   const auto eligible = [&](name n) {
      auto c = candidates.find(n.value);
      return c != candidates.end() && !c->banned && c->seen_epoch == s.published_epoch && c->rank < invalid_rank_floor;
   };
   for (auto n : s.active) if (banned(n)) { outgoing = n; break; }
   const auto retry = s.retry_outgoing.value_or(name{});
   if (!outgoing && retry && contains(s.active, retry)) outgoing = retry;
   if (!outgoing && !s.penalty_paused) {
      // Replace a finalized zero-output seat before removing a healthy FIFO
      // incumbent. Alternating misses cannot veto ALL future rotations, and
      // this avoids accumulating offline seats. A single miss is NOT a ban.
      for (auto n : s.active) {
         const auto c = candidates.find(n.value);
         if (c != candidates.end() && c->penalty_epoch == s.penalty_epoch && c->zero_windows > 0) {
            outgoing = n;
            break;
         }
      }
   }
   if (!outgoing) for (auto n : s.active) if (!eligible(n)) { outgoing = n; break; }
   if (!outgoing && s.active.size() == rotation::committee_limit) outgoing = s.active.front();
   auto tickets = candidates.get_index<"byticket"_n>();
   for (uint32_t work = 0; work < rotation::queue_work_budget; ++work) {
      auto c = tickets.lower_bound(1);
      if (c == tickets.end() || c->ticket > s.scan_end_ticket) { s.selecting = false; break; }
      const auto n = c->owner;
      const bool ok = eligible(n);
      tickets.modify(c, same_payer, [&](auto& r) { r.ticket = ok ? next_ticket(s) : 0; });
      if (ok && !contains(s.active, n)) { incoming = n; s.selecting = false; break; }
   }
   if (s.selecting) { table.set(s, get_self()); return; }
   s.next_attempt_slot = uint64_t(ctx.slot) + rotation::normal_epoch_slots;
   // A permanent ban revokes the current seat as well as future eligibility.
   // Prefer the ordinary one-for-one replacement. With no replacement, remove
   // just this banned member through the SAME native joint-policy handoff.
   // Neither a transient miss nor loss of registration authorizes shrinking.
   // Never emit an empty policy, even for a malformed external starting state.
   const bool eviction = !incoming && outgoing && banned(outgoing) && s.active.size() > 1;
   const bool membership_change = bool(incoming) || eviction;
   auto target = s.active;
   if (membership_change) {
      if (outgoing) target.erase(std::find(target.begin(), target.end(), outgoing));
      if (incoming) target.push_back(incoming);
   } else if (fault || retry) s.blocked = 2;

   std::vector<eosio::producer_authority> proposers;
   std::vector<finalizer_auth_info> finalizers;
   // Retain actual keys of any unregistered, non-banned incumbent until a
   // one-seat replacement is possible. Banned seats leave via the target above.
   for (auto n : target) {
      auto p = _producers.find(n.value);
      auto f = _finalizers.find(n.value);
      if (p != _producers.end() && usable_producer(*p) && f != _finalizers.end() && f->active_key_binary.size() == 96) {
         proposers.push_back({n, p->get_producer_authority()});
         finalizers.emplace_back(*f);
      } else {
         // A partially invalid registry does not block replacing some OTHER
         // member. For retained members preserve native keys, separately below.
         auto old = std::find_if(ctx.producers.begin(), ctx.producers.end(), [&](const auto& x) { return x.producer_name == n; });
         check(old != ctx.producers.end(), "new rotation candidate lost qualification");
         proposers.push_back(*old);
         auto oldf = std::find_if(ctx.finalizers.begin(), ctx.finalizers.end(), [&](const auto& x) { return x.description == n.to_string(); });
         check(oldf != ctx.finalizers.end(), "missing incumbent finalizer");
         finalizer_auth_info retained;
         retained.key_id = n.value; // sorting key is overridden by account order below
         retained.fin_authority = *oldf;
         finalizers.push_back(retained);
      }
   }
   std::sort(proposers.begin(), proposers.end(), [](const auto& a, const auto& b) { return a.producer_name < b.producer_name; });
   // Fixed account order makes key-only comparisons independent of registration order.
   std::sort(finalizers.begin(), finalizers.end(), [](const auto& a, const auto& b) { return name{a.fin_authority.description} < name{b.fin_authority.description}; });
   eosio::finalizer_policy fp;
   fp.threshold = rotation::finalizer_threshold(target.size());
   for (const auto& f : finalizers) fp.finalizers.push_back(f.fin_authority);
   const auto pd = digest(proposers), fd = digest(fp);
   const bool change_p = pd != digest(ctx.producers), change_f = fd != digest(final_policy(ctx));
   if (!change_p && !change_f) {
      table.set(s, get_self());
      return;
   }
   uint32_t version = ctx.proposer_version;
   if (change_p) {
      check(version < std::numeric_limits<uint32_t>::max(), "proposer version exhausted");
      const auto proposed = set_proposed_producers(proposers);
      check(proposed.has_value(), "native proposer update rejected");
      // Savanna returns UINT32_MAX here, NOT the policy version. With no
      // in-flight policy the assembler assigns active_version + 1. Activation
      // is still verified against that version AND the frozen policy digest.
      ++version;
   }
   if (change_f) {
      check(ctx.latest_finalizer_generation < std::numeric_limits<uint32_t>::max(), "finalizer generation exhausted");
      eosio::set_finalizers(fp);
   }
   s.target = target;
   s.proposer_version = uint32_t(version);
   s.finalizer_generation = ctx.latest_finalizer_generation + uint32_t(change_f);
   s.proposer_digest = pd;
   s.finalizer_digest = fd;
   s.stage = membership_change ? 1 : 3;
   s.incoming = membership_change ? incoming : name{};
   s.outgoing = membership_change ? outgoing : name{};
   _gstate.last_producer_schedule_size = target.size();
   table.set(s, get_self());
}
}
