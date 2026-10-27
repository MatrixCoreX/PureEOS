// Real system WASM/actions/token transfers. No privileged table edits or
// shortened production halving/review periods are used to advance test time.
#include <cmath>
namespace {
struct reward_fixture : sequential_rotation_fixture {
   static constexpr int64_t day_us = 86400LL * 1000000;
   static constexpr int64_t period_us = 1461LL * day_us;
   static constexpr double base_rate = 0.04879016416943201 / 5.0;
   explicit reward_fixture(bool headroom = false, std::filesystem::path root = rotation_contracts_for_test(),
                           bool empty_treasury = false, uint8_t legacy_vote_revision = 0)
      : sequential_rotation_fixture(2, true, std::move(root), headroom, empty_treasury, legacy_vote_revision) {
      completed(2);
   }
   fc::variant rewards() const { return row("rewardpolicy"_n, "rewardpolicy"_n.to_uint64_t()); }
   fc::variant global() const { return row("globalv2"_n, "globalv2"_n.to_uint64_t()); }
   static int64_t units(const fc::variant& v) { return asset::from_string(v.as_string()).get_amount(); }
   int64_t balance(name owner) const {
      const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
      if (chain.get_row_by_account("eosio.token"_n, owner, "accounts"_n, name{code}).empty()) return 0;
      return units(row("accounts"_n, code, owner, "eosio.token"_n)["balance"]);
   }
   int64_t supply() const {
      const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
      return units(row("stat"_n, code, name{code}, "eosio.token"_n)["supply"]);
   }
   static fc::time_point when(const fc::variant& v) { return fc::time_point::from_iso_string(v.as_string()); }
   fc::time_point origin() const { return when(rewards()["start_time"]); }
   void at(fc::time_point target) {
      // Commit before a time jump; never replay short-lived pending user actions
      // into a later era. The first block evaluates onblock for its parent.
      block();
      const auto delta = target - chain.head().block_time();
      BOOST_REQUIRE_GT(delta.count(), 0);
      BOOST_REQUIRE_EQUAL(delta.count() % config::block_interval_us, 0);
      block(uint32_t(delta.count() / config::block_interval_us));
      block();
   }
   struct claim_result { int64_t funding = 0, block_funding = 0, vote_funding = 0, issued = 0, block_paid = 0, vote_paid = 0; };
   claim_result claim(name owner = {}) {
      if (!owner) owner = nodes[0];
      const auto saved = balance("eosio.saving"_n);
      const auto vote_balance = balance("eosio.vpay"_n);
      const auto vote_bucket = global()["pervote_bucket"].as_int64();
      const auto block_bucket = global()["perblock_bucket"].as_int64();
      const auto total = global()["total_unpaid_blocks"].as_uint64();
      const auto blocks = row("producersv2"_n, owner.to_uint64_t())["unpaid_blocks"].as_uint64();
      const auto trace = call("claimrewards"_n, fc::mutable_variant_object()("owner", owner), owner);
      abi_def abi;
      BOOST_REQUIRE(abi_serializer::to_abi(chain.control->db().get<account_object, by_name>("eosio.token"_n).abi, abi));
      auto yield = abi_serializer::create_yield_function(fc::seconds(5));
      abi_serializer serializer(abi, yield);
      claim_result result;
      for (const auto& action : trace->action_traces) {
         if (action.receiver != "eosio.token"_n || action.act.account != "eosio.token"_n) continue;
         if (action.act.name != "issue"_n && action.act.name != "transfer"_n) continue;
         const auto data = serializer.binary_to_variant(action.act.name.to_string(), action.act.data, yield);
         const int64_t amount = units(data["quantity"]);
         if (action.act.name == "issue"_n) { result.issued += amount; continue; }
         const auto from = data["from"].as_string(), to = data["to"].as_string();
         BOOST_CHECK_NE(to, "eosio.saving");
         BOOST_CHECK_NE(to, "eosio.vpay");
         BOOST_CHECK_NE(from, "eosio.vpay");
         if (from == "eosio" && to == "eosio.bpay") result.block_funding += amount;
         if (from == "eosio" && to == "eosio.vpay") result.vote_funding += amount;
         if (to == owner.to_string() && from == "eosio.bpay") result.block_paid += amount;
         if (to == owner.to_string() && from == "eosio.vpay") result.vote_paid += amount;
      }
      result.funding = result.block_funding + result.vote_funding;
      BOOST_CHECK_EQUAL(result.block_funding, result.funding);
      BOOST_CHECK_EQUAL(result.vote_funding, 0);
      BOOST_CHECK_EQUAL(result.vote_paid, 0);
      BOOST_REQUIRE_GT(total, 0u);
      const auto expected_pay = int64_t(static_cast<unsigned __int128>(block_bucket + result.funding) * blocks / total);
      BOOST_CHECK_EQUAL(result.block_paid, expected_pay);
      BOOST_CHECK_EQUAL(global()["perblock_bucket"].as_int64(), block_bucket + result.funding - expected_pay);
      BOOST_CHECK_EQUAL(balance("eosio.saving"_n), saved);
      BOOST_CHECK_EQUAL(balance("eosio.vpay"_n), vote_balance);
      BOOST_CHECK_EQUAL(global()["pervote_bucket"].as_int64(), vote_bucket);
      return result;
   }
   // Independent piecewise oracle, with long-double intermediate values.
   int64_t expected(fc::time_point from, fc::time_point to, int64_t principal) const {
      const auto start = origin();
      long double weighted = 0;
      for (int era = 0; era < 20; ++era) {
         const auto left = std::max(from, start + fc::microseconds(era * period_us));
         const auto right = std::min(to, start + fc::microseconds((era + 1) * period_us));
         if (right > left) weighted += std::ldexp(static_cast<long double>((right - left).count()), -era);
      }
      return int64_t(static_cast<long double>(principal) * base_rate * weighted / (364LL * day_us));
   }
   claim_result checked_claim(name owner = {}) {
      const auto from = when(global()["last_pervote_bucket_fill"]);
      const auto to = chain.control->pending_block_time();
      const auto predicted = expected(from, to, supply());
      const auto result = claim(owner);
      // Inflation retains legacy IEEE-754 accrual; ranking and reward division
      // remain exact integers. Permit only one smallest unit at a floor boundary.
      BOOST_CHECK_LE(std::llabs(result.funding - predicted), 1);
      return result;
   }
};
}

