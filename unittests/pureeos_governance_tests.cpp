#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <eosio/chain/upgrade_governance.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/permission_object.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#ifdef PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#endif
#include <boost/test/unit_test.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include "snapshot_suites.hpp"

using namespace eosio::chain;
using namespace eosio::testing;

namespace {
genesis_state governance_genesis() {
   auto g = base_tester::default_genesis();
   auto [priv, pub, pop] = get_bls_key("eosio"_n);
   g.initial_savanna.emplace();
   g.initial_savanna->version = 2;
   g.initial_savanna->finalizer_key = pub;
   g.initial_savanna->proof_of_possession = pop;
   return g;
}

// Deliberately permissive TEST ONLY system code: supplies arbitrary electorates
// and privileged forwarding, so native boundary checks are tested independently
// of the production contract's ranking/authorization logic.
std::string fixture_wat() {
   return R"((module
     (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
     (import "env" "set_upgrade_voters" (func $voters (param i32 i32)))
     (import "env" "send_inline" (func $send (param i32 i32)))
     (import "env" "set_privileged" (func $priv (param i64 i32)))
     (memory 1)
     (func (export "apply") (param $receiver i64) (param $code i64) (param $action i64) (local $size i32)
       (if (i64.eq (get_local $action) (i64.const )" + std::to_string("govinit"_n.to_uint64_t()) + R"())
         (then (i64.store (i32.const 0) (i64.const )" + std::to_string("eosio"_n.to_uint64_t()) + R"())
               (call $voters (i32.const 0) (i32.const 8))))
       (if (i64.eq (get_local $action) (i64.const )" + std::to_string("refreshgov"_n.to_uint64_t()) + R"())
         (then (set_local $size (call $read (i32.const 0) (i32.const 65536)))
               (call $voters (i32.const 0) (get_local $size))))
       (if (i64.eq (get_local $action) (i64.const )" + std::to_string("forward"_n.to_uint64_t()) + R"())
         (then (set_local $size (call $read (i32.const 0) (i32.const 65536)))
               (call $send (i32.const 0) (get_local $size))))
       (if (i64.eq (get_local $action) (i64.const )" + std::to_string("privilege"_n.to_uint64_t()) + R"())
         (then (call $priv (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"() (i32.const 1))))
     )))";
}

struct governance_fixture {
   fc::temp_directory dir;
   genesis_state genesis = governance_genesis();
   tester chain{base_tester::default_config(dir).first, genesis};
   std::vector<account_name> nodes;
   uint32_t nonce = 0;
   bytes upgrade_code = [] {
      const auto wasm = wast_to_wasm("(module (func (export \"apply\") (param i64 i64 i64) nop))");
      return bytes(wasm.begin(), wasm.end());
   }();
   bytes upgrade_abi = [] { abi_def abi; abi.version = "eosio::abi/1.2"; return fc::raw::pack(abi); }();

   governance_fixture(size_t count = 4, bool seal_now = true) {
      const std::array<account_name, 1> finalizers{"eosio"_n};
      chain.set_node_finalizers(finalizers);
      chain.produce_blocks(2);
      chain.create_accounts({"developer"_n, "alice"_n, "eosio.token"_n, "eosio.msig"_n, "eosio.wrap"_n});
      for (size_t i = 0; i < count; ++i) {
         std::string n = "node";
         BOOST_REQUIRE_LE(count, 26u * 26u * 26u);
         if (count > 26u * 26u) n += char('a' + i / (26 * 26));
         n += char('a' + (i / 26) % 26);
         n += char('a' + i % 26);
         nodes.emplace_back(n);
         chain.create_account(nodes.back());
         if (i % 32 == 31) chain.produce_block();
      }
      chain.set_authority("developer"_n, "upgrade"_n, authority(chain.get_public_key("developer"_n, "active")), "active"_n);
      chain.set_code("eosio"_n, fixture_wat().c_str());
      if (seal_now) seal();
   }

   const upgrade_governance_object& state() const { return *find_upgrade_governance(chain.control->db()); }
   transaction_trace_ptr push(std::vector<action> actions, account_name signer, std::string key = "active") {
      signed_transaction trx;
      trx.actions = std::move(actions);
      chain.set_transaction_headers(trx, 60 + ++nonce);
      // Simulated policy/review jumps can leave the open block far ahead of
      // head time. After a restart there may be no open block yet; in that
      // case push_transaction starts the next block and the standard headers
      // above already give a valid expiration relative to head time.
      if (chain.control->is_building_block())
         trx.expiration = fc::time_point_sec(chain.control->pending_block_time() + fc::seconds(60 + nonce));
      trx.sign(chain.get_private_key(signer, key), chain.control->get_chain_id());
      return chain.push_transaction(trx);
   }
   template<class T>
   transaction_trace_ptr native(T args, account_name signer, permission_name perm = "active"_n, std::string key = "active") {
      return push({action({permission_level{signer, perm}}, std::move(args))}, signer, key);
   }
   void seal() { native(govinit{"developer"_n, upgrade_review_seconds, {}}, "eosio"_n, "owner"_n); }
   action refresh(const std::vector<account_name>& voters) {
      bytes data;
      for (auto voter : voters) {
         const auto packed = fc::raw::pack(voter);
         data.insert(data.end(), packed.begin(), packed.end());
      }
      return action({permission_level{"developer"_n, "active"_n}}, "eosio"_n, "refreshgov"_n, data);
   }
   devpropose proposal(account_name target = "eosio.token"_n) const {
      const auto& account = chain.control->db().get<account_object, by_name>(target);
      return {target, chain.control->db().get<account_metadata_object, by_name>(target).code_hash,
              fc::sha256::hash(account.abi.data(), account.abi.size()),
              fc::sha256::hash(upgrade_code.data(), upgrade_code.size()),
              fc::sha256::hash(upgrade_abi.data(), upgrade_abi.size()), fc::sha256::hash("published review document")};
   }
   void propose(std::vector<account_name> voters = {}) {
      if (voters.empty()) voters = nodes.empty() ? std::vector<account_name>{"eosio"_n} : nodes;
      push({refresh(voters), action({permission_level{"developer"_n, "upgrade"_n}}, proposal())}, "developer"_n);
   }
   void advance_to(time_point pending_time) {
      chain.produce_block(); // Commit short-lived bootstrap transactions before advancing time.
      const auto delta = pending_time - chain.control->pending_block_time();
      BOOST_REQUIRE(delta >= fc::microseconds(0));
      if (delta > fc::microseconds(0)) chain.produce_block(delta);
      BOOST_REQUIRE(chain.control->pending_block_time() == pending_time);
   }
   void wait_review() { advance_to(state().execute_after); }
   void execute() { native(devexec{state().proposal_id, upgrade_code, upgrade_abi}, "developer"_n, "upgrade"_n); }
   void veto(account_name voter) {
      native(devveto{voter, state().proposal_id}, voter, voter == "eosio"_n ? "veto"_n : "active"_n);
      if (state().vetoes.size() % 25 == 0) chain.produce_block();
   }
   template<class T>
   void forward(T args, account_name actor, permission_name perm = "active"_n) {
      const action child({permission_level{actor, perm}}, std::move(args));
      push({action({permission_level{"alice"_n, "active"_n}}, "eosio"_n, "forward"_n, fc::raw::pack(child))}, "alice"_n);
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(pureeos_governance_tests)

BOOST_AUTO_TEST_CASE(profile_and_thresholds) {
   auto g = governance_genesis();
   BOOST_REQUIRE_NO_THROW(g.validate());
   const auto profile2_id = g.compute_chain_id();
   g.initial_savanna->version = 1;
   BOOST_CHECK(profile2_id != g.compute_chain_id());
   for (uint32_t n = 1; n <= 210; ++n) {
      const auto k = upgrade_veto_threshold(n);
      BOOST_CHECK_GE(3 * k, 2 * n);
      BOOST_CHECK_LT(3 * (k - 1), 2 * n);
   }
   BOOST_CHECK_EQUAL(upgrade_veto_threshold(210), 140u);
}

BOOST_AUTO_TEST_CASE(bootstrap_rejects_wrong_contract_without_locking_accounts) {
   governance_fixture f(1, false);
   f.chain.set_code("eosio"_n, "(module (func (export \"apply\") (param i64 i64 i64)))");
   BOOST_CHECK_THROW(f.seal(), fc::exception);
   BOOST_CHECK(f.state().developer == account_name{});
   BOOST_CHECK_EQUAL(f.chain.control->get_authorization_manager().get_permission({"eosio"_n, "active"_n}).auth.keys.size(), 1u);
   f.chain.produce_block();
   BOOST_REQUIRE_NO_THROW(f.chain.set_code("eosio"_n, fixture_wat().c_str()));
   BOOST_REQUIRE_NO_THROW(f.seal());
}

BOOST_AUTO_TEST_CASE(review_period_is_fixed_at_fourteen_days) {
   governance_fixture f(1, false);
   BOOST_CHECK_EQUAL(upgrade_review_seconds, 1209600u);
   for (uint32_t seconds : {0u, 1u, 20u, 604800u, 1209599u, 1209601u, 31536000u, 0xffffffffu}) {
      BOOST_CHECK_THROW(f.native(govinit{"developer"_n, seconds, {}}, "eosio"_n, "owner"_n),
                        action_validate_exception);
      BOOST_CHECK(f.state().developer == account_name{});
      BOOST_CHECK_EQUAL(f.state().review_seconds, 0u);
   }
   BOOST_CHECK_EQUAL(f.chain.control->get_authorization_manager().get_permission({"eosio"_n, "active"_n}).auth.keys.size(), 1u);
   BOOST_REQUIRE_NO_THROW(f.seal());
   BOOST_CHECK_EQUAL(f.state().review_seconds, 1209600u);
   f.propose();
   BOOST_CHECK(f.state().execute_after - f.state().proposed_at == fc::seconds(1209600));
}

BOOST_AUTO_TEST_CASE(bootstrap_seals_management_and_preserves_normal_accounts) {
   governance_fixture f;
   BOOST_CHECK(f.state().developer == "developer"_n);
   const auto& auth = f.chain.control->get_authorization_manager();
   for (auto account : {"eosio"_n, "eosio.token"_n, "eosio.prods"_n, "eosio.msig"_n, "eosio.wrap"_n}) {
      BOOST_CHECK(auth.get_permission({account, "owner"_n}).auth.keys.empty());
      BOOST_CHECK(auth.get_permission({account, "active"_n}).auth.keys.empty());
   }
   BOOST_CHECK(auth.get_permission({"eosio.prods"_n, "active"_n}).auth.accounts.empty());
   f.chain.produce_blocks(4);
   BOOST_CHECK(auth.get_permission({"eosio.prods"_n, "active"_n}).auth.accounts.empty());
   BOOST_CHECK_THROW(f.seal(), fc::exception);
   BOOST_CHECK_THROW(f.chain.set_code("eosio"_n, "(module)"), fc::exception);
   BOOST_REQUIRE_NO_THROW(f.chain.set_code("alice"_n, "(module (func (export \"apply\") (param i64 i64 i64)))"));
   BOOST_REQUIRE_NO_THROW(f.chain.set_authority("alice"_n, "custom"_n, authority(f.chain.get_public_key("alice"_n, "active")), "active"_n));
}

BOOST_AUTO_TEST_CASE(developer_only_and_no_stale_electorate) {
   governance_fixture f;
   BOOST_CHECK_THROW(f.native(f.proposal(), "alice"_n), fc::exception);
   BOOST_CHECK_THROW(f.native(f.proposal(), "developer"_n, "upgrade"_n), fc::exception);
   f.push({f.refresh(f.nodes)}, "developer"_n);
   BOOST_CHECK_THROW(f.native(f.proposal(), "developer"_n, "upgrade"_n), fc::exception);
   auto noop = action({permission_level{"developer"_n, "active"_n}}, "alice"_n, "noop"_n, bytes{});
   BOOST_CHECK_THROW(f.push({f.refresh(f.nodes), noop, action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal())}, "developer"_n), fc::exception);
   f.propose();
   BOOST_CHECK_EQUAL(f.state().proposal_voters.size(), 4u);
   BOOST_CHECK_THROW(f.propose(), fc::exception);
}

BOOST_AUTO_TEST_CASE(dedicated_upgrade_key_needs_no_active_key) {
   governance_fixture f(1, false);
   f.chain.set_authority("developer"_n, "upgrade"_n,
                         authority(f.chain.get_public_key("developer"_n, "upgrade")), "active"_n);
   f.seal();
   auto refresh = f.refresh(f.nodes);
   refresh.authorization[0].permission = "upgrade"_n;
   BOOST_REQUIRE_NO_THROW(f.push({refresh, action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal())}, "developer"_n, "upgrade"));
   f.wait_review();
   BOOST_REQUIRE_NO_THROW(f.native(devexec{f.state().proposal_id, f.upgrade_code, f.upgrade_abi}, "developer"_n, "upgrade"_n, "upgrade"));
}

BOOST_AUTO_TEST_CASE(electorate_validation_and_snapshot_immutability) {
   governance_fixture f;
   BOOST_CHECK_THROW(f.push({f.refresh({f.nodes[0], f.nodes[0]})}, "developer"_n), fc::exception);
   BOOST_CHECK_THROW(f.push({f.refresh({f.nodes[1], f.nodes[0]})}, "developer"_n), fc::exception);
   BOOST_CHECK_THROW(f.push({f.refresh({"developer"_n})}, "developer"_n), fc::exception);
   BOOST_CHECK_THROW(f.push({f.refresh({"missing"_n})}, "developer"_n), fc::exception);
   f.propose();
   f.push({f.refresh({f.nodes[0]})}, "developer"_n);
   BOOST_CHECK_EQUAL(f.state().current_voters.size(), 1u);
   BOOST_CHECK_EQUAL(f.state().proposal_voters.size(), 4u);
   BOOST_CHECK_THROW(f.veto("alice"_n), fc::exception);
   f.veto(f.nodes[3]);
   BOOST_CHECK_THROW(f.veto(f.nodes[3]), fc::exception);
}

BOOST_AUTO_TEST_CASE(full_review_then_atomic_code_and_abi_upgrade) {
   governance_fixture f;
   f.propose();
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   f.wait_review();
   auto wrong = f.upgrade_code;
   wrong.push_back(0);
   BOOST_CHECK_THROW(f.native(devexec{f.state().proposal_id, wrong, f.upgrade_abi}, "developer"_n, "upgrade"_n), fc::exception);
   BOOST_CHECK(f.state().pending);
   f.execute();
   BOOST_CHECK(!f.state().pending);
   BOOST_CHECK((f.chain.control->db().get<account_metadata_object, by_name>("eosio.token"_n).code_hash == f.state().new_code_hash));
   BOOST_CHECK_THROW(f.execute(), fc::exception);
}

BOOST_AUTO_TEST_CASE(no_objections_allows_execution_only_after_fourteen_days) {
   governance_fixture f;
   f.propose();
   const auto deadline = f.state().execute_after;
   BOOST_REQUIRE(deadline - f.state().proposed_at == fc::seconds(1209600));
   f.advance_to(deadline - fc::seconds(1));
   BOOST_CHECK(f.state().vetoes.empty());
   BOOST_CHECK_THROW(f.execute(), action_validate_exception);
   f.chain.produce_block(); // Last half-second before the deadline.
   BOOST_CHECK_THROW(f.execute(), action_validate_exception);
   f.chain.produce_block();
   BOOST_REQUIRE(f.chain.control->pending_block_time() == deadline);
   BOOST_CHECK(f.state().vetoes.empty());
   BOOST_REQUIRE_NO_THROW(f.execute());
   BOOST_CHECK(!f.state().pending);
}

BOOST_AUTO_TEST_CASE(one_two_three_four_nodes_veto_threshold) {
   for (size_t n : {1u, 2u, 3u, 4u}) {
      governance_fixture f(n);
      f.propose();
      const auto threshold = upgrade_veto_threshold(n);
      for (size_t i = 0; i < threshold; ++i) f.veto(f.nodes[i]);
      f.wait_review();
      BOOST_CHECK_THROW(f.execute(), fc::exception);
      BOOST_CHECK(f.state().pending);
   }
}

BOOST_AUTO_TEST_CASE(two_of_four_objections_do_not_block) {
   governance_fixture f;
   f.propose();
   f.veto(f.nodes[0]);
   f.veto(f.nodes[1]);
   f.wait_review();
   BOOST_CHECK_THROW(f.veto(f.nodes[2]), fc::exception);
   BOOST_REQUIRE_NO_THROW(f.execute());
}

BOOST_AUTO_TEST_CASE(exact_deadline_ends_veto_and_allows_execution) {
   governance_fixture f;
   f.propose();
   const auto deadline = f.state().execute_after;
   f.advance_to(deadline - fc::seconds(1));
   BOOST_REQUIRE_NO_THROW(f.veto(f.nodes[0])); // A late objection below the threshold still counts.
   f.chain.produce_blocks(2);
   BOOST_REQUIRE(f.chain.control->pending_block_time() == deadline);
   BOOST_CHECK_THROW(f.veto(f.nodes[0]), fc::exception);
   BOOST_REQUIRE_NO_THROW(f.execute());
}

BOOST_AUTO_TEST_CASE(abi_only_upgrade_requires_a_new_full_review) {
   governance_fixture f;
   f.propose();
   f.wait_review();
   f.execute();
   const auto code_hash = f.state().new_code_hash;
   auto abi = fc::raw::unpack<abi_def>(f.upgrade_abi);
   abi.ricardian_clauses.push_back({"reviewed", "ABI-only upgrade"});
   f.upgrade_abi = fc::raw::pack(abi);
   f.propose();
   BOOST_CHECK(f.state().old_code_hash == f.state().new_code_hash);
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   f.wait_review();
   BOOST_REQUIRE_NO_THROW(f.execute());
   BOOST_CHECK((f.chain.control->db().get<account_metadata_object, by_name>("eosio.token"_n).code_hash == code_hash));
}

BOOST_AUTO_TEST_CASE(genesis_producer_has_only_narrow_veto) {
   governance_fixture f(0);
   f.propose();
   BOOST_REQUIRE_NO_THROW(f.veto("eosio"_n));
   f.wait_review();
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   BOOST_CHECK_THROW(f.native(setcode{"eosio"_n, 0, 0, f.upgrade_code}, "eosio"_n, "veto"_n), fc::exception);
}

BOOST_AUTO_TEST_CASE(privileged_forwarding_cannot_forge_governance_or_management) {
   governance_fixture f;
   f.propose();
   BOOST_CHECK_THROW(f.forward(devveto{f.nodes[0], f.state().proposal_id}, f.nodes[0]), fc::exception);
   BOOST_CHECK_THROW(f.forward(devcancel{f.state().proposal_id}, "developer"_n, "upgrade"_n), fc::exception);
   BOOST_CHECK_THROW(f.forward(setcode{"eosio.token"_n, 0, 0, f.upgrade_code}, "eosio.token"_n), fc::exception);
   BOOST_CHECK_THROW(f.forward(setabi{"eosio.token"_n, f.upgrade_abi}, "eosio.token"_n), fc::exception);
   BOOST_CHECK_THROW(f.forward(updateauth{"eosio"_n, "active"_n, "owner"_n, authority(f.chain.get_public_key("alice"_n, "active"))}, "eosio"_n), fc::exception);
   BOOST_CHECK_THROW(f.forward(linkauth{"eosio"_n, "eosio"_n, "setcode"_n, "veto"_n}, "eosio"_n), fc::exception);
   BOOST_CHECK_THROW(f.push({action({permission_level{"alice"_n, "active"_n}}, "eosio"_n, "privilege"_n, bytes{})}, "alice"_n), fc::exception);
   for (auto old : {"eosio.msig"_n, "eosio.wrap"_n})
      BOOST_CHECK_THROW(f.push({action({permission_level{"alice"_n, "active"_n}}, old, "propose"_n, bytes{})}, "alice"_n), fc::exception);
}

BOOST_AUTO_TEST_CASE(cancel_and_resubmit_restarts_review) {
   governance_fixture f;
   f.propose();
   const auto id = f.state().proposal_id;
   const auto old_deadline = f.state().execute_after;
   f.veto(f.nodes[0]);
   BOOST_CHECK_THROW(f.native(devcancel{id}, "alice"_n), fc::exception);
   f.native(devcancel{id}, "developer"_n, "upgrade"_n);
   f.advance_to(old_deadline - fc::seconds(60));
   f.propose();
   BOOST_CHECK_EQUAL(f.state().proposal_id, id + 1);
   BOOST_CHECK(f.state().execute_after - f.state().proposed_at == fc::seconds(1209600));
   BOOST_CHECK(f.state().execute_after > old_deadline);
   BOOST_CHECK(f.state().vetoes.empty());
   BOOST_CHECK_THROW(f.native(devveto{f.nodes[0], id}, f.nodes[0]), fc::exception);
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   f.advance_to(old_deadline);
   BOOST_CHECK_THROW(f.execute(), action_validate_exception);
   f.wait_review();
   BOOST_REQUIRE_NO_THROW(f.execute());
}

BOOST_AUTO_TEST_CASE(execution_failure_rolls_back_proposal) {
   governance_fixture f;
   f.upgrade_code = {'b', 'a', 'd', 'w', 'a', 's', 'm'};
   f.propose();
   f.wait_review();
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   BOOST_CHECK(f.state().pending);
   BOOST_CHECK((f.chain.control->db().get<account_metadata_object, by_name>("eosio.token"_n).code_hash == digest_type{}));
}

BOOST_AUTO_TEST_CASE(transaction_retry_discards_single_use_upgrade_capability) {
   governance_fixture f;
   f.propose();
   f.wait_review();
   // Exercise the real OC-interrupt retry path without requiring an OC build:
   // interrupt once after the native ABI child has consumed its capability.
   auto* handler = const_cast<apply_handler*>(f.chain.control->find_apply_handler("eosio"_n, "eosio"_n, "setabi"_n));
   BOOST_REQUIRE(handler != nullptr);
   auto original = *handler;
   auto restore = fc::make_scoped_exit([&] { *handler = original; });
   bool interrupted = false;
   *handler = [&](apply_context& ctx) {
      original(ctx);
      if (!interrupted) {
         interrupted = true;
         EOS_THROW(interrupt_oc_exception, "test-only upgrade execution interruption");
      }
   };
   BOOST_REQUIRE_NO_THROW(f.execute());
   BOOST_CHECK(interrupted);
   BOOST_CHECK(!f.state().pending);
}

BOOST_AUTO_TEST_CASE(two_hundred_ten_nodes_require_140_not_141) {
   governance_fixture f(210);
   f.propose();
   for (size_t i = 0; i < 139; ++i) f.veto(f.nodes[i]);
   BOOST_CHECK_EQUAL(f.state().vetoes.size(), 139u);
   f.veto(f.nodes[139]);
   f.wait_review();
   BOOST_CHECK_THROW(f.execute(), fc::exception);
}

BOOST_AUTO_TEST_CASE(snapshot_binary_json_and_restart_preserve_frozen_proposal) {
   governance_fixture f;
   f.propose();
   f.veto(f.nodes[0]);
   f.chain.produce_blocks(4);
   f.chain.control->abort_block();
   const auto expected = fc::raw::pack(f.state());
   // Exercise the shared_vector JSON conversion as well as binary snapshots.
   const auto json = fc::variant(f.state());
   upgrade_governance_object copy([](auto&) {}, chainbase::constructor_tag{});
   fc::from_variant(json, copy);
   BOOST_CHECK(fc::raw::pack(copy) == expected);
   auto writer = buffered_snapshot_suite::get_writer();
   f.chain.control->write_snapshot(writer);
   auto snapshot = buffered_snapshot_suite::finalize(writer);
   fc::temp_directory restored_dir;
   const auto cfg = base_tester::default_config(restored_dir).first;
   tester restored(cfg, f.genesis, call_startup_t::no);
   restored.close();
   restored.init(cfg, buffered_snapshot_suite::get_reader(snapshot));
   BOOST_CHECK(fc::raw::pack(*find_upgrade_governance(restored.control->db())) == expected);
   f.chain.close();
   f.chain.open();
   BOOST_CHECK(fc::raw::pack(f.state()) == expected);
}

BOOST_AUTO_TEST_CASE(legacy_v9_snapshot_does_not_enable_governance) {
   const auto snapshot = buffered_snapshot_suite::load_from_file("snap_v9");
   fc::temp_directory dir;
   const auto cfg = base_tester::default_config(dir).first;
   tester restored(cfg, base_tester::default_genesis(), call_startup_t::no);
   restored.close();
   restored.init(cfg, buffered_snapshot_suite::get_reader(snapshot));
   BOOST_CHECK(find_upgrade_governance(restored.control->db()) == nullptr);
}

BOOST_AUTO_TEST_CASE(sealed_governance_replays_from_genesis) {
   governance_fixture f;
   f.propose();
   f.veto(f.nodes[0]);
   f.chain.produce_blocks(8);
   const auto expected = fc::raw::pack(f.state());
   const auto lib_id = f.chain.last_irreversible_block_id();
   const auto source_cfg = base_tester::default_config(f.dir).first;
   f.chain.close();
   fc::temp_directory replay_dir;
   const auto cfg = base_tester::default_config(replay_dir).first;
   std::filesystem::create_directories(cfg.blocks_dir);
   for (const char* filename : {"blocks.log", "blocks.index"})
      std::filesystem::copy_file(source_cfg.blocks_dir / filename, cfg.blocks_dir / filename);
   tester replay(cfg, f.genesis);
   BOOST_CHECK(replay.head().id() == lib_id);
   BOOST_CHECK(fc::raw::pack(*find_upgrade_governance(replay.control->db())) == expected);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef PUREEOS_GOVERNANCE_CONTRACTS_DIR
namespace {
struct production_governance_fixture : governance_fixture {
   std::filesystem::path contract_dir;
   production_governance_fixture(std::filesystem::path root = PUREEOS_GOVERNANCE_CONTRACTS_DIR,
                                 bool seal_now = true, bool with_extra_voter = false, bool large_supply = false,
                                 bool resource_split = false, size_t node_count = 4, bool rotation = false,
                                 bool reward_headroom = false, std::string initial_supply = {})
      : governance_fixture(node_count, false), contract_dir(std::move(root)) {
      if (rotation) {
         // Activate the versioned host API through a real privileged contract
         // action. Existing genesis manifests and legacy profiles stay unchanged.
         deploy("eosio"_n, "eosio.bios");
         chain.activate_builtin_protocol_features({builtin_protocol_feature_t::pureeos_rotation_context});
         chain.produce_blocks(2);
      }
      chain.create_accounts({"eosio.stake"_n, "eosio.rex"_n, "eosio.ram"_n, "eosio.ramfee"_n,
                             "eosio.bpay"_n, "eosio.vpay"_n, "eosio.saving"_n, "eosio.fees"_n});
      // Bootstrap test users before installing system newaccount RAM billing.
      if (resource_split) chain.create_account("eosio.reserv"_n);
      if (with_extra_voter) chain.create_account("b1"_n);
      deploy("eosio.token"_n, "eosio.token");
      const std::string supply = initial_supply.empty()
         ? (large_supply ? "10000000000000.0000 SYS" : "1000000000.0000 SYS") : initial_supply;
      const std::string maximum = reward_headroom ? "2000000000.0000 SYS" : supply;
      BOOST_REQUIRE(!reward_headroom || !large_supply);
      chain.push_action("eosio.token"_n, "create"_n, "eosio.token"_n,
                        fc::mutable_variant_object()("issuer", "eosio")("maximum_supply", maximum));
      chain.push_action("eosio.token"_n, "issue"_n, "eosio"_n,
                        fc::mutable_variant_object()("to", "eosio")("quantity", supply)("memo", "test"));
      chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n,
                        fc::mutable_variant_object()("from", "eosio")("to", "alice")("quantity", "1000.0000 SYS")("memo", "test"));
      deploy("eosio"_n, "eosio.system");
      chain.push_action("eosio"_n, "init"_n, "eosio"_n, fc::mutable_variant_object()("version", 0)("core", "4,SYS"));
      if (seal_now) seal();
   }
   void deploy(account_name account, const std::string& contract) {
      const auto base = contract_dir / contract / contract;
      chain.set_code(account, read_wasm((base.string() + ".wasm").c_str()));
      std::string json;
      fc::read_file_contents(base.string() + ".abi", json);
      chain.set_abi(account, json);
   }
   action production_refresh() {
      return action({permission_level{"developer"_n, "active"_n}}, "eosio"_n, "refreshgov"_n, bytes{});
   }
   void register_node(account_name node, bool with_bls = true) {
      chain.push_action("eosio"_n, "regproducer"_n, node, fc::mutable_variant_object()
         ("producer", node)("producer_key", chain.get_public_key(node, "active"))("url", "")("location", 0));
      if (with_bls) {
         auto [priv, pub, pop] = get_bls_key(node);
         chain.push_action("eosio"_n, "regfinkey"_n, node, fc::mutable_variant_object()
            ("finalizer_name", node)("finalizer_key", pub.to_string())("proof_of_possession", pop.to_string()));
      }
   }
   void fund_voter() {
      chain.push_action("eosio"_n, "buyram"_n, "alice"_n, fc::mutable_variant_object()
         ("payer", "alice")("receiver", "alice")("quant", "10.0000 SYS"));
      chain.push_action("eosio"_n, "delegatebw"_n, "alice"_n, fc::mutable_variant_object()
         ("from", "alice")("receiver", "alice")("stake_net_quantity", "100.0000 SYS")
         ("stake_cpu_quantity", "100.0000 SYS")("transfer", false));
   }
   void populate_sparse_governance_candidates(bool first_already_registered = false) {
      for (size_t i = 0; i < nodes.size(); ++i) {
         const auto n = nodes[i];
         chain.push_action("eosio"_n, "buyram"_n, "eosio"_n, fc::mutable_variant_object()
            ("payer", "eosio")("receiver", n)("quant", "100.0000 SYS"));
         chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
            ("from", "eosio")("to", n)("quantity", "100.0000 SYS")("memo", "bounded governance scan"));
         if (i || !first_already_registered) register_node(n, i < 209 || i + 1 == nodes.size());
         chain.push_action("eosio"_n, "delegatebw"_n, n, fc::mutable_variant_object()
            ("from", n)("receiver", n)("stake_net_quantity", "10.0000 SYS")
            ("stake_cpu_quantity", "10.0000 SYS")("transfer", false));
         chain.push_action("eosio"_n, "voteproducer"_n, n, fc::mutable_variant_object()
            ("voter", n)("proxy", "")("producers", std::vector<name>{n}));
         if (i % 4 == 3) chain.produce_block();
      }
   }
};
}

BOOST_AUTO_TEST_SUITE(pureeos_governance_contract_tests)
BOOST_AUTO_TEST_CASE(production_bootstrap_and_electorate_ranking) {
   production_governance_fixture f;
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_REQUIRE_EQUAL(f.state().current_voters.size(), 1u);
   BOOST_CHECK(f.state().current_voters[0] == "eosio"_n);
   for (size_t i = 0; i < f.nodes.size(); ++i) f.register_node(f.nodes[i], i != 3);
   f.fund_voter();
   f.chain.push_action("eosio"_n, "voteproducer"_n, "alice"_n,
                       fc::mutable_variant_object()("voter", "alice")("proxy", "")("producers", f.nodes));
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal())}, "developer"_n);
   BOOST_REQUIRE_EQUAL(f.state().proposal_voters.size(), 3u); // fourth lacks BLS
   for (size_t i = 0; i < 3; ++i) BOOST_CHECK(f.state().proposal_voters[i] == f.nodes[i]);
   f.chain.push_action("eosio"_n, "unregprod"_n, f.nodes[0], fc::mutable_variant_object()("producer", f.nodes[0]));
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_CHECK_EQUAL(f.state().current_voters.size(), 2u);
   BOOST_CHECK_EQUAL(f.state().proposal_voters.size(), 3u);
   f.veto(f.nodes[0]); // proposal snapshot survives unregistration
   f.veto(f.nodes[1]);
   f.wait_review();
   BOOST_CHECK_THROW(f.execute(), fc::exception);
   BOOST_CHECK_THROW(f.chain.push_action("eosio"_n, "rmvproducer"_n, f.nodes[2],
      fc::mutable_variant_object()("producer", f.nodes[1])), fc::exception);
}

