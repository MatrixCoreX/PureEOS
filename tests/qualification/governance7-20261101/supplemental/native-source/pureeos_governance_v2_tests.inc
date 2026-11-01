
namespace {
void enable_governance_v2(sequential_rotation_fixture& f) {
 const auto digest=f.chain.control->get_protocol_feature_manager().get_builtin_digest(builtin_protocol_feature_t::pureeos_seven_day_governance);
 BOOST_REQUIRE(digest);f.chain.schedule_protocol_features_wo_preactivation({*digest});f.block();f.block();
 BOOST_REQUIRE(f.chain.control->is_builtin_activated(builtin_protocol_feature_t::pureeos_seven_day_governance));
}
void production_propose(sequential_rotation_fixture& f) {
 f.push({f.production_refresh(),action({permission_level{"developer"_n,"upgrade"_n}},f.proposal())},"developer"_n);
}
void blacklist_pending_voter(bool voted) {
 sequential_rotation_fixture f(25);f.completed(21);enable_governance_v2(f);
 const auto victim=f.nodes[21];f.online.erase(std::find(f.online.begin(),f.online.end(),victim));f.restart(f.online);
 production_propose(f);
 BOOST_REQUIRE_EQUAL(f.state().proposal_voters.size(),25u);
 for(size_t i=0;i<15;++i)f.veto(f.nodes[i]);
 f.veto(voted?victim:f.nodes[15]);
 const auto frozen=f.state().proposal_voters;const auto raw_vetoes=f.state().vetoes;
 BOOST_REQUIRE_EQUAL(raw_vetoes.size(),16u);
 BOOST_REQUIRE_EQUAL(upgrade_veto_threshold(25),17u);
 f.until([&]{return f.rotation()["stage"].as_uint64()==2 && f.rotation()["incoming"].as_string()==victim.to_string();},1200);
 f.until([&]{return f.candidate(victim)["banned"].as_bool();},1200);
 BOOST_REQUIRE_EQUAL(f.candidate(victim)["zero_windows"].as_uint64(),3u);
 const auto tally=get_upgrade_effective_tally(f.chain.control->db(),f.state(),true);
 BOOST_CHECK_EQUAL(tally.voters.size(),24u);BOOST_CHECK_EQUAL(tally.vetoes.size(),voted?15u:16u);
 BOOST_CHECK_EQUAL(upgrade_veto_threshold(tally.voters.size()),16u);
 BOOST_CHECK(f.state().proposal_voters==frozen);BOOST_CHECK(f.state().vetoes==raw_vetoes);
 BOOST_CHECK(std::find(tally.excluded_banned_voters.begin(),tally.excluded_banned_voters.end(),victim)!=tally.excluded_banned_voters.end());
 BOOST_CHECK_THROW(f.native(devveto{victim,f.state().proposal_id},victim),action_validate_exception);
 f.until([&]{const auto members=f.list("active");return f.rotation()["stage"].as_uint64()==0 && std::find(members.begin(),members.end(),victim)==members.end();},1200);
 BOOST_REQUIRE_EQUAL(f.chain.control->head_active_producers().producers.size(),21u);
 f.wait_review();
 if(voted) {BOOST_REQUIRE_NO_THROW(f.execute());BOOST_CHECK(!f.state().pending);}
 else {BOOST_CHECK_THROW(f.execute(),action_validate_exception);BOOST_CHECK(f.state().pending);}
 f.block();f.snapshot_check();f.observer_replay();
 BOOST_TEST_MESSAGE((voted?"Real finalized ban:25/16 ->24/15; execution allowed, raw veto retained. Denominator-only exclusion would incorrectly veto.":"Real finalized ban without prior veto:25/16 ->24/16; threshold17->16, execution vetoed; only denominator changes."));
}
}
BOOST_AUTO_TEST_SUITE(pureeos_governance_v2_tests)
BOOST_AUTO_TEST_CASE(seven_days_permissions_boundary_and_old_pending_deadline) {
 sequential_rotation_fixture f(23);f.completed(21);
 production_propose(f);const auto old_deadline=f.state().execute_after;
 BOOST_REQUIRE(f.state().execute_after-f.state().proposed_at==fc::seconds(1209600));
 enable_governance_v2(f);
 BOOST_CHECK_EQUAL(f.state().review_seconds,604800u);BOOST_CHECK(f.state().execute_after==old_deadline);
 f.native(devcancel{f.state().proposal_id},"developer"_n,"upgrade"_n);
 production_propose(f);const auto deadline=f.state().execute_after;const auto id=f.state().proposal_id;
 BOOST_REQUIRE(f.state().execute_after-f.state().proposed_at==fc::seconds(604800));
 for(auto actor:{f.nodes[0],"alice"_n}) {
  BOOST_CHECK_THROW(f.native(f.proposal(),actor),fc::exception);
  BOOST_CHECK_THROW(f.native(devcancel{id},actor),fc::exception);
  BOOST_CHECK_THROW(f.native(devexec{id,f.upgrade_code,f.upgrade_abi},actor),fc::exception);
  BOOST_CHECK_EQUAL(f.state().proposal_id,id);BOOST_CHECK(f.state().pending);
 }
 BOOST_CHECK_THROW(f.native(devveto{"alice"_n,id},"alice"_n),action_validate_exception);
 // Registration/vote changes alone do not rewrite a frozen electorate.
 f.call("unregprod"_n,fc::mutable_variant_object()("producer",f.nodes[22]),f.nodes[22]);
 const auto tally=get_upgrade_effective_tally(f.chain.control->db(),f.state(),true);BOOST_CHECK_EQUAL(tally.voters.size(),23u);
 f.veto(f.nodes[22]); // still a frozen voter, not permanently banned
 f.advance_to(deadline-fc::milliseconds(500));BOOST_CHECK_THROW(f.execute(),action_validate_exception);
 f.advance_to(deadline);BOOST_REQUIRE_NO_THROW(f.execute());BOOST_CHECK(!f.state().pending);
 f.block();f.snapshot_check();f.observer_replay();
 BOOST_TEST_MESSAGE("Protocol activation preserves existing14day pending deadline; next proposal exactly7days; half-second early blocked/exact boundary succeeds; ordinaryBP and user cannot propose/cancel/execute; unregistration alone retains frozen voter");
}
BOOST_AUTO_TEST_CASE(ban_removes_already_cast_veto_and_denominator) {blacklist_pending_voter(true);}
BOOST_AUTO_TEST_CASE(ban_without_veto_changes_denominator_and_threshold) {blacklist_pending_voter(false);}
BOOST_AUTO_TEST_SUITE_END()
