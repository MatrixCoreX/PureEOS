#ifdef PUREEOS_ROTATION_STANDALONE
#define BOOST_TEST_MODULE pureeos_rotation
#include <boost/test/included/unit_test.hpp>
#else
#include <boost/test/unit_test.hpp>
#endif

#include <eosio.system/rotation.hpp>
#include <eosio.system/rotation_growth.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

using namespace eosiosystem::rotation;

namespace {

std::vector<account_id> accounts(account_id first, account_id last) {
   std::vector<account_id> result;
   for (auto n = first; n <= last; ++n)
      result.push_back(n);
   return result;
}

void equal_accounts(const std::vector<account_id>& actual, const std::vector<account_id>& expected) {
   BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}

auto fields(const state& s) {
   return std::tie(s.queue, s.snapshot, s.active, s.confirmed, s.target, s.stage, s.cause,
                   s.outgoing, s.incoming, s.scan_remaining, s.proposed.proposer,
                   s.proposed.finalizer, s.active_policies_match, s.active_since_slot);
}

void check_checkpoint(const scheduler& original) {
   scheduler restored;
   BOOST_REQUIRE(restored.restore(original.get_state()) == error::none);
   BOOST_CHECK(fields(restored.get_state()) == fields(original.get_state()));
}

account_id next_candidate(scheduler& s, uint32_t budget = candidate_limit) {
   uint32_t inspected = 0;
   while (s.get_state().stage == phase::scanning) {
      const auto result = s.scan(budget);
      BOOST_REQUIRE(result.status == error::none);
      BOOST_REQUIRE(result.inspected <= budget);
      inspected += result.inspected;
      BOOST_REQUIRE(inspected <= candidate_limit);
      check_checkpoint(s);
      if (result.candidate != 0)
         return result.candidate;
   }
   return 0;
}

void submit_and_activate(scheduler& s, uint64_t slot, policy_versions versions = {1, 1}) {
   BOOST_REQUIRE(s.accept_candidate() == error::none);
   check_checkpoint(s);
   BOOST_REQUIRE(s.mark_submitted(versions) == error::none);
   check_checkpoint(s);
   auto proposer_order = s.get_state().target;
   auto finalizer_order = proposer_order;
   std::sort(proposer_order.begin(), proposer_order.end());
   std::reverse(finalizer_order.begin(), finalizer_order.end());
   BOOST_REQUIRE(s.observe_active(versions, proposer_order, finalizer_order, slot) == error::none);
   check_checkpoint(s);
}

void finish(scheduler& s, uint64_t slot = 1000, policy_versions versions = {1, 1}) {
   submit_and_activate(s, slot, versions);
   const auto duration = s.get_state().active.size() * producer_slots;
   BOOST_REQUIRE(s.complete_round(slot + duration, true, true) == error::none);
   check_checkpoint(s);
}

} // namespace

BOOST_AUTO_TEST_SUITE(pureeos_rotation_tests)

BOOST_AUTO_TEST_CASE(committee_limits_and_quorum) {
   BOOST_CHECK_EQUAL(candidate_limit, 210u);
   BOOST_CHECK_EQUAL(committee_limit, 21u);
   BOOST_CHECK_EQUAL(normal_epoch_slots, 252u);
   BOOST_CHECK_EQUAL(finalizer_threshold(0), 0u);
   BOOST_CHECK_EQUAL(finalizer_threshold(1), 1u);
   BOOST_CHECK_EQUAL(finalizer_threshold(2), 2u);
   BOOST_CHECK_EQUAL(finalizer_threshold(3), 3u);
   BOOST_CHECK_EQUAL(finalizer_threshold(4), 3u);
   BOOST_CHECK_EQUAL(finalizer_threshold(21), 15u);
   BOOST_CHECK_EQUAL(finalizer_threshold(22), 0u);
   BOOST_CHECK_EQUAL(finalizer_threshold(std::numeric_limits<uint32_t>::max()), 0u);
}