BOOST_AUTO_TEST_CASE(production_system_self_upgrade_and_resource_transfers_survive) {
   production_governance_fixture f;
   f.fund_voter(); // real RAM fee and staking transfers after permissions seal
   const auto base = std::filesystem::path(PUREEOS_GOVERNANCE_CONTRACTS_DIR) / "eosio.system" / "eosio.system.wasm";
   const auto wasm = read_wasm(base.c_str());
   f.upgrade_code.assign(wasm.begin(), wasm.end());
   // Append a valid custom WASM section; semantics unchanged but reviewed hash changes.
   f.upgrade_code.insert(f.upgrade_code.end(), {0, 2, 1, 'x'});
   const auto& abi = f.chain.control->db().get<account_object, by_name>("eosio"_n).abi;
   f.upgrade_abi.assign(abi.data(), abi.data() + abi.size());
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal("eosio"_n))}, "developer"_n);
   f.wait_review();
   BOOST_REQUIRE_NO_THROW(f.execute());
   BOOST_CHECK((f.chain.control->db().get<account_metadata_object, by_name>("eosio"_n).code_hash == f.state().new_code_hash));
   f.push({f.production_refresh()}, "developer"_n);
   f.chain.produce_blocks(6);
   BOOST_CHECK(!f.state().pending);
}
BOOST_AUTO_TEST_SUITE_END()
#ifdef PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR
namespace {
struct single_choice_fixture : production_governance_fixture {
   explicit single_choice_fixture(bool with_loan_liquidity = false, bool large_supply = false)
      : production_governance_fixture(PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR, false, true, large_supply) {
      deploy("eosio.bpay"_n, "eosio.bpay");
      chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n,
         fc::mutable_variant_object()("from", "eosio")("to", "b1")("quantity", "100.0000 SYS")("memo", "test"));
      if (with_loan_liquidity) {
         chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n,
            fc::mutable_variant_object()("from", "eosio")("to", "alice")("quantity", "50000.0000 SYS")("memo", "test liquidity"));
      }
      if (large_supply) {
         for (auto user : {"alice"_n, "b1"_n})
            chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
               ("from", "eosio")("to", user)("quantity", "950000000000.0000 SYS")("memo", "precision test"));
      }
      seal();
      for (auto node : nodes) register_node(node);
      const auto ram = large_supply ? "1000000.0000 SYS" : "10.0000 SYS";
      call("buyram"_n, fc::mutable_variant_object()("payer", "alice")("receiver", "alice")("quant", ram));
      call("buyram"_n, fc::mutable_variant_object()("payer", "b1")("receiver", "b1")("quant", ram), "b1"_n);
   }
   transaction_trace_ptr call(action_name act, const fc::variant_object& args, account_name signer = "alice"_n) {
      return push({chain.get_action("eosio"_n, act, {{signer, "active"_n}}, args)}, signer);
   }
   transaction_trace_ptr vote(const std::vector<account_name>& selected, account_name voter = "alice"_n,
                              account_name proxy = {}) {
      return call("voteproducer"_n, fc::mutable_variant_object()
         ("voter", voter)("proxy", proxy)("producers", selected), voter);
   }
   static name versioned(name table) {
      if (table == "voters"_n) return "votersv2"_n;
      if (table == "producers"_n) return "producersv2"_n;
      return table;
   }
   auto raw(name table, account_name account = "alice"_n) const {
      table = versioned(table);
      return chain.get_row_by_account("eosio"_n, "eosio"_n, table, account);
   }
   fc::variant row(name table, account_name account = "alice"_n) const {
      table = versioned(table);
      const auto data = raw(table, account);
      BOOST_REQUIRE(!data.empty());
      abi_def abi;
      BOOST_REQUIRE(abi_serializer::to_abi(chain.control->db().get<account_object, by_name>("eosio"_n).abi, abi));
      const auto yield = abi_serializer::create_yield_function(fc::seconds(5));
      abi_serializer serializer(abi, yield);
      return serializer.binary_to_variant(serializer.get_table_type(table), data, yield);
   }
   void deposit(const std::string& quantity) {
      call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", quantity));
   }
   void buy(const std::string& quantity = "100.0000 SYS") {
      call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", quantity));
   }
   void mature() {
      chain.produce_block();
      chain.produce_block(fc::days(6));
   }
   static bool message(const fc::exception& e, const std::string& text) {
      return e.to_detail_string().find(text) != std::string::npos;
   }
   auto producers(uint32_t limit = 50, const std::string& lower = "", bool json = true) const {
      std::optional<eosio::chain_apis::tracked_votes> tracked;
      return eosio::chain_apis::read_only(*chain.control, {}, {}, tracked, fc::seconds(5), fc::seconds(5), nullptr)
         .get_producers({.json = json, .lower_bound = lower, .limit = limit}, fc::time_point::maximum());
   }
   auto account(account_name owner) const {
      std::optional<eosio::chain_apis::tracked_votes> tracked;
      auto result = eosio::chain_apis::read_only(*chain.control, {}, {}, tracked, fc::seconds(5), fc::seconds(5), nullptr)
         .get_account({.account_name = owner}, fc::time_point::maximum())();
      return std::get<eosio::chain_apis::read_only::get_account_results>(result);
   }
};
}

