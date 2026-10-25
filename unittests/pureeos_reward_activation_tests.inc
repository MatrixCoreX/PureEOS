// Real WASM/transactions with a deliberately small issued supply. No table
// mutation, threshold override, or shortened production clock is used.
namespace {
struct positive_vote_reward_fixture : resource_split_fixture {
   explicit positive_vote_reward_fixture(std::filesystem::path root = rotation_contracts_for_test(),
                                         std::string issued_supply = "10000.0000 SYS")
      : resource_split_fixture(true, 2, std::move(root), true, true, std::move(issued_supply)) {
      register_node(nodes[1]);
      // Local signing keys are startup-only; preserve/commit the real chain
      // before enabling the ordinary producers' keys, as in rotation fixtures.
      chain.produce_block();
      chain.close();
      chain.open();
      chain.set_node_finalizers(std::vector<name>{"eosio"_n, nodes[0], nodes[1]});
      chain.do_check_for_votes(false);
      // Resource grants belong to eosio, not alice: they do not give her votes.
      stake("1.0000 SYS", "1.0000 SYS", "eosio"_n, "alice"_n);
   }
   fc::variant global() const { return row("globalv2"_n, "globalv2"_n.to_uint64_t()); }
   fc::variant rewards() const { return row("rewardpolicy"_n, "rewardpolicy"_n.to_uint64_t()); }
   int64_t balance(name owner) const {
      const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
      if (chain.get_row_by_account("eosio.token"_n, owner, "accounts"_n, name{code}).empty()) return 0;
      return reward_fixture::units(row("accounts"_n, code, owner, "eosio.token"_n)["balance"]);
   }
   int64_t supply() const {
      const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
      return reward_fixture::units(row("stat"_n, code, name{code}, "eosio.token"_n)["supply"]);
   }
   void clear_vote() {
      call("voteproducer"_n, fc::mutable_variant_object()("voter", "alice")("proxy", "")
         ("producers", std::vector<name>{}));
   }
   void one_unit() { stake("0.0001 SYS", "0.0000 SYS"); }
   void settle_single_producer() {
      for (size_t i = 0; i < 2000; ++i) {
         chain.produce_block();
         const auto& producers = chain.control->head_active_producers().producers;
         const auto& finalizers = chain.control->head_active_finalizer_policy()->finalizers;
         if (producers.size() == 1 && producers[0].producer_name == nodes[0] && finalizers.size() == 1 &&
             finalizers[0].description == nodes[0].to_string()) {
            chain.produce_blocks(12);
            return;
         }
      }
      BOOST_FAIL("ordinary one-producer policy did not become active");
   }
   void require_unactivated() const {
      BOOST_CHECK_EQUAL(global()["thresh_activated_stake_time"].as_string(), "1970-01-01T00:00:00.000");
      BOOST_CHECK_EQUAL(rewards()["start_time"].as_string(), "1970-01-01T00:00:00.000");
      BOOST_CHECK_EQUAL(global()["total_unpaid_blocks"].as_uint64(), 0u);
      BOOST_CHECK_EQUAL(global()["perblock_bucket"].as_int64(), 0);
   }
};

#ifdef PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR
void reviewed_positive_vote_upgrade(resource_split_fixture& f) {
   const auto base = rotation_contracts_for_test() / "eosio.system/eosio.system";
   const auto code = read_wasm((base.string() + ".wasm").c_str());
   f.upgrade_code.assign(code.begin(), code.end());
   abi_def abi;
   fc::from_variant(fc::json::from_file(base.string() + ".abi"), abi);
   f.upgrade_abi = fc::raw::pack(abi);
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}},
           f.proposal("eosio"_n))}, "developer"_n);
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   f.wait_review();
   f.execute();
}
#endif
}

BOOST_AUTO_TEST_SUITE(pureeos_reward_activation_tests)

