"""Remote isolated controller suites; original live nodes are never test actors."""
from pathlib import Path
import datetime,hashlib,json,os,signal,subprocess,time,traceback,urllib.request,xml.etree.ElementTree as ET
R=Path('/opt/pureeos-system-signup-fix-20261105-r2');V=Path('/opt/pureeos-keynet-20261103-r2/pureeos-test-1.0.0-test.1')
maintenance=os.environ.get('FIX_MODE')=='maintenance'
attempt=int(os.environ.get('FIX_TEST_ATTEMPT','1'));build=int(os.environ.get('FIX_BUILD_ATTEMPT','1'))
out=R/('qualification-'+str(attempt));out.mkdir(exist_ok=False);binary=R/('build-'+str(build))/'system_signup_native_test'
meta=json.loads((R/('build-'+str(build))/'RESULT.json').read_text());assert meta['status']=='BUILT_NOT_TESTED_NOT_DEPLOYED'
baseline=json.loads((R/'LIVE_BASELINE.json').read_text());start=time.time();failures=0
result={'status':'RUNNING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Remote in-process native controller tests; controlled isolated tester clock; no live transaction, account, activation or installation','build':build,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
def save(): (out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
def guard():
 global failures
 assert time.time()-start<900
 assert int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))>=524288
 if maintenance:
  window=json.loads((R/'WINDOW.json').read_text());assert window['status']=='PAUSED_FOR_BUILD_ONLY' and window['stop_build_unix']-time.time()>120
  assert (Path('/proc')/str(baseline['node_pids']['pureeos-keynet2-bp-germanycrest-20261103.service'])).exists();return
 assert all((Path('/proc')/str(p)).exists() for p in baseline['node_pids'].values()),'Live process changed'
 assert int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))>=524288
 try:
  q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/get_info',data=b'{}',headers={'Content-Type':'application/json'})
  i=json.load(urllib.request.urlopen(q,timeout=4));assert i['chain_id']==baseline['chain_id']
  age=(datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)-datetime.datetime.fromisoformat(i['head_block_time'])).total_seconds()
  bad=age>max(8,baseline['head_age_seconds']+5) or i['head_block_num']-i['last_irreversible_block_num']>max(60,baseline['head_LIB_gap']+24)
 except Exception:bad=True
 failures=failures+1 if bad else 0;assert failures<3,'Original live finality guard'
def run(argv,label):
 guard();log=out/(label+'.log');print(label,flush=True)
 with log.open('w') as f:
  child=subprocess.Popen(argv,cwd=R/'cache',stdout=f,stderr=subprocess.STDOUT,env=dict(os.environ,TMPDIR=str(out/'private-tmp')),start_new_session=True)
  try:
   while child.poll() is None:time.sleep(2);guard()
  except BaseException:
   os.killpg(child.pid,signal.SIGTERM)
   try:child.wait(timeout=5)
   except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait()
   raise
 return child.returncode
def execute(label,suite):
 xml=out/(label+'.xml')
 code=run([str(V/'ld-linux-x86-64.so.2'),'--library-path',str(V/'lib'),str(binary),'--run_test='+suite,'--report_level=detailed','--report_format=XML','--report_sink='+str(xml),'--log_level=test_suite','--color_output=no'],label)
 cases=[dict(n.attrib) for n in ET.parse(xml).iter('TestCase')] if xml.exists() else []
 result[label]={'exit_code':code,'cases':cases,'xml_sha256':hashlib.sha256(xml.read_bytes()).hexdigest() if xml.exists() else None};save()
 return code==0
try:
 (out/'private-tmp').mkdir(mode=0o700);save()
 passed=[execute('native_governance','pureeos_governance_tests'),execute('real_contracts','pureeos_signup_real_tests')]
 contracts=R/'real-contracts';(contracts/'wire-review.md').write_text('isolated real signup review')
 args=['python3',str(R/'source/tools/governance/system_account_bundle.py'),'--wasm',str(contracts/'eosio.signup.wasm'),'--raw-abi',str(contracts/'eosio.signup.raw.abi'),'--review',str(contracts/'wire-review.md'),'--output',str(out/'unsigned-candidate.json'),'--target','eosio.signup','--payer','developer','--developer','developer','--chain-id','0'*64,'--ram-atoms','10000','--proposal-id','1']
 code=run(args,'unsigned-wire-helper');assert not code,'Unsigned helper failed'
 (contracts/'unsigned-candidate.json').write_bytes((out/'unsigned-candidate.json').read_bytes())
 passed.append(execute('unsigned_wire','pureeos_signup_wire_tests'))
 result.update(status='PASS' if all(passed) else 'FAIL',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),elapsed_seconds=time.time()-start);save()
except BaseException:
 result.update(status='FAIL',error=traceback.format_exc());save();print(result['error'],flush=True);raise
print(json.dumps({k:({'exit_code':v['exit_code'],'cases':len(v['cases']),'failed':[c['name'] for c in v['cases'] if c['result']!='passed']} if isinstance(v,dict) and 'cases' in v else v) for k,v in result.items()}),flush=True)
