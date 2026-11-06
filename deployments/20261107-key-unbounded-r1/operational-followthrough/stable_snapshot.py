"""Read-only stable deployment invariants; native rows remain server-side public JSON."""
from pathlib import Path
import sys,json,datetime
import ops_common as o
R=o.R
names=['eosio','eosio.key','eosio.signup','eosio.token','eosio.dev','eosio.prods','eosio.wrap','eosio.msig','eosio.null','usd','cny','gptgenesisbp','civicgovauth']
def snapshot():
 info=o.guard();permissions={}
 for n in names:
  a=o.rpc('get_account',{'account_name':n});permissions[n]={'privileged':a['privileged'],'permissions':a['permissions']}
 stats={token:o.rows(token,sym,'stat')for token,sym in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]}
 return{'observed_UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'head':info['head_block_num'],'LIB':info['last_irreversible_block_num'],'chain_id':info['chain_id'],'permissions':permissions,'token_stats':stats,'consensus':o.rpc('get_consensus_parameters'),'native_schedule':o.rpc('get_producer_schedule'),'processes':o.process_guard(),'KEY_config':o.rows('eosio.key','eosio.key','config'),'codes':{n:o.rpc('get_code_hash',{'account_name':n})['code_hash']for n in ['eosio','eosio.key','eosio.signup','eosio.token','usd','cny']}}
if __name__=='__main__':
 assert sys.argv[1]=='before';p=R/'STABLE_BEFORE.json';assert not p.exists();o.require_codes(o.OLD);o.save(p,snapshot());print('READ_ONLY_STABLE_BASELINE_SAVED')
