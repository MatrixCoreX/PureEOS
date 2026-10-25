// Real WASM election + native policy handoffs/QCs. No setprods/setfinalizer
// overrides. A socket-free tester simulates signers, not independent P2P nodes.
#include <cstdlib>
namespace {
std::filesystem::path rotation_contracts_for_test() {
   const auto* override = std::getenv("PUREEOS_TEST_ROTATION_DIR");
   return override ? override : PUREEOS_ROTATION_CONTRACTS_DIR;
}
struct sequential_rotation_fixture : resource_split_fixture {
   std::vector<name> online;
   std::vector<name> previous_proposers, previous_finalizers;
   std::map<name, uint32_t> produced;

   explicit sequential_rotation_fixture(size_t count = 23, bool activate_rewards = false,
                                         std::filesystem::path root = rotation_contracts_for_test(),
                                         bool reward_headroom = false, bool empty_treasury = false,
                                         uint8_t legacy_vote_revision = 0)
      : resource_split_fixture(true, std::max(size_t(2), count), std::move(root), true, reward_headroom) {
      const size_t initial_vote = std::max(size_t(2300), count * 100);
      const auto funding = std::to_string(std::max(size_t(5000), initial_vote + 1000)) + ".0000 SYS";
      for (size_t i = 0; i < count; ++i) {
         const auto node = nodes[i];
         call("buyram"_n, fc::mutable_variant_object()("payer", "eosio")("receiver", node)
            ("quant", "100.0000 SYS"), "eosio"_n);
         chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
            ("from", "eosio")("to", node)("quantity", funding)("memo", "rotation test"));
         if (i) register_node(node);
         const auto half = std::to_string((initial_vote - 100 * i) / 2) + ".0000 SYS";
         stake(half, half, node, node);
         choose(node, node);
         chain.produce_block();
      }
      if (activate_rewards) {
         // Keep the historical funding path for old-contract upgrade fixtures
         // and advance beyond the claim cooldown. The current contract already
         // activates on the first small vote; no large balance is required.
         // Mature and redeem the shares before freezing the test ranks; no
         // native/table mutation bypasses the undelegation prerequisite.
         chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
            ("from", "eosio")("to", "alice")("quantity", "160000000.0000 SYS")("memo", "activation test"));
         choose("alice"_n, nodes[0]);
         call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", "160000000.0000 SYS"));
         call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "160000000.0000 SYS"));
         chain.produce_block();
         chain.produce_block(fc::days(5));
         call("sellrex"_n, fc::mutable_variant_object()("from", "alice")
            ("rex", row("rexbal"_n, "alice"_n.to_uint64_t())["rex_balance"]));
         call("voteproducer"_n, fc::mutable_variant_object()("voter", "alice")("proxy", "")
            ("producers", std::vector<name>{}));
      }
      online.assign(nodes.begin(), nodes.begin() + count);
      online.push_back("eosio"_n);
      if (empty_treasury) {
         const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
         const auto balance = row("accounts"_n, code, "eosio"_n, "eosio.token"_n)["balance"];
         chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
            ("from", "eosio")("to", "alice")("quantity", balance)("memo", "isolated empty treasury test"));
      }
      if (legacy_vote_revision)
         call("updtrevision"_n, fc::mutable_variant_object()("revision", legacy_vote_revision), "eosio"_n);
      seal();
      chain.produce_block();
      restart(online);
      const auto code_hash = chain.control->db().get<account_metadata_object, by_name>("eosio"_n).code_hash;
      BOOST_TEST_MESSAGE("Rotation contract code: " << code_hash.str());
      BOOST_REQUIRE_EQUAL(row("globalv2"_n, "globalv2"_n.to_uint64_t())["total_activated_stake"].as_int64(),
                          int64_t(count * initial_vote - 100 * count * (count - 1) / 2) * 10000);
   }

   fc::variant rotation() const { return row("rotation"_n, "rotation"_n.to_uint64_t()); }
   fc::variant candidate(name n) const { return row("rotcands"_n, n.to_uint64_t()); }
   std::vector<name> list(const char* field) const {
      std::vector<name> result;
      const auto state = rotation(); // keep the variant owning get_array() alive
      if (std::string(field) == "queue") {
         std::vector<std::pair<uint64_t, name>> entries;
         for (auto n : nodes) {
            if (chain.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, n).empty()) continue;
            const auto c = candidate(n);
            if (c["ticket"].as_uint64() && c["seen_epoch"].as_uint64() == state["published_epoch"].as_uint64() && !c["banned"].as_bool())
               entries.emplace_back(c["ticket"].as_uint64(), n);
         }
         std::sort(entries.begin(), entries.end());
         for (const auto& e : entries) result.push_back(e.second);
         return result;
      }
      for (const auto& n : state[field].get_array()) result.emplace_back(n.as_string());
      return result;
   }
   void choose(name voter, name target) {
      call("voteproducer"_n, fc::mutable_variant_object()("voter", voter)("proxy", "")
         ("producers", std::vector<name>{target}), voter);
   }
   void restart(const std::vector<name>& signers) {
      const auto id = chain.head().id();
      chain.close();
      chain.open();
      chain.set_node_finalizers(signers);
      chain.do_check_for_votes(false);
      BOOST_REQUIRE(chain.head().id() == id);
   }
   static void one_seat(const std::vector<name>& before, const std::vector<name>& after) {
      if (before.empty()) return;
      size_t added = 0, removed = 0;
      for (auto n : after) added += std::find(before.begin(), before.end(), n) == before.end();
      for (auto n : before) removed += std::find(after.begin(), after.end(), n) == after.end();
      BOOST_REQUIRE_LE(added, 1u);
      BOOST_REQUIRE_LE(removed, 1u);
      BOOST_REQUIRE_LE(after.size(), 21u);
      BOOST_REQUIRE(!after.empty());
   }
   signed_block_ptr block(uint32_t step = 1) {
      uint32_t slot = chain.head().header().timestamp.slot + step;
      name scheduled;
      uint32_t skipped_windows = 0;
      for (;;) {
         block_timestamp_type when{slot};
         scheduled = chain.control->head_active_producers(when).get_scheduled_producer(when).producer_name;
         if (std::find(online.begin(), online.end(), scheduled) != online.end()) break;
         BOOST_REQUIRE_LT(++skipped_windows, 43u);
         slot += config::producer_repetitions; // never auto-sign an offline member
      }
      const auto delta = fc::microseconds(int64_t(slot - chain.head().header().timestamp.slot) * config::block_interval_us);
      auto result = chain.produce_block_ex(delta);
      BOOST_REQUIRE(result.block);
      if (result.onblock_trace && result.onblock_trace->except) BOOST_FAIL(result.onblock_trace->except->to_detail_string());
      BOOST_REQUIRE(result.block->producer == scheduled);
      ++produced[scheduled];
      std::vector<name> p, f;
      for (const auto& x : chain.control->head_active_producers().producers) p.push_back(x.producer_name);
      for (const auto& x : chain.control->head_active_finalizer_policy()->finalizers) f.emplace_back(x.description);
      one_seat(previous_proposers, p);
      one_seat(previous_finalizers, f);
      previous_proposers = p;
      previous_finalizers = f;
      return result.block;
   }
   // Visit every real slot, never sign for an unavailable proposer. Unlike
   // step(), this measures exact missed-slot counts during fault experiments.
   std::pair<uint32_t, uint32_t> production_slots(uint32_t slots) {
      const auto start = chain.head().header().timestamp.slot;
      uint32_t made = 0, missed = 0;
      for (uint32_t slot = start + 1; slot <= start + slots; ++slot) {
         const block_timestamp_type when{slot};
         const auto scheduled = chain.control->head_active_producers(when).get_scheduled_producer(when).producer_name;
         if (std::find(online.begin(), online.end(), scheduled) == online.end()) {
            ++missed;
            continue;
         }
         const auto b = block(slot - chain.head().header().timestamp.slot);
         BOOST_REQUIRE_EQUAL(b->timestamp.slot, slot);
         ++made;
      }
      return {made, missed};
   }
   void step() {
      // Test clock only: jump across idle/observation gaps to keep long FIFO
      // tests short, but produce the entrant's actual block and real QCs.
      // verify_full_production_round separately covers every contiguous slot.
      const auto s = rotation();
      const auto stage = s["stage"].as_uint64();
      const auto slot = chain.head().header().timestamp.slot;
      const auto next = s["next_attempt_slot"].as_uint64();
      // Once full, every healthy member must really produce in every complete
      // personal window. Skipping whole rounds would now (correctly) earn bans.
      if (chain.control->head_active_producers().producers.size() == 21) block(12 - slot % 12);
      else if (s["ranking"].as_bool() || s["selecting"].as_bool()) block();
      else if (stage == 1 || stage == 3) block(252);
      else if (stage == 0 && next > slot) block(uint32_t(next - slot));
      else if (stage == 2) {
         const uint64_t start = s["observation_start"].as_uint64();
         const uint64_t end = s["observation_end"].as_uint64();
         const auto entrant = name{s["incoming"].as_string()};
         const auto& schedule = chain.control->head_active_producers().producers;
         auto it = std::find_if(schedule.begin(), schedule.end(), [&](const auto& p) { return p.producer_name == entrant; });
         BOOST_REQUIRE(it != schedule.end());
         const uint64_t entrant_start = start + (it - schedule.begin()) * config::producer_repetitions;
         if (slot < entrant_start) block(uint32_t(entrant_start - slot));
         else if (slot <= end) block(uint32_t(end + 1 - slot));
         else block();
      }
      else block(12);
   }
   template<typename Predicate> void until(Predicate predicate, uint32_t limit = 5000) {
      for (uint32_t i = 0; i < limit && !predicate(); ++i) step();
      BOOST_REQUIRE_MESSAGE(predicate(), "rotation did not reach expected state: " << resource_json(rotation()));
   }
   void completed(uint64_t value) {
      until([&] { return rotation()["completed"].as_uint64() >= value; });
      block(); // persist the observed completion, not just speculative onblock
      BOOST_REQUIRE_EQUAL(rotation()["completed"].as_uint64(), value);
      BOOST_REQUIRE_EQUAL(rotation()["stage"].as_uint64(), 0u);
   }
   void observer_replay() {
      chain.control->abort_block(); // compare committed state, not speculative onblock
      const auto committed = chain.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n);
      fc::temp_directory observer_dir;
      tester observer{base_tester::default_config(observer_dir).first, genesis};
      observer.do_check_for_votes(false);
      for (uint32_t n = 2; n <= chain.head().block_num(); ++n) {
         auto b = chain.control->fetch_block_by_number(n);
         BOOST_REQUIRE(b);
         observer.push_block(b);
      }
      BOOST_CHECK(observer.head().id() == chain.head().id());
      BOOST_CHECK(observer.last_irreversible_block_id() == chain.last_irreversible_block_id());
      BOOST_CHECK(observer.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n) == committed);
      for (auto n : nodes)
         BOOST_CHECK(observer.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, n) ==
                     chain.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, n));
      BOOST_TEST_MESSAGE("Independent observer replayed " << chain.head().block_num() << " blocks with identical rotation state");
   }
   void snapshot_check() {
      chain.control->abort_block();
      auto writer = buffered_snapshot_suite::get_writer();
      chain.control->write_snapshot(writer);
      auto snapshot = buffered_snapshot_suite::finalize(writer);
      fc::temp_directory restored_dir;
      const auto cfg = base_tester::default_config(restored_dir).first;
      tester restored(cfg, genesis, call_startup_t::no);
      restored.close();
      restored.init(cfg, buffered_snapshot_suite::get_reader(snapshot));
      restored.do_check_for_votes(false);
      for (unsigned i = 0; i < 6; ++i) restored.push_block(block());
      chain.control->abort_block();
      BOOST_CHECK(restored.head().id() == chain.head().id());
      BOOST_CHECK(restored.control->get_rotation_context_v1() == chain.control->get_rotation_context_v1());
      BOOST_CHECK(restored.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n) ==
                  chain.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n));
      for (auto n : nodes)
         BOOST_CHECK(restored.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, n) ==
                     chain.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, n));
   }
   void verify_full_production_round() {
      const auto schedule = chain.control->head_active_producers();
      const uint32_t span = schedule.producers.size() * config::producer_repetitions;
      const auto start = (chain.head().header().timestamp.slot / span + 1) * span;
      while (chain.head().header().timestamp.slot + 1 < start) block();
      std::map<name, uint32_t> counts;
      uint32_t last = 0;
      for (uint32_t slot = start; slot < start + span; ++slot) {
         const auto b = block();
         BOOST_REQUIRE_EQUAL(b->timestamp.slot, slot);
         BOOST_CHECK(b->producer == schedule.get_scheduled_producer(block_timestamp_type{slot}).producer_name);
         ++counts[b->producer];
         last = b->block_num();
      }
      for (const auto& p : schedule.producers) BOOST_CHECK_EQUAL(counts[p.producer_name], 12u);
      for (unsigned i = 0; i < 4; ++i) block();
      BOOST_CHECK_GE(chain.last_irreversible_block_num(), last);
   }
};
}

