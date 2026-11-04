// Three tiny, immutable, founder-only intents. Remote private key never leaves host.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {spawnSync} from 'node:child_process';
import {KeyWalletClient,signPrepared,randomRequestId} from './runtime-rpcsplit/sdk/index.mjs';
const K='/opt/pureeos-dev-proposals-20261104-r1',R='/opt/pureeos-keynet-20261103-r2',origin='https://aic-test.matrixai.one';
const {PrivateKey}=createRequire(K+'/runtime/package.json')('@wharfkit/antelope');
const dir=K+'/private-smoke';fs.mkdirSync(dir,{mode:0o700,recursive:true});
const read=p=>JSON.parse(fs.readFileSync(p,'utf8')),write=(p,v)=>fs.writeFileSync(p,JSON.stringify(v,(_,x)=>typeof x==='bigint'?x.toString():x,2)+'\n',{mode:0o600});
assert.equal(read(K+'/APPS_SWITCHED.json').status,'PASS');
const source=read(R+'/private/key-wallet-test-keys.json'),sender=source.holder['Public key'];
const keyfile=dir+'/recipient.json';if(!fs.existsSync(keyfile)){const privateKey=PrivateKey.generate('K1');write(keyfile,{private_key:privateKey.toString(),public_key:privateKey.toPublic().toString()});}
const readFetch=async(url,options)=>{
 for(let attempt=1;attempt<=3;attempt++){
  const response=await fetch(url,options);if(response.status<500||String(url).endsWith('/push_transaction')||attempt===3)return response;
  fs.appendFileSync(K+'/SMOKE_READ_RETRIES.ndjson',JSON.stringify({url:String(url),status:response.status,attempt,UTC:new Date().toISOString()})+'\n');await new Promise(r=>setTimeout(r,1500));
 }
};
const receiver=read(keyfile).public_key,client=await new KeyWalletClient(origin+'/rpc',{readEndpoint:origin+'/api',fetchImpl:readFetch}).load();
assert.equal(client.config.chain_id,'116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0');
assert.equal((await client.rpc('get_code_hash',{account_name:'eosio.key'})).code_hash,'bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1');
const json=async(url,body)=>{
 for(let attempt=1;attempt<=3;attempt++){
  const r=await fetch(url,{...(body?{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)}:{}),signal:AbortSignal.timeout(30000)});const text=await r.text();
  if(r.ok)return JSON.parse(text);
  if(attempt<3&&(r.status>=500||url.endsWith('/relay/submit')&&r.status===400&&/RPC \w+: HTTP 500/.test(text))){
   fs.appendFileSync(K+'/SMOKE_HTTP_RETRIES.ndjson',JSON.stringify({url,status:r.status,response:text,attempt,transaction_id:body?.prepared?.transaction_id,identical_signed_body:!!body,UTC:new Date().toISOString()})+'\n');await new Promise(r=>setTimeout(r,2000));continue;
  }
  assert.fail('HTTP '+r.status+' '+text);
 }
};
const snapshot=async()=>({senderAIC:String(await client.balance(sender,'eosio.token','4,AIC')),receiverAIC:String(await client.balance(receiver,'eosio.token','4,AIC')),
 senderUSD:String(await client.balance(sender,'keyusdtoken1','4,USD')),receiverUSD:String(await client.balance(receiver,'keyusdtoken1','4,USD')),
 buckets:await client.rows('feebuckets'),assets:await client.rows('assets'),pricing:await client.rows('pricing'),market:(await client.rpc('get_table_rows',{json:true,code:'eosio',scope:'eosio',table:'rammarket',limit:2})).rows});
