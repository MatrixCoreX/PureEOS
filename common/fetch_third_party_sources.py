from pathlib import Path
import os,subprocess,json,hashlib,time,concurrent.futures
B=Path('/opt/aic-bp-discovery-live-20261030');D=B/'third-party-sources';D.mkdir(exist_ok=True)
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
repos=[('bancor-eos','https://github.com/bancorprotocol/contracts_eos.git'),('atomicassets','https://github.com/atomicassets/atomicassets-contract.git'),('atomicmarket','https://github.com/atomicassets/atomicmarket-contract.git')]
def get(entry):
 name,url=entry;p=D/name;env=dict(os.environ,GIT_TERMINAL_PROMPT='0',GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null')
 r=subprocess.run(['git','-c','http.version=HTTP/1.1','clone','--depth=1','--no-tags',url,str(p)],capture_output=True,text=True,timeout=180,env=env)
 (D/(name+'-download.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,name
 g=['git','-c','safe.directory=*','-C',str(p)]
 commit=subprocess.check_output(g+['rev-parse','HEAD'],text=True).strip()
 subprocess.run(g+['checkout','--detach',commit],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True)
 licenses={str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest()for x in p.glob('*LICENSE*')if x.is_file()}
 return dict(name=name,url=url,commit=commit,path=str(p),license_files_sha256=licenses,status='source-pinned-not-built-or-deployed',upstream_restart_scripts_executed=False)
with concurrent.futures.ThreadPoolExecutor(max_workers=3)as pool:proof=list(pool.map(get,repos))
(D/'PROVENANCE.json').write_text(json.dumps(dict(status='real-third-party-sources-pinned',acquired_UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),repositories=proof,pressure_load=False),indent=2)+'\n')
print(json.dumps(dict(status='sources-downloaded-no-build-or-deployment-claim',repositories=[dict(name=x['name'],commit=x['commit'])for x in proof])))