BOOST_AUTO_TEST_SUITE(pureeos_sequential_rotation_tests)

BOOST_AUTO_TEST_CASE(grows_one_at_a_time_then_fifo_across_vote_changes_and_restart) {
   sequential_rotation_fixture f;
   for (uint64_t k = 1; k <= 21; ++k) {
      f.completed(k); // first handoff replaces eosio, then 1 -> 2 -> ... -> 21
      auto expected = std::vector<name>(f.nodes.begin(), f.nodes.begin() + k);
      BOOST_REQUIRE_MESSAGE(f.list("active") == expected, "growth " << k << ": " << resource_json(f.rotation()));
      BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, k * 2 / 3 + 1);
      for (auto n : expected) BOOST_REQUIRE_GT(f.produced[n], 0u);
   }
   BOOST_TEST_MESSAGE("Native genesis -> 1 -> 2 -> ... -> 21: real single-seat handoffs with small positive votes");
   // Rank 23 becomes rank 1. It is already waiting, so it MUST NOT jump rank 22.
   f.stake("1250.0000 SYS", "1250.0000 SYS", f.nodes[22], f.nodes[22]);
   f.block(); // commit actions before advancing test time
   auto expected = f.list("active");
   for (uint64_t turn = 0; turn < 25; ++turn) {
      const auto incoming = f.nodes[(21 + turn) % 23];
      const auto outgoing = expected.front();
      expected.erase(expected.begin());
      expected.push_back(incoming);
      f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
      BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), incoming.to_string());
      BOOST_CHECK_EQUAL(f.rotation()["outgoing"].as_string(), outgoing.to_string());
      const auto frozen = f.list("target");
      if (turn == 0 || turn == 12) {
         f.block();
         f.chain.control->abort_block();
         const auto before = f.rotation();
         f.restart(f.online);
         BOOST_CHECK_EQUAL(resource_json(before), resource_json(f.rotation()));
      }
      f.completed(22 + turn);
      BOOST_REQUIRE(frozen == expected);
      BOOST_REQUIRE(f.list("active") == expected);
   }
   f.check_split();
   // Freeze the physical sample inside an actual in-flight admission's
   // observation round; ordinary FIFO is allowed to change later rounds.
   f.until([&] { return f.rotation()["stage"].as_uint64() == 2; });
   f.verify_full_production_round();
   f.observer_replay();
   BOOST_TEST_MESSAGE("23 candidates: 25 FIFO replacements; rank refresh/restart did not reset queue; 10/90 resources preserved");
}