BOOST_AUTO_TEST_SUITE(pureeos_single_choice_contract_tests)
BOOST_AUTO_TEST_CASE(governance_scan_bounds_visited_rows_and_never_truncates_electorate) {
   production_governance_fixture f(PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR, false, false, false, false, 1025);
   f.populate_sparse_governance_candidates();
   f.chain.push_action("eosio"_n, "unregprod"_n, f.nodes[209],
      fc::mutable_variant_object()("producer", f.nodes[209]));
   // Positive votes now activate immediately, but must not change the native
   // one-producer bootstrap policy before governance can be sealed.
   BOOST_REQUIRE_EQUAL(f.chain.control->head_active_producers().producers.size(), 1u);
   BOOST_REQUIRE(f.chain.control->head_active_producers().producers[0].producer_name == "eosio"_n);
   f.seal();
   const auto before = fc::raw::pack(f.state().current_voters);
   f.register_node(f.nodes[209], false);
   const auto propose = [&] {
      return f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal())},
                    "developer"_n);
   };
   BOOST_CHECK_EXCEPTION(propose(), fc::exception, [](const auto& e) {
      return e.to_detail_string().find("governance candidate scan limit exceeded") != std::string::npos;
   });
   BOOST_REQUIRE(!f.state().pending); // never publish the first 209 with a smaller threshold
   BOOST_CHECK(fc::raw::pack(f.state().current_voters) == before); // complete transaction rollback
   f.chain.produce_block(); // give the second identical unregister action a distinct transaction header
   f.chain.push_action("eosio"_n, "unregprod"_n, f.nodes[209],
      fc::mutable_variant_object()("producer", f.nodes[209]));
   BOOST_REQUIRE_NO_THROW(propose()); // exactly 1024 visited rows now contain all 210 eligible nodes
   BOOST_REQUIRE_EQUAL(f.state().proposal_voters.size(), 210u);
   BOOST_CHECK(std::find(f.state().proposal_voters.begin(), f.state().proposal_voters.end(), f.nodes.back()) !=
               f.state().proposal_voters.end());
}

