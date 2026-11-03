// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
import {createHash} from 'node:crypto';
import {Asset, Checksum256, PrivateKey, PublicKey, Serializer, Signature,
        Transaction, SignedTransaction, PackedTransaction} from '@wharfkit/antelope';

export const CONTRACT = 'eosio.key';
export const TAG = 'PureEOS:eosio.key';
const LIMIT = (1n << 62n) - 1n;
const REQUESTS = {
  ksend: 'transfer_request', withdraw: 'withdrawal_request', selfout: 'withdrawal_request',
  cancel: 'cancellation_request', selfcancel: 'cancellation_request',
};
const FIELDS = {
  transfer_request: ['from','to','token_contract','quantity','memo','nonce','expires','relayer','reward_to','max_fee'],
  withdrawal_request: ['from','token_contract','to','quantity','memo','nonce','expires','relayer','reward_to','max_fee'],
  cancellation_request: ['from','nonce','next_nonce','expires','relayer','reward_to','max_fee'],
};
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
// Preserve uint64/int64 RPC integers before JavaScript rounding. Node >=22.12
// exposes the original JSON primitive in the reviver's source context.
export function parseRPCJSON(text) {
  return JSON.parse(text,(key,value,context)=>{
    if(typeof value==='number'&&Number.isInteger(value)&&!Number.isSafeInteger(value)){
      if(!context?.source||! /^-?\d+$/.test(context.source))throw new Error('RPC contains an unsafe numeric value');
      return context.source;
    }
    return value;
  });
}
export function normalizePublicKey(key) {
  const value = PublicKey.from(key);
  if (value.type !== 'K1') throw new Error('K1 public key required');
  return value.toString();
}
export function keyHash(key) {
  return sha(Serializer.encode({type: 'public_key', object: PublicKey.from(normalizePublicKey(key))}).array);
}
export function assetIdentity(tokenContract, tokenSymbol) {
  return sha(Serializer.encode({abi: {version:'eosio::abi/1.2',types:[],structs:[{
    name:'asset_identity',base:'',fields:[{name:'token_contract',type:'name'},{name:'token_symbol',type:'symbol'}],
  }],actions:[],tables:[]}, type:'asset_identity',object:{token_contract:tokenContract,token_symbol:tokenSymbol}}).array);
}
export function atomicAsset(quantity) {
  const asset = Asset.from(quantity);
  const units = BigInt(asset.units.toString());
  if (units < -LIMIT || units > LIMIT) throw new Error('Asset amount out of range');
  return {units, symbol:asset.symbol.toString(), text:asset.toString()};
}
export function formatFee(units) {
  const value = BigInt(units);
  if (value < 0n || value > LIMIT) throw new Error('Invalid fee amount');
  return `${value / 10000n}.${String(value % 10000n).padStart(4,'0')} AIC`;
}
export function depositMemo(key, maximumFee) {
  const cap = atomicAsset(maximumFee);
  if (cap.symbol !== '4,AIC' || cap.units < 0n) throw new Error('AIC fee cap required');
  return `PK1|${normalizePublicKey(key)}|${cap.units}`;
}
export function validateRequest(operation, request) {
  const type = REQUESTS[operation];
  if (!type || !request || typeof request !== 'object' || Array.isArray(request)) throw new Error('Unsupported wallet operation');
  const fields = FIELDS[type];
  if (Object.keys(request).length !== fields.length || fields.some(k => !Object.hasOwn(request,k)) ||
      Object.keys(request).some(k => !fields.includes(k))) throw new Error('Request fields must match the signed schema exactly');
  normalizePublicKey(request.from);
  if (operation === 'ksend') normalizePublicKey(request.to);
  if(typeof request.nonce==='number'&&!Number.isSafeInteger(request.nonce))throw new Error('Nonce must be an exact integer');
  const nonce = BigInt(request.nonce);
  if (nonce < 0n || nonce >= 1n << 64n) throw new Error('Invalid nonce');
  if (!Number.isInteger(request.expires) || request.expires < 0 || request.expires > 0xffffffff) throw new Error('Invalid expiration');
  const maximum = atomicAsset(request.max_fee);
  if (maximum.symbol !== '4,AIC' || maximum.units < 0n) throw new Error('Invalid AIC fee cap');
  if (type !== 'cancellation_request') {
    if (atomicAsset(request.quantity).units <= 0n) throw new Error('Principal must be positive');
    if (typeof request.memo !== 'string' || Buffer.byteLength(request.memo,'utf8') > 128) throw new Error('Memo too long');
  } else if ((typeof request.next_nonce==='number'&&!Number.isSafeInteger(request.next_nonce))||BigInt(request.next_nonce) <= nonce || BigInt(request.next_nonce) >= 1n << 64n) throw new Error('Invalid next nonce');
  if (operation.startsWith('self')) {
    if (request.relayer || request.reward_to) throw new Error('Self operation cannot pay a relay reward');
  } else if (!request.relayer || request.reward_to !== request.relayer) throw new Error('Selected BP and reward identity must match');
  return type;
}
export function intentDigest(abi, config, operation, request, contract = CONTRACT) {
  const type = validateRequest(operation,request);
  const envelope = {name:'key_signing_envelope',base:'',fields:[
    {name:'tag',type:'string'},{name:'version',type:'uint32'},{name:'chain_id',type:'checksum256'},
    {name:'contract',type:'name'},{name:'deployment',type:'checksum256'},
    {name:'operation',type:'name'},{name:'request',type},
  ]};
  const combined = {...abi,structs:[...abi.structs.filter(s=>s.name!==envelope.name),envelope]};
  const bytes = Serializer.encode({abi:combined,type:envelope.name,object:{
    tag:TAG,version:config.version,chain_id:config.chain_id,contract,
    deployment:config.deployment,operation,request,
  }});
  return Checksum256.from(sha(bytes.array));
}
// This helper is for the user's local wallet/test environment. The relay HTTP
// protocol accepts only completed signatures and never accepts a user key.
export function signIntent(abi, config, operation, request, userPrivateKey, contract = CONTRACT) {
  const key = PrivateKey.from(userPrivateKey);
  if (!key.toPublic().equals(PublicKey.from(request.from))) throw new Error('Signing key does not own sender wallet');
  return key.signDigest(intentDigest(abi,config,operation,request,contract)).toString();
}
export function verifyIntent(abi,config,operation,request,signature,contract=CONTRACT) {
  try {
    return Signature.from(signature).verifyDigest(intentDigest(abi,config,operation,request,contract),PublicKey.from(request.from));
  } catch { return false; }
}
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
export function calculateFee(policy,multiplier,units,rows,relayed) {
  if (!Number.isInteger(units)||units<0||units>128||!Number.isInteger(rows)||rows<0||rows>4) throw new Error('Invalid work bound');
  const numerator=(BigInt(policy.unit_fee)*BigInt(units)+BigInt(policy.new_row_fee)*BigInt(rows))*BigInt(multiplier);
  const computed=(numerator+9999n)/10000n;
  const base=computed>BigInt(policy.minimum_fee)?computed:BigInt(policy.minimum_fee);
  const premium=relayed?(base*30n+99n)/100n:0n;
  if(base<=0n||base+premium>LIMIT)throw new Error('Fee out of range');
  return {base,premium,total:base+premium,base_asset:formatFee(base),premium_asset:formatFee(premium),total_asset:formatFee(base+premium)};
}