BOOST_AUTO_TEST_CASE(offline_entrant_three_finalized_windows_ban_and_single_seat_replacement) {
   sequential_rotation_fixture f(22);
   f.completed(21);
   const auto entrant = f.nodes[21];
   f.online.erase(std::find(f.online.begin(), f.online.end(), entrant));
   f.restart(f.online);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 2; });
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), entrant.to_string());
   const auto members = f.list("active");
   BOOST_CHECK(std::find(members.begin(), members.end(), entrant) != members.end());
   const auto lib = f.chain.last_irreversible_block_num();
   f.until([&] { return f.candidate(entrant)["banned"].as_bool(); }, 1000);
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 21u);
   BOOST_CHECK(f.list("active") == members);
   BOOST_CHECK_EQUAL(f.produced[entrant], 0u);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), lib);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, entrant.to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.candidate(entrant)["zero_windows"].as_uint64(), 3u);
   const auto evidence = f.candidate(entrant)["evidence_slots"].get_array();
   BOOST_CHECK_EQUAL(evidence[1].as_uint64() - evidence[0].as_uint64(), 252u);
   BOOST_CHECK_EQUAL(evidence[2].as_uint64() - evidence[1].as_uint64(), 252u);
   f.completed(22); // failed entrant is NOT counted as a successful admission
   BOOST_CHECK_EQUAL(f.rotation()["failed_admissions"].as_uint64(), 1u);
   const auto replaced = f.list("active");
   BOOST_CHECK(std::find(replaced.begin(), replaced.end(), entrant) == replaced.end());
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", entrant), entrant);
   f.call("regproducer"_n, fc::mutable_variant_object()("producer", entrant)
      ("producer_key", f.chain.get_public_key(entrant, "active"))("url", "")("location", 0), entrant);
   const auto extra = "bannedkey"_n;
   auto [priv, pub, pop] = get_bls_key(extra);
   f.call("regfinkey"_n, fc::mutable_variant_object()("finalizer_name", entrant)
      ("finalizer_key", pub.to_string())("proof_of_possession", pop.to_string()), entrant);
   f.call("actfinkey"_n, fc::mutable_variant_object()("finalizer_name", entrant)("finalizer_key", pub.to_string()), entrant);
   f.block();
   f.online.push_back(entrant);
   f.online.push_back(extra);
   f.restart(f.online);
   for (unsigned i = 0; i < 180; ++i) f.step();
   BOOST_CHECK_EQUAL(resource_json(f.candidate(entrant)["evidence_slots"]), resource_json(evidence));
   BOOST_CHECK(f.candidate(entrant)["banned"].as_bool());
   BOOST_CHECK_EQUAL(f.produced[entrant], 0u);
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 22u);
   f.snapshot_check(); // permanent evidence and queue indexes survive snapshot restore too
   f.observer_replay();
   BOOST_TEST_MESSAGE("Offline entrant banned after three finalized personal windows; replaced one seat; re-registration/key changes/restart cannot clear ban");
}

BOOST_AUTO_TEST_CASE(no_alternative_keeps_nonempty_committee_and_no_automatic_ban_under21) {
   sequential_rotation_fixture f(1);
   f.completed(1);
   for (unsigned i = 0; i < 60; ++i) f.step();
   BOOST_CHECK_EQUAL(f.list("active").size(), 1u);
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(f.rotation()["stalled_windows"].as_uint64(), 0u);
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", f.nodes[0]), f.nodes[0]);
   f.block();
   for (unsigned i = 0; i < 60; ++i) f.step();
   BOOST_CHECK_EQUAL(f.list("active").size(), 1u);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_producers().producers.size(), 1u);
   BOOST_CHECK_EQUAL(f.rotation()["stage"].as_uint64(), 0u);
   BOOST_TEST_MESSAGE("No eligible replacement: retained real nonempty single-node policy; no asset or blacklist mutations");
}

BOOST_AUTO_TEST_CASE(key_change_during_handoff_waits_then_updates_without_changing_seats) {
   sequential_rotation_fixture f(2);
   f.completed(1);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   const auto target = f.list("target");
   const auto generation = f.rotation()["finalizer_generation"].as_uint64();
   const auto extra = "newblskey"_n;
   auto [priv, pub, pop] = get_bls_key(extra);
   f.call("regfinkey"_n, fc::mutable_variant_object()("finalizer_name", f.nodes[1])
      ("finalizer_key", pub.to_string())("proof_of_possession", pop.to_string()), f.nodes[1]);
   f.call("actfinkey"_n, fc::mutable_variant_object()("finalizer_name", f.nodes[1])
      ("finalizer_key", pub.to_string()), f.nodes[1]);
   f.block();
   auto signers = f.online;
   signers.push_back(extra);
   f.restart(signers);
   f.completed(2);
   BOOST_CHECK(f.list("active") == target);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->generation, generation);
   const auto proposer_version = f.chain.control->head_active_producers().version;
   const auto queue = f.list("queue");
   f.until([&] { return f.rotation()["stage"].as_uint64() == 3; });
   f.until([&] { return f.rotation()["stage"].as_uint64() == 0; });
   BOOST_CHECK(f.list("queue") == queue);
   BOOST_CHECK(f.list("active") == target);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_producers().version, proposer_version);
   const auto policy = f.chain.control->head_active_finalizer_policy();
   BOOST_REQUIRE_EQUAL(policy->generation, generation + 1);
   BOOST_CHECK(policy->finalizers[1].public_key == pub);
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(large_valid_signing_authority_does_not_strand_rotation_context) {
   sequential_rotation_fixture f(1);
   f.completed(1);
   std::vector<key_weight> keys{{f.chain.get_public_key(f.nodes[0], "active"), 1}};
   for (unsigned i = 0; i < 2048; ++i)
      keys.push_back({f.chain.get_public_key(f.nodes[0], "rotation-large-" + std::to_string(i)), 1});
   std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
   fc::variants authority{fc::variant("block_signing_authority_v0"),
      fc::variant(fc::mutable_variant_object()("threshold", 1)("keys", keys))};
   f.call("regproducer2"_n, fc::mutable_variant_object()("producer", f.nodes[0])
      ("producer_authority", authority)("url", "")("location", 0), f.nodes[0]);
   f.block();
   f.until([&] { return f.rotation()["stage"].as_uint64() == 3; });
   f.until([&] { return f.rotation()["stage"].as_uint64() == 0; });
   BOOST_CHECK_GT(f.chain.control->get_rotation_context_v1().size(), 65536u);
   BOOST_CHECK_EQUAL(f.list("active").size(), 1u);
   f.block();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(small_committees_quorum_loss_and_recovery_under_rotation_profile) {
   for (size_t count : {2u, 3u, 4u}) {
      sequential_rotation_fixture f(count);
      f.completed(count);
      const auto members = f.list("active");
      const auto policy = *f.chain.control->head_active_finalizer_policy();
      const uint32_t span = count * config::producer_repetitions;
      f.online.assign(members.begin(), members.end() - 1);
      f.restart(f.online);
      f.production_slots(span); // drain pre-fault QCs
      const auto before = f.chain.last_irreversible_block_num();
      const auto sample = f.production_slots(span * 2);
      BOOST_CHECK_EQUAL(sample.first, (count - 1) * 24);
      BOOST_CHECK_EQUAL(sample.second, 24u);
      if (count < 4) BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), before);
      else BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), before);
      BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), count);
      BOOST_CHECK(f.list("active") == members);
      BOOST_CHECK(*f.chain.control->head_active_finalizer_policy() == policy);
      if (count == 4) {
         f.online.resize(2);
         f.restart(f.online);
         f.production_slots(span);
         const auto halted = f.chain.last_irreversible_block_num();
         f.production_slots(span * 2);
         BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), halted);
         f.online.push_back(members[2]);
         f.restart(f.online);
         f.production_slots(span);
         BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), halted);
      }
      const auto checkpoint = f.chain.last_irreversible_block_num();
      f.online = members;
      f.restart(f.online);
      f.production_slots(span * 2);
      BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), checkpoint);
      BOOST_CHECK(f.list("active") == members); // no automatic ban under 21
      f.observer_replay();
      BOOST_TEST_MESSAGE(count << " members: real proposer outages/quorum loss/restart recovery tested in rotation WASM profile");
   }
}

