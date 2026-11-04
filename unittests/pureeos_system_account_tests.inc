// Native boundary tests use the deliberately permissive system fixture above.
// Token transfer/buyram are no-ops here; these cases cannot qualify economics,
// resource admission, or the production registration contract.
namespace {
struct system_account_fixture : governance_fixture {
   system_account_fixture(size_t voters=4, bool activate=true):governance_fixture(voters,false) {
      chain.set_code("eosio.token"_n,vector<uint8_t>(upgrade_code.begin(),upgrade_code.end()));
      if(activate) {
         const auto digest=chain.control->get_protocol_feature_manager().get_protocol_feature_set().get_builtin_digest(
            builtin_protocol_feature_t::pureeos_rotation_context);
         BOOST_REQUIRE(digest);
         push({action({permission_level{"eosio"_n,"active"_n}},"eosio"_n,"activate"_n,fc::raw::pack(*digest))},"eosio"_n);
         chain.produce_blocks(2);
      }
      seal(); // Legacy bootstrap is unchanged; the feature is activated afterward.
      if(activate) {
         const auto& features=chain.control->get_protocol_feature_manager().get_protocol_feature_set();
         chain.schedule_protocol_features_wo_preactivation({
            *features.get_builtin_digest(builtin_protocol_feature_t::pureeos_seven_day_governance),
            *features.get_builtin_digest(builtin_protocol_feature_t::pureeos_system_account_governance)});
         chain.produce_blocks(2);
      }
      for(auto name:{"syspropose"_n,"syscancel"_n,"sysexec"_n})
         native(linkauth{"developer"_n,"eosio"_n,name,"upgrade"_n},"developer"_n);
   }
   const system_account_proposal_object& creation()const {
      const auto* value=find_system_account_proposal(chain.control->db());BOOST_REQUIRE(value);return *value;
   }
   syspropose creation_args(account_name target="eosio.signup"_n)const {
      return {target,"developer"_n,10000,1,uint32_t(upgrade_code.size()),uint32_t(upgrade_abi.size()),
         fc::sha256::hash(upgrade_code.data(),upgrade_code.size()),fc::sha256::hash(upgrade_abi.data(),upgrade_abi.size()),
         fc::sha256::hash("system account creation review")};
   }
   void propose_creation(syspropose args) {
      push({refresh(nodes),action({permission_level{"developer"_n,"upgrade"_n}},args)},"developer"_n);
   }
   void propose_creation(){propose_creation(creation_args());}
   void execute_creation(bool sponsor=true,bytes code={}) {
      std::vector<permission_level> auth{{"developer"_n,"upgrade"_n}};
      if(sponsor)auth.push_back({"developer"_n,"active"_n});
      push({action(auth,sysexec{creation().proposal_id,code.empty()?upgrade_code:code,upgrade_abi})},"developer"_n);
   }
   void veto_creation(account_name voter) {native(sysveto{voter,creation().proposal_id},voter);}
   bool exists(account_name name="eosio.signup"_n)const{return chain.control->db().find<account_object,by_name>(name);}
   void simulated_ban_record(account_name voter) {
      // Disposable native-reader fixture only. This is not a production proof
      // of a ban or a substitute for finalized personal zero-production windows.
      auto& db=const_cast<eosio::chain::database&>(chain.control->db());
      auto table=[&](name n)->const table_id_object& {
         if(const auto* t=db.find<table_id_object,by_code_scope_table>(boost::make_tuple("eosio"_n,"eosio"_n,n)))return *t;
         return db.create<table_id_object>([&](auto& t){t.code="eosio"_n;t.scope="eosio"_n;t.table=n;t.payer="eosio"_n;});
      };
      const auto& rotation=table("rotation"_n);
      if(!db.find<key_value_object,by_scope_primary>(boost::make_tuple(rotation.id,"rotation"_n.to_uint64_t())))
         db.create<key_value_object>([&](auto& r){r.t_id=rotation.id;r.primary_key="rotation"_n.to_uint64_t();r.payer="eosio"_n;r.value=std::string_view("\x02\0\0\0",4);});
      const auto& candidates=table("rotcands"_n);
      std::string data(114,'\0');const auto owner=fc::raw::pack(voter);
      std::copy(owner.begin(),owner.end(),data.begin());data[73]=1;
      db.create<key_value_object>([&](auto& r){r.t_id=candidates.id;r.primary_key=voter.to_uint64_t();r.payer=voter;r.value=std::string_view(data);});
   }
};
}

