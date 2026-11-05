// Native boundary tests use the deliberately permissive system fixture above.
// Token transfer/buyram are no-ops here; these cases cannot qualify economics,
// resource admission, or the production registration contract.
namespace {
digest_type fixture_init_commitment(const digest_type& human,const bytes& data) {
   const std::string domain="PureEOS creation initializer v1";bytes value(domain.begin(),domain.end());value.push_back('\0');
   const auto review=fc::raw::pack(human),payload=fc::raw::pack(fc::sha256::hash(data.data(),data.size()));
   value.insert(value.end(),review.begin(),review.end());value.insert(value.end(),payload.begin(),payload.end());
   return fc::sha256::hash(value.data(),value.size());
}
struct system_account_fixture : governance_fixture {
   system_account_fixture(size_t voters=4, bool activate=true,const std::string& system_wat=fixture_wat()):governance_fixture(voters,false,system_wat) {
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
   transaction_trace_ptr execute_creation(bool sponsor=true,bytes code={}) {
      std::vector<permission_level> auth{{"developer"_n,"upgrade"_n}};
      if(sponsor)auth.push_back({"developer"_n,"active"_n});
      try {return push({action(auth,sysexec{creation().proposal_id,code.empty()?upgrade_code:code,upgrade_abi})},"developer"_n);}
      catch(const fc::exception& error){BOOST_TEST_MESSAGE(error.to_detail_string());throw;}
   }
   void veto_creation(account_name voter) {native(sysveto{voter,creation().proposal_id},voter);}
   bool exists(account_name name="eosio.signup"_n)const{return chain.control->db().find<account_object,by_name>(name);}
   void reserve_short_name(account_name target,int64_t bid=1000) {
      // Native reservation boundary only; real bidname is exercised separately.
      auto& db=const_cast<eosio::chain::database&>(chain.control->db());
      const auto* table=db.find<table_id_object,by_code_scope_table>(boost::make_tuple("eosio"_n,"eosio"_n,"namebids"_n));
      if(!table)table=&db.create<table_id_object>([](auto& t){t.code="eosio"_n;t.scope="eosio"_n;t.table="namebids"_n;t.payer="eosio"_n;});
      const auto data=fc::raw::pack(std::make_tuple(target,"alice"_n,bid,chain.control->pending_block_time()));
      db.create<key_value_object>([&](auto& r){r.t_id=table->id;r.primary_key=target.to_uint64_t();r.payer="alice"_n;r.value=std::string_view(data.data(),data.size());});
   }
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
   BOOST_CHECK_THROW(f.native(sysstage{1,{},{}},"alice"_n),fc::exception);
   BOOST_CHECK_THROW(f.forward(sysstage{1,{},{}},"eosio"_n),fc::exception);
   BOOST_CHECK_THROW(f.native(sysfinish{1},"alice"_n),fc::exception);
   BOOST_CHECK(!f.exists());
}

BOOST_AUTO_TEST_CASE(system_creation_validates_exact_manifest_and_fresh_electorate) {
   system_account_fixture f;
   auto args=f.creation_args();
   BOOST_CHECK_THROW(f.native(args,"developer"_n,"upgrade"_n),fc::exception);
   for(auto target:{"alice"_n,"plainaccount"_n,"eosio.dev"_n,"eosio.prods"_n,"eosio.msig"_n,"eosio.wrap"_n,"eosio.null"_n,"eosio."_n}) {
      auto invalid=args;invalid.target=target;
      BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
   }
   for(int64_t amount:{0ll,-1ll,100000001ll}){auto invalid=args;invalid.ram_payment=amount;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);}
   for(auto payer:{"eosio"_n,"missing"_n,"eosio.token"_n}){auto invalid=args;invalid.resource_payer=payer;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);}
   auto invalid=args;invalid.authority_version=3;BOOST_CHECK_THROW(f.propose_creation(invalid),fc::exception);
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

BOOST_AUTO_TEST_CASE(system_creation_short_services_share_review_sealing_and_namespace_boundaries) {
   for(auto target:{"a"_n,"usd"_n,"abcdefghijk"_n}) {
      system_account_fixture f;auto args=f.creation_args();args.target=target;
      f.propose_creation(args);BOOST_CHECK_THROW(f.execute_creation(),fc::exception);
      f.advance_to(f.creation().execute_after);BOOST_REQUIRE_NO_THROW(f.execute_creation());
      BOOST_CHECK(f.exists(target));BOOST_CHECK(!f.creation().pending);
      BOOST_CHECK(is_protected_system_account(f.chain.control->db(),target));
      BOOST_CHECK((!f.chain.control->db().get<account_metadata_object,by_name>(target).is_privileged()));
      const auto owner=f.chain.control->get_authorization_manager().get_permission({target,"owner"_n}).auth.to_authority();
      BOOST_CHECK(owner.keys.empty()&&owner.accounts.empty()&&owner.waits.empty()&&owner.threshold==1);
   }
   system_account_fixture f;
   for(auto target:{""_n,"eosio"_n,"alice"_n,"plainaccount"_n,"foo.bar"_n,"usd.foo"_n,"eosio.prods"_n}) {
      auto args=f.creation_args();args.target=target;BOOST_CHECK_THROW(f.propose_creation(args),fc::exception);
   }
   BOOST_CHECK(!find_system_account_proposal(f.chain.control->db()));
}

BOOST_AUTO_TEST_CASE(system_creation_short_services_do_not_take_pending_or_awarded_auction_names) {
   for(int64_t bid:{1000ll,-1000ll}) {
      system_account_fixture f;f.reserve_short_name("usd"_n,bid);auto args=f.creation_args();args.target="usd"_n;
      BOOST_CHECK_THROW(f.propose_creation(args),fc::exception);BOOST_CHECK(!f.exists("usd"_n));
      BOOST_CHECK(!find_system_account_proposal(f.chain.control->db()));
   }
   system_account_fixture f;auto args=f.creation_args();args.target="usd"_n;f.propose_creation(args);
   f.reserve_short_name("usd"_n);f.advance_to(f.creation().execute_after);
   const auto before=fc::raw::pack(f.creation());
   BOOST_CHECK_EXCEPTION(f.execute_creation(),action_validate_exception,[](const auto& e){return e.to_detail_string().find("existing auction bid")!=std::string::npos;});
   BOOST_CHECK(!f.exists("usd"_n));BOOST_CHECK(!is_protected_system_account(f.chain.control->db(),"usd"_n));
   BOOST_CHECK(fc::raw::pack(f.creation())==before);
}

BOOST_AUTO_TEST_CASE(system_creation_initializer_commitment_requires_exact_data_and_review) {
   system_account_fixture f;const bytes data{1,2,3};const auto human=fc::sha256::hash("reviewed target initialization");
   auto args=f.creation_args();args.authority_version=2;args.review_document_hash=fixture_init_commitment(human,data);
   f.propose_creation(args);f.advance_to(f.creation().execute_after);
   BOOST_CHECK_THROW(f.execute_creation(),fc::exception);
   auto execute=[&](bytes payload,digest_type review) {
      return f.push({action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},
         sysexec{f.creation().proposal_id,f.upgrade_code,f.upgrade_abi,std::move(payload),review})},"developer"_n);
   };
   BOOST_CHECK_THROW(execute(bytes{1,2,4},human),fc::exception);
   BOOST_CHECK_THROW(execute(data,fc::sha256::hash("different review")),fc::exception);
   transaction_trace_ptr trace;BOOST_REQUIRE_NO_THROW(trace=execute(data,human));
   uint32_t stage=0,init=0,finish=0;size_t count=0;
   for(const auto& t:trace->action_traces)if(t.receiver==t.act.account) {
      if(t.act.name=="sysstage"_n)stage=t.action_ordinal.value;
      if(t.act.account=="eosio.signup"_n&&t.act.name=="init"_n) {
         ++count;init=t.action_ordinal.value;BOOST_CHECK_EQUAL(t.creator_action_ordinal.value,stage);
         BOOST_CHECK(t.act.data==data);BOOST_CHECK((t.act.authorization==std::vector<permission_level>{{"eosio.signup"_n,"active"_n}}));
      }
      if(t.act.name=="sysfinish"_n)finish=t.action_ordinal.value;
   }
   BOOST_CHECK_EQUAL(count,1u);BOOST_CHECK(stage>0&&init>stage&&finish>init);BOOST_CHECK(!f.creation().pending);
}

