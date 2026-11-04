import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {PrivateKey,PublicKey} from '@wharfkit/antelope';
import {normalizePublicKey,keyHash,assetIdentity,depositMemo,atomicAsset,formatFee,
  prepareEnvelope,signPrepared,verifySigned,verifyPrepared,ownerDigest,resourcePacket,randomRequestId,validateRequest,marketMultiplier,calculateFee,parseRPCJSON,ramRegistrationActions,KeyWalletClient} from '../sdk/index.mjs';

const abi=JSON.parse(readFileSync(process.env.KEY_ABI||new URL('../abi/eosio.key.abi',import.meta.url)));
const owner=PrivateKey.generate('K1'),receiver=PrivateKey.generate('K1');
const config={version:2,chain_id:'ab'.repeat(32),deployment:'cd'.repeat(32)};
const request={from:owner.toPublic().toString(),to:receiver.toPublic().toString(),
 token_contract:'eosio.token',quantity:'1.2345 AIC',memo:'全额到账',auth_epoch:'9007199254740993',request_id:'ef'.repeat(32),relayer:'keyrelaymoon',reward_to:'keyrelaymoon',max_fee:'0.1000 AIC'};
test('RAM admission binds actual native purchase and full token identity to the same sponsor',()=>{
 const actions=ramRegistrationActions('keyusersolar','keytesttoken','6,USDT','1.0000 AIC');
 assert.deepEqual(actions.map(x=>[x.account,x.name]),[['eosio','buyram'],['eosio.key','regtoken']]);
 assert.equal(actions[0].data.receiver,'eosio.key');assert.equal(actions[1].data.token_symbol,'6,USDT');
 for(const a of actions)assert.equal(a.authorization[0].actor,'keyusersolar');
 assert.throws(()=>ramRegistrationActions('eosio.key','keytesttoken','6,USDT','1.0000 AIC'));
 assert.throws(()=>ramRegistrationActions('keyusersolar','keytesttoken','6,USDT','1.0000 USDT'));
 assert.throws(()=>ramRegistrationActions('keyusersolar','keytesttoken','6,USDT','0.0000 AIC'));
 assert.throws(()=>ramRegistrationActions('keyusersolar','keytesttoken','6,USDT','1.0000 AIC',{minimumBytes:0}));
});
test('RPC JSON retains exact uint64/int64 lexemes before number rounding',()=>{
 const value=parseRPCJSON('{"nonce":9007199254740993,"max":18446744073709551615,"debt":-9007199254740993,"height":100,"memo":"9007199254740993"}');
 assert.equal(value.nonce,'9007199254740993');assert.equal(value.max,'18446744073709551615');
 assert.equal(value.debt,'-9007199254740993');assert.equal(value.height,100);assert.equal(value.memo,'9007199254740993');
});
test('canonical K1 and contract+precision asset identities',()=>{
 const legacy=owner.toPublic().toLegacyString();
 assert.equal(normalizePublicKey(legacy),request.from);
 assert.equal(keyHash(legacy),keyHash(request.from));
 assert.notEqual(assetIdentity('eosio.token','4,AIC'),assetIdentity('keytesttoken','4,AIC'));
 assert.notEqual(assetIdentity('eosio.token','4,AIC'),assetIdentity('eosio.token','8,AIC'));
 assert.equal(depositMemo(legacy,'0.1000 AIC','ef'.repeat(32)),`PK2|${request.from}|1000|${'ef'.repeat(32)}`);assert.notEqual(depositMemo(legacy,'0.1000 AIC'),depositMemo(legacy,'0.1000 AIC'));
 assert.throws(()=>normalizePublicKey(PrivateKey.generate('R1').toPublic()));
 assert.throws(()=>depositMemo(request.from,'1.0000 USD'));
});
test('exact integers above JavaScript safe-number range and fee rounding',()=>{
 assert.equal(atomicAsset('123456789012.3456 AIC').units,1234567890123456n);
 assert.equal(formatFee(1n),'0.0001 AIC');
 const p={unit_fee:1,minimum_fee:1,new_row_fee:7,maximum_multiplier_bps:100000};
 assert.deepEqual([calculateFee(p,10001n,1,0,true).base,calculateFee(p,10001n,1,0,true).premium],[2n,1n]);
 assert.equal(calculateFee(p,10000n,1,2,false).total,15n);
 assert.throws(()=>calculateFee(p,10000n,129,0,true));
 assert.throws(()=>formatFee(1n<<62n));
});
test('market occupancy, monotonic bounded multiplier, invalid market fails',()=>{
 const r=n=>({version:0,weight:'10000',utilization:String(n),adjusted_utilization:String(n)});
 const p={maximum_multiplier_bps:100000};
 assert.equal(marketMultiplier(p,{version:1,cpu:r(0),net:r(0)}).multiplier,10000n);
 assert.equal(marketMultiplier(p,{version:1,cpu:r(5000),net:r(0)}).multiplier,32500n);
 assert.equal(marketMultiplier(p,{version:1,cpu:r(10000),net:r(0)}).multiplier,100000n);
 assert.throws(()=>marketMultiplier(p,{version:1,cpu:r(10001),net:r(0)}));
 assert.equal(marketMultiplier(p,{version:1,cpu:{...r(5000),adjusted_utilization:'0'},net:r(0)}).multiplier,32500n);
 assert.throws(()=>marketMultiplier(p,{version:1,cpu:{...r(0),adjusted_utilization:'-1'},net:r(0)}));
 assert.throws(()=>marketMultiplier(p,{version:0,cpu:r(0),net:r(0)}));
});
test('fee-free income claim quotes zero without a market or prepayment and requires claimant-funded token RAM',async()=>{
 const client=new KeyWalletClient('http://127.0.0.1:1');
 client.config={fee_contract:'eosio.token',fee_symbol:'4,AIC'};
 let opened=true;
 client.rpc=async(method,data)=>{
  if(method==='get_info')return {head_block_num:123};
  assert.equal(method,'get_table_rows');assert.equal(data.code,'eosio.token');
  assert.equal(data.scope,'keyrelaymoon');assert.equal(data.table,'accounts');
  return {rows:opened?[{balance:'0.0000 AIC'}]:[]};
 };
 const request={to:'keyrelaymoon',quantity:'0.0001 AIC'};
 assert.deepEqual(await client.quote('claim',request),{base:0n,premium:0n,total:0n,multiplier:10000n,units:0,rows:0,head:123});
 await assert.rejects(client.quote('claim',{...request,quantity:'0.0001 USD'}),/native AIC/);
 opened=false;await assert.rejects(client.quote('claim',request),/claimant-paid RAM/);
});
const info={chain_id:config.chain_id,head_block_time:'2026-11-03T00:00:00',last_irreversible_block_num:20,last_irreversible_block_id:'00000014'+'00'.repeat(28)};
test('complete immutable envelope binds principal, recipient, fee, payer, epoch and native headers',()=>{
 const p=prepareEnvelope(config,info,'ksend',request),body=signPrepared(p,config,owner);
 assert(verifySigned(body,config));
 for(const [k,v]of Object.entries({quantity:'1.2346 AIC',memo:'changed',to:PrivateKey.generate('K1').toPublic().toString(),auth_epoch:'9007199254740994',request_id:randomRequestId(),max_fee:'0.2000 AIC',token_contract:'keytesttoken'})){
  const altered=prepareEnvelope(config,info,'ksend',{...request,[k]:v});assert.throws(()=>verifySigned({prepared:altered,owner_signature:body.owner_signature},config),/signature/);
 }
 const cloned=()=>JSON.parse(JSON.stringify(p));
 for(const mutate of [q=>q.transaction.expiration='2026-11-03T00:04:00',q=>q.transaction.ref_block_prefix++,q=>q.transaction.actions.push(q.transaction.actions[0]),q=>q.transaction.actions[0].authorization[0].permission='active',q=>q.transaction.context_free_actions[0].data+='00',q=>q.transaction.extra='hidden']){const q=cloned();mutate(q);assert.throws(()=>verifyPrepared(q,config));}
 for(const patch of [{chain_id:'11'.repeat(32)},{deployment:'12'.repeat(32)},{version:1}])assert.throws(()=>verifySigned(body,{...config,...patch}));
 assert.throws(()=>signPrepared(p,config,receiver));
 assert.throws(()=>verifySigned({...body,private_key:'forbidden'},config));
});
test('independent devices use the same epoch and different native IDs without reservations',()=>{
 const a=prepareEnvelope(config,info,'ksend',{...request,request_id:randomRequestId()}),b=prepareEnvelope(config,info,'ksend',{...request,request_id:randomRequestId()});
 assert.equal(a.request.auth_epoch,b.request.auth_epoch);assert.notEqual(a.transaction_id,b.transaction_id);
 assert(verifySigned(signPrepared(a,config,owner),config));assert(verifySigned(signPrepared(b,config,owner),config));
 const c={from:request.from,auth_epoch:request.auth_epoch,next_epoch:'9007199254741000',request_id:randomRequestId(),relayer:request.relayer,reward_to:request.relayer,max_fee:request.max_fee};
 assert(verifySigned(signPrepared(prepareEnvelope(config,info,'cancel',c),config,owner),config));
 assert.throws(()=>validateRequest('cancel',{...c,next_epoch:c.auth_epoch}));
 assert.throws(()=>validateRequest('ksend',{...request,auth_epoch:9007199254740993}));
 assert.throws(()=>validateRequest('ksend',{...request,memo:'字'.repeat(43)}));
});
test('self withdrawal binds explicit native payer and has no BP premium',()=>{
 const r={...request,to:'keyuserlunar',relayer:'',reward_to:''};
 const p=prepareEnvelope(config,info,'selfout',r,{payer:'keyusersolar',permission:'active'});assert(verifySigned(signPrepared(p,config,owner),config));
 assert.throws(()=>prepareEnvelope(config,info,'selfout',r));
 const q={...p,payer:'keyuserlunar'};assert.throws(()=>verifyPrepared(q,config));
});