BOOST_AUTO_TEST_CASE(rotation_committee_six_offline_seven_offline_and_recovery) {
   sequential_rotation_fixture f(21);
   f.completed(21);
   const auto members = f.list("active");
   const auto policy = *f.chain.control->head_active_finalizer_policy();
   const auto checkpoint_num = f.chain.last_irreversible_block_num();
   const auto checkpoint_id = f.chain.last_irreversible_block_id();
   BOOST_REQUIRE_EQUAL(members.size(), 21u);
   BOOST_REQUIRE_EQUAL(policy.finalizers.size(), 21u);
   BOOST_REQUIRE_EQUAL(policy.threshold, 15u);
   // Test the STATIC 21-member quorum before any member can accumulate three
   // complete zero-output windows. Previously six absences were allowed to
   // earn permanent bans first; their legitimate eviction changed membership
   // and made the subsequent hardcoded 14/21 expectation invalid.
   f.online.assign(members.begin(), members.begin() + 14);
   f.restart(f.online);
   for (unsigned i = 0; i < 8; ++i) f.block(); // drain pre-stop QCs, not three rounds
   const auto halted = f.chain.last_irreversible_block_num();
   const auto counts = f.produced;
   const auto sample = f.production_slots(252);
   BOOST_CHECK_EQUAL(sample.first, 14u * 12);
   BOOST_CHECK_EQUAL(sample.second, 7u * 12);
   BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), halted);
   BOOST_REQUIRE(*f.chain.control->head_active_finalizer_policy() == policy);
   BOOST_REQUIRE(f.list("active") == members);
   BOOST_REQUIRE_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   for (size_t i = 14; i < members.size(); ++i)
      BOOST_CHECK_EQUAL(f.produced[members[i]], counts.at(members[i]));
   f.online.push_back(members[14]);
   f.restart(f.online);
   // At most 84 skipped slots + 8 draining blocks + 252 + 126 < 516 slots,
   // the minimum span containing three full personal windows of one member.
   // Thus recovery proves 15/21 WITHOUT accidentally testing a smaller policy.
   f.production_slots(126);
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), halted);
   BOOST_REQUIRE(*f.chain.control->head_active_finalizer_policy() == policy);
   BOOST_REQUIRE(f.list("active") == members);
   BOOST_REQUIRE_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   for (size_t i = 15; i < members.size(); ++i)
      BOOST_CHECK_EQUAL(f.produced[members[i]], counts.at(members[i]));
   f.online = members;
   f.restart(f.online);
   f.verify_full_production_round();
   BOOST_CHECK(f.chain.control->fetch_block_by_number(checkpoint_num)->calculate_id() == checkpoint_id);
   BOOST_CHECK(f.list("active") == members);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.observer_replay();
   BOOST_TEST_MESSAGE("Static 21-member profile: 14/21 stalls, restoring 15/21 recovers before ban windows; actual missing slots produce no blocks. Permanent-ban eviction is tested separately.");
}

BOOST_AUTO_TEST_CASE(producing_without_new_finalizer_quorum_cannot_complete_handoff) {
   sequential_rotation_fixture f(3);
   f.completed(2);
   const std::vector<name> old_signers(f.nodes.begin(), f.nodes.begin() + 2);
   f.restart(old_signers); // all three proposers online, but node 3 never votes
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   for (unsigned i = 0; i < 24; ++i) f.block(252);
   const auto completed = f.rotation()["completed"].as_uint64();
   const auto lib = f.chain.last_irreversible_block_num();
   const auto head = f.chain.head().block_num();
   for (unsigned i = 0; i < 24; ++i) f.block(252);
   BOOST_CHECK_EQUAL(completed, 2u);
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 2u);
   BOOST_CHECK_EQUAL(f.chain.last_irreversible_block_num(), lib);
   BOOST_CHECK_GT(f.chain.head().block_num(), head);
   f.restart(f.online);
   f.completed(3);
   BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, 3u);
   f.observer_replay();
   BOOST_TEST_MESSAGE("2 -> 3 handoff cannot finish without the newcomer's BLS confirmations; restoring its signer recovers");
}

BOOST_AUTO_TEST_CASE(ineligible_incumbent_replaced_one_at_a_time_before_growth) {
   sequential_rotation_fixture f(4);
   f.completed(3);
   const auto departed = f.nodes[1];
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", departed), departed);
   f.block();
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   BOOST_CHECK_EQUAL(f.rotation()["outgoing"].as_string(), departed.to_string());
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), f.nodes[3].to_string());
   f.completed(4);
   const std::vector<name> expected{f.nodes[0], f.nodes[2], f.nodes[3]};
   BOOST_CHECK(f.list("active") == expected);
   for (unsigned i = 0; i < 12; ++i) f.step();
   BOOST_CHECK(f.list("active") == expected);
   // Existing finalizer registration and votes survive unregprod.
   f.call("regproducer"_n, fc::mutable_variant_object()("producer", departed)
      ("producer_key", f.chain.get_public_key(departed, "active"))("url", "")("location", 0), departed);
   f.block();
   f.completed(5);
   auto restored = expected;
   restored.push_back(departed);
   BOOST_CHECK(f.list("active") == restored);
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(real_211_candidates_exact_vote_boundary_and_persistent_queue) {
   sequential_rotation_fixture f(211);
   f.completed(1);
   auto queue = f.list("queue");
   BOOST_REQUIRE_EQUAL(queue.size(), 210u);
   const auto outsider = f.nodes[210], boundary = f.nodes[209];
   BOOST_CHECK(std::find(queue.begin(), queue.end(), outsider) == queue.end());
   f.stake("100.0000 SYS", "0.0000 SYS", outsider, outsider); // tie rank 210
   f.block();
   f.completed(2);
   queue = f.list("queue");
   BOOST_CHECK(std::find(queue.begin(), queue.end(), boundary) != queue.end());
   BOOST_CHECK(std::find(queue.begin(), queue.end(), outsider) == queue.end());
   f.stake("0.0001 SYS", "0.0000 SYS", outsider, outsider);
   f.block();
   f.completed(3);
   queue = f.list("queue");
   BOOST_REQUIRE_EQUAL(queue.size(), 210u);
   BOOST_CHECK(std::find(queue.begin(), queue.end(), boundary) == queue.end());
   BOOST_CHECK(std::find(queue.begin(), queue.end(), outsider) != queue.end());
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), f.nodes[2].to_string());
   // A newly qualified candidate is appended AFTER the existing waiters.
   const auto outsider_it = std::find(queue.begin(), queue.end(), outsider);
   const auto waiting_it = std::find(queue.begin(), queue.end(), f.nodes[3]);
   BOOST_CHECK(waiting_it < outsider_it);
   f.block();
   f.chain.control->abort_block();
   const auto persisted = resource_json(f.rotation());
   f.restart(f.online);
   BOOST_CHECK_EQUAL(resource_json(f.rotation()), persisted);
   f.observer_replay();
   BOOST_TEST_MESSAGE("211 real registered/voted candidates: cap 210; equal votes use account order; +0.0001 SYS changes eligibility, not FIFO priority");
}

BOOST_AUTO_TEST_CASE(snapshot_during_handoff_restores_policy_queue_and_deterministic_context) {
   sequential_rotation_fixture f(2);
   f.completed(1);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 1; });
   f.block();
   f.chain.control->abort_block();
   BOOST_REQUIRE_EQUAL(f.rotation()["stage"].as_uint64(), 1u);
   auto writer = buffered_snapshot_suite::get_writer();
   f.chain.control->write_snapshot(writer);
   auto snapshot = buffered_snapshot_suite::finalize(writer);
   fc::temp_directory restored_dir;
   const auto cfg = base_tester::default_config(restored_dir).first;
   tester restored(cfg, f.genesis, call_startup_t::no);
   restored.close();
   restored.init(cfg, buffered_snapshot_suite::get_reader(snapshot));
   restored.do_check_for_votes(false); // validates, but supplies no BLS votes
   const auto same = [&] {
      BOOST_CHECK(restored.head().id() == f.chain.head().id());
      BOOST_CHECK(restored.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n) ==
                  f.chain.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n));
      BOOST_CHECK(restored.control->get_rotation_context_v1() == f.chain.control->get_rotation_context_v1());
   };
   same();
   for (unsigned i = 0; i < 1000 && f.rotation()["completed"].as_uint64() < 2; ++i) {
      f.step();
      restored.push_block(f.chain.control->fetch_block_by_number(f.chain.head().block_num()));
   }
   BOOST_REQUIRE_EQUAL(f.rotation()["completed"].as_uint64(), 2u);
   restored.push_block(f.block()); // commit the observed completion on both sides
   f.chain.control->abort_block();
   same();
   BOOST_CHECK(restored.last_irreversible_block_id() == f.chain.last_irreversible_block_id());
   BOOST_TEST_MESSAGE("In-flight rotation snapshot restored into a keyless controller and validated through the completed handoff");
}

