"""Once-only application compatibility activation, after irreversible KEY upgrade.
Only sole relay and derived key-history app; never nodes/source history/business.
"""
from pathlib import Path
import json,subprocess,time,traceback,urllib.request
import ops_common as o
R=o.R;S=R/'relay-runtime';G=Path('/opt/pureeos-devnet-20261105-r1')
UNITS=['pureeos-devnet-relay-20261105.service','pureeos-devnet-key-history-20261105.service']
try:
 assert not(R/'CLIENT_ACTIVATION_STARTED.json').exists(),'Reconcile existing activation; do not repeat'
 proof=json.loads((R/'governance/key/EXECUTION.json').read_text());assert proof['status']=='UPGRADED_IRREVERSIBLE'and proof['new_code_hash']==o.NEW
 assert json.loads((R/'CLIENT_PREACTIVATION.json').read_text())['status']=='PASS'
 o.guard();o.require_codes(o.NEW);o.authority_guard()
 manifest=json.loads((R/'RUNTIME_SOURCE.json').read_text())
 for name,digest in manifest['files'].items():assert o.sha((S/name).read_bytes())==digest,name
 jobs=json.loads((G/'relay-state/journal.json').read_text());assert all(j['status']in ['finalized','expired']for j in jobs.values())
 founder=o.rpc('get_account',{'account_name':'gptgenesisbp'})['permissions'];registry=o.rows('eosio.key','eosio.key','relays');enabled=[r for r in registry if r['enabled']]
 assert len(enabled)==1 and enabled[0]['bp']=='gptgenesisbp'
 before={unit:{'unit_SHA256':o.sha(subprocess.check_output(['systemctl','cat',unit])),'PID':int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value']))}for unit in UNITS}
 dropins=[]
 for i,unit in enumerate(UNITS):
  directory=Path('/etc/systemd/system')/(unit+'.d');directory.mkdir(exist_ok=True);path=directory/'zzz-key-growth-20261107.conf';assert not path.exists()
  text='[Service]\n'
  if i==0:text+=f'WorkingDirectory={S}\nExecStart=\nExecStart={G}/tools/node-current/bin/node --max-old-space-size=128 {S}/relayer/server.mjs\nEnvironment=KEY_CODE_HASH={o.NEW}\n'
  else:text+=f'Environment=EXPECTED_KEY_CODE_HASH={o.NEW}\n'
  dropins.append((path,text))
 o.save(R/'CLIENT_ACTIVATION_STARTED.json',{'status':'READY_ONLY_TWO_APPLICATIONS','units':UNITS,'before':before,'original_terminal_job_ids':sorted(jobs),'original_journal_SHA256':o.sha((G/'relay-state/journal.json').read_bytes())})
 for path,text in dropins:path.write_text(text)
 subprocess.run(['systemctl','daemon-reload'],check=True)
 for unit in UNITS:
  subprocess.run(['systemctl','restart',unit],check=True,timeout=90);subprocess.run(['systemctl','is-active','--quiet',unit],check=True)
 deadline=time.monotonic()+90
 while True:
  try:
   with urllib.request.urlopen('http://127.0.0.1:8787/health',timeout=10)as f:health=json.load(f)
   assert health['bp']=='gptgenesisbp'and health['fee_bps']==3000
   break
  except(OSError,AssertionError):assert time.monotonic()<deadline;time.sleep(2)
 for unit in UNITS:
  env=subprocess.check_output(['systemctl','show',unit,'-p','Environment','--value'],text=True);assert 'KEY_CODE_HASH='+o.NEW in env
 current=json.loads((G/'relay-state/journal.json').read_text());assert all(current.get(k)==v for k,v in jobs.items())
 assert o.rpc('get_account',{'account_name':'gptgenesisbp'})['permissions']==founder
 assert o.rows('eosio.key','eosio.key','relays')==registry
 info=o.guard();o.require_codes(o.NEW);o.authority_guard()
 o.save(R/'CLIENTS_ACTIVATED.json',{'status':'PASS','chain_id':o.CHAIN,'KEY':o.NEW,'observed_LIB':info['last_irreversible_block_num'],'units':UNITS,'health':health,'original_terminal_jobs_preserved':len(jobs),'sole_HTTP_registry_and_founder_authority_unchanged':True,'all_26_native_processes_unchanged':True,'runtime_manifest_SHA256':o.sha((R/'RUNTIME_SOURCE.json').read_bytes())})
except BaseException:
 o.save(R/'CLIENT_ACTIVATION_FAILURE.json',{'status':'FAIL','error':traceback.format_exc(),'do_not_blindly_repeat_or_replay':True});raise
