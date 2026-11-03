import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync,readFileSync,rmSync}from'node:fs';import{tmpdir}from'node:os';import{join}from'node:path';import http from'node:http';
import{PrivateKey,PackedTransaction}from'@wharfkit/antelope';
import{KeyWalletClient,prepareEnvelope,signPrepared,resourcePacket,randomRequestId}from'../sdk/index.mjs';
import{Journal,Relay,createServer}from'../relayer/server.mjs';
const owner=PrivateKey.generate('K1'),hot=PrivateKey.generate('K1'),config={version:2,chain_id:'ab'.repeat(32),deployment:'cd'.repeat(32),fee_contract:'eosio.token'};
const request={from:owner.toPublic().toString(),to:PrivateKey.generate('K1').toPublic().toString(),token_contract:'eosio.token',quantity:'1.0000 AIC',memo:'test',auth_epoch:'0',request_id:'ef'.repeat(32),relayer:'keyrelaymoon',reward_to:'keyrelaymoon',max_fee:'1.0000 AIC'};
const info={chain_id:config.chain_id,head_block_time:'2026-11-03T00:00:00',head_block_num:22,last_irreversible_block_num:20,last_irreversible_block_id:'00000014'+'00'.repeat(28)};
const signed=(r=request)=>signPrepared(prepareEnvelope(config,info,'ksend',r),config,owner);
test('resource signature preserves known native ID and carries exactly one owner proof',()=>{
 const body=signed(),p=resourcePacket(body,config,hot),tx=PackedTransaction.from(p.packed).getTransaction();
 assert.equal(p.id,body.prepared.transaction_id);assert.equal(tx.id.toString(),p.id);assert.equal(p.packed.signatures.length,1);assert.equal(tx.context_free_actions.length,1);assert.equal(tx.actions[0].authorization[0].permission.toString(),'keyrelay');
});
test('durable immutable retry, independent same-epoch transactions, cross-worker recovery anchor and LIB proof',async()=>{
 const dir=mkdtempSync(join(tmpdir(),'eosio-key-relay-')),journal=new Journal(dir);try{
 let pushed=0;const packets=[],body=signed(),id=body.prepared.transaction_id;
 const client={config,contract:'eosio.key',wallet:async()=>({id:7,auth_epoch:'0'}),quote:async()=>({total:100n}),balance:async()=>100000n,
 prepareRelayTransaction:async b=>resourcePacket(b,config,hot),rpc:async(method,data)=>{
 if(method==='get_info')return{...info,last_irreversible_block_num:22};
 if(method==='push_transaction'){assert(JSON.parse(readFileSync(join(dir,'journal.json')))[data===undefined?'none':Object.keys(journal.jobs)[0]].packed);packets.push(JSON.stringify(data));if(++pushed===1)throw new Error('Connection lost after submission');return{processed:{block_num:99,receipt:{status:'executed'}}};}
 if(method==='get_block')return{id:data.block_num_or_id===20?info.last_irreversible_block_id:'aa'.repeat(32),timestamp:'2026-11-03T00:00:01',transactions:data.block_num_or_id===22?[{status:'executed',trx:{id}}]:[]};throw new Error(method);}};
 const relay=new Relay({bp:request.relayer,permission:'keyrelay',key:hot,expectedHash:'11'.repeat(32),journal,client});relay.profile=async()=>{};
 await assert.rejects(()=>relay.submit(signed({...request,token_contract:'keybadtokenx'})),/operator review/);
 const a=await relay.submit(body);assert.equal(a.status,'pending');const b=await relay.submit(body);assert.equal(b.status,'submitted');assert.equal(packets[0],packets[1]);assert.equal(b.transaction_id,id);assert.equal(journal.jobs[id].next_block,21);
 const second=await relay.submit(signed({...request,memo:'independent device',request_id:randomRequestId()}));assert.notEqual(second.transaction_id,id);assert.equal(Object.keys(journal.jobs).length,2);
 assert(!journal.jobs[id].confirmation);await relay.reconcile();assert.equal(journal.jobs[id].status,'finalized');assert.equal(journal.jobs[id].confirmation.block_num,22);
 relay.profile=async()=>{throw new Error('RPC offline');};assert.equal((await relay.submit(body)).status,'finalized');
 await assert.rejects(()=>relay.submit({...body,private_key:'forbidden'}),/Fields/);
 }finally{journal.close();rmSync(dir,{recursive:true,force:true});}
});
test('disk persistence failure never broadcasts, including a repeated request',async()=>{
 let writes=0,pushes=0;const journal={jobs:{},save(){writes++;throw new Error('disk full');}};
 const client={config,contract:'eosio.key',wallet:async()=>({id:1,auth_epoch:'0'}),quote:async()=>({total:100n}),balance:async()=>100000n,
 prepareRelayTransaction:async b=>resourcePacket(b,config,hot),rpc:async(method)=>{if(method==='get_info')return info;if(method==='get_block')return{id:info.last_irreversible_block_id};if(method==='push_transaction'){pushes++;return{};}throw new Error(method);}};
 const relay=new Relay({bp:request.relayer,permission:'keyrelay',key:hot,expectedHash:'11'.repeat(32),journal,client});relay.profile=async()=>{};
 const body=signed();await assert.rejects(()=>relay.submit(body),/disk full/);assert.equal(Object.keys(journal.jobs).length,0);
 await assert.rejects(()=>relay.submit(body),/disk full/);assert.equal(pushes,0);assert.equal(writes,2);
 // Even manually recovered pending jobs cannot bypass a failing pre-broadcast fsync.
 await assert.rejects(()=>relay.broadcast({status:'pending',packed:{}}),/disk full/);assert.equal(pushes,0);
});
test('automatic broadcasts are bounded while canonical reconciliation remains available',async()=>{
 let broadcasts=0;const relay=new Relay({bp:request.relayer,permission:'keyrelay',key:hot,expectedHash:'11'.repeat(32),journal:{jobs:{},save(){}},client:{rpc:async()=>{broadcasts++;throw new Error('unknown outcome');}}});
 const job={status:'pending',packed:{},id:'ab'.repeat(32)};for(let i=0;i<8;i++)await relay.broadcast(job);assert.equal(broadcasts,3);assert.equal(job.attempts,3);assert.equal(job.status,'pending');
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