export class KeyWalletClient {
  constructor(endpoint,{contract=CONTRACT,fetchImpl=globalThis.fetch}={}) {
    this.endpoint=endpoint.replace(/\/+$/,'');this.contract=contract;this.fetch=fetchImpl;
  }
  async rpc(method,data={}) {
    const result=await this.fetch(this.endpoint+'/v1/chain/'+method,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(data),signal:AbortSignal.timeout(20000)});
    if(!result.ok)throw new Error(`RPC ${method}: HTTP ${result.status}`);
    return parseRPCJSON(await result.text());
  }
  async rows(table,scope=this.contract,extra={}) {
    const value=await this.rpc('get_table_rows',{json:true,code:this.contract,scope,table,limit:1000,...extra});
    if(value.more)throw new Error('Truncated wallet query; explicit pagination required');
    return value.rows;
  }
  async load() {
    const [info,configs,abi]=await Promise.all([this.rpc('get_info'),this.rows('config'),this.rpc('get_abi',{account_name:this.contract})]);
    if(configs.length!==1||configs[0].chain_id!==info.chain_id||Number(configs[0].version)!==1)throw new Error('Wallet domain does not match actual node chain');
    this.config=configs[0];this.abi=abi.abi;return this;
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
    const code=await this.rpc('get_code_hash',{account_name:'eosio'});
    const exit=['selfout','selfcancel','refundfee','claim'].includes(operation);
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
      if(!wallet)rows=2;
      else if(!asset||(await this.rows('balances',String(wallet.id),{lower_bound:String(asset.id),upper_bound:String(asset.id),limit:2})).length===0)rows=1;
      if(operation==='deposit'&&!asset)rows++;
      units=(operation==='deposit'?2:4)+Math.ceil(Buffer.byteLength(request.memo||'','utf8')/64);
    }else if(operation==='withdraw'||operation==='selfout')units=6+Math.ceil(Buffer.byteLength(request.memo||'','utf8')/64);
    else if(operation==='cancel'||operation==='selfcancel')units=3;
    else if(operation==='claim'||operation==='refundfee')units=4;
    else throw new Error('Unsupported fee operation');
    return {...calculateFee(cfg.policy,multiplier,units,rows,['ksend','withdraw','cancel'].includes(operation)),multiplier,units,rows,head:info.head_block_num};
  }
  async prepareRelayTransaction(operation,request,signature,bp,permission,relayPrivateKey) {
    if(!['ksend','withdraw','cancel'].includes(operation))throw new Error('Commercial relay operation not supported');
    if(!this.config)await this.load();
    validateRequest(operation,request);
    if(request.relayer!==bp||request.reward_to!==bp||!verifyIntent(this.abi,this.config,operation,request,signature,this.contract))throw new Error('Invalid sender authorization');
    const info=await this.rpc('get_info');
    if(info.chain_id!==this.config.chain_id)throw new Error('Node chain changed');
    const block=Buffer.from(info.last_irreversible_block_id,'hex');
    const data=Serializer.encode({abi:this.abi,type:this.abi.actions.find(a=>a.name===operation).type,object:{request,sig:signature}}).hexString;
    const expiration=Math.min(Date.parse(info.head_block_time+'Z')+180000,request.expires*1000);
    if(!Number.isFinite(expiration))throw new Error('Invalid consensus timestamp');
    const transaction=Transaction.from({expiration:new Date(expiration).toISOString().slice(0,19),
      ref_block_num:info.last_irreversible_block_num&0xffff,ref_block_prefix:block.readUInt32LE(8),
      max_net_usage_words:0,max_cpu_usage_ms:0,delay_sec:0,context_free_actions:[],transaction_extensions:[],
      actions:[{account:this.contract,name:operation,authorization:[{actor:bp,permission}],data}]});
    const hotKey=PrivateKey.from(relayPrivateKey);
    const signed=SignedTransaction.from({...transaction.toJSON(),signatures:[hotKey.signDigest(transaction.signingDigest(info.chain_id)).toString()],context_free_data:[]});
    return {id:transaction.id.toString(),expiration:transaction.expiration.toString(),startLIB:info.last_irreversible_block_num,
      packed:PackedTransaction.fromSigned(signed).toJSON()};
  }
}
