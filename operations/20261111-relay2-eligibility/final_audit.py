"""Final read-only audit. No new signatures, payments, phone or service operations."""
from pathlib import Path
import json,sys,datetime,time,subprocess,urllib.request,urllib.parse,urllib.error
from decimal import Decimal
R=Path('/opt/pureeos-relay-eligibility-20261111-r1');sys.path.insert(0,str(R));import ops_common as o
assert not(R/'FINAL_AUDIT.json').exists();original_rpc=o.rpc
def read_rpc(method,data=None,port=8888):
 for n in range(5):
  try:return original_rpc(method,data,port)
  except urllib.error.HTTPError as e:
   raw=e.read().decode(errors='replace')
   with(R/'final-read-retries.log').open('a')as f:f.write(json.dumps({'method':method,'HTTP':e.code,'body':raw,'attempt':n+1})+'\n')
   if e.code not in[500,502,503,504]or n==4:raise
   time.sleep(.2*2**n)
o.rpc=read_rpc
base=json.loads((R/'BASELINE.json').read_text());processes=o.process_guard();assert processes==base['services'];o.require_codes(o.NEW);o.authority_guard();info=o.guard()
for name,pin in base['code_ABI_pins'].items():
 a=o.rpc('get_raw_abi',{'account_name':name});assert all(a[k]==v for k,v in pin.items()),name
assert o.rpc('get_consensus_parameters')==base['native_consensus']
rot=o.rows('eosio','eosio','rotcands');assert sorted(x['owner']for x in rot if x['banned'])==sorted(x['owner']for x in base['records']['rotcands']if x['banned'])
assert o.rows('eosio','eosio','bpbonds')==base['records']['bpbonds'];assert o.rows('eosio','eosio','bpmaint')==base['records']['bpmaint']
account=o.rpc('get_account',{'account_name':'anthropiclab'});old=base['BP_account'];parent=lambda a:[(p['perm_name'],p['parent'],p['required_auth'])for p in a['permissions']if p['perm_name']in['owner','active']];assert parent(account)==parent(old)
child=next(p for p in account['permissions']if p['perm_name']=='keyrelay');assert{(x['account'],x['action'])for x in child['linked_actions']}=={('eosio.key',x)for x in ['ksend','withdraw','cancel']}
reg=json.loads((R/'REGISTER_RESULT.json').read_text());assert child==reg['child_permission']
relays=o.rows('eosio.key','eosio.key','relays');rates=o.rows('eosio.key','eosio.key','relayrates')
assert[x for x in relays if x['bp']!='anthropiclab']==[x for x in base['relays']if x['bp']!='anthropiclab'];assert[x for x in rates if x['bp']!='anthropiclab']==[x for x in base['rates']if x['bp']!='anthropiclab'];assert next(x for x in relays if x['bp']=='anthropiclab')['endpoint']=='https://relay2.matrixai.one';assert next(x for x in rates if x['bp']=='anthropiclab')['fee_bps']==1000
qa=json.loads((R/'PUBLIC_QA_RESULT.json').read_text());assert qa['status']=='PASS'and len(qa['cases'])==2
receipts=[reg['canonical_receipt'],qa['funding']]+[x['canonical_receipt']for x in qa['cases']]
for r in receipts:
 b=o.rpc('get_block',{'block_num_or_id':r['block_num']});assert b['id']==r['block_id'];assert sum((x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==r['transaction_id']and x['status']=='executed'for x in b['transactions'])==1
assert len({x['transaction_id']for x in receipts})==4
journal=json.loads((R/'relay2-state/journal.json').read_text());assert len(journal)==2 and all(x['status']=='finalized'for x in journal.values())
def vault():
 assets=o.rows('eosio.key','eosio.key','assets');wallets=o.rows('eosio.key','eosio.key','wallets');balances=[b for w in wallets for b in o.rows('eosio.key',str(w['id']),'balances')];fees=o.rows('eosio.key','eosio.key','feebuckets');native=o.rows('eosio.key','eosio.key','nativefees');custody={}
 for a in assets:
  precision,symbol=a['token_symbol'].split(',');v=o.rpc('get_currency_balance',{'code':a['token_contract'],'account':'eosio.key','symbol':symbol});custody[str(a['id'])]=int(Decimal(v[0].split()[0])*10**int(precision))if v else 0
 return{'assets':assets,'wallets':wallets,'balances':balances,'fees':fees,'nativefees':native,'custody':custody}
for n in range(5):
 v=vault()
 if v==vault():break
else:raise RuntimeError('Changing vault; no mixed-state solvency claim')
reserves=[]
for a in v['assets']:
 liability=sum(int(x['amount'])for x in v['balances']if int(x['asset_id'])==int(a['id']))
 if int(a['id'])==1:liability+=sum(int(x['amount'])for x in v['nativefees'])+sum(int(x['network'])+int(x['relay_earned'])for x in v['fees'])
 assert liability==int(a['reserve'])==v['custody'][str(a['id'])];reserves.append({'token':a['token_contract'],'symbol':a['token_symbol'],'atoms':liability})
assert not o.rows('eosio.key','eosio.key','pending')and not o.rows('eosio.signup','eosio.signup','pendingkey')
def public(url):
 q=urllib.request.Request(url,headers={'User-Agent':'PureEOS-relay-eligibility-final/1.0'})
 with urllib.request.urlopen(q,timeout=30)as f:return json.load(f)
health=[public('https://relay2.matrixai.one/health'),public('https://aic-test.matrixai.one/relay/health')];assert health[0]['bp']=='anthropiclab'and health[0]['fee_bps']==1000 and health[1]['bp']=='gptgenesisbp'and health[1]['fee_bps']==3000
owner=json.loads((R/'public-qa-private/keys.json').read_text())['owner']['Public key'];history=public('https://aic-test.matrixai.one/history/v2/history/get_key_transactions?public_key='+urllib.parse.quote(owner)+'&limit=100');assert history['chain_id']==o.CHAIN and not history['indexing']['stale'];ids={e['transaction_id']for e in history['entries']};assert all(r['transaction_id']in ids for r in receipts[1:]);o.save(R/'PUBLIC_HISTORY_QA.json',history)
ui=json.loads((R/'UI_PUBLIC.json').read_text());assert ui['status']=='PASS'and len(ui['variants'])==4 and len(ui['assets'])==6;assert all(not any(x['value']=='singaporentu'for x in variant['options'])for variant in ui['variants'])
phones=json.loads((R/'PHONE_UPDATE_BOTH_R30.json').read_text());assert phones['status']=='PASS'and phones['version']=='0.1.0-test.30'and len(phones['devices'])==2 and all(x['UID_preserved']and x['installed_hash_matches']for x in phones['devices'])
assert json.loads((R/'WALLET_LIVE_READONLY.json').read_text())['status']=='PASS_EXACT_APK_READONLY_DISCOVERY'
for unit in ['pureeos-relay2-register-20261111.service','pureeos-relay2-public-qa-20261111.service','pureeos-relay-eligibility-public-20261111.service']:
 assert subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True).strip()=='0'
