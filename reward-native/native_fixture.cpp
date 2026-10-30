#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <eosio/chain/upgrade_governance.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/peer_keys_db.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/json.hpp>
#include <fc/io/fstream.hpp>
#include <fstream>
#include <filesystem>
#include <set>
#include <sys/stat.h>
using namespace eosio::chain;
using namespace eosio::testing;
namespace fs=std::filesystem;
namespace {
std::vector<uint8_t> file_bytes(const fs::path& p) {
 std::ifstream f(p,std::ios::binary);BOOST_REQUIRE_MESSAGE(f.good(),"Missing input "<<p);
 return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
}
BOOST_AUTO_TEST_CASE(bp_discovery_native_fixture) {
 umask(0077);
 const fs::path out="/campaign/fixture";
 fs::create_directories(out);
 auto g=base_tester::default_genesis();
 g.initial_timestamp=fc::time_point::now()-fc::hours(6);
 g.initial_configuration=fc::json::from_file("/campaign/input/chain-parameters.json").as<chain_config>();
 auto [ep,eu,ez]=get_bls_key("eosio"_n);
 g.initial_savanna.emplace();g.initial_savanna->version=2;
 g.initial_savanna->finalizer_key=eu;g.initial_savanna->proof_of_possession=ez;
 fc::temp_directory temp;
 auto cfg=base_tester::default_config(temp).first;
 cfg.wasm_runtime=wasm_interface::vm_type::eos_vm_jit;
 cfg.state_size=uint64_t(4096)*1024*1024;
 tester t(cfg,g);
 std::vector<name> bps;for(unsigned i=0;i<30;++i){std::string tail="aa";tail[0]+=i/26;tail[1]+=i%26;bps.emplace_back("bpa"+tail);}
 auto signers=bps;signers.push_back("eosio"_n);t.set_node_finalizers(signers);
 t.produce_blocks(2);
 t.create_accounts({"eosio.token"_n,"eosio.stake"_n,"eosio.rex"_n,"eosio.ram"_n,"eosio.ramfee"_n,
  "eosio.bpay"_n,"eosio.vpay"_n,"eosio.saving"_n,"eosio.fees"_n,"eosio.reserv"_n,"developer"_n,"tokenone"_n});
 t.create_accounts(bps);
 std::vector<name> users;
 for(int i=0;i<20;++i) {std::string n(2,'a');n[0]+=i/26;n[1]+=i%26;
  users.emplace_back("src"+n);users.emplace_back("dst"+n);t.create_accounts({users[users.size()-2],users.back()});}
 t.produce_blocks(2);
 uint32_t nonce=0;
 auto submit=[&](std::vector<action> acts,const std::vector<std::pair<name,std::string>>& auth) {
  signed_transaction trx;trx.actions=std::move(acts);t.set_transaction_headers(trx,60+(++nonce)%1000);
  if(t.control->is_building_block())trx.expiration=fc::time_point_sec(t.control->pending_block_time()+fc::seconds(60+nonce%1000));
  for(auto [who,role]:auth)trx.sign(t.get_private_key(who,role),t.control->get_chain_id());
  auto r=t.push_transaction(trx,fc::time_point::maximum(),0);BOOST_REQUIRE(!r->except);return r;
 };
 auto act=[&](name code,name action_name,name actor,const fc::variant_object& v) {
  return t.get_action(code,action_name,{{actor,"active"_n}},v);
 };
 auto call=[&](name code,name action_name,name actor,const fc::variant_object& v) {
  return submit({act(code,action_name,actor,v)},{{actor,"active"}});
 };
 auto deploy=[&](name who,const fs::path& base) {
  t.set_code(who,file_bytes(base.string()+".wasm"));
  std::string abi;fc::read_file_contents(base.string()+".abi",abi);t.set_abi(who,abi);t.produce_block();
 };
 const fs::path released="/work/contracts-new";
 deploy("eosio"_n,released/"eosio.bios/eosio.bios");
 for(auto feature:{builtin_protocol_feature_t::pureeos_rotation_context,builtin_protocol_feature_t::disable_deferred_trxs_stage_1,
                  builtin_protocol_feature_t::disable_deferred_trxs_stage_2})
  if(!t.control->is_builtin_activated(feature)){t.activate_builtin_protocol_features({feature});t.produce_blocks(2);}
 BOOST_REQUIRE(!t.control->is_builtin_activated(builtin_protocol_feature_t::sync_call));
 deploy("eosio.token"_n,released/"eosio.token/eosio.token");
 call("eosio.token"_n,"create"_n,"eosio.token"_n,fc::mutable_variant_object()("issuer","eosio")("maximum_supply","2000000000.0000 SYS"));
 call("eosio.token"_n,"issue"_n,"eosio"_n,fc::mutable_variant_object()("to","eosio")("quantity","1000000000.0000 SYS")("memo","BP discovery fixture"));
 deploy("eosio"_n,fs::path("/campaign/contract/eosio.system"));
 call("eosio"_n,"init"_n,"eosio"_n,fc::mutable_variant_object()("version",0)("core","4,SYS"));
 call("eosio"_n,"setramrate"_n,"eosio"_n,fc::mutable_variant_object()("bytes_per_block",1024));
 deploy("eosio.reserv"_n,released/"eosio.system/.powerup/powup.results");
 auto buy=[&](name owner) {call("eosio"_n,"buyram"_n,"eosio"_n,fc::mutable_variant_object()("payer","eosio")("receiver",owner)("quant","100.0000 SYS"));};
 buy("eosio.reserv"_n);buy("tokenone"_n);buy("developer"_n);
 for(auto owner:bps)buy(owner);
 for(auto owner:users)buy(owner);
 auto pricing=[] {return fc::mutable_variant_object()("current_weight_ratio",fc::variant())("target_weight_ratio",fc::variant())
  ("assumed_stake_weight",fc::variant())("target_timestamp",fc::variant())("exponent",2.0)("decay_secs",86400)
  ("min_price","1.0000 SYS")("max_price","100.0000 SYS");};
 call("eosio"_n,"cfgpowerup"_n,"eosio"_n,fc::mutable_variant_object()("args",fc::mutable_variant_object()
  ("net",pricing())("cpu",pricing())("powerup_days",1)("min_powerup_fee","0.0001 SYS")));
 t.produce_block();
 auto provision=[&](name owner,bool rented) {
  std::vector<action> aa;
  auto open=act("eosio.token"_n,"open"_n,owner,fc::mutable_variant_object()("owner",owner)("symbol","4,SYS")("ram_payer",owner));
  open.authorization.insert(open.authorization.begin(),{"eosio"_n,"active"_n});aa.push_back(open);
  aa.push_back(act("eosio.token"_n,"transfer"_n,"eosio"_n,fc::mutable_variant_object()("from","eosio")("to",owner)
   ("quantity",rented?"1000.0000 SYS":"10000.0000 SYS")("memo","normal setup")));
  aa.push_back(rented?act("eosio"_n,"powerup"_n,owner,fc::mutable_variant_object()("payer",owner)("receiver",owner)
   ("days",1)("net_frac",int64_t(900000000000000LL/36))("cpu_frac",int64_t(900000000000000LL/36))("max_payment","100.0000 SYS")):
   act("eosio"_n,"delegatebw"_n,owner,fc::mutable_variant_object()("from",owner)("receiver",owner)
    ("stake_net_quantity","1000.0000 SYS")("stake_cpu_quantity","1000.0000 SYS")("transfer",false)));
  submit(std::move(aa),{{"eosio"_n,"active"},{owner,"active"}});t.produce_block();
 };
 provision("developer"_n,false);
 for(auto owner:bps)provision(owner,false);
 provision("tokenone"_n,false);
 auto sponsored_call=[&](name code,name action_name,name owner,const fc::variant_object& value) {
  auto a=act(code,action_name,owner,value);a.authorization.insert(a.authorization.begin(),{"eosio"_n,"active"_n});
  return submit({a},{{"eosio"_n,"active"},{owner,"active"}});
 };
 for(auto bp:bps) {
  sponsored_call("eosio"_n,"regproducer"_n,bp,fc::mutable_variant_object()("producer",bp)("producer_key",t.get_public_key(bp,"active"))("url","")("location",0));
  auto [priv,pub,pop]=get_bls_key(bp);
  sponsored_call("eosio"_n,"regfinkey"_n,bp,fc::mutable_variant_object()("finalizer_name",bp)("finalizer_key",pub.to_string())("proof_of_possession",pop.to_string()));
  sponsored_call("eosio"_n,"voteproducer"_n,bp,fc::mutable_variant_object()("voter",bp)("proxy","")("producers",std::vector<name>{bp}));
  t.produce_blocks(2);
 }
 auto self_vote=[&](name owner) {sponsored_call("eosio"_n,"voteproducer"_n,owner,
  fc::mutable_variant_object()("voter",owner)("proxy","")("producers",std::vector<name>{bps[0]}));t.produce_block();};
 self_vote("developer"_n);self_vote("tokenone"_n);
 for(size_t i=0;i<users.size();++i) {provision(users[i],i>=4);if(i<4)self_vote(users[i]);}

 // A separate ordinary token contract, same reviewed released token bytes.
 deploy("tokenone"_n,released/"eosio.token/eosio.token");
 call("tokenone"_n,"create"_n,"tokenone"_n,fc::mutable_variant_object()("issuer","tokenone")("maximum_supply","1000000000.0000 TKN"));
 call("tokenone"_n,"issue"_n,"tokenone"_n,fc::mutable_variant_object()("to","tokenone")("quantity","1000000000.0000 TKN")("memo","fixture"));
 for(auto owner:users) {
  auto open=act("tokenone"_n,"open"_n,owner,fc::mutable_variant_object()("owner",owner)("symbol","4,TKN")("ram_payer",owner));
  call("tokenone"_n,"open"_n,owner,fc::mutable_variant_object()("owner",owner)("symbol","4,TKN")("ram_payer",owner));
  call("tokenone"_n,"transfer"_n,"tokenone"_n,fc::mutable_variant_object()("from","tokenone")("to",owner)
   ("quantity","1000.0000 TKN")("memo","prepaid token balance"));
  t.produce_block();
 }
 t.set_authority("developer"_n,"upgrade"_n,authority(t.get_public_key("developer"_n,"active")),"active"_n);
 submit({action({permission_level{"eosio"_n,"owner"_n}},govinit{"developer"_n,upgrade_review_seconds,{}})},{{"eosio"_n,"active"}});
 t.produce_blocks(2);
 BOOST_REQUIRE(find_upgrade_governance(t.control->db())->developer=="developer"_n);
 for(unsigned n=0;n<20000;++n) {
  const auto fp=t.control->head_active_finalizer_policy();
  if(t.control->head_active_producers().producers.size()==21 && fp && fp->finalizers.size()==21)break;
  t.produce_block();
 }
 BOOST_REQUIRE_EQUAL(t.control->head_active_producers().producers.size(),21u);
 BOOST_REQUIRE_EQUAL(t.control->head_active_finalizer_policy()->finalizers.size(),21u);
 std::set<name> actual;
 for(const auto& p:t.control->head_active_producers().producers)actual.insert(p.producer_name);
 BOOST_REQUIRE_EQUAL(actual.size(),21u);for(auto bp:actual)BOOST_REQUIRE(std::find(bps.begin(),bps.end(),bp)!=bps.end());
 t.produce_blocks(6);
 std::vector<private_key_type> peerkeys;
 for(auto bp:bps)peerkeys.push_back(t.get_private_key(bp,"peer-v1"));
 auto register_key=[&](name owner,const public_key_type& key,name signer) {
  return call("eosio"_n,"regpeerkey"_n,signer,fc::mutable_variant_object()("proposer_finalizer_name",owner)("key",key));
 };
 const auto& limits=t.control->get_resource_limits_manager();
 auto used_before=limits.get_account_ram_usage(bps[0]);
 for(size_t i=0;i<bps.size();++i)register_key(bps[i],peerkeys[i].get_public_key(),bps[i]);
 BOOST_REQUIRE(limits.get_account_ram_usage(bps[0])>used_before);
 auto stable=t.get_row_by_account("eosio"_n,"eosio"_n,"peerkeys"_n,bps[0]);
 auto ram_before=limits.get_account_ram_usage(bps[0]);
 BOOST_CHECK_THROW(register_key(bps[0],peerkeys[1].get_public_key(),bps[1]),fc::exception);
 BOOST_CHECK_THROW(register_key(users[0],peerkeys[1].get_public_key(),users[0]),fc::exception);
 BOOST_CHECK_THROW(register_key(bps[0],public_key_type{},bps[0]),fc::exception);
 BOOST_CHECK_THROW(register_key(bps[0],t.get_public_key<mock::webauthn_private_key>(bps[0],"peer-wa"),bps[0]),fc::exception);
 BOOST_CHECK_THROW(register_key(bps[0],peerkeys[0].get_public_key(),bps[0]),fc::exception);
 BOOST_REQUIRE(stable==t.get_row_by_account("eosio"_n,"eosio"_n,"peerkeys"_n,bps[0]));
 BOOST_REQUIRE_EQUAL(limits.get_account_ram_usage(bps[0]),ram_before);
 auto r1=t.get_private_key<fc::crypto::r1::private_key_shim>(bps[0],"peer-r1");
 register_key(bps[0],r1.get_public_key(),bps[0]);
 BOOST_CHECK_THROW(call("eosio"_n,"delpeerkey"_n,bps[0],fc::mutable_variant_object()
  ("proposer_finalizer_name",bps[0])("key",peerkeys[0].get_public_key())),fc::exception);
 call("eosio"_n,"delpeerkey"_n,bps[0],fc::mutable_variant_object()("proposer_finalizer_name",bps[0])("key",r1.get_public_key()));
 BOOST_REQUIRE(t.get_row_by_account("eosio"_n,"eosio"_n,"peerkeys"_n,bps[0]).empty());
 BOOST_REQUIRE(limits.get_account_ram_usage(bps[0])<ram_before);
 register_key(bps[0],peerkeys[0].get_public_key(),bps[0]);
 BOOST_REQUIRE_EQUAL(limits.get_account_ram_usage(bps[0]),ram_before);
 t.produce_blocks(4);
 const auto hash=t.control->calculate_integrity_hash();
 auto top=t.control->get_top_producer_keys();
 BOOST_REQUIRE_EQUAL(t.control->calculate_integrity_hash(),hash);
 for(size_t i=0;i<bps.size();++i) {
  auto it=std::find_if(top.begin(),top.end(),[&](const auto& p){return p.producer_name==bps[i];});
  BOOST_REQUIRE(it!=top.end());BOOST_REQUIRE(it->peer_key==peerkeys[i].get_public_key());
  const auto* table=t.find_table("eosio"_n,"eosio"_n,"peerkeys"_n);BOOST_REQUIRE(table);
  const auto& rows=t.control->db().get_index<key_value_index,by_scope_primary>();
  auto row=rows.find(boost::make_tuple(table->id,bps[i].to_uint64_t()));BOOST_REQUIRE(row!=rows.end());
  BOOST_REQUIRE(row->payer==bps[i]);
 }
 
 // Planned maintenance cases use real actions and finalized 21-member policies.
 abi_serializer system_abi(fc::json::from_file("/campaign/contract/eosio.system.abi").as<abi_def>(),
                    abi_serializer::create_yield_function(fc::seconds(10)));
 auto row=[&](name table,name owner) {
  const auto bytes=t.get_row_by_account("eosio"_n,"eosio"_n,table,owner);
  BOOST_REQUIRE_MESSAGE(!bytes.empty(),"Missing row "<<table<<" "<<owner);
  return system_abi.binary_to_variant(system_abi.get_table_type(table),bytes,abi_serializer::create_yield_function(fc::seconds(10)));
 };
 auto maintenance=[&](name owner,bool enabled) {return call("eosio"_n,"setmaint"_n,owner,
  fc::mutable_variant_object()("producer",owner)("maintenance",enabled));};
 auto state=[&](name owner){return row("bpmaint"_n,owner).get_object()["state"].as_uint64();};
 auto policy_members=[&](){std::set<name> out;for(auto& p:t.control->head_active_producers().producers)out.insert(p.producer_name);return out;};
 bool finalizer_only_discovery_observed=false;
 auto require_authority_peers=[&](){
  auto listed=t.control->get_top_producer_keys();
  const auto has=[&](name owner){return std::any_of(listed.begin(),listed.end(),[&](const auto& p){return p.producer_name==owner;});};
  std::set<name> proposer_names;
  for(const auto& p:t.control->head_active_producers().producers){proposer_names.insert(p.producer_name);BOOST_REQUIRE(has(p.producer_name));}
  for(const auto& f:t.control->head_active_finalizer_policy()->finalizers){
   name who{f.description};BOOST_REQUIRE_MESSAGE(has(who),"A current native finalizer lost discovery before handoff: "<<who);
   if(!proposer_names.contains(who))finalizer_only_discovery_observed=true;
  }
 };
 auto require21=[&](){
  BOOST_REQUIRE_EQUAL(t.control->head_active_producers().producers.size(),21u);
  BOOST_REQUIRE_EQUAL(t.control->head_active_finalizer_policy()->finalizers.size(),21u);
  BOOST_REQUIRE_EQUAL(t.control->head_active_finalizer_policy()->threshold,15u);
  require_authority_peers();
 };
 auto wait_idle=[&](){for(unsigned n=0;n<3000;++n){
  if(row("rotation"_n,"rotation"_n).get_object()["stage"].as_uint64()==0)return;
  t.produce_block();require21();
 }BOOST_FAIL("Normal full-committee rotation did not become idle");};
 fc::variants checks;
 auto passed=[&](const std::string& id,const std::string& scope){
  checks.emplace_back(fc::mutable_variant_object()("id",id)("status","PASS")("scope",scope)
   ("head",t.head().block_num())("LIB",t.last_irreversible_block_num()));
  BOOST_REQUIRE(fc::json::save_to_file(checks,out/"MAINTENANCE_CHECKS.json"));
 };
 wait_idle();require21();
 auto membership=policy_members();
 name standby;for(auto bp:bps)if(!membership.contains(bp)){standby=bp;break;}
 BOOST_REQUIRE(standby!=name{});
 auto no_row=t.get_row_by_account("eosio"_n,"eosio"_n,"bpmaint"_n,standby);
 BOOST_CHECK_THROW(call("eosio"_n,"setmaint"_n,bps[0],fc::mutable_variant_object()
  ("producer",standby)("maintenance",true)),fc::exception);
 BOOST_REQUIRE(no_row==t.get_row_by_account("eosio"_n,"eosio"_n,"bpmaint"_n,standby));
 passed("MAINT-NATIVE-AUTH","A different account cannot suspend the producer; atomic row unchanged");
 BOOST_CHECK_THROW(maintenance(users[0],true),fc::exception);
 passed("MAINT-NATIVE-NONBP","An ordinary unregistered account cannot request BP maintenance");
 auto standby_ram=limits.get_account_ram_usage(standby);
 auto standby_candidate=t.get_row_by_account("eosio"_n,"eosio"_n,"rotcands"_n,standby);
 auto standby_producer=t.get_row_by_account("eosio"_n,"eosio"_n,"producersv2"_n,standby);
 BOOST_REQUIRE(!standby_producer.empty());
 maintenance(standby,true);
 BOOST_REQUIRE_EQUAL(state(standby),2u);
 BOOST_REQUIRE_EQUAL(t.get_row_by_account("eosio"_n,"eosio"_n,"rotcands"_n,standby).size(),standby_candidate.size());
 BOOST_REQUIRE(standby_candidate==t.get_row_by_account("eosio"_n,"eosio"_n,"rotcands"_n,standby));
 BOOST_REQUIRE(standby_producer==t.get_row_by_account("eosio"_n,"eosio"_n,"producersv2"_n,standby));
 BOOST_REQUIRE(limits.get_account_ram_usage(standby)>standby_ram);
 const auto* maintenance_table=t.find_table("eosio"_n,"eosio"_n,"bpmaint"_n);BOOST_REQUIRE(maintenance_table);
 const auto& maintenance_rows=t.control->db().get_index<key_value_index,by_scope_primary>();
 auto maintenance_record=maintenance_rows.find(boost::make_tuple(maintenance_table->id,standby.to_uint64_t()));
 BOOST_REQUIRE(maintenance_record!=maintenance_rows.end());BOOST_REQUIRE(maintenance_record->payer==standby);
 passed("MAINT-NATIVE-STANDBY","Standby pauses immediately, funds own RAM, preserves candidate and reward state");
 BOOST_CHECK_THROW(maintenance(standby,true),fc::exception);
 passed("MAINT-NATIVE-DUPLICATE","Duplicate maintenance request is rejected");
 t.produce_blocks(4);
 auto keys=t.control->get_top_producer_keys();
 BOOST_REQUIRE(std::none_of(keys.begin(),keys.end(),[&](auto& p){return p.producer_name==standby;}));
 passed("MAINT-NATIVE-PEER-EXCLUSION","Paused standby is removed from eligible peer discovery");
 auto row_after_pause=t.get_row_by_account("eosio"_n,"eosio"_n,"bpmaint"_n,standby);
 auto bad=act("eosio"_n,"setmaint"_n,standby,fc::mutable_variant_object()("producer",standby)("maintenance",false));
 auto bad_transfer=act("eosio.token"_n,"transfer"_n,standby,fc::mutable_variant_object()
   ("from",standby)("to",users[0])("quantity","2000000000.0000 SYS")("memo","expected atomic rollback"));
 BOOST_CHECK_THROW(submit({bad,bad_transfer},{{standby,"active"}}),fc::exception);
 BOOST_REQUIRE(row_after_pause==t.get_row_by_account("eosio"_n,"eosio"_n,"bpmaint"_n,standby));
 passed("MAINT-NATIVE-ROLLBACK","Later failed transfer rolls back a preceding maintenance cancellation");
 maintenance(standby,false);BOOST_REQUIRE_EQUAL(state(standby),0u);
 BOOST_CHECK_THROW(maintenance(standby,false),fc::exception);
 t.produce_blocks(4);keys=t.control->get_top_producer_keys();
 BOOST_REQUIRE(std::any_of(keys.begin(),keys.end(),[&](auto& p){return p.producer_name==standby;}));
 passed("MAINT-NATIVE-RESUME","Resume restores normal eligibility; duplicate resume rejects");
 wait_idle();membership=policy_members();name incumbent=*membership.rbegin();
 auto producer_before=row("producersv2"_n,incumbent);
 auto candidate_before=t.get_row_by_account("eosio"_n,"eosio"_n,"rotcands"_n,incumbent);
 maintenance(incumbent,true);
 BOOST_REQUIRE_EQUAL(state(incumbent),1u);require21();
 BOOST_REQUIRE(candidate_before==t.get_row_by_account("eosio"_n,"eosio"_n,"rotcands"_n,incumbent));
 BOOST_REQUIRE_EQUAL(row("producersv2"_n,incumbent).get_object()["unpaid_blocks"].as_uint64(),
   producer_before.get_object()["unpaid_blocks"].as_uint64());
 passed("MAINT-NATIVE-DRAINING","Incumbent request is pending and preserves historical unpaid blocks and violations");
 unsigned handoff_blocks=0;
 for(;handoff_blocks<6000 && state(incumbent)!=2;++handoff_blocks){t.produce_block();require21();}
 BOOST_REQUIRE_MESSAGE(state(incumbent)==2,"Incumbent maintenance did not finish normal proposer/finalizer handoff");
 auto effective=row("bpmaint"_n,incumbent).get_object()["effective_block"].as_uint64();
 BOOST_REQUIRE(effective>0);
 for(unsigned n=0;n<3000&&t.last_irreversible_block_num()<effective;++n){t.produce_block();require21();}
 BOOST_REQUIRE(t.last_irreversible_block_num()>=effective);
 membership=policy_members();BOOST_REQUIRE(!membership.contains(incumbent));
 auto finalizers=t.control->head_active_finalizer_policy();
 BOOST_REQUIRE(std::none_of(finalizers->finalizers.begin(),finalizers->finalizers.end(),
  [&](auto& p){return p.description==incumbent.to_string();}));
 passed("MAINT-NATIVE-HANDOFF","Maintenance becomes effective only after both 21-member policies release the BP and checkpoint finalizes");
 auto unpaid_paused=row("producersv2"_n,incumbent).get_object()["unpaid_blocks"].as_uint64();
 auto candidate_paused=row("rotcands"_n,incumbent);
 t.produce_blocks(504);require21();
 auto paused_after=row("rotcands"_n,incumbent);
 BOOST_REQUIRE_EQUAL(row("producersv2"_n,incumbent).get_object()["unpaid_blocks"].as_uint64(),unpaid_paused);
 BOOST_REQUIRE_EQUAL(paused_after.get_object()["zero_windows"].as_uint64(),candidate_paused.get_object()["zero_windows"].as_uint64());
 BOOST_REQUIRE(paused_after.get_object()["evidence_slots"]==candidate_paused.get_object()["evidence_slots"]);
 BOOST_REQUIRE(!paused_after.get_object()["banned"].as_bool());
 membership=policy_members();BOOST_REQUIRE(!membership.contains(incumbent));
 passed("MAINT-NATIVE-NO-NEW-REWARD-OR-DUTY","Two healthy full rounds accrue no paused BP rewards or new assigned-duty violations; no reentry");

 // Long cooldowns use the real contract and a separate native virtual clock.
 // This laboratory chain is never started by live nodeos.
 const auto historical_credits=row("producersv2"_n,incumbent).get_object()["unpaid_blocks"].as_uint64();
 BOOST_REQUIRE(historical_credits>0);
 t.produce_block(fc::hours(25));t.produce_blocks(4);require21();
 BOOST_REQUIRE_EQUAL(state(incumbent),2u);
 const auto currency=symbol::from_string("4,SYS");
 const auto wallet_before=t.get_currency_balance("eosio.token"_n,currency,incumbent);
 call("eosio"_n,"claimrewards"_n,incumbent,fc::mutable_variant_object()("owner",incumbent));
 t.produce_blocks(4);require21();
 const auto wallet_after=t.get_currency_balance("eosio.token"_n,currency,incumbent);
 BOOST_REQUIRE(wallet_after>wallet_before);
 BOOST_REQUIRE_EQUAL(row("producersv2"_n,incumbent).get_object()["unpaid_blocks"].as_uint64(),0u);
 BOOST_REQUIRE_EQUAL(state(incumbent),2u);
 BOOST_CHECK_THROW(call("eosio"_n,"claimrewards"_n,incumbent,fc::mutable_variant_object()("owner",incumbent)),fc::exception);
 BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()
  ("status","PASS")("case","MAINT-08")("scope","isolated fresh-genesis real-WASM native25hour virtual-clock cooldown; never live deployed")
  ("chain_id",t.control->get_chain_id())("owner",incumbent)("credits_before",historical_credits)
  ("wallet_before",wallet_before)("wallet_after",wallet_after)("paused_state_preserved",true)
  ("repeat_claim_rejected",true)("constants_changed",false)("baseline_promoted",false),out/"REWARD_MATURITY_RESULT.json"));
 passed("MAINT-NATIVE-HISTORICAL-CLAIM","Mature historical credits can be claimed during effective maintenance; original24hour cooldown remains");

 maintenance(incumbent,false);BOOST_REQUIRE_EQUAL(state(incumbent),0u);
 passed("MAINT-NATIVE-INCUMBENT-RESUME","A completed maintainer resumes through ordinary eligibility, without forced policy rewrite");
 wait_idle();membership=policy_members();
 std::vector<name> paused_reserves;
 for(auto bp:bps)if(!membership.contains(bp)){maintenance(bp,true);BOOST_REQUIRE_EQUAL(state(bp),2u);paused_reserves.push_back(bp);}
 BOOST_REQUIRE_EQUAL(paused_reserves.size(),9u);
 incumbent=*membership.rbegin();maintenance(incumbent,true);BOOST_REQUIRE_EQUAL(state(incumbent),1u);
 t.produce_blocks(756);require21();
 BOOST_REQUIRE_EQUAL(state(incumbent),1u);
 BOOST_REQUIRE(policy_members().contains(incumbent));
 passed("MAINT-NATIVE-NO-REPLACEMENT","All nine reserves paused: incumbent remains draining, never shrinks either 21-member policy");
 maintenance(incumbent,false);BOOST_REQUIRE_EQUAL(state(incumbent),0u);
 for(auto bp:paused_reserves){maintenance(bp,false);BOOST_REQUIRE_EQUAL(state(bp),0u);}
 t.produce_blocks(4);require21();
 passed("MAINT-NATIVE-CANCEL","Cancel pending maintenance safely and restore all reserve eligibility");
 actual=policy_members();
 BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()
  ("status","PASS")("scope","isolated real-WASM native maintenance cases at full 21-member committee; not multi-node qualification")
  ("chain_id",t.control->get_chain_id())("checks",checks)("handoff_blocks",handoff_blocks)("finalizer_only_discovery_observed",finalizer_only_discovery_observed)
  ("active_members",actual)("baseline_promoted",false)("TPS","unmeasured"),out/"MAINTENANCE_NATIVE_RESULT.json"));

 // Catch the snapshot up to the actual clock before making live transactions.
 auto delta=fc::time_point::now()-fc::seconds(2)-t.head().block_time();
 if(delta>fc::milliseconds(500))t.produce_block(delta);
 t.produce_blocks(4);
 BOOST_REQUIRE(t.control->is_builtin_activated(builtin_protocol_feature_t::disable_deferred_trxs_stage_1));
 BOOST_REQUIRE(t.control->is_builtin_activated(builtin_protocol_feature_t::disable_deferred_trxs_stage_2));
 BOOST_REQUIRE(!t.control->is_builtin_activated(builtin_protocol_feature_t::sync_call));
 fc::variants private_nodes,public_nodes;
 for(size_t i=0;i<bps.size();++i) {
  auto [priv,pub,pop]=get_bls_key(bps[i]);auto next=t.get_private_key<fc::crypto::r1::private_key_shim>(bps[i],"peer-v2");
  private_nodes.emplace_back(fc::mutable_variant_object()("account",bps[i])("block_provider",
   t.get_public_key(bps[i],"active").to_string({})+"=KEY:"+t.get_private_key(bps[i],"active").to_string({}))
   ("finalizer_provider",pub.to_string()+"=KEY:"+priv.to_string())("peer_public",peerkeys[i].get_public_key())
   ("peer_private",peerkeys[i])("next_peer_public",next.get_public_key())("next_peer_private",next));
  public_nodes.emplace_back(fc::mutable_variant_object()("account",bps[i])("block_key",t.get_public_key(bps[i],"active"))
   ("finalizer_key",pub.to_string())("peer_key",peerkeys[i].get_public_key())("next_peer_key",next.get_public_key()));
 }
 BOOST_REQUIRE(fc::json::save_to_file(private_nodes,out/"PRIVATE_NODES.json"));
 fc::variants commands;
 auto pack_command=[&](std::string label,std::vector<action> acts,name signer) {
  signed_transaction trx;trx.actions=std::move(acts);t.set_transaction_headers(trx,3300-(++nonce)%600);
  trx.expiration=fc::time_point_sec(fc::time_point::now()+fc::seconds(3300-nonce%600));
  trx.sign(t.get_private_key(signer,"active"),t.control->get_chain_id());
  commands.emplace_back(fc::mutable_variant_object()("label",label)("id",trx.id())("packed",packed_transaction(trx,packed_transaction::compression_type::none)));
 };
 for(size_t i=0;i<bps.size();++i) {
  auto bp=bps[i];auto next=t.get_private_key<fc::crypto::r1::private_key_shim>(bp,"peer-v2");
  auto keyact=[&](name action_name,public_key_type key) {return act("eosio"_n,action_name,bp,fc::mutable_variant_object()
   ("proposer_finalizer_name",bp)("key",key));};
  pack_command("rotate-"+bp.to_string(),{keyact("regpeerkey"_n,next.get_public_key())},bp);
  pack_command("stale-delete-"+bp.to_string(),{keyact("delpeerkey"_n,peerkeys[i].get_public_key())},bp);
  pack_command("delete-"+bp.to_string(),{keyact("delpeerkey"_n,next.get_public_key())},bp);
  pack_command("restore-"+bp.to_string(),{keyact("regpeerkey"_n,peerkeys[i].get_public_key())},bp);
 }
 for(int i=0;i<160;++i) {
  auto from=users[(i%20)*2],to=users[(i%20)*2+1];bool sys=i%2==0;
  pack_command("transfer-"+std::to_string(i),{act(sys?"eosio.token"_n:"tokenone"_n,"transfer"_n,from,
   fc::mutable_variant_object()("from",from)("to",to)("quantity",sys?"0.0001 SYS":"0.0001 TKN")("memo","BP discovery "+std::to_string(i)))},from);
 }
 BOOST_REQUIRE(fc::json::save_to_file(commands,out/"SIGNED_COMMANDS.json"));
 t.control->abort_block();
 {std::ofstream stream(out/"bootstrap.snapshot",std::ios::binary);auto w=std::make_shared<ostream_snapshot_writer>(stream);t.control->write_snapshot(w);w->finalize();}
 BOOST_REQUIRE(fc::json::save_to_file(g,out/"genesis.json"));
 BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()
  ("status","real-system-extension-deployed-native-identity-authorization-and-readonly-checks-passed")
  ("chain_id",t.control->get_chain_id())("snapshot_head",t.head().id())("head_block_num",t.head().block_num())
  ("LIB",t.last_irreversible_block_num())("integrity_hash",t.control->calculate_integrity_hash())
  ("nodes",public_nodes)("business_accounts",users)("active_producer_accounts",actual)("active_finalizer_policy",*t.control->head_active_finalizer_policy())("business_stakers",4)("business_powerup_renters",36)
  ("genesis_native_version",2)("CPU_multiplier",16)("NET_bytes_per_block",16777216)("RAM_rate",1024)
  ("system_code_hash",t.control->db().get<account_metadata_object,by_name>("eosio"_n).code_hash)
  ("token_code_hash",t.control->db().get<account_metadata_object,by_name>("eosio.token"_n).code_hash)
  ("tokenone_code_hash",t.control->db().get<account_metadata_object,by_name>("tokenone"_n).code_hash)
  ("governance_sealed",true)("deferred_disabled",true)("sync_call_enabled",false),out/"PUBLIC_FIXTURE.json"));
 std::cout<<"BP discovery native fixture prepared: 30 registered producers/finalizers; 21 active, 40 normally provisioned business accounts"<<std::endl;
}
