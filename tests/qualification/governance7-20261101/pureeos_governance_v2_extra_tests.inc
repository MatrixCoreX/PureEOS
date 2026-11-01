
namespace {
auto upgrade_api(sequential_rotation_fixture& f) {
 std::optional<eosio::chain_apis::tracked_votes> tracked;
 return eosio::chain_apis::read_only(*f.chain.control,{},{},tracked,fc::seconds(5),fc::seconds(5),nullptr)
  .get_upgrade_state({},fc::time_point::maximum());
}
struct expected_wasm_scope {
 std::optional<std::string> old;
 explicit expected_wasm_scope(const char* value) {if(const auto* p=std::getenv("PUREEOS_TEST_EXPECTED_SYSTEM_WASM"))old=p;setenv("PUREEOS_TEST_EXPECTED_SYSTEM_WASM",value,1);}
 ~expected_wasm_scope(){if(old)setenv("PUREEOS_TEST_EXPECTED_SYSTEM_WASM",old->c_str(),1);else unsetenv("PUREEOS_TEST_EXPECTED_SYSTEM_WASM");}
};
}
BOOST_AUTO_TEST_SUITE(pureeos_governance_v2_extra_tests)
BOOST_AUTO_TEST_CASE(fresh_bootstrap_accepts_only_seven_days_after_activation) {
 production_governance_fixture f(rotation_contracts_for_test(),false);
 const auto d=f.chain.control->get_protocol_feature_manager().get_builtin_digest(builtin_protocol_feature_t::pureeos_seven_day_governance);BOOST_REQUIRE(d);
 f.chain.schedule_protocol_features_wo_preactivation({*d});f.chain.produce_blocks(2);
 BOOST_REQUIRE(f.chain.control->is_builtin_activated(builtin_protocol_feature_t::pureeos_seven_day_governance));
 for(uint32_t seconds:{0u,604799u,604801u,1209600u}) {
  BOOST_CHECK_THROW(f.native(govinit{"developer"_n,seconds,{}},"eosio"_n,"owner"_n),action_validate_exception);
  BOOST_CHECK(f.state().developer==account_name{});
 }
 BOOST_REQUIRE_NO_THROW(f.native(govinit{"developer"_n,604800u,{}},"eosio"_n,"owner"_n));
 BOOST_CHECK_EQUAL(f.state().review_seconds,604800u);BOOST_CHECK(f.state().developer=="developer"_n);
 BOOST_CHECK_THROW(f.native(govinit{"developer"_n,604800u,{}},"eosio"_n,"owner"_n),fc::exception);
}
BOOST_AUTO_TEST_CASE(empty_or_malformed_effective_electorate_fails_closed_fault_injection) {
 sequential_rotation_fixture f(23);f.completed(21);enable_governance_v2(f);production_propose(f);f.veto(f.nodes[0]);f.wait_review();
 const auto frozen=f.state().proposal_voters,raw=f.state().vetoes;
 auto& db=const_cast<eosio::chain::database&>(f.chain.control->db());
 const auto& t=db.get<table_id_object,by_code_scope_table>(boost::make_tuple("eosio"_n,"eosio"_n,"rotcands"_n));
 const auto& c=db.get<key_value_object,by_scope_primary>(boost::make_tuple(t.id,f.nodes[0].to_uint64_t()));
 const std::string original(c.value.data(),c.value.size());BOOST_REQUIRE_EQUAL(original.size(),114u);
 // Explicit disposable-database corruption test, NOT finalized-duty evidence.
 db.modify(c,[&](auto& r){auto b=original;b[73]=2;r.value=std::string_view(b);});
 BOOST_CHECK_THROW(upgrade_api(f),action_validate_exception);BOOST_CHECK_THROW(f.execute(),action_validate_exception);
 db.modify(c,[&](auto& r){r.value=std::string_view(original);});
 for(auto n:f.nodes){const auto& row=db.get<key_value_object,by_scope_primary>(boost::make_tuple(t.id,n.to_uint64_t()));db.modify(row,[](auto& r){std::string b(r.value.data(),r.value.size());b[73]=1;r.value=std::string_view(b);});}
 const auto api=upgrade_api(f);BOOST_CHECK_EQUAL(api["effective_voter_count"].as_uint64(),0u);BOOST_CHECK_EQUAL(api["effective_veto_count"].as_uint64(),0u);BOOST_CHECK(api["execution_blocked_no_electorate"].as_bool());
 BOOST_CHECK_THROW(f.execute(),action_validate_exception);BOOST_CHECK(f.state().pending);BOOST_CHECK(f.state().proposal_voters==frozen);BOOST_CHECK(f.state().vetoes==raw);
 BOOST_TEST_MESSAGE("Fault injection only: malformed ban bytes and all-excluded electorate fail closed; no zero-denominator approval or history deletion");
}
BOOST_AUTO_TEST_CASE(maintenance_preserves_frozen_governance_rights_and_resumes) {
 sequential_rotation_fixture f(23);f.completed(21);enable_governance_v2(f);production_propose(f);
 const auto standby=f.nodes[22];
 f.call("setmaint"_n,fc::mutable_variant_object()("producer",standby)("maintenance",true),standby);
 BOOST_REQUIRE_EQUAL(f.row("bpmaint"_n,standby.to_uint64_t())["state"].as_uint64(),2u);
 auto api=upgrade_api(f);BOOST_CHECK_EQUAL(api["effective_voter_count"].as_uint64(),23u);BOOST_REQUIRE_NO_THROW(f.veto(standby));
 api=upgrade_api(f);BOOST_CHECK_EQUAL(api["effective_veto_count"].as_uint64(),1u);BOOST_CHECK_EQUAL(api["required_vetoes"].as_uint64(),16u);
 BOOST_CHECK_EQUAL(api["proposal_voters"].get_array().size(),23u);BOOST_CHECK_EQUAL(api["vetoes"].get_array().size(),1u);
 f.call("setmaint"_n,fc::mutable_variant_object()("producer",standby)("maintenance",false),standby);
 BOOST_CHECK_EQUAL(f.row("bpmaint"_n,standby.to_uint64_t())["state"].as_uint64(),0u);
 f.block();f.snapshot_check();
}
BOOST_AUTO_TEST_CASE(incumbent_maintenance_handoff_pauses_only_new_duties_then_resumes) {
 sequential_rotation_fixture f(23);f.completed(21);const auto victim=f.nodes[0];
 f.call("setmaint"_n,fc::mutable_variant_object()("producer",victim)("maintenance",true),victim);
 BOOST_REQUIRE_EQUAL(f.row("bpmaint"_n,victim.to_uint64_t())["state"].as_uint64(),1u);
 f.until([&]{return f.row("bpmaint"_n,victim.to_uint64_t())["state"].as_uint64()==2;});f.block();
 for(auto& p:f.chain.control->head_active_producers().producers)BOOST_REQUIRE(p.producer_name!=victim);
 for(auto& p:f.chain.control->head_active_finalizer_policy()->finalizers)BOOST_REQUIRE(p.description!=victim.to_string());
 const auto unpaid=f.row("producersv2"_n,victim.to_uint64_t())["unpaid_blocks"].as_uint64();const auto zero=f.candidate(victim)["zero_windows"].as_uint64();
 f.online.erase(std::find(f.online.begin(),f.online.end(),victim));f.restart(f.online);f.production_slots(3*21*12);
 BOOST_CHECK_EQUAL(f.row("producersv2"_n,victim.to_uint64_t())["unpaid_blocks"].as_uint64(),unpaid);BOOST_CHECK_EQUAL(f.candidate(victim)["zero_windows"].as_uint64(),zero);BOOST_CHECK(!f.candidate(victim)["banned"].as_bool());
 f.online.push_back(victim);f.restart(f.online);f.call("setmaint"_n,fc::mutable_variant_object()("producer",victim)("maintenance",false),victim);
 BOOST_CHECK_EQUAL(f.row("bpmaint"_n,victim.to_uint64_t())["state"].as_uint64(),0u);
 f.until([&]{const auto a=f.list("active");return std::find(a.begin(),a.end(),victim)!=a.end();});f.check_split();f.block();f.snapshot_check();f.observer_replay();
}
BOOST_AUTO_TEST_CASE(reviewed_upgrade_normalizes_existing_released_bans_and_backfills211) {
 expected_wasm_scope expected("6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62");
 sequential_rotation_fixture f(212,false,"/campaign/contracts-legacy-current");f.completed(21);const auto victim=f.nodes[21],backfill=f.nodes[210];
 f.online.erase(std::find(f.online.begin(),f.online.end(),victim));f.restart(f.online);f.until([&]{return f.candidate(victim)["banned"].as_bool();},1500);
 const auto evidence=resource_json(f.candidate(victim)["evidence_slots"]),old_rank=f.candidate(victim)["rank"].as_string();
 BOOST_REQUIRE_EQUAL(f.candidate(backfill)["ticket"].as_uint64(),0u);
 f.until([&]{const auto a=f.list("active");return f.rotation()["stage"].as_uint64()==0 && std::find(a.begin(),a.end(),victim)==a.end();},1500);
 enable_governance_v2(f);
 const auto code=read_wasm("/campaign/contracts/eosio.system/eosio.system.wasm");f.upgrade_code.assign(code.begin(),code.end());
 const auto& abi=f.chain.control->db().get<account_object,by_name>("eosio"_n).abi;f.upgrade_abi.assign(abi.data(),abi.data()+abi.size());
 f.push({f.production_refresh(),action({permission_level{"developer"_n,"upgrade"_n}},f.proposal("eosio"_n))},"developer"_n);
 BOOST_REQUIRE(f.state().execute_after-f.state().proposed_at==fc::seconds(604800));f.wait_review();BOOST_REQUIRE_NO_THROW(f.execute());
 BOOST_REQUIRE_EQUAL((f.chain.control->db().get<account_metadata_object,by_name>("eosio"_n).code_hash.str()),"373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb");
 f.block();f.until([&]{return f.candidate(backfill)["ticket"].as_uint64()>0 && f.candidate(backfill)["seen_epoch"].as_uint64()==f.rotation()["published_epoch"].as_uint64();},1500);
 BOOST_CHECK_EQUAL(f.rotation()["ranked_count"].as_uint64(),f.rotation()["capacity"].as_uint64());BOOST_CHECK(f.candidate(victim)["rank"].as_string()!=old_rank);BOOST_CHECK_EQUAL(f.candidate(victim)["ticket"].as_uint64(),0u);BOOST_CHECK_EQUAL(resource_json(f.candidate(victim)["evidence_slots"]),evidence);
 f.block();f.snapshot_check();f.observer_replay();
 BOOST_TEST_MESSAGE("Actual released6ed4 ban -> normal sealed7day review/devexec ->590c: old ban survives, live rank revoked and rank211 backfills; independent replay and snapshot agree");
}
BOOST_AUTO_TEST_SUITE_END()