assert subprocess.check_output(['systemctl','is-active','pureeos-relay2-anthropiclab-20261111.service'],text=True).strip()=='active'
assert not any(b'/opt/rly2-admin-20261111'in(p/'cmdline').read_bytes()or b'/opt/rly2-qa-20261111'in(p/'cmdline').read_bytes()for p in Path('/proc').iterdir()if p.name.isdigit()and(p/'cmdline').exists())
native='/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos';assert o.sha(Path(native).read_bytes())=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397';physical=[]
for p in Path('/proc').iterdir():
 if p.name.isdigit():
  try:
   if native in(p/'maps').read_text():physical.append(int(p.name))
  except OSError:pass
assert len(physical)==26 and set(physical)=={processes[u]['PID']for u in json.loads((R/'PROCESS_BASELINE.json').read_text())['native_units']}
f=o.rpc('get_finalizer_info');assert len(f['active_finalizer_policy']['finalizers'])==21 and f['active_finalizer_policy']['threshold']==15
kernel=subprocess.run(['journalctl','-k','--since',base['utc'],'--no-pager','-o','json'],capture_output=True,text=True,timeout=30);assert kernel.returncode==0;assert not any(any(t in str(json.loads(line).get('MESSAGE','')).lower()for t in ['out of memory','killed process','oom-kill','invoked oom'])for line in kernel.stdout.splitlines()if line.strip())
out={'status':'PASS_FINAL_ELIGIBILITY_FIX_WALLET30_RELAY2_AUDIT','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'info':o.guard(),'original34_monitored_processes_preserved':True,'native26_physical_processes_and946_binary_preserved':True,'protected6_code_ABI_pins_unchanged':True,'consensus_bans_bonds_maintenance_unchanged':True,'BP_owner_active_andproductionkeys_unchanged':True,'only_new_child_keyrelay_3restrictedlinks':True,'only_anthropiclab_registration_rate_changed':True,'original30percent_service_preserved':True,'public_health':health,'canonical_registration_funding_and2payments':receipts,'new_relay2_jobs_finalized':2,'all_three_owned_QA_history_entries_present':True,'double_snapshot_exact_solvency':reserves,'public_4browser_variants_and6assets_PASS':True,'wallet30_BOTH_installed_PASS':True,'temporary_signers_closed':True,'no_new_kernel_OOM':True,'scope':'No contract/native upgrades, BP/node/history restart, reset, ban/vote/bond changes, privatephonefiles or phonepayments. Initialfixturefailures retained; allsuccessfulpayment/installcontrollers complete and mustneverrerun.'};o.save(R/'FINAL_AUDIT.json',out);print(json.dumps({k:out[k]for k in ['status','double_snapshot_exact_solvency','new_relay2_jobs_finalized']}))