BOOST_AUTO_TEST_SUITE(pureeos_reward_halving_tests)

BOOST_AUTO_TEST_CASE(permanently_banned_producer_can_claim_old_unpaid_blocks_but_not_new_rewards) {
   sequential_rotation_fixture f(21, true);
   f.completed(21);
   const auto victim = f.nodes[0];
   const auto unpaid = [&] { return f.row("producersv2"_n, victim.to_uint64_t())["unpaid_blocks"].as_uint64(); };
   const auto global = [&] { return f.row("globalv2"_n, "globalv2"_n.to_uint64_t()); };
   const auto balance = [&](name owner) {
      return reward_fixture::units(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value,
          owner, "eosio.token"_n)["balance"]);
   };
   const auto old_blocks = unpaid();
   BOOST_REQUIRE_GT(old_blocks, 0u);
   const auto old_balance = balance(victim);
   f.online.erase(std::find(f.online.begin(), f.online.end(), victim));
   f.restart(f.online);
   f.until([&] { return f.candidate(victim)["banned"].as_bool(); }, 1000);
   BOOST_REQUIRE_EQUAL(f.candidate(victim)["zero_windows"].as_uint64(), 3u);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 0 && f.list("active").size() == 20; });
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 14u);
   f.online.push_back(victim);
   f.restart(f.online);
   const auto made = f.produced[victim];
   f.production_slots(2 * 20 * config::producer_repetitions);
   BOOST_REQUIRE_EQUAL(f.produced[victim], made);
   BOOST_REQUIRE_EQUAL(unpaid(), old_blocks);
   BOOST_REQUIRE_EQUAL(balance(victim), old_balance);
   const auto total = global()["total_unpaid_blocks"].as_uint64();
   const auto bucket = global()["perblock_bucket"].as_int64();
   const auto trace = f.call("claimrewards"_n, fc::mutable_variant_object()("owner", victim), victim);
   abi_def abi;
   BOOST_REQUIRE(abi_serializer::to_abi(f.chain.control->db().get<account_object, by_name>("eosio.token"_n).abi, abi));
   auto yield = abi_serializer::create_yield_function(fc::seconds(5));
   abi_serializer serializer(abi, yield);
   int64_t funded = 0, paid = 0;
   for (const auto& action : trace->action_traces) {
      if (action.receiver != "eosio.token"_n || action.act.account != "eosio.token"_n || action.act.name != "transfer"_n) continue;
      const auto data = serializer.binary_to_variant("transfer", action.act.data, yield);
      const auto from = data["from"].as_string(), to = data["to"].as_string();
      const auto amount = reward_fixture::units(data["quantity"]);
      BOOST_CHECK_NE(to, "eosio.saving"); BOOST_CHECK_NE(to, "eosio.vpay");
      if (from == "eosio" && to == "eosio.bpay") funded += amount;
      if (from == "eosio.bpay" && to == victim.to_string()) paid += amount;
   }
   BOOST_REQUIRE_GT(paid, 0);
   BOOST_CHECK_EQUAL(paid, int64_t(static_cast<unsigned __int128>(bucket + funded) * old_blocks / total));
   BOOST_CHECK_EQUAL(balance(victim), old_balance + paid);
   BOOST_CHECK_EQUAL(unpaid(), 0u);
   BOOST_CHECK_EQUAL(global()["total_unpaid_blocks"].as_uint64(), total - old_blocks);
   BOOST_CHECK_EQUAL(global()["perblock_bucket"].as_int64(), bucket + funded - paid);
   BOOST_CHECK(f.candidate(victim)["banned"].as_bool());
   const auto paid_balance = balance(victim);
   BOOST_CHECK_THROW(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", victim), victim), fc::exception);
   BOOST_CHECK_EQUAL(balance(victim), paid_balance);
   f.block();
   f.snapshot_check();
   f.observer_replay();
   BOOST_TEST_MESSAGE("Actual WASM claim after permanent ban paid only preserved historical blocks; no asset freeze or new reward");
}