BOOST_AUTO_TEST_CASE(system_creation_requires_feature_and_native_parent) {
   system_account_fixture off(4,false);
   // Old unknown-action behavior is replayed, but grants no native capability.
   BOOST_REQUIRE_NO_THROW(off.propose_creation());
   BOOST_CHECK(!find_system_account_proposal(off.chain.control->db()));
   system_account_fixture f;
   const authority key(f.chain.get_public_key("alice"_n,"active"));
   BOOST_CHECK_THROW(f.forward(newaccount{"eosio"_n,"eosio.signup"_n,key,key},"eosio"_n),fc::exception);
   BOOST_CHECK_THROW(f.forward(f.creation_args(),"developer"_n,"upgrade"_n),fc::exception);
   BOOST_CHECK_THROW(f.native(sysfinish{1},"alice"_n),fc::exception);
   BOOST_CHECK(!f.exists());
}

BOOST_AUTO_TEST_CASE(system_creation_validates_exact_manifest_and_fresh_electorate) {
   system_account_fixture f;
   auto args=f.creation_args();
   BOOST_CHECK_THROW(f.native(args,"developer"_n,"upgrade"_n),fc::exception);
   for(auto target:{"alice"_n,"plainaccount"_n,"eosio.prods"_n,"eosio.msig"_n,"eosio.wrap"_n,"eosio.null"_n,"eosio."_n}) {
      auto invalid=args;invalid.target=target;
      BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
   }
   for(int64_t amount:{0ll,-1ll,100000001ll}){auto invalid=args;invalid.ram_payment=amount;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);}
   for(auto payer:{"eosio"_n,"missing"_n,"eosio.token"_n}){auto invalid=args;invalid.resource_payer=payer;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);}
   auto invalid=args;invalid.authority_version=2;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
   invalid=args;invalid.code_bytes=0;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
   invalid=args;invalid.abi_bytes=1024*1024+1;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
   invalid=args;invalid.review_document_hash={};BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
   BOOST_REQUIRE_NO_THROW(f.propose_creation());
   BOOST_CHECK_THROW(f.propose_creation(),fc::exception);
   const auto delay=f.creation().execute_after-f.creation().proposed_at;
#if PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS == 1800
   BOOST_CHECK(delay==fc::seconds(1800));
#else
   BOOST_CHECK(delay==fc::seconds(upgrade_review_seconds_v2));
#endif
}

BOOST_AUTO_TEST_CASE(system_creation_preserves_existing_pending_upgrade) {
   system_account_fixture f;
   f.propose();f.veto(f.nodes[0]);
   // refreshgov changes only its current-voter fields; the original proposal,
   // payload, deadline and veto evidence must survive both lane operations.
   const auto before=fc::raw::pack(f.state());
   f.propose_creation();
   BOOST_CHECK(fc::raw::pack(f.state())==before);
   f.native(syscancel{f.creation().proposal_id},"developer"_n,"upgrade"_n);
   BOOST_CHECK(fc::raw::pack(f.state())==before);
   BOOST_CHECK_EQUAL(f.creation().outcome,2u);
   BOOST_CHECK(!f.creation().pending);
}

