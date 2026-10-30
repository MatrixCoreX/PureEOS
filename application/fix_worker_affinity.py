from functional_30_common import *
Q=C/'worker-affinity-fix';Q.mkdir(exist_ok=True)
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.N=nodes;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  reports=[]
  for machine in topology['machines']:
   members=[n for n in nodes if n['host_index']==machine['host_index']]
   code="from pathlib import Path\nimport os,json,subprocess\nout=[]\nfor n in "+repr(members)+":\n pid=int(subprocess.check_output(['systemctl','show',n['unit'],'-p','MainPID','--value'],text=True));assert pid>0 and os.stat(Path('/proc')/str(pid)).st_uid==n['uid']\n tasks=list((Path('/proc')/str(pid)/'task').iterdir());before={p.name:sorted(os.sched_getaffinity(int(p.name)))for p in tasks}\n for p in tasks:\n  try:os.sched_setaffinity(int(p.name),set(range(16)))\n  except ProcessLookupError:pass\n after={p.name:sorted(os.sched_getaffinity(int(p.name)))for p in (Path('/proc')/str(pid)/'task').iterdir()};assert len(after)>=8 and all(len(v)==16 for v in after.values())\n out.append(dict(account=n['account'],pid=pid,before=before,after=after))\nprint(json.dumps(out))"
   reports+=operate(machine,code)
  f.write(Q/'ALL30_AFFINITIES.json',reports)
  receipt=f.send_fresh([f.action('eosio.token','transfer','srcab',{'from':'srcab','to':'dstab','quantity':'0.0001 SYS','memo':'all worker CPU affinities restored to full16 logical CPUs'})])
  f.write(Q/'QUALIFICATION.json',dict(status='PASS',scope='Functional 30-process launcher correction; no historical TPS rerun or performance claim',early_main_pin_caused_worker_inheritance=True,all30_main_and_worker_tasks_now_allowed_16_cpus=True,threads=reports,transaction=receipt,chain_caps_unchanged=True,node_binary_unchanged=True,TPS='unmeasured'))
except BaseException as e:
 f.write(Q/'FAILED.json',dict(error=str(e),traceback=traceback.format_exc()));raise
