import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {PrivateKey,PublicKey} from '@wharfkit/antelope';
import {normalizePublicKey,keyHash,assetIdentity,depositMemo,atomicAsset,formatFee,
  intentDigest,signIntent,verifyIntent,validateRequest,marketMultiplier,calculateFee,parseRPCJSON} from '../sdk/index.mjs';

const abi=JSON.parse(readFileSync(process.env.KEY_ABI||new URL('../abi/eosio.key.abi',import.meta.url)));
const owner=PrivateKey.generate('K1'),receiver=PrivateKey.generate('K1');
const config={version:1,chain_id:'ab'.repeat(32),deployment:'cd'.repeat(32)};
const request={from:owner.toPublic().toString(),to:receiver.toPublic().toString(),
 token_contract:'eosio.token',quantity:'1.2345 AIC',memo:'全额到账',nonce:'9007199254740993',
 expires:1800000000,relayer:'keyrelaymoon',reward_to:'keyrelaymoon',max_fee:'0.1000 AIC'};
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
 assert.equal(depositMemo(legacy,'0.1000 AIC'),`PK1|${request.from}|1000`);
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
test('every value-moving field and domain is bound by the owner signature',()=>{
 const sig=signIntent(abi,config,'ksend',request,owner);
 assert(verifyIntent(abi,config,'ksend',request,sig));
 const changes={to:PrivateKey.generate('K1').toPublic().toString(),token_contract:'keytesttoken',
 quantity:'1.2346 AIC',memo:'tampered',nonce:'9007199254740994',expires:1800000001,
 max_fee:'0.1001 AIC'};
 for(const [key,value]of Object.entries(changes))assert(!verifyIntent(abi,config,'ksend',{...request,[key]:value},sig),key);
 assert(!verifyIntent(abi,config,'ksend',{...request,relayer:'keyrelaystar',reward_to:'keyrelaystar'},sig));
 for(const patch of [{chain_id:'ef'.repeat(32)},{deployment:'ef'.repeat(32)},{version:2}])
  assert(!verifyIntent(abi,{...config,...patch},'ksend',request,sig));
 assert(!verifyIntent(abi,config,'ksend',request,sig,'keyalternate'));
 assert.throws(()=>signIntent(abi,config,'ksend',request,receiver));
 assert.notEqual(intentDigest(abi,config,'ksend',request).toString(),intentDigest(abi,config,'ksend',{...request,nonce:'9007199254740992'}).toString());
});
test('withdrawal to native account and self exit have separate signature domains',()=>{
 const out={...request,to:'keyuserlunar'};
 const sig=signIntent(abi,config,'withdraw',out,owner);
 assert(verifyIntent(abi,config,'withdraw',out,sig));
 assert(!verifyIntent(abi,config,'withdraw',{...out,to:'keyusersolar'},sig));
 const self={...out,relayer:'',reward_to:''};
 assert(!verifyIntent(abi,config,'selfout',self,sig));
 assert(verifyIntent(abi,config,'selfout',self,signIntent(abi,config,'selfout',self,owner)));
 assert.throws(()=>validateRequest('selfout',out));
 assert.throws(()=>validateRequest('ksend',{...request,private_key:'must never be accepted'}));
 assert.throws(()=>validateRequest('ksend',{...request,memo:'字'.repeat(43)}));
 assert.throws(()=>validateRequest('ksend',{...request,nonce:9007199254740993}));
});
test('cancellation signs both current and replacement nonce',()=>{
 const c={from:request.from,nonce:request.nonce,next_nonce:'9007199254741000',expires:request.expires,
 relayer:request.relayer,reward_to:request.reward_to,max_fee:request.max_fee};
 const sig=signIntent(abi,config,'cancel',c,owner);
 assert(verifyIntent(abi,config,'cancel',c,sig));
 assert(!verifyIntent(abi,config,'cancel',{...c,next_nonce:'9007199254741001'},sig));
 assert.throws(()=>validateRequest('cancel',{...c,next_nonce:c.nonce}));
});