BOOST_AUTO_TEST_CASE(system_creation_initializer_bounds_and_version1_forbid_unreviewed_calls) {
   const auto human=fc::sha256::hash("reviewed initialization bounds");
   system_account_fixture plain;plain.propose_creation();plain.advance_to(plain.creation().execute_after);
   BOOST_CHECK_THROW(plain.push({action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},
      sysexec{plain.creation().proposal_id,plain.upgrade_code,plain.upgrade_abi,bytes{1},human})},"developer"_n),fc::exception);
   BOOST_CHECK(!plain.exists()&&plain.creation().pending);
   system_account_fixture large;const bytes payload(4097,'x');auto args=large.creation_args();args.authority_version=2;
   args.review_document_hash=fixture_init_commitment(human,payload);large.propose_creation(args);large.advance_to(large.creation().execute_after);
   BOOST_CHECK_EXCEPTION(large.push({action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},
      sysexec{large.creation().proposal_id,large.upgrade_code,large.upgrade_abi,payload,human})},"developer"_n),action_validate_exception,
      [](const auto& e){return e.to_detail_string().find("4096bytes")!=std::string::npos;});
   BOOST_CHECK(!large.exists()&&large.creation().pending);BOOST_CHECK(!is_protected_system_account(large.chain.control->db(),"eosio.signup"_n));
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
   transaction_trace_ptr trace;
   BOOST_REQUIRE_NO_THROW(trace=f.execute_creation());
   uint32_t stage=0;size_t stages=0,finishes=0;std::vector<action_name> root_children;
   const auto root=trace->action_traces.front().action_ordinal.value;
   for(const auto& t:trace->action_traces) {
      if(t.receiver!=t.act.account)continue;
      if(t.creator_action_ordinal.value==root)root_children.push_back(t.act.name);
      if(t.act.name=="sysstage"_n){stage=t.action_ordinal.value;++stages;}
   }
   BOOST_REQUIRE_EQUAL(stages,1u);
   BOOST_REQUIRE((root_children==std::vector<action_name>{"newaccount"_n,"sysstage"_n}));
   for(const auto& t:trace->action_traces)if(t.receiver==t.act.account &&
       (t.act.name=="buyram"_n || t.act.name=="setcode"_n || t.act.name=="setabi"_n || t.act.name=="sysfinish"_n)) {
      BOOST_CHECK_EQUAL(t.creator_action_ordinal.value,stage);
      if(t.act.name=="sysfinish"_n)++finishes;
   }
   BOOST_CHECK_EQUAL(finishes,1u);
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