BOOST_AUTO_TEST_CASE(zero_stake_can_select_one_producer_without_creating_votes) {
   single_choice_fixture f;
   BOOST_CHECK(f.raw("voters"_n).empty());
   BOOST_REQUIRE_NO_THROW(f.vote({f.nodes[0]}));
   const auto voter = f.row("voters"_n);
   BOOST_CHECK_EQUAL(voter["staked"].as_int64(), 0);
   BOOST_CHECK_EQUAL(voter["last_vote_amount"].as_int64(), 0);
   BOOST_CHECK_EQUAL(voter["producers"].get_array().size(), 1u);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_double(), 0);
   BOOST_REQUIRE_NO_THROW(f.vote({}));
   BOOST_CHECK(f.row("voters"_n)["producers"].get_array().empty());
}

BOOST_AUTO_TEST_CASE(multi_choice_and_proxy_actions_reject_and_roll_back) {
   single_choice_fixture f;
   BOOST_CHECK_EXCEPTION(f.vote({f.nodes[0], f.nodes[1]}), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "select at most one producer"); });
   BOOST_CHECK(f.raw("voters"_n).empty());
   BOOST_CHECK_EXCEPTION(f.vote({}, "alice"_n, f.nodes[0]), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "proxy voting is disabled"); });
   for (bool flag : {true, false}) {
      BOOST_CHECK_EXCEPTION(f.call("regproxy"_n, fc::mutable_variant_object()("proxy", "alice")("isproxy", flag)),
         fc::exception, [](const auto& e) { return single_choice_fixture::message(e, "proxy voting is disabled"); });
   }
   BOOST_CHECK(f.raw("voters"_n).empty());
   BOOST_REQUIRE_NO_THROW(f.vote({f.nodes[0]}));
   const auto before = f.raw("voters"_n);
   BOOST_CHECK_THROW(f.vote({f.nodes[0], f.nodes[0]}), fc::exception);
   BOOST_CHECK(f.raw("voters"_n) == before);
}

