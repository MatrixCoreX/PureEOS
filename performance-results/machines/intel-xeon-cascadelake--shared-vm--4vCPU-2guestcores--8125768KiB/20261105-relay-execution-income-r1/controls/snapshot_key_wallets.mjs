import fs from 'node:fs';
import {KeyWalletClient} from '/opt/pureeos-relay-execution-income-20261105-r1/runtime/sdk/index.mjs';
const R='/opt/pureeos-relay-execution-income-20261105-r1',qa=JSON.parse(fs.readFileSync(R+'/PUBLIC_TRANSFER_RESULT.json'));
if(qa.status!=='PASS')throw Error('Actual transfer QA required');
const c=await new KeyWalletClient('http://127.0.0.1:8888',{readEndpoint:'http://127.0.0.1:8890'}).load(),assets=await c.rows('assets'),wallets={};
for(const [label,key]of Object.entries(qa.public_keys)){
 const w=await c.wallet(key),balances=w?await c.rows('balances',String(w.id)):[];
 wallets[label]={public_key:key,status:w?'active':'unregistered',holdings:balances.map(b=>{const a=assets.find(a=>String(a.id)===String(b.asset_id));return{contract:a.token_contract,symbol:a.token_symbol.split(',')[1],amount:String(b.amount)}})};
}
fs.writeFileSync(R+'/FINAL_KEY_SNAPSHOT.json',JSON.stringify({status:'PASS',chain_id:c.config.chain_id,UTC:new Date().toISOString(),wallets},null,2)+'\n');
console.log('ACTUAL_FINAL_OWNED_WALLET_SNAPSHOT_SAVED');
