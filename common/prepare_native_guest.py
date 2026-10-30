from pathlib import Path
import os,json,subprocess,shlex,hashlib,time
B=Path('/campaign');O=Path('/opt/aic-owner-preparation-combinations-20261029/build');G=Path('/opt/aic-bp-discovery-parallel-qualification-20261030')
E=B/'native-build'/('attempt-'+str(time.time_ns()));E.mkdir(parents=True)
import shutil
shutil.copy2(B/'input/native_fixture.cpp',E/'native_fixture.cpp')
(E/'SOURCE_SHA256.txt').write_text(hashlib.sha256((E/'native_fixture.cpp').read_bytes()).hexdigest()+'\n')
jobs=[]
def write(n,d):
 p=B/'native-build'/n;q=p.with_suffix('.pending');q.write_text(json.dumps(d,indent=2)+'\n');q.replace(p)
def run(label,args):
 with(E/(label+'.log')).open('x')as f:r=subprocess.run(list(map(str,args)),cwd=O,stdout=f,stderr=subprocess.STDOUT,timeout=1200)
 jobs.append(dict(label=label,returncode=r.returncode));(E/'BUILD_JOBS.json').write_text(json.dumps(jobs,indent=2)+'\n')
 assert r.returncode==0,(label,r.returncode)
db=json.loads((O/'compile_commands.json').read_text())
row=next(x for x in db if x['file'].endswith('/unittests/parallel_validation_tests.cpp'))
cmd=shlex.split(row['command']);obj=E/'native_fixture.cpp.o';cmd[cmd.index('-o')+1]=str(obj)
cmd=[str(B/'input/native_fixture.cpp')if x==row['file']else x for x in cmd]
for i,x in enumerate(cmd):
 if x.startswith('-fmodule-mapper='):
  m=E/'native.modmap';m.write_text('');cmd[i]='-fmodule-mapper='+str(m)
write('NATIVE_PREPARATION_STATUS.json',dict(status='compiling-normal-contract-deployment-and-peer-key-fixture'))
run('compile-native',cmd)
ls=(O/'build.ninja').read_text().splitlines()
start=next(i for i,l in enumerate(ls)if l.startswith('build unittests/parallel_validation_test:'))
line=next(x for x in ls[start:start+20]if x.startswith('  LINK_LIBRARIES = '))
libs=shlex.split(line.split(' = ',1)[1])
build=json.loads((G/'NODE_BUILD_REPORT.json').read_text());chain=Path(build['evidence'])/'libeosio_chain.a'
exe=E/'native_fixture';args=['/usr/bin/c++','-Wall','-O3','-DNDEBUG','-latomic','-o',str(exe),str(O/'unittests/CMakeFiles/parallel_validation_test.dir/main.cpp.o'),str(obj)]
args +=[str(chain)if x=='libraries/chain/libeosio_chain.a'else x for x in libs]
run('link-native',args)
write('NATIVE_PREPARATION_STATUS.json',dict(status='deploying-real-system-extension-registering-real-BPs-and-testing-on-chain-peer-keys',evidence=str(E)))
run('prepare-real-contract-fixture',[exe,'--report_level=detailed','--color_output=no','--run_test=bp_discovery_native_fixture'])
p=B/'fixture/PUBLIC_FIXTURE.json';assert p.exists()
write('NATIVE_PREPARATION_STATUS.json',dict(status='native-contract-fixture-prepared-awaiting-three-live-parallel-node-activation',evidence=str(E),fixture=json.loads(p.read_text())))