BOOST_AUTO_TEST_CASE(genesis_membership_validation) {
   scheduler s;
   BOOST_CHECK(s.begin_round({1}) == error::not_initialized);
   BOOST_CHECK(s.initialize({}) == error::invalid_members);
   BOOST_CHECK(s.initialize({0}) == error::invalid_members);
   BOOST_CHECK(s.initialize({1, 1}) == error::invalid_members);
   BOOST_CHECK(s.initialize(accounts(1, 22)) == error::invalid_members);
   BOOST_REQUIRE(s.initialize({77}) == error::none);
   BOOST_CHECK(s.initialize({88}) == error::busy);
   equal_accounts(s.get_state().active, {77});
   check_checkpoint(s);
}

BOOST_AUTO_TEST_CASE(candidate_count_boundaries) {
   for (uint32_t count : {0u, 1u, 20u, 21u, 22u, 209u, 210u, 211u}) {
      scheduler s;
      BOOST_REQUIRE(s.initialize({1}) == error::none);
      const auto result = s.begin_round(accounts(1, count));
      if (count > candidate_limit) {
         BOOST_CHECK(result == error::invalid_snapshot);
         BOOST_CHECK(s.get_state().stage == phase::idle);
      } else {
         BOOST_REQUIRE(result == error::none);
         BOOST_CHECK_EQUAL(next_candidate(s), count > 1 ? 2u : 0u);
      }
      equal_accounts(s.get_state().active, {1});
      check_checkpoint(s);
   }
}

BOOST_AUTO_TEST_CASE(bad_snapshot_does_not_mutate_state) {
   scheduler s;
   BOOST_REQUIRE(s.initialize({1}) == error::none);
   const auto before = s.get_state();
   for (auto bad : {std::vector<account_id>{0, 1}, std::vector<account_id>{1, 1}, accounts(1, 211)}) {
      BOOST_CHECK(s.begin_round(bad) == error::invalid_snapshot);
      BOOST_CHECK(fields(s.get_state()) == fields(before));
   }
}

BOOST_AUTO_TEST_CASE(grow_one_at_a_time_then_rotate) {
   scheduler s;
   BOOST_REQUIRE(s.initialize({1}) == error::none);
   const auto pool = accounts(1, 210);
   for (uint32_t newcomer = 2; newcomer <= 21; ++newcomer) {
      BOOST_REQUIRE(s.begin_round(pool) == error::none);
      BOOST_CHECK(s.get_state().cause == reason::growth);
      BOOST_CHECK_EQUAL(s.get_state().outgoing, 0u);
      BOOST_CHECK_EQUAL(next_candidate(s), newcomer);
      equal_accounts(s.get_state().active, accounts(1, newcomer - 1));
      finish(s, newcomer * 1000, {newcomer, newcomer});
      equal_accounts(s.get_state().active, accounts(1, newcomer));
   }
   BOOST_REQUIRE(s.begin_round(pool) == error::none);
   BOOST_CHECK(s.get_state().cause == reason::rotation);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 1u);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   finish(s);
   equal_accounts(s.get_state().active, accounts(2, 22));
}

BOOST_AUTO_TEST_CASE(bootstrap_account_gets_no_permanent_seat) {
   scheduler s;
   BOOST_REQUIRE(s.initialize({999}) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK(s.get_state().cause == reason::ineligible);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 999u);
   BOOST_CHECK_EQUAL(next_candidate(s), 1u);
   finish(s);
   equal_accounts(s.get_state().active, {1});
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK(s.get_state().cause == reason::growth);
   BOOST_CHECK_EQUAL(next_candidate(s), 2u);
}