BOOST_AUTO_TEST_CASE(system_creation_internal_stage_preserves_pre_feature_and_inline_authorization_checks) {
   system_account_fixture off(4,false);
   BOOST_REQUIRE_NO_THROW(off.push({action({permission_level{"alice"_n,"active"_n}},"eosio"_n,"sysstage"_n,bytes{char(0xff)})},"alice"_n));
   BOOST_CHECK(!find_system_account_proposal(off.chain.control->db()));
   system_account_fixture f;f.propose_creation();
   for(const auto& authorization:{permission_level{"missing"_n,"active"_n},permission_level{"alice"_n,"missing"_n}}) {
      action child({authorization},sysstage{f.creation().proposal_id,{}, {}});
      BOOST_CHECK_EXCEPTION(f.push({action({permission_level{"alice"_n,"active"_n}},"eosio"_n,"forward"_n,fc::raw::pack(child))},"alice"_n),
         action_validate_exception,[](const auto& e){return e.to_detail_string().find("does not exist")!=std::string::npos ||
            e.to_detail_string().find("non-existent permission")!=std::string::npos;});
   }
   BOOST_CHECK(f.creation().pending&&!f.exists());
}

BOOST_AUTO_TEST_CASE(system_creation_rejects_nested_and_duplicate_native_continuations_atomically) {
   // Hostile TEST system code tries callbacks from newaccount, the root, or
   // the stage itself. Real missing-actor/permission checks remain enabled.
   size_t scenario=0;
   for(auto trigger:{"newaccount"_n,"sysexec"_n,"sysstage"_n,"sysexec"_n}) {
      auto wat=fixture_wat();
      const auto actor=scenario==3?"eosio.signup"_n:"eosio"_n;
      const auto packed=fc::raw::pack(action({permission_level{actor,"active"_n}},sysstage{1,{}, {}}));
      std::string escaped;constexpr char hex[]="0123456789abcdef";
      for(unsigned char byte:packed){escaped+='\\';escaped+=hex[byte>>4];escaped+=hex[byte&15];}
      wat.insert(wat.find("     (func (export \"apply\")"),"     (data (i32.const 1024) \""+escaped+"\")\n");
      const auto hook="       (if (i64.eq (get_local $action) (i64.const "+std::to_string(trigger.to_uint64_t())+")) "
         "(then (call $send (i32.const 1024) (i32.const "+std::to_string(packed.size())+"))))\n";
      const auto marker="       (if (i64.eq (get_local $action) (i64.const "+std::to_string("privilege"_n.to_uint64_t());
      wat.insert(wat.find(marker),hook);
      system_account_fixture f(4,true,wat);f.propose();f.veto(f.nodes[0]);f.propose_creation();f.advance_to(f.creation().execute_after);
      const auto before=fc::raw::pack(f.creation()),gov=fc::raw::pack(f.state());
      if(scenario==3) {
         // The root has armed a creation capability, but the queued newaccount
         // has not run. Even this exact pending target must fail actor checks.
         BOOST_CHECK_EXCEPTION(f.execute_creation(),action_validate_exception,[](const auto& e){
            return e.to_detail_string().find("authorizing actor eosio.signup does not exist")!=std::string::npos;});
      } else {BOOST_CHECK_THROW(f.execute_creation(),fc::exception);}
      BOOST_CHECK(!f.exists()&&!is_protected_system_account(f.chain.control->db(),"eosio.signup"_n));
      BOOST_CHECK(fc::raw::pack(f.creation())==before&&fc::raw::pack(f.state())==gov);
      ++scenario;
   }
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

namespace {
std::vector<std::pair<uint64_t,bytes>> creation_records(const system_account_fixture& f) {
   std::vector<std::pair<uint64_t,bytes>> rows;
   for(const auto& s:f.chain.control->db().get_index<system_account_proposal_index,by_id>())
      rows.emplace_back(s.proposal_id,fc::raw::pack(s));
   return rows;
}
const system_account_proposal_object& creation_by_id(const system_account_fixture& f,uint64_t id) {
   const auto* p=find_system_account_proposal(f.chain.control->db(),id);BOOST_REQUIRE(p);return *p;
}
void propose_four(system_account_fixture& f) {
   std::vector<action> actions;
   for(auto target:{"eosio.key"_n,"usd"_n,"cny"_n,"eosio.signup"_n}) {
      actions.push_back(f.refresh(f.nodes));
      actions.emplace_back(std::vector<permission_level>{{"developer"_n,"upgrade"_n}},f.creation_args(target));
   }
   f.push(std::move(actions),"developer"_n);
}
transaction_trace_ptr execute_id(system_account_fixture& f,uint64_t id) {
   return f.push({action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},
      sysexec{id,f.upgrade_code,f.upgrade_abi})},"developer"_n);
}
}

