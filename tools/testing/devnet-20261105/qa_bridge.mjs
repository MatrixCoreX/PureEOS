// Remote owned-demo signer: private keys only enter stdin, never output or relay API.
import fs from 'node:fs';
import {KeyWalletClient,nativeDepositActions,signPrepared,resourcePacket} from './key-service/sdk/index.mjs';
const p=JSON.parse(fs.readFileSync(0,'utf8')),client=new KeyWalletClient('http://127.0.0.1:8888',{readEndpoint:'http://127.0.0.1:8890'});
await client.load();let result;
if(p.mode==='deposit')result={actions:nativeDepositActions(p.request),quote:await client.quote('deposit',p.request)};
else if(p.mode==='prepare'){const prepared=await client.prepare(p.operation,p.request,{validSeconds:180});result={body:signPrepared(prepared,client.config,p.key),quote:await client.quote(p.operation,prepared.request)};}
else if(p.mode==='capfailure'){const prepared=await client.prepare(p.operation,p.request,{validSeconds:180});result=resourcePacket(signPrepared(prepared,client.config,p.key),client.config,p.hot_key);}
else throw Error('Unsupported bounded QA request');
process.stdout.write(JSON.stringify(result,(_,v)=>typeof v==='bigint'?v.toString():v));