BOOST_AUTO_TEST_CASE(two_full_cycles_never_starve_lower_ranked_candidates) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   std::map<account_id, uint32_t> admissions;
   for (uint32_t round = 0; round < 420; ++round) {
      BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
      BOOST_CHECK_EQUAL(s.get_state().outgoing, round % 210 + 1);
      const auto newcomer = next_candidate(s, 7);
      BOOST_CHECK_EQUAL(newcomer, (round + 21) % 210 + 1);
      ++admissions[newcomer];
      const auto previous = s.get_state().active;
      finish(s, (round + 1) * 1000, {round + 1, round + 1});
      BOOST_REQUIRE_EQUAL(s.get_state().active.size(), 21u);
      BOOST_CHECK_EQUAL_COLLECTIONS(previous.begin() + 1, previous.end(),
                                    s.get_state().active.begin(), s.get_state().active.end() - 1);
   }
   BOOST_CHECK_EQUAL(admissions.size(), 210u);
   for (const auto& admission : admissions)
      BOOST_CHECK_EQUAL(admission.second, 2u);
   equal_accounts(s.get_state().active, accounts(1, 21));
}

BOOST_AUTO_TEST_CASE(preflight_failure_keeps_incumbent_until_a_valid_replacement) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   BOOST_REQUIRE(s.reject_candidate() == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 23u);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 1u);
   equal_accounts(s.get_state().active, accounts(1, 21));
   finish(s);
   auto expected = accounts(2, 21);
   expected.push_back(23);
   equal_accounts(s.get_state().active, expected);
}

BOOST_AUTO_TEST_CASE(all_standby_failures_are_bounded_and_keep_active_committee) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 30)) == error::none);
   for (uint32_t n = 22; n <= 30; ++n) {
      BOOST_CHECK_EQUAL(next_candidate(s), n);
      BOOST_REQUIRE(s.reject_candidate() == error::none);
      check_checkpoint(s);
   }
   BOOST_CHECK(s.get_state().stage == phase::idle);
   equal_accounts(s.get_state().active, accounts(1, 21));
   BOOST_CHECK(s.scan(1).status == error::wrong_phase);
}

BOOST_AUTO_TEST_CASE(no_standby_or_no_candidates_never_removes_members) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 21)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 0u);
   BOOST_CHECK(s.get_state().stage == phase::idle);
   BOOST_REQUIRE(s.begin_round({}) == error::none);
   BOOST_CHECK(s.get_state().stage == phase::idle);
   equal_accounts(s.get_state().active, accounts(1, 21));
}

BOOST_AUTO_TEST_CASE(scan_obeys_per_call_budget) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   const auto before = s.get_state();
   BOOST_CHECK(s.scan(0).status == error::invalid_budget);
   BOOST_CHECK(s.scan(211).status == error::invalid_budget);
   BOOST_CHECK(fields(s.get_state()) == fields(before));
   for (uint32_t n = 1; n <= 21; ++n) {
      const auto result = s.scan(1);
      BOOST_CHECK_EQUAL(result.inspected, 1u);
      BOOST_CHECK_EQUAL(result.candidate, 0u);
      BOOST_CHECK_EQUAL(s.get_state().scan_remaining, 210u - n);
   }
   BOOST_CHECK_EQUAL(s.scan(1).candidate, 22u);
}

BOOST_AUTO_TEST_CASE(rank_reordering_preserves_cursor_and_new_members_append) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   BOOST_REQUIRE(s.cancel_before_submission() == error::none);
   auto reversed = accounts(1, 210);
   std::reverse(reversed.begin(), reversed.end());
   BOOST_REQUIRE(s.begin_round(reversed) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 23u); // Not 210 or 22 again.
   BOOST_REQUIRE(s.cancel_before_submission() == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(2, 211)) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().queue.front(), 24u);
   BOOST_CHECK_EQUAL(s.get_state().queue.back(), 211u);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 1u);
   BOOST_CHECK(s.get_state().cause == reason::ineligible);
}

BOOST_AUTO_TEST_CASE(reentry_after_leaving_snapshot_does_not_jump_queue) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 30)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   BOOST_REQUIRE(s.cancel_before_submission() == error::none);
   auto without_22 = accounts(1, 30);
   without_22.erase(std::find(without_22.begin(), without_22.end(), 22));
   BOOST_REQUIRE(s.begin_round(without_22) == error::none);
   BOOST_REQUIRE(s.cancel_before_submission() == error::none);
   auto reranked = without_22;
   reranked.insert(reranked.begin(), 22);
   BOOST_REQUIRE(s.begin_round(reranked) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().queue.front(), 23u);
   BOOST_CHECK_EQUAL(s.get_state().queue.back(), 22u);
}