BOOST_AUTO_TEST_CASE(system_creation_reviews_sponsor_payload_and_seals_nonprivileged_account) {
   system_account_fixture f;f.propose_creation();
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);
   const auto deadline=f.creation().execute_after;
   f.advance_to(deadline-fc::seconds(1));
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);
   f.chain.produce_blocks(2);
   BOOST_REQUIRE(f.chain.control->pending_block_time()==deadline);
   BOOST_CHECK_THROW(f.execute_creation(false),fc::exception);
   auto bad=f.upgrade_code;bad.push_back(0);
   BOOST_CHECK_THROW(f.execute_creation(true,bad),fc::exception);
   BOOST_CHECK(!f.exists());BOOST_CHECK(f.creation().pending);
   BOOST_REQUIRE_NO_THROW(f.execute_creation());
   BOOST_CHECK(f.exists());BOOST_CHECK(!f.creation().pending);BOOST_CHECK_EQUAL(f.creation().outcome,1u);
   const auto& db=f.chain.control->db();
   const auto& metadata=db.get<account_metadata_object,by_name>("eosio.signup"_n);
   BOOST_CHECK(!metadata.is_privileged());BOOST_CHECK(metadata.code_hash==f.creation().code_hash);
   BOOST_CHECK(is_protected_system_account(db,"eosio.signup"_n));
   const auto& manager=f.chain.control->get_authorization_manager();
   const auto owner=manager.get_permission({"eosio.signup"_n,"owner"_n}).auth.to_authority();
   const auto active=manager.get_permission({"eosio.signup"_n,"active"_n}).auth.to_authority();
   BOOST_CHECK(owner.keys.empty()&&owner.accounts.empty()&&owner.waits.empty()&&owner.threshold==1);
   BOOST_CHECK(active.keys.empty()&&active.waits.empty()&&active.threshold==1&&active.accounts.size()==2);
   BOOST_CHECK_THROW(f.forward(setcode{"eosio.signup"_n,0,0,bad},"eosio.signup"_n),fc::exception);
   BOOST_CHECK_THROW(f.forward(updateauth{"eosio.signup"_n,"active"_n,"owner"_n,authority(f.chain.get_public_key("alice"_n,"active"))},"eosio.signup"_n),fc::exception);
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);
   BOOST_CHECK_THROW(f.native(sysfinish{f.creation().proposal_id},"alice"_n),fc::exception);
}

BOOST_AUTO_TEST_CASE(system_creation_failure_rolls_back_account_protection_and_execution) {
   system_account_fixture f;
   // Hash matches review, but native WASM validation must still fail atomically.
   f.upgrade_code={'b','a','d'};f.propose_creation();
   f.advance_to(f.creation().execute_after);
   const auto original=fc::raw::pack(f.creation());
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);
   BOOST_CHECK(!f.exists());BOOST_CHECK(!is_protected_system_account(f.chain.control->db(),"eosio.signup"_n));
   BOOST_CHECK(fc::raw::pack(f.creation())==original);
}

BOOST_AUTO_TEST_CASE(system_creation_effective_veto_and_cancellation_boundaries) {
   system_account_fixture f;f.propose_creation();
   BOOST_CHECK_THROW(f.veto_creation("alice"_n),fc::exception);
   f.veto_creation(f.nodes[0]);BOOST_CHECK_THROW(f.veto_creation(f.nodes[0]),fc::exception);
   f.veto_creation(f.nodes[1]);f.veto_creation(f.nodes[2]);
   BOOST_CHECK_EQUAL(get_system_account_effective_tally(f.chain.control->db(),f.creation()).vetoes.size(),3u);
   f.advance_to(f.creation().execute_after);
   BOOST_CHECK_THROW(f.veto_creation(f.nodes[3]),fc::exception);
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);BOOST_CHECK(!f.exists());
   const auto id=f.creation().proposal_id;
   const auto old_deadline=f.creation().execute_after;
   BOOST_CHECK_THROW(f.native(syscancel{id},"alice"_n),fc::exception);
   f.native(syscancel{id},"developer"_n,"upgrade"_n);
   f.propose_creation();BOOST_CHECK_EQUAL(f.creation().proposal_id,id+1);
   BOOST_CHECK(f.creation().execute_after>old_deadline&&f.creation().vetoes.empty());
   BOOST_CHECK_THROW(f.native(sysveto{f.nodes[0],id},f.nodes[0]),fc::exception);
}