BOOST_AUTO_TEST_CASE(dynamic_capacity_opens_individual_slots_and_four_year_epochs_on_chain) {
   sequential_rotation_fixture f(211);
   f.completed(1);
   const uint64_t origin = f.rotation()["growth_start_slot"].as_uint64();
   const uint64_t period = uint64_t(1461) * 86400 * 2;
   const auto advance_to = [&](uint64_t slot) {
      const auto current = f.chain.head().header().timestamp.slot;
      BOOST_REQUIRE_GT(slot, current);
      f.block(uint32_t(slot - current));
      f.block();
   };
   const auto first = (period + 209) / 210;
   advance_to(origin + first - 2);
   BOOST_CHECK_EQUAL(f.rotation()["capacity"].as_uint64(), 210u);
   f.block();
   BOOST_CHECK_EQUAL(f.rotation()["capacity"].as_uint64(), 211u);
   f.completed(2);
   // A snapshot already started just before the boundary retains its frozen
   // capacity. Only the next snapshot admits the newly opened slot.
   if (f.rotation()["snapshot_capacity"].as_uint64() == 210) f.completed(3);
   BOOST_CHECK_EQUAL(f.list("queue").size(), 211u);
   for (uint64_t epoch = 1; epoch <= 3; ++epoch) {
      advance_to(origin + epoch * period - 2);
      BOOST_CHECK_EQUAL(f.rotation()["capacity"].as_uint64(), (210u << epoch) - 1);
      f.block();
      BOOST_CHECK_EQUAL(f.rotation()["capacity"].as_uint64(), 210u << epoch);
      BOOST_CHECK_LE(f.list("active").size(), 21u);
   }
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.observer_replay();
   BOOST_TEST_MESSAGE("Real WASM: first added candidate slot, then 420/840/1680 capacities; growth never bulk-replaces the active committee");
}

BOOST_AUTO_TEST_CASE(paginated_snapshot_survives_concurrent_vote_changes_and_restart) {
   sequential_rotation_fixture f(211);
   f.completed(1);
   f.until([&] { return f.rotation()["ranking"].as_bool(); });
   BOOST_REQUIRE_LT(f.rotation()["ranked_count"].as_uint64(), 210u);
   const auto outsider = f.nodes[210];
   f.stake("21100.0000 SYS", "0.0000 SYS", outsider, outsider);
   f.stake("0.0001 SYS", "0.0000 SYS", outsider, outsider); // keep the FIRST before-image
   f.block();
   f.chain.control->abort_block();
   const auto partial = resource_json(f.rotation());
   f.restart(f.online);
   BOOST_CHECK_EQUAL(resource_json(f.rotation()), partial);
   while (f.rotation()["ranking"].as_bool()) {
      const auto count = f.rotation()["ranked_count"].as_uint64();
      f.block();
      f.chain.control->abort_block(); // measure one COMMITTED onblock, not a speculative second one
      BOOST_CHECK_LE(f.rotation()["ranked_count"].as_uint64() - count, 16u);
   }
   f.completed(2);
   auto queue = f.list("queue");
   BOOST_CHECK(std::find(queue.begin(), queue.end(), outsider) == queue.end());
   f.completed(3);
   queue = f.list("queue");
   BOOST_CHECK(std::find(queue.begin(), queue.end(), outsider) != queue.end());
   BOOST_CHECK(std::find(queue.begin(), queue.end(), f.nodes[3]) < std::find(queue.begin(), queue.end(), outsider));
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), f.nodes[2].to_string());
   f.observer_replay();
   BOOST_TEST_MESSAGE("Copy-on-write ranked snapshot stayed at one logical time across vote changes and restart; next snapshot applied the new rank without queue jumping");
}

BOOST_AUTO_TEST_CASE(moving_live_keys_cannot_consume_the_frozen_scan_budget) {
   sequential_rotation_fixture f(23, true);
   f.until([&] { return f.rotation()["ranking"].as_bool(); });
   BOOST_REQUIRE_EQUAL(f.rotation()["ranked_count"].as_uint64(), 16u);
   const auto epoch = f.rotation()["snapshot_epoch"].as_uint64();
   // Move all 16 already visited candidates immediately in front of the
   // unvisited low ranks. A merged mutable-index scan would waste its budget
   // on these after-images, and repeating this could starve later candidates.
   for (size_t i = 0; i < 16; ++i) {
      const auto before = resource_json(f.candidate(f.nodes[i])["rank"]);
      const auto amount = std::to_string((2300 - 100 * i - 601) / 2) + ".5000 SYS";
      f.call("undelegatebw"_n, fc::mutable_variant_object()("from", f.nodes[i])("receiver", f.nodes[i])
         ("unstake_net_quantity", amount)("unstake_cpu_quantity", amount), f.nodes[i]);
      BOOST_CHECK_EQUAL(resource_json(f.candidate(f.nodes[i])["previous_rank"]), before);
      BOOST_CHECK_EQUAL(f.candidate(f.nodes[i])["frozen_epoch"].as_uint64(), epoch);
   }
   f.block();
   f.chain.control->abort_block();
   BOOST_REQUIRE(f.rotation()["ranking"].as_bool());
   f.restart(f.online);
   f.block();
   f.chain.control->abort_block();
   BOOST_CHECK(!f.rotation()["ranking"].as_bool());
   BOOST_CHECK_EQUAL(f.rotation()["ranked_count"].as_uint64(), 23u);
   BOOST_CHECK_EQUAL(f.rotation()["published_epoch"].as_uint64(), epoch);
   // We intentionally aborted speculative blocks above. With committed-only
   // state, step() can cross the next-attempt time before we observe completion;
   // the next handoff may already be submitted, so idle is not a requirement.
   f.until([&] { return f.rotation()["completed"].as_uint64() >= 1; });
   f.block();
   f.chain.control->abort_block();
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 1u);
   for (size_t i = 0; i < 16; ++i) {
      const auto c = f.candidate(f.nodes[i]);
      BOOST_CHECK_EQUAL(c["frozen_epoch"].as_uint64(), 0u);
      BOOST_CHECK_EQUAL(resource_json(c["previous_rank"]), resource_json(c["rank"]));
   }
   f.until([&] { return f.rotation()["completed"].as_uint64() >= 2; });
   f.block();
   f.chain.control->abort_block();
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 2u);
   BOOST_CHECK_EQUAL(f.rotation()["incoming"].as_string(), f.nodes[1].to_string());
   f.snapshot_check();
   f.observer_replay();
   BOOST_TEST_MESSAGE("Frozen rank index progresses despite 16 moved live keys; bounded dirty drain/restart/snapshot preserve FIFO");
}

