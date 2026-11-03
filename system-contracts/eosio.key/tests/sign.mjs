// Test-only stdin signing bridge. Never expose this helper as a relay API.
import {readFileSync} from 'node:fs';
import {signIntent,intentDigest} from '../sdk/index.mjs';
const input=JSON.parse(readFileSync(0,'utf8'));
const abi=JSON.parse(readFileSync(input.abi));
process.stdout.write(JSON.stringify({digest:intentDigest(abi,input.config,input.operation,input.request).toString(),
 signature:signIntent(abi,input.config,input.operation,input.request,input.key)}));
