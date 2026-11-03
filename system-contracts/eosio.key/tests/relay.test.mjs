import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync,readFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import http from 'node:http';
import {PrivateKey,PackedTransaction,PublicKey} from '@wharfkit/antelope';
import {KeyWalletClient,signIntent} from '../sdk/index.mjs';
import {Journal,Relay,createServer} from '../relayer/server.mjs';
const abi=JSON.parse(readFileSync(process.env.KEY_ABI||new URL('../abi/eosio.key.abi',import.meta.url)));
const owner=PrivateKey.generate('K1'),hot=PrivateKey.generate('K1');
const config={version:1,chain_id:'ab'.repeat(32),deployment:'cd'.repeat(32),fee_contract:'eosio.token'};
const request={from:owner.toPublic().toString(),to:PrivateKey.generate('K1').toPublic().toString(),
 token_contract:'eosio.token',quantity:'1.0000 AIC',memo:'test',nonce:'0',expires:1800001000,
 relayer:'keyrelaymoon',reward_to:'keyrelaymoon',max_fee:'1.0000 AIC'};
test('BP native packet signs the exact owner-signed action using dedicated permission',async()=>{
 const client=new KeyWalletClient('http://localhost');client.config=config;client.abi=abi;
 client.rpc=async()=>({chain_id:config.chain_id,head_block_time:'2026-11-02T23:00:00',last_irreversible_block_num:20,last_irreversible_block_id:'00000014'+'00'.repeat(28)});
 const signature=signIntent(abi,config,'ksend',request,owner);
 const packet=await client.prepareRelayTransaction('ksend',request,signature,request.relayer,'keyrelay',hot);
 assert.equal(packet.packed.signatures.length,1);assert.equal(packet.startLIB,20);
 const transaction=PackedTransaction.from(packet.packed).getTransaction();
 assert.equal(transaction.id.toString(),packet.id);
 assert.equal(transaction.actions[0].authorization[0].permission.toString(),'keyrelay');
 assert.equal(transaction.actions[0].authorization[0].actor.toString(),request.relayer);
});
test('durable exact-packet retry, nonce exclusion and canonical irreversible confirmation',async()=>{
 const dir=mkdtempSync(join(tmpdir(),'eosio-key-relay-'));const journal=new Journal(dir);
 try{
  let pushed=0;const packets=[];const id='ef'.repeat(32);
  const client={config,abi,contract:'eosio.key',wallet:async()=>({id:7,next_nonce:'0'}),
   quote:async()=>({total:100n}),balance:async()=>100000n,
   prepareRelayTransaction:async()=>({id,startLIB:10,expiration:'2027-01-15T08:03:00',packed:{fixture:'original native packet'}}),
   rpc:async(method,data)=>{
    if(method==='get_info')return {chain_id:config.chain_id,head_block_time:'2027-01-15T08:00:00',last_irreversible_block_num:12};
    if(method==='push_transaction'){
      assert(JSON.parse(readFileSync(join(dir,'journal.json')))[Object.keys(journal.jobs)[0]].packed);
      packets.push(JSON.stringify(data));if(++pushed===1)throw new Error('Connection lost after submission');
      return {processed:{block_num:99,receipt:{status:'executed'}}};
    }
    if(method==='get_block')return {id:'aa'.repeat(32),timestamp:'2027-01-15T08:00:01',transactions:data.block_num_or_id===12?[{status:'executed',trx:{id}}]:[]};
    throw new Error(method);
   }};
  const relay=new Relay({bp:request.relayer,permission:'keyrelay',key:hot,expectedHash:'11'.repeat(32),journal,client});
  relay.profile=async()=>{};
  const req={...request,expires:Math.floor(Date.parse('2027-01-15T08:00:00Z')/1000)+600};
  const body={operation:'ksend',request:req,signature:signIntent(abi,config,'ksend',req,owner)};
  const first=await relay.submit(body);assert.equal(first.status,'pending');
  const repeated=await relay.submit(body);assert.equal(repeated.status,'submitted');
  assert.equal(packets[0],packets[1]);assert.equal(repeated.transaction_id,id);
  const changed={...req,memo:'another intent for the same nonce'};
  await assert.rejects(()=>relay.submit({...body,request:changed,signature:signIntent(abi,config,'ksend',changed,owner)}),/pending relay transaction/);
  assert(!journal.jobs[first.intent].confirmation,'speculative push response must not count as finalized');
  await relay.reconcile();assert.equal(journal.jobs[first.intent].status,'finalized');
  assert.equal(journal.jobs[first.intent].confirmation.block_num,12);
  relay.profile=async()=>{throw new Error('Node unavailable after finality');};
  assert.equal((await relay.submit(body)).status,'finalized');
  await assert.rejects(()=>relay.submit({...body,private_key:'never accepted'}),/Only operation/);
 }finally{journal.close();rmSync(dir,{recursive:true,force:true});}
});
test('relay bounds queued work even when HTTP clients disconnect',async()=>{
 const relay=new Relay({bp:request.relayer,permission:'keyrelay',key:hot,expectedHash:'11'.repeat(32),journal:{jobs:{}},client:{}});
 let release;const gate=new Promise(resolve=>{release=resolve;});
 const jobs=Array.from({length:32},()=>relay.exclusive(()=>gate));
 await assert.rejects(()=>relay.exclusive(()=>{}),/queue is full/);
 release();await Promise.all(jobs);assert.equal(relay.pending,0);
});
test('HTTP handles split UTF-8 correctly and rejects oversized bodies before submission',async()=>{
 let accepted=0;const expected={operation:'ksend',request:{memo:'中文🙂'},signature:'test-only'};
 const server=createServer({bp:request.relayer,permission:'keyrelay',journal:{jobs:{}},submit:async body=>{
  assert.deepEqual(body,expected);accepted++;return {status:'pending'};
 }},{port:0});
 await new Promise(resolve=>server.once('listening',resolve));
 const send=async(bytes,split)=>new Promise((resolve,reject)=>{
  const req=http.request({hostname:'127.0.0.1',port:server.address().port,path:'/submit',method:'POST',
    headers:{'content-type':'application/json','content-length':bytes.length}},res=>{
    res.resume();res.on('end',()=>resolve(res.statusCode));
  });req.on('error',reject);req.write(bytes.subarray(0,split));setTimeout(()=>req.end(bytes.subarray(split)),5);
 });
 try{
  const bytes=Buffer.from(JSON.stringify(expected));const split=bytes.indexOf(Buffer.from('中文'))+1;
  assert.equal(await send(bytes,split),202);assert.equal(accepted,1);
  assert.equal(await send(Buffer.alloc(9000,0x20),200),413);assert.equal(accepted,1);
 }finally{await new Promise(resolve=>server.close(resolve));}
});
