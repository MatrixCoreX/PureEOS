"""Remote isolated native tests. No live RPC, keys, config or package mutation."""
from pathlib import Path
import datetime,hashlib,json,os,shutil,subprocess,sys,time,traceback,xml.etree.ElementTree as ET
R=Path('/opt/pureeos-system-signup-20261105-r1');N=R/'native-r1';S=N/'source';B=N/'cache'
V=Path('/opt/pureeos-keynet-20261103-r2/pureeos-test-1.0.0-test.1')
attempt=int(sys.argv[1]);out=N/('qualification-'+str(attempt));out.mkdir(exist_ok=False)
meta=json.loads((N/'INPUTS.json').read_text());window=json.loads((R/'WINDOW.json').read_text());assert window['status']=='PAUSED_FOR_BUILD_ONLY'
assert (N/'BUILD_RESULT.json').exists();result={'status':'RUNNING','attempt':attempt,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Isolated native controller tests; controlled tester time; no live chain action'}
def save(): (out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def remap(value):return value.replace('/work/source',str(S)).replace('/work/build',str(B))
def run(command,label,limit=600):
    remaining=window['stop_build_unix']-time.time();assert remaining>120,'Preserve restoration time'
    print(label,flush=True)
    with (out/(label+'.log')).open('w') as stream:code=subprocess.run(command,cwd=B,stdout=stream,stderr=subprocess.STDOUT,timeout=min(limit,remaining-30)).returncode
    if code:raise RuntimeError(label+' failed: '+(out/(label+'.log')).read_text()[-5500:])
def execute(binary,label,suites):
    tmp=out/(label+'-private-tmp');tmp.mkdir(mode=0o700)
    env=dict(os.environ,TMPDIR=str(tmp))
    command=[str(V/'ld-linux-x86-64.so.2'),'--library-path',str(V/'lib'),str(binary),
       '--run_test='+suites,'--report_level=detailed','--report_format=XML','--report_sink='+str(out/(label+'.xml')),
       '--log_level=test_suite','--color_output=no']
    remaining=window['stop_build_unix']-time.time();assert remaining>120
    with (out/(label+'.log')).open('w') as stream:code=subprocess.run(command,cwd=B,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=min(600,remaining-30)).returncode
    xml=out/(label+'.xml');summary=[]
    if xml.exists():
        tree=ET.parse(xml)
        for node in tree.iter('TestCase'):summary.append(dict(node.attrib))
    result[label]={'exit_code':code,'binary_sha256':sha(binary),'cases':summary,'xml_sha256':sha(xml) if xml.exists() else None};save()
    if code:raise RuntimeError(label+' failed: '+(out/(label+'.log')).read_text()[-6500:])
try:
    if attempt==1:execute(N/'native_savanna_test','native_governance','pureeos_governance_tests')
    template=next(x for x in meta['entries'] if x['source'].endswith('/tests/native_policy_boundary_fixture.cpp'))
    args=[remap(x) for x in template['argv'] if x not in {'-fmodules-ts','-fdeps-format=p1689r5','-MD'} and not x.startswith(('-fmodule-mapper=','-O','-g')) and not (x.startswith('-DPUREEOS_') and '_CONTRACTS_DIR=' in x)]
    obj=out/'signup-real.o';args[args.index('-o')+1]=str(obj);args[args.index('-c')+1]=str(S/'unittests/pureeos_signup_real_tests.cpp')
    args[1:1]=['-O1','-DPUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS=1800','-DPUREEOS_SIGNUP_REAL_CONTRACTS_DIR="'+str(N/'real-contracts')+'"']
    run(args,'compile-real',900);result['real_compile_argv']=args
    result['real_test_source_sha256']=sha(S/'unittests/pureeos_signup_real_tests.cpp');save()
    expected_build=int(os.environ.get('EXPECTED_NATIVE_BUILD','0'))
    while json.loads((N/'BUILD_RESULT.json').read_text())['attempt']<expected_build:
        assert window['stop_build_unix']-time.time()>120,'Preserve restoration time while waiting for current native link'
        time.sleep(2)
    result['native_build_manifest_sha256']=sha(N/'BUILD_RESULT.json');save()
    link=[remap(x).replace('/usr/lib/x86_64-linux-gnu/libzstd.so.1.5.7','/usr/lib/x86_64-linux-gnu/libzstd.so.1').replace('/usr/lib/x86_64-linux-gnu/libgmp.so','/usr/lib/x86_64-linux-gnu/libgmp.so.10') for x in meta['test_link'] if not x.startswith('-Wl,--dependency-file=') and (not x.endswith('.o') or x.endswith('/main.cpp.o'))]
    binary=out/'system_signup_native_test';link[link.index('-o')+1]=str(binary);link.insert(link.index('-o'),str(obj));run(link,'link-real')
    failures=[]
    if attempt>1:
        try:execute(binary,'native_governance','pureeos_governance_tests')
        except RuntimeError as error:failures.append(str(error))
    try:execute(binary,'real_contracts','pureeos_signup_real_tests')
    except RuntimeError as error:failures.append(str(error))
    root=N/'real-contracts';(root/'wire-review.md').write_text('isolated real signup review')
    run(['/usr/bin/python3',str(R/'system_account_bundle.py'),'--wasm',str(root/'eosio.signup.wasm'),'--raw-abi',str(root/'eosio.signup.raw.abi'),'--review',str(root/'wire-review.md'),'--output',str(out/'unsigned-candidate.json'),'--target','eosio.signup','--payer','developer','--developer','developer','--chain-id','0'*64,'--ram-atoms','10000','--proposal-id','1'],'unsigned-wire-helper')
    if (root/'unsigned-candidate.json').exists():shutil.copyfile(root/'unsigned-candidate.json',out/'previous-unsigned-candidate.json')
    shutil.copyfile(out/'unsigned-candidate.json',root/'unsigned-candidate.json')
    execute(binary,'unsigned_wire','pureeos_signup_wire_tests')
    result['native_raw_abi_sha256']=sha(root/'eosio.signup.raw.abi')
    result['status']='FAIL' if failures else 'PASS';result['failures']=failures;result['finished_UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();result['public_chain_or_binary_changed']=False;save();print(json.dumps(result),flush=True)
except BaseException:
    result['status']='FAIL';result['error']=traceback.format_exc();save();print(result['error'],flush=True);raise
