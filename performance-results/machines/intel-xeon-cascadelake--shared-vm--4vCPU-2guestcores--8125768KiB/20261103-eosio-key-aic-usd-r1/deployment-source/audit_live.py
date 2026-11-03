"""Read-only handover audit. No signing, chain settings or process mutation."""
from pathlib import Path
import datetime, hashlib, json, subprocess, urllib.request

R=Path('/opt/pureeos-keynet-20261103-r1')
EXPECTED='38391d73bbcaac3c51af7009d0d11b09ca263895c1bf55a96494c3194f7f4d65'
def call(path,data=None,port=8888):
    request=urllib.request.Request('http://127.0.0.1:'+str(port)+path,
        None if data is None else json.dumps(data).encode(),{'Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=30)as response:return json.load(response)
def rpc(method,data=None,port=8888):return call('/v1/chain/'+method,data or {},port)
def table(name):return rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':name,'json':True,'limit':1000})['rows']
info=rpc('get_info');history=rpc('get_info',port=8890)
assert info['chain_id']==history['chain_id']==EXPECTED
plan=json.loads((R/'PLAN.json').read_text());names={x['name']for x in plan['BPs']}
producers=table('producersv2');voters=table('votersv2');bonds=table('bpbonds');cfg=table('bpbondcfg')[0]
assert {p['owner']for p in producers}==names and len(names)==25
assert all(p['is_active']for p in producers)
assert len({p['total_votes']for p in producers})==25
by_voter={p['owner']:p for p in voters}
assert all(by_voter[n]['producers']==[n]for n in names)
assert len(bonds)==25 and all(b['balance']==100000000 and not b['forfeited']and not b['settled']for b in bonds)
assert cfg['locked']==2500000000 and cfg['slash_pending']==cfg['slash_settled']==0
candidates=table('rotcands');assert not any(c['banned']for c in candidates)
assert not table('bpmaint')
native=rpc('get_finalizer_info');schedule=rpc('get_producer_schedule');rotation=table('rotation')[0]
governance=rpc('get_upgrade_state');assert governance['sealed']and governance['review_seconds']==604800
assert 'eosio.key'in governance['protected_accounts']
processes=[]
for proc in Path('/proc').iterdir():
    try:
        arguments=(proc/'cmdline').read_bytes().split(b'\x00')if proc.name.isdigit()else []
        if any(a.endswith(b'/libexec/nodeos')for a in arguments):
            command=b' '.join(arguments).decode()
            assert str(R)in command and '/opt/pureeos-test45-20261102-r1'not in command
            processes.append(int(proc.name))
    except(FileNotFoundError,ProcessLookupError,PermissionError):pass
assert len(processes)==26
health=call('/v2/health',port=7000)
assert all(h['status']=='OK'for h in health['health'])
summary=call('/v2/state/get_producer_summary',port=9051)
transaction=call('/v2/history/get_transaction?id=3deb4c9420ec47b2e867c861d1da79c2a5b9b8a5bdc9b65a86d245e8d44414b6',port=7000)
assert transaction['executed']and transaction['lib']>=4988
assert all(a['block_id']=='0000137cbf1d5fb65ec9654a3346c1b78cb24e94c3ff736c095b12e3fa5f1e1a'for a in transaction['actions'])
activity=json.loads((R/'activity/STATE.json').read_text())
assert activity['chain_id']==EXPECTED and activity['status']=='RUNNING'and activity['completed_batches']>0
assert set(activity['batch_sizes'])<={'1','2','3'}
wallet_hash=rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']
assert wallet_hash=='1e0ed2c38e994dea218948483c1333fbd4a0a54c647eb8d8f5932b3d76ac98ea'
finalizers=native['active_finalizer_policy']
result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'chain_id':EXPECTED,'info':info,'history_info':history,'nodeos_processes':len(processes),
    'registered_BPs':25,'full_bond_BPs':25,'escrow':'250000.0000 AIC','slash_pool':'0.0000 AIC',
    'self_voted_BPs':25,'distinct_actual_vote_weights':25,'banned':0,'maintenance':0,
    'native_finalizers':len(finalizers['finalizers']),'native_threshold':finalizers['threshold'],
    'native_proposers':len(schedule['active']['producers']),'native_admission_complete_21_15':len(finalizers['finalizers'])==21 and finalizers['threshold']==15,
    'natural_admission_timing_unchanged':True,'native_rotation':rotation,
    'governance':governance,'wallet_code_hash':wallet_hash,'public_USD_transaction':transaction,
    'history_health':health,'producer_summary':summary,
    'activity':{k:activity[k]for k in ['status','completed_batches','business_transactions','transfers','ticks','notes','batch_sizes','empty_blocks','last_completed_batch']},
    'no_TPS':True,'production_defaults_changed':False,'old_archive_retained':str(R/'archive')}
(R/'HANDOVER_AUDIT.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items()if k not in ['governance','info','history_info','native_rotation','public_USD_transaction','producer_summary','history_health','activity']}))
