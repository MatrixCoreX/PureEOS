// Remote private integration helper. Never reads or emits private keys.
import {readFileSync} from 'node:fs';
import {KeyWalletClient} from '../sdk/index.mjs';
const p=JSON.parse(readFileSync(0,'utf8'));
const client=await new KeyWalletClient(p.endpoint).load();
const fee=await client.quote(p.operation,p.request,p.options||{});
process.stdout.write(JSON.stringify(fee,(_k,v)=>typeof v==='bigint'?v.toString():v));