BOOST_AUTO_TEST_CASE(zero_amount_claim_preserves_block_credits_cooldown_and_fractional_interval) {
   positive_vote_reward_fixture f(rotation_contracts_for_test(), "1100.0000 SYS");
   f.seal();
   f.vote();
   f.one_unit();
   f.settle_single_producer();
   const auto owner = f.nodes[0];
   // Exercise the real fixed policy, never edit a table or shorten its period.
   // Ten four-year eras fit the native block timestamp range and make a daily
   // reward smaller than one SYS unit even with this fixture's bootstrap funds.
   for (unsigned era = 1; era <= 10; ++era) {
      f.chain.produce_block(fc::days(1461));
      f.chain.produce_blocks(2);
      const auto before = f.balance(owner);
      f.call("claimrewards"_n, fc::mutable_variant_object()("owner", owner), owner);
      BOOST_REQUIRE_GT(f.balance(owner), before);
      BOOST_REQUIRE_EQUAL(f.global()["perblock_bucket"].as_int64(), 0);
      BOOST_REQUIRE_EQUAL(f.global()["total_unpaid_blocks"].as_uint64(), 0u);
      // Commit the claim before the next multi-year jump. The tester replays
      // pending transactions when starting a later block; their short-lived
      // headers must not be carried into the next era.
      f.chain.produce_block();
   }
   BOOST_REQUIRE_EQUAL(f.rewards()["current_era"].as_uint64(), 10u);
   f.chain.produce_block(fc::days(2));
   f.chain.produce_blocks(2);
   const auto global = resource_json(f.global());
   const auto producer = resource_json(f.row("producersv2"_n, owner.to_uint64_t()));
   const auto policy = resource_json(f.rewards());
   const auto supply = f.supply(), balance = f.balance(owner), bucket_balance = f.balance("eosio.bpay"_n);
   const auto unpaid = f.row("producersv2"_n, owner.to_uint64_t())["unpaid_blocks"].as_uint64();
   BOOST_REQUIRE_GT(unpaid, 0u);
   const auto from = reward_fixture::when(f.global()["last_pervote_bucket_fill"]);
   const auto elapsed = (f.chain.control->pending_block_time() - from).count();
   const long double accrued = static_cast<long double>(supply) * std::ldexp(
      static_cast<long double>(reward_fixture::base_rate), -10) * elapsed / (364LL * reward_fixture::day_us);
   BOOST_REQUIRE_GT(accrued, 0);
   BOOST_REQUIRE_LT(accrued, 1);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", owner), owner),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "no payable block rewards yet"); });
   BOOST_CHECK_EQUAL(resource_json(f.global()), global);
   BOOST_CHECK_EQUAL(resource_json(f.row("producersv2"_n, owner.to_uint64_t())), producer);
   BOOST_CHECK_EQUAL(resource_json(f.rewards()), policy);
   BOOST_CHECK_EQUAL(f.supply(), supply);
   BOOST_CHECK_EQUAL(f.balance(owner), balance);
   BOOST_CHECK_EQUAL(f.balance("eosio.bpay"_n), bucket_balance);
   // The rejected claim must not reset the fill interval: four days accrue one
   // whole unit, whereas two separate truncated two-day fills both accrue zero.
   // The old-WASM negative control unexpectedly accepts the zero claim. Commit
   // that transaction too, so its intended assertion failures cannot be hidden
   // behind a later expired-transaction abort when advancing two more days.
   f.chain.produce_block();
   f.chain.produce_block(fc::days(2));
   f.chain.produce_blocks(2);
   f.call("claimrewards"_n, fc::mutable_variant_object()("owner", owner), owner);
   BOOST_CHECK_EQUAL(f.supply() - supply, 1);
   BOOST_CHECK_EQUAL(f.balance(owner) - balance, 1);
   BOOST_CHECK_EQUAL(f.global()["total_unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, owner.to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
}

BOOST_AUTO_TEST_CASE(liquid_balance_registration_empty_votes_and_unvoted_stake_do_not_activate) {
   positive_vote_reward_fixture f;
   f.seal();
   BOOST_CHECK_EQUAL(f.supply(), 100000000LL);
   f.require_unactivated();
   f.vote(); // a zero-token choice is allowed before the first REX buy
   f.chain.produce_blocks(3);
   f.vote(); // repeated empty choices must not manufacture activation
   f.require_unactivated();
   f.clear_vote();
   f.one_unit(); // staking without allocating it is not a producer vote
   f.chain.produce_block();
   f.chain.produce_block(fc::days(2));
   f.chain.produce_block();
   f.require_unactivated();
   BOOST_CHECK_EQUAL(f.global()["total_producer_vote_weight"].as_int64(), 0);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "no produced blocks"); });
   BOOST_CHECK_EXCEPTION(f.call("undelegatebw"_n, fc::mutable_variant_object()("from", "alice")("receiver", "alice")
      ("unstake_net_quantity", "0.0001 SYS")("unstake_cpu_quantity", "0.0000 SYS")), fc::exception,
      [](const auto& e) { return resource_split_fixture::message(e, "first positive producer vote"); });
   BOOST_CHECK_THROW(f.vote("notanode"_n), fc::exception);
   f.require_unactivated();
}

