// Separate optional target: real, hash-pinned system/token/registration WASM.
// All keys are fresh process-local fixtures; never serialize private keys.
#include "pureeos_governance_tests.cpp"
#include <fc/crypto/base58.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/crypto/hex.hpp>

#ifdef PUREEOS_SIGNUP_REAL_CONTRACTS_DIR
namespace {
struct signup_real_fixture:governance_fixture {
   std::filesystem::path root{PUREEOS_SIGNUP_REAL_CONTRACTS_DIR};
   bytes signup_code,signup_abi;
   signup_real_fixture():governance_fixture(0,false) {
      auto feature=chain.control->get_protocol_feature_manager().get_protocol_feature_set().get_builtin_digest(builtin_protocol_feature_t::pureeos_rotation_context);
      push({action({permission_level{"eosio"_n,"active"_n}},"eosio"_n,"activate"_n,fc::raw::pack(*feature))},"eosio"_n);chain.produce_blocks(2);
      chain.create_accounts({"eosio.stake"_n,"eosio.rex"_n,"eosio.ram"_n,"eosio.ramfee"_n,"eosio.bpay"_n,"eosio.vpay"_n,
         "eosio.saving"_n,"eosio.fees"_n,"eosio.reserv"_n});
      deploy("eosio.token"_n,"eosio.token/eosio.token");
      BOOST_REQUIRE((chain.control->db().get<account_metadata_object,by_name>("eosio.token"_n).code_hash==digest_type("f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c")));
      call("eosio.token"_n,"create"_n,"eosio.token"_n,fc::mutable_variant_object()("issuer","eosio")("maximum_supply","2100000000.0000 AIC"));
      call("eosio.token"_n,"issue"_n,"eosio"_n,fc::mutable_variant_object()("to","eosio")("quantity","100000000.0000 AIC")("memo","isolated fixture"));
      deploy("eosio.bpay"_n,"eosio.bpay/eosio.bpay");
      deploy("eosio.rex"_n,"eosio.system/.rex/rex.results");
      deploy("eosio.reserv"_n,"eosio.system/.powerup/powup.results");
      deploy("eosio"_n,"eosio.system/eosio.system");
      BOOST_REQUIRE((chain.control->db().get<account_metadata_object,by_name>("eosio"_n).code_hash==digest_type("b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170")));
      call("eosio"_n,"init"_n,"eosio"_n,fc::mutable_variant_object()("version",0)("core","4,AIC"));
      call("eosio"_n,"setramrate"_n,"eosio"_n,fc::mutable_variant_object()("bytes_per_block",1024));
      for(auto account:{"eosio.reserv"_n,"developer"_n,"alice"_n})
         call("eosio"_n,"buyram"_n,"eosio"_n,fc::mutable_variant_object()("payer","eosio")("receiver",account)("quant","100.0000 AIC"));
      auto resource=[] {return fc::mutable_variant_object()("current_weight_ratio",fc::variant())("target_weight_ratio",fc::variant())
         ("assumed_stake_weight",fc::variant())("target_timestamp",fc::variant())("exponent",2.0)("decay_secs",86400)("min_price","1.0000 AIC")("max_price","100.0000 AIC");};
      call("eosio"_n,"cfgpowerup"_n,"eosio"_n,fc::mutable_variant_object()("args",fc::mutable_variant_object()("net",resource())("cpu",resource())("powerup_days",30)("min_powerup_fee","0.0001 AIC")));
      for(auto account:{"developer"_n,"alice"_n}) {
         call("eosio.token"_n,"transfer"_n,"eosio"_n,fc::mutable_variant_object()("from","eosio")("to",account)("quantity","1000.0000 AIC")("memo","owned fixture funds"));
         call("eosio"_n,"delegatebw"_n,account,fc::mutable_variant_object()("from",account)("receiver",account)("stake_net_quantity","10.0000 AIC")("stake_cpu_quantity","100.0000 AIC")("transfer",false));
      }
      seal();
      const auto& features=chain.control->get_protocol_feature_manager().get_protocol_feature_set();
      chain.schedule_protocol_features_wo_preactivation({*features.get_builtin_digest(builtin_protocol_feature_t::pureeos_seven_day_governance),*features.get_builtin_digest(builtin_protocol_feature_t::pureeos_system_account_governance)});chain.produce_blocks(2);
      for(auto name:{"syspropose"_n,"syscancel"_n,"sysexec"_n})native(linkauth{"developer"_n,"eosio"_n,name,"upgrade"_n},"developer"_n);
      std::string code;fc::read_file_contents((root/"eosio.signup.wasm").string(),code);signup_code.assign(code.begin(),code.end());
      BOOST_REQUIRE(fc::sha256::hash(signup_code.data(),signup_code.size())==digest_type("996d520ce32c01da431334fc7bf1f03755f4c82f7ae73ddf8b9d931054ae35f3"));
      abi_def abi;fc::from_variant(fc::json::from_file((root/"eosio.signup.abi").string()),abi);signup_abi=fc::raw::pack(abi);
      std::ofstream raw_abi(root/"eosio.signup.raw.abi",std::ios::binary);raw_abi.write(signup_abi.data(),signup_abi.size());BOOST_REQUIRE(raw_abi.good());
   }
   void deploy(account_name account,const std::string& stem) {
      std::string wasm,abi;fc::read_file_contents((root/(stem+".wasm")).string(),wasm);fc::read_file_contents((root/(stem+".abi")).string(),abi);
      chain.set_code(account,std::vector<uint8_t>(wasm.begin(),wasm.end()));chain.set_abi(account,abi);chain.produce_block();
   }
   void call(account_name contract,action_name name,account_name signer,const fc::variant_object& data) {
      chain.push_action(contract,name,signer,data);chain.produce_block();
   }
   const system_account_proposal_object& creation()const{return *find_system_account_proposal(chain.control->db());}
   void propose(account_name target,int64_t payment) {
      syspropose args{target,"developer"_n,payment,1,uint32_t(signup_code.size()),uint32_t(signup_abi.size()),
         fc::sha256::hash(signup_code.data(),signup_code.size()),fc::sha256::hash(signup_abi.data(),signup_abi.size()),fc::sha256::hash("isolated real signup review")};
      push({action({permission_level{"developer"_n,"upgrade"_n}},"eosio"_n,"refreshgov"_n,bytes{}),action({permission_level{"developer"_n,"upgrade"_n}},args)},"developer"_n);
   }
   void execute() {push({action({permission_level{"developer"_n,"upgrade"_n},permission_level{"developer"_n,"active"_n}},sysexec{creation().proposal_id,signup_code,signup_abi})},"developer"_n);}
   int64_t balance(account_name account)const {
      auto row=chain.get_row_by_account("eosio.token"_n,account,"accounts"_n,name{uint64_t(0x434941)});if(row.empty())return 0;
      fc::datastream<const char*> stream(row.data(),row.size());asset value;fc::raw::unpack(stream,value);return value.get_amount();
   }
   void registration(const std::string& memo,const std::string& amount="1.0000 AIC") {
      call("eosio.token"_n,"transfer"_n,"alice"_n,fc::mutable_variant_object()("from","alice")("to","eosio.signup")("quantity",amount)("memo",memo));
   }
   void authorize_new(account_name account,const private_key_type& key) {
      // Registration stake ownership and usable PowerUp work are separate.
      call("eosio"_n,"powerup"_n,"alice"_n,fc::mutable_variant_object()("payer","alice")("receiver",account)("days",30)("net_frac",10000000000000ll)("cpu_frac",10000000000000ll)("max_payment","10.0000 AIC"));
      signed_transaction trx;trx.actions={action({permission_level{account,"active"_n}},"alice"_n,"proof"_n,bytes{})};chain.set_transaction_headers(trx);trx.sign(key,chain.control->get_chain_id());chain.push_transaction(trx);chain.produce_block();
   }
};
std::string modern(const public_key_type& key){return fc::variant(key).as_string();}
std::string legacy_public_text(const public_key_type& key) {
   auto raw=fc::raw::pack(key);BOOST_REQUIRE(raw.size()==34&&raw[0]==0);bytes payload(raw.begin()+1,raw.end());
   auto checksum=fc::ripemd160::hash(payload.data(),payload.size());payload.insert(payload.end(),checksum.data(),checksum.data()+4);
   return "EOS"+fc::to_base58(payload.data(),payload.size(),{});
}
}
BOOST_AUTO_TEST_SUITE(pureeos_signup_real_tests)
BOOST_AUTO_TEST_CASE(genesis_BP_has_only_narrow_system_creation_veto) {
   pureeos_governance_tests::system_account_fixture f;
   f.push({f.refresh({"eosio"_n}),action({permission_level{"developer"_n,"upgrade"_n}},f.creation_args())},"developer"_n);
   BOOST_REQUIRE_NO_THROW(f.native(sysveto{"eosio"_n,f.creation().proposal_id},"eosio"_n,"veto"_n));
   BOOST_CHECK_THROW(f.native(syscancel{f.creation().proposal_id},"eosio"_n,"veto"_n),fc::exception);
   f.advance_to(f.creation().execute_after);BOOST_CHECK_THROW(f.execute_creation(),fc::exception);BOOST_CHECK(!f.exists());
}
BOOST_AUTO_TEST_CASE(pre_feature_v10_snapshot_preserves_vetoed_upgrade3) {
   governance_fixture f(4,false);
   const auto& features=f.chain.control->get_protocol_feature_manager().get_protocol_feature_set();
   const auto rotation=*features.get_builtin_digest(builtin_protocol_feature_t::pureeos_rotation_context);
   f.push({action({permission_level{"eosio"_n,"active"_n}},"eosio"_n,"activate"_n,fc::raw::pack(rotation))},"eosio"_n);f.chain.produce_blocks(2);f.seal();
   f.chain.schedule_protocol_features_wo_preactivation({*features.get_builtin_digest(builtin_protocol_feature_t::pureeos_seven_day_governance)});f.chain.produce_blocks(2);
   for(int i=0;i<2;++i){f.propose();f.native(devcancel{f.state().proposal_id},"developer"_n,"upgrade"_n);}
   f.propose();for(size_t i=0;i<3;++i)f.veto(f.nodes[i]);
   // Pre-feature unknown actions must not begin decoding a new native schema.
   BOOST_REQUIRE_NO_THROW(f.push({action({permission_level{"developer"_n,"active"_n}},"eosio"_n,"syspropose"_n,bytes{char(0xff)})},"developer"_n));
   f.chain.produce_blocks(8);f.chain.control->abort_block();
   const auto expected=fc::raw::pack(f.state());auto writer=variant_snapshot_suite::get_writer();f.chain.control->write_snapshot(writer);
   const auto snapshot=variant_snapshot_suite::finalize(writer);auto sections=snapshot["sections"].get_array();bool found=false;
   for(auto& section:sections)if(section["name"].as_string().ends_with("::chain_snapshot_header")) {
      section=fc::mutable_variant_object(section.get_object()).set("rows",fc::variants{fc::mutable_variant_object()("version",10)});found=true;
   }
   BOOST_REQUIRE(found);const auto legacy_snapshot=fc::mutable_variant_object(snapshot.get_object()).set("sections",std::move(sections));
   fc::temp_directory dir;const auto cfg=base_tester::default_config(dir).first;tester restored(cfg,f.genesis,call_startup_t::no);restored.close();
   BOOST_REQUIRE_NO_THROW(restored.init(cfg,variant_snapshot_suite::get_reader(legacy_snapshot)));
   BOOST_CHECK(restored.control->get_chain_id()==f.chain.control->get_chain_id());
   BOOST_CHECK(fc::raw::pack(*find_upgrade_governance(restored.control->db()))==expected);
   BOOST_CHECK(find_system_account_proposal(restored.control->db())==nullptr);
}
BOOST_AUTO_TEST_CASE(actual_native_creation_K1_R1_legacy_and_atomic_failures) {
   signup_real_fixture f;f.propose("eosio.signup"_n,10000);
   BOOST_CHECK_THROW(f.execute(),fc::exception);f.advance_to(f.creation().execute_after);BOOST_REQUIRE_NO_THROW(f.execute());f.chain.produce_block();
   BOOST_CHECK((!f.chain.control->db().get<account_metadata_object,by_name>("eosio.signup"_n).is_privileged()));
   auto k1=private_key_type::generate(),r1=private_key_type::generate_r1(),owner=private_key_type::generate();
   const auto p1=modern(k1.get_public_key()),p2=modern(r1.get_public_key()),po=modern(owner.get_public_key());
   BOOST_REQUIRE(p1.starts_with("PUB_K1_")&&p2.starts_with("PUB_R1_"));
   f.registration("signupkeyaaa-"+p1);f.registration("signupkeyaab-"+p2);
   f.registration("signupkeyaac-"+po+"-"+p2);f.registration("signupkeyaad "+legacy_public_text(k1.get_public_key()));
   for(auto account:{"signupkeyaaa"_n,"signupkeyaab"_n,"signupkeyaac"_n,"signupkeyaad"_n}) {
      const auto& auth=f.chain.control->get_authorization_manager();
      const auto active=auth.get_permission({account,"active"_n}).auth.to_authority();
      BOOST_REQUIRE_EQUAL(active.keys.size(),1u);BOOST_CHECK_EQUAL(active.threshold,1u);BOOST_CHECK(active.accounts.empty());
      BOOST_CHECK_EQUAL(f.balance(account),0); // Remainder went to RAM, not liquid tokens.
      const auto resources=f.chain.get_row_by_account("eosio"_n,account,"userres"_n,account);BOOST_REQUIRE(!resources.empty());
      fc::datastream<const char*> res_stream(resources.data(),resources.size());account_name resource_owner;asset net,cpu;int64_t ram_bytes=0;
      fc::raw::unpack(res_stream,resource_owner);fc::raw::unpack(res_stream,net);fc::raw::unpack(res_stream,cpu);fc::raw::unpack(res_stream,ram_bytes);
      BOOST_CHECK(resource_owner==account);BOOST_CHECK_EQUAL(net.get_amount(),1000);BOOST_CHECK_EQUAL(cpu.get_amount(),1000);BOOST_CHECK_GE(ram_bytes,4096);
      const auto delegated=f.chain.get_row_by_account("eosio"_n,account,"delband"_n,account);BOOST_REQUIRE(!delegated.empty());
      fc::datastream<const char*> stake_stream(delegated.data(),delegated.size());account_name from,to;
      fc::raw::unpack(stake_stream,from);fc::raw::unpack(stake_stream,to);BOOST_CHECK(from==account&&to==account);
   }
   const auto& auth=f.chain.control->get_authorization_manager();
   BOOST_CHECK(auth.get_permission({"signupkeyaac"_n,"owner"_n}).auth.to_authority().keys[0].key==owner.get_public_key());
   BOOST_CHECK(auth.get_permission({"signupkeyaac"_n,"active"_n}).auth.to_authority().keys[0].key==r1.get_public_key());
   f.authorize_new("signupkeyaaa"_n,k1);f.authorize_new("signupkeyaab"_n,r1);f.authorize_new("signupkeyaac"_n,r1);f.authorize_new("signupkeyaad"_n,k1);
   signed_transaction owner_proof;
   owner_proof.actions={action({permission_level{"signupkeyaac"_n,"owner"_n}},updateauth{"signupkeyaac"_n,"audit"_n,"active"_n,authority(k1.get_public_key())})};
   f.chain.set_transaction_headers(owner_proof);owner_proof.sign(owner,f.chain.control->get_chain_id());BOOST_REQUIRE_NO_THROW(f.chain.push_transaction(owner_proof));f.chain.produce_block();
   auto before=f.balance("alice"_n);auto broken=p1;broken.back()=broken.back()=='1'?'2':'1';
   BOOST_CHECK_THROW(f.registration("signupkeyaae-"+broken),fc::exception);BOOST_CHECK_EQUAL(f.balance("alice"_n),before);
   BOOST_CHECK_THROW(f.registration("signupkeyaaa-"+p1),fc::exception);BOOST_CHECK_EQUAL(f.balance("alice"_n),before);
   BOOST_CHECK_THROW(f.registration("signupkeyaaf-"+p1,"0.2001 AIC"),fc::exception);BOOST_CHECK_EQUAL(f.balance("alice"_n),before);
   BOOST_CHECK((!f.chain.control->db().find<account_object,by_name>("signupkeyaae"_n)));
   BOOST_CHECK((!f.chain.control->db().find<account_object,by_name>("signupkeyaaf"_n)));
   const auto fake="(module (import \"env\" \"require_recipient\" (func $notify (param i64))) (memory 1) (func (export \"apply\") (param i64 i64 i64) (call $notify (i64.const "+std::to_string("eosio.signup"_n.to_uint64_t())+"))))";
   f.chain.set_code("alice"_n,fake.c_str());f.chain.produce_block();
   const auto fake_data=fc::raw::pack(std::make_tuple("alice"_n,"eosio.signup"_n,asset::from_string("1.0000 AIC"),std::string("signupkeyaag-")+p1));
   BOOST_CHECK_THROW(f.push({action({permission_level{"alice"_n,"active"_n}},"alice"_n,"transfer"_n,fake_data)},"alice"_n),fc::exception);
   BOOST_CHECK_EQUAL(f.balance("alice"_n),before);BOOST_CHECK((!f.chain.control->db().find<account_object,by_name>("signupkeyaag"_n)));
   auto sponsor=f.balance("developer"_n);f.propose("eosio.failrm"_n,1);f.advance_to(f.creation().execute_after);
   BOOST_CHECK_THROW(f.execute(),fc::exception);BOOST_CHECK(f.creation().pending);BOOST_CHECK_EQUAL(f.balance("developer"_n),sponsor);
   BOOST_CHECK((!f.chain.control->db().find<account_object,by_name>("eosio.failrm"_n)));
   BOOST_CHECK(!is_protected_system_account(f.chain.control->db(),"eosio.failrm"_n));
   BOOST_TEST_MESSAGE("Real pinned WASM: native nonprivileged creation, K1/R1/legacy registration, explicit PowerUp signing and atomic failure paths; isolated controlled tester clock only.");
}
BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE(pureeos_signup_wire_tests)
BOOST_AUTO_TEST_CASE(unsigned_helper_matches_native_action_serialization) {
   const std::filesystem::path root{PUREEOS_SIGNUP_REAL_CONTRACTS_DIR};
   std::string wasm,raw,review;fc::read_file_contents((root/"eosio.signup.wasm").string(),wasm);fc::read_file_contents((root/"eosio.signup.raw.abi").string(),raw);fc::read_file_contents((root/"wire-review.md").string(),review);
   const auto bundle=fc::json::from_file((root/"unsigned-candidate.json").string());
   syspropose proposal{"eosio.signup"_n,"developer"_n,10000,1,uint32_t(wasm.size()),uint32_t(raw.size()),fc::sha256::hash(wasm.data(),wasm.size()),fc::sha256::hash(raw.data(),raw.size()),fc::sha256::hash(review.data(),review.size())};
   const auto proposed=fc::raw::pack(proposal);BOOST_CHECK_EQUAL(bundle["proposal_actions"].get_array().at(1)["data"].as_string(),fc::to_hex(proposed.data(),proposed.size()));
   const auto execution=fc::raw::pack(sysexec{1,bytes(wasm.begin(),wasm.end()),bytes(raw.begin(),raw.end())});
   BOOST_CHECK_EQUAL(bundle["execution_actions"].get_array().at(0)["data"].as_string(),fc::to_hex(execution.data(),execution.size()));
   size_t i=0;for(auto name:{"syspropose"_n,"syscancel"_n,"sysexec"_n}) {
      const auto linked=fc::raw::pack(linkauth{"developer"_n,"eosio"_n,name,"upgrade"_n});
      BOOST_CHECK_EQUAL(bundle["permission_setup_actions"].get_array().at(i++)["data"].as_string(),fc::to_hex(linked.data(),linked.size()));
   }
}
BOOST_AUTO_TEST_SUITE_END()
#endif