BOOST_AUTO_TEST_CASE(owner_authorization_includes_b1_no_special_system_vote) {
   single_choice_fixture f;
   BOOST_CHECK_THROW(f.call("voteproducer"_n, fc::mutable_variant_object()
      ("voter", "alice")("proxy", "")("producers", std::vector{f.nodes[0]}), f.nodes[1]), fc::exception);
   BOOST_CHECK(f.raw("voters"_n).empty());
   BOOST_REQUIRE_NO_THROW(f.vote({f.nodes[0]}, "b1"_n));
   BOOST_CHECK_EQUAL(f.row("voters"_n, "b1"_n)["producers"].get_array().size(), 1u);
}

BOOST_AUTO_TEST_CASE(rex_deposit_is_free_of_voting_but_buy_requires_choice) {
   single_choice_fixture f;
   BOOST_REQUIRE_NO_THROW(f.deposit("150.0000 SYS"));
   const auto fund = f.raw("rexfund"_n);
   BOOST_CHECK_EXCEPTION(f.buy(), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "select one producer before buying REX"); });
   BOOST_CHECK(f.raw("rexfund"_n) == fund);
   BOOST_CHECK(f.raw("rexbal"_n).empty());
   BOOST_REQUIRE_NO_THROW(f.vote({f.nodes[0]}));
   BOOST_REQUIRE_NO_THROW(f.buy());
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 1000000);
   BOOST_CHECK(f.row("producers"_n, f.nodes[0])["total_votes"].as_double() > 0);
}