BOOST_AUTO_TEST_CASE(one_smallest_unit_vote_and_one_producer_can_issue_and_claim_with_small_supply) {
   positive_vote_reward_fixture f;
   f.seal();
   f.vote();
   const auto activation = f.chain.control->pending_block_time();
   f.one_unit();
   BOOST_CHECK_EQUAL(f.global()["total_activated_stake"].as_int64(), 1);
   BOOST_CHECK(reward_fixture::when(f.rewards()["start_time"]) == activation);
   BOOST_CHECK(reward_fixture::when(f.global()["thresh_activated_stake_time"]) == activation);
   f.settle_single_producer();
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_producers().producers.size(), 1u);
   BOOST_REQUIRE(f.chain.control->head_active_producers().producers.front().producer_name == f.nodes[0]);
   BOOST_REQUIRE_GT(f.global()["total_unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "within past day"); });
   f.chain.produce_block(fc::days(2));
   f.chain.produce_blocks(2);
   const auto old_supply = f.supply(), old_balance = f.balance(f.nodes[0]);
   const auto from = reward_fixture::when(f.global()["last_pervote_bucket_fill"]);
   const auto to = f.chain.control->pending_block_time();
   const auto expected = int64_t(static_cast<long double>(old_supply) * reward_fixture::base_rate *
      (to - from).count() / (364LL * reward_fixture::day_us));
   BOOST_REQUIRE_EQUAL(f.global()["total_unpaid_blocks"].as_uint64(),
      f.row("producersv2"_n, f.nodes[0].to_uint64_t())["unpaid_blocks"].as_uint64());
   f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]);
   const auto issued = f.supply() - old_supply;
   BOOST_REQUIRE_GT(issued, 0);
   BOOST_CHECK_LE(std::llabs(issued - expected), 1);
   BOOST_CHECK_EQUAL(f.balance(f.nodes[0]) - old_balance, issued);
   BOOST_CHECK_EQUAL(f.balance("eosio.bpay"_n), 0);
   BOOST_CHECK_EQUAL(f.balance("eosio.vpay"_n), 0);
   BOOST_CHECK_EQUAL(f.balance("eosio.saving"_n), 0);
   BOOST_CHECK_EQUAL(f.global()["total_unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, f.nodes[0].to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[1]), f.nodes[1]),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "no produced blocks"); });
   f.chain.produce_blocks(2);
   BOOST_CHECK_THROW(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]), fc::exception);
}

BOOST_AUTO_TEST_CASE(first_rex_purchase_activates_from_zero_weight_selection) {
   positive_vote_reward_fixture f;
   f.seal();
   f.vote();
   f.call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", "1.0000 SYS"));
   f.require_unactivated();
   const auto activation = f.chain.control->pending_block_time();
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "0.0001 SYS"));
   BOOST_CHECK_EQUAL(f.global()["total_activated_stake"].as_int64(), 1);
   BOOST_CHECK(reward_fixture::when(f.rewards()["start_time"]) == activation);
   f.settle_single_producer();
   f.vote();
   BOOST_CHECK_EQUAL(f.global()["total_activated_stake"].as_int64(), 1);
   BOOST_CHECK(reward_fixture::when(f.rewards()["start_time"]) == activation);
   BOOST_REQUIRE_GT(f.global()["total_unpaid_blocks"].as_uint64(), 0u);
}

BOOST_AUTO_TEST_CASE(withdrawal_restart_and_halving_preserve_the_first_positive_vote_clock) {
   positive_vote_reward_fixture f;
   f.seal();
   f.vote();
   f.one_unit();
   const auto activation = reward_fixture::when(f.rewards()["start_time"]);
   f.clear_vote();
   f.call("undelegatebw"_n, fc::mutable_variant_object()("from", "alice")("receiver", "alice")
      ("unstake_net_quantity", "0.0001 SYS")("unstake_cpu_quantity", "0.0000 SYS"));
   BOOST_CHECK_EQUAL(f.global()["total_activated_stake"].as_int64(), 0);
   f.chain.produce_block();
   f.chain.close();
   f.chain.open();
   f.chain.set_node_finalizers(std::vector<name>{"eosio"_n, f.nodes[0], f.nodes[1]});
   f.chain.produce_block();
   BOOST_CHECK(reward_fixture::when(f.rewards()["start_time"]) == activation);
   f.chain.produce_block(fc::days(1461));
   f.chain.produce_block();
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 1u);
   BOOST_CHECK(reward_fixture::when(f.rewards()["start_time"]) == activation);
   f.vote();
   f.one_unit();
   BOOST_CHECK(reward_fixture::when(f.global()["thresh_activated_stake_time"]) == activation);
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 1u);
}