BOOST_AUTO_TEST_CASE(system_creation_snapshot_restart_and_replay_preserve_both_lanes) {
   system_account_fixture f;f.propose();f.propose_creation();f.veto_creation(f.nodes[0]);
   f.chain.produce_blocks(8);f.chain.control->abort_block();
   const auto expected=fc::raw::pack(f.creation()),upgrade=fc::raw::pack(f.state());
   system_account_proposal_object copy([](auto&){},chainbase::constructor_tag{});
   fc::from_variant(fc::variant(f.creation()),copy);BOOST_CHECK(fc::raw::pack(copy)==expected);
   auto writer=buffered_snapshot_suite::get_writer();f.chain.control->write_snapshot(writer);
   auto snapshot=buffered_snapshot_suite::finalize(writer);
   fc::temp_directory restored_dir;const auto cfg=base_tester::default_config(restored_dir).first;
   tester restored(cfg,f.genesis,call_startup_t::no);restored.close();
   restored.init(cfg,buffered_snapshot_suite::get_reader(snapshot));
   BOOST_CHECK(fc::raw::pack(*find_system_account_proposal(restored.control->db()))==expected);
   BOOST_CHECK(fc::raw::pack(*find_upgrade_governance(restored.control->db()))==upgrade);
   const auto lib_id=f.chain.last_irreversible_block_id();
   const auto source_cfg=base_tester::default_config(f.dir).first;
   f.chain.close();f.chain.open();BOOST_CHECK(fc::raw::pack(f.creation())==expected);f.chain.close();
   fc::temp_directory replay_dir;const auto replay_cfg=base_tester::default_config(replay_dir).first;
   std::filesystem::create_directories(replay_cfg.blocks_dir);
   for(const char* filename:{"blocks.log","blocks.index"})std::filesystem::copy_file(source_cfg.blocks_dir/filename,replay_cfg.blocks_dir/filename);
   tester replay(replay_cfg,f.genesis);
   BOOST_CHECK(replay.head().id()==lib_id);
   BOOST_CHECK(fc::raw::pack(*find_system_account_proposal(replay.control->db()))==expected);
   BOOST_CHECK(fc::raw::pack(*find_upgrade_governance(replay.control->db()))==upgrade);
}

BOOST_AUTO_TEST_CASE(system_creation_native_ban_reader_recomputes_effective_threshold) {
   system_account_fixture f;f.propose_creation();
   f.veto_creation(f.nodes[0]);f.veto_creation(f.nodes[1]);
   f.advance_to(f.creation().execute_after);
   f.simulated_ban_record(f.nodes[3]);
   const auto tally=get_system_account_effective_tally(f.chain.control->db(),f.creation());
   BOOST_CHECK_EQUAL(tally.voters.size(),3u);BOOST_CHECK_EQUAL(tally.vetoes.size(),2u);
   BOOST_CHECK_EQUAL(tally.excluded_banned_voters.size(),1u);
   BOOST_CHECK_THROW(f.veto_creation(f.nodes[3]),fc::exception);
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);BOOST_CHECK(!f.exists());
}

BOOST_AUTO_TEST_CASE(system_creation_native_ban_reader_excludes_veto_and_blocks_empty_electorate) {
   system_account_fixture f(1);f.propose_creation();f.veto_creation(f.nodes[0]);
   f.advance_to(f.creation().execute_after);
   f.simulated_ban_record(f.nodes[0]);
   const auto tally=get_system_account_effective_tally(f.chain.control->db(),f.creation());
   BOOST_CHECK(tally.voters.empty()&&tally.vetoes.empty());
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);BOOST_CHECK(!f.exists());
}

BOOST_AUTO_TEST_CASE(system_creation_activated_snapshot_rejects_downgraded_header) {
   system_account_fixture f;f.propose_creation();f.chain.produce_blocks(8);f.chain.control->abort_block();
   auto writer=variant_snapshot_suite::get_writer();f.chain.control->write_snapshot(writer);
   const auto snapshot=variant_snapshot_suite::finalize(writer);
   auto sections=snapshot["sections"].get_array();bool found=false;
   for(auto& section:sections) {
      if(!section["name"].as_string().ends_with("::chain_snapshot_header"))continue;
      auto rows=section["rows"].get_array();BOOST_REQUIRE_EQUAL(rows.size(),1u);
      rows[0]=fc::mutable_variant_object()("version",10);
      section=fc::mutable_variant_object(section.get_object()).set("rows",std::move(rows));found=true;
   }
   BOOST_REQUIRE(found);
   const auto downgraded=fc::mutable_variant_object(snapshot.get_object()).set("sections",std::move(sections));
   fc::temp_directory dir;const auto cfg=base_tester::default_config(dir).first;
   tester restored(cfg,f.genesis,call_startup_t::no);restored.close();
   BOOST_CHECK_EXCEPTION(restored.init(cfg,variant_snapshot_suite::get_reader(downgraded)),snapshot_exception,
      [](const auto& error){return error.to_detail_string().find("requires snapshot version11")!=std::string::npos;});
}
