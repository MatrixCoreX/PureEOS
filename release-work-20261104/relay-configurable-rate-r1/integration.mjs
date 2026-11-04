import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {calculateFee, marketMultiplier} from '/opt/pureeos-key-rate-20261104-r1/source-r3/sdk/index.mjs';
const K='/opt/pureeos-explorer-key-rate-20261104-r3',S=K+'/source';
const deps=createRequire(S+'/package.json'),{build}=await import(pathToFileURL(deps.resolve('vite')).href);
console.log('INTEGRATION: bundle actual explorer API and fee model');
await build({root:S,configFile:false,logLevel:'error',build:{outDir:S+'/remote-integration-build',emptyOutDir:true,ssr:true,rollupOptions:{input:{api:S+'/src/lib/api.ts','key-fees':S+'/src/lib/key-fees.ts'},output:{entryFileNames:'[name].mjs'}}}});
{
 const {ExplorerApi}=await import(pathToFileURL(S+'/remote-integration-build/api.mjs').href),model=await import(pathToFileURL(S+'/remote-integration-build/key-fees.mjs').href);
 console.log('INTEGRATION: read actual chain pricing and compare60 cases');
 const api=new ExplorerApi({mode:'live',rpc:'http://127.0.0.1:8890',history:'http://127.0.0.1:7000',symbol:'AIC',chainId:'116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'});
 const snapshot=await model.keyFeeMarket(api),cfg=(await api.table({code:'eosio.key',scope:'eosio.key',table:'config',limit:1})).rows[0];
 const market=(await api.table({code:'eosio',scope:'0',table:'powup.state',limit:1})).rows[0];
 if(!snapshot.cached)assert.equal(snapshot.multiplierBps,Number(marketMultiplier(cfg.policy,market).multiplier));
 const quotes=[];
 for(const rate of [1000,1500,2000,2500,3000])for(const rows of [0,1,2])for(const memo of [0,64,65,128]){
  const expected=calculateFee(cfg.policy,BigInt(snapshot.multiplierBps),4+Math.ceil(memo/64),rows,true,rate);
  const actual=model.estimateKeyFee(snapshot,rows,memo,rate);
  for(const field of ['base','total'])assert.equal(actual[field],expected[field].toString());
  assert.equal(actual.relay,expected.premium.toString());
  assert.equal(BigInt(actual.execution)+BigInt(actual.storage)+BigInt(actual.supplement)+BigInt(actual.relay),BigInt(actual.total));
  quotes.push({relay_fee_bps:rate,rows,memo_bytes:memo,...actual});
 }
 const result={status:'PASS',chain_id:cfg.chain_id,snapshot,quotes,SDK_comparisons:quotes.length,read_only:true,UTC:new Date().toISOString()};
 fs.writeFileSync(K+'/INTEGRATION_RESULT.json',JSON.stringify(result,null,2)+'\n');
 console.log(JSON.stringify({status:'PASS',SDK_comparisons:quotes.length,read_only:true}));
}
