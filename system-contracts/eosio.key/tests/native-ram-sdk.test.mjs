import test from 'node:test';
import assert from 'node:assert/strict';
import {KeyWalletClient} from '../sdk/index.mjs';
import {NATIVE_RAM_SENDER_CODES} from '../sdk/fee-profiles.mjs';

function fixture(){
 const client=new KeyWalletClient('http://127.0.0.1:1');
 client.config={fee_contract:'eosio.token',fee_symbol:'4,AIC',system_hash:'ab'.repeat(32),policy:{minimum_fee:10000000,unit_fee:10000000}};
 const calls=[];let codeReads=0;
 client.rpc=async(method,data)=>{calls.push(method);if(method==='get_info')return{head_block_num:80};if(method==='get_code_hash'){if(data.account_name==='eosio')return{code_hash:client.config.system_hash};codeReads++;return{code_hash:client.changed&&codeReads>1?'7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416':NATIVE_RAM_SENDER_CODES[0]};}throw Error('Forbidden market, balance or relay request');};
 client.asset=async()=>({id:27,ram_purchased:4096});
 return{client,calls};
}
test('native sender fee quote stays zero without AIC, markets or relay and ignores fee floors',async()=>{
 const{client,calls}=fixture();const quote=await client.quote('deposit',{from:'keyusersolar',token_contract:'usd',quantity:'1.0000 USD'});
 assert.equal(quote.total,0n);assert.equal(quote.base,0n);assert.equal(quote.premium,0n);assert.equal(quote.storage,0n);assert.equal(quote.ram_payer,'keyusersolar');assert.equal(quote.native_sender_resources,true);
 assert.deepEqual(calls,['get_info','get_code_hash','get_code_hash','get_code_hash']);
});
test('native deposit quote refuses unprepared third-party custody and concurrent code changes',async()=>{
 const a=fixture();a.client.asset=async()=>({ram_purchased:0});await assert.rejects(a.client.quote('deposit',{from:'keyusersolar',token_contract:'usd',quantity:'1.0000 USD'}),/RAM registration/);
 const b=fixture();b.client.changed=true;await assert.rejects(b.client.quote('deposit',{from:'keyusersolar',token_contract:'usd',quantity:'1.0000 USD'}),/changed during/);
});