BOOST_AUTO_TEST_CASE(snapshot_cannot_change_during_a_round) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   const auto before = s.get_state();
   BOOST_CHECK(s.begin_round(accounts(2, 211)) == error::busy);
   BOOST_CHECK(fields(s.get_state()) == fields(before));
   BOOST_REQUIRE(s.accept_candidate() == error::none);
   // Registration/key revalidation can still reject BEFORE policy submission.
   BOOST_REQUIRE(s.reject_candidate() == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 23u);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 1u);
}

BOOST_AUTO_TEST_CASE(faults_have_priority_and_preserve_other_twenty_members) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210), {10, 5}) == error::none);
   BOOST_CHECK(s.get_state().cause == reason::fault);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 5u); // Tenure order, not report order.
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   finish(s);
   auto expected = accounts(1, 21);
   expected.erase(expected.begin() + 4);
   expected.push_back(22);
   equal_accounts(s.get_state().active, expected);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 1u); // Newcomer is not kicked next.
}

BOOST_AUTO_TEST_CASE(faults_precede_growth_and_invalid_reports_do_not_mutate_state) {
   scheduler s;
   BOOST_REQUIRE(s.initialize({1, 2}) == error::none);
   const auto before = s.get_state();
   BOOST_CHECK(s.begin_round(accounts(1, 210), {3}) == error::invalid_faults);
   BOOST_CHECK(s.begin_round(accounts(1, 210), {1, 1}) == error::invalid_faults);
   BOOST_CHECK(fields(s.get_state()) == fields(before));
   BOOST_REQUIRE(s.begin_round(accounts(1, 210), {2}) == error::none);
   BOOST_CHECK(s.get_state().cause == reason::fault);
   BOOST_CHECK_EQUAL(next_candidate(s), 3u);
   finish(s);
   equal_accounts(s.get_state().active, {1, 3});
}

BOOST_AUTO_TEST_CASE(both_actual_policies_and_versions_must_match) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   BOOST_REQUIRE(s.accept_candidate() == error::none);
   BOOST_CHECK(s.mark_submitted({0, 1}) == error::invalid_policy);
   BOOST_CHECK(s.mark_submitted({1, 0}) == error::invalid_policy);
   BOOST_REQUIRE(s.mark_submitted({7, 9}) == error::none);
   const auto target = s.get_state().target;
   BOOST_CHECK(s.observe_active({7, 9}, target, accounts(1, 21), 1000) == error::not_active);
   BOOST_CHECK(s.observe_active({7, 8}, target, target, 1000) == error::not_active);
   BOOST_CHECK(s.observe_active({6, 9}, target, target, 1000) == error::not_active);
   auto duplicate = target;
   duplicate[0] = duplicate[1];
   BOOST_CHECK(s.observe_active({7, 9}, target, duplicate, 1000) == error::invalid_members);
   BOOST_CHECK(s.get_state().stage == phase::submitted);
   equal_accounts(s.get_state().active, accounts(1, 21));
   BOOST_CHECK(s.cancel_before_submission() == error::wrong_phase);
   BOOST_CHECK(s.reject_candidate() == error::wrong_phase);
   BOOST_CHECK(s.retry_failed_admission(true) == error::wrong_phase);
   BOOST_CHECK(s.begin_round(accounts(1, 210)) == error::busy);
   auto reversed = target;
   std::reverse(reversed.begin(), reversed.end());
   BOOST_REQUIRE(s.observe_active({7, 9}, reversed, target, 1000) == error::none);
   equal_accounts(s.get_state().active, accounts(2, 22));
   equal_accounts(s.get_state().confirmed, accounts(1, 21));
}

