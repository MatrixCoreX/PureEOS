// New owned QA only. Private keys enter stdin and never leave this process.
import fs from 'node:fs';
import {KeyWalletClient,nativeDepositActions,signPrepared} from '../relay-runtime/sdk/index.mjs';
const p=JSON.parse(fs.readFileSync(0,'utf8')),c=await new KeyWalletClient('http://127.0.0.1:8888',{readEndpoint:'http://127.0.0.1:8890'}).load();
if(c.config.code_hash&&c.config.code_hash!=='7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416')throw Error('Unexpected code');
let r;if(p.mode==='deposit')r={actions:nativeDepositActions(p.request),quote:await c.quote('deposit',p.request)};
else if(p.mode==='prepare'){const prepared=await c.prepare(p.operation,p.request,{validSeconds:180});r={body:signPrepared(prepared,c.config,p.key),quote:await c.quote(p.operation,prepared.request)};}
else throw Error('Unknown owned QA mode');
process.stdout.write(JSON.stringify(r,(_,v)=>typeof v==='bigint'?v.toString():v));