BOOST_AUTO_TEST_CASE(rex_holding_requires_choice_but_allows_atomic_switch) {
   single_choice_fixture f;
   f.vote({f.nodes[0]}); f.deposit("150.0000 SYS"); f.buy();
   const auto before = f.raw("voters"_n);
   BOOST_CHECK_EXCEPTION(f.vote({}), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "cannot clear vote while holding REX"); });
   BOOST_CHECK(f.raw("voters"_n) == before);
   BOOST_CHECK_THROW(f.vote({"missing"_n}), fc::exception);
   BOOST_CHECK(f.raw("voters"_n) == before);
   BOOST_REQUIRE_NO_THROW(f.vote({f.nodes[1]}));
   BOOST_CHECK_SMALL(f.row("producers"_n, f.nodes[0])["total_votes"].as_double(), 0.01);
   BOOST_CHECK(f.row("producers"_n, f.nodes[1])["total_votes"].as_double() > 0);
   f.call("mvtosavings"_n, fc::mutable_variant_object()("owner", "alice")("rex", f.row("rexbal"_n)["rex_balance"]));
   BOOST_CHECK_EXCEPTION(f.vote({}), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "cannot clear vote while holding REX"); });
}

BOOST_AUTO_TEST_CASE(partial_sale_still_requires_vote_full_sale_allows_clear) {
   single_choice_fixture f;
   f.vote({f.nodes[0]}); f.deposit("100.0000 SYS"); f.buy(); f.mature();
   const auto all = f.row("rexbal"_n)["rex_balance"].as<asset>();
   f.call("sellrex"_n, fc::mutable_variant_object()("from", "alice")("rex", asset(all.get_amount()/2, all.get_symbol())));
   BOOST_CHECK_EXCEPTION(f.vote({}), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "cannot clear vote while holding REX"); });
   f.call("sellrex"_n, fc::mutable_variant_object()("from", "alice")("rex", f.row("rexbal"_n)["rex_balance"]));
   BOOST_CHECK_EQUAL(f.row("rexbal"_n)["rex_balance"].as<asset>().get_amount(), 0);
   BOOST_REQUIRE_NO_THROW(f.vote({}));
   BOOST_CHECK(f.row("voters"_n)["producers"].get_array().empty());
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 0);
}

BOOST_AUTO_TEST_CASE(inactive_target_blocks_new_buy_but_not_refresh_or_exit) {
   single_choice_fixture f;
   f.vote({f.nodes[0]}); f.deposit("150.0000 SYS"); f.buy();
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", f.nodes[0]), f.nodes[0]);
   const auto fund = f.raw("rexfund"_n);
   BOOST_CHECK_EXCEPTION(f.buy("1.0000 SYS"), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "vote target must be an active registered producer"); });
   BOOST_CHECK(f.raw("rexfund"_n) == fund);
   BOOST_REQUIRE_NO_THROW(f.call("voteupdate"_n, fc::mutable_variant_object()("voter_name", "alice")));
   f.mature();
   BOOST_REQUIRE_NO_THROW(f.call("sellrex"_n, fc::mutable_variant_object()("from", "alice")("rex", f.row("rexbal"_n)["rex_balance"])));
   BOOST_REQUIRE_NO_THROW(f.vote({}));
   BOOST_REQUIRE_NO_THROW(f.call("withdraw"_n, fc::mutable_variant_object()("owner", "alice")("amount", "150.0000 SYS")));
}

