// Two tiny founder-only payments: modern exact-rate SDK, then installed cleos.
// Each packet is saved before submission. A rerun reconciles the saved ID.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {KeyWalletClient,signPrepared,randomRequestId} from '/opt/pureeos-key-rate-20261104-r1/source-r3/sdk/index.mjs';
const K='/opt/pureeos-key-rate-20261104-r1',R='/opt/pureeos-keynet-20261103-r2',origin='https://aic-test.matrixai.one';
const dir=K+'/live-smoke';fs.mkdirSync(dir,{mode:0o700,recursive:true});
const write=(file,value)=>fs.writeFileSync(file,JSON.stringify(value,null,2)+'\n',{mode:0o600});
const read=file=>JSON.parse(fs.readFileSync(file,'utf8'));
assert.equal(read(K+'/DISPLAY_RELAYS.json').status,'DISPLAY_RELAYS_CONFIGURED');
const keys=read(R+'/private/key-wallet-test-keys.json'),client=await new KeyWalletClient(origin+'/rpc').load();
assert.equal(client.config.chain_id,'116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0');
const json=async(url,body)=>{const r=await fetch(url,{...(body?{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)}:{}),signal:AbortSignal.timeout(30000)});assert(r.ok,'HTTP '+r.status);return r.json();};
const snapshot=async()=>({
 senderAIC:String(await client.balance(keys.holder['Public key'],'eosio.token','4,AIC')),
 receiverAIC:String(await client.balance(keys.recipient['Public key'],'eosio.token','4,AIC')),
 senderUSD:String(await client.balance(keys.holder['Public key'],'keyusdtoken1','4,USD')),
 receiverUSD:String(await client.balance(keys.recipient['Public key'],'keyusdtoken1','4,USD')),
 buckets:await client.rows('feebuckets'),assets:await client.rows('assets'),wallets:await client.rows('wallets')});
