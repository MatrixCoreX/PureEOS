// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
// Reference BP relay: loopback HTTP behind an HTTPS proxy. No user private keys.
import http from 'node:http';
import {readFileSync,statSync,mkdirSync,openSync,writeFileSync,fsyncSync,closeSync,renameSync,existsSync,unlinkSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {pathToFileURL} from 'node:url';
import {PrivateKey,PublicKey} from '@wharfkit/antelope';
import {KeyWalletClient,intentDigest,verifyIntent,validateRequest,atomicAsset} from '../sdk/index.mjs';

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
  constructor({endpoint,bp,permission,key,expectedHash,journal,client}){
    if(!/^[0-9a-f]{64}$/.test(expectedHash||''))throw new Error('Pin the qualified eosio.key code hash');
    this.client=client||new KeyWalletClient(endpoint);this.bp=bp;this.permission=permission;
    this.key=PrivateKey.from(key);this.expectedHash=expectedHash;this.journal=journal;this.queue=Promise.resolve();this.pending=0;
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
    if(!body||Object.keys(body).sort().join(',')!=='operation,request,signature')throw new Error('Only operation, request and signature are accepted');
    const {operation,request,signature}=body;
    if(!['ksend','withdraw','cancel'].includes(operation))throw new Error('Unsupported commercial relay action');
    validateRequest(operation,request);
    if(request.relayer!==this.bp||request.reward_to!==this.bp)throw new Error('Wrong selected BP');
    // An already irreversible/expired packet is a read of our durable journal,
    // not new relay work. It remains queryable during RPC outages/revocation.
    if(this.client.config&&this.client.abi&&verifyIntent(this.client.abi,this.client.config,operation,request,signature)){
      const cached=this.journal.jobs[intentDigest(this.client.abi,this.client.config,operation,request).toString()];
      if(cached&&terminal(cached))return publicJob(cached);
    }
    await this.profile();
    if(request.relayer!==this.bp||request.reward_to!==this.bp||!verifyIntent(this.client.abi,this.client.config,operation,request,signature))throw new Error('Invalid owner authorization');
    const intent=intentDigest(this.client.abi,this.client.config,operation,request).toString();
    const existing=this.journal.jobs[intent];if(existing){if(!terminal(existing))await this.broadcast(existing);return publicJob(existing);}
    if(Object.keys(this.journal.jobs).length>=100000)throw new Error('Journal capacity reached; archive completed jobs');
    const wallet=await this.client.wallet(request.from);
    if(!wallet||BigInt(wallet.next_nonce)!==BigInt(request.nonce))throw new Error('Nonce is stale or out of order');
    for(const job of Object.values(this.journal.jobs))if(!terminal(job)&&job.wallet_id===String(wallet.id)&&job.nonce===String(request.nonce))throw new Error('This nonce already has a pending relay transaction');
    const info=await this.client.rpc('get_info');const now=Math.floor(Date.parse(info.head_block_time+'Z')/1000);
    if(request.expires<now+10||request.expires>now+3598)throw new Error('Owner intent expiration must allow submission within one hour');
    const fee=await this.client.quote(operation,request);
    if(fee.total>atomicAsset(request.max_fee).units)throw new Error('Current fee exceeds owner maximum');
    const aic=await this.client.balance(request.from,this.client.config.fee_contract,'4,AIC');
    const principal=operation==='cancel'?null:atomicAsset(request.quantity);
    const aicPrincipal=principal&&request.token_contract===this.client.config.fee_contract&&principal.symbol==='4,AIC'?principal.units:0n;
    if(aic<fee.total+aicPrincipal)throw new Error('Insufficient additional AIC fee balance');
    if(principal&&await this.client.balance(request.from,request.token_contract,principal.symbol)<principal.units)throw new Error('Insufficient principal');
    const prepared=await this.client.prepareRelayTransaction(operation,request,signature,this.bp,this.permission,this.key);
    const job={...prepared,intent,status:'pending',wallet_id:String(wallet.id),nonce:String(request.nonce),
      next_block:prepared.startLIB+1,created_at:new Date().toISOString(),operation,request,signature};
    // Persist the EXACT native packet before its first broadcast. Retries never
    // choose a new TAPOS/expiration or create a second packet for this intent.
    this.journal.jobs[intent]=job;this.journal.save();await this.broadcast(job);return publicJob(job);
  });}
  async broadcast(job){
    if(terminal(job))return;
    job.last_attempt=Date.now();
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
        // Native transaction expiration is fixed by prepareRelayTransaction.
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
      if(req.method==='GET'&&req.url==='/health')return send(200,{bp:relay.bp,permission:relay.permission,contract:'eosio.key'});
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
  const relay=new Relay({endpoint:process.env.NODEOS_URL,bp:process.env.BP_ACCOUNT,permission:process.env.BP_PERMISSION||'keyrelay',
    key:readFileSync(path,'utf8').trim(),expectedHash:process.env.KEY_CODE_HASH,journal});
  await relay.profile();const server=createServer(relay,{port:Number(process.env.PORT||8787)});
  const timer=setInterval(()=>relay.reconcile().catch(e=>process.stderr.write(`Relay reconciliation paused: ${e.message}\n`)),2000);
  const shutdown=()=>{clearInterval(timer);server.close(()=>{journal.close();process.exit(0);});};
  process.on('SIGINT',shutdown);process.on('SIGTERM',shutdown);
  process.stdout.write(`BP relay ${relay.bp}@${relay.permission} listening on loopback\n`);
}
if(process.argv[1]&&import.meta.url===pathToFileURL(resolve(process.argv[1])).href)main().catch(e=>{process.stderr.write(e.message+'\n');process.exit(1);});
