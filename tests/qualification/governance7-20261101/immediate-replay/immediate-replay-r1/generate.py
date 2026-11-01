from pathlib import Path
import json,subprocess,shlex,hashlib,time,os
P=Path('/campaign');O=Path('/opt/aic-owner-preparation-combinations-20261029/build');E=P/'immediate-replay-r1/build';E.mkdir()
report={'status':'running','cases':[]};jobs=[]
def save():
 report['jobs']=jobs;(P/'immediate-replay-r1/GENERATOR_RESULT.json').write_text(json.dumps(report,indent=2)+'\n')
def run(label,cmd,env=None):
 with(E/(label+'.log')).open('x')as out:r=subprocess.run(list(map(str,cmd)),cwd=O,stdout=out,stderr=subprocess.STDOUT,timeout=1800,env=env)
 jobs.append({'name':label,'exit':r.returncode});save()
 return r.returncode
try:
 rows=json.loads((O/'compile_commands.json').read_text());row=next(r for r in rows if r['file'].endswith('/pureeos_governance_tests.cpp'))
 cmd=shlex.split(row['command']);cmd[cmd.index('-o')+1]=str(E/'reward-tests.o')
 for i,arg in enumerate(cmd):
  if arg==row['file']:cmd[i]=str(P/'immediate-replay-r1/native-source/pureeos_governance_tests.cpp')
  if arg=='-MF':cmd[i+1]=str(E/'reward-tests.d')
  if arg.startswith('-fdeps-file='):cmd[i]='-fdeps-file='+str(E/'reward-tests.ddi')
  if arg.startswith('-fmodule-mapper='):
   (E/'native.modmap').write_text('');cmd[i]='-fmodule-mapper='+str(E/'native.modmap')
 cmd=[a.replace('/work/source','/opt/aic-owner-preparation-combinations-20261029/source').replace('/work/build',str(O))for a in cmd]
 cmd=[('-D'+a.split('=',1)[0][2:]+'=\"/campaign/contracts\"')if a.split('=',1)[0] in ['-DPUREEOS_GOVERNANCE_CONTRACTS_DIR','-DPUREEOS_SINGLE_CHOICE_CONTRACTS_DIR','-DPUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR','-DPUREEOS_ROTATION_CONTRACTS_DIR']else a for a in cmd]
 cmd[1:1]=['-I/campaign/source/libraries/chain/include','-I/campaign/source/plugins/chain_plugin/include']
 assert run('compile',cmd)==0
 lines=(O/'build.ninja').read_text().splitlines();n=next(i for i,l in enumerate(lines)if l.startswith('build unittests/unit_test:'));line=next(l for l in lines[n:n+20]if l.startswith('  LINK_LIBRARIES = '));libs=shlex.split(line.split(' = ',1)[1])
 G=Path('/opt/aic-bp-discovery-parallel-qualification-20261030');b=json.loads((G/'NODE_BUILD_REPORT.json').read_text());chain=P/'build/libeosio_chain.a'
 exe=E/'reward-tests';cmd=['/usr/bin/c++','-Wall','-O3','-DNDEBUG','-latomic','-o',str(exe),str(O/'unittests/CMakeFiles/parallel_validation_test.dir/main.cpp.o'),str(E/'reward-tests.o')]+[str(chain)if a=='libraries/chain/libeosio_chain.a'else str(P/'build/libchain_plugin.a')if a=='plugins/chain_plugin/libchain_plugin.a'else a for a in libs]
 assert run('link',cmd)==0
 report['native_program_SHA256']=hashlib.sha256(exe.read_bytes()).hexdigest();report['core_library_SHA256']=hashlib.sha256(chain.read_bytes()).hexdigest()
 cases={'pureeos_governance_v2_extra_tests':['immediate_reviewed_upgrade_export_for_nodeos_full_replay']}
 report['tested_system_WASM']=hashlib.sha256((P/'contracts/eosio.system/eosio.system.wasm').read_bytes()).hexdigest();assert report['tested_system_WASM']=='373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb'
 env={**os.environ,'PUREEOS_TEST_ROTATION_DIR':'/campaign/contracts','PUREEOS_TEST_EXPECTED_SYSTEM_WASM':'373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb'}
 for suite,names in cases.items():
  for name in names:
   rc=run(name,[exe,'--run_test='+suite+'/'+name,'--report_level=detailed','--color_output=no'],env)
   report['cases'].append({'name':suite+'/'+name,'exit':rc});save()
   assert rc==0,name
 report['validated_positive_case_total']=len(report['cases']);report['status']='PASS';save()
except BaseException as error:
 import traceback
 report.update(status='FAIL',error=traceback.format_exc());save();raise
