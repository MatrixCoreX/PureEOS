import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {calculateFee,calculateMarketFee,allocationBytes} from '/opt/pureeos-key-ram-market-20261104-r1/source-final/sdk/index.mjs';
const K='/opt/pureeos-key-ram-market-20261104-r1',S=K+'/frontend';
const deps=createRequire(S+'/package.json'),{build}=await import(pathToFileURL(deps.resolve('vite')).href);
await build({root:S,configFile:false,logLevel:'error',build:{outDir:S+'/remote-integration-build',emptyOutDir:true,ssr:true,rollupOptions:{input:{api:S+'/src/lib/api.ts','key-fees':S+'/src/lib/key-fees.ts'},output:{entryFileNames:'[name].mjs'}}}});
const {ExplorerApi}=await import(pathToFileURL(S+'/remote-integration-build/api.mjs').href),model=await import(pathToFileURL(S+'/remote-integration-build/key-fees.mjs').href);
const api=new ExplorerApi({mode:'live',rpc:'http://127.0.0.1:8890',history:'http://127.0.0.1:7000',symbol:'AIC',chainId:'116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'});
const snapshot=await model.keyFeeMarket(api),cfg=(await api.table({code:'eosio.key',scope:'eosio.key',table:'config',limit:1})).rows[0];
assert.equal(snapshot.ramPricing,'fixed','Development must not change the public contract');
const ram=(await api.table({code:'eosio',scope:'eosio',table:'rammarket',limit:2})).rows[0],reserves=model.ramMarketReserves(ram);
const cases=[];
for(const pricing of ['fixed','market'])for(const multiplier of [10000,20712,32500,100000])for(const rate of [1000,1500,2000,2500,3000])for(const rows of [0,1,2])for(const memo of [0,64,65,128]){
 const market={...snapshot,ramPricing:pricing,multiplierBps:multiplier,ram:reserves};
 const expected=pricing==='market'?calculateMarketFee(cfg.policy,BigInt(multiplier),4+Math.ceil(memo/64),allocationBytes(rows),ram,true,rate):calculateFee(cfg.policy,BigInt(multiplier),4+Math.ceil(memo/64),rows,true,rate);
 const actual=model.estimateKeyFee(market,rows,memo,rate);
 for(const field of ['base','total'])assert.equal(actual[field],String(expected[field]));
 assert.equal(actual.relay,String(expected.premium));
 if(pricing==='market')for(const field of ['execution','storage','supplement'])assert.equal(actual[field],String(expected[field]));
 assert.equal(BigInt(actual.execution)+BigInt(actual.storage)+BigInt(actual.supplement)+BigInt(actual.relay),BigInt(actual.total));
 cases.push({pricing,multiplier,rate,rows,memo,...actual});
}
fs.writeFileSync(K+'/INTEGRATION_RESULT.json',JSON.stringify({status:'PASS',scope:'Read-only public legacy-profile RPC plus candidate math against actual RAM reserves; not public candidate execution',snapshot,ram,cases,SDK_comparisons:cases.length,UTC:new Date().toISOString()},null,2)+'\n');
console.log('SDK/EXPLORER PASS '+cases.length+' exact comparisons');
