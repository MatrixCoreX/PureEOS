import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {KeyWalletClient,calculateMarketFee,allocationBytes} from './runtime/sdk/index.mjs';
const K='/opt/pureeos-dev-proposals-20261104-r1',S=K+'/frontend',deps=createRequire(S+'/package.json');
const {build}=await import(pathToFileURL(deps.resolve('vite')).href);
await build({root:S,configFile:false,logLevel:'error',build:{outDir:S+'/fee-integration-build',emptyOutDir:true,ssr:true,rollupOptions:{input:{api:S+'/src/lib/api.ts','key-fees':S+'/src/lib/key-fees.ts'},output:{entryFileNames:'[name].mjs'}}}});
const {ExplorerApi}=await import(pathToFileURL(S+'/fee-integration-build/api.mjs').href),model=await import(pathToFileURL(S+'/fee-integration-build/key-fees.mjs').href);
const api=new ExplorerApi({mode:'live',rpc:'http://127.0.0.1:8890',history:'http://127.0.0.1:7000',symbol:'AIC',chainId:'116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'});
const snapshot=await model.keyFeeMarket(api),client=await new KeyWalletClient('http://127.0.0.1:8890').load();assert.equal(snapshot.ramPricing,'market');
assert.equal((await client.rpc('get_code_hash',{account_name:'eosio.key'})).code_hash,'bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1');
const ram=(await client.rpc('get_table_rows',{json:true,code:'eosio',scope:'eosio',table:'rammarket',limit:2})).rows[0],cases=[];
for(const rate of [1000,1500,2000,2500,3000])for(const rows of [0,1,2]){
 const expected=calculateMarketFee(client.config.policy,BigInt(snapshot.multiplierBps),4,allocationBytes(rows),ram,true,rate),actual=model.estimateKeyFee(snapshot,rows,0,rate);
 for(const field of ['base','total','execution','storage','supplement'])assert.equal(actual[field],String(expected[field]));assert.equal(actual.relay,String(expected.premium));cases.push({rate,rows,...actual});
}
fs.writeFileSync(K+'/LIVE_FEE_INTEGRATION.json',JSON.stringify({status:'PASS',actual_deployed_profile:true,SDK_UI_comparisons:cases.length,snapshot,ram,cases,UTC:new Date().toISOString()},null,2)+'\n');console.log('ACTUAL MARKET SDK/UI PASS '+cases.length);
