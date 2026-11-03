import {readFileSync} from 'node:fs';
import {KeyWalletClient} from '../sdk/index.mjs';
const p=JSON.parse(readFileSync(0,'utf8'));
const client=await new KeyWalletClient(p.endpoint).load();
const balance=await client.balance(p.key,p.token,p.symbol);
const wallet=await client.wallet(p.key);
process.stdout.write(JSON.stringify({balance:String(balance),wallet}));
