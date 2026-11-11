"""Execute the production guard with isolated paths and mocked RPC/systemd, never real nodes."""
import ast,contextlib,datetime,hashlib,io,json,subprocess,tempfile,unittest
from pathlib import Path
from unittest.mock import patch

SOURCE=Path(__file__).with_name('recycle_guard.py')
PIN='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
class Harness:
 def __init__(self):
  self.temp=tempfile.TemporaryDirectory();self.base=Path(self.temp.name);self.r=self.base/'case';self.old=self.base/'old';self.binary=self.base/'native';self.binary.write_bytes(b'isolated native fixture')
  self.r.mkdir();self.old.mkdir();self.names=[f'n{i:02d}'for i in range(25)];self.pids={n:str(1000+i)for i,n in enumerate(self.names)};self.calls=[];self.votes=21;self.stale=False;self.fail_start=False;self.pin_bad=False;self.missed=set()
  self.active=self.names[:21];self.banned=[f'b{i:02d}'for i in range(15)]
  self.nodes={n:{'name':n,'unit':n+'.service','rpc_port':9000+i,'data_dir':str(self.base/'data'/n)}for i,n in enumerate(self.names)}
  for n,p in self.pids.items():
   d=self.base/'proc'/p;d.mkdir(parents=True);(d/'status').write_text('VmRSS: 60000 kB\nVmSwap: 0 kB\n')
   safety=Path(self.nodes[n]['data_dir'])/'finalizers';safety.mkdir(parents=True);(safety/'safety.dat').write_bytes(b'committed previous vote fixture')
  self.put(self.old/'RESTORE_NODE_PATHS.json',{n:self.nodes[n]for n in self.active});self.put(self.old/'REPLACEMENTS_PREPARED.json',{'nodes':[]})
  self.put(self.r/'REPLACEMENTS_PREPARED.json',{'nodes':[self.nodes[n]for n in self.names[21:]]})
  self.put(self.r/'REPLACEMENT_LIVE_BASELINE.json',{'banned':self.banned});self.put(self.r/'REPLACEMENT_ENROLL_RESULT.json',{'status':'PASS_4_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS'})
 def put(self,p,v):p.write_text(json.dumps(v))
 def high(self,n):
  (self.base/'proc'/self.pids[n]/'status').write_text('VmRSS: 1800000 kB\nVmSwap: 0 kB\n')
 def stamp(self):return(datetime.datetime.now(datetime.timezone.utc)-datetime.timedelta(seconds=60 if self.stale else 0)).replace(tzinfo=None).isoformat()
 def urlopen(self,q,**kw):
  name=q.full_url.rsplit('/',1)[-1];assert name.startswith('get_');data=json.loads(q.data)
  if name=='get_info':v={'chain_id':'09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532','head_block_num':700000,'last_irreversible_block_num':699998,'head_block_time':self.stamp(),'last_irreversible_block_time':self.stamp()}
  elif name=='get_table_rows':
   rows=[{'owner':n,'banned':False,'zero_windows':int(n in self.missed)}for n in self.names]+[{'owner':n,'banned':True,'zero_windows':3}for n in self.banned]+[{'owner':'montrealmila','banned':False,'zero_windows':0}]
   v={'rows':rows,'more':False}
  elif name=='get_finalizer_info':v={'active_finalizer_policy':{'threshold':15,'finalizers':[{'description':n}for n in self.active]},'pending_finalizer_policy':None,'last_tracked_votes':[{'description':n,'is_vote_strong':True,'voted_for_block_timestamp':self.stamp()}for n in self.active[:self.votes]]}
  elif name=='get_producer_schedule':v={'active':{'producers':[{'producer_name':n}for n in self.active]},'pending':None,'proposed':None}
  else:raise AssertionError(name)
  return io.BytesIO(json.dumps(v).encode())
 def output(self,args,**kw):
  assert args[:2]==['systemctl','show'];n=args[2].removesuffix('.service');p=self.pids[n]
  return 'MainPID='+p+'\nActiveState='+('active'if p!='0'else'inactive')+'\n'
 def run(self,args,**kw):
  self.calls.append(args)
  if args[:2]==['systemctl','stop']and args[2].removesuffix('.service')in self.pids:self.pids[args[2].removesuffix('.service')]='0'
  if args[:2]==['systemctl','start']:
   if self.fail_start:raise subprocess.CalledProcessError(1,args)
   self.pids[args[2].removesuffix('.service')]='9999'
  return subprocess.CompletedProcess(args,0,b'',b'')
 def execute(self,check=False):
  harness=self
  class Rewrite(ast.NodeTransformer):
   def visit_Assign(self,node):
    node=self.generic_visit(node)
    for t in node.targets:
     if isinstance(t,ast.Name)and t.id in['R','OLD','NATIVE']:
      value={'R':harness.r,'OLD':harness.old,'NATIVE':harness.binary}[t.id];node.value=ast.Call(func=ast.Name(id='Path',ctx=ast.Load()),args=[ast.Constant(str(value))],keywords=[])
    return node
   def visit_Constant(self,node):
    if node.value=='/proc':return ast.copy_location(ast.Constant(str(harness.base/'proc')),node)
    if node.value==PIN:return ast.copy_location(ast.Constant('wrong'if harness.pin_bad else hashlib.sha256(harness.binary.read_bytes()).hexdigest()),node)
    return node
  tree=ast.fix_missing_locations(Rewrite().visit(ast.parse(SOURCE.read_text())))
  with patch('sys.argv',['recycle_guard.py']+(['--check-only']if check else[])),patch('urllib.request.urlopen',self.urlopen),patch('subprocess.check_output',self.output),patch('subprocess.run',self.run),patch('time.sleep',lambda _:None),contextlib.redirect_stdout(io.StringIO()):
   env={'__name__':'__main__'}
   try:exec(compile(tree,str(SOURCE),'exec'),env)
   except SystemExit:pass
   finally:
    if'lock'in env:env['lock'].close()
  return json.loads((self.r/('RECYCLE_CHECK_ONLY.json'if check else'RECYCLE_LATEST.json')).read_text())
 def stops(self):return[a for a in self.calls if a[:2]==['systemctl','stop']and a[2].endswith('.service')]