BOOST_AUTO_TEST_CASE(queued_sale_blocks_clear_and_closed_sale_settles_once_before_switch) {
   single_choice_fixture f(true);
   f.vote({f.nodes[0]}); f.deposit("50000.0000 SYS"); f.buy("50000.0000 SYS"); f.mature();
   // A pure renter needs no voting choice. A live loan prevents immediate full redemption.
   f.call("deposit"_n, fc::mutable_variant_object()("owner", "b1")("amount", "1.0000 SYS"), "b1"_n);
   f.call("rentcpu"_n, fc::mutable_variant_object()("from", "b1")("receiver", "b1")
      ("loan_payment", "1.0000 SYS")("loan_fund", "0.0000 SYS"), "b1"_n);
   BOOST_CHECK(f.raw("voters"_n, "b1"_n).empty());
   const auto shares = f.row("rexbal"_n)["rex_balance"];
   f.call("sellrex"_n, fc::mutable_variant_object()("from", "alice")("rex", shares));
   BOOST_CHECK(f.row("rexqueue"_n)["is_open"].as_bool());
   BOOST_CHECK_EQUAL(f.row("rexbal"_n)["rex_balance"].as<asset>().get_amount(), shares.as<asset>().get_amount());
   BOOST_CHECK_EXCEPTION(f.vote({}), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "cannot clear vote while holding REX"); });
   // A third party closes the loan: v2 consumes the voting delta immediately.
   f.chain.produce_block();
   f.chain.produce_block(fc::days(31));
   // The maintenance payer must not lose its entire CPU allowance with this loan.
   f.call("rexexec"_n, fc::mutable_variant_object()("user", f.nodes[3])("max", 10), f.nodes[3]);
   BOOST_CHECK(!f.row("rexqueue"_n)["is_open"].as_bool());
   BOOST_CHECK_EQUAL(f.row("rexbal"_n)["rex_balance"].as<asset>().get_amount(), 0);
   BOOST_CHECK_EQUAL(f.row("rexqueue"_n)["stake_change"].as<asset>().get_amount(), 0);
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 0);
   BOOST_REQUIRE_NO_THROW(f.vote({f.nodes[1]}));
   BOOST_CHECK(f.raw("rexqueue"_n).empty());
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 0);
   BOOST_CHECK_SMALL(f.row("producers"_n, f.nodes[0])["total_votes"].as_double(), 0.01);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[1])["total_votes"].as_double(), 0);
   const auto fund = f.raw("rexfund"_n);
   BOOST_REQUIRE_NO_THROW(f.vote({}));
   BOOST_REQUIRE_NO_THROW(f.call("voteupdate"_n, fc::mutable_variant_object()("voter_name", "alice")));
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 0);
   BOOST_CHECK(f.raw("rexfund"_n) == fund); // no second proceeds transfer or stake deduction
}

BOOST_AUTO_TEST_CASE(unstaketorex_checks_owner_not_resource_receiver) {
   single_choice_fixture f;
   f.call("buyram"_n, fc::mutable_variant_object()("payer", "alice")("receiver", f.nodes[1])("quant", "10.0000 SYS"));
   f.call("delegatebw"_n, fc::mutable_variant_object()("from", "alice")("receiver", f.nodes[1])
      ("stake_net_quantity", "10.0000 SYS")("stake_cpu_quantity", "10.0000 SYS")("transfer", false));
   f.vote({f.nodes[0]}, f.nodes[1]); // Receiver's vote must NOT authorize owner's conversion.
   const auto before = f.raw("voters"_n);
   auto convert = [&] { return f.call("unstaketorex"_n, fc::mutable_variant_object()("owner", "alice")
      ("receiver", f.nodes[1])("from_net", "2.0000 SYS")("from_cpu", "3.0000 SYS")); };
   BOOST_CHECK_EXCEPTION(convert(), fc::exception,
      [](const auto& e) { return single_choice_fixture::message(e, "select one producer before buying REX"); });
   BOOST_CHECK(f.raw("voters"_n) == before);
   f.vote({f.nodes[0]});
   BOOST_REQUIRE_NO_THROW(convert());
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 200000);
   BOOST_CHECK_EQUAL(f.row("voters"_n, f.nodes[1])["staked"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.row("rexbal"_n)["vote_stake"].as<asset>().get_amount(), 50000);
}

BOOST_AUTO_TEST_CASE(cpu_net_only_voter_can_refresh_and_clear) {
   single_choice_fixture f;
   f.fund_voter(); f.vote({f.nodes[0]});
   BOOST_REQUIRE_NO_THROW(f.call("voteupdate"_n, fc::mutable_variant_object()("voter_name", "alice")));
   BOOST_REQUIRE_NO_THROW(f.vote({}));
   BOOST_CHECK(f.row("voters"_n)["producers"].get_array().empty());
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 2000000);
}

BOOST_AUTO_TEST_CASE(uninitialized_upgrade_cannot_silently_enable_new_policy) {
   production_governance_fixture f(PUREEOS_GOVERNANCE_CONTRACTS_DIR, false);
   f.contract_dir = PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR;
   f.deploy("eosio"_n, "eosio.system");
   BOOST_CHECK_EXCEPTION(f.chain.push_action("eosio"_n, "voteproducer"_n, "alice"_n,
      fc::mutable_variant_object()("voter", "alice")("proxy", "")("producers", std::vector<account_name>{})),
      fc::exception, [](const auto& e) { return single_choice_fixture::message(e, "requires fresh-chain initialization"); });
   BOOST_CHECK_THROW(f.chain.push_action("eosio"_n, "init"_n, "eosio"_n,
      fc::mutable_variant_object()("version", 0)("core", "4,SYS")), fc::exception);
}

BOOST_AUTO_TEST_CASE(integer_votes_do_not_grow_with_time_or_repeated_refresh) {
   single_choice_fixture f;
   f.fund_voter(); f.vote({f.nodes[0]});
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 2000000);
   BOOST_CHECK_EQUAL(f.row("voters"_n)["last_vote_amount"].as_int64(), 2000000);
   f.chain.produce_block(); f.chain.produce_block(fc::days(365));
   for (int i = 0; i < 3; ++i) {
      f.vote({f.nodes[0]});
      f.call("voteupdate"_n, fc::mutable_variant_object()("voter_name", "alice"));
   }
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 2000000);
   BOOST_CHECK_EQUAL(f.row("globalv2"_n, "globalv2"_n)["total_producer_vote_weight"].as_int64(), 2000000);
   f.vote({f.nodes[1]});
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[1])["total_votes"].as_int64(), 2000000);
   f.vote({});
   BOOST_CHECK_EQUAL(f.row("voters"_n)["last_vote_amount"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.row("globalv2"_n, "globalv2"_n)["total_producer_vote_weight"].as_int64(), 0);
}

