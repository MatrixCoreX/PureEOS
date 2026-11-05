import test from 'node:test';import assert from 'node:assert/strict';
import {PrivateKey,Serializer}from'@wharfkit/antelope';
import {CORE_ABI,prepareEnvelope,verifyPrepared,signPrepared,verifySigned,resourcePacket}from'../sdk/index.mjs';
const owner=PrivateKey.generate('K1'),payer=PrivateKey.generate('K1');
const config={version:2,chain_id:'ab'.repeat(32),deployment:'cd'.repeat(32),fee_contract:'eosio.token',fee_symbol:'4,AIC'};
const info={chain_id:config.chain_id,head_block_time:'2026-11-05T12:00:00',last_irreversible_block_num:20,last_irreversible_block_id:'00000014'+'00'.repeat(28)};
const request={from:owner.toPublic().toString(),to:'eosio.signup',token_contract:'eosio.token',quantity:'0.2500 AIC',memo:'mobilekeyone-'+owner.toPublic(),auth_epoch:'0',request_id:'ef'.repeat(32),relayer:'keyrelaymoon',reward_to:'keyrelaymoon',max_fee:'0.1000 AIC',relay_fee_bps:3000};
test('atomic signup signs both exact roots and request-bound continuation',()=>{
 for(const op of ['withdraw','selfout']){
  const r=op==='withdraw'?request:{...request,relayer:'',reward_to:'',relay_fee_bps:0};
  const prepared=prepareEnvelope(config,info,op,r,{payer:'keyusersolar',permission:op==='withdraw'?'keyrelay':'active'});
  assert.equal(prepared.transaction.actions.length,2);assert.deepEqual(prepared.transaction.actions[1],{account:'eosio.signup',name:'createkey',authorization:[],data:r.request_id});
  assert.equal(Serializer.decode({abi:CORE_ABI,type:'createkey',data:r.request_id}).request_id.toString(),r.request_id);
  const signed=signPrepared(prepared,config,owner);assert(verifySigned(signed,config));assert.equal(resourcePacket(signed,config,payer).id,prepared.transaction_id);
  for(const mutate of [p=>p.transaction.actions.pop(),p=>p.transaction.actions.push(p.transaction.actions[0]),p=>p.transaction.actions.reverse(),p=>p.transaction.actions[1].data='11'.repeat(32),p=>p.transaction.actions[1].authorization=p.transaction.actions[0].authorization,p=>p.transaction.actions[1].account='eosio',p=>p.transaction.actions[1].data+='00',p=>p.transaction.actions[1].name='receipt']){
   const bad=JSON.parse(JSON.stringify(prepared));mutate(bad);assert.throws(()=>verifyPrepared(bad,config));
  }
 }
});
test('extra signup root is confined to exact native-AIC withdrawal',()=>{
 for(const patch of [{token_contract:'usd',quantity:'0.2500 USD'},{quantity:'0.25000 AIC'}])assert.throws(()=>prepareEnvelope(config,info,'withdraw',{...request,...patch}),/native AIC/);
 const p=prepareEnvelope(config,info,'withdraw',{...request,to:'keyusersolar'});assert.equal(p.transaction.actions.length,1);
 const q=JSON.parse(JSON.stringify(p));q.transaction.actions.push({account:'eosio.signup',name:'createkey',authorization:[],data:request.request_id});assert.throws(()=>verifyPrepared(q,config));
});
