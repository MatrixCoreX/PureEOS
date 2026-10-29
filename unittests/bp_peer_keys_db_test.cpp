#include <eosio/chain/peer_keys_db.hpp>
#include <fc/crypto/private_key.hpp>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace eosio::chain;
void require(bool value,const char* what){if(!value)throw std::runtime_error(what);}
int main(){try{
 const auto a="alice"_n,b="bob"_n;
 const auto k1=fc::crypto::private_key::generate().get_public_key();
 const auto k2=fc::crypto::private_key::generate().get_public_key();
 peer_keys_db_t cache;cache.set_active({a,b});
 cache.update_peer_keys(1,{{a,k1},{b,k2}});
 require(cache.get_peer_info(a)->key==k1,"initial identity");
 require(cache.get_peer_info(b)->rank==1,"rank");
 require(cache.configured_peer_keys_updated(),"initial configured update");
 cache.update_peer_keys(120,{});
 require(!cache.get_peer_info(a)&&!cache.get_peer_info(b),"empty roster must revoke every cached identity");
 require(cache.configured_peer_keys_updated(),"empty roster must notify local signer");
 require(!cache.should_update(121)&&cache.should_update(240),"empty roster must advance cache refresh marker");
 cache.update_peer_keys(240,{{a,k2}});
 require(cache.get_peer_info(a)->key==k2,"key rotation");
 cache.update_peer_keys(360,{{a,std::nullopt}});
 require(cache.get_peer_info(a)&&!cache.get_peer_info(a)->key,"key deletion for retained BP");
 cache.update_peer_keys(480,{{b,k1}});
 require(!cache.get_peer_info(a)&&cache.get_peer_info(b)->key==k1,"removed producer invalidation");
 peer_keys_db_t empty;empty.set_active({});empty.update_peer_keys(120,{});
 require(!empty.should_update(121),"empty startup must not retry on every block");
 std::atomic<bool> done=false;std::atomic<unsigned> reads=0;
 std::thread reader([&]{while(!done.load()){cache.get_peer_info(a);cache.get_peer_info(b);++reads;}});
 for(unsigned i=0;i<5000;++i)cache.update_peer_keys(600+i,i%2?getpeerkeys_res_t{}:getpeerkeys_res_t{{a,k1},{b,k2}});
 done=true;reader.join();require(reads>0,"concurrent readers ran");
 cache.update_peer_keys(6000,{});require(!cache.get_peer_info(a)&&!cache.get_peer_info(b),"final concurrent revocation");
 std::cout<<"BP peer-key cache: empty roster, startup refresh, key rotation/deletion, eligibility removal and concurrent reads passed\n";
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
