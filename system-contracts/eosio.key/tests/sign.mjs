// Test-only attack/signing bridge. Keys enter via stdin, never a relay API.
import{readFileSync}from'node:fs';import{createHash}from'node:crypto';
import{Transaction,SignedTransaction,PackedTransaction,PrivateKey,Checksum256,Serializer}from'@wharfkit/antelope';
import{prepareEnvelope,signPrepared,ownerDigest,CORE_ABI}from'../sdk/index.mjs';
const p=JSON.parse(readFileSync(0,'utf8')),sha=x=>createHash('sha256').update(x).digest();
let prepared=p.original?.prepared||p.prepared||prepareEnvelope(p.config,p.info,p.operation,p.request,{payer:p.payer||'',permission:p.permission||(p.operation.startsWith('self')?'active':'keyrelay'),validSeconds:p.ttl??120});
let body=p.original||signPrepared(prepared,p.config,p.key),raw=JSON.parse(JSON.stringify(prepared.transaction));
if(p.original&&JSON.stringify(p.request)!==JSON.stringify(prepared.request))raw.actions[0].data=Serializer.encode({abi:CORE_ABI,type:p.operation,object:p.operation.startsWith('self')?{payer:p.payer||'keyusersolar',request:p.request}:{request:p.request}}).hexString;
if(p.actor){const [actor,permission='active']=p.actor.split('@');raw.actions[0].authorization=[{actor,permission}];}
if(p.attack==='expiration')raw.expiration=new Date(Date.parse(raw.expiration+'Z')+1000).toISOString().slice(0,19);
if(p.attack==='extra-root')raw.actions.push(raw.actions[0]);
if(p.attack==='domain')raw.context_free_actions[0].data=Serializer.encode({abi:CORE_ABI,type:'prove',object:{owner:prepared.request.from,chain_id:'11'.repeat(32),deployment:p.config.deployment,reference_block_num:prepared.reference_block_num}}).hexString;
if(p.attack==='bridge-missing')raw.actions.pop();
if(p.attack==='bridge-nonce')raw.actions[1].data='11'.repeat(32);
if(p.attack==='bridge-auth')raw.actions[1].authorization=raw.actions[0].authorization;
if(p.attack==='bridge-extra')raw.actions.push(raw.actions[0]);
if(p.attack==='bridge-reorder')raw.actions.reverse();
if(p.attack==='bridge-account')raw.actions[1].account='eosio.key';
const tx=Transaction.from(raw);
// Adversarial bridge cases deliberately have a VALID owner's proof over the
// malformed complete transaction: contract layout guards must reject them.
const proof=p.attack?.startsWith('bridge-')?PrivateKey.from(p.key).signDigest(ownerDigest(p.config,tx)).toString():body.owner_signature;
let cfd=[Serializer.encode({type:'signature',object:proof}).hexString];
if(p.attack==='invalid-proof')cfd=['00'+'00'.repeat(65)];
if(p.attack==='extra-cfd')cfd.push('1234');
const nativeDigest=Checksum256.from(sha(Buffer.concat([Buffer.from(p.config.chain_id,'hex'),Buffer.from(Serializer.encode({type:Transaction,object:tx}).array),sha(Serializer.encode({type:'bytes[]',object:cfd}).array)])));
const signed=SignedTransaction.from({...tx.toJSON(),context_free_data:cfd,signatures:[PrivateKey.from(p.payer_key).signDigest(nativeDigest).toString()]});
process.stdout.write(JSON.stringify({digest:ownerDigest(p.config,prepared.transaction).toString(),signature:body.owner_signature,body,prepared,signed:JSON.parse(JSON.stringify(signed)),packet:PackedTransaction.fromSigned(signed,0).toJSON()}));