const cli=args=>{const p=spawnSync(R+'/native-client/cleos',['--no-auto-keosd','--url',origin+'/rpc','--wallet-url','unix://'+R+'/founder/wallet/keosd.sock',...args],{encoding:'utf8',timeout:90000});if(p.status!==0)throw Error('Installed cleos smoke failed; inspect private remote diagnostics');return JSON.parse(p.stdout);};
const cases=[];
for(const kind of ['sdk-exact-rate','installed-cleos-legacy']){
 const completed=dir+'/'+kind+'-RESULT.json';
 if(fs.existsSync(completed)){const done=read(completed);assert.equal(done.status,'PASS');cases.push(done);continue;}
 const p=dir+'/'+kind+'.json';let state;
 if(fs.existsSync(p))state=read(p);else{
  const before=await snapshot(),lib=(await client.rpc('get_info')).last_irreversible_block_num;
  let signed;
  if(kind==='sdk-exact-rate'){
   const wallet=await client.wallet(keys.holder['Public key']);
   const prepared=await client.prepare('ksend',{from:keys.holder['Public key'],to:keys.recipient['Public key'],token_contract:'eosio.token',quantity:'0.0001 AIC',memo:'Relay rate upgrade smoke',auth_epoch:String(wallet.auth_epoch),request_id:randomRequestId(),relayer:'gptgenesisbp',reward_to:'gptgenesisbp',max_fee:'2.0000 AIC'},{validSeconds:1800});
   assert.equal(prepared.request.relay_fee_bps,3000);signed=signPrepared(prepared,client.config,keys.holder['Private key']);
  }else{
   const prepared=cli(['key','prepare',keys.holder['Public key'],keys.recipient['Public key'],'0.0001 USD','-c','keyusdtoken1','--bp','gptgenesisbp','--max-fee','2.0000 AIC','--valid-seconds','1800']);
   signed=cli(['key','sign',JSON.stringify(prepared)]);assert(!Object.hasOwn(signed.prepared.request,'relay_fee_bps'));
  }
  state={kind,before,start_LIB:lib,signed,transaction_id:signed.prepared.transaction_id};write(p,state);
 }
 const reply=await json(origin+'/relay/submit',state.signed);assert.equal(reply.transaction_id,state.transaction_id);
 let job=reply,info,canonical;
 const deadline=Date.now()+240000;
 while(Date.now()<deadline){
  job=await json(origin+'/relay/status/'+state.transaction_id);
  if(job.status==='finalized'){
   info=await client.rpc('get_info');canonical=await client.rpc('get_block',{block_num_or_id:job.confirmation.block_num});
   assert(canonical.block_num<=info.last_irreversible_block_num);assert.equal(canonical.id,job.confirmation.block_id);
   assert(canonical.transactions.some(r=>(typeof r.trx==='string'?r.trx:r.trx.id)===state.transaction_id&&r.status==='executed'));break;
  }
  await new Promise(r=>setTimeout(r,2000));
 }
 assert.equal(job.status,'finalized');
 const again=await json(origin+'/relay/submit',state.signed);assert.equal(again.transaction_id,state.transaction_id);assert.equal(again.status,'finalized');
 const after=await snapshot(),before=state.before,b=before.buckets.find(x=>x.owner==='gptgenesisbp'),a=after.buckets.find(x=>x.owner==='gptgenesisbp');
 const base=BigInt(a.network)-BigInt(b.network),service=BigInt(a.relay_earned)-BigInt(b.relay_earned),total=base+service;
 assert(base>0n);assert.equal(service,(base*3000n+9999n)/10000n);
 assert.equal(BigInt(before.senderAIC)-BigInt(after.senderAIC),total+(kind==='sdk-exact-rate'?1n:0n));
 assert.equal(BigInt(after.receiverAIC)-BigInt(before.receiverAIC),kind==='sdk-exact-rate'?1n:0n);
 assert.equal(BigInt(before.senderUSD)-BigInt(after.senderUSD),kind==='installed-cleos-legacy'?1n:0n);
 assert.equal(BigInt(after.receiverUSD)-BigInt(before.receiverUSD),kind==='installed-cleos-legacy'?1n:0n);
 for(const asset of after.assets){
  const symbol=String(asset.token_symbol).split(',')[1];
  const balances=await client.rpc('get_currency_balance',{code:asset.token_contract,account:'eosio.key',symbol});
  const reserve=BigInt(balances[0].split(' ')[0].replace('.',''));
  assert.equal(reserve,BigInt(asset.reserve));assert([false,0].includes(asset.busy));
 }
 const result={kind,status:'PASS',transaction_id:state.transaction_id,confirmation:job.confirmation,base_atoms:String(base),service_atoms:String(service),total_atoms:String(total),rate_bps:3000,principal:'0.0001 '+(kind==='sdk-exact-rate'?'AIC':'USD'),same_packet_retry:true,only_founder_used:true};
 write(dir+'/'+kind+'-RESULT.json',result);cases.push(result);
}
const history=[];
if(process.argv.includes('--payments-only')){
 write(K+'/LIVE_PAYMENTS.json',{status:'PASS',cases,chain_id:client.config.chain_id,only_founder_used:true,UTC:new Date().toISOString()});
 console.log(JSON.stringify({status:'PASS',phase:'payments-only',transactions:cases.map(x=>x.transaction_id)}));
 process.exit(0);
}
for(const key of [keys.holder['Public key'],keys.recipient['Public key']]){
 const deadline=Date.now()+240000;let entries;
 while(Date.now()<deadline){
  const result=await json(origin+'/history/v2/history/get_key_transactions?'+new URLSearchParams({public_key:key}));
  entries=result.entries;
  if(cases.every(c=>entries.some(e=>e.transaction_id===c.transaction_id)))break;
  await new Promise(r=>setTimeout(r,4000));
 }
 for(const c of cases){
  const entry=entries.find(e=>e.transaction_id===c.transaction_id);assert(entry,'History must index the upgraded ABI receipt');
  assert.equal(String(entry.fee_total_atomic),c.total_atoms);history.push({public_key:key,transaction_id:c.transaction_id,id:entry.id,block_id:entry.block_id});
 }
}
write(K+'/LIVE_SMOKE.json',{status:'PASS',cases,history,chain_id:client.config.chain_id,new_relay_endpoints_used:0,UTC:new Date().toISOString()});
console.log(JSON.stringify({status:'PASS',transactions:cases.map(x=>x.transaction_id)}));