class SafetyTests(unittest.TestCase):
 def setUp(self):self.h=Harness();self.addCleanup(self.h.temp.cleanup)
 def test_below_limit_never_stops(self):
  self.assertEqual(self.h.execute()['status'],'NO_ACTION_ALL_BELOW1280MiB');self.assertEqual(self.h.stops(),[])
 def test_stale_chain_never_stops(self):
  self.h.high('n24');self.h.stale=True;self.assertEqual(self.h.execute()['status'],'ALERT_NO_FURTHER_AUTOMATIC_ACTION');self.assertEqual(self.h.stops(),[])
 def test_insufficient_votes_never_stops(self):
  self.h.high('n24');self.h.votes=17;self.assertEqual(self.h.execute()['status'],'ALERT_NO_FURTHER_AUTOMATIC_ACTION');self.assertEqual(self.h.stops(),[])
 def test_missing_node_blocks_second_outage(self):
  self.h.high('n24');self.h.pids['n01']='0';self.assertEqual(self.h.execute()['status'],'ALERT_NO_FURTHER_AUTOMATIC_ACTION');self.assertEqual(self.h.stops(),[])
 def test_existing_miss_streak_blocks_target(self):
  self.h.high('n24');self.h.missed.add('n24');self.assertEqual(self.h.execute()['status'],'NO_ACTION_WAIT_SAFE_DUTY_WINDOW');self.assertEqual(self.h.stops(),[])
 def test_current_production_turn_is_never_interrupted(self):
  epoch=datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc);slot=int((datetime.datetime.now(datetime.timezone.utc)-epoch).total_seconds()*2);self.h.high(self.h.active[(slot//12)%21]);self.assertEqual(self.h.execute()['status'],'NO_ACTION_WAIT_SAFE_DUTY_WINDOW');self.assertEqual(self.h.stops(),[])
 def test_check_only_never_changes_services(self):
  self.h.high('n24');self.assertEqual(self.h.execute(check=True)['status'],'CHECK_ONLY_WOULD_RECYCLE_ONE');self.assertEqual(self.h.calls,[])
 def test_one_standby_recycles_with_safeguard_then_cooldown(self):
  self.h.high('n24');self.h.high('n23');x=self.h.execute();self.assertEqual(x['status'],'PASS_ONE_GUARDED_RECYCLE');self.assertEqual(len(self.h.stops()),1)
  self.assertTrue(any(a[0]=='systemd-run'and'--on-active=60s'in a for a in self.h.calls));self.assertEqual(self.h.execute()['status'],'NO_ACTION_COOLDOWN');self.assertEqual(len(self.h.stops()),1)
 def test_failed_start_latches_and_keeps_safeguard(self):
  self.h.high('n24');self.h.fail_start=True;x=self.h.execute();self.assertEqual(x['status'],'ALERT_NO_FURTHER_AUTOMATIC_ACTION');self.assertTrue((self.h.r/'RECYCLE_BLOCKED.json').exists());self.assertEqual(len(self.h.stops()),1)
  self.assertFalse(any(a[:2]==['systemctl','stop']and a[2].endswith('.timer')for a in self.h.calls));self.assertEqual(self.h.execute()['status'],'ALERT_LATCHED_REQUIRES_MANUAL_RECONCILIATION')
 def test_unfinished_prior_attempt_latches_without_second_action(self):
  self.h.put(self.h.r/'RECYCLE_LAST_ATTEMPT.json',{'status':'ATTEMPT_STARTED','utc':datetime.datetime.now(datetime.timezone.utc).isoformat()});self.assertEqual(self.h.execute()['status'],'ALERT_UNRECONCILED_PREVIOUS_ATTEMPT');self.assertTrue((self.h.r/'RECYCLE_BLOCKED.json').exists());self.assertEqual(self.h.calls,[])
 def test_binary_change_blocks_recycle(self):
  self.h.high('n24');self.h.pin_bad=True;self.assertEqual(self.h.execute()['status'],'ALERT_NO_FURTHER_AUTOMATIC_ACTION');self.assertEqual(self.h.stops(),[])
if __name__=='__main__':unittest.main(verbosity=2)
