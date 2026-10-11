// Socket-free characterization of the CURRENT production-contract election.
// Real WASM/actions/controller/BLS/QCs are used; neither setprods nor setfinalizer
// overrides the election. One tester hosts the simulated signers. Disabling
// local BLS keys + skipping their slots models unavailable members, NOT OS/P2P
// failures or independent nodeos processes. A second controller validates replay.
// This does NOT implement/accept the planned 210-candidate queue or blacklist.
namespace {
struct committee_boundary_fixture : resource_split_fixture {
   std::map<name, int64_t> expected_votes;

   committee_boundary_fixture() : resource_split_fixture(true, 22) {
      deploy("eosio.bpay"_n, "eosio.bpay");
      for (size_t i = 0; i < nodes.size(); ++i) {
         const auto node = nodes[i];
         call("buyram"_n, fc::mutable_variant_object()("payer", "eosio")("receiver", node)
            ("quant", "100.0000 SYS"), "eosio"_n);
         chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
            ("from", "eosio")("to", node)("quantity", "5000.0000 SYS")("memo", "local committee test"));
         if (i != 0) register_node(node); // resource fixture already registered node 0
         const int64_t tokens = 2200 - 100 * i;
         const auto half = std::to_string(tokens / 2) + ".0000 SYS";
         stake(half, half, node, node);
         choose(node, node);
         expected_votes[node] = tokens * 10000;
         produce_checked();
      }
      chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
         ("from", "eosio")("to", "alice")("quantity", "170000000.0000 SYS")("memo", "normal activation funds"));
      seal();
      produce_checked();
      auto signers = nodes;
      signers.push_back("eosio"_n); // needed until the real election replaces genesis
      restart_signers(signers);
      BOOST_REQUIRE_EQUAL(nodes.size(), 22u);
      BOOST_REQUIRE(state().developer == "developer"_n);
      BOOST_REQUIRE_EQUAL(state().review_seconds, 1209600u);
      BOOST_REQUIRE_EQUAL(chain.control->head_active_producers().producers.size(), 1u);
   }

   signed_block_ptr produce_checked(fc::microseconds delta = base_tester::default_skip_time) {
      auto result = chain.produce_block_ex(delta);
      BOOST_REQUIRE(result.block);
      // onblock failures must not be silently swallowed by a successful block.
      if (result.onblock_trace && result.onblock_trace->except)
         BOOST_FAIL(result.onblock_trace->except->to_detail_string());
      return result.block;
   }

   void advance(uint32_t blocks) {
      // Unlike produce_blocks(), this votes on EVERY block, including samples.
      for (uint32_t i = 0; i < blocks; ++i) produce_checked();
   }

   void restart_signers(const std::vector<name>& names) {
      // set_node_finalizers is startup-only. Preserve the real chain and finalizer
      // safety file; change only configured LOCAL signing keys on restart.
      const auto id = chain.head().id();
      chain.close();
      chain.open();
      chain.set_node_finalizers(names);
      chain.do_check_for_votes(false); // missing votes are intentional in fault cases
      BOOST_REQUIRE(chain.head().id() == id);
   }

   void choose(name voter, name producer) {
      call("voteproducer"_n, fc::mutable_variant_object()("voter", voter)("proxy", "")
         ("producers", std::vector<name>{producer}), voter);
   }

   auto ranked(uint32_t limit = 50, const std::string& lower = "") const {
      std::optional<eosio::chain_apis::tracked_votes> tracked;
      return eosio::chain_apis::read_only(*chain.control, {}, {}, tracked, fc::seconds(5), fc::seconds(5), nullptr)
         .get_producers({.json = true, .lower_bound = lower, .limit = limit}, fc::time_point::maximum());
   }

   std::vector<name> ranking_order() const {
      auto order = nodes;
      std::sort(order.begin(), order.end(), [&](name a, name b) {
         return expected_votes.at(a) == expected_votes.at(b) ? a < b : expected_votes.at(a) > expected_votes.at(b);
      });
      return order;
   }

   void check_ranking() {
      const auto result = ranked();
      const auto expected = ranking_order();
      BOOST_REQUIRE_EQUAL(result.rows.size(), 22u);
      BOOST_REQUIRE_EQUAL(result.vote_schema, "integer-v2");
      BOOST_REQUIRE(result.total_producer_vote_amount);
      int64_t total = 0;
      for (size_t i = 0; i < expected.size(); ++i) {
         const auto n = expected[i];
         BOOST_CHECK_EQUAL(result.rows[i]["owner"].as_string(), n.to_string());
         BOOST_CHECK_EQUAL(result.rows[i]["total_votes"].as_string(), std::to_string(expected_votes.at(n)));
         total += expected_votes.at(n);
      }
      BOOST_CHECK_EQUAL(*result.total_producer_vote_amount, std::to_string(total));
      const auto first = ranked(21);
      BOOST_REQUIRE_EQUAL(first.rows.size(), 21u);
      BOOST_REQUIRE_EQUAL(first.more, expected.back().to_string());
      const auto last = ranked(21, first.more);
      BOOST_REQUIRE_EQUAL(last.rows.size(), 1u);
      BOOST_CHECK_EQUAL(last.rows.front()["owner"].as_string(), expected.back().to_string());
   }

   std::vector<name> top21() const {
      auto result = ranking_order();
      result.resize(21);
      std::sort(result.begin(), result.end());
      return result;
   }

   bool settled(const std::vector<name>& expected) const {
      std::vector<name> actual;
      for (const auto& p : chain.control->head_active_producers().producers) actual.push_back(p.producer_name);
      const auto policy = chain.control->head_active_finalizer_policy();
      if (actual != expected || !policy || policy->finalizers.size() != expected.size() ||
          policy->threshold != (expected.size() * 2) / 3 + 1 ||
          chain.control->head_pending_finalizer_policy() || chain.control->pending_producers()) return false;
      std::vector<name> finalizers;
      for (const auto& f : policy->finalizers) {
         if (f.weight != 1) return false;
         finalizers.emplace_back(f.description);
      }
      std::sort(finalizers.begin(), finalizers.end());
      return finalizers == expected;
   }

   void wait_committee(const std::vector<name>& expected) {
      for (uint32_t i = 0; i < 1100 && !settled(expected); ++i) produce_checked();
      BOOST_REQUIRE_MESSAGE(settled(expected), "Real election/proposer/finalizer policy did not settle");
      BOOST_REQUIRE_EQUAL(chain.control->head_active_finalizer_policy()->threshold, 15u);
      BOOST_REQUIRE_EQUAL(chain.control->head_active_producers().producers.size(), 21u);
   }

   void activate() {
      choose("alice"_n, nodes[0]);
      call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", "160000000.0000 SYS"));
      call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "160000000.0000 SYS"));
      expected_votes[nodes[0]] += 1600000000000LL;
      produce_checked();
      BOOST_REQUIRE_GT(row("globalv2"_n, "globalv2"_n.to_uint64_t())["total_activated_stake"].as_int64(), 1500000000000LL);
      check_ranking();
      wait_committee(top21());
      BOOST_TEST_MESSAGE("22 positive-vote candidates elected exactly 21 producers/finalizers; threshold is 15");
   }

   std::map<name, uint32_t> verify_rounds(const std::vector<name>& expected, unsigned rounds = 2) {
      BOOST_REQUIRE(settled(expected));
      const uint32_t span = expected.size() * config::producer_repetitions;
      std::vector<signed_block_ptr> blocks;
      for (uint32_t i = 0; i < span * (rounds + 1); ++i) blocks.push_back(produce_checked());
      advance(4);
      BOOST_REQUIRE_GE(chain.last_irreversible_block_num(), blocks.back()->block_num());
      std::map<uint32_t, std::map<name, uint32_t>> counts_by_round;
      std::map<name, uint32_t> counts;
      bool ordered = true, consecutive = true;
      for (size_t i = 0; i < blocks.size(); ++i) {
         const auto& b = blocks[i];
         const auto scheduled = expected[(b->timestamp.slot % span) / config::producer_repetitions];
         ordered &= b->is_proper_svnn_block() && b->producer == scheduled;
         if (i) consecutive &= b->timestamp.slot == blocks[i - 1]->timestamp.slot + 1;
         ++counts[b->producer];
         ++counts_by_round[b->timestamp.slot / span][b->producer];
      }
      BOOST_CHECK(ordered);
      BOOST_CHECK(consecutive);
      unsigned complete = 0;
      for (const auto& [round, by_name] : counts_by_round) {
         if (by_name.size() == expected.size() && std::all_of(by_name.begin(), by_name.end(), [](const auto& entry) {
            return entry.second == config::producer_repetitions;
         })) ++complete;
      }
      BOOST_CHECK_GE(complete, rounds);
      BOOST_CHECK_EQUAL(counts.size(), 21u);
      BOOST_REQUIRE(settled(expected));
      BOOST_TEST_MESSAGE("Finalized sample: " << blocks.size() << " blocks, " << complete
                         << " full rounds; each member makes 12 blocks/round, sorted by account name");
      return counts;
   }

   fc::variant producer(name n) const { return row("producersv2"_n, n.to_uint64_t()); }

   void verify_independent_observer() {
      fc::temp_directory observer_dir;
      tester observer(base_tester::default_config(observer_dir).first, genesis);
      observer.do_check_for_votes(false); // validating observer has NO finalizer keys
      const auto head_num = chain.head().block_num();
      for (uint32_t n = 2; n <= head_num; ++n) {
         auto block = chain.control->fetch_block_by_number(n);
         BOOST_REQUIRE(block);
         observer.push_block(block); // native signature, transaction, WASM and QC validation
      }
      BOOST_CHECK(observer.head().id() == chain.head().id());
      BOOST_CHECK(observer.last_irreversible_block_id() == chain.last_irreversible_block_id());
      for (auto n : nodes)
         BOOST_CHECK(observer.get_row_by_account("eosio"_n, "eosio"_n, "producersv2"_n, n) ==
                     chain.get_row_by_account("eosio"_n, "eosio"_n, "producersv2"_n, n));
      BOOST_TEST_MESSAGE("Separate observer controller validated " << head_num
                         << " blocks; head and LIB agree without network sockets");
   }

   struct online_sample { uint32_t produced = 0, skipped = 0; };
   online_sample produce_online(const std::vector<name>& online, uint32_t slots) {
      const auto start = chain.head().header().timestamp.slot;
      online_sample result;
      for (uint32_t slot = start + 1; slot <= start + slots; ++slot) {
         const block_timestamp_type when{slot};
         const auto scheduled = chain.control->head_active_producers(when).get_scheduled_producer(when).producer_name;
         if (std::find(online.begin(), online.end(), scheduled) == online.end()) {
            ++result.skipped;
            continue; // do NOT let tester automatically sign for an unavailable producer
         }
         const auto delta = fc::microseconds(int64_t(slot - chain.head().header().timestamp.slot) * config::block_interval_us);
         const auto block = produce_checked(delta);
         BOOST_REQUIRE(block->producer == scheduled);
         ++result.produced;
      }
      return result;
   }
};
}