BOOST_AUTO_TEST_CASE(observation_cannot_be_shortened_or_passed_without_verified_health) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   submit_and_activate(s, 1000);
   BOOST_CHECK(s.complete_round(999, true, true) == error::slot_regression);
   BOOST_CHECK(s.complete_round(1251, true, true) == error::observation_incomplete);
   BOOST_CHECK(s.complete_round(1252, false, true) == error::observation_incomplete);
   BOOST_CHECK(s.complete_round(1252, true, false) == error::observation_incomplete);
   const auto target = s.get_state().target;
   BOOST_CHECK(s.observe_active({1, 1}, target, target, 999) == error::slot_regression);
   BOOST_REQUIRE(s.observe_active({1, 1}, target, target, 1100) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().active_since_slot, 1000u);
   BOOST_REQUIRE(s.complete_round(1252, true, true) == error::none);
   equal_accounts(s.get_state().confirmed, accounts(2, 22));
}

BOOST_AUTO_TEST_CASE(failed_newcomer_is_replaced_without_rolling_back_active_policies) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   submit_and_activate(s, 1000);
   BOOST_CHECK(s.retry_failed_admission(false) == error::unverified_fault);
   BOOST_REQUIRE(s.retry_failed_admission(true) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 22u);
   BOOST_CHECK(s.get_state().cause == reason::fault);
   equal_accounts(s.get_state().active, accounts(2, 22));
   equal_accounts(s.get_state().confirmed, accounts(1, 21));
   check_checkpoint(s);
   BOOST_CHECK_EQUAL(next_candidate(s), 23u);
   finish(s, 2000, {2, 2});
   auto expected = accounts(2, 21);
   expected.push_back(23);
   equal_accounts(s.get_state().confirmed, expected);
}

BOOST_AUTO_TEST_CASE(conflicting_active_policy_invalidates_the_observation_interval) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   submit_and_activate(s, 1000);
   const auto target = s.get_state().target;
   BOOST_CHECK(s.observe_active({1, 2}, target, target, 1100) == error::not_active);
   BOOST_CHECK(s.complete_round(1252, true, true) == error::not_active);
   BOOST_CHECK(s.retry_failed_admission(true) == error::not_active);
   check_checkpoint(s);
   BOOST_REQUIRE(s.observe_active({1, 1}, target, target, 1300) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().active_since_slot, 1300u);
   BOOST_CHECK(s.complete_round(1551, true, true) == error::observation_incomplete);
   BOOST_REQUIRE(s.complete_round(1552, true, true) == error::none);
}

BOOST_AUTO_TEST_CASE(exhausted_retry_keeps_actual_failed_member_for_next_recovery_round) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 22)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   submit_and_activate(s, 1000);
   BOOST_REQUIRE(s.retry_failed_admission(true) == error::none);
   BOOST_CHECK(s.get_state().stage == phase::idle);
   equal_accounts(s.get_state().active, accounts(2, 22));
   equal_accounts(s.get_state().confirmed, accounts(1, 21));
   check_checkpoint(s);
   BOOST_REQUIRE(s.begin_round(accounts(1, 22), {22}) == error::none);
   BOOST_CHECK_EQUAL(s.get_state().outgoing, 22u);
   BOOST_CHECK_EQUAL(next_candidate(s), 1u);
}