BOOST_AUTO_TEST_CASE(activation_and_real_issuance_fund_only_actual_blocks) {
   reward_fixture f(true);
   BOOST_CHECK_EQUAL(f.rewards()["version"].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(f.rewards()["halving_seconds"].as_uint64(), 1461u * 86400u);
   BOOST_CHECK(f.origin() == reward_fixture::when(f.global()["thresh_activated_stake_time"]));
   const auto principal = f.supply(), balance = f.balance(f.nodes[0]);
   const auto result = f.checked_claim();
   BOOST_REQUIRE_GT(result.funding, 0);
   BOOST_CHECK_EQUAL(result.issued, result.funding);
   BOOST_CHECK_EQUAL(f.supply(), principal + result.issued);
   BOOST_CHECK_EQUAL(f.balance(f.nodes[0]), balance + result.block_paid + result.vote_paid);
   BOOST_CHECK_GT(result.block_paid, 0);
   BOOST_CHECK_EQUAL(result.vote_paid, 0);
   BOOST_CHECK_EQUAL(f.row("global4"_n, "global4"_n.to_uint64_t())["inflation_pay_factor"].as_int64(), 10000);
   BOOST_CHECK_EQUAL(f.row("global4"_n, "global4"_n.to_uint64_t())["votepay_factor"].as_int64(), 10000);
   f.block();
   BOOST_CHECK_THROW(f.claim(), fc::exception); // unchanged >24-hour claim interval
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(large_vote_changes_do_not_change_block_entitlements_or_create_vote_shares) {
   reward_fixture f;
   const auto counts = [&](name n) { return f.row("producersv2"_n, n.to_uint64_t())["unpaid_blocks"].as_uint64(); };
   const auto first_blocks = counts(f.nodes[0]), second_blocks = counts(f.nodes[1]);
   BOOST_REQUIRE_GT(first_blocks, 0u);
   BOOST_REQUIRE_GT(second_blocks, 0u);
   f.choose("alice"_n, f.nodes[1]);
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "160000000.0000 SYS"));
   BOOST_CHECK_EQUAL(counts(f.nodes[0]), first_blocks);
   BOOST_CHECK_EQUAL(counts(f.nodes[1]), second_blocks);
   BOOST_REQUIRE_GT(f.row("producersv2"_n, f.nodes[1].to_uint64_t())["total_votes"].as_int64(),
                    10000 * f.row("producersv2"_n, f.nodes[0].to_uint64_t())["total_votes"].as_int64());
   f.call("voteproducer"_n, fc::mutable_variant_object()("voter", f.nodes[0])("proxy", "")
      ("producers", std::vector<name>{}), f.nodes[0]);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, f.nodes[0].to_uint64_t())["total_votes"].as_int64(), 0);
   // Claims in the same pending block: the first claim fills the pool, and the
   // second receives its remaining exact block share, independent of votes.
   const auto first = f.checked_claim(f.nodes[0]);
   const auto second = f.checked_claim(f.nodes[1]);
   BOOST_REQUIRE_GT(first.block_paid, 0);
   BOOST_REQUIRE_GT(second.block_paid, 0);
   BOOST_CHECK_EQUAL(first.block_paid + second.block_paid, first.funding);
   for (auto n : f.nodes)
      BOOST_CHECK(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "producers2"_n, n).empty());
   BOOST_CHECK_EQUAL(f.row("global2"_n, "global2"_n.to_uint64_t())["total_producer_votepay_share"].as_double(), 0);
   f.block();
   f.snapshot_check();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(exact_four_year_boundary_and_later_eras_do_not_reprice_prior_accrual) {
   reward_fixture f;
   const auto origin = f.origin();
   f.at(origin + fc::microseconds(reward_fixture::period_us - reward_fixture::day_us));
   const auto before = f.checked_claim();
   BOOST_CHECK_EQUAL(before.issued, 0); // capped token falls back to treasury, never exceeds max_supply
   f.at(origin + fc::microseconds(reward_fixture::period_us + reward_fixture::day_us));
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 1u);
   f.checked_claim(); // includes one day at full rate and one day at half rate
   f.at(origin + fc::microseconds(2 * reward_fixture::period_us + reward_fixture::day_us));
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 2u);
   f.checked_claim();
   f.at(origin + fc::microseconds(4 * reward_fixture::period_us + reward_fixture::day_us));
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 4u);
   f.checked_claim(); // spans two complete halving eras
   BOOST_CHECK_EQUAL(f.supply(), 10000000000000LL);
   BOOST_CHECK_EQUAL(f.balance("eosio.saving"_n), 0);
   f.snapshot_check();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(rate_changes_at_the_exact_boundary_not_one_block_early) {
   reward_fixture f;
   const auto boundary = f.origin() + fc::microseconds(reward_fixture::period_us);
   f.at(boundary - fc::microseconds(1500000));
   BOOST_REQUIRE(f.chain.control->pending_block_time() == boundary - fc::microseconds(500000));
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.row("global4"_n, "global4"_n.to_uint64_t())["continuous_rate"].as_double(), reward_fixture::base_rate);
   f.block();
   BOOST_REQUIRE(f.chain.control->pending_block_time() == boundary);
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(f.row("global4"_n, "global4"_n.to_uint64_t())["continuous_rate"].as_double(), reward_fixture::base_rate / 2);
   f.block();
   BOOST_REQUIRE(f.chain.control->pending_block_time() == boundary + fc::microseconds(500000));
   BOOST_CHECK_EQUAL(f.rewards()["current_era"].as_uint64(), 1u);
   f.checked_claim();
}

