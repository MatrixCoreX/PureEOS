// Native identity/authority tests; process-local fixture keys never leave tests.
#ifndef PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS
#if defined(PUREEOS_TEST_BUILD) && defined(PUREEOS_TEST_GOVERNANCE_30MIN)
#define PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS 1800
#else
#define PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS 604800
#endif
#endif
namespace {
void activate_named_developer_feature(governance_fixture& f) {
   const auto& features=f.chain.control->get_protocol_feature_manager().get_protocol_feature_set();
   f.push({action({permission_level{"eosio"_n,"active"_n}},"eosio"_n,"activate"_n,
      fc::raw::pack(*features.get_builtin_digest(builtin_protocol_feature_t::pureeos_rotation_context)))},"eosio"_n);
   f.chain.produce_blocks(2);
   f.chain.schedule_protocol_features_wo_preactivation({*features.get_builtin_digest(builtin_protocol_feature_t::pureeos_seven_day_governance),
      *features.get_builtin_digest(builtin_protocol_feature_t::pureeos_system_account_governance)});
   f.chain.produce_blocks(2);
}
authority developer_delegates(uint32_t threshold,std::vector<account_name> members={"alice"_n,"bob"_n,"developer"_n}) {
   authority a(threshold,{},{});
   for(auto actor:members)a.accounts.push_back({{actor,"active"_n},1});
   std::sort(a.accounts.begin(),a.accounts.end());return a;
}
struct named_developer_fixture:governance_fixture {
   named_developer_fixture(uint32_t threshold=2):governance_fixture(0,false) {
      chain.create_accounts({"eosio.dev"_n,"bob"_n});
      chain.set_code("eosio.token"_n,vector<uint8_t>(upgrade_code.begin(),upgrade_code.end()));
      const auto delegated=developer_delegates(threshold);
      for(auto role:{"upgrade"_n,"active"_n,"owner"_n})
         native(updateauth{"eosio.dev"_n,role,role=="owner"_n?name{}:role=="active"_n?"owner"_n:"active"_n,delegated},"eosio.dev"_n,"owner"_n,"owner");
      activate_named_developer_feature(*this);
      native(govinit{"eosio.dev"_n,PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS,{}},"eosio"_n,"owner"_n);
   }
   transaction_trace_ptr signed_by(std::vector<action> actions,std::vector<account_name> signers) {
      signed_transaction trx;trx.actions=std::move(actions);chain.set_transaction_headers(trx,60+ ++nonce);
      if(chain.control->is_building_block())trx.expiration=fc::time_point_sec(chain.control->pending_block_time()+fc::seconds(60+nonce));
      for(auto actor:signers)trx.sign(chain.get_private_key(actor,"active"),chain.control->get_chain_id());
      return chain.push_transaction(trx);
   }
   std::vector<action> creation_proposal()const {
      const syspropose proposed{"eosio.signup"_n,"alice"_n,10000,1,uint32_t(upgrade_code.size()),uint32_t(upgrade_abi.size()),
         fc::sha256::hash(upgrade_code.data(),upgrade_code.size()),fc::sha256::hash(upgrade_abi.data(),upgrade_abi.size()),fc::sha256::hash("named dev review")};
      return {action({permission_level{"eosio.dev"_n,"upgrade"_n}},"eosio"_n,"refreshgov"_n,fc::raw::pack("eosio"_n)),
              action({permission_level{"eosio.dev"_n,"upgrade"_n}},proposed)};
   }
   action change(permission_name role,const authority& a)const {
      return action({permission_level{"eosio.dev"_n,"owner"_n}},updateauth{"eosio.dev"_n,role,role=="owner"_n?name{}:role=="active"_n?"owner"_n:"active"_n,a});
   }
};
}

BOOST_AUTO_TEST_CASE(developer_named_identity_requires_feature_and_account_delegates) {
   governance_fixture off(0,false);off.chain.create_account("eosio.dev"_n);
   off.native(updateauth{"eosio.dev"_n,"upgrade"_n,"active"_n,authority(off.chain.get_public_key("alice"_n,"active"))},"eosio.dev"_n,"owner"_n,"owner");
   BOOST_CHECK_THROW(off.native(govinit{"eosio.dev"_n,upgrade_review_seconds,{}},"eosio"_n,"owner"_n),fc::exception);
   activate_named_developer_feature(off);
   BOOST_CHECK_EXCEPTION(off.native(govinit{"eosio.dev"_n,PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS,{}},"eosio"_n,"owner"_n),action_validate_exception,
      [](const auto& e){return e.to_detail_string().find("account-based")!=std::string::npos;});
   named_developer_fixture f;
   BOOST_CHECK(f.state().developer=="eosio.dev"_n);
   BOOST_CHECK(!is_protected_system_account(f.chain.control->db(),"eosio.dev"_n));
   BOOST_CHECK(is_protected_system_account(f.chain.control->db(),"eosio"_n));
   BOOST_CHECK((!f.chain.control->db().get<account_metadata_object,by_name>("eosio.dev"_n).is_privileged()));
   for(auto role:{"owner"_n,"active"_n,"upgrade"_n}) {
      const auto a=f.chain.control->get_authorization_manager().get_permission({"eosio.dev"_n,role}).auth.to_authority();
      BOOST_CHECK(a.keys.empty()&&a.waits.empty());BOOST_CHECK_EQUAL(a.threshold,2u);BOOST_CHECK_EQUAL(a.accounts.size(),3u);
   }
   BOOST_CHECK_THROW(f.native(govinit{"eosio.dev"_n,PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS,{}},"eosio"_n,"owner"_n),fc::exception);
}

