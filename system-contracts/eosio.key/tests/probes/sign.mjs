// Test-only stdin keys; stdout contains public transactions/signatures only.
import {readFileSync}from'node:fs';
import{createHash,randomBytes}from'node:crypto';
import{Transaction,SignedTransaction,PackedTransaction,PrivateKey,Checksum256,Serializer}from'@wharfkit/antelope';
const p=JSON.parse(readFileSync(0,'utf8')),abi=JSON.parse(readFileSync(p.abi));
const sha=x=>createHash('sha256').update(x).digest();
const owner=PrivateKey.from(p.owner_key),payer=PrivateKey.from(p.payer_key),account='keyproofdemo';
const encode=(type,object)=>Serializer.encode({abi,type,object}).hexString;
const block=Buffer.from(p.info.last_irreversible_block_id,'hex');
const fields={owner:owner.toPublic().toString(),value:p.value||1,request_id:p.request_id||randomBytes(32).toString('hex'),recurse:!!p.recurse};
let tx=Transaction.from(p.template||{expiration:new Date(Date.parse(p.info.head_block_time+'Z')+(p.ttl??120)*1000).toISOString().slice(0,19),
 ref_block_num:p.info.last_irreversible_block_num&65535,ref_block_prefix:block.readUInt32LE(8),max_net_usage_words:0,max_cpu_usage_ms:0,delay_sec:0,
 context_free_actions:[{account,name:'prove',authorization:[],data:encode('prove',{owner:fields.owner,chain_id:p.chain_id,deployment:p.deployment})}],
 actions:[{account,name:'run',authorization:[{actor:'eosio',permission:'active'}],data:encode('run',fields)}],transaction_extensions:[]});
const schema={version:'eosio::abi/1.2',types:[],structs:[{name:'proof_envelope',base:'',fields:[
 {name:'tag',type:'string'},{name:'version',type:'uint32'},{name:'chain_id',type:'checksum256'},{name:'contract',type:'name'},
 {name:'deployment',type:'checksum256'},{name:'transaction_id',type:'checksum256'}]}],actions:[],tables:[]};
const digest=Checksum256.from(sha(Serializer.encode({abi:schema,type:'proof_envelope',object:{tag:'PureEOS:tx-proof-probe',version:2,
 chain_id:p.chain_id,contract:account,deployment:p.deployment,transaction_id:tx.id}}).array));
const proof=p.proof_override||owner.signDigest(digest).toString();
if(p.tamper==='expiration')tx=Transaction.from({...tx.toJSON(),expiration:new Date(Date.parse(tx.expiration.toString()+'Z')+1000).toISOString().slice(0,19)});
if(p.tamper==='value')tx=Transaction.from({...tx.toJSON(),actions:[{...tx.actions[0].toJSON(),data:encode('run',{...fields,value:99})}]});
let cfd=Serializer.encode({type:'signature',object:proof}).hexString;
if(p.tamper==='proof')cfd='00'+'00'.repeat(65);
const unsigned=SignedTransaction.from({...tx.toJSON(),context_free_data:[cfd],signatures:[]});
const raw=PackedTransaction.fromSigned(unsigned,0).packed_trx.array;
const cfhash=sha(Serializer.encode({type:'bytes[]',object:[cfd]}).array);
const nativeDigest=Checksum256.from(sha(Buffer.concat([Buffer.from(p.info.chain_id,'hex'),Buffer.from(raw),cfhash])));
const signed=SignedTransaction.from({...unsigned.toJSON(),signatures:[payer.signDigest(nativeDigest).toString()]});
process.stdout.write(JSON.stringify({id:tx.id.toString(),transaction:tx.toJSON(),proof,owner_digest:digest.toString(),packet:PackedTransaction.fromSigned(signed,0).toJSON()}));
