import test from 'node:test';
import assert from 'node:assert/strict';
import {PrivateKey,Serializer} from '@wharfkit/antelope';
import {nativeDepositActions,CORE_ABI,ramMarketQuote,calculateMarketFee} from '../sdk/index.mjs';
test('direct deposit uses one atomic two-action sender-resource envelope and one sender authority',()=>{
 const request={from:'keyusersolar',token_contract:'usd',to:PrivateKey.generate('K1').toPublic().toString(),quantity:'1.0000 USD',max_fee:'0.0000 AIC',request_id:'42'.repeat(32)};
 const actions=nativeDepositActions(request);assert.equal(actions.length,2);const action=actions[0];
 assert.deepEqual(actions.map(a=>a.name),['deposit','transfer']);
 const funding=Serializer.decode({abi:CORE_ABI,type:'token_transfer',data:actions[1].data});assert.equal(funding.quantity.toString(),request.quantity);assert.equal(funding.memo,'atomic key principal');
 assert.equal(action.account,'eosio.key');assert.equal(action.name,'deposit');assert.deepEqual(action.authorization,[{actor:'keyusersolar',permission:'active'}]);
 const decoded=Serializer.decode({abi:CORE_ABI,type:'deposit',data:action.data});
 assert.equal(decoded.quantity.toString(),'1.0000 USD');assert.equal(decoded.max_fee.toString(),'0.0000 AIC');
 assert.throws(()=>nativeDepositActions({...request,unknown:true}),/exactly/);
 assert.throws(()=>nativeDepositActions({...request,max_fee:'0.1000 USD'}),/zero/);assert.throws(()=>nativeDepositActions({...request,max_fee:'0.0001 AIC'}),/zero/);assert.throws(()=>nativeDepositActions(request,{feeFunding:'0.0001 AIC'}),/funding/);assert.throws(()=>nativeDepositActions({...request,request_id:'0'.repeat(64)}),/zero/);
});
test('atomic RAM quote is a gross native spend with fee and integer rounding margin',()=>{
 const market={supply:'10000000000.0000 RAMCORE',base:{balance:'68719476736 RAM',weight:0.5},quote:{balance:'100000.0000 AIC',weight:0.5}};
 const policy={unit_fee:10,minimum_fee:100,maximum_multiplier_bps:100000};
 for(const b of [0,512,1536,2560,3584]){
  const gross=ramMarketQuote(market,b,true),net=gross-(gross+199n)/200n;
  if(b)assert.ok(net*68719476736n/(1000000000n+net)>=BigInt(b));else assert.equal(gross,0n);
  const f=calculateMarketFee(policy,10000n,8,b,market,true,3000,true);
  assert.equal(f.storage,gross);assert.ok(f.base>=gross);assert.equal(f.total,gross+(f.base-gross)+f.premium);
 }
});