BOOST_AUTO_TEST_CASE(checkpoint_replay_resumes_the_same_scan_and_policy_barrier) {
   scheduler original;
   BOOST_REQUIRE(original.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(original.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(original.scan(5).inspected, 5u);
   scheduler replay;
   BOOST_REQUIRE(replay.restore(original.get_state()) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(original, 3), 22u);
   BOOST_CHECK_EQUAL(next_candidate(replay, 3), 22u);
   BOOST_REQUIRE(original.reject_candidate() == error::none);
   BOOST_REQUIRE(replay.reject_candidate() == error::none);
   BOOST_CHECK_EQUAL(next_candidate(original), 23u);
   BOOST_CHECK_EQUAL(next_candidate(replay), 23u);
   finish(original, 1000, {3, 5});
   finish(replay, 1000, {3, 5});
   BOOST_CHECK(fields(original.get_state()) == fields(replay.get_state()));
}

BOOST_AUTO_TEST_CASE(invalid_checkpoints_are_rejected_atomically) {
   scheduler s;
   BOOST_REQUIRE(s.initialize(accounts(1, 21)) == error::none);
   BOOST_REQUIRE(s.begin_round(accounts(1, 210)) == error::none);
   BOOST_CHECK_EQUAL(next_candidate(s), 22u);
   BOOST_REQUIRE(s.accept_candidate() == error::none);
   const auto before = s.get_state();
   const auto reject = [&](const state& bad) {
      BOOST_CHECK(s.restore(bad) == error::invalid_checkpoint);
      BOOST_CHECK(fields(s.get_state()) == fields(before));
   };
   auto bad = before;
   bad.scan_remaining = 211;
   reject(bad);
   bad = before;
   bad.queue[1] = bad.queue[0];
   reject(bad);
   bad = before;
   bad.outgoing = 999;
   reject(bad);
   bad = before;
   bad.outgoing = 2;
   reject(bad); // Normal rotation cannot quietly switch the FIFO exit member.
   bad = before;
   std::swap(bad.queue.front(), bad.queue.back());
   reject(bad); // The accepted candidate must be the last position inspected.
   bad = before;
   bad.scan_remaining = static_cast<uint32_t>(bad.queue.size());
   reject(bad); // Selection already consumed at least one snapshot position.
   bad = before;
   bad.target[0] = 999;
   reject(bad);
   bad = before;
   bad.snapshot.pop_back();
   reject(bad);
   bad = before;
   bad.stage = static_cast<phase>(255);
   reject(bad);
   bad = before;
   bad.cause = static_cast<reason>(255);
   reject(bad);
   bad = before;
   bad.active.clear();
   reject(bad);
   bad = before;
   bad.stage = phase::submitted;
   reject(bad); // Missing expected policy versions.
}

BOOST_AUTO_TEST_CASE(growth_is_piecewise_linear_and_continuous_at_four_year_boundaries) {
   const uint64_t origin = 123456;
   const auto p = growth_period_slots;
   BOOST_CHECK_EQUAL(candidate_capacity(origin, origin - 1), 210u);
   for (unsigned epoch = 0; epoch < 16; ++epoch) {
      const auto start = origin + epoch * p;
      const auto base = uint64_t(210) << epoch;
      BOOST_CHECK_EQUAL(candidate_capacity(origin, start), base);
      BOOST_CHECK_EQUAL(candidate_capacity(origin, start + p / 4), base + base / 4);
      BOOST_CHECK_EQUAL(candidate_capacity(origin, start + p / 2), base + base / 2);
      BOOST_CHECK_EQUAL(candidate_capacity(origin, start + p - 1), 2 * base - 1);
      BOOST_CHECK_EQUAL(candidate_capacity(origin, start + p), 2 * base);
   }
   const auto first_slot = (p + 209) / 210;
   BOOST_CHECK_EQUAL(candidate_capacity(origin, origin + first_slot - 1), 210u);
   BOOST_CHECK_EQUAL(candidate_capacity(origin, origin + first_slot), 211u);
}

BOOST_AUTO_TEST_CASE(growth_uses_integer_arithmetic_and_never_wraps) {
   for (uint64_t delta = 0; delta < growth_period_slots; delta += 123457) {
      BOOST_CHECK_EQUAL(candidate_capacity(0, delta), 210 + 210 * delta / growth_period_slots);
      BOOST_CHECK_LE(candidate_capacity(0, delta), candidate_capacity(0, delta + 1));
   }
   BOOST_CHECK_EQUAL(candidate_capacity(0, uint64_t(-1)), uint32_t(-1));
   BOOST_CHECK_EQUAL(candidate_capacity(100, 1), 210u);
   BOOST_CHECK_GT(candidate_capacity(0, uint32_t(-1)), 210u);
}

BOOST_AUTO_TEST_SUITE_END()
