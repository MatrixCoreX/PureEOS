import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
const F='/opt/pureeos-explorer-key-rate-20261104-r3',origin='https://aic-test.matrixai.one';
const build=JSON.parse(fs.readFileSync(F+'/BUILD_RESULT.json')),deployment=JSON.parse(fs.readFileSync(F+'/DEPLOYMENT.json'));
assert.deepEqual(build.files,deployment.files);
const files={};
for(const [name,digest]of Object.entries(build.files)){
 const response=await fetch(origin+'/'+name+'?relay-rates='+Date.now(),{signal:AbortSignal.timeout(30000)});
 assert(response.ok,'Public artifact HTTP '+response.status);
 const actual=createHash('sha256').update(Buffer.from(await response.arrayBuffer())).digest('hex');
 assert.equal(actual,digest,name);files[name]=actual;
}
fs.writeFileSync(F+'/PUBLIC_ARTIFACT_RESULT.json',JSON.stringify({status:'PASS',UTC:new Date().toISOString(),origin,files},null,2)+'\n');
console.log(JSON.stringify({status:'PASS',public_files:Object.keys(files).length}));
