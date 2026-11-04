import test from 'node:test';
import assert from 'node:assert/strict';
import {PrivateKey} from '@wharfkit/antelope';
import {KeyWalletClient} from '../sdk/index.mjs';
import {Relay} from '../relayer/server.mjs';
const writer='http://writer.invalid',reader='http://reader.invalid',hash='ab'.repeat(32);
test('split RPC reads use reader; exact write packets and unknown methods stay on writer without hidden retries',async()=>{
 const calls=[],packet={signatures:['opaque-test-signature'],packed_trx:'aabb',compression:0};
 const client=new KeyWalletClient(writer,{readEndpoint:reader,fetchImpl:async(url,options)=>{calls.push({url,body:JSON.parse(options.body)});return new Response('{}',{status:url.endsWith('/push_transaction')?500:200});}});
 await client.rpc('get_table_rows',{table:'wallets'});await client.rpcWriter('get_code_hash',{account_name:'eosio.key'});
 await assert.rejects(client.rpc('push_transaction',packet),/HTTP 500/);await client.rpc('send_transaction',packet);
 assert.deepEqual(calls.map(x=>x.url.split('/v1')[0]),[reader,writer,writer,writer]);assert.deepEqual(calls[2].body,packet);assert.equal(calls.filter(x=>x.url.endsWith('/push_transaction')).length,1);
});
test('split RPC load requires equal read/write chain IDs before accepting the wallet configuration',async()=>{
 let wrong=true;const calls=[];const client=new KeyWalletClient(writer,{readEndpoint:reader,fetchImpl:async url=>{
  calls.push(url);const value=url.endsWith('/get_table_rows')?{rows:[{version:2,chain_id:hash}],more:false}:{chain_id:url.startsWith(writer)&&wrong?'cd'.repeat(32):hash};return new Response(JSON.stringify(value));
 }});
 await assert.rejects(client.load(),/chain IDs differ/);assert.equal(client.config,undefined);wrong=false;await client.load();assert.equal(client.config.chain_id,hash);assert(calls.some(url=>url===writer+'/v1/chain/get_info'));
});
test('relay checks the writer wallet code as well as the read node before accepting a new intent',async()=>{
 let laterReads=0;const client={endpoint:writer,readEndpoint:reader,contract:'eosio.key',load:async()=>{},rpc:async()=>({code_hash:hash}),rpcWriter:async()=>({code_hash:'cd'.repeat(32)}),rows:async()=>{laterReads++;return[];}};
 const relay=new Relay({bp:'gptgenesisbp',permission:'keyrelay',key:PrivateKey.generate('K1'),expectedHash:hash,journal:{jobs:{}},client});
 await assert.rejects(relay.profile(),/Writer wallet code changed/);assert.equal(laterReads,0);
});