BOOST_AUTO_TEST_CASE(fixed_principal_equal_intervals_halve_and_clock_survives_restart) {
   reward_fixture f;
   const auto origin = f.origin();
   std::vector<int64_t> amounts;
   for (int era = 0; era < 3; ++era) {
      f.at(origin + fc::microseconds(era * reward_fixture::period_us + 10 * reward_fixture::day_us));
      f.checked_claim();
      f.block();
      const auto start = reward_fixture::when(f.global()["last_pervote_bucket_fill"]);
      f.at(start + fc::days(2));
      amounts.push_back(f.checked_claim().funding);
      f.block();
      const auto state = resource_json(f.rewards());
      f.restart(f.online);
      BOOST_CHECK_EQUAL(resource_json(f.rewards()), state);
      BOOST_CHECK(f.origin() == origin);
   }
   // at() adds the same two half-second commits to each equal interval.
   BOOST_CHECK_LE(std::llabs(amounts[0] - 2 * amounts[1]), 2);
   BOOST_CHECK_LE(std::llabs(amounts[1] - 2 * amounts[2]), 2);
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(legacy_management_actions_cannot_override_the_fixed_policy) {
   resource_split_fixture f(true, 2, rotation_contracts_for_test(), true);
   const auto rejected = [&](name action, const fc::variant_object& data, name signer = "eosio"_n) {
      BOOST_CHECK_EXCEPTION(f.call(action, data, signer), fc::exception, [](const auto& e) {
         return resource_split_fixture::message(e, "reward halving policy is fixed");
      });
   };
   rejected("setinflation"_n, fc::mutable_variant_object()("annual_rate", 500)("inflation_pay_factor", 50000)("votepay_factor", 40000));
   rejected("setpayfactor"_n, fc::mutable_variant_object()("inflation_pay_factor", 50000)("votepay_factor", 40000));
   rejected("setschedule"_n, fc::mutable_variant_object()("start_time", "2030-01-01T00:00:00")("continuous_rate", 1.0));
   rejected("delschedule"_n, fc::mutable_variant_object()("start_time", "2030-01-01T00:00:00"));
   rejected("execschedule"_n, fc::mutable_variant_object(), "alice"_n);
   BOOST_CHECK_EXCEPTION(f.call("updtrevision"_n, fc::mutable_variant_object()("revision", 1), "eosio"_n),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "vote-pay revisions are disabled"); });
   BOOST_CHECK_EQUAL(f.row("rewardpolicy"_n, "rewardpolicy"_n.to_uint64_t())["start_time"].as_string(), "1970-01-01T00:00:00.000");
   f.chain.produce_block();
   f.chain.produce_block(fc::days(1461));
   f.chain.produce_block();
   BOOST_CHECK_EQUAL(f.row("rewardpolicy"_n, "rewardpolicy"_n.to_uint64_t())["start_time"].as_string(), "1970-01-01T00:00:00.000");
   BOOST_CHECK_EQUAL(f.row("rewardpolicy"_n, "rewardpolicy"_n.to_uint64_t())["current_era"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.row("globalv2"_n, "globalv2"_n.to_uint64_t())["perblock_bucket"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.row("globalv2"_n, "globalv2"_n.to_uint64_t())["pervote_bucket"].as_int64(), 0);
}

BOOST_AUTO_TEST_CASE(insufficient_treasury_rolls_back_claim_without_losing_reward_accrual) {
   reward_fixture f(false, rotation_contracts_for_test(), true);
   BOOST_REQUIRE_EQUAL(f.balance("eosio"_n), 0);
   const auto policy = resource_json(f.rewards()), before = resource_json(f.global());
   const auto producer = resource_json(f.row("producersv2"_n, f.nodes[0].to_uint64_t()));
   BOOST_CHECK_EXCEPTION(f.claim(), fc::exception, [](const auto& e) {
      return resource_split_fixture::message(e, "insufficient system token balance");
   });
   BOOST_CHECK_EQUAL(resource_json(f.rewards()), policy);
   BOOST_CHECK_EQUAL(resource_json(f.global()), before);
   BOOST_CHECK_EQUAL(resource_json(f.row("producersv2"_n, f.nodes[0].to_uint64_t())), producer);
   f.chain.push_action("eosio.token"_n, "transfer"_n, "alice"_n, fc::mutable_variant_object()
      ("from", "alice")("to", "eosio")("quantity", "1000000.0000 SYS")("memo", "restore isolated reward treasury"));
   const auto result = f.checked_claim();
   BOOST_CHECK_GT(result.funding, 0);
   BOOST_CHECK_EQUAL(result.issued, 0);
   BOOST_CHECK_EQUAL(f.balance("eosio"_n), 10000000000LL - result.funding);
   f.block();
   BOOST_CHECK_THROW(f.claim(), fc::exception);
}

#ifdef PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR
void check_reviewed_block_pay_upgrade(uint8_t revision) {
   reward_fixture f(false, PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR, false, revision);
   // Fund old pools using a real legacy claim, which also creates savings.
   f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]);
   f.block();
   const auto saved = f.balance("eosio.saving"_n);
   BOOST_REQUIRE_GT(saved, 0);
   const auto activation = reward_fixture::when(f.global()["thresh_activated_stake_time"]);
   const auto base = std::filesystem::path(PUREEOS_ROTATION_CONTRACTS_DIR) / "eosio.system/eosio.system";
   const auto code = read_wasm((base.string() + ".wasm").c_str());
   f.upgrade_code.assign(code.begin(), code.end());
   abi_def abi;
   fc::from_variant(fc::json::from_file(base.string() + ".abi"), abi);
   f.upgrade_abi = fc::raw::pack(abi);
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal("eosio"_n))}, "developer"_n);
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   f.wait_review();
   const auto global = f.global();
   const auto old_vote_balance = f.balance("eosio.vpay"_n);
   BOOST_REQUIRE_GT(old_vote_balance, 0);
   const auto old_shares = f.chain.get_row_by_account("eosio"_n, "eosio"_n, "producers2"_n, f.nodes[0]);
   f.execute();
   f.block();
   BOOST_CHECK_EQUAL(f.global()["perblock_bucket"].as_int64(), global["perblock_bucket"].as_int64());
   BOOST_CHECK_EQUAL(f.global()["pervote_bucket"].as_int64(), global["pervote_bucket"].as_int64());
   BOOST_CHECK(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "producers2"_n, f.nodes[0]) == old_shares);
   BOOST_CHECK_EQUAL(f.balance("eosio.saving"_n), saved);
   BOOST_CHECK(f.origin() == activation);
   f.checked_claim();
   BOOST_CHECK_EQUAL(f.balance("eosio.saving"_n), saved);
   BOOST_CHECK_EQUAL(f.balance("eosio.vpay"_n), old_vote_balance);
   BOOST_CHECK_EQUAL(f.global()["pervote_bucket"].as_int64(), global["pervote_bucket"].as_int64());
   f.block();
   f.snapshot_check();
   f.observer_replay();
}
BOOST_AUTO_TEST_CASE(reviewed_upgrade_preserves_old_funded_pools_savings_and_activation_clock) {
   check_reviewed_block_pay_upgrade(0);
}
BOOST_AUTO_TEST_CASE(legacy_revision_one_cannot_restore_vote_pay_after_reviewed_upgrade) {
   check_reviewed_block_pay_upgrade(1);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