BOOST_AUTO_TEST_CASE(two_empty_windows_recovery_resets_and_global_stalls_do_not_mass_ban) {
   sequential_rotation_fixture f(21);
   f.completed(21);
   const auto members = f.list("active");
   const auto victim = members.back();
   const auto balance = resource_json(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value,
      victim, "eosio.token"_n));
   f.online = members;
   f.online.pop_back();
   f.restart(f.online);
   f.until([&] { return f.candidate(victim)["zero_windows"].as_uint64() == 2; });
   BOOST_CHECK(!f.candidate(victim)["banned"].as_bool());
   f.online = members;
   f.restart(f.online);
   f.until([&] { return f.candidate(victim)["zero_windows"].as_uint64() == 0; });
   BOOST_CHECK(!f.candidate(victim)["banned"].as_bool());
   // A whole-chain gap longer than H must not count all missing windows as bans.
   f.block(1260);
   for (unsigned i = 0; i < 80; ++i) f.step();
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.until([&] { return !f.rotation()["penalty_paused"].as_bool(); });
   f.online.resize(14);
   f.restart(f.online);
   f.until([&] { return f.rotation()["penalty_paused"].as_bool(); });
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   const auto halted = f.chain.last_irreversible_block_num();
   f.online = members;
   f.restart(f.online);
   f.until([&] { return !f.rotation()["penalty_paused"].as_bool(); });
   for (unsigned i = 0; i < 80; ++i) f.step();
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), halted);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(resource_json(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value,
      victim, "eosio.token"_n)), balance);
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(twenty_members_many_missed_windows_never_create_penalties) {
   sequential_rotation_fixture f(20);
   f.completed(20);
   const auto missing = f.nodes.back();
   f.online.erase(std::find(f.online.begin(), f.online.end(), missing));
   f.restart(f.online);
   const auto before = f.produced[missing];
   for (unsigned i = 0; i < 150; ++i) f.step();
   BOOST_CHECK_EQUAL(f.produced[missing], before);
   BOOST_CHECK_EQUAL(f.candidate(missing)["zero_windows"].as_uint64(), 0u);
   BOOST_CHECK(!f.candidate(missing)["banned"].as_bool());
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   BOOST_CHECK(f.rotation()["penalty_paused"].as_bool());
}

BOOST_AUTO_TEST_CASE(real_841_candidates_grow_through_315_420_630_840_with_bounded_scans) {
   sequential_rotation_fixture f(841);
   f.completed(1);
   BOOST_REQUIRE_EQUAL(f.list("queue").size(), 210u);
   const uint64_t origin = f.rotation()["growth_start_slot"].as_uint64();
   const uint64_t period = uint64_t(1461) * 86400 * 2;
   const auto row_size = f.chain.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, f.nodes[0]).size();
   for (const auto [half_epochs, capacity] : std::vector<std::pair<uint64_t, uint64_t>>{{1,315},{2,420},{3,630},{4,840}}) {
      const auto slot = origin + half_epochs * period / 2 + 2;
      f.block(uint32_t(slot - f.chain.head().header().timestamp.slot));
      f.until([&] { return f.rotation()["ranking"].as_bool() &&
                            f.rotation()["snapshot_capacity"].as_uint64() == capacity; });
      f.block();
      f.chain.control->abort_block();
      uint32_t batches = 0;
      while (f.rotation()["ranking"].as_bool()) {
         const auto before = f.rotation()["ranked_count"].as_uint64();
         f.block();
         f.chain.control->abort_block();
         BOOST_REQUIRE_LE(f.rotation()["ranked_count"].as_uint64() - before, 16u);
         BOOST_REQUIRE_LE(++batches, (capacity + 15) / 16 + 1);
      }
      const auto queue = f.list("queue");
      BOOST_REQUIRE_EQUAL(queue.size(), capacity);
      BOOST_CHECK(std::find(queue.begin(), queue.end(), f.nodes[capacity - 1]) != queue.end());
      BOOST_CHECK(std::find(queue.begin(), queue.end(), f.nodes[capacity]) == queue.end());
      BOOST_CHECK_LE(f.list("active").size(), 21u);
      BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
      for (auto n : f.nodes)
         BOOST_CHECK_EQUAL(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "rotcands"_n, n).size(), row_size);
      f.restart(f.online);
      BOOST_CHECK(f.list("queue") == queue);
      BOOST_TEST_MESSAGE("841 real candidates: capacity=" << capacity << ", committed rank batches=" << batches);
   }
   f.snapshot_check();
   f.observer_replay();
}

namespace {
void incumbent_fault_scenario(uint32_t seed, size_t missing_count) {
   sequential_rotation_fixture f(21, true);
   f.completed(21);
   const auto members = f.list("active");
   auto shuffled = members;
   // Explicit deterministic PRNG; this varies the actual faulted accounts,
   // not just Boost's order of execution. Every seed is printed for replay.
   const auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
   for (size_t i = shuffled.size() - 1; i; --i) std::swap(shuffled[i], shuffled[next() % (i + 1)]);
   const std::vector<name> victims(shuffled.begin(), shuffled.begin() + missing_count);
   std::map<name, uint64_t> historical_blocks;
   std::map<name, std::string> balances;
   const auto balance = [&](name n) {
      return resource_json(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value, n, "eosio.token"_n));
   };
   const auto disconnect = [&] {
      f.online = members;
      for (auto n : victims) f.online.erase(std::find(f.online.begin(), f.online.end(), n));
      f.restart(f.online);
   };
   disconnect();
   f.until([&] { return std::all_of(victims.begin(), victims.end(), [&](name n) {
      return f.candidate(n)["zero_windows"].as_uint64() == 2; }); });
   for (auto n : victims) BOOST_CHECK(!f.candidate(n)["banned"].as_bool());
   f.online = members;
   f.restart(f.online);
   f.until([&] { return std::all_of(victims.begin(), victims.end(), [&](name n) {
      return f.candidate(n)["zero_windows"].as_uint64() == 0; }); });
   for (auto n : victims) {
      BOOST_CHECK_EQUAL(resource_json(f.candidate(n)["evidence_slots"]), "[0,0,0]");
      historical_blocks[n] = f.row("producersv2"_n, n.to_uint64_t())["unpaid_blocks"].as_uint64();
      BOOST_REQUIRE_GT(historical_blocks[n], 0u);
      balances[n] = balance(n);
   }
   disconnect();
   const auto before = f.chain.last_irreversible_block_num();
   f.until([&] { return f.rotation()["total_banned"].as_uint64() > 0; });
   f.until([&] {
      const auto active = f.list("active");
      return f.rotation()["stage"].as_uint64() == 0 && active.size() < 21 &&
         std::none_of(active.begin(), active.end(), [&](name n) { return f.candidate(n)["banned"].as_bool(); });
   });
   const auto banned_count = f.rotation()["total_banned"].as_uint64();
   BOOST_REQUIRE_GE(banned_count, 1u);
   BOOST_REQUIRE_LE(banned_count, missing_count);
   BOOST_REQUIRE_EQUAL(f.list("active").size(), 21u - banned_count);
   f.block();
   f.chain.control->abort_block();
   BOOST_CHECK_GT(f.chain.last_irreversible_block_num(), before);
   for (auto n : members) {
      const bool victim = std::find(victims.begin(), victims.end(), n) != victims.end();
      const bool banned = f.candidate(n)["banned"].as_bool();
      if (!victim) BOOST_CHECK(!banned);
      if (!victim) continue;
      const auto c = f.candidate(n);
      if (banned) {
         const auto evidence = c["evidence_slots"].get_array();
         BOOST_CHECK_EQUAL(c["zero_windows"].as_uint64(), 3u);
         BOOST_CHECK_EQUAL(evidence[1].as_uint64() - evidence[0].as_uint64(), 252u);
         BOOST_CHECK_EQUAL(evidence[2].as_uint64() - evidence[1].as_uint64(), 252u);
         BOOST_CHECK_LE(c["ban_final_block"].as_uint64(), f.chain.last_irreversible_block_num());
      }
      BOOST_CHECK_EQUAL(f.row("producersv2"_n, n.to_uint64_t())["unpaid_blocks"].as_uint64(), historical_blocks[n]);
      BOOST_CHECK_EQUAL(balance(n), balances[n]);
   }
   // Once the real committee shrinks below 21, all new penalties pause. Some
   // other victims may have only two finalized windows: never manufacture a
   // third one just to satisfy a pre-eviction expected count.
   f.production_slots(3 * 21 * config::producer_repetitions);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), banned_count);
   BOOST_CHECK(f.rotation()["penalty_paused"].as_bool());
   BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold, (21u-banned_count)*2/3+1);
   f.snapshot_check();
   f.observer_replay();
   BOOST_TEST_MESSAGE("Incumbent fault scenario: " << missing_count << " victims, two zero windows then recovery; " << banned_count << " finalized bans safely evicted; remaining under-21 penalties paused; balances/history intact");
}
}

