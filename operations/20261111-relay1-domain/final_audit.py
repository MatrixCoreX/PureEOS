"""Read-only final audit plus invalid unsigned HTTP transport probes. No payments or resubmission."""
from pathlib import Path
import sys,json,datetime,subprocess,urllib.request,urllib.error,time
R=Path('/opt/pureeos-relay1-domain-20261111-r1');sys.path.insert(0,str(R));import ops_common as o
assert not(R/'FINAL_AUDIT.json').exists()
base=json.loads((R/'BASELINE.json').read_text());o.guard();o.require_codes(o.NEW);o.authority_guard();processes=o.process_guard()
for unit,b in base['services'].items():
 if unit!='pureeos-devnet-ordinary-activity-20261105.service':assert processes[unit]==b,unit
for name,pin in base['code_ABI_pins'].items():
 current=o.rpc('get_raw_abi',{'account_name':name});assert all(current[k]==v for k,v in pin.items()),name
assert o.rpc('get_consensus_parameters')==base['native_consensus']
assert o.rows('eosio','eosio','bpbonds')==base['records']['bpbonds']
assert o.rows('eosio','eosio','bpmaint')==base['records']['bpmaint']
rot=o.rows('eosio','eosio','rotcands');assert sorted(x['owner']for x in rot if x['banned'])==sorted(x['owner']for x in base['records']['rotcands']if x['banned'])
account=o.rpc('get_account',{'account_name':'gptgenesisbp'});assert account['permissions']==base['BP_account']['permissions']
relays=o.rows('eosio.key','eosio.key','relays');assert relays==[dict(x,endpoint='https://relay1.matrixai.one')if x['bp']=='gptgenesisbp'else x for x in base['relays']]
assert o.rows('eosio.key','eosio.key','relayrates')==base['rates']
cut=json.loads((R/'CUTOVER_RESULT.json').read_text());receipt=cut['canonical_receipt'];block=o.rpc('get_block',{'block_num_or_id':receipt['block_num']});assert block['id']==receipt['block_id']
assert sum((x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==receipt['transaction_id']and x['status']=='executed'for x in block['transactions'])==1
headers={'User-Agent':'PureEOS-relay1-domain-validation/1.0','Content-Type':'application/json'}
def http(url,method='GET',body=None):
 req=urllib.request.Request(url,method=method,data=body,headers=headers)
 try:
  with urllib.request.urlopen(req,timeout=30)as r:return r.status,json.load(r),dict(r.headers)
 except urllib.error.HTTPError as e:return e.code,json.loads(e.read()),dict(e.headers)
roots=['https://relay1.matrixai.one','https://aic-test.matrixai.one/relay','https://relay2.matrixai.one'];health=[]
for root,bp,bps in zip(roots,['gptgenesisbp','gptgenesisbp','anthropiclab'],[3000,3000,1000]):
 status,h,hh=http(root+'/health');assert status==200 and h['bp']==bp and h['permission']=='keyrelay'and h['fee_bps']==bps;health.append({'endpoint':root,'health':h})
assert health[0]['health']==health[1]['health']
journal=Path('/opt/pureeos-devnet-20261105-r1/relay-state/journal.json');original=json.loads((R/'JOURNAL_BEFORE.json').read_text());jobs=json.loads(journal.read_text());assert o.sha(journal.read_bytes())==original['SHA256']
id=next(k for k,v in jobs.items()if v['status']=='finalized');status=[];rejections=[]
for root in roots[:2]:
 code,result,h=http(root+'/status/'+id);assert code==200 and result['status']=='finalized';status.append(result)
 code,result,h=http(root+'/submit','POST',b'{}');assert code==400 and 'error'in result;rejections.append({'endpoint':root,'HTTP':code,'response':result})
assert status[0]==status[1]and rejections[0]['response']==rejections[1]['response']
assert o.sha(journal.read_bytes())==original['SHA256'],'Invalid unsigned probe must not create jobs'
assert o.sha(Path('/etc/nginx/sites-available/aic-test').read_bytes())==json.loads((R/'TLS_STARTED.json').read_text())['existing_vhost_SHA256']
assert subprocess.run(['nginx','-t'],capture_output=True).returncode==0
native=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos');assert o.sha(native.read_bytes())=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
physical=[]
for p in Path('/proc').iterdir():
 if p.name.isdigit():
  try:
   if str(native)in(p/'maps').read_text():physical.append(int(p.name))
  except OSError:pass
assert len(physical)==26 and set(physical)=={processes[u]['PID']for u in json.loads((R/'PROCESS_BASELINE.json').read_text())['native_units']}
unit='pureeos-relay2-anthropiclab-20261111.service';pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True));assert pid>0
p=Path('/proc')/str(pid);boot=int(next(x.split()[1]for x in Path('/proc/stat').read_text().splitlines()if x.startswith('btime ')));start=boot+int((p/'stat').read_text().split()[21])/100
assert start<o.stamp(base['utc']).timestamp(),'Relay2 restarted after baseline'
assert not any(b'/opt/rly1-admin-20261111'in(p/'cmdline').read_bytes()for p in Path('/proc').iterdir()if p.name.isdigit()and(p/'cmdline').exists())
for unit in ['pureeos-relay1-cutover-20261111.service','pureeos-relay1-tls-20261111.service']:assert subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True).strip()=='0'
f=o.rpc('get_finalizer_info');assert len(f['active_finalizer_policy']['finalizers'])==21 and f['active_finalizer_policy']['threshold']==15
# Exact unchanged frontend assets; endpoint data is read dynamically from registry.
manifest=json.loads(Path('/opt/pureeos-relay-eligibility-20261111-r1/web-qualification/RESULT.json').read_text())['dist'];assets=[]
for path,want in manifest.items():
 req=urllib.request.Request('https://aic-test.matrixai.one/'+path,headers=headers)
 with urllib.request.urlopen(req,timeout=30)as r:got=o.sha(r.read())
 assert got==want;assets.append({'path':path,'SHA256':got})
phones=json.loads((R/'PHONE_UPDATE_BOTH_R31.json').read_text());assert phones['status']=='PASS'and phones['version']=='0.1.0-test.31'and len(phones['devices'])==2
assert all(x['installed_hash_matches']and x['UID_preserved']for x in phones['devices'])
o.save(R/'FINAL_AUDIT.json',{'status':'PASS_RELAY1_ENDPOINT_MIGRATION','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'info':o.guard(),'canonical_receipt':receipt,'health':health,'registry':relays,'rates':base['rates'],'native26_946_and_original_monitored_processes_preserved':True,'relay2_process_started_before_task':True,'protected6_code_ABI_consensus_bans_bonds_maintenance_unchanged':True,'founder_permissions_unchanged':True,'original_journal_SHA256':original['SHA256'],'journal_counts':{s:sum(x['status']==s for x in jobs.values())for s in ['finalized','expired']},'same_existing_status_through_both_domains':True,'invalid_unsigned_POST_rejections':rejections,'no_journal_mutation_from_probes':True,'old_nginx_vhost_byteexact':True,'unchanged_frontend_assets':assets,'phones':phones,'scope':'Only one endpoint registration action. Health/status plus deliberately invalid unsigned HTTPPOST validate proxy transport; NO new financial transfer/claim/re-sign/resend or phonefinancial/Keystore/privatefile claim. No native/contract upgrade or BP/node/history restart. Mi11foregroundset changed between metadata snapshots without agent launching/switching apps; no preservation claim for that set.'})
print('FINAL_RELAY1_MIGRATION_AUDIT_PASS')
