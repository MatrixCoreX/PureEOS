"""Remote native/real-WASM/wire qualification of the new genesis candidate."""
from pathlib import Path
import datetime,hashlib,json,os,subprocess,time,traceback,xml.etree.ElementTree as ET
R=Path('/opt/pureeos-dev-account-genesis-20261105-r1');V=Path('/opt/pureeos-keynet-20261103-r2/pureeos-test-1.0.0-test.1')
build=int(os.environ.get('BUILD_ATTEMPT','1'));attempt=int(os.environ.get('QUALIFY_ATTEMPT','1'));deadline=time.time()+3650
while True:
 m=json.loads((R/'build-5/RESULT.json').read_text());assert m['status']!='FAIL','Preserved build5 failure'
 if m['status']=='BUILT_REQUIRES_QUALIFICATION':break
 assert time.time()<deadline;time.sleep(4)
assert hashlib.sha256((R/'build-5/nodeos').read_bytes()).hexdigest()==m['nodeos']['sha256']
OUT=R/('qualification-'+str(attempt));OUT.mkdir(exist_ok=False);(OUT/'private-tmp').mkdir(mode=0o700)
pre=json.loads((R/'PREFLIGHT5.json').read_text());contracts=R/'real-contracts';binary=Path(os.environ.get('TEST_BINARY',str(R/('build-'+str(build))/'native-tests')))
result={'status':'RUNNING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Remote isolated native controllers; full actual fees/REX at depth4; no live deployment in qualification','build':build,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'suites':{}}
def save():(OUT/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
def run(argv,label):
 assert all((Path('/proc')/str(p)).exists()for p in pre['node_pids'].values())
 with (OUT/(label+'.log')).open('w')as log:
  p=subprocess.run(argv,cwd=R/'cache',env=dict(os.environ,TMPDIR=str(OUT/'private-tmp')),stdout=log,stderr=subprocess.STDOUT,timeout=600)
 assert all((Path('/proc')/str(p)).exists()for p in pre['node_pids'].values());return p.returncode
def execute(label,suite,expected):
 xml=OUT/(label+'.xml');code=run([str(V/'ld-linux-x86-64.so.2'),'--library-path',str(V/'lib'),str(binary),'--run_test='+suite,'--report_level=detailed','--report_format=XML','--report_sink='+str(xml),'--log_level=test_suite','--color_output=no'],label)
 cases=[dict(t.attrib)for t in ET.parse(xml).iter('TestCase')]if xml.exists()else[]
 ok=code==0 and len(cases)==expected and all(c['result']=='passed'for c in cases)
 result['suites'][label]={'status':'PASS'if ok else'FAIL','exit_code':code,'cases':cases,'xml_sha256':hashlib.sha256(xml.read_bytes()).hexdigest()if xml.exists()else None};save();return ok
try:
 save();passed=[execute('native_governance','pureeos_governance_tests',46),execute('real_contracts','pureeos_signup_real_tests',8)]
 (contracts/'wire-review.md').write_text('isolated real signup review');(contracts/'initialization-example.bin').write_bytes(bytes([1,2,3]))
 for label,target,payment,stem in [('regular','eosio.signup',10000,'eosio.signup'),('short','usd',10000,'eosio.signup'),('init','eosio.key',100000,'eosio.key'),('issuer','usd',10000,'custody.token')]:
  name={'regular':'unsigned-candidate.json','short':'unsigned-short-candidate.json','init':'unsigned-init-candidate.json','issuer':'unsigned-issuer-candidate.json'}[label]
  argv=['python3',str(R/'source-5/tools/governance/system_account_bundle.py'),'--wasm',str(contracts/(stem+'.wasm')),'--raw-abi',str(contracts/(stem+'.raw.abi')),'--review',str(contracts/'wire-review.md'),'--output',str(OUT/name),'--target',target,'--payer','developer','--developer','developer','--chain-id','0'*64,'--ram-atoms',str(payment),'--proposal-id','1']
  if label in ['init','issuer']:argv+=['--init-data',str(contracts/'initialization-example.bin')]
  if label=='issuer':argv+=['--ordinary-issuer']
  assert run(argv,'helper-'+label)==0,'Wire helper failure '+label
  (contracts/name).write_bytes((OUT/name).read_bytes())
 passed.append(execute('unsigned_wire','pureeos_signup_wire_tests',4))
 assert hashlib.sha256((contracts/'eosio.key.raw.abi').read_bytes()).hexdigest()=='3470a5e243680c4c93e7ef4b481bcd54cb4f626ed3d92331f414386bb5bb0043'
 result.update(status='PASS'if all(passed)else'FAIL',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),current25PIDs_preserved=True)
 save();print(json.dumps({**{k:v for k,v in result.items()if k!='suites'},'summary':{k:{'status':v['status'],'cases':len(v['cases']),'failed':[c['name']for c in v['cases']if c['result']!='passed']}for k,v in result['suites'].items()}}),flush=True)
except BaseException:
 result.update(status='FAIL',error=traceback.format_exc());save();raise