BOOST_AUTO_TEST_CASE(incumbent_fault_seed_17_one_offline) { incumbent_fault_scenario(17, 1); }
BOOST_AUTO_TEST_CASE(incumbent_fault_seed_2027_two_offline) { incumbent_fault_scenario(2027, 2); }
BOOST_AUTO_TEST_CASE(incumbent_fault_seed_65537_three_offline) { incumbent_fault_scenario(65537, 3); }

BOOST_AUTO_TEST_CASE(real_reward_claim_pays_producer_but_never_produced_candidate_cannot_claim) {
   sequential_rotation_fixture f(2, true);
   f.completed(1);
   const auto producer = f.nodes[0], waiting = f.nodes[1];
   f.choose("alice"_n, waiting);
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "160000000.0000 SYS"));
   BOOST_REQUIRE_GT(f.row("producersv2"_n, waiting.to_uint64_t())["total_votes"].as_int64(),
                    f.row("producersv2"_n, producer.to_uint64_t())["total_votes"].as_int64());
   BOOST_REQUIRE_GT(f.row("producersv2"_n, producer.to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
   BOOST_REQUIRE_EQUAL(f.row("producersv2"_n, waiting.to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
   const auto balance = [&](name n) {
      return asset::from_string(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value,
                                     n, "eosio.token"_n)["balance"].as_string()).get_amount();
   };
   const auto paid_before = balance(producer), waiting_before = balance(waiting);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", waiting), waiting),
      fc::exception, [](const auto& e) { return resource_split_fixture::message(e, "standby rewards are disabled"); });
   f.call("claimrewards"_n, fc::mutable_variant_object()("owner", producer), producer);
   BOOST_CHECK_GT(balance(producer), paid_before);
   BOOST_CHECK_EQUAL(balance(waiting), waiting_before);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, producer.to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
   BOOST_CHECK_THROW(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", producer), producer), fc::exception);
   f.block();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(large_vote_balances_cannot_affect_exact_block_reward_share) {
   sequential_rotation_fixture f(2, true);
   f.completed(1);
   const auto producer = f.nodes[0];
   // Activation fixture redeemed this amount back into alice's REX fund.
   f.choose("alice"_n, producer);
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "160000000.0000 SYS"));
   f.block();
   const auto votes = f.row("producersv2"_n, producer.to_uint64_t())["total_votes"].as_int64();
   f.block(2 * 86400); // accrue a real reward bucket large enough to overflow the old product
   const auto blocks = f.row("producersv2"_n, producer.to_uint64_t())["unpaid_blocks"].as_uint64();
   const auto total = f.row("globalv2"_n, "globalv2"_n.to_uint64_t())["total_unpaid_blocks"].as_uint64();
   const auto trace = f.call("claimrewards"_n, fc::mutable_variant_object()("owner", producer), producer);
   abi_def abi;
   BOOST_REQUIRE(abi_serializer::to_abi(f.chain.control->db().get<account_object, by_name>("eosio.token"_n).abi, abi));
   auto yield = abi_serializer::create_yield_function(fc::seconds(5));
   abi_serializer serializer(abi, yield);
   int64_t paid = 0;
   for (const auto& a : trace->action_traces) {
      if (a.receiver != "eosio.token"_n || a.act.account != "eosio.token"_n || a.act.name != "transfer"_n) continue;
      const auto data = serializer.binary_to_variant("transfer", a.act.data, yield);
      BOOST_CHECK_NE(data["from"].as_string(), "eosio.vpay");
      BOOST_CHECK_NE(data["to"].as_string(), "eosio.vpay");
      if (data["from"].as_string() == "eosio.bpay" && data["to"].as_string() == producer.to_string())
         paid += asset::from_string(data["quantity"].as_string()).get_amount();
   }
   const auto remaining = f.row("globalv2"_n, "globalv2"_n.to_uint64_t())["perblock_bucket"].as_int64();
   const int64_t available = remaining + paid;
   const unsigned __int128 product = static_cast<unsigned __int128>(available) * votes;
   BOOST_REQUIRE(product > uint64_t(std::numeric_limits<int64_t>::max()));
   // Votes are deliberately large enough to overflow the removed vote-pay
   // multiplication. The real payout uses block counts instead.
   const int64_t expected = int64_t(static_cast<unsigned __int128>(available) * blocks / total);
   BOOST_CHECK_EQUAL(paid, expected);
   BOOST_CHECK_EQUAL(remaining, available - expected);
   BOOST_CHECK_EQUAL(f.row("globalv2"_n, "globalv2"_n.to_uint64_t())["pervote_bucket"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, producer.to_uint64_t())["total_votes"].as_int64(), votes);
   BOOST_CHECK_EQUAL(f.row("producersv2"_n, producer.to_uint64_t())["unpaid_blocks"].as_uint64(), 0u);
   f.block();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(under21_offline_entrant_is_replaced_without_blacklist_or_growth) {
   sequential_rotation_fixture f(5);
   f.completed(3);
   const auto entrant = f.nodes[3], replacement = f.nodes[4];
   f.online.erase(std::find(f.online.begin(), f.online.end(), entrant));
   f.restart(f.online);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 2; });
   BOOST_REQUIRE_EQUAL(f.rotation()["incoming"].as_string(), entrant.to_string());
   const auto baseline = f.rotation()["completed"].as_uint64();
   bool replaced = false;
   for (unsigned i = 0; i < 1600 && !replaced; ++i) {
      f.block(12 - f.chain.head().header().timestamp.slot % 12);
      const auto members = f.list("active");
      BOOST_REQUIRE_LE(members.size(), 4u); // replace the untested seat, do not grow around it
      replaced = std::find(members.begin(), members.end(), replacement) != members.end() &&
                 std::find(members.begin(), members.end(), entrant) == members.end() &&
                 f.rotation()["completed"].as_uint64() > baseline;
   }
   BOOST_REQUIRE_MESSAGE(replaced, "offline under-21 entrant still blocks the next candidate");
   BOOST_CHECK_GT(f.rotation()["failed_admissions"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   BOOST_CHECK(!f.candidate(entrant)["banned"].as_bool());
   BOOST_CHECK_EQUAL(f.candidate(entrant)["zero_windows"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.produced[entrant], 0u);
   // The temporarily rotated-out account remains eligible and can rejoin.
   f.online.push_back(entrant);
   f.restart(f.online);
   f.until([&] { return f.rotation()["completed"].as_uint64() >= baseline + 2; });
   const auto members = f.list("active");
   BOOST_CHECK(std::find(members.begin(), members.end(), entrant) != members.end());
   BOOST_CHECK_GT(f.produced[entrant], 0u);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.snapshot_check();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(alternating_incumbent_misses_cannot_veto_all_rotations) {
   sequential_rotation_fixture f(21);
   f.completed(21);
   const uint64_t baseline = 21;
   const std::vector<name> attackers{f.nodes[0], f.nodes[1], f.nodes[2]};
   // Each attacker produces in one out of three rounds: at most two consecutive
   // personal zero windows, but some finalized zero evidence always remains.
   // They keep signing finality votes while strategically withholding blocks.
   auto slot = (f.chain.head().header().timestamp.slot / 12 + 1) * 12;
   uint32_t blocked_samples = 0;
   const auto tick = [&](uint32_t when) {
      const auto round = when / 252;
      f.online.assign(f.nodes.begin(), f.nodes.end());
      f.online.push_back("alice"_n);
      for (size_t i = 0; i < attackers.size(); ++i)
         if (round % 3 != i) f.online.erase(std::find(f.online.begin(), f.online.end(), attackers[i]));
      const auto scheduled = f.chain.control->head_active_producers(block_timestamp_type{when})
                                .get_scheduled_producer(block_timestamp_type{when}).producer_name;
      if (std::find(f.online.begin(), f.online.end(), scheduled) == f.online.end()) return;
      f.block(when - f.chain.head().header().timestamp.slot);
      if (f.rotation()["blocked"].as_uint64() == 3) ++blocked_samples;
   };
   // Establish the alternating pattern with a stable 21-member schedule and
   // no replacement yet. This avoids mistaking a startup grace window or a
   // shifted producer slot during a handoff for the adversarial steady state.
   const auto warm_end = slot + 3 * 252;
   for (; slot < warm_end; slot += 12) tick(slot);
   BOOST_REQUIRE_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.register_node("alice"_n);
   f.stake("10.0000 SYS", "10.0000 SYS");
   f.choose("alice"_n, "alice"_n);
   auto signers = f.nodes;
   signers.push_back("alice"_n);
   f.block(); // commit registration and votes before closing/reopening the tester
   f.restart(signers);
   bool submitted = false;
   const auto limit = slot + 12 * 252;
   // A real block per available window is enough for the zero/nonzero property.
   for (; slot < limit && !submitted; slot += 12) {
      tick(slot);
      submitted = f.rotation()["stage"].as_uint64() == 1 && f.rotation()["incoming"].as_string() == "alice";
   }
   BOOST_TEST_MESSAGE("Alternating-miss experiment: blocked samples=" << blocked_samples
      << ", replacement submitted=" << submitted);
   if (!submitted) BOOST_TEST_MESSAGE("Final rotation state: " << resource_json(f.rotation()));
   BOOST_REQUIRE_MESSAGE(submitted, "alternating incumbent zero windows still veto every rotation");
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   const auto outgoing = name{f.rotation()["outgoing"].as_string()};
   BOOST_CHECK(std::find(attackers.begin(), attackers.end(), outgoing) != attackers.end());
   f.online = signers; // stop the attack after proving it cannot veto submission
   f.completed(baseline + 1);
   BOOST_CHECK_EQUAL(f.list("active").size(), 21u);
   f.block();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(under21_global_pause_and_no_replacement_preserve_membership_until_late_candidate) {
   sequential_rotation_fixture f(4);
   f.completed(3);
   const auto entrant = f.nodes[3];
   f.online.erase(std::find(f.online.begin(), f.online.end(), entrant));
   f.restart(f.online);
   f.until([&] { return f.rotation()["stage"].as_uint64() == 2; });
   f.block(4 * 21 * 12 + 48); // whole-chain pause, not evidence against a particular node
   BOOST_CHECK_EQUAL(f.rotation()["failed_admissions"].as_uint64(), 0u);
   f.until([&] { return f.rotation()["blocked"].as_uint64() == 2; }, 300);
   const auto members = f.list("active");
   BOOST_REQUIRE_EQUAL(members.size(), 4u);
   f.production_slots(4 * 48);
   BOOST_CHECK(f.list("active") == members); // no shrinking, invented signer or empty policy
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   BOOST_CHECK_EQUAL(f.candidate(entrant)["zero_windows"].as_uint64(), 0u);
   f.register_node("alice"_n);
   f.stake("10.0000 SYS", "10.0000 SYS");
   f.choose("alice"_n, "alice"_n);
   f.online.push_back("alice"_n);
   f.block(); // pending user transactions must survive the restart
   f.restart(f.online);
   f.completed(4);
   const auto updated = f.list("active");
   BOOST_CHECK(std::find(updated.begin(), updated.end(), "alice"_n) != updated.end());
   BOOST_CHECK(std::find(updated.begin(), updated.end(), entrant) == updated.end());
   BOOST_CHECK(!f.candidate(entrant)["banned"].as_bool());
   f.snapshot_check();
   f.observer_replay();
}

BOOST_AUTO_TEST_CASE(governance_live_index_skips_incomplete_registrations_and_tracks_mutations) {
   resource_split_fixture f(true, 1025, PUREEOS_ROTATION_CONTRACTS_DIR, true);
   f.populate_sparse_governance_candidates(true);
   f.seal();
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal())}, "developer"_n);
   BOOST_REQUIRE_EQUAL(f.state().proposal_voters.size(), 210u);
   BOOST_CHECK(std::find(f.state().proposal_voters.begin(), f.state().proposal_voters.end(), f.nodes.back()) !=
               f.state().proposal_voters.end());
   const auto frozen = fc::raw::pack(f.state().proposal_voters);
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", f.nodes[0]), f.nodes[0]);
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_CHECK_EQUAL(f.state().current_voters.size(), 209u);
   f.register_node(f.nodes[0], false);
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_CHECK_EQUAL(f.state().current_voters.size(), 210u);
   const auto [priv, pub, pop] = get_bls_key(f.nodes.back());
   f.call("delfinkey"_n, fc::mutable_variant_object()("finalizer_name", f.nodes.back())
      ("finalizer_key", pub.to_string()), f.nodes.back());
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_CHECK_EQUAL(f.state().current_voters.size(), 209u);
   f.call("regfinkey"_n, fc::mutable_variant_object()("finalizer_name", f.nodes.back())
      ("finalizer_key", pub.to_string())("proof_of_possession", pop.to_string()), f.nodes.back());
   f.call("voteproducer"_n, fc::mutable_variant_object()("voter", f.nodes[0])("proxy", "")
      ("producers", std::vector<name>{f.nodes.back()}), f.nodes[0]);
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_CHECK_EQUAL(f.state().current_voters.size(), 209u); // zero-vote producer leaves the live index immediately
   BOOST_CHECK(fc::raw::pack(f.state().proposal_voters) == frozen);
}

BOOST_AUTO_TEST_CASE(reviewed_system_self_upgrade_preserves_live_rotation_and_resource_policy) {
   sequential_rotation_fixture f(4);
   f.completed(4);
   const auto wasm = read_wasm((f.contract_dir / "eosio.system/eosio.system.wasm").c_str());
   f.upgrade_code.assign(wasm.begin(), wasm.end());
   f.upgrade_code.insert(f.upgrade_code.end(), {0, 2, 1, 'r'}); // same semantics, different reviewed hash
   const auto& abi = f.chain.control->db().get<account_object, by_name>("eosio"_n).abi;
   f.upgrade_abi.assign(abi.data(), abi.data() + abi.size());
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}},
      f.proposal("eosio"_n))}, "developer"_n);
   BOOST_REQUIRE_EQUAL(f.state().proposal_voters.size(), 4u);
   f.veto(f.nodes[0]);
   f.veto(f.nodes[1]); // 2/4 is below ceil(2N/3), so review can pass
   BOOST_CHECK_THROW(f.execute(), fc::exception); // public review cannot be skipped
   f.wait_review();
   const auto rotation = resource_json(f.rotation()), resources = resource_json(f.policy());
   f.execute();
   BOOST_CHECK(!f.state().pending);
   BOOST_CHECK_EQUAL(resource_json(f.rotation()), rotation);
   BOOST_CHECK_EQUAL(resource_json(f.policy()), resources);
   f.block();
   BOOST_CHECK_EQUAL(f.rotation()["completed"].as_uint64(), 4u);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.check_split();
   f.snapshot_check();
   f.observer_replay();
}