BOOST_AUTO_TEST_SUITE(pureeos_committee_boundary_tests)

BOOST_AUTO_TEST_CASE(twenty_two_candidates_select_twenty_one_and_exclude_zero_output_rewards) {
   committee_boundary_fixture f;
   f.activate();
   const auto outside = f.nodes.back();
   BOOST_REQUIRE_EQUAL(f.producer(outside)["unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", outside), outside),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "standby rewards are disabled"); });
   const auto counts = f.verify_rounds(f.top21());
   BOOST_CHECK(counts.find(outside) == counts.end());
   BOOST_CHECK_EQUAL(f.producer(outside)["unpaid_blocks"].as_uint64(), 0u);
   f.check_ranking();
   f.check_split();
   BOOST_TEST_MESSAGE("Rank 22 stays registered but produces zero blocks and cannot claim rewards; no automatic 210 rotation exists");
}

BOOST_AUTO_TEST_CASE(tied_boundary_one_unit_overtake_and_single_choice_replacement) {
   committee_boundary_fixture f;
   f.activate();
   const auto old_member = f.nodes[20], newcomer = f.nodes[21];
   f.stake("100.0000 SYS", "0.0000 SYS", newcomer, newcomer);
   f.expected_votes[newcomer] += 1000000;
   f.check_ranking();
   BOOST_CHECK(f.ranking_order()[20] == old_member); // equal votes: smaller account name wins
   f.advance(260);
   BOOST_REQUIRE(f.settled(f.top21()));

   const auto old_generation = f.chain.control->head_active_finalizer_policy()->generation;
   f.stake("0.0001 SYS", "0.0000 SYS", newcomer, newcomer);
   ++f.expected_votes[newcomer];
   f.check_ranking();
   BOOST_CHECK(f.ranking_order()[20] == newcomer);
   f.produce_checked();
   const auto replacement = f.top21();
   f.wait_committee(replacement);
   BOOST_CHECK_GT(f.chain.control->head_active_finalizer_policy()->generation, old_generation);
   const auto counts = f.verify_rounds(replacement);
   BOOST_CHECK(counts.find(old_member) == counts.end());
   BOOST_CHECK_GT(counts.at(newcomer), 0u);
   BOOST_TEST_MESSAGE("Rank 22 overtook rank 21 by 0.0001 SYS; elected/finalizer membership changed, newcomer produced finalized blocks");

   // Atomic one-choice switch moves all principal, not a copy of the vote.
   f.choose(newcomer, old_member);
   f.expected_votes[old_member] += f.expected_votes[newcomer];
   f.expected_votes[newcomer] = 0;
   f.check_ranking();
   f.produce_checked();
   f.wait_committee(f.top21());
   const auto restored = f.verify_rounds(f.top21(), 1);
   BOOST_CHECK(restored.find(newcomer) == restored.end());
   BOOST_CHECK_GT(restored.at(old_member), 0u);
   f.check_split();
   // Abort the uncommitted onblock update before comparing persisted table rows.
   f.chain.control->abort_block();
   f.verify_independent_observer();
}

