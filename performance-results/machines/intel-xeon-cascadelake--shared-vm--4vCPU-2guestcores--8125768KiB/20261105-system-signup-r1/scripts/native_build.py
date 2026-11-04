"""Remote-only bounded candidate build. Never writes a live package or unit."""
from pathlib import Path
import datetime,hashlib,json,os,re,subprocess,sys,time,traceback
ROOT=Path('/opt/pureeos-system-signup-20261105-r1')
N=ROOT/'native-r1';S=N/'source';B=N/'cache'
meta=json.loads((N/'INPUTS.json').read_text())
attempt=int(sys.argv[1]);logs=N/('logs-attempt-'+str(attempt));logs.mkdir(exist_ok=False)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def remap(value):return value.replace('/work/source',str(S)).replace('/work/build',str(B))
def save(path,data):
    p=path.with_suffix(path.suffix+'.tmp');p.write_text(json.dumps(data,indent=2)+'\n');p.replace(path)
window=json.loads((ROOT/'WINDOW.json').read_text());assert window['status']=='PAUSED_FOR_BUILD_ONLY'
assert time.time()<window['stop_build_unix']
generated=json.loads((N/'GENERATED_ADAPTATIONS.json').read_text()) if (N/'GENERATED_ADAPTATIONS.json').exists() else {}
overrides=dict(generated)
if (N/'PATCHES.json').exists():overrides.update(json.loads((N/'PATCHES.json').read_text()))
for name,item in meta['files'].items():
    expected=overrides.get(name,item['sha256'])
    assert sha(N/name)==expected,'Input mismatch: '+name
for p in [B/'libraries/testing/contracts.cpp', B/'unittests/include/snapshots.hpp']:
    text=p.read_text();updated=remap(text)
    if text!=updated:p.write_text(updated)
    generated[str(p.relative_to(N))]=sha(p)
version=B/'libraries/version/src/version_impl.cpp'
text=version.read_text()
text=re.sub(r'(const std::string\s+version_hash\s*\{)"[^"]*"',lambda m:m[1]+json.dumps(meta['source_commit']),text)
version.write_text(text);generated[str(version.relative_to(N))]=sha(version)
# Generated path/version changes are recorded, not represented as original source.
save(N/'GENERATED_ADAPTATIONS.json',generated)
header_digest=hashlib.sha256(json.dumps({name:sha(N/name) for name in meta['files'] if Path(name).suffix in {'.h','.hpp','.hh','.hxx','.inc','.ipp','.inl','.def'}},sort_keys=True).encode()).hexdigest()
old=json.loads((N/'JOBS.json').read_text()) if (N/'JOBS.json').exists() else {}
jobs={};start=time.time()
try:
    priority=['system_account_proposal.cpp','controller.cpp','eosio_contract.cpp','pureeos_genesis_tests.cpp','chain_plugin.cpp','chain_api_plugin.cpp']
    entries=sorted(meta['entries'],key=lambda x:priority.index(Path(x['source']).name) if Path(x['source']).name in priority else 99)
    for entry in entries:
        assert time.time()<window['stop_build_unix'],'90minute build deadline reached'
        args=[remap(x) for x in entry['argv'] if x not in {'-fmodules-ts','-fdeps-format=p1689r5','-MD'} and not x.startswith(('-fmodule-mapper=','-O','-g')) and not (x.startswith('-DPUREEOS_') and '_CONTRACTS_DIR=' in x)]
        args[1:1]=['-O1','-fdiagnostics-color=never']
        if entry['library']=='libraries/chain/libeosio_chain.a' or entry['source'].endswith('/programs/nodeos/main.cpp'):
            args[1:1]=['-DPUREEOS_TEST_BUILD=1','-DPUREEOS_TEST_GOVERNANCE_30MIN=1']
        if entry.get('test_only'):args[1:1]=['-DPUREEOS_EXPECTED_SYSTEM_REVIEW_SECONDS=1800']
        out=B/entry['output'];out.parent.mkdir(parents=True,exist_ok=True)
        fingerprint=hashlib.sha256(json.dumps([args,header_digest,sha(remap(entry['source']))]).encode()).hexdigest()
        cached=old.get(entry['output'])
        if cached and cached['fingerprint']==fingerprint and out.exists() and sha(out)==cached['sha256']:
            jobs[entry['output']]=cached;print('REUSE '+entry['output'],flush=True);continue
        log=logs/(entry['output'].replace('/','_')+'.log')
        print('COMPILE '+str(len(jobs)+1)+'/'+str(len(entries))+' '+entry['output'],flush=True)
        with log.open('w') as stream:
            result=subprocess.run(args,cwd=B,stdout=stream,stderr=subprocess.STDOUT,timeout=max(1,window['stop_build_unix']-time.time()))
        assert result.returncode==0,log.read_text()[-6500:]
        jobs[entry['output']]={'source':entry['source'],'argv':args,'fingerprint':fingerprint,'sha256':sha(out),'attempt':attempt}
        save(N/'JOBS.json',{**old,**jobs})
    for library,objects in meta['groups'].items():
        path=B/library;path.unlink(missing_ok=True)
        subprocess.run(['ar','rcs',str(path),*[str(B/x) for x in objects]],check=True)
        generated['cache/'+library]=sha(path)
    save(N/'GENERATED_ADAPTATIONS.json',generated)
    outputs={}
    for label,original in [('nodeos',meta['link']),('native_savanna_test',meta['test_link'])]:
        args=[remap(x).replace('/usr/lib/x86_64-linux-gnu/libzstd.so.1.5.7','/usr/lib/x86_64-linux-gnu/libzstd.so.1').replace('/usr/lib/x86_64-linux-gnu/libgmp.so','/usr/lib/x86_64-linux-gnu/libgmp.so.10') for x in original if not x.startswith('-Wl,--dependency-file=')]
        args[args.index('-o')+1]=str(N/label)
        with (logs/('link-'+label+'.log')).open('w') as stream:result=subprocess.run(args,cwd=B,stdout=stream,stderr=subprocess.STDOUT,timeout=max(1,window['stop_build_unix']-time.time()))
        assert not result.returncode,(logs/('link-'+label+'.log')).read_text()[-6500:]
        outputs[label]={'sha256':sha(N/label),'bytes':(N/label).stat().st_size,'link_argv':args}
    result={'status':'BUILT_NOT_QUALIFIED_NOT_DEPLOYED','attempt':attempt,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_commit':meta['source_commit'],'objects':len(jobs),'elapsed_seconds':time.time()-start,'optimization':'O1','TEST_review_seconds':1800,'outputs':outputs,'all_first_party_native_objects_rebuilt':True}
    save(N/'BUILD_RESULT.json',result);print(json.dumps(result),flush=True)
except BaseException:
    result={'status':'FAIL','attempt':attempt,'error':traceback.format_exc(),'successful_this_attempt':len(jobs)}
    save(N/('BUILD_FAILURE_'+str(attempt)+'.json'),result);print(result['error'],flush=True);raise
