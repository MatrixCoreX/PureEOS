"""Reconcile already-irreversible issuance. Never sign, start wallet or submit."""
from pathlib import Path
import json,hashlib,urllib.request,subprocess,datetime
R=Path('/opt/pureeos-usd-cny-test-issuance-20261105-r1')
source=(R/'issue.py').read_text();prefix=source[:source.index("\nprogress={'status':'PREFLIGHT'")]
exec(compile(prefix,'read-only-issuer-helpers','exec'),globals())
assert not(R/'RESULT.json').exists(),'Completed issuance must never be repeated'
raw=(R/'PROGRESS.json').read_bytes();progress=json.loads(raw)
assert progress['status']=='NEEDS_CANONICAL_RECONCILIATION'and progress.get('canonical_receipt')
assert "KeyError: 'receiver'"in progress['error']
assert not(R/'INITIAL_PROGRESS_FAILURE.json').exists();(R/'INITIAL_PROGRESS_FAILURE.json').write_bytes(raw)
before=json.loads((R/'BEFORE.json').read_text());intent=json.loads((R/'INTENT.json').read_text());actions=intent['actions'];canonical=progress['canonical_receipt'];tid=progress['transaction_id']
assert hashlib.sha256((P/'signed.json').read_bytes()).hexdigest()==progress['packet_SHA256']
packed=run(['convert','pack_transaction',str(P/'signed.json')]);assert hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()==tid
info=rpc('get_info');assert info['chain_id']==CHAIN and info['last_irreversible_block_num']>=canonical['block_num']
b=rpc('get_block',{'block_num_or_id':canonical['block_num']});assert b['id']==canonical['block_id']
native=next(x for x in b['transactions']if(x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==tid);assert native['status']=='executed'
with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+tid,timeout=20)as h:history=json.load(h)
assert history['executed']and history['trx_id']==tid
entries=sorted(history['actions'],key=lambda x:x['action_ordinal']);assert len(entries)==6 and [x['action_ordinal']for x in entries]==list(range(1,7))
for entry,expected in zip(entries,actions):
 assert entry['creator_action_ordinal']==0 and entry['block_id']==b['id']and entry['block_num']==b['block_num']and entry['trx_id']==tid
 act=entry['act'];assert act['account']==expected['account']and act['name']==expected['name']and act['authorization']==expected['authorization']
 assert act['account']in entry['notified'].split(',')
 for k,v in expected['data'].items():assert act['data'][k]==v,(expected['account'],expected['name'],k)
 extra=set(act['data'])-set(expected['data'])
 if act['name']=='transfer':
  assert extra=={'amount','symbol'}and act['data']['amount']==100000000 and act['data']['symbol']==act['account'].upper()and RECIPIENT in entry['notified'].split(',')
 else:assert not extra
after=token_snapshot()
for n,v in after.items():
 sym=n.upper();old=before['tokens'][n]
 assert v['stats']=={'supply':EXPECTED+' '+sym,'max_supply':CAP+' '+sym,'issuer':n}
 assert v['recipient_balance_atoms']-old['recipient_balance_atoms']==1000000000000
 for k in ['raw_ABI','code_hash','permissions','issuer_balance_atoms','KEY_balance_atoms']:assert v[k]==old[k],(n,k)
baseline_pids=before['node_pids'];assert pids()==baseline_pids
assert key_snapshot()==before['KEY_snapshot']and governance()==before['governance']and permissions('eosio.dev')==before['developer_permissions']
assert rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})==before['AIC_stats']
signer=json.loads((R/'TEMP_SIGNER_CLOSED.json').read_text());assert signer['status']=='PASS'and signer['active']is False and subprocess.run(['systemctl','is-active','--quiet',unit]).returncode!=0
canonical['rechecked_LIB']=info['last_irreversible_block_num']
result={'status':'PASS','UTC':now(),'chain_id':CHAIN,'transaction_id':tid,'canonical_receipt':canonical,'actions':actions,'before':before['tokens'],'after':after,'additional_issued_each':'100000000.0000','cap_each':'1000000000.0000','issued_supply_each':'100010000.0000','recipient':RECIPIENT,'native_CPU_NET_payer':'eosio.dev','original25_node_PIDs_unchanged':True,'AIC_stats_unchanged':before['AIC_stats'],'KEY_ledger_reserves_RAM_unchanged':True,'code_ABI_authorities_unchanged':True,'developer_authority_used':True,'single_atomic_transaction':True,'no_governance_proposal':True,'native_history_actions_match':True,'signing_packet_exported':False,'temporary_signer_stopped':True,'initial_verification_failure':progress['error'],'reconciliation':'Read-only original immutable packet ID, irreversible native receipt, current stats/balances and six root actions. API uses notified rather than receiver and adds amount/symbol aliases to transfer data; aliases were separately validated. No re-sign, resubmit or repeat issuance.','scope':'Authorized issuer operations only. No AIC issuance/cap changes, node stop/reset, votes/bonds, contract upgrade, new server or TPS. No earlier successful payment replay.'}
save('RESULT.json',result);progress['initial_history_verification_error']=progress.pop('error');progress.update(status='PASS',reconciliation=result['reconciliation']);save('PROGRESS.json',progress)
print('READONLY_CANONICAL_USD_CNY_1B_CAP_PLUS_100M_EACH_PASS '+tid)
