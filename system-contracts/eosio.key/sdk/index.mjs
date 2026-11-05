// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
import {createHash,randomBytes} from 'node:crypto';
import {Asset,Name,Checksum256,PrivateKey,PublicKey,Serializer,Signature,Transaction,SignedTransaction,PackedTransaction} from '@wharfkit/antelope';
import {ramFeeProfile} from './fee-profiles.mjs';
export const CONTRACT='eosio.key',TAG='PureEOS:eosio.key:transaction';
const LIMIT=(1n<<62n)-1n;
const REQUESTS={ksend:'transfer_request',withdraw:'withdrawal_request',selfout:'withdrawal_request',cancel:'cancellation_request',selfcancel:'cancellation_request'};
const common=[['auth_epoch','uint64'],['request_id','checksum256'],['relayer','name'],['reward_to','name'],['max_fee','asset'],['relay_fee_bps','uint16$']];
const schema={
 deposit:[['from','name'],['token_contract','name'],['to','public_key'],['quantity','asset'],['max_fee','asset'],['request_id','checksum256']],
 createkey:[['request_id','checksum256']],
 token_transfer:[['from','name'],['to','name'],['quantity','asset'],['memo','string']],
 transfer_request:[['from','public_key'],['to','public_key'],['token_contract','name'],['quantity','asset'],['memo','string'],...common],
 withdrawal_request:[['from','public_key'],['token_contract','name'],['to','name'],['quantity','asset'],['memo','string'],...common],
 cancellation_request:[['from','public_key'],['auth_epoch','uint64'],['next_epoch','uint64'],['request_id','checksum256'],['relayer','name'],['reward_to','name'],['max_fee','asset'],['relay_fee_bps','uint16$']],
 prove:[['owner','public_key'],['chain_id','checksum256'],['deployment','checksum256'],['reference_block_num','uint32']],
 proof_envelope:[['tag','string'],['version','uint32'],['chain_id','checksum256'],['contract','name'],['deployment','checksum256'],['transaction_id','checksum256']],
};
for(const [op,type] of Object.entries(REQUESTS))schema[op]=[...(op.startsWith('self')?[['payer','name']]:[]),['request',type]];
// Signing schemas are bundled protocol definitions. Untrusted RPC ABIs never
// determine the bytes a holder authorizes.
export const CORE_ABI={version:'eosio::abi/1.2',types:[],structs:Object.entries(schema).map(([name,fields])=>({name,base:'',fields:fields.map(([name,type])=>({name,type}))})),actions:[],tables:[]};
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
const encode=(type,object)=>Serializer.encode({abi:CORE_ABI,type,object});
function exact(value,fields){if(!value||typeof value!=='object'||Array.isArray(value)||Object.keys(value).sort().join(',')!==[...fields].sort().join(','))throw new Error('Fields must match protocol schema exactly');}
function u64(value){if(typeof value==='number'&&!Number.isSafeInteger(value))throw new Error('Exact uint64 required');if(!/^(0|[1-9][0-9]*)$/.test(String(value))||BigInt(value)>=1n<<64n)throw new Error('Invalid uint64');return BigInt(value);}
function hash(value){if(typeof value!=='string'||! /^[0-9a-f]{64}$/.test(value)||/^0+$/.test(value))throw new Error('Nonzero lowercase 32-byte hex required');return value;}
function nativeName(s,empty=false){if(typeof s!=='string'||(!s&&!empty)||Name.from(s).toString()!==s)throw new Error('Canonical native name required');return s;}
export const randomRequestId=()=>randomBytes(32).toString('hex');
export function parseRPCJSON(text){return JSON.parse(text,(key,value,context)=>{if(typeof value==='number'&&Number.isInteger(value)&&!Number.isSafeInteger(value)){if(!context?.source||! /^-?\d+$/.test(context.source))throw new Error('RPC contains an unsafe numeric value');return context.source;}return value;});}
export function normalizePublicKey(key){const p=PublicKey.from(key);if(p.type!=='K1')throw new Error('K1 public key required');return p.toString();}
export function keyHash(key){return sha(Serializer.encode({type:'public_key',object:normalizePublicKey(key)}).array);}
export function assetIdentity(tokenContract,tokenSymbol){return sha(Serializer.encode({abi:{...CORE_ABI,structs:[{name:'identity',base:'',fields:[{name:'token',type:'name'},{name:'symbol',type:'symbol'}]}]},type:'identity',object:{token:tokenContract,symbol:tokenSymbol}}).array);}
export function atomicAsset(quantity){const a=Asset.from(quantity),units=BigInt(a.units.toString());if(units< -LIMIT||units>LIMIT)throw new Error('Asset amount out of range');return{units,symbol:a.symbol.toString(),text:a.toString()};}
export function formatFee(value){value=BigInt(value);if(value<0n||value>LIMIT)throw new Error('Invalid fee amount');return`${value/10000n}.${String(value%10000n).padStart(4,'0')} AIC`;}
export function depositMemo(key,maximumFee,id=randomRequestId()){const cap=atomicAsset(maximumFee);if(cap.symbol!=='4,AIC'||cap.units<0n)throw new Error('AIC fee cap required');return`PK2|${normalizePublicKey(key)}|${cap.units}|${hash(id)}`;}
export function nativeDepositAction(request,contract=CONTRACT) {
  exact(request,['from','token_contract','to','quantity','max_fee','request_id']);
  nativeName(request.from);nativeName(request.token_contract);nativeName(contract);
  normalizePublicKey(request.to);hash(request.request_id);
  const q=atomicAsset(request.quantity),cap=atomicAsset(request.max_fee);
  if(q.units<=0n||cap.symbol!=='4,AIC'||cap.units<0n||request.from===contract)throw new Error('Invalid direct deposit');
  return {account:contract,name:'deposit',authorization:[{actor:request.from,permission:'active'}],data:encode('deposit',request).hexString};
}
export function nativeDepositActions(request,{contract=CONTRACT,feeFunding=request.max_fee}={}) {
  const control=nativeDepositAction(request,contract),fund=atomicAsset(feeFunding),cap=atomicAsset(request.max_fee);
  if(fund.symbol!=='4,AIC'||fund.units<=0n||fund.units>cap.units)throw new Error('Invalid direct deposit funding');
  const transfer=(token,quantity,memo)=>({account:token,name:'transfer',authorization:[{actor:request.from,permission:'active'}],data:encode('token_transfer',{from:request.from,to:contract,quantity,memo}).hexString});
  return [control,transfer('eosio.token',fund.text,'atomic key fee'),transfer(request.token_contract,request.quantity,'atomic key principal')];
}
export function ramRegistrationActions(payer,tokenContract,tokenSymbol,ramPayment,{minimumBytes=4096,contract=CONTRACT}={}) {
  nativeName(payer);nativeName(tokenContract);nativeName(contract);
  if(payer===contract||tokenContract===contract)throw new Error('External RAM payer and token issuer required');
  const payment=atomicAsset(ramPayment),symbol=Asset.Symbol.from(tokenSymbol).toString();
  if(symbol!==tokenSymbol||payment.symbol!=='4,AIC'||payment.units<=0n)throw new Error('Canonical token symbol and positive AIC RAM purchase required');
  if(!Number.isInteger(minimumBytes)||minimumBytes<4096||minimumBytes>1073741824)throw new Error('Invalid minimum RAM bytes');
  const authorization=[{actor:payer,permission:'active'}];
  return [{account:'eosio',name:'buyram',authorization,data:{payer,receiver:contract,quant:payment.text}},
          {account:contract,name:'regtoken',authorization:[...authorization],data:{payer,token_contract:tokenContract,token_symbol:tokenSymbol,min_bytes:minimumBytes}}];
}
export function validateRequest(operation,request){const type=REQUESTS[operation];if(!type)throw new Error('Unsupported wallet operation');exact(request,schema[type].filter(([name,type])=>!type.endsWith('$')||Object.hasOwn(request,name)).map(x=>x[0]));if(Object.hasOwn(request,'relay_fee_bps')){const b=request.relay_fee_bps;if(!Number.isInteger(b)||(operation.startsWith('self')?b!==0:b<1000||b>3000))throw new Error('Relay fee must be 10% to 30%, or zero for self operations');}normalizePublicKey(request.from);if(operation==='ksend')normalizePublicKey(request.to);u64(request.auth_epoch);hash(request.request_id);nativeName(request.relayer,true);nativeName(request.reward_to,true);if(type!=='cancellation_request'){nativeName(request.token_contract);if(operation!=='ksend')nativeName(request.to);}const cap=atomicAsset(request.max_fee);if(cap.symbol!=='4,AIC'||cap.units<0n)throw new Error('Invalid AIC fee cap');if(type==='cancellation_request'){if(u64(request.next_epoch)<=u64(request.auth_epoch))throw new Error('Invalid next epoch');}else{if(atomicAsset(request.quantity).units<=0n||typeof request.memo!=='string'||Buffer.byteLength(request.memo)>128)throw new Error('Invalid principal or memo');}if(operation.startsWith('self')){if(request.relayer||request.reward_to)throw new Error('Self operation cannot pay relay');}else if(!request.relayer||request.reward_to!==request.relayer)throw new Error('Selected BP and reward identity must match');return type;}
export function isKeySignup(operation,request) {
 return ['withdraw','selfout'].includes(operation)&&request.to==='eosio.signup';
}
function signupContinuation(operation,request,config) {
 if(!isKeySignup(operation,request))return [];
 const amount=atomicAsset(request.quantity);
 if(request.token_contract!==config.fee_contract||amount.symbol!==config.fee_symbol||config.fee_contract!=='eosio.token'||config.fee_symbol!=='4,AIC')throw new Error('Registration requires native AIC');
 return [{account:'eosio.signup',name:'createkey',authorization:[],data:encode('createkey',{request_id:request.request_id}).hexString}];
}
export function ownerDigest(config,transaction,contract=CONTRACT){if(Number(config.version)!==2)throw new Error('Unsupported owner proof version');return Checksum256.from(sha(encode('proof_envelope',{tag:TAG,version:2,chain_id:hash(config.chain_id),contract,deployment:hash(config.deployment),transaction_id:Transaction.from(transaction).id}).array));}
export function prepareEnvelope(config,info,operation,request,{payer='',permission='keyrelay',validSeconds=120,contract=CONTRACT}={}){
 validateRequest(operation,request);if(info.chain_id!==config.chain_id)throw new Error('Node chain mismatch');if(!Number.isInteger(validSeconds)||validSeconds<1||validSeconds>3500)throw new Error('Invalid transaction lifetime');
 const self=operation.startsWith('self'),actor=self?payer:request.relayer;nativeName(actor);nativeName(permission);
 const reference=Number(info.last_irreversible_block_num),block=Buffer.from(info.last_irreversible_block_id,'hex');if(!Number.isSafeInteger(reference)||reference<=0||block.length!==32||block.readUInt32BE(0)!==reference)throw new Error('Invalid irreversible reference block');
 const tx=Transaction.from({expiration:new Date(Date.parse(info.head_block_time+'Z')+validSeconds*1000).toISOString().slice(0,19),ref_block_num:reference&65535,ref_block_prefix:block.readUInt32LE(8),max_net_usage_words:0,max_cpu_usage_ms:0,delay_sec:0,transaction_extensions:[],
 context_free_actions:[{account:contract,name:'prove',authorization:[],data:encode('prove',{owner:request.from,chain_id:config.chain_id,deployment:config.deployment,reference_block_num:reference}).hexString}],
 actions:[{account:contract,name:operation,authorization:[{actor,permission}],data:encode(operation,self?{payer,request}:{request}).hexString},...signupContinuation(operation,request,config)]});
 return{version:2,chain_id:config.chain_id,deployment:config.deployment,operation,request,payer:self?payer:'',permission,reference_block_num:reference,transaction:JSON.parse(JSON.stringify(tx)),transaction_id:tx.id.toString(),digest:ownerDigest(config,tx,contract).toString()};
}
export function verifyPrepared(prepared,config,contract=CONTRACT){
 exact(prepared,['version','chain_id','deployment','operation','request','payer','permission','reference_block_num','transaction','transaction_id','digest']);
 if(prepared.version!==2||prepared.chain_id!==config.chain_id||prepared.deployment!==config.deployment)throw new Error('Prepared domain mismatch');
 const {operation,request,payer,permission,reference_block_num:reference}=prepared;validateRequest(operation,request);const self=operation.startsWith('self');nativeName(permission);nativeName(payer,true);if(self?!payer:!!payer)throw new Error('Invalid payer');
 const continuation=signupContinuation(operation,request,config);
 const tx=Transaction.from(prepared.transaction);if(tx.actions.length!==1+continuation.length||tx.context_free_actions.length!==1||Number(tx.delay_sec)!==0||tx.transaction_extensions.length||Number(tx.max_net_usage_words)!==0||Number(tx.max_cpu_usage_ms)!==0)throw new Error('Unexpected native transaction fields');
 // Reject ignored JSON fields or structured ABI-dependent action objects.
 exact(prepared.transaction,['expiration','ref_block_num','ref_block_prefix','max_net_usage_words','max_cpu_usage_ms','delay_sec','context_free_actions','actions','transaction_extensions']);
 for(const a of [...prepared.transaction.actions,...prepared.transaction.context_free_actions]){exact(a,['account','name','authorization','data']);if(typeof a.data!=='string')throw new Error('Raw canonical action bytes required');for(const p of a.authorization)exact(p,['actor','permission']);}
 if(!Number.isInteger(reference)||reference<=0||reference>0xffffffff||(reference&65535)!==Number(tx.ref_block_num))throw new Error('Invalid signed reference block');
 const proof=tx.context_free_actions[0],root=tx.actions[0];
 if(proof.account.toString()!==contract||proof.name.toString()!=='prove'||proof.authorization.length||proof.data.hexString!==encode('prove',{owner:request.from,chain_id:config.chain_id,deployment:config.deployment,reference_block_num:reference}).hexString)throw new Error('Proof action mismatch');
 if(root.account.toString()!==contract||root.name.toString()!==operation||root.authorization.length!==1||root.authorization[0].actor.toString()!==(self?payer:request.relayer)||root.authorization[0].permission.toString()!==permission||root.data.hexString!==encode(operation,self?{payer,request}:{request}).hexString)throw new Error('Root operation mismatch');
 if(continuation.length){const c=tx.actions[1];if(c.account.toString()!=='eosio.signup'||c.name.toString()!=='createkey'||c.authorization.length||c.data.hexString!==continuation[0].data)throw new Error('Atomic registration continuation mismatch');}
 if(Serializer.encode({type:Transaction,object:tx}).array.length>16384||tx.id.toString()!==prepared.transaction_id||ownerDigest(config,tx,contract).toString()!==prepared.digest)throw new Error('Prepared transaction hash mismatch');return tx;
}
export function signPrepared(prepared,config,userPrivateKey,contract=CONTRACT){verifyPrepared(prepared,config,contract);const key=PrivateKey.from(userPrivateKey);if(!key.toPublic().equals(PublicKey.from(prepared.request.from)))throw new Error('Signing key does not own sender wallet');return{prepared,owner_signature:key.signDigest(prepared.digest).toString()};}
export function verifySigned(body,config,contract=CONTRACT){exact(body,['prepared','owner_signature']);const tx=verifyPrepared(body.prepared,config,contract),sig=Signature.from(body.owner_signature);if(sig.type!=='K1'||!sig.verifyDigest(body.prepared.digest,PublicKey.from(body.prepared.request.from)))throw new Error('Invalid owner signature');return tx;}
export function resourcePacket(body,config,payerPrivateKey,contract=CONTRACT){const tx=verifySigned(body,config,contract),cfd=[Serializer.encode({type:'signature',object:body.owner_signature}).hexString];const raw=Serializer.encode({type:Transaction,object:tx}).array,cfhash=Buffer.from(sha(Serializer.encode({type:'bytes[]',object:cfd}).array),'hex');const digest=Checksum256.from(sha(Buffer.concat([Buffer.from(config.chain_id,'hex'),Buffer.from(raw),cfhash])));const signed=SignedTransaction.from({...tx.toJSON(),context_free_data:cfd,signatures:[PrivateKey.from(payerPrivateKey).signDigest(digest).toString()]});return{id:tx.id.toString(),expiration:tx.expiration.toString(),startLIB:body.prepared.reference_block_num,packed:PackedTransaction.fromSigned(signed,0).toJSON()};}