#ifdef PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR
BOOST_AUTO_TEST_CASE(previous_v2_state_survives_reviewed_upgrade_and_new_admission) {
   sequential_rotation_fixture f(5, false, PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR);
   f.completed(3);
   const auto current = std::filesystem::path(PUREEOS_ROTATION_CONTRACTS_DIR) / "eosio.system/eosio.system";
   const auto wasm = read_wasm((current.string() + ".wasm").c_str());
   f.upgrade_code.assign(wasm.begin(), wasm.end());
   abi_def abi;
   fc::from_variant(fc::json::from_file(current.string() + ".abi"), abi);
   f.upgrade_abi = fc::raw::pack(abi);
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}},
      f.proposal("eosio"_n))}, "developer"_n);
   f.wait_review();
   const auto before = f.chain.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n);
   f.execute();
   BOOST_CHECK(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "rotation"_n, "rotation"_n) == before);
   f.block(); // commit the upgrade before step() advances across a policy interval
   f.completed(4); // new WASM must read the actual old row, then continue the handoff
   f.completed(5);
   BOOST_CHECK_EQUAL(f.rotation()["version"].as_uint64(), 2u);
   BOOST_CHECK_EQUAL(f.rotation()["total_banned"].as_uint64(), 0u);
   f.snapshot_check();
   f.observer_replay();
}
#endif

BOOST_AUTO_TEST_SUITE_END()
