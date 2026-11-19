import unittest,copy
from stability_policy import *
class PolicyTest(unittest.TestCase):
 def fixture(self):
  return {'disk_free':40*GIB,'mem_available':2*GIB,'head_age':1,'lib_age':2,'policy_count':21,'threshold':15,'recent':list('abcdefghijklmnopqrstu'),'all_running':True,'elapsed':1000,'swap_free':40*GIB,'nodes':{'a':{'banned':False,'safety_exists':True,'bytes':900*MIB,'strikes':0,'distance':100,'standby':False},'b':{'banned':False,'safety_exists':True,'bytes':850*MIB,'strikes':1,'distance':10000,'standby':True}}}
 def test_prefer_standby_then_largest(self):self.assertEqual(select(self.fixture()),('b','QUALIFIED_ONE'))
 def test_disk_reserve(self):s=self.fixture();s['disk_free']=19*GIB;self.assertIsNone(select(s)[0])
 def test_restart_memory_headroom(self):
  for k,v in [('mem_available',511*MIB),('swap_free',GIB)]:s=self.fixture();s[k]=v;self.assertIsNone(select(s)[0])
 def test_never_selected_standby_without_safety_is_skipped(self):s=self.fixture();s['nodes']['b']['safety_exists']=False;self.assertEqual(select(s)[0],'a');s['nodes']['a']['safety_exists']=False;self.assertIsNone(select(s)[0])
 def test_stale_head(self):s=self.fixture();s['head_age']=6;self.assertIsNone(select(s)[0])
 def test_stale_lib(self):s=self.fixture();s['lib_age']=21;self.assertIsNone(select(s)[0])
 def test_native_policy_not_weakened(self):
  for field,value in [('policy_count',19),('threshold',13)]:s=self.fixture();s[field]=value;self.assertIsNone(select(s)[0])
 def test_missing_node(self):s=self.fixture();s['all_running']=False;self.assertIsNone(select(s)[0])
 def test_weak_quorum(self):s=self.fixture();s['recent']=list('abcdefghijklmnopqr');self.assertIsNone(select(s)[0])
 def test_cooldown(self):s=self.fixture();s['elapsed']=179;self.assertIsNone(select(s)[0])
 def test_banned_never_restart(self):s=self.fixture();s['nodes']['b']['banned']=True;self.assertEqual(select(s)[0],'a')
 def test_near_duty_never_restart(self):s=self.fixture();s['nodes']['a']['distance']=89;s['nodes']['b']['distance']=89;self.assertIsNone(select(s)[0])
 def test_historical_strikes_not_permanent_exclusion(self):s=self.fixture();s['nodes']['b']['strikes']=1;self.assertEqual(select(s)[0],'b')
 def test_two_strikes_require_longer_margin(self):s=self.fixture();s['nodes']['b'].update(strikes=2,distance=109);self.assertEqual(select(s)[0],'a');s['nodes']['b']['distance']=110;self.assertEqual(select(s)[0],'b')
 def test_memory_threshold_and_global_headroom(self):s=self.fixture();s['nodes']['a']['bytes']=600*MIB;s['nodes']['b']['bytes']=600*MIB;self.assertIsNone(select(s)[0]);s['swap_free']=11*GIB;self.assertEqual(select(s)[0],'b')
 def test_shared_founder_is_not_automatically_recycled(self):
  s=self.fixture();s['nodes']={'gptgenesisbp':dict(s['nodes']['b'])};s['nodes']['gptgenesisbp']['bytes']=2000*MIB;self.assertEqual(select(s),(None,'SHARED_SOURCE_REQUIRES_REVIEW'))
 def test_slot_distance_and_pending_duty(self):self.assertEqual(distance('b',['a','b'],0,{'a','b'}),6);self.assertEqual(distance('x',['a','b'],0,{'x'}),0);self.assertEqual(distance('z',['a','b'],0,{'a','b'}),10000)
 def test_reconcile_requires_positive_identity_and_canonical_proof(self):
  s={'new_pid':'222','old_pid':'111','other_pids_unchanged':True,'anchor_matches':True,'target_fresh':True,'other_recent':18,'safety_exists':True,'config_unchanged':True,'banned':False};self.assertTrue(reconciled(s))
  for key in ['other_pids_unchanged','anchor_matches','target_fresh','safety_exists','config_unchanged']:
   q=copy.deepcopy(s);q[key]=False;self.assertFalse(reconciled(q),key)
  for key,value in [('new_pid','0'),('new_pid','111'),('other_recent',17),('banned',True)]:q=copy.deepcopy(s);q[key]=value;self.assertFalse(reconciled(q),key)
if __name__=='__main__':unittest.main()