function fraction(resource) {
  const capacity = BigInt(resource.weight), adjusted = BigInt(resource.adjusted_utilization), used = BigInt(resource.utilization);
  if (Number(resource.version)!==0 || capacity<=0n || used<0n || used>capacity || adjusted<0n || adjusted>capacity) throw new Error('Unsupported resource market');
  return (adjusted>used?adjusted:used)*10000n/capacity;
}
export function marketMultiplier(policy,market) {
  if (Number(market.version)!==1) throw new Error('Unsupported resource-split market version');
  const cpu = fraction(market.cpu),net=fraction(market.net),use=cpu>net?cpu:net;
  return {multiplier:10000n+(BigInt(policy.maximum_multiplier_bps)-10000n)*use*use/100000000n,cpu,net};
}
export function calculateFee(policy,multiplier,units,rows,relayed,relayFeeBps=3000) {
  if(relayed&&(!Number.isInteger(relayFeeBps)||relayFeeBps<1000||relayFeeBps>3000))throw new Error('Relay fee must be 10% to 30%');
  if (!Number.isInteger(units)||units<0||units>128||!Number.isInteger(rows)||rows<0||rows>4) throw new Error('Invalid work bound');
  const numerator=(BigInt(policy.unit_fee)*BigInt(units)+BigInt(policy.new_row_fee)*BigInt(rows))*BigInt(multiplier);
  const computed=(numerator+9999n)/10000n;
  const base=computed>BigInt(policy.minimum_fee)?computed:BigInt(policy.minimum_fee);
  const premium=relayed?(base*BigInt(relayFeeBps)+9999n)/10000n:0n;
  if(base<=0n||base+premium>LIMIT)throw new Error('Fee out of range');
  return {base,premium,total:base+premium,base_asset:formatFee(base),premium_asset:formatFee(premium),total_asset:formatFee(base+premium)};
}

