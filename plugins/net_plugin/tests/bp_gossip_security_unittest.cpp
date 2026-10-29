#include <boost/test/unit_test.hpp>
#include <eosio/net_plugin/auto_bp_peering.hpp>
#include <eosio/chain/peer_keys_db.hpp>
#include <fc/crypto/private_key.hpp>
using namespace eosio::chain;
namespace {
struct auth_chain {
   peer_keys_db_t keys;
   auth_chain() {keys.set_active({"alice"_n,"bob"_n});}
   auto get_peer_info(name who) const {return keys.get_peer_info(who);}
};
struct auth_chain_plugin {auth_chain database; const auth_chain& chain() const {return database;}};
struct auth_connection {};
struct auth_net : eosio::auto_bp_peering::bp_connection_manager<auth_net,auth_connection> {
   auth_chain_plugin plugin;
   auth_chain_plugin* chain_plug=&plugin;
   chain_id_type chain_id=chain_id_type::empty_chain_id();
   std::atomic<block_timestamp_type> head_block_time{block_timestamp_type{10000}};
   fc::logger get_logger() const {return fc::logger::get(DEFAULT_LOGGER);}
};
using message=eosio::gossip_bp_peers_message;
message::signed_bp_peer signed_endpoint(auth_net& net,name owner,const fc::crypto::private_key& key,
                                       std::string endpoint="127.0.0.1:9876") {
   message::signed_bp_peer peer{{.producer_name=owner}};
   message::bp_peer_info_v1 info{endpoint,"127.0.0.1",net.head_block_time.load()+fc::minutes(30)};
   peer.bp_peer_info=fc::raw::pack(info);
   peer.sig=key.sign(peer.digest(net.chain_id));return peer;
}
message wire(message msg) {return fc::raw::unpack<message>(fc::raw::pack(msg));}
bool accepts(auth_net& net,message msg) {
   msg=wire(std::move(msg));return net.validate_gossip_bp_peers_message(msg)&&!msg.peers.empty();
}
void remember(auth_net& net,const message::signed_bp_peer& peer) {
   auto msg=wire(message{{peer}});BOOST_REQUIRE(net.validate_gossip_bp_peers_message(msg));
   BOOST_REQUIRE_EQUAL(msg.peers.size(),1u);net.update_gossip_bps(msg);
}
}
BOOST_AUTO_TEST_CASE(gossip_cached_signature_cannot_authorize_a_modified_payload_or_initial_message) {
   auth_net net;auto key=fc::crypto::private_key::generate();
   net.plugin.database.keys.update_peer_keys(120,{{"alice"_n,key.get_public_key()}});
   const auto valid=signed_endpoint(net,"alice"_n,key);remember(net,valid);
   BOOST_REQUIRE(accepts(net,message{{valid}}));
   auto changed=valid;
   changed.bp_peer_info=fc::raw::pack(message::bp_peer_info_v1{
      "127.0.0.1:9999","127.0.0.1",net.head_block_time.load()+fc::minutes(30)});
   BOOST_CHECK(!accepts(net,message{{changed}}));
   auto forged_initial=valid;forged_initial.bp_peer_info.clear();
   BOOST_CHECK(!accepts(net,message{{forged_initial}}));
   message::signed_bp_peer initial{{.producer_name="alice"_n}};
   initial.sig=key.sign(initial.digest(net.chain_id));
   BOOST_CHECK(accepts(net,message{{initial}}));
}
BOOST_AUTO_TEST_CASE(gossip_replay_must_follow_current_key_and_expiration) {
   auth_net net;auto old=fc::crypto::private_key::generate(),fresh=fc::crypto::private_key::generate();
   auto& cache=net.plugin.database.keys;
   cache.update_peer_keys(120,{{"alice"_n,old.get_public_key()}});
   const auto saved=signed_endpoint(net,"alice"_n,old);remember(net,saved);
   cache.update_peer_keys(240,{{"alice"_n,fresh.get_public_key()}});
   BOOST_CHECK(!accepts(net,message{{saved}}));
   const auto replacement=signed_endpoint(net,"alice"_n,fresh);remember(net,replacement);
   BOOST_CHECK(accepts(net,message{{replacement}}));
   cache.update_peer_keys(360,{{"alice"_n,std::nullopt}});
   BOOST_CHECK(!accepts(net,message{{replacement}}));
   cache.update_peer_keys(480,{{"alice"_n,fresh.get_public_key()}});
   net.head_block_time.store(net.head_block_time.load()+fc::hours(2));
   BOOST_CHECK(!accepts(net,message{{replacement}}));
   auto now=signed_endpoint(net,"alice"_n,fresh);BOOST_CHECK(accepts(net,message{{now}}));
   auth_net other_chain;other_chain.chain_id=chain_id_type(fc::sha256::hash("a different chain"));
   other_chain.plugin.database.keys.update_peer_keys(120,{{"alice"_n,fresh.get_public_key()}});
   BOOST_CHECK(!accepts(other_chain,message{{now}}));
}
BOOST_AUTO_TEST_CASE(gossip_endpoint_count_is_eight_and_malformed_data_is_rejected) {
   auth_net net;auto key=fc::crypto::private_key::generate();
   net.plugin.database.keys.update_peer_keys(120,{{"alice"_n,key.get_public_key()}});
   message eight;
   for(int n=0;n<8;++n)eight.peers.push_back(signed_endpoint(net,"alice"_n,key,"127.0.0.1:"+std::to_string(8100+n)));
   BOOST_REQUIRE(accepts(net,eight));
   auto nine=eight;nine.peers.push_back(signed_endpoint(net,"alice"_n,key,"127.0.0.1:8108"));
   BOOST_CHECK(!accepts(net,nine));
   auto malformed=eight;malformed.peers[0].bp_peer_info={'x'};
   BOOST_CHECK(!accepts(net,malformed));
   auto duplicate=eight;duplicate.peers[1]=duplicate.peers[0];
   BOOST_CHECK(!accepts(net,duplicate));
}
