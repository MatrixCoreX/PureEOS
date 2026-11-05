// Read-only comparison of the previous owned-demo balances and canonical receipts.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {KeyWalletClient} from '/opt/pureeos-relay-execution-income-20261105-r1/runtime/sdk/index.mjs';
const K='/opt/pureeos-relay-execution-income-20261105-r1',C='/opt/pureeos-devnet-20261105-r1';
const old=JSON.parse(fs.readFileSync(C+'/FINAL_KEY_SNAPSHOT.json'));
const qa=JSON.parse(fs.readFileSync(C+'/PUBLIC_TRANSFER_RESULT.json'));
const client=await new KeyWalletClient('http://127.0.0.1:8888',{readEndpoint:'http://127.0.0.1:8890'}).load();
assert.equal(client.config.chain_id,old.chain_id);
const assets=await client.rows('assets'),views=[],seen=new Set(),cases=new Map(qa.cases.map(c=>[c.transaction_id,c]));
for(const [label,value] of Object.entries(old.wallets)){
 const wallet=await client.wallet(value.public_key);
 assert.equal(Boolean(wallet),value.status==='active');
 const balances=wallet?await client.rows('balances',String(wallet.id)):[];
 const holdings=balances.map(b=>{const a=assets.find(a=>String(a.id)===String(b.asset_id));return {contract:a.token_contract,symbol:a.token_symbol.split(',')[1],amount:String(b.amount)}});
 assert.deepEqual(holdings,value.holdings);
 const response=await fetch('http://127.0.0.1:7000/v2/history/get_key_transactions?'+new URLSearchParams({public_key:value.public_key,kind:'all',direction:'all'}));
 if(!response.ok)throw Error('Key history HTTP '+response.status);
 const history=await response.json();assert.equal(history.chain_id,old.chain_id);assert.equal(history.indexing.paused,false);
 for(const entry of history.entries){
  const c=cases.get(entry.transaction_id);assert(c,'Unexpected old owned-demo transaction');
  assert.equal(entry.block_id,c.canonical_receipt.block_id);assert.equal(entry.block_num,c.canonical_receipt.block_num);
  assert.equal(BigInt(entry.fee_total_atomic),BigInt(c.fee.total.split(' ')[0].replace('.','')));seen.add(entry.transaction_id);
 }
 views.push({label,holdings,history_entries:history.entries.length});
}
assert.equal(seen.size,cases.size);
fs.writeFileSync(K+'/OLD_OWNED_HISTORY_PRESERVED.json',JSON.stringify({status:'PASS',UTC:new Date().toISOString(),chain_id:old.chain_id,views,canonical_transactions:seen.size,scope:'Read-only; no historical payment or claim resent; unchanged old balances and original per-receipt fees.'},null,2)+'\n');
console.log('OLD_OWNED_DEMO_BALANCES_AND_HISTORY_PRESERVED');
