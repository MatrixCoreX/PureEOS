from pathlib import Path
import os,subprocess,json,time,hashlib,traceback
B=Path('/campaign');SDK=Path('/work/sdk/aic-cdt-1.0.0');E=B/'third-party-build'/('attempt-'+str(time.time_ns()));E.mkdir(parents=True)
report=dict(status='building-pinned-real-third-party-contracts',evidence=str(E),contracts=[],pressure_load=False)
def save():
 p=B/'third-party-build/STATUS.json';q=p.with_suffix('.pending');q.write_text(json.dumps(report,indent=2)+'\n');q.replace(p)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
try:
 compiler=SDK/'bin/cdt-cpp';assert compiler.exists()
 v=subprocess.run([str(compiler),'--version'],capture_output=True,text=True);(E/'COMPILER_VERSION.txt').write_text(v.stdout+v.stderr);assert v.returncode==0
 jobs=[('atomicassets','atomicassets','src/atomicassets.cpp',['-I./include']),('atomicmarket','atomicmarket','src/atomicmarket.cpp',['-I./include','-R./resource'])]
 jobs +=[('bancor-eos',n,'contracts/eos/'+n+'/'+n+'.cpp',['-I.'])for n in ['Token','BancorConverter','BancorNetwork']]
 for repo,contract,src,flags in jobs:
  root=B/'third-party-sources'/repo;D=E/contract;D.mkdir()
  output=D/(contract+'.wasm');args=[str(compiler),'-abigen','-contract='+contract,*flags,src,'-o',str(output)]
  item=dict(repository=repo,contract=contract,arguments=args,source=str(root/src),status='RUNNING');report['contracts'].append(item);save()
  with(D/'compile.log').open('x')as log:r=subprocess.run(args,cwd=root,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
  item['returncode']=r.returncode;item['status']='built'if r.returncode==0 else'FAIL'
  if r.returncode==0:
   for suffix in ['wasm','abi']:
    p=D/(contract+'.'+suffix);assert p.exists(),str(p);item[suffix+'_sha256']=sha(p)
   item['actions']=[a['name']for a in json.loads((D/(contract+'.abi')).read_text())['actions']]
  save()
 report['status']='all-pinned-contracts-built'if all(x['status']=='built'for x in report['contracts'])else'build-failures-preserved-for-compatibility-review'
 save()
except BaseException as e:
 report.update(status='build-controller-error',error=str(e),traceback=traceback.format_exc());save();raise