BOOST_AUTO_TEST_CASE(system_creation_parallel_four_have_independent_frozen_reviews_and_bounded_slots) {
   system_account_fixture f;f.propose();f.veto(f.nodes[0]);const auto legacy=fc::raw::pack(f.state());
   BOOST_REQUIRE_NO_THROW(propose_four(f));BOOST_CHECK_EQUAL(f.creation().proposal_id,4u);
   const auto deadline=creation_by_id(f,1).execute_after;
   BOOST_CHECK_EQUAL(creation_records(f).size(),4u);
   for(uint64_t id=1;id<=4;++id) {
      const auto& s=creation_by_id(f,id);BOOST_CHECK(s.pending&&s.execute_after==deadline);
      BOOST_CHECK(s.proposal_voters==creation_by_id(f,1).proposal_voters);
      BOOST_CHECK(s.execute_after-s.proposed_at==fc::seconds(PUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS));
      BOOST_CHECK_THROW(execute_id(f,id),fc::exception);
   }
   const auto before=creation_records(f);
   BOOST_CHECK_THROW(f.propose_creation(f.creation_args("gold"_n)),fc::exception);
   BOOST_CHECK_THROW(f.propose_creation(f.creation_args("usd"_n)),fc::exception);
   BOOST_CHECK(creation_records(f)==before&&fc::raw::pack(f.state())==legacy);
}

BOOST_AUTO_TEST_CASE(system_creation_parallel_veto_cancel_and_execution_are_per_id) {
   system_account_fixture f;propose_four(f);
   for(size_t i=0;i<3;++i)f.native(sysveto{f.nodes[i],1},f.nodes[i]);
   f.native(sysveto{f.nodes[0],3},f.nodes[0]);
   BOOST_CHECK_EQUAL(creation_by_id(f,1).vetoes.size(),3u);
   BOOST_CHECK(creation_by_id(f,2).vetoes.empty()&&creation_by_id(f,4).vetoes.empty());
   f.advance_to(creation_by_id(f,1).execute_after);
   BOOST_CHECK_THROW(execute_id(f,1),fc::exception);
   const auto one=fc::raw::pack(creation_by_id(f,1)),four=fc::raw::pack(creation_by_id(f,4));
   BOOST_REQUIRE_NO_THROW(execute_id(f,2));BOOST_CHECK(f.exists("usd"_n));
   BOOST_CHECK(!find_system_account_proposal(f.chain.control->db(),2));
   BOOST_CHECK(fc::raw::pack(creation_by_id(f,1))==one&&fc::raw::pack(creation_by_id(f,4))==four);
   f.native(syscancel{3},"developer"_n,"upgrade"_n);
   BOOST_CHECK(!find_system_account_proposal(f.chain.control->db(),3));
   f.propose_creation(f.creation_args("cny"_n));BOOST_CHECK_EQUAL(f.creation().proposal_id,5u);
   BOOST_CHECK(f.creation().vetoes.empty());BOOST_CHECK_THROW(execute_id(f,5),fc::exception);
   BOOST_CHECK_THROW(f.native(sysveto{f.nodes[0],2},f.nodes[0]),fc::exception);
   BOOST_CHECK_THROW(f.native(syscancel{4},"alice"_n),fc::exception);
   BOOST_REQUIRE_NO_THROW(execute_id(f,4));BOOST_CHECK(f.exists("eosio.signup"_n));
   BOOST_CHECK(creation_by_id(f,1).pending&&creation_by_id(f,5).pending);
}

