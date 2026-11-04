"""Read-only check of original services/code/state. No controls or transactions."""
from pathlib import Path
import datetime,hashlib,json,subprocess,urllib.request
K=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2')
def rpc(method,data={}):
    request=urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+method,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
    return json.load(urllib.request.urlopen(request,timeout=40))
def table(name):
    data=rpc('get_table_rows',{'json':True,'code':'eosio','scope':'eosio','table':name,'limit':1000})
    assert not data['more'];return data['rows']
info=rpc('get_info');assert info['chain_id']=='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
hashes={a:rpc('get_code_hash',{'account_name':a})['code_hash']for a in ['eosio','eosio.key','eosio.token']}
assert hashes=={'eosio':'b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170','eosio.key':'1933629daab4d8fe3b64bfe0528b7d64b85dff97011b71301ba4122f590cf0f5','eosio.token':'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'}
old=json.loads(Path('/opt/pureeos-key-rate-20261104-r1/LIVE_AUDIT_after.json').read_text())
node=R/'pureeos-test-1.0.0-test.1/libexec/nodeos'
assert hashlib.sha256(node.read_bytes()).hexdigest()==old['stable']['nodehash']
nodes={};unit_hashes={}
for unit,original_pid in old['node_pids'].items():
    pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True));assert pid>0
    assert str(node)in (Path('/proc')/str(pid)/'maps').read_text()
    assert pid==original_pid,'An original node restarted: '+unit
    nodes[unit]=pid;unit_hashes[unit]=hashlib.sha256(subprocess.check_output(['systemctl','cat',unit])).hexdigest()
assert unit_hashes==old['stable']['node_unit_hashes']
assert table('votersv2')==old['stable']['voters']and table('bpbonds')==old['stable']['bonds']
banned=[r['owner']for r in table('rotcands')if r['banned']]
assert banned==old['stable']['permanent_bans']
services=[*old['business_units'],'pureeos-key-history-20261103.service','pureeos-keynet2-relay-20261103.service','pureeos-keynet2-purehistory-api-20261103.service','pureeos-keynet2-purehistory-indexer-20261103.service']
for u in services:assert subprocess.run(['systemctl','is-active','--quiet',u]).returncode==0,u
site=Path('/var/www/aic-test/current').resolve()
published=json.loads(Path('/opt/pureeos-explorer-key-rate-20261104-r4-collapse/DEPLOYMENT.json').read_text())
assert str(site)==published['release']
assert {str(p.relative_to(site)):hashlib.sha256(p.read_bytes()).hexdigest()for p in site.rglob('*')if p.is_file()}==published['files']
f=rpc('get_finalizer_info')['active_finalizer_policy'];activity=json.loads((R/'activity/STATE.json').read_text())
result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'head':info['head_block_num'],'LIB':info['last_irreversible_block_num'],
 'head_age_seconds':(datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)-datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds(),
 'code_hashes':hashes,'node_count':len(nodes),'original_node_PIDs_unchanged':True,'unit_config_hashes_unchanged':True,'votes_bonds_unchanged':True,
 'permanent_bans':banned,'registered':len(table('producersv2')),'eligible':sum(not r['banned']for r in table('rotcands')),
 'native_finalizer_count':len(f['finalizers']),'native_finalizer_threshold':f['threshold'],'public_frontend_release':str(site),'public_static_unchanged':True,
 'services_active':services,'ordinary_progress':{k:v for k,v in activity.items()if type(v)is int},'ordinary_has_pending':activity.get('pending')is not None,
 'scope':'Read only; no public transaction, upgrade, chain/service stop/restart, node addition, TPS or capacity qualification'}
(K/'LIVE_UNCHANGED_AUDIT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
