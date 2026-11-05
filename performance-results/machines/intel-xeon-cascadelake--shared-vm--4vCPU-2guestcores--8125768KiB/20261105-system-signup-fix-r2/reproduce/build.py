"""Remote incremental fix build. Never pauses or installs into the live chain."""
from pathlib import Path
import datetime,hashlib,json,os,signal,subprocess,time,traceback,urllib.request
R=Path('/opt/pureeos-system-signup-fix-20261105-r2'); S=R/'source';B=R/'cache'
OLD=Path('/opt/pureeos-system-signup-20261105-r1/native-r1')
meta=json.loads((OLD/'INPUTS.json').read_text());oldjobs=json.loads((OLD/'JOBS.json').read_text())
maintenance=os.environ.get('FIX_MODE')=='maintenance'
attempt=int(os.environ.get('FIX_ATTEMPT','1'));out=R/('build-'+str(attempt));out.mkdir(exist_ok=False)
result={'status':'RUNNING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Remote bounded incremental repair; live original programs remain running','optimization':'Changed objects O0 with aggressive GCC GC; unchanged verified O1 objects reused','jobs':{}}
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(): (out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
def rpc(method):
 req=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=b'{}',headers={'Content-Type':'application/json'})
 return json.load(urllib.request.urlopen(req,timeout=4))
def remap(x):return x.replace('/work/source',str(S)).replace('/work/build',str(B))
baseline=json.loads((R/'LIVE_BASELINE.json').read_text()); start=time.time();live_failures=0
def guard():
 global live_failures
 if maintenance:
  window=json.loads((R/'WINDOW.json').read_text());assert window['status']=='PAUSED_FOR_BUILD_ONLY' and window['stop_build_unix']-time.time()>120,'Preserve restoration time'
 else:assert time.time()-start<2400,'Bounded build deadline'
 free=int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
 assert free>=524288,'Stop owned build: less than512MiB available'
 if maintenance:
  assert (Path('/proc')/str(baseline['node_pids']['pureeos-keynet2-bp-germanycrest-20261103.service'])).exists();return
 assert all((Path('/proc')/str(p)).exists() for p in baseline['node_pids'].values()),'Original process changed'
 try:
  info=rpc('get_info');assert info['chain_id']==baseline['chain_id']
  age=(datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)-datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds()
  unhealthy=age>max(8,baseline['head_age_seconds']+5) or info['head_block_num']-info['last_irreversible_block_num']>max(60,baseline['head_LIB_gap']+24)
 except Exception:unhealthy=True
 live_failures=live_failures+1 if unhealthy else 0
 assert live_failures<3,'Stop owned build: live finality/response guard repeated'
