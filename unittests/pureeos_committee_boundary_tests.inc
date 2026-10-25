// Current full-profile boundary tests. Uses real WASM, single-seat FIFO,
// checked onblock/QCs and independent replay; never a legacy fixed top-21 pool.
namespace {
struct current_committee_fixture : sequential_rotation_fixture {
   std::map<name, int64_t> expected_votes;
   explicit current_committee_fixture(size_t count) : sequential_rotation_fixture(count) {
      for (size_t i = 0; i < count; ++i)
         expected_votes[nodes[i]] = int64_t(std::max(size_t(2300), count * 100) - 100 * i) * 10000;
   }
   fc::variant producer(name n) const { return row("producersv2"_n, n.to_uint64_t()); }
   void check_ranking() const {
      std::optional<eosio::chain_apis::tracked_votes> tracked;
      const auto result = eosio::chain_apis::read_only(*chain.control, {}, {}, tracked,
         fc::seconds(5), fc::seconds(5), nullptr)
         .get_producers({.json = true, .limit = 50}, fc::time_point::maximum());
      auto expected = nodes;
      std::sort(expected.begin(), expected.end(), [&](name a, name b) {
         return expected_votes.at(a) == expected_votes.at(b) ? a < b : expected_votes.at(a) > expected_votes.at(b);
      });
      BOOST_REQUIRE_EQUAL(result.rows.size(), expected.size());
      BOOST_REQUIRE_EQUAL(result.vote_schema, "integer-v2");
      BOOST_REQUIRE(result.total_producer_vote_amount);
      int64_t total = 0;
      for (size_t i = 0; i < expected.size(); ++i) {
         BOOST_CHECK_EQUAL(result.rows[i]["owner"].as_string(), expected[i].to_string());
         BOOST_CHECK_EQUAL(result.rows[i]["total_votes"].as_string(), std::to_string(expected_votes.at(expected[i])));
         total += expected_votes.at(expected[i]);
      }
      BOOST_CHECK_EQUAL(*result.total_producer_vote_amount, std::to_string(total));
   }
};
}

BOOST_AUTO_TEST_SUITE(pureeos_committee_boundary_tests)

BOOST_AUTO_TEST_CASE(twenty_two_candidates_all_rotate_but_zero_output_cannot_claim) {
   current_committee_fixture f(22);
   f.completed(21);
   const auto waiting = f.nodes.back();
   const auto initial = f.list("active");
   BOOST_REQUIRE_EQUAL(initial.size(), 21u);
   BOOST_REQUIRE(std::find(initial.begin(), initial.end(), waiting) == initial.end());
   BOOST_REQUIRE_EQUAL(f.producer(waiting)["unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", waiting), waiting),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "standby rewards are disabled"); });
   f.check_ranking();
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), waiting.to_string());
   BOOST_CHECK_EQUAL(f.rotation()["outgoing"].as_string(), initial.front().to_string());
   f.completed(22);
   auto expected = initial; expected.erase(expected.begin()); expected.push_back(waiting);
   BOOST_REQUIRE(f.list("active") == expected);
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 15u);
   for (auto n : f.nodes) {
      BOOST_REQUIRE_GT(f.produced[n], 0u);
      BOOST_REQUIRE_GT(f.producer(n)["unpaid_blocks"].as_uint64(), 0u);
      BOOST_CHECK(!f.candidate(n)["banned"].as_bool());
   }
   f.until([&] { return f.rotation()["stage"].as_uint64() == 2; });
   f.verify_full_production_round();
   f.check_split();
   f.observer_replay();
   BOOST_TEST_MESSAGE("All 22 eligible accounts really produce; rank 22 is not a permanent standby. No output means no claim.");
}

