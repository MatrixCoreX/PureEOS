import unittest,tempfile,hashlib,json
from pathlib import Path
from unittest.mock import patch
import stability_guard as g
class LifecycleTest(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);self.oldR=g.R;g.R=self.root;cfg=self.root/'config';data=self.root/'data';cfg.mkdir();(data/'finalizers').mkdir(parents=True);(cfg/'config.ini').write_text('synthetic config');(data/'state').mkdir();(data/'state/chain_head.dat').write_bytes(b'fixture-state');(data/'finalizers/safety.dat').write_bytes(b'synthetic-safety-not-a-live-record');self.v={'unit':'synthetic.service','config_dir':str(cfg),'data_dir':str(data),'rpc_port':18888,'config_sha256':g.sha(cfg/'config.ini')};self.nodes={'a':self.v};self.ops=[];self.arm_error=False;self.pid='111';self.proof={'new_pid':'222','old_pid':'111','other_pids_unchanged':True,'anchor_matches':True,'target_fresh':True,'other_recent':20,'safety_exists':True,'config_unchanged':True,'banned':False}
  self.ctx=({'nodes':{'a':{'bytes':900*1024**2,'distance':120}}},{'last_irreversible_block_num':10,'last_irreversible_block_id':'fixtureanchor'},{'a':'111','b':'333'},{'a':{'banned':False},'b':{'banned':False}},{'SwapFree':40*1024**3})
 def tearDown(self):g.R=self.oldR;self.temp.cleanup()
 def fake_run(self,args,timeout=15):
  self.ops.append(args)
  if args[0]=='systemd-run'and self.arm_error:raise OSError('synthetic watchdog arm failure')
  if args[:2]==['systemctl','stop']and args[2]=='synthetic.service':self.pid='0'
  if args[:2]==['systemctl','start']:self.pid='222'
 def invoke(self,ban=False,post_error=False):
  def proof(e,nodes):
   if post_error:raise OSError('synthetic transient post-start read failure')
   return True,self.proof
  ticks=iter([0]+list(range(0,200,20)))
  with patch.object(g,'run',side_effect=self.fake_run),patch.object(g,'state',side_effect=lambda u:{'MainPID':self.pid}),patch.object(g,'rpc',return_value={'chain_id':g.CHAIN}),patch.object(g,'rows',return_value={'a':{'banned':ban}}),patch.object(g,'assert_native_binding'),patch.object(g,'positive_reconciliation',side_effect=proof),patch.object(g.time,'sleep'),patch.object(g.time,'monotonic',side_effect=lambda:next(ticks)):
   return g.perform('a',self.nodes,self.ctx)
 def test_one_stop_start_preserves_safety_and_records_proof(self):
  r=self.invoke();self.assertEqual(r['status'],'PASS_ONE_GUARDED_RECYCLE');self.assertEqual(sum(a[:2]==['systemctl','stop']and a[2]=='synthetic.service'for a in self.ops),1);self.assertEqual(sum(a[:2]==['systemctl','start']for a in self.ops),1);self.assertEqual((Path(self.v['data_dir'])/'finalizers/safety.dat').read_bytes(),b'synthetic-safety-not-a-live-record');self.assertTrue((self.root/r['event']/'RESULT.json').exists())
 def test_transient_after_start_keeps_event_for_readonly_reconciliation(self):
  r=self.invoke(post_error=True);self.assertEqual(r['status'],'NEEDS_POSITIVE_RECONCILIATION_NO_SECOND_STOP');self.assertEqual(sum(a[:2]==['systemctl','start']for a in self.ops),1);self.assertEqual(r['phase'],'START_REQUESTED');self.assertIn('synthetic transient',r['error']);self.assertTrue((self.root/'GUARD_INFLIGHT.json').exists())
 def test_new_ban_after_stop_prevents_any_restart(self):
  r=self.invoke(ban=True);self.assertEqual(r['status'],'NEEDS_POSITIVE_RECONCILIATION_NO_SECOND_STOP');self.assertFalse(any(a[:2]==['systemctl','start']for a in self.ops));self.assertIn('Target became banned',r['error'])
 def test_arm_failure_does_not_latch_or_request_native_stop(self):
  self.arm_error=True;r=self.invoke();self.assertEqual(r['status'],'ABORTED_BEFORE_NATIVE_STOP_RETRY_AFTER_COOLDOWN');self.assertFalse(any(a[:2]==['systemctl','stop']and a[2]=='synthetic.service'for a in self.ops));self.assertTrue((self.root/'GUARD_LAST.json').exists())
 def test_missing_safety_is_never_fabricated_or_restarted(self):
  (Path(self.v['data_dir'])/'finalizers/safety.dat').unlink()
  with self.assertRaises(AssertionError):self.invoke()
  self.assertEqual(self.ops,[])
 def test_config_change_rejected_before_watchdog_or_stop(self):
  (Path(self.v['config_dir'])/'config.ini').write_text('changed config')
  with self.assertRaises(AssertionError):self.invoke()
  self.assertEqual(self.ops,[])
 def test_founder_post_stop_ban_check_uses_independent_reader(self):
  self.nodes={'gptgenesisbp':self.v};ctx=({'nodes':{'gptgenesisbp':{'bytes':900*1024**2,'distance':120}}},self.ctx[1],{'gptgenesisbp':'111','b':'333'},{'gptgenesisbp':{'banned':False},'b':{'banned':False}},self.ctx[4]);ticks=iter([0,0,20,40,60])
  with patch.object(g,'run',side_effect=self.fake_run),patch.object(g,'state',side_effect=lambda u:{'MainPID':self.pid}),patch.object(g,'rpc',return_value={'chain_id':g.CHAIN}),patch.object(g,'rows',return_value={'gptgenesisbp':{'banned':False}})as rows,patch.object(g,'assert_native_binding'),patch.object(g,'positive_reconciliation',return_value=(True,self.proof)),patch.object(g.time,'sleep'),patch.object(g.time,'monotonic',side_effect=lambda:next(ticks)):
   r=g.perform('gptgenesisbp',self.nodes,ctx)
  self.assertEqual(r['status'],'PASS_ONE_GUARDED_RECYCLE');rows.assert_called_once_with(port=8890)
 def test_restore_watchdog_uses_fresh_reader_for_founder(self):
  native=self.root/'native';native.write_bytes(b'fixture-native');original_sha=g.sha;nodes={'gptgenesisbp':self.v,**{'fixture'+str(n):self.v for n in range(24)}}
  def load(n):
   return nodes if n=='GUARD_NODE_MAP.json'else {'status':'PASS_6_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS'}
  info={'chain_id':g.CHAIN,'head_block_time':g.now(),'last_irreversible_block_time':g.now()}
  with patch.object(g,'NATIVE',native),patch.object(g,'sha',side_effect=lambda p:g.PIN if p==native else original_sha(p)),patch.object(g,'load',side_effect=load),patch.object(g.sys,'argv',['guard','--restore','gptgenesisbp']),patch.object(g,'rpc',return_value=info)as rpc,patch.object(g,'rows',return_value={'gptgenesisbp':{'banned':False}})as rows,patch.object(g,'run')as run:
   g.main()
  rpc.assert_called_once_with('get_info',port=8890);rows.assert_called_once_with(port=8890);run.assert_called_once_with(['systemctl','start','synthetic.service'],15)
if __name__=='__main__':unittest.main()
