"""New read-only baseline; no keys exported or financial/service changes."""
from pathlib import Path
import json,subprocess,sys,datetime,hashlib
sys.path.insert(0,'/opt/pureeos-key-native-ram-20261111-r1/controllers');import ops_common as o
R=Path('/opt/pureeos-relay1-domain-20261111-r1');assert not R.exists();R.mkdir(mode=0o700)
info=o.guard();o.require_codes(o.NEW);o.authority_guard()
records={t:o.rows('eosio','eosio',t)for t in ['producersv2','rotcands','bpbonds','bpmaint']};relays=o.rows('eosio.key','eosio.key','relays');rates=o.rows('eosio.key','eosio.key','relayrates');bp='gptgenesisbp'
p=next(r for r in records['producersv2']if r['owner']==bp);c=next(r for r in records['rotcands']if r['owner']==bp);b=next(r for r in records['bpbonds']if r['owner']==bp);m=next((r for r in records['bpmaint']if r['owner']==bp),None)
assert p['is_active']and not c['banned']and int(b['state'])==0 and int(b['balance'])==100000000 and(m is None or int(m['state'])==0)
account=o.rpc('get_account',{'account_name':bp});active=next(x['required_auth']for x in account['permissions']if x['perm_name']=='active');keys=json.loads(Path('/opt/pureeos-devnet-20261105-r1/private/bootstrap-keys.json').read_text())['accounts'][bp];assert active['threshold']==1 and len(active['keys'])==1 and not active['accounts']and active['keys'][0]['key']==keys['Public key'];keys=None
baseline={'status':'PASS_READONLY','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'info':info,'services':o.process_guard(),'records':records,'relays':relays,'rates':rates,'selected_BP':bp,'BP_account':account,'native_consensus':o.rpc('get_consensus_parameters'),'code_ABI_pins':{n:{k:v for k,v in o.rpc('get_raw_abi',{'account_name':n}).items()if k in ['code_hash','abi_hash']}for n in ['eosio','eosio.token','eosio.key','eosio.signup','usd','cny']}}
(R/'BASELINE.json').write_text(json.dumps(baseline,indent=2)+'\n')
nginx={}
for p in Path('/etc/nginx').rglob('*'):
 if p.is_file()and p.suffix not in['.crt','.key','.pem']:
  try:
   s=p.read_text()
   if 'server_name'in s:nginx[str(p)]=[l.strip()for l in s.splitlines()if any(k in l for k in ['server_name','ssl_certificate ','ssl_certificate_key ','proxy_pass'])]
  except(UnicodeError,OSError):pass
journal_path=Path('/opt/pureeos-devnet-20261105-r1/relay-state/journal.json');jobs=json.loads(journal_path.read_text());(R/'JOURNAL_BEFORE.json').write_text(json.dumps({'jobs':{k:{x:j.get(x)for x in ['id','status','expiration','confirmation']}for k,j in jobs.items()},'SHA256':hashlib.sha256(journal_path.read_bytes()).hexdigest()},indent=2)+'\n')
print(json.dumps({'journal_counts':{s:sum(j['status']==s for j in jobs.values())for s in set(j['status']for j in jobs.values())},'status':baseline['status'],'BP':bp,'active_key_matched_without_export':True,'registered_relays':relays,'rates':rates,'nginx_public_routes':nginx,'memory_kib':{l.split(':')[0]:int(l.split()[1])for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapFree:'))}}))