BOOST_AUTO_TEST_CASE(developer_named_identity_two_of_three_controls_creation_and_deployment) {
   named_developer_fixture f;
   BOOST_CHECK_THROW(f.signed_by(f.creation_proposal(),{"alice"_n}),fc::exception);
   BOOST_REQUIRE_NO_THROW(f.signed_by(f.creation_proposal(),{"alice"_n,"bob"_n}));
   const auto* state=find_system_account_proposal(f.chain.control->db());BOOST_REQUIRE(state&&state->pending);
   f.advance_to(state->execute_after);
   const auto execution=action({permission_level{"eosio.dev"_n,"upgrade"_n},permission_level{"alice"_n,"active"_n}},sysexec{state->proposal_id,f.upgrade_code,f.upgrade_abi});
   BOOST_CHECK_THROW(f.signed_by({execution},{"alice"_n}),fc::exception);
   BOOST_REQUIRE_NO_THROW(f.signed_by({execution},{"alice"_n,"bob"_n}));
   BOOST_CHECK((f.chain.control->db().find<account_object,by_name>("eosio.signup"_n)));
   BOOST_CHECK(!find_system_account_proposal(f.chain.control->db())->pending);
   BOOST_CHECK((!f.chain.control->db().get<account_metadata_object,by_name>("eosio.signup"_n).is_privileged()));
}

BOOST_AUTO_TEST_CASE(developer_named_identity_management_cannot_weaken_or_embed_direct_keys) {
   named_developer_fixture f;const auto& manager=f.chain.control->get_authorization_manager();
   const auto before=fc::raw::pack(manager.get_permission({"eosio.dev"_n,"upgrade"_n}).auth.to_authority());
   for(auto role:{"owner"_n,"active"_n})
      BOOST_CHECK_EXCEPTION(f.signed_by({f.change(role,developer_delegates(1,{"alice"_n}))},{"alice"_n,"bob"_n}),action_validate_exception,
         [](const auto& e){return e.to_detail_string().find("cannot be weaker")!=std::string::npos;});
   std::vector<authority> invalid{authority(f.chain.get_public_key("alice"_n,"active"))};
   auto weighted=developer_delegates(2,{"alice"_n});weighted.accounts[0].weight=2;invalid.push_back(weighted);
   auto duplicate=developer_delegates(2,{"alice"_n});duplicate.accounts.push_back({{"alice"_n,"owner"_n},1});std::sort(duplicate.accounts.begin(),duplicate.accounts.end());invalid.push_back(duplicate);
   auto code=developer_delegates(1,{"alice"_n});code.accounts[0].permission.permission=config::eosio_code_name;invalid.push_back(code);
   auto waits=developer_delegates(1,{"alice"_n});waits.waits.push_back({1,1});invalid.push_back(waits);
   for(const auto& a:invalid)BOOST_CHECK_THROW(f.signed_by({f.change("upgrade"_n,a)},{"alice"_n,"bob"_n}),fc::exception);
   BOOST_CHECK_THROW(f.signed_by({action({permission_level{"eosio.dev"_n,"owner"_n}},deleteauth{"eosio.dev"_n,"upgrade"_n})},{"alice"_n,"bob"_n}),fc::exception);
   BOOST_CHECK(fc::raw::pack(manager.get_permission({"eosio.dev"_n,"upgrade"_n}).auth.to_authority())==before);
}

BOOST_AUTO_TEST_CASE(developer_named_identity_multisig_transition_covers_parent_permissions) {
   named_developer_fixture f(1);const auto multi=developer_delegates(2);
   BOOST_CHECK_THROW(f.signed_by({f.change("upgrade"_n,multi)},{"alice"_n}),fc::exception);
   BOOST_REQUIRE_NO_THROW(f.signed_by({f.change("owner"_n,multi),f.change("active"_n,multi),f.change("upgrade"_n,multi)},{"alice"_n}));
   for(auto role:{"owner"_n,"active"_n,"upgrade"_n})
      BOOST_CHECK_EQUAL(f.chain.control->get_authorization_manager().get_permission({"eosio.dev"_n,role}).auth.threshold,2u);
   BOOST_CHECK_THROW(f.signed_by(f.creation_proposal(),{"alice"_n}),fc::exception);
   BOOST_REQUIRE_NO_THROW(f.signed_by(f.creation_proposal(),{"alice"_n,"bob"_n}));
}