test('relay rates are bounded, rounded upward and charged at least one AIC atom',()=>{
 const p={unit_fee:1,minimum_fee:1,new_row_fee:7,maximum_multiplier_bps:100000};
 for(const rate of [1000,1501,2000,3000]){
  assert.equal(calculateFee(p,10000n,1,0,true,rate).premium,1n);
  const q=calculateFee(p,10000n,101,0,true,rate);
  assert.equal(q.premium,(101n*BigInt(rate)+9999n)/10000n);
  assert(q.premium<=calculateFee(p,10000n,101,0,true,3000).premium);
 }
 for(const rate of [0,999,3001,1000.5,NaN])assert.throws(()=>calculateFee(p,10000n,1,0,true,rate));
 assert.equal(calculateFee(p,10000n,1,0,false).premium,0n);
});
test('owner signatures bind the selected relay rate for sends, withdrawals and cancellations',()=>{
 const cases=[['ksend',{...request}],['withdraw',{...request,to:'keyuserlunar'}],['cancel',{from:request.from,auth_epoch:request.auth_epoch,next_epoch:'9007199254741000',request_id:request.request_id,relayer:request.relayer,reward_to:request.reward_to,max_fee:request.max_fee}]];
 for(const [operation,r]of cases){
  const prepared=prepareEnvelope(config,info,operation,{...r,relay_fee_bps:1000}),signed=signPrepared(prepared,config,owner);
  assert(verifySigned(signed,config));
  const changed=prepareEnvelope(config,info,operation,{...r,relay_fee_bps:3000});
  assert.notEqual(prepared.transaction_id,changed.transaction_id);
  assert.throws(()=>verifySigned({prepared:changed,owner_signature:signed.owner_signature},config),/signature/);
  for(const rate of [999,3001,1000.5])assert.throws(()=>prepareEnvelope(config,info,operation,{...r,relay_fee_bps:rate}));
 }
});
test('relay rate reads validate identity and bounds; legacy registrations retain30%',async()=>{
 const client=new KeyWalletClient('http://127.0.0.1:1');let rows=[];client.rows=async()=>rows;
 assert.equal(await client.relayRate('keyrelaymoon'),3000);
 rows=[{bp:'keyrelaymoon',fee_bps:1000}];assert.equal(await client.relayRate('keyrelaymoon'),1000);
 for(const row of [{bp:'keyusersolar',fee_bps:1000},{bp:'keyrelaymoon',fee_bps:999},{bp:'keyrelaymoon',fee_bps:3001}]){rows=[row];await assert.rejects(client.relayRate('keyrelaymoon'));}
});