export function allocationBytes(rows) {
  if(!Number.isInteger(rows)||rows<0||rows>2)throw new Error('Invalid recipient allocation');
  return [0,512,1536][rows];
}
export function ramMarketQuote(market,bytes,atomicPurchase=false) {
  if(!Number.isInteger(bytes)||bytes<0||bytes>4096)throw new Error('Invalid RAM byte budget');
  if(!bytes)return 0n;
  const amount=(text,symbol)=>{
    if(typeof text!=='string')throw new Error('Invalid RAM market asset');
    const value=atomicAsset(text);
    if(value.symbol!==symbol||value.units<=0n||value.text!==text)throw new Error('Invalid RAM market asset');
    return value.units;
  };
  amount(market?.supply,'4,RAMCORE');
  const reserve=amount(market?.base?.balance,'0,RAM'),quote=amount(market?.quote?.balance,'4,AIC');
  const half=value=>value===0.5||typeof value==='string'&&/^0\.50*$/.test(value);
  if(!half(market.base.weight)||!half(market.quote.weight)||reserve<=BigInt(bytes))throw new Error('Unsupported or invalid RAM market');
  const remaining=reserve-BigInt(bytes),net=(quote*BigInt(bytes)+remaining-1n)/remaining+(atomicPurchase?1n:0n);
  const gross=(net*200n+198n)/199n;
  if(gross>LIMIT)throw new Error('RAM quote out of range');
  return gross;
}
export function calculateMarketFee(policy,multiplier,units,bytes,market,relayed,relayFeeBps=3000,atomicPurchase=false) {
  if(!Number.isInteger(units)||units<0||units>128)throw new Error('Invalid work bound');
  multiplier=BigInt(multiplier);
  if(multiplier<10000n||multiplier>BigInt(policy.maximum_multiplier_bps))throw new Error('Invalid fee multiplier');
  if(relayed&&(!Number.isInteger(relayFeeBps)||relayFeeBps<1000||relayFeeBps>3000))throw new Error('Relay fee must be 10% to 30%');
  const execution=(BigInt(policy.unit_fee)*BigInt(units)*multiplier+9999n)/10000n;
  const storage=ramMarketQuote(market,bytes,atomicPurchase),computed=execution+storage;
  const base=computed>BigInt(policy.minimum_fee)?computed:BigInt(policy.minimum_fee);
  const premium=relayed?(base*BigInt(relayFeeBps)+9999n)/10000n:0n;
  if(execution<0n||base<=0n||base+premium>LIMIT)throw new Error('Fee out of range');
  return {base,premium,total:base+premium,execution,storage,supplement:base-computed,ram_bytes:bytes,
    base_asset:formatFee(base),premium_asset:formatFee(premium),total_asset:formatFee(base+premium)};
}