BOOST_AUTO_TEST_CASE(twenty_one_members_six_offline_seven_offline_and_recovery) {
   committee_boundary_fixture f;
   f.activate();
   const auto members = f.top21();
   const std::vector<name> fifteen(members.begin(), members.begin() + 15);
   const std::vector<name> fourteen(members.begin(), members.begin() + 14);
   const auto policy = *f.chain.control->head_active_finalizer_policy();
   const auto checkpoint_num = f.chain.last_irreversible_block_num();
   const auto checkpoint_id = f.chain.last_irreversible_block_id();

   f.restart_signers(fifteen);
   f.produce_online(fifteen, 252); // drain pre-fault QCs and prior producer bookkeeping
   const auto before = f.chain.last_irreversible_block_num();
   std::map<name, uint64_t> unpaid;
   for (size_t i = 15; i < members.size(); ++i)
      unpaid[members[i]] = f.producer(members[i])["unpaid_blocks"].as_uint64();
   const auto sample = f.produce_online(fifteen, 504);
   BOOST_CHECK_EQUAL(sample.produced, 360u);
   BOOST_CHECK_EQUAL(sample.skipped, 144u);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), before);
   for (const auto& [n, count] : unpaid) BOOST_CHECK_EQUAL(f.producer(n)["unpaid_blocks"].as_uint64(), count);
   BOOST_CHECK(*f.chain.control->head_active_finalizer_policy() == policy);
   BOOST_TEST_MESSAGE("21 members / 6 unavailable: 15 signers finalize; two rounds produce 360 blocks and miss 144 slots; offline unpaid counters do not grow");

   f.restart_signers(fourteen);
   f.produce_online(fourteen, 252); // drain any pre-fault quorum certificates
   const auto halted = f.chain.last_irreversible_block_num();
   const auto failed = f.produce_online(fourteen, 504);
   BOOST_CHECK_EQUAL(failed.produced, 336u);
   BOOST_CHECK_EQUAL(failed.skipped, 168u);
   BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), halted);
   BOOST_CHECK(*f.chain.control->head_active_finalizer_policy() == policy);
   BOOST_TEST_MESSAGE("21 members / 7 unavailable: 14 signers still build unfinalized blocks, but LIB stays at " << halted);

   f.restart_signers(fifteen);
   f.produce_online(fifteen, 252);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), halted);
   BOOST_TEST_MESSAGE("Restoring signer 15 resumes finality without changing the 21-member policy or threshold");
   f.restart_signers(members);
   f.verify_rounds(members, 1);
   BOOST_CHECK(f.chain.control->fetch_block_by_number(checkpoint_num)->calculate_id() == checkpoint_id);
   f.check_ranking();
   f.check_split();
   BOOST_TEST_MESSAGE("All 21 simulated members resume scheduled production; historical finalized block survives. Planned three-window blacklist is NOT implemented/accepted here");
}

BOOST_AUTO_TEST_SUITE_END()
