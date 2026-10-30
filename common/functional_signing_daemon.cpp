#include <boost/test/unit_test.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/net_plugin/protocol.hpp>
#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/hex.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
using namespace eosio;
using namespace eosio::chain;
namespace fs=std::filesystem;
static private_key_type test_key(const std::string& who,const std::string& role,bool r1=false){
 auto secret=fc::sha256::hash(who+role);
 if(r1)return private_key_type::regenerate<fc::crypto::r1::private_key_shim>(secret);
 return private_key_type::regenerate(secret);
}
static digest_type peer_digest(const gossip_bp_peers_message::bp_peer& p,const chain_id_type& chain){
 digest_type::encoder enc;fc::raw::pack(enc,chain);fc::raw::pack(enc,p);return enc.result();
}
static fc::variant frame(const net_message& n){
 auto b=fc::raw::pack(n);uint32_t size=b.size();std::vector<char> out=fc::raw::pack(size);out.insert(out.end(),b.begin(),b.end());
 return fc::mutable_variant_object()("hex",fc::to_hex(out.data(),out.size()))("message_index",n.index())("payload_bytes",size);
}
static fc::variant process(const fc::variant& r){
 auto o=r.get_object();auto mode=o["mode"].as_string();
 if(mode=="sign"){
  signed_transaction tx;tx.expiration=o["expiration"].as<fc::time_point_sec>();tx.set_reference_block(o["ref_block_id"].as<block_id_type>());
  auto abis=o["abis"].get_object();
  for(const auto& av:o["actions"].get_array()){
   auto a=av.get_object();action act;act.account=a["account"].as<name>();act.name=a["name"].as<name>();act.authorization=a["authorization"].as<std::vector<permission_level>>();
   if(a.contains("hex")){auto h=a["hex"].as_string();act.data.resize(h.size()/2);fc::from_hex(h,act.data.data(),act.data.size());}
   else{auto y=abi_serializer::create_yield_function(fc::seconds(20));abi_serializer abi(abis[act.account.to_string()].as<abi_def>(),y);auto type=abi.get_action_type(act.name);FC_ASSERT(!type.empty(),"Action absent from ABI");act.data=abi.variant_to_binary(type,a["data"],y);}
   tx.actions.emplace_back(std::move(act));
  }
  for(const auto& s:o["signers"].get_array()){auto v=s.get_object();auto role=v.contains("role")?v["role"].as_string():"active";auto k=test_key(v["account"].as_string(),role);tx.sign(k,o["chain_id"].as<chain_id_type>());}
  return fc::mutable_variant_object()("id",tx.id())("packed",packed_transaction(tx,packed_transaction::compression_type::none));
 }
 if(mode=="wire"){
  handshake_message hs;hs.network_version=o["network_version"].as<uint16_t>();hs.chain_id=o["chain_id"].as<chain_id_type>();hs.node_id=fc::sha256::hash(o["nonce"].as_string());
  hs.time=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  hs.p2p_address="127.0.0.1:20000 - functional-probe";hs.os="linux";hs.agent="aic-functional-owned-probe";hs.generation=1;
  hs.fork_db_root_num=o["root_num"].as<uint32_t>();hs.fork_db_root_id=o["root_id"].as<block_id_type>();hs.fork_db_head_num=o["head_num"].as<uint32_t>();hs.fork_db_head_id=o["head_id"].as<block_id_type>();
  gossip_bp_peers_message g;
  for(const auto& pv:o["peers"].get_array()){
   auto p=pv.get_object();gossip_bp_peers_message::signed_bp_peer v;v.producer_name=p["account"].as<name>();
   if(!p.contains("initial")||!p["initial"].as_bool()){
    gossip_bp_peers_message::bp_peer_info_v1 info;info.server_endpoint=p["endpoint"].as_string();info.outbound_ip_address=p["outbound_ip"].as_string();info.expiration=block_timestamp_type(p["expiration"].as<fc::time_point>());
    v.bp_peer_info=fc::raw::pack(info);
   }
   auto chain=p.contains("sign_chain_id")?p["sign_chain_id"].as<chain_id_type>():hs.chain_id;
   bool r1=p.contains("r1")&&p["r1"].as_bool();auto k=test_key(p["account"].as_string(),p["role"].as_string(),r1);v.sig=k.sign(peer_digest(v,chain));
   if(p.contains("tamper_endpoint")){
    auto info=fc::raw::unpack<gossip_bp_peers_message::bp_peer_info_v1>(v.bp_peer_info);info.server_endpoint=p["tamper_endpoint"].as_string();v.bp_peer_info=fc::raw::pack(info);
   }
   if(p.contains("tamper_account"))v.producer_name=p["tamper_account"].as<name>();
   if(p.contains("tamper_expiration")){auto info=fc::raw::unpack<gossip_bp_peers_message::bp_peer_info_v1>(v.bp_peer_info);info.expiration=block_timestamp_type(p["tamper_expiration"].as<fc::time_point>());v.bp_peer_info=fc::raw::pack(info);}
   if(p.contains("tamper_initial")&&p["tamper_initial"].as_bool())v.bp_peer_info.clear();
   g.peers.push_back(v);
  }
  return fc::mutable_variant_object()("handshake",frame(net_message(hs)))("gossip",frame(net_message(g)));
 }
 if(mode=="decode"){
  auto h=o["hex"].as_string();std::vector<char>b(h.size()/2);fc::from_hex(h,b.data(),b.size());auto n=fc::raw::unpack<net_message>(b);fc::variant value;
  if(auto p=std::get_if<handshake_message>(&n))fc::to_variant(*p,value);
  else if(auto p=std::get_if<gossip_bp_peers_message>(&n))fc::to_variant(*p,value);
  else if(auto p=std::get_if<go_away_message>(&n))value=fc::mutable_variant_object()("reason",(int)p->reason)("node_id",p->node_id);
  return fc::mutable_variant_object()("message_index",n.index())("value",value);
 }
 FC_THROW("Unknown helper mode");
}
BOOST_AUTO_TEST_CASE(functional_signing_daemon){
 const fs::path dir="/campaign/signing";fs::create_directories(dir);
 std::ofstream(dir/"READY")<<"ready\n";
 while(!fs::exists(dir/"STOP")){
  for(const auto& e:fs::directory_iterator(dir)){
   if(e.path().extension()!=".request")continue;
   fs::path out=e.path();out.replace_extension(".response");if(fs::exists(out))continue;
   fc::variant result;
   try{result=fc::mutable_variant_object()("ok",true)("result",process(fc::json::from_file(e.path())));}
   catch(const fc::exception& ex){result=fc::mutable_variant_object()("ok",false)("error",ex.to_detail_string());}
   catch(const std::exception& ex){result=fc::mutable_variant_object()("ok",false)("error",ex.what());}
   fs::path tmp=out;tmp+=".pending";fc::json::save_to_file(result,tmp);fs::rename(tmp,out);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
 }
}
