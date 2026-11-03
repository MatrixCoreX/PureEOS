// TEST ONLY: qualify native transaction-bound owner proof, not a deployed wallet.
#include <eosio/eosio.hpp>
#include <eosio/crypto.hpp>
#include <eosio/singleton.hpp>
#include <eosio/transaction.hpp>
#include <tuple>
using namespace eosio;
class [[eosio::contract("keyproofdemo")]] proof_probe:public contract {
 public:
 using contract::contract;
 struct [[eosio::table("config")]] config {checksum256 chain_id,deployment;};
 struct [[eosio::table("total")]] total {uint64_t count=0,value=0;};
 using configs=singleton<"config"_n,config>;
 using totals=singleton<"total"_n,total>;
 static std::vector<char> raw(){
    auto size=transaction_size();check(size>0&&size<=16384,"probe transaction size bound");
    std::vector<char> data(size);check(read_transaction(data.data(),data.size())==size,"transaction read incomplete");return data;
 }
 [[eosio::action]]void init(checksum256 chain_id,checksum256 deployment){
    require_auth(get_self());configs c(get_self(),get_self().value);check(!c.exists(),"already initialized");
    c.set(config{chain_id,deployment},get_self());
 }
 [[eosio::action]]void prove(public_key owner,checksum256 chain_id,checksum256 deployment){
    // No table/time access in this context-free action. Signature bytes are
    // outside transaction::id(), removing the signature/hash circularity.
    check(owner.index()==0,"K1 proof required");
    int size=get_context_free_data(0,nullptr,0);check(size==66,"one canonical K1 proof required");
    std::vector<char> bytes(size);check(get_context_free_data(0,bytes.data(),bytes.size())==size,"proof read incomplete");
    auto sig=unpack<signature>(bytes);check(sig.index()==0,"K1 signature required");
    auto data=raw();auto id=sha256(data.data(),data.size());
    auto envelope=pack(std::make_tuple(std::string("PureEOS:tx-proof-probe"),uint32_t(2),chain_id,get_self(),deployment,id));
    assert_recover_key(sha256(envelope.data(),envelope.size()),sig,owner);
 }
 [[eosio::action]]void run(public_key owner,uint64_t value,checksum256 request_id,bool recurse){
    check(get_first_receiver()==get_self()&&!get_sender().value,"top-level wallet action required");
    auto cfg=configs(get_self(),get_self().value).get();auto data=raw();auto tx=unpack<transaction>(data);
    check(tx.context_free_actions.size()==1&&tx.actions.size()==1,"one proof and one root operation required");
    const auto&p=tx.context_free_actions[0];
    check(p.account==get_self()&&p.name=="prove"_n&&p.authorization.empty()&&
          p.data==pack(std::make_tuple(owner,cfg.chain_id,cfg.deployment)),"proof domain/owner mismatch");
    const auto&a=tx.actions[0];
    check(a.account==get_self()&&a.name=="run"_n&&a.data==pack(std::make_tuple(owner,value,request_id,recurse)),"root operation mismatch");
    check(tx.delay_sec.value==0&&tx.transaction_extensions.empty(),"unsupported transaction extensions");
    check(value>0&&value<=100,"value bound");
    totals t(get_self(),get_self().value);auto s=t.get_or_default();s.count++;s.value+=value;t.set(s,get_self());
    if(recurse)action(permission_level{get_self(),"active"_n},get_self(),"run"_n,
       std::make_tuple(owner,value,request_id,false)).send();
 }
};
