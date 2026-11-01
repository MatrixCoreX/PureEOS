from pathlib import Path
import json,subprocess,os,time
G=Path('/campaign');R=G/'current-feature-regression';E=G/'native-build-r7/reward-tests';O=Path('/opt/aic-owner-preparation-combinations-20261029/build')
limit=time.monotonic()+900
while not E.exists():
 assert time.monotonic()<limit;time.sleep(2)
# Wait until the linker finished and the main driver successfully started a case.
while True:
 p=G/'NATIVE_RESULT.json'
 if p.exists():
  j=json.load(open(p))
  if j.get('tested_system_WASM')=='373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb':break
 assert time.monotonic()<limit;time.sleep(2)
state={'status':'RUNNING','scope':'Current final WASM supported functionality; socket-free, not live-node/TPS qualification','system_WASM':'373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb','results':[]}
env={**os.environ,'PUREEOS_TEST_ROTATION_DIR':'/campaign/contracts','PUREEOS_TEST_EXPECTED_SYSTEM_WASM':state['system_WASM']}
for suite,count in [('pureeos_resource_split_tests',14),('pureeos_single_choice_contract_tests',19),('pureeos_reward_halving_tests',10),('pureeos_reward_activation_tests',8),('pureeos_governance_contract_tests',2),('pureeos_committee_boundary_tests',3)]:
 with(R/(suite+'.log')).open('x')as log:p=subprocess.run([str(E),'--run_test='+suite,'--report_level=detailed','--color_output=no'],cwd=O,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=3600)
 state['results'].append({'suite':suite,'declared_cases':count,'exit':p.returncode});(R/'RESULT.json').write_text(json.dumps(state,indent=2)+'\n');assert p.returncode==0,suite
state['status']='PASS';state['declared_case_total']=sum(x['declared_cases']for x in state['results']);(R/'RESULT.json').write_text(json.dumps(state,indent=2)+'\n')