BOOST_AUTO_TEST_CASE(failed_positive_vote_transaction_rolls_back_activation) {
   positive_vote_reward_fixture f;
   f.seal();
   f.vote();
   const auto before = resource_json(f.global());
   const auto policy = resource_json(f.rewards());
   auto stake = f.chain.get_action("eosio"_n, "delegatebw"_n, {{"alice"_n, "active"_n}}, fc::mutable_variant_object()
      ("from", "alice")("receiver", "alice")("stake_net_quantity", "0.0001 SYS")
      ("stake_cpu_quantity", "0.0000 SYS")("transfer", false));
   auto fail = f.chain.get_action("eosio.token"_n, "transfer"_n, {{"alice"_n, "active"_n}}, fc::mutable_variant_object()
      ("from", "alice")("to", "eosio")("quantity", "100000.0000 SYS")("memo", "expected overdraw"));
   BOOST_CHECK_THROW(f.push({stake, fail}, "alice"_n), fc::exception);
   BOOST_CHECK_EQUAL(resource_json(f.global()), before);
   BOOST_CHECK_EQUAL(resource_json(f.rewards()), policy);
   f.chain.produce_blocks(3);
   f.require_unactivated();
}

#ifdef PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR
BOOST_AUTO_TEST_CASE(reviewed_upgrade_activates_existing_small_votes_without_revote_or_backpay) {
   const auto* override = std::getenv("PUREEOS_TEST_PREACTIVATION_DIR");
   const auto* baseline = override ? override : PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR;
   for (bool already_voted : {false, true}) {
      positive_vote_reward_fixture f(baseline);
      if (already_voted) { f.vote(); f.one_unit(); }
      f.seal();
      f.chain.produce_blocks(3);
      f.require_unactivated();
      reviewed_positive_vote_upgrade(f);
      const auto execution = f.chain.control->pending_block_time();
      f.chain.produce_blocks(3);
      if (!already_voted) {
         f.require_unactivated();
         f.vote();
         f.one_unit();
      }
      const auto origin = reward_fixture::when(f.rewards()["start_time"]);
      BOOST_CHECK(origin >= execution); // never backdate to the old, unfunded vote
      BOOST_CHECK(origin <= f.chain.control->pending_block_time());
      BOOST_CHECK_EQUAL(f.global()["total_activated_stake"].as_int64(), 1);
      BOOST_CHECK_EQUAL(f.global()["perblock_bucket"].as_int64(), 0);
      BOOST_CHECK_LE(f.global()["total_unpaid_blocks"].as_uint64(), 4u);
      BOOST_CHECK_EQUAL(f.supply(), 100000000LL);
      f.settle_single_producer(); // also commits fresh vote transactions before the clock jump
      f.chain.produce_block(fc::days(2));
      f.chain.produce_blocks(2);
      const auto before = f.supply();
      f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]);
      BOOST_REQUIRE_GT(f.supply(), before);
      BOOST_CHECK_LT(f.supply() - before, 10000); // about 0.54 SYS, not 14 days of backpay
   }
}

BOOST_AUTO_TEST_CASE(reviewed_upgrade_preserves_already_active_block_only_rewards) {
   const auto* override = std::getenv("PUREEOS_TEST_PREACTIVATION_DIR");
   const auto* baseline = override ? override : PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR;
   reward_fixture f(true, baseline);
   f.checked_claim();
   f.block();
   const auto origin = f.origin();
   reviewed_positive_vote_upgrade(f);
   f.block();
   BOOST_CHECK(f.origin() == origin);
   BOOST_CHECK(reward_fixture::when(f.global()["thresh_activated_stake_time"]) == origin);
   BOOST_REQUIRE_GT(f.checked_claim().block_paid, 0);
   f.snapshot_check();
   f.observer_replay();
}
#endif

BOOST_AUTO_TEST_SUITE_END()
