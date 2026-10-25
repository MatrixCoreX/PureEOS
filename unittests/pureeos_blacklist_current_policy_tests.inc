// Actual system WASM and native Savanna handoffs, never privileged table writes.
// The pre-fix boundary reproduction is preserved in its immutable attempt-2
// report/checkpoint; this is the regression for the user-approved eviction rule.
BOOST_AUTO_TEST_SUITE(pureeos_blacklist_current_policy_tests)
BOOST_AUTO_TEST_CASE(no_replacement_evicts_banned_member_and_later_refills_one_seat) {
   sequential_rotation_fixture f(22);
   const auto spare = f.nodes[21];
   // Initially there are exactly 21 qualified accounts: no replacement exists.
   f.call("voteproducer"_n, fc::mutable_variant_object()("voter", spare)("proxy", "")
      ("producers", std::vector<name>{}), spare);
   f.completed(21);
   const auto victim = f.nodes[0];
   const auto members = f.list("active");
   const auto unpaid = [&] { return f.row("producersv2"_n, victim.to_uint64_t())["unpaid_blocks"].as_uint64(); };
   BOOST_REQUIRE(std::find(members.begin(), members.end(), victim) != members.end());
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 15u);
   BOOST_REQUIRE_GT(unpaid(), 0u);
   f.online.erase(std::find(f.online.begin(), f.online.end(), victim));
   f.restart(f.online);
   f.until([&] { return f.candidate(victim)["banned"].as_bool(); }, 1000);
   BOOST_REQUIRE_EQUAL(f.candidate(victim)["zero_windows"].as_uint64(), 3u);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1 &&
      f.rotation()["incoming"].as_string().empty(); }, 120);
   // row() may see speculative onblock for the pending next block. Persist
   // that submission before testing restart durability; never compare a
   // rolled-back pending state with the committed database after reopen.
   f.block();
   f.chain.control->abort_block();
   const auto proposed = f.rotation();
   BOOST_REQUIRE_EQUAL(proposed["stage"].as_uint64(), 1u);
   BOOST_REQUIRE_EQUAL(proposed["outgoing"].as_string(), victim.to_string());
   auto survivors = members;
   survivors.erase(std::find(survivors.begin(), survivors.end(), victim));
   BOOST_REQUIRE(f.list("target") == survivors);
   // Submission does not pretend that native consensus has already switched.
   BOOST_REQUIRE(f.list("active") == members);
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 15u);
   const std::vector<name> insufficient(survivors.begin(), survivors.begin() + 14);
   f.restart(insufficient);
   f.production_slots(21 * config::producer_repetitions); // drain pre-stop QCs
   const auto halted = f.chain.last_irreversible_block_num();
   f.production_slots(21 * config::producer_repetitions);
   BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), halted);
   BOOST_CHECK_EQUAL(f.rotation()["stage"].as_uint64(), 1u);
   BOOST_CHECK(f.list("active") == members);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 15u);
   // Recovery DURING the handoff must not cancel a finalized permanent ban.
   // Any blocks still valid under the OLD policy remain historical entitlement.
   f.online.push_back(victim);
   f.chain.control->abort_block();
   const auto before_restart = resource_json(f.rotation());
   f.restart(f.online);
   BOOST_REQUIRE_EQUAL(resource_json(f.rotation()), before_restart);
   f.snapshot_check(); // restore while the real native policy handoff is pending
   f.until([&] { return f.rotation()["stage"].as_uint64() == 0 && f.list("active") == survivors; });
   BOOST_REQUIRE_EQUAL(f.rotation()["completed"].as_uint64(), 21u); // eviction != admission
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_producers().producers.size(), 20u);
   const auto policy = f.chain.control->head_active_finalizer_policy();
   BOOST_REQUIRE_EQUAL(policy->finalizers.size(), 20u);
   BOOST_REQUIRE_EQUAL(policy->threshold, 14u);
   for (const auto& p : f.chain.control->head_active_producers().producers) BOOST_REQUIRE(p.producer_name != victim);
   for (const auto& p : policy->finalizers) BOOST_REQUIRE(p.description != victim.to_string());
   const auto before = unpaid();
   const auto made = f.produced[victim];
   const auto evidence = resource_json(f.candidate(victim)["evidence_slots"]);
   const auto ban_block = f.candidate(victim)["ban_final_block"].as_uint64();
   const auto before_lib = f.chain.last_irreversible_block_num();
   f.restart(f.online);
   f.production_slots(3 * 21 * config::producer_repetitions);
   BOOST_CHECK(f.candidate(victim)["banned"].as_bool());
   BOOST_CHECK(f.list("active") == survivors);
   BOOST_CHECK_EQUAL(f.produced[victim], made);
   BOOST_CHECK_EQUAL(unpaid(), before);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), before_lib);
   BOOST_CHECK(f.rotation()["penalty_paused"].as_bool());
   BOOST_CHECK_EQUAL(resource_json(f.candidate(victim)["evidence_slots"]), evidence);
   BOOST_CHECK_EQUAL(f.candidate(victim)["ban_final_block"].as_uint64(), ban_block);
   // Neither re-registration nor replacement proposer/finalizer keys clear ban.
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", victim), victim);
   f.call("regproducer"_n, fc::mutable_variant_object()("producer", victim)
      ("producer_key", f.chain.get_public_key(victim, "replacement"))("url", "")("location", 0), victim);
   const auto [priv, pub, pop] = get_bls_key("evictedkey"_n);
   f.call("regfinkey"_n, fc::mutable_variant_object()("finalizer_name", victim)
      ("finalizer_key", pub.to_string())("proof_of_possession", pop.to_string()), victim);
   f.call("actfinkey"_n, fc::mutable_variant_object()("finalizer_name", victim)("finalizer_key", pub.to_string()), victim);
   f.block();
   f.restart(f.online);
   f.production_slots(2 * 20 * config::producer_repetitions);
   BOOST_CHECK(f.list("active") == survivors);
   BOOST_CHECK(f.candidate(victim)["banned"].as_bool());
   BOOST_CHECK_EQUAL(unpaid(), before);
   BOOST_CHECK_EQUAL(f.produced[victim], made);
   f.choose(spare, spare); // a real new qualified candidate, not a policy override
   f.completed(22);
   survivors.push_back(spare);
   BOOST_CHECK(f.list("active") == survivors);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 15u);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_producers().producers.size(), 21u);
   BOOST_CHECK_GT(f.produced[spare], 0u);
   BOOST_CHECK_EQUAL(unpaid(), before);
   BOOST_CHECK(f.candidate(victim)["banned"].as_bool());
   f.snapshot_check();
   f.observer_replay();
   BOOST_TEST_MESSAGE("No spare: native 21/15->20/14 eviction; returning/re-keyed banned member cannot produce or accrue rewards; history intact; new candidate refills one seat to 21/15");
}
BOOST_AUTO_TEST_CASE(six_finalized_bans_exit_sequentially_even_after_committee_drops_below21) {
   sequential_rotation_fixture f(21);
   f.completed(21);
   const auto members = f.list("active");
   const std::vector<name> survivors(members.begin(), members.begin()+15);
   const std::vector<name> victims(members.begin()+15, members.end());
   std::map<name,uint64_t> unpaid;
   std::map<name,uint32_t> made;
   std::map<name,std::string> balances;
   const auto balance = [&](name n) {
      return resource_json(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value,
         n, "eosio.token"_n));
   };
   for (auto n : victims) {
      unpaid[n]=f.row("producersv2"_n,n.to_uint64_t())["unpaid_blocks"].as_uint64();
      made[n]=f.produced[n];balances[n]=balance(n);
      BOOST_REQUIRE_GT(unpaid[n],0u);
   }
   f.online=survivors;
   f.restart(f.online);
   // Consecutive missed owners are finalized together when the post-gap QC
   // arrives. They must all earn their bans while the actual policy is 21.
   f.until([&] { return f.rotation()["total_banned"].as_uint64()>0; });
   BOOST_REQUIRE_EQUAL(f.rotation()["total_banned"].as_uint64(),6u);
   for (auto n : victims) {
      BOOST_REQUIRE(f.candidate(n)["banned"].as_bool());
      BOOST_REQUIRE_EQUAL(f.candidate(n)["zero_windows"].as_uint64(),3u);
   }
   // block() validates <=1 proposer/finalizer seat change at EVERY real block.
   // Existing finalized bans keep exiting after the first 21->20 handoff;
   // pausing NEW under-21 penalties must not strand those already-banned seats.
   f.until([&] { return f.rotation()["stage"].as_uint64()==0 && f.list("active")==survivors; });
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_producers().producers.size(),15u);
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_finalizer_policy()->finalizers.size(),15u);
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold,11u);
   BOOST_REQUIRE_EQUAL(f.rotation()["completed"].as_uint64(),21u);
   f.online=members; // every evicted producer comes back, including its old keys
   f.restart(f.online);
   const auto before_lib=f.chain.last_irreversible_block_num();
   f.production_slots(21*config::producer_repetitions);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(),before_lib);
   BOOST_CHECK(f.list("active")==survivors);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(),6u);
   BOOST_CHECK(f.rotation()["penalty_paused"].as_bool());
   for (auto n : victims) {
      BOOST_CHECK(f.candidate(n)["banned"].as_bool());
      BOOST_CHECK_EQUAL(f.produced[n],made[n]);
      BOOST_CHECK_EQUAL(f.row("producersv2"_n,n.to_uint64_t())["unpaid_blocks"].as_uint64(),unpaid[n]);
      BOOST_CHECK_EQUAL(balance(n),balances[n]);
   }
   f.snapshot_check();f.observer_replay();
   BOOST_TEST_MESSAGE("Six co-finalized bans: native 21->20->19->18->17->16->15 one-seat handoffs, quorum 11; all six return without producing/new rewards; balances/history intact");
}
BOOST_AUTO_TEST_SUITE_END()