BOOST_AUTO_TEST_CASE(integer_ties_and_one_unit_changes_preserve_fifo_and_atomic_one_choice) {
   current_committee_fixture f(23);
   f.completed(21);
   const auto earlier = f.nodes[21], later = f.nodes[22];
   const auto active = f.list("active"), queue = f.list("queue");
   BOOST_REQUIRE_GE(queue.size(), 2u);
   BOOST_REQUIRE(queue[0] == earlier && queue[1] == later);
   f.stake("100.0000 SYS", "0.0000 SYS", later, later);
   f.expected_votes[later] += 1000000;
   BOOST_CHECK_EQUAL(f.expected_votes.at(later), f.expected_votes.at(earlier));
   BOOST_CHECK(earlier < later);
   f.check_ranking(); // deterministic name ordering at an exact integer tie
   f.stake("0.0001 SYS", "0.0000 SYS", later, later);
   ++f.expected_votes[later];
   f.check_ranking(); // a single smallest unit, not a rounded double
   BOOST_CHECK(f.list("active") == active);
   BOOST_CHECK(f.list("queue") == queue);
   f.block();
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), earlier.to_string()); // rank cannot jump FIFO
   f.completed(22);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), later.to_string());
   f.completed(23);
   BOOST_REQUIRE_GT(f.produced[earlier], 0u);
   BOOST_REQUIRE_GT(f.produced[later], 0u);
   const auto history = f.producer(later)["unpaid_blocks"].as_uint64();
   f.choose(later, earlier);
   f.expected_votes[earlier] += f.expected_votes[later]; f.expected_votes[later] = 0;
   f.check_ranking();
   BOOST_CHECK_EQUAL(f.producer(later)["total_votes"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.producer(later)["unpaid_blocks"].as_uint64(), history);
   f.block();
   f.until([&] { const auto a=f.list("active"); return std::find(a.begin(), a.end(), later) == a.end(); });
   BOOST_CHECK(!f.candidate(later)["banned"].as_bool()); // loss of votes is not an offline blacklist
   f.check_split();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(twenty_one_member_quorum_loss_and_recovery_before_blacklist_windows) {
   current_committee_fixture f(21);
   f.completed(21);
   const auto members = f.list("active");
   const auto policy = *f.chain.control->head_active_finalizer_policy();
   const auto checkpoint = f.chain.last_irreversible_block_id();
   const auto checkpoint_num = f.chain.last_irreversible_block_num();
   BOOST_REQUIRE_EQUAL(policy.finalizers.size(), 21u);
   BOOST_REQUIRE_EQUAL(policy.threshold, 15u);
   f.online.assign(members.begin(), members.begin()+14);
   f.restart(f.online);
   for (unsigned i=0;i<8;++i) f.block(); // drain old QCs, not three empty windows
   const auto halted = f.chain.last_irreversible_block_num();
   const auto counts = f.produced;
   const auto missing = f.production_slots(252);
   BOOST_CHECK_EQUAL(missing.first, 14u*12);
   BOOST_CHECK_EQUAL(missing.second, 7u*12);
   BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), halted);
   BOOST_REQUIRE(*f.chain.control->head_active_finalizer_policy() == policy);
   BOOST_REQUIRE_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   for (size_t i=14;i<members.size();++i) BOOST_CHECK_EQUAL(f.produced[members[i]], counts.at(members[i]));
   f.online.push_back(members[14]); f.restart(f.online);
   f.production_slots(126);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), halted);
   BOOST_REQUIRE(*f.chain.control->head_active_finalizer_policy() == policy);
   BOOST_REQUIRE(f.list("active") == members);
   BOOST_REQUIRE_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.online=members; f.restart(f.online);
   f.verify_full_production_round();
   BOOST_CHECK(f.chain.control->fetch_block_by_number(checkpoint_num)->calculate_id() == checkpoint);
   f.check_ranking(); f.check_split(); f.observer_replay();
   BOOST_TEST_MESSAGE("14/21 cannot finalize; 15/21 resumes before three zero windows. Historical LIB, resources and replay agree.");
}

BOOST_AUTO_TEST_SUITE_END()