const READ_RPC_METHODS=new Set(['get_info','get_block','get_account','get_abi','get_code_hash','get_table_rows','get_currency_balance','get_currency_stats','get_required_keys','get_transaction_status']);
export class KeyWalletClient {
  constructor(endpoint,{contract=CONTRACT,readEndpoint=endpoint,fetchImpl=globalThis.fetch}={}) {
    this.endpoint=endpoint.replace(/\/+$/,'');this.readEndpoint=readEndpoint.replace(/\/+$/,'');this.contract=contract;this.fetch=fetchImpl;
  }
  async rpcAt(endpoint,method,data={}) {
    const result=await this.fetch(endpoint+'/v1/chain/'+method,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(data),signal:AbortSignal.timeout(20000)});
    if(!result.ok)throw new Error(`RPC ${method}: HTTP ${result.status}`);
    return parseRPCJSON(await result.text());
  }
  async rpc(method,data={}) {return this.rpcAt(READ_RPC_METHODS.has(method)?this.readEndpoint:this.endpoint,method,data);}
  async rpcWriter(method,data={}) {return this.rpcAt(this.endpoint,method,data);}
  async rows(table,scope=this.contract,extra={}) {
    const value=await this.rpc('get_table_rows',{json:true,code:this.contract,scope,table,limit:1000,...extra});
    if(value.more)throw new Error('Truncated wallet query; explicit pagination required');
    return value.rows;
  }
  async load() {
    const [info,configs]=await Promise.all([this.rpc('get_info'),this.rows('config')]);
    if(configs.length!==1||configs[0].chain_id!==info.chain_id||Number(configs[0].version)!==2)throw new Error('Wallet domain does not match actual node chain');
    if(this.readEndpoint!==this.endpoint&&(await this.rpcWriter('get_info')).chain_id!==info.chain_id)throw new Error('Read and write RPC chain IDs differ');
    this.config=configs[0];this.abi=CORE_ABI;return this;
  }
  async relayRate(bp) {
    nativeName(bp);
    const rows=await this.rows('relayrates',this.contract,{lower_bound:bp,upper_bound:bp,limit:2});
    if(rows.length>1||rows.length&&rows[0].bp!==bp)throw new Error('Invalid relay rate identity');
    const rate=rows.length?Number(rows[0].fee_bps):3000;
    if(!Number.isInteger(rate)||rate<1000||rate>3000)throw new Error('Relay fee must be 10% to 30%');
    return rate;
  }
  async wallet(key) {
    const hash=keyHash(key);
    const rows=await this.rows('wallets',this.contract,{index_position:2,key_type:'sha256',lower_bound:hash,upper_bound:hash,limit:2});
    if(!rows.length)return null;
    if(rows.length!==1||normalizePublicKey(rows[0].key)!==normalizePublicKey(key))throw new Error('Wallet index identity mismatch');
    return rows[0];
  }
  async asset(tokenContract,symbol) {
    const identity=assetIdentity(tokenContract,symbol);
    const rows=await this.rows('assets',this.contract,{index_position:2,key_type:'sha256',lower_bound:identity,upper_bound:identity,limit:2});
    if(!rows.length)return null;
    if(rows.length!==1||rows[0].token_contract!==tokenContract||rows[0].token_symbol!==symbol)throw new Error('Asset identity mismatch');
    return rows[0];
  }
  async balance(key,tokenContract,symbol) {
    const [wallet,asset]=await Promise.all([this.wallet(key),this.asset(tokenContract,symbol)]);
    if(!wallet||!asset)return 0n;
    const rows=await this.rows('balances',String(wallet.id),{lower_bound:String(asset.id),upper_bound:String(asset.id),limit:2});
    return rows.length?BigInt(rows[0].amount):0n;
  }
  async quote(operation,request,{recipientKey=null}={}) {
    if(!this.config)await this.load();
    const cfg=this.config,info=await this.rpc('get_info');
    // Claims remain independent of both resource markets and pricing profiles.
    if(operation==='claim'){
      const amount=atomicAsset(request.quantity);
      if(amount.symbol!==cfg.fee_symbol||amount.units<=0n)throw new Error('Claim requires positive native AIC income');
      if(await this.nativeRows(cfg.fee_contract,request.to,request.quantity))throw new Error('Claim recipient must open AIC balance with claimant-paid RAM');
      return {base:0n,premium:0n,total:0n,multiplier:10000n,units:0,rows:0,head:info.head_block_num};
    }
    const [code,walletCode]=await Promise.all([
      this.rpc('get_code_hash',{account_name:'eosio'}),
      this.rpc('get_code_hash',{account_name:this.contract}),
    ]);
    const ramPricing=ramFeeProfile(walletCode.code_hash);
    const exit=['selfout','selfcancel','refundfee'].includes(operation);
    let multiplier=10000n;
    if(code.code_hash!==cfg.system_hash){if(!exit)throw new Error('System code changed; relay profile needs review');}
    else {
      const now=Math.floor((Date.parse(info.head_block_time+'Z')+500)/1000);
      const epoch=Math.floor(now/Number(cfg.policy.epoch_seconds));
      const cached=await this.rows('pricing');
      if(cached.length&&BigInt(cached[0].epoch)===BigInt(epoch))multiplier=BigInt(cached[0].multiplier_bps);
      else {
        const value=await this.rpc('get_table_rows',{json:true,code:'eosio',scope:'0',table:'powup.state',limit:1});
        if(value.rows.length!==1)throw new Error('PowerUp market unavailable');
        multiplier=marketMultiplier(cfg.policy,value.rows[0]).multiplier;
      }
    }
    let units,rows=0;
    if(operation==='ksend'||operation==='deposit'){
      const symbol=atomicAsset(request.quantity).symbol;
      const key=recipientKey||request.to;
      const [wallet,asset]=await Promise.all([this.wallet(key),this.asset(request.token_contract,symbol)]);
      if(ramPricing!=='atomic'&&(!asset||!BigInt(asset.ram_purchased||0)))throw new Error('Token requires paid RAM registration before receiving funds');
      if(!wallet)rows=2;
      else if(!asset||(await this.rows('balances',String(wallet.id),{lower_bound:String(asset.id),upper_bound:String(asset.id),limit:2})).length===0)rows=1;
      units=(ramPricing==='atomic'?8:(operation==='deposit'?2:4))+Math.ceil(Buffer.byteLength(request.memo||'','utf8')/64);
    }else if(operation==='withdraw'||operation==='selfout'){
      units=(ramPricing==='atomic'?10:6)+Math.ceil(Buffer.byteLength(request.memo||'','utf8')/64);rows=await this.nativeRows(request.token_contract,request.to,request.quantity);
      if(ramPricing!=='atomic'&&rows&&request.token_contract!==cfg.fee_contract)throw new Error('Recipient must open the third-party token balance before withdrawal');
    }
    else if(operation==='cancel'||operation==='selfcancel')units=3;
    else if(operation==='refundfee'){units=4;rows=await this.nativeRows(cfg.fee_contract,request.to,request.quantity);}
    else throw new Error('Unsupported fee operation');
    const relayed=['ksend','withdraw','cancel'].includes(operation),rate=relayed?await this.relayRate(request.relayer):0;
    if(Object.hasOwn(request,'relay_fee_bps')&&request.relay_fee_bps!==rate)throw new Error('Relay rate changed; sign an updated quote');
    let fee;
    if(ramPricing==='market'||ramPricing==='atomic'){
      let bytes=allocationBytes(rows);let market=null;
      if(ramPricing==='atomic'&&['ksend','deposit'].includes(operation)){
        const a=await this.asset(request.token_contract,atomicAsset(request.quantity).symbol);
        if(request.token_contract!==cfg.fee_contract&&(!a||!BigInt(a.ram_purchased||0)))bytes+=2048;
      }
      if(bytes){
        if(code.code_hash!==cfg.system_hash)throw new Error('System code changed; pre-open recipient RAM before exiting');
        const result=await this.rpc('get_table_rows',{json:true,code:'eosio',scope:'eosio',table:'rammarket',limit:2});
        if(result.more||result.rows?.length!==1)throw new Error('RAM market unavailable');
        market=result.rows[0];
      }
      fee=calculateMarketFee(cfg.policy,multiplier,units,bytes,market,relayed,rate,ramPricing==='atomic');
    }else fee=calculateFee(cfg.policy,multiplier,units,rows,relayed,rate);
    const finalCode=await this.rpc('get_code_hash',{account_name:this.contract});
    if(finalCode.code_hash!==walletCode.code_hash)throw new Error('Wallet code changed during fee quote');
    return {...fee,ram_pricing:ramPricing,relay_fee_bps:rate,multiplier,units,rows,head:info.head_block_num};
  }
  async nativeRows(token,recipient,quantity){const symbol=atomicAsset(quantity).symbol.split(',')[1],raw=BigInt('0x'+Buffer.from(symbol,'ascii').reverse().toString('hex')).toString();const result=await this.rpc('get_table_rows',{json:true,code:token,scope:recipient,table:'accounts',lower_bound:raw,upper_bound:raw,limit:1});return result.rows.length?0:1;}
  async prepare(operation,request,options={}){if(!this.config)await this.load();if(['ksend','withdraw','cancel'].includes(operation)&&!Object.hasOwn(request,'relay_fee_bps'))request={...request,relay_fee_bps:await this.relayRate(request.relayer)};return prepareEnvelope(this.config,await this.rpc('get_info'),operation,request,{contract:this.contract,...options});}
  async prepareRelayTransaction(body,bp,permission,relayPrivateKey){if(!this.config)await this.load();verifySigned(body,this.config,this.contract);const p=body.prepared;if(p.operation.startsWith('self')||p.request.relayer!==bp||p.permission!==permission)throw new Error('Wrong BP permission');return resourcePacket(body,this.config,relayPrivateKey,this.contract);}
}
