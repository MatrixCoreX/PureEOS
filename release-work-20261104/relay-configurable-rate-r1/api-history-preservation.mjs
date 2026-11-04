import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
const R='/opt/pureeos-keynet-20261103-r2',K='/opt/pureeos-key-rate-20261104-r1';
const {PublicKey,Checksum256,Serializer}=createRequire(R+'/history-source/package.json')('@wharfkit/antelope');
const chain='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0';
const origin=process.argv[2]||'http://127.0.0.1:7000';
async function rpc(method,body={}){const r=await fetch('http://127.0.0.1:8890/v1/chain/'+method,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body),signal:AbortSignal.timeout(30000)});assert(r.ok);return r.json();}
async function ledger(key,extra={}){const r=await fetch(origin+'/v2/history/get_key_transactions?'+new URLSearchParams({public_key:key,...extra}),{signal:AbortSignal.timeout(30000)});assert(r.ok,'History HTTP '+r.status+' '+await r.clone().text());return r.json();}
const info=await rpc('get_info');assert.equal(info.chain_id,chain);
const wallets=(await rpc('get_table_rows',{code:'eosio.key',scope:'eosio.key',table:'wallets',json:true,limit:100})).rows;
assert.equal(wallets.length,8);
const views=[],byTransaction=new Map(),samples=[];
for(const w of wallets){
 const key=PublicKey.from(w.key),hash=Checksum256.hash(Serializer.encode({type:'public_key',object:key}).array).toString().toLowerCase();
 let result=await ledger(key.toString()),first=result;
 assert.equal(result.chain_id,chain);assert.equal(result.key_hash,hash);
 const alias=await ledger(key.toLegacyString());assert.deepEqual(alias.entries.map(x=>x.id),result.entries.map(x=>x.id));
 const seen=new Set();
 do{
  for(const e of result.entries){
   assert(!seen.has(e.id));seen.add(e.id);assert.equal(e.wallet_hash,hash);assert(e.block_num<=info.last_irreversible_block_num);
   let tx=byTransaction.get(e.transaction_id);
   if(!tx){const r=await fetch(origin+'/v2/history/get_transaction?id='+e.transaction_id);assert(r.ok);tx=await r.json();byTransaction.set(e.transaction_id,tx);}
   const receipt=tx.actions.find(a=>a.act.account==='eosio.key'&&a.act.name==='receipt'&&String(a.action_ordinal)===e.action_ordinal);assert(receipt);
   const d=receipt.act.data;assert.equal(e.block_id,receipt.block_id);assert.equal(e.kind,d.kind);
   assert.equal(e.quantity_atomic,BigInt(d.quantity.split(' ')[0].replace('.','')).toString());
   assert.equal(e.fee_total_atomic,BigInt(d.fee.split(' ')[0].replace('.','')).toString());
   if(e.kind==='deposit'){assert.equal(e.fee_charged_atomic,'0');assert.equal(e.fee_paid_by.type,'account');assert.equal(e.fee_paid_by.value,d.native_from);}
   else assert.equal(e.fee_charged_atomic,d.from_key.toLowerCase()===hash?e.fee_total_atomic:'0');
   views.push(e);
  }
  if(!result.next_cursor)break;
  result=await ledger(key.toString(),{cursor:result.next_cursor});
 }while(true);
 const aic=await ledger(key.toString(),{asset:'eosio.token:4,AIC'});
 assert(aic.entries.every(e=>e.asset.id==='eosio.token:4,AIC'||BigInt(e.fee_charged_atomic)>0n));
 if(views.some(e=>e.wallet_hash===hash&&e.asset.symbol==='USD'&&BigInt(e.fee_charged_atomic)>0n))assert(aic.entries.some(e=>e.asset.symbol==='USD'&&BigInt(e.fee_charged_atomic)>0n));
 const before=await ledger(key.toString(),{before:'2020-01-01T00:00:00.000Z'});assert.deepEqual(before.entries,[]);
 samples.push({public_key:key.toString(),rows:seen.size,indexing:first.indexing,AIC_filter_rows:aic.entries.length});
}
assert(views.some(e=>e.kind==='ksend'));assert(views.some(e=>e.kind==='selfout'));
assert(views.every(e=>!['claim','fundfees','refundfee'].includes(e.kind)));
for(const extra of [{public_key:'PUB_K1_invalid'},{public_key:wallets[0].key,cursor:'invalid'},{public_key:wallets[0].key,asset:'AIC'},{public_key:wallets[0].key,after:'2026-11-04T00:00:00Z',before:'2026-11-01T00:00:00Z'}]){
 const r=await fetch(origin+'/v2/history/get_key_transactions?'+new URLSearchParams(extra));assert.equal(r.status,400);
}
const missing=PublicKey.from('EOS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV').toString();assert.equal((await ledger(missing)).entries.length,0);
const result={status:'PASS',origin,chain_id:chain,wallets:samples.length,views:views.length,unique_native_transactions:byTransaction.size,samples,exact_receipt_comparison:true,legacy_alias:true,negative_filters:true,unknown_key_empty:true,native_claim_excluded:true,UTC:new Date().toISOString()};
fs.writeFileSync(K+'/API_'+(origin.startsWith('https:')?'PUBLIC':'LOCAL')+'_RESULT.json',JSON.stringify(result,null,2));console.log(JSON.stringify(result));