BOOST_AUTO_TEST_CASE(system_creation_parallel_atomic_failures_and_completed_state_pruning) {
   system_account_fixture f;propose_four(f);f.advance_to(creation_by_id(f,4).execute_after);
   const auto before=creation_records(f);const auto legacy=fc::raw::pack(f.state());
   auto bad=f.upgrade_code;bad.push_back(0);
   BOOST_CHECK_THROW(f.push({action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},
      sysexec{1,bad,f.upgrade_abi})},"developer"_n),fc::exception);
   BOOST_CHECK_THROW(f.push({
      action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},sysexec{1,f.upgrade_code,f.upgrade_abi}),
      action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},sysexec{2,f.upgrade_code,f.upgrade_abi})},"developer"_n),fc::exception);
   BOOST_CHECK(creation_records(f)==before&&fc::raw::pack(f.state())==legacy);
   BOOST_CHECK(!f.exists("eosio.key"_n)&&!f.exists("usd"_n));
   for(uint64_t id=1;id<=4;++id)BOOST_REQUIRE_NO_THROW(execute_id(f,id));
   BOOST_CHECK_EQUAL(creation_records(f).size(),1u);BOOST_CHECK_EQUAL(f.creation().proposal_id,4u);
   BOOST_CHECK(!f.creation().pending&&f.creation().outcome==1);
   f.propose_creation(f.creation_args("gold"_n));BOOST_CHECK_EQUAL(f.creation().proposal_id,5u);
   BOOST_CHECK_EQUAL(creation_records(f).size(),1u);BOOST_CHECK(f.creation().pending);
}

BOOST_AUTO_TEST_CASE(system_creation_parallel_snapshot_restart_and_replay_keep_all_pending_ids) {
   system_account_fixture f;propose_four(f);f.native(sysveto{f.nodes[0],2},f.nodes[0]);
   f.chain.produce_blocks(8);f.chain.control->abort_block();const auto expected=creation_records(f);
   auto writer=buffered_snapshot_suite::get_writer();f.chain.control->write_snapshot(writer);
   auto snapshot=buffered_snapshot_suite::finalize(writer);
   fc::temp_directory restored_dir;const auto cfg=base_tester::default_config(restored_dir).first;
   tester restored(cfg,f.genesis,call_startup_t::no);restored.close();restored.init(cfg,buffered_snapshot_suite::get_reader(snapshot));
   for(const auto& [id,packed]:expected) {
      const auto* p=find_system_account_proposal(restored.control->db(),id);BOOST_REQUIRE(p);BOOST_CHECK(fc::raw::pack(*p)==packed);
   }
   const auto lib_id=f.chain.last_irreversible_block_id();const auto source_cfg=base_tester::default_config(f.dir).first;
   f.chain.close();f.chain.open();BOOST_CHECK(creation_records(f)==expected);f.chain.close();
   fc::temp_directory replay_dir;const auto replay_cfg=base_tester::default_config(replay_dir).first;
   std::filesystem::create_directories(replay_cfg.blocks_dir);
   for(const char* name:{"blocks.log","blocks.index"})std::filesystem::copy_file(source_cfg.blocks_dir/name,replay_cfg.blocks_dir/name);
   tester replay(replay_cfg,f.genesis);BOOST_CHECK(replay.head().id()==lib_id);
   for(const auto& [id,packed]:expected) {
      const auto* p=find_system_account_proposal(replay.control->db(),id);BOOST_REQUIRE(p);BOOST_CHECK(fc::raw::pack(*p)==packed);
   }
}
