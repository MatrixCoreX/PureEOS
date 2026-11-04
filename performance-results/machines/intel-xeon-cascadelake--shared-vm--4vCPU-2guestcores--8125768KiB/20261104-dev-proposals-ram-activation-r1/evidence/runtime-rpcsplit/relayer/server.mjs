// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
// Reference BP relay: loopback HTTP behind an HTTPS proxy. No user private keys.
import http from 'node:http';
import {readFileSync,statSync,mkdirSync,openSync,writeFileSync,fsyncSync,closeSync,renameSync,existsSync,unlinkSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {PrivateKey,PublicKey} from '@wharfkit/antelope';
import {KeyWalletClient,verifySigned,atomicAsset} from '../sdk/index.mjs';

export class Journal {
  constructor(directory){
    this.directory=resolve(directory);mkdirSync(this.directory,{recursive:true,mode:0o700});
    if(statSync(this.directory).mode&0o077)throw new Error('Journal directory must be private (0700)');
    this.lock=join(this.directory,'relay.lock');
    const fd=openSync(this.lock,'wx',0o600);writeFileSync(fd,String(process.pid));closeSync(fd);
    this.file=join(this.directory,'journal.json');
    this.jobs=existsSync(this.file)?JSON.parse(readFileSync(this.file,'utf8')):{};
  }
  save(){
    const temporary=this.file+'.tmp';const fd=openSync(temporary,'w',0o600);
    try{writeFileSync(fd,JSON.stringify(this.jobs));fsyncSync(fd);}finally{closeSync(fd);}
    renameSync(temporary,this.file);const dir=openSync(this.directory,'r');
    try{fsyncSync(dir);}finally{closeSync(dir);}
  }
  close(){unlinkSync(this.lock);}
}
const terminal=job=>['finalized','expired'].includes(job.status);
const publicJob=job=>({intent:job.intent,transaction_id:job.id,status:job.status,
  ...(job.confirmation?{confirmation:job.confirmation}:{}),...(job.last_error?{last_error:job.last_error}:{})});
export class Relay {
  constructor({endpoint,readEndpoint,bp,permission,key,expectedHash,journal,client,allowedTokens=[]}){
    if(!/^[0-9a-f]{64}$/.test(expectedHash||''))throw new Error('Pin the qualified eosio.key code hash');
    this.client=client||new KeyWalletClient(endpoint,{readEndpoint});this.bp=bp;this.permission=permission;
    this.allowedTokens=new Set(allowedTokens);this.key=PrivateKey.from(key);this.expectedHash=expectedHash;this.journal=journal;this.queue=Promise.resolve();this.pending=0;
  }
  exclusive(fn){
    if(this.pending>=32)return Promise.reject(new Error('Relay queue is full'));
    this.pending++;const next=this.queue.then(fn);this.queue=next.catch(()=>{});
    return next.finally(()=>{this.pending--;});
  }
  async profile(){
    await this.client.load();
    const code=await this.client.rpc('get_code_hash',{account_name:this.client.contract});
    if(code.code_hash!==this.expectedHash)throw new Error('Wallet code changed; review required');
    if(this.client.readEndpoint!==this.client.endpoint&&(await this.client.rpcWriter('get_code_hash',{account_name:this.client.contract})).code_hash!==this.expectedHash)throw new Error('Writer wallet code changed; review required');
    const relay=(await this.client.rows('relays',this.client.contract,{lower_bound:this.bp,upper_bound:this.bp,limit:2}))[0];
    if(!relay||relay.bp!==this.bp||!relay.enabled||relay.permission!==this.permission)throw new Error('BP relay is not enabled with the configured permission');
    const rows=async table=>(await this.client.rpc('get_table_rows',{json:true,code:'eosio',scope:'eosio',table,
      lower_bound:this.bp,upper_bound:this.bp,limit:2})).rows;
    const [p,c,b,m,account]=await Promise.all([rows('producersv2'),rows('rotcands'),rows('bpbonds'),rows('bpmaint'),
      this.client.rpc('get_account',{account_name:this.bp})]);
    if(!p[0]||p[0].owner!==this.bp||!p[0].is_active||!c[0]||c[0].owner!==this.bp||c[0].banned||
       !b[0]||b[0].owner!==this.bp||Number(b[0].state)!==0||BigInt(b[0].balance)!==100000000n||
       (m[0]?.owner===this.bp&&Number(m[0].state)!==0))throw new Error('BP is not eligible to relay');
    const auth=account.permissions.find(p=>p.perm_name===this.permission);
    const pub=this.key.toPublic();
    if(!auth||auth.parent!=='active'||auth.required_auth.threshold!==1||auth.required_auth.accounts.length||
       auth.required_auth.waits.length||auth.required_auth.keys.length!==1||auth.required_auth.keys[0].weight!==1||
       !PublicKey.from(auth.required_auth.keys[0].key).equals(pub))throw new Error('Use a separate single-key child permission under active');
    for(const parent of account.permissions.filter(p=>['owner','active'].includes(p.perm_name)))
      if(parent.required_auth.keys.some(k=>PublicKey.from(k.key).equals(pub)))throw new Error('Relay key must be separate from owner/active');
    if(PublicKey.from(p[0].producer_key).equals(pub))throw new Error('Relay key must be separate from block production');
  }
  async submit(body){return this.exclusive(async()=>{
    // Verify the bounded owner envelope before any expensive remote work.
    if(!this.client.config)await this.profile();
    const tx=verifySigned(body,this.client.config,this.client.contract);
    const {operation,request,reference_block_num:reference,permission}=body.prepared;
    if(!['ksend','withdraw','cancel'].includes(operation)||request.relayer!==this.bp||permission!==this.permission)throw new Error('Wrong selected BP or permission');
    const intent=tx.id.toString(),existing=this.journal.jobs[intent];
    if(existing){if(!terminal(existing))await this.broadcast(existing);return publicJob(existing);}
    await this.profile();verifySigned(body,this.client.config,this.client.contract);
    if(Object.keys(this.journal.jobs).length>=10000)throw new Error('Journal capacity reached; archive completed jobs');
    const pending=Object.values(this.journal.jobs).filter(j=>!terminal(j));
    if(pending.length>=128)throw new Error('Pending transaction capacity reached');
    if(pending.filter(j=>j.owner===request.from).length>=8)throw new Error('Sender pending capacity reached');
    if(operation!=='cancel'&&request.token_contract!==this.client.config.fee_contract&&!this.allowedTokens.has(request.token_contract))throw new Error('Token issuer requires relay operator review');
    const wallet=await this.client.wallet(request.from);
    if(!wallet||BigInt(wallet.auth_epoch)!==BigInt(request.auth_epoch))throw new Error('Authorization epoch revoked');
    const info=await this.client.rpc('get_info'),now=Date.parse(info.head_block_time+'Z'),expiration=Date.parse(tx.expiration.toString()+'Z');
    if(expiration<now+10000||expiration>now+3500000)throw new Error('Native transaction expiration must allow safe submission');
    if(reference>info.last_irreversible_block_num||info.head_block_num-reference>65000)throw new Error('Signed reference is not a recent irreversible block');
    const block=await this.client.rpc('get_block',{block_num_or_id:reference});
    const bid=Buffer.from(block.id,'hex');
    if(bid.length!==32||bid.readUInt32BE(0)!==reference||bid.readUInt32LE(8)!==Number(tx.ref_block_prefix))throw new Error('Signed reference block identity mismatch');
    const fee=await this.client.quote(operation,request);
    if(fee.total>atomicAsset(request.max_fee).units)throw new Error('Current fee exceeds owner maximum');
    const aic=await this.client.balance(request.from,this.client.config.fee_contract,'4,AIC');
    const principal=operation==='cancel'?null:atomicAsset(request.quantity);
    const aicPrincipal=principal&&request.token_contract===this.client.config.fee_contract&&principal.symbol==='4,AIC'?principal.units:0n;
    if(aic<fee.total+aicPrincipal)throw new Error('Insufficient additional AIC fee balance');
    if(principal&&await this.client.balance(request.from,request.token_contract,principal.symbol)<principal.units)throw new Error('Insufficient principal');
    const packet=await this.client.prepareRelayTransaction(body,this.bp,this.permission,this.key);
    const job={...packet,intent,status:'pending',wallet_id:String(wallet.id),auth_epoch:String(request.auth_epoch),
      owner:request.from,next_block:reference+1,created_at:new Date().toISOString(),operation};
    // Persist the EXACT native packet before its first broadcast. Retries never
    // choose a new TAPOS/expiration or create a second packet for this intent.
    this.journal.jobs[intent]=job;
    try{this.journal.save();}catch(error){delete this.journal.jobs[intent];throw error;}
    await this.broadcast(job);return publicJob(job);
  });}
  async broadcast(job){
    if(terminal(job)||Number(job.attempts||0)>=3)return;
    job.attempts=Number(job.attempts||0)+1;
    job.last_attempt=Date.now();this.journal.save();
    try{await this.client.rpc('push_transaction',job.packed);job.status='submitted';delete job.last_error;}
    catch(e){job.last_error=String(e.message).slice(0,300);}
    this.journal.save();
  }
  async reconcile(){return this.exclusive(async()=>{
    const jobs=Object.values(this.journal.jobs).filter(j=>!terminal(j));if(!jobs.length)return;
    const info=await this.client.rpc('get_info');
    if(info.chain_id!==this.client.config.chain_id)throw new Error('Node chain changed');
    const lib=info.last_irreversible_block_num;
    const cache=new Map();let budget=100;
    for(const job of jobs){
      while(job.next_block<=lib&&budget>0){
        const n=job.next_block;
        let block=cache.get(n);if(!block){block=await this.client.rpc('get_block',{block_num_or_id:n});cache.set(n,block);budget--;}
        const receipt=block.transactions.find(r=>(typeof r.trx==='string'?r.trx:r.trx.id)===job.id);
        if(receipt){
          if(receipt.status!=='executed')throw new Error('Unexpected non-executed receipt; operator review required');
          job.status='finalized';job.confirmation={block_num:n,block_id:block.id,observed_lib:lib};delete job.last_error;break;
        }
        job.next_block++;
        // Native transaction expiration is fixed by the holder, before relay submission.
        // Decode only our stored original packet, never re-sign it.
        if(n===lib&&Date.parse(block.timestamp+'Z')>Date.parse(job.expiration+'Z')){job.status='expired';break;}
      }
      if(!terminal(job)&&Date.now()-(job.last_attempt||0)>10000)await this.broadcast(job);
    }
    this.journal.save();
  });}
}
export function createServer(relay,{host='127.0.0.1',port=8787}={}){
  const clients=new Map();
  const server=http.createServer(async(req,res)=>{
    const send=(status,data)=>{res.writeHead(status,{'content-type':'application/json','cache-control':'no-store'});res.end(JSON.stringify(data));};
    const ip=req.socket.remoteAddress,minute=Math.floor(Date.now()/60000);
    let rate=clients.get(ip);
    if(!rate||rate.minute!==minute){
      if(!rate&&clients.size>=10000)for(const[k,v]of clients)if(v.minute!==minute)clients.delete(k);
      if(!rate&&clients.size>=10000)return send(429,{error:'Rate tracker capacity reached'});
      clients.set(ip,rate={minute,count:0});
    }
    if(++rate.count>30)return send(429,{error:'Rate limit exceeded'});
    try{
      if(req.method==='GET'&&req.url==='/health')return send(200,{bp:relay.bp,permission:relay.permission,contract:'eosio.key',fee_bps:await relay.client.relayRate(relay.bp),minimum_fee_bps:1000,maximum_fee_bps:3000,minimum_service_fee:'0.0001 AIC'});
      if(req.method==='GET'&&/^\/status\/[0-9a-f]{64}$/.test(req.url)){
        const job=relay.journal.jobs[req.url.slice(8)];return send(job?200:404,job?publicJob(job):{error:'Unknown intent'});
      }
      if(req.method!=='POST'||req.url!=='/submit')return send(404,{error:'Unknown route'});
      if(!String(req.headers['content-type']).startsWith('application/json'))return send(415,{error:'JSON required'});
      const chunks=[];let length=0;
      for await(const chunk of req){length+=chunk.length;if(length>8192)return send(413,{error:'Request too large'});chunks.push(chunk);}
      const body=new TextDecoder('utf-8',{fatal:true}).decode(Buffer.concat(chunks));
      const result=await relay.submit(JSON.parse(body));send(202,result);
    }catch(e){send(400,{error:String(e.message).slice(0,300)});}
  });
  server.requestTimeout=20000;server.headersTimeout=10000;server.maxConnections=32;
  return server.listen(port,host);
}
async function main(){
  const path=process.env.RELAY_KEY_FILE;if(!path||statSync(path).mode&0o077)throw new Error('RELAY_KEY_FILE must be readable only by its owner');
  const journal=new Journal(process.env.RELAY_STATE||'./state');
  const relay=new Relay({endpoint:process.env.NODEOS_URL,readEndpoint:process.env.NODEOS_READ_URL,bp:process.env.BP_ACCOUNT,permission:process.env.BP_PERMISSION||'keyrelay',
    key:readFileSync(path,'utf8').trim(),allowedTokens:(process.env.RELAY_ALLOWED_TOKENS||'').split(',').filter(Boolean),expectedHash:process.env.KEY_CODE_HASH,journal});
  await relay.profile();const server=createServer(relay,{port:Number(process.env.PORT||8787)});
  const timer=setInterval(()=>relay.reconcile().catch(e=>process.stderr.write(`Relay reconciliation paused: ${e.message}\n`)),2000);
  const shutdown=()=>{clearInterval(timer);server.close(()=>{journal.close();process.exit(0);});};
  process.on('SIGINT',shutdown);process.on('SIGTERM',shutdown);
  process.stdout.write(`BP relay ${relay.bp}@${relay.permission} listening on loopback\n`);
}
if(process.argv[1]&&import.meta.url===pathToFileURL(resolve(process.argv[1])).href)main().catch(e=>{process.stderr.write(e.message+'\n');process.exit(1);});