def run(argv,label):
 guard();log=out/(label+'.log');print(label,flush=True)
 with log.open('w') as stream:
  child=subprocess.Popen(argv,cwd=B,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
  try:
   while child.poll() is None:time.sleep(5);guard()
  except BaseException:
   os.killpg(child.pid,signal.SIGTERM)
   try:child.wait(timeout=5)
   except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait()
   raise
 assert child.returncode==0,label+' failed: '+log.read_text()[-5000:]
def normalize_link(argv):
 return [remap(x).replace('/usr/lib/x86_64-linux-gnu/libzstd.so.1.5.7','/usr/lib/x86_64-linux-gnu/libzstd.so.1').replace('/usr/lib/x86_64-linux-gnu/libgmp.so','/usr/lib/x86_64-linux-gnu/libgmp.so.10') for x in argv if not x.startswith('-Wl,--dependency-file=')]
try:
 for name,digest in json.loads((R/'SOURCE_HASHES.json').read_text()).items():assert sha(S/name)==digest,name
 names=['system_account_proposal.cpp','eosio_contract.cpp','controller.cpp','chain_plugin.cpp']
 selected=[next(x for x in meta['entries'] if Path(x['source']).name==name) for name in names]
 selected_outputs={e['output'] for e in selected}
 for name,job in oldjobs.items():
  if name not in selected_outputs:assert sha(B/name)==job['sha256'],'Reused object mismatch '+name
 prior_jobs={}
 for previous in sorted(R.glob('build-*/RESULT.json')):
  if previous.parent==out:continue
  prior_jobs.update(json.loads(previous.read_text()).get('jobs',{}))
 for entry in selected:
  argv=[remap(x) for x in entry['argv'] if x not in {'-fmodules-ts','-fdeps-format=p1689r5','-MD'} and not x.startswith(('-fmodule-mapper=','-O','-g')) and not (x.startswith('-DPUREEOS_') and '_CONTRACTS_DIR=' in x)]
  argv[1:1]=['-O1','-ftrack-macro-expansion=0','-fdiagnostics-color=never'] if maintenance else ['-O0','--param=ggc-min-expand=5','--param=ggc-min-heapsize=16384','-ftrack-macro-expansion=0','-fdiagnostics-color=never']
  if entry['library']=='libraries/chain/libeosio_chain.a':argv[1:1]=['-DPUREEOS_TEST_BUILD=1','-DPUREEOS_TEST_GOVERNANCE_30MIN=1']
  obj=B/entry['output'];prior=prior_jobs.get(entry['output'])
  if prior and obj.exists() and prior['source_sha256']==sha(remap(entry['source'])) and sha(obj)==prior['sha256']:
   result['jobs'][entry['output']]=prior;save();continue
  obj.unlink(missing_ok=True);run(argv,'compile-'+Path(entry['source']).stem)
  result['jobs'][entry['output']]={'sha256':sha(obj),'source_sha256':sha(remap(entry['source'])),'argv':argv};save()
 for library in {e['library'] for e in selected}:
  dest=B/library;dest.unlink();run(['ar','rcs',str(dest),*[str(B/x) for x in meta['groups'][library]]],'archive-'+Path(library).name)
 argv=normalize_link(meta['link']);argv[argv.index('-o')+1]=str(out/'nodeos');run(argv,'link-nodeos')
 result['nodeos']={'sha256':sha(out/'nodeos'),'bytes':(out/'nodeos').stat().st_size};save()
 template=next(x for x in meta['entries'] if x['source'].endswith('/tests/native_policy_boundary_fixture.cpp'))
 argv=[remap(x) for x in template['argv'] if x not in {'-fmodules-ts','-fdeps-format=p1689r5','-MD'} and not x.startswith(('-fmodule-mapper=','-O','-g')) and not (x.startswith('-DPUREEOS_') and '_CONTRACTS_DIR=' in x)]
 argv[1:1]=(['-O1','-ftrack-macro-expansion=0'] if maintenance else ['-O0','--param=ggc-min-expand=5','--param=ggc-min-heapsize=16384','-ftrack-macro-expansion=0'])+['-DPUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS=1800','-DPUREEOS_SIGNUP_REAL_CONTRACTS_DIR="'+str(R/'real-contracts')+'"']
 obj=out/'signup-real.o';argv[argv.index('-o')+1]=str(obj);argv[argv.index('-c')+1]=str(S/'unittests/pureeos_signup_real_tests.cpp');run(argv,'compile-real-tests')
 argv=[x for x in normalize_link(meta['test_link']) if not x.endswith('.o') or x.endswith('/main.cpp.o')];argv[argv.index('-o')+1]=str(out/'system_signup_native_test');argv.insert(argv.index('-o'),str(obj));run(argv,'link-real-tests')
 result.update(status='BUILT_NOT_TESTED_NOT_DEPLOYED',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),elapsed_seconds=time.time()-start,test_binary_sha256=sha(out/'system_signup_native_test'));save()
except BaseException:
 result.update(status='FAIL',error=traceback.format_exc(),elapsed_seconds=time.time()-start);save();print(result['error'],flush=True);raise