BOOST_AUTO_TEST_CASE(integer_rpc_preserves_above_double_precision_and_pagination) {
   single_choice_fixture f(false, true);
   auto stake = [&](account_name user, const std::string& cpu) {
      f.call("delegatebw"_n, fc::mutable_variant_object()("from", user)("receiver", user)
         ("stake_net_quantity", "1.0000 SYS")("stake_cpu_quantity", cpu)("transfer", false), user);
   };
   stake("alice"_n, "900719925473.0992 SYS");
   stake("b1"_n, "900719925473.0993 SYS");
   f.vote({f.nodes[0]}); f.vote({f.nodes[1]}, "b1"_n);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 9007199254740992LL);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[1])["total_votes"].as_int64(), 9007199254740993LL);
   const auto first = f.producers(1);
   BOOST_CHECK_EQUAL(first.vote_schema, "integer-v2");
   BOOST_REQUIRE(first.vote_precision && first.total_producer_vote_amount);
   BOOST_CHECK_EQUAL(*first.vote_precision, 4u);
   BOOST_CHECK_EQUAL(*first.total_producer_vote_amount, "18014398509481985");
   BOOST_REQUIRE_EQUAL(first.rows.size(), 1u);
   BOOST_CHECK_EQUAL(first.rows[0]["owner"].as_string(), f.nodes[1].to_string());
   BOOST_CHECK_EQUAL(first.rows[0]["total_votes"].as_string(), "9007199254740993");
   const auto voter_account = f.account("b1"_n);
   BOOST_CHECK(voter_account.voter_info["staked"].is_string());
   BOOST_CHECK_EQUAL(voter_account.voter_info["staked"].as_string(), "9007199254740993");
   BOOST_CHECK_EQUAL(voter_account.voter_info["last_vote_amount"].as_string(), "9007199254740993");
   BOOST_CHECK_EQUAL(first.more, f.nodes[0].to_string());
   const auto second = f.producers(1, first.more);
   BOOST_REQUIRE_EQUAL(second.rows.size(), 1u);
   BOOST_CHECK_EQUAL(second.rows[0]["owner"].as_string(), f.nodes[0].to_string());
   BOOST_CHECK_EQUAL(second.rows[0]["total_votes"].as_string(), "9007199254740992");
   BOOST_CHECK(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "producers"_n, f.nodes[0]).empty());
   BOOST_CHECK_EQUAL(f.producers(0).more, f.nodes[1].to_string());
   BOOST_REQUIRE_EQUAL(f.producers(1, "", false).rows.size(), 1u);
   f.call("delegatebw"_n, fc::mutable_variant_object()("from", "alice")("receiver", "alice")
      ("stake_net_quantity", "0.0001 SYS")("stake_cpu_quantity", "0.0000 SYS")("transfer", false));
   f.vote({f.nodes[0]}); // repeating a selection must not add the principal again
   const auto tied = f.producers(2);
   BOOST_REQUIRE_EQUAL(tied.rows.size(), 2u);
   BOOST_CHECK_EQUAL(tied.rows[0]["owner"].as_string(), f.nodes[0].to_string());
   BOOST_CHECK_EQUAL(tied.rows[1]["owner"].as_string(), f.nodes[1].to_string());
   BOOST_CHECK_EQUAL(*tied.total_producer_vote_amount, "18014398509481986");
}

BOOST_AUTO_TEST_CASE(integer_governance_ranking_keeps_frozen_snapshot) {
   single_choice_fixture f;
   f.vote({f.nodes[1]}); f.deposit("100.0000 SYS"); f.buy();
   f.vote({f.nodes[0]}, "b1"_n);
   f.call("deposit"_n, fc::mutable_variant_object()("owner", "b1")("amount", "90.0000 SYS"), "b1"_n);
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "b1")("amount", "90.0000 SYS"), "b1"_n);
   f.push({f.production_refresh(), action({permission_level{"developer"_n, "upgrade"_n}}, f.proposal())}, "developer"_n);
   BOOST_REQUIRE_EQUAL(f.state().proposal_voters.size(), 2u);
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", f.nodes[1]), f.nodes[1]);
   f.push({f.production_refresh()}, "developer"_n);
   BOOST_REQUIRE_EQUAL(f.state().current_voters.size(), 1u);
   BOOST_CHECK_EQUAL(f.state().current_voters[0], f.nodes[0]);
   BOOST_CHECK_EQUAL(f.state().proposal_voters.size(), 2u);
   f.veto(f.nodes[1]); // frozen electorate is not retroactively recomputed
   BOOST_CHECK_EQUAL(f.state().vetoes.size(), 1u);
   const auto ranked = f.producers();
   BOOST_REQUIRE_EQUAL(ranked.rows.size(), 4u);
   BOOST_CHECK_EQUAL(ranked.rows[0]["owner"].as_string(), f.nodes[0].to_string());
}

BOOST_AUTO_TEST_CASE(zero_block_candidate_cannot_claim_either_reward_bucket) {
   single_choice_fixture f;
   f.fund_voter(); f.vote({f.nodes[0]});
   const auto before = f.raw("producers"_n, f.nodes[0]);
   BOOST_CHECK_EXCEPTION(f.call("claimrewards"_n, fc::mutable_variant_object()("owner", f.nodes[0]), f.nodes[0]),
      fc::exception, [](const auto& e) { return single_choice_fixture::message(e, "standby rewards are disabled"); });
   BOOST_CHECK(f.raw("producers"_n, f.nodes[0]) == before);
}

BOOST_AUTO_TEST_CASE(integer_rpc_invalid_policy_is_an_error_not_zero_vote_fallback) {
   single_choice_fixture f;
   const auto& table = f.chain.control->db().get<table_id_object, by_code_scope_table>(
      boost::make_tuple("eosio"_n, "eosio"_n, "votepolicy"_n));
   const auto& value = f.chain.control->db().get<key_value_object, by_scope_primary>(
      boost::make_tuple(table.id, "votepolicy"_n.to_uint64_t()));
   // Simulate a corrupt/incompatible policy in this disposable test database.
   auto& test_db = const_cast<eosio::chain::database&>(f.chain.control->db());
   test_db.modify(value, [](auto& row) {
      std::string bytes(row.value.data(), row.value.size());
      bytes[0] = 99;
      row.value = std::string_view(bytes);
   });
   BOOST_CHECK_THROW(f.producers(), contract_table_query_exception);
}

BOOST_AUTO_TEST_CASE(independent_bpay_cannot_distribute_by_rank_without_production) {
   single_choice_fixture f;
   f.fund_voter(); f.vote({f.nodes[0]});
   const auto core_code = name{uint64_t(symbol(4, "SYS").to_symbol_code())};
   const auto before = f.chain.get_row_by_account("eosio.token"_n, "alice"_n, "accounts"_n, core_code);
   BOOST_CHECK_EXCEPTION(f.push({f.chain.get_action("eosio.token"_n, "transfer"_n, {{"alice"_n, "active"_n}},
      fc::mutable_variant_object()("from", "alice")("to", "eosio.bpay")("quantity", "1.0000 SYS")("memo", "rank rewards"))}, "alice"_n),
      fc::exception, [](const auto& e) { return single_choice_fixture::message(e, "rank-only standby reward distribution is disabled"); });
   BOOST_CHECK(f.chain.get_row_by_account("eosio.token"_n, "alice"_n, "accounts"_n, core_code) == before);
   BOOST_CHECK(f.chain.get_row_by_account("eosio.bpay"_n, "eosio.bpay"_n, "rewards"_n, f.nodes[0]).empty());
}

BOOST_AUTO_TEST_CASE(integer_stake_ownership_and_transfer_are_counted_once) {
   single_choice_fixture f;
   const auto receiver = f.nodes[2];
   f.call("buyram"_n, fc::mutable_variant_object()("payer", "alice")("receiver", receiver)("quant", "20.0000 SYS"));
   f.vote({f.nodes[0]});
   f.vote({f.nodes[1]}, receiver);
   auto stake = [&](bool transfer) {
      f.call("delegatebw"_n, fc::mutable_variant_object()("from", "alice")("receiver", receiver)
         ("stake_net_quantity", "10.0000 SYS")("stake_cpu_quantity", "20.0000 SYS")("transfer", transfer));
   };
   stake(false);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 300000);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[1])["total_votes"].as_int64(), 0);
   stake(true);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[0])["total_votes"].as_int64(), 300000);
   BOOST_CHECK_EQUAL(f.row("producers"_n, f.nodes[1])["total_votes"].as_int64(), 300000);
   for (auto owner : {"alice"_n, receiver})
      f.call("voteupdate"_n, fc::mutable_variant_object()("voter_name", owner), owner);
   BOOST_CHECK_EQUAL(f.row("voters"_n)["staked"].as_int64(), 300000);
   BOOST_CHECK_EQUAL(f.row("voters"_n, receiver)["staked"].as_int64(), 300000);
   BOOST_CHECK_EQUAL(f.row("globalv2"_n, "globalv2"_n)["total_producer_vote_weight"].as_int64(), 600000);
}

BOOST_AUTO_TEST_SUITE_END()
#endif // PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR
#ifdef PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR
#include "pureeos_resource_split_tests.inc"
#ifdef PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR
#include "pureeos_committee_boundary_tests.inc"
#endif
#ifdef PUREEOS_ROTATION_CONTRACTS_DIR
#include "pureeos_sequential_rotation_tests.inc"
#include "pureeos_reward_halving_tests.inc"
#include "pureeos_reward_activation_tests.inc"
#endif
#endif
#endif
