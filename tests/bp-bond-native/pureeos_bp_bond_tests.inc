
namespace {
constexpr int64_t bond_units=100000000;
struct bp_bond_fixture : sequential_rotation_fixture {
 explicit bp_bond_fixture(size_t count=23):sequential_rotation_fixture(count,false,rotation_contracts_for_test(),true){completed(21);}
 fc::variant bond(name owner)const{return row("bpbonds"_n,owner.to_uint64_t());}
 fc::variant bond_config()const{return row("bpbondcfg"_n,"bpbondcfg"_n.to_uint64_t());}
 int64_t balance(name owner)const{
  const auto code=symbol::from_string("4,AIC").to_symbol_code().value;
  if(chain.get_row_by_account("eosio.token"_n,owner,"accounts"_n,name{code}).empty())return 0;
  return asset::from_string(row("accounts"_n,code,owner,"eosio.token"_n)["balance"].as_string()).get_amount();
 }
 void transfer_to(name owner,const std::string& amount){
  push({chain.get_action("eosio.token"_n,"transfer"_n,{{nodes[0],"active"_n}},fc::mutable_variant_object()
    ("from",nodes[0])("to",owner)("quantity",amount)("memo","BP bond functional test"))},nodes[0]);
 }
 void post(name owner){call("bondbp"_n,fc::mutable_variant_object()("producer",owner),owner);}
 void request_exit(name owner){call("exitbond"_n,fc::mutable_variant_object()("producer",owner),owner);}
 void refund(name owner){call("refundbond"_n,fc::mutable_variant_object()("producer",owner),owner);}
 void mature(name owner){
  until([&]{return bond(owner)["state"].as_uint64()==2;},2000);
  const auto exit_checkpoint=bond(owner)["exit_block"].as_uint64();
  until([&]{return chain.last_irreversible_block_num()>=exit_checkpoint;});
  const auto deadline=bond(owner)["refundable_at"].as_uint64();
  const auto now=chain.head().header().timestamp.to_time_point().sec_since_epoch();
  if(deadline>now)block(uint32_t((deadline-now)*2+2));
  until([&]{return rotation()["stage"].as_uint64()==0 && chain.control->head_pending_finalizer_policy()==nullptr;},2500);
  for(unsigned i=0;i<8;++i)block();
  const auto future_checkpoint=chain.head().block_num();
  until([&]{return chain.last_irreversible_block_num()>=future_checkpoint;});
  BOOST_TEST_MESSAGE("Mature bond: head="<<chain.head().header().timestamp.to_time_point().sec_since_epoch()<<" deadline="<<bond(owner)["refundable_at"].as_uint64()<<" exit="<<exit_checkpoint<<" LIB="<<chain.last_irreversible_block_num());
 }
 void reg_unbonded(name n){call("regproducer"_n,fc::mutable_variant_object()("producer",n)
    ("producer_key",chain.get_public_key(n,"active"))("url","")("location",0),n);}
};
}
BOOST_AUTO_TEST_SUITE(pureeos_bp_bond_tests)
BOOST_AUTO_TEST_CASE(fixed_policy_seven_days_and_custody_permissions){
 bp_bond_fixture f;const auto cfg=f.bond_config();BOOST_CHECK_EQUAL(cfg["refund_delay_sec"].as_uint64(),604800u);
 BOOST_CHECK(cfg["enforced"].as_bool());BOOST_CHECK_EQUAL(cfg["locked"].as_int64(),int64_t(f.nodes.size())*bond_units);
 for(auto n:{"eosio.bond"_n,"eosio.bslash"_n}){
  const auto& auth=f.chain.control->get_authorization_manager().get_permission({n,"active"_n}).auth;
  BOOST_CHECK(auth.keys.empty());BOOST_CHECK(std::any_of(auth.accounts.begin(),auth.accounts.end(),[](const auto& a){
   return a.permission.actor=="eosio"_n && a.permission.permission=="eosio.code"_n;}));
 }
 BOOST_CHECK_EQUAL(f.balance("eosio.bond"_n),int64_t(f.nodes.size())*bond_units);
 BOOST_CHECK_EQUAL(f.balance("eosio.bslash"_n),0);BOOST_CHECK_EQUAL(f.balance("eosio.token"_n),0);
 BOOST_CHECK_THROW(f.call("initbpbond"_n,fc::mutable_variant_object()),fc::exception);
 BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold,15u);
}
BOOST_AUTO_TEST_CASE(missing_deposit_blocks_both_registration_actions){
 bp_bond_fixture f;const auto n="bonduser1111"_n;f.stake("10.0000 AIC","10.0000 AIC");
 BOOST_CHECK_EXCEPTION(f.reg_unbonded(n),fc::exception,[](const auto&e){return resource_split_fixture::message(e,"10000.0000 AIC");});
 fc::variants authority{fc::variant("block_signing_authority_v0"),fc::variant(fc::mutable_variant_object()("threshold",1)
  ("keys",fc::variants{fc::variant(fc::mutable_variant_object()("key",f.chain.get_public_key(n,"active"))("weight",1))}))};
 auto data=fc::mutable_variant_object()("producer",n)("producer_authority",authority)("url","")("location",0);
 BOOST_CHECK_EXCEPTION(f.call("regproducer2"_n,data,n),fc::exception,[](const auto&e){return resource_split_fixture::message(e,"10000.0000 AIC");});
 f.transfer_to(n,"10000.0000 AIC");f.post(n);BOOST_REQUIRE_NO_THROW(f.reg_unbonded(n));BOOST_REQUIRE_NO_THROW(f.call("regproducer2"_n,data,n));
}
BOOST_AUTO_TEST_CASE(insufficient_funds_and_wrong_authorization_rollback){
 bp_bond_fixture f;const auto n="bonduser1111"_n;const auto cfg=resource_json(f.bond_config());const auto before=f.balance(n);
 BOOST_REQUIRE_LT(before,bond_units);BOOST_CHECK_THROW(f.post(n),fc::exception);
 BOOST_CHECK_EQUAL(f.balance(n),before);BOOST_CHECK_EQUAL(resource_json(f.bond_config()),cfg);
 BOOST_CHECK(f.chain.get_row_by_account("eosio"_n,"eosio"_n,"bpbonds"_n,n).empty());
 BOOST_CHECK_THROW(f.call("bondbp"_n,fc::mutable_variant_object()("producer",n),f.nodes[0]),fc::exception);
}
BOOST_AUTO_TEST_CASE(exact_deposit_no_vote_resource_change_and_no_double_charge){
 bp_bond_fixture f;auto n="bonduser1111"_n;f.stake("10.0000 AIC","10.0000 AIC");
 const auto net=f.resource(n,"net"),cpu=f.resource(n,"cpu"),custody=f.balance("eosio.bond"_n);
 f.transfer_to(n,"10000.0000 AIC");const auto wallet=f.balance(n);f.post(n);
 BOOST_CHECK_EQUAL(f.balance(n),wallet-bond_units);BOOST_CHECK_EQUAL(f.balance("eosio.bond"_n),custody+bond_units);
 BOOST_CHECK_EQUAL(f.resource(n,"net"),net);BOOST_CHECK_EQUAL(f.resource(n,"cpu"),cpu);
 const auto cfg=resource_json(f.bond_config());BOOST_CHECK_THROW(f.post(n),fc::exception);
 BOOST_CHECK_EQUAL(resource_json(f.bond_config()),cfg);BOOST_CHECK_EQUAL(f.balance(n),wallet-bond_units);
 BOOST_CHECK_EQUAL(f.balance("eosio.token"_n),0);
}
BOOST_AUTO_TEST_CASE(uncredited_direct_transfer_cannot_forge_a_bond){
 bp_bond_fixture f;auto n="bonduser1111"_n;f.stake("10.0000 AIC","10.0000 AIC");const auto locked=f.bond_config()["locked"].as_int64();
 f.transfer_to(n,"10000.0000 AIC");
 f.chain.push_action("eosio.token"_n,"transfer"_n,n,fc::mutable_variant_object()("from",n)("to","eosio.bond")("quantity","10000.0000 AIC")("memo","pretend bond"));
 BOOST_CHECK_EQUAL(f.bond_config()["locked"].as_int64(),locked);BOOST_CHECK_EXCEPTION(f.reg_unbonded(n),fc::exception,[](const auto&e){return resource_split_fixture::message(e,"10000.0000 AIC");});
 f.transfer_to(n,"10000.0000 AIC");f.post(n);BOOST_CHECK_EQUAL(f.bond_config()["locked"].as_int64(),locked+bond_units);
}
BOOST_AUTO_TEST_CASE(active_exit_requires_real_handoff_and_full_seven_day_cooldown){
 bp_bond_fixture f;const auto owner=f.nodes[0];f.request_exit(owner);
 BOOST_CHECK_EQUAL(f.bond(owner)["state"].as_uint64(),1u);BOOST_CHECK_THROW(f.refund(owner),fc::exception);
 BOOST_CHECK_THROW(f.reg_unbonded(owner),fc::exception);f.until([&]{return f.bond(owner)["state"].as_uint64()==2;},2000);
 BOOST_CHECK_EXCEPTION(f.refund(owner),fc::exception,[](const auto&e){return resource_split_fixture::message(e,"cooldown");});
 f.mature(owner);const auto before=f.balance(owner);f.refund(owner);BOOST_CHECK_EQUAL(f.balance(owner)-before,bond_units);
 BOOST_CHECK_EQUAL(f.bond(owner)["state"].as_uint64(),3u);BOOST_CHECK_THROW(f.refund(owner),fc::exception);
 f.post(owner);BOOST_REQUIRE_NO_THROW(f.reg_unbonded(owner));f.check_split();
}
BOOST_AUTO_TEST_CASE(no_funded_replacement_keeps_duty_and_deposit_locked){
 bp_bond_fixture f(21);const auto owner=f.nodes[0];f.request_exit(owner);
 for(unsigned i=0;i<200;++i)f.step();
 BOOST_CHECK_EQUAL(f.bond(owner)["state"].as_uint64(),1u);BOOST_CHECK_THROW(f.refund(owner),fc::exception);
 BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->finalizers.size(),21u);
 BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold,15u);BOOST_CHECK(!f.candidate(owner)["banned"].as_bool());
}
BOOST_AUTO_TEST_CASE(real_finalized_blacklist_forfeits_once_and_locks_dedicated_pool){
 bp_bond_fixture f(22);const auto bad=f.nodes[21];f.online.erase(std::find(f.online.begin(),f.online.end(),bad));f.restart(f.online);
 f.until([&]{return f.candidate(bad)["banned"].as_bool();},1800);
 BOOST_CHECK_EQUAL(f.bond(bad)["state"].as_uint64(),4u);BOOST_CHECK_EQUAL(f.bond(bad)["balance"].as_int64(),0);
 BOOST_CHECK_EQUAL(f.bond(bad)["forfeited"].as_int64(),bond_units);BOOST_CHECK_EQUAL(f.bond_config()["slash_pending"].as_int64(),bond_units);
 BOOST_CHECK_THROW(f.refund(bad),fc::exception);BOOST_CHECK_THROW(f.request_exit(bad),fc::exception);BOOST_CHECK_THROW(f.post(bad),fc::exception);
 const auto checkpoint=f.bond(bad)["ban_record_block"].as_uint64();f.until([&]{return f.chain.last_irreversible_block_num()>=checkpoint;});
 f.call("settlebond"_n,fc::mutable_variant_object()("producer",bad));
 BOOST_CHECK_EQUAL(f.balance("eosio.bslash"_n),bond_units);BOOST_CHECK_EQUAL(f.bond_config()["slash_pending"].as_int64(),0);
 BOOST_CHECK_EQUAL(f.bond_config()["slash_settled"].as_int64(),bond_units);BOOST_CHECK_EQUAL(f.balance("eosio.token"_n),0);
 BOOST_CHECK_THROW(f.call("settlebond"_n,fc::mutable_variant_object()("producer",bad)),fc::exception);
 BOOST_CHECK_THROW(f.chain.push_action("eosio.token"_n,"transfer"_n,"eosio.bslash"_n,fc::mutable_variant_object()
  ("from","eosio.bslash")("to",bad)("quantity","10000.0000 AIC")("memo","unauthorized withdrawal")),fc::exception);
 f.block();f.snapshot_check();f.observer_replay();
}
BOOST_AUTO_TEST_CASE(refund_notification_failure_rolls_back_entire_liability){
 bp_bond_fixture f;auto owner=f.nodes.back();f.request_exit(owner);f.mature(owner);
 f.chain.set_code(owner,"(module (func (export \"apply\") (param i64 i64 i64) unreachable))");
 const auto cfg=resource_json(f.bond_config()),record=resource_json(f.bond(owner));const auto wallet=f.balance(owner);
 BOOST_CHECK_THROW(f.refund(owner),fc::exception);BOOST_CHECK_EQUAL(resource_json(f.bond_config()),cfg);
 BOOST_CHECK_EQUAL(resource_json(f.bond(owner)),record);BOOST_CHECK_EQUAL(f.balance(owner),wallet);
}
BOOST_AUTO_TEST_CASE(inactive_exit_keeps_earned_reward_claim_rights){
 bp_bond_fixture f;auto owner=f.nodes[0];
 BOOST_REQUIRE_GT(f.row("producersv2"_n,owner.to_uint64_t())["unpaid_blocks"].as_uint64(),0u);
 f.request_exit(owner);f.until([&]{return f.bond(owner)["state"].as_uint64()==2;},2000);
 f.block(2u*(86400u+1));f.until([&]{return f.rotation()["stage"].as_uint64()==0;},2500);
 const auto before=f.balance(owner);f.call("claimrewards"_n,fc::mutable_variant_object()("owner",owner),owner);
 BOOST_CHECK_GT(f.balance(owner),before);BOOST_CHECK_EQUAL(f.bond(owner)["balance"].as_int64(),bond_units);
 BOOST_CHECK_EQUAL(f.row("producersv2"_n,owner.to_uint64_t())["unpaid_blocks"].as_uint64(),0u);
}
BOOST_AUTO_TEST_SUITE_END()