const cli=args=>{const p=spawnSync(R+'/native-client/cleos',['--no-auto-keosd','--url','http://127.0.0.1:8890','--wallet-url','unix://'+R+'/founder/wallet/keosd.sock',...args],{encoding:'utf8',timeout:90000});if(p.status!==0){write(dir+'/cli-diagnostic-'+Date.now()+'.json',{command:args.slice(0,2),status:p.status,stdout:p.stdout,stderr:p.stderr});throw Error('Installed cleos failed; private diagnostics preserved');}return JSON.parse(p.stdout);};
const cases=[];
for(const [kind,token,symbol,bytes]of [['new-key-AIC','eosio.token','AIC',1536],['new-balance-USD','keyusdtoken1','USD',512],['existing-AIC-cleos','eosio.token','AIC',0]]){
 const completed=K+'/'+kind+'-RESULT.json';if(fs.existsSync(completed)){const done=read(completed);assert.equal(done.status,'PASS');cases.push(done);continue;}
 const packet=dir+'/'+kind+'.json';let state;
 if(fs.existsSync(packet))state=read(packet);else{
  const before=await snapshot(),request={from:sender,to:receiver,token_contract:token,quantity:'0.0001 '+symbol,memo:'',auth_epoch:String((await client.wallet(sender)).auth_epoch),request_id:randomRequestId(),relayer:'gptgenesisbp',reward_to:'gptgenesisbp',max_fee:'2.0000 AIC'};
  const quote=await client.quote('ksend',request);assert.equal(quote.ram_pricing,'market');assert.equal(quote.ram_bytes,bytes);
  let signed;if(kind==='existing-AIC-cleos'){const prepared=cli(['key','prepare',sender,receiver,'0.0001 AIC','-c',token,'--bp','gptgenesisbp','--max-fee','2.0000 AIC','--valid-seconds','1800']);signed=cli(['key','sign',JSON.stringify(prepared)]);assert(!Object.hasOwn(signed.prepared.request,'relay_fee_bps'));}
  else signed=signPrepared(await client.prepare('ksend',request,{validSeconds:1800}),client.config,source.holder['Private key']);
  state={before,quote,signed,transaction_id:signed.prepared.transaction_id};write(packet,state);
 }
 const reply=await json(origin+'/relay/submit',state.signed);assert.equal(reply.transaction_id,state.transaction_id);
 let job,info,canonical;const deadline=Date.now()+600000;
 while(Date.now()<deadline){job=await json(origin+'/relay/status/'+state.transaction_id);if(job.status==='finalized'){
  info=await client.rpc('get_info');canonical=await client.rpc('get_block',{block_num_or_id:job.confirmation.block_num});assert(canonical.block_num<=info.last_irreversible_block_num);assert.equal(canonical.id,job.confirmation.block_id);
  assert(canonical.transactions.some(r=>(typeof r.trx==='string'?r.trx:r.trx.id)===state.transaction_id&&r.status==='executed'));break;
 }await new Promise(r=>setTimeout(r,5000));}
 assert.equal(job.status,'finalized');const again=await json(origin+'/relay/submit',state.signed);assert.equal(again.transaction_id,state.transaction_id);assert.equal(again.status,'finalized');
 const after=await snapshot(),before=state.before,b=before.buckets.find(x=>x.owner==='gptgenesisbp'),a=after.buckets.find(x=>x.owner==='gptgenesisbp');
 const base=BigInt(a.network)-BigInt(b.network),service=BigInt(a.relay_earned)-BigInt(b.relay_earned),total=base+service;
 assert.equal(service,(base*3000n+9999n)/10000n);assert.equal(total,BigInt(state.quote.total));assert(total<=20000n);
 assert.equal(BigInt(before.senderAIC)-BigInt(after.senderAIC),total+(symbol==='AIC'?1n:0n));assert.equal(BigInt(after.receiverAIC)-BigInt(before.receiverAIC),symbol==='AIC'?1n:0n);
 assert.equal(BigInt(before.senderUSD)-BigInt(after.senderUSD),symbol==='USD'?1n:0n);assert.equal(BigInt(after.receiverUSD)-BigInt(before.receiverUSD),symbol==='USD'?1n:0n);
 for(const asset of after.assets){const prior=before.assets.find(x=>String(x.id)===String(asset.id));assert.equal(BigInt(asset.ram_used)-BigInt(prior.ram_used),asset.token_contract===token?BigInt(bytes):0n);assert.equal(String(asset.ram_purchased),String(prior.ram_purchased));}
 assert.equal(after.market[0].quote.balance,before.market[0].quote.balance,'No automatic RAM purchase');
 const result={status:'PASS',kind,transaction_id:state.transaction_id,confirmation:job.confirmation,quote:state.quote,base_atoms:String(base),service_atoms:String(service),total_atoms:String(total),ram_budget_bytes:bytes,principal:'0.0001 '+symbol,public_key:receiver,full_principal:true,same_packet_retry:true,only_founder_used:true,no_automatic_RAM_purchase:true};write(completed,result);cases.push(result);
}
const histories=[];
for(const key of [sender,receiver]){let entries=[];const deadline=Date.now()+600000;
 while(Date.now()<deadline){entries=(await json(origin+'/history/v2/history/get_key_transactions?'+new URLSearchParams({public_key:key}))).entries;if(cases.every(c=>entries.some(e=>e.transaction_id===c.transaction_id)))break;await new Promise(r=>setTimeout(r,4000));}
 for(const c of cases){const e=entries.find(e=>e.transaction_id===c.transaction_id);assert(e,'Real receipt absent from public key history');assert.equal(e.fee_total_atomic,c.total_atoms);histories.push({public_key:key,transaction_id:c.transaction_id,entry_id:e.id,block_id:e.block_id});}
}
const assets=await client.rows('assets'),wallets=await client.rows('wallets'),liabilities=new Map(assets.map(a=>[String(a.id),0n]));
for(const w of wallets)for(const b of await client.rows('balances',String(w.id)))liabilities.set(String(b.asset_id),liabilities.get(String(b.asset_id))+BigInt(b.amount));
const credits=(await client.rows('nativefees')).reduce((n,x)=>n+BigInt(x.amount),0n),buckets=(await client.rows('feebuckets')).reduce((n,x)=>n+BigInt(x.network)+BigInt(x.relay_earned),0n),reserves=[];
for(const a of assets){const balance=(await client.rpc('get_currency_balance',{code:a.token_contract,account:'eosio.key',symbol:a.token_symbol.split(',')[1]}))[0],native=BigInt(balance.split(' ')[0].replace('.','')),expected=liabilities.get(String(a.id))+(a.token_symbol==='4,AIC'?credits+buckets:0n);assert.equal(native,BigInt(a.reserve));assert.equal(native,expected);assert([false,0].includes(a.busy));reserves.push({token:a.token_contract,symbol:a.token_symbol,reserve_atoms:String(native),liability_atoms:String(expected)});}
write(K+'/LIVE_SMOKE.json',{status:'PASS',cases,histories,reserves,public_key:receiver,chain_id:client.config.chain_id,UTC:new Date().toISOString()});console.log(JSON.stringify({status:'PASS',transactions:cases.map(c=>c.transaction_id)}));
