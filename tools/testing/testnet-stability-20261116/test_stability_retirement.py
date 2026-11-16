import unittest,tempfile,json,hashlib
from pathlib import Path
from types import SimpleNamespace
from retire_banned_runtime import retire_confirmed
class RetirementTest(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();root=Path(self.tmp.name);self.cfg=root/'a/config';self.data=root/'a/data';self.cfg.mkdir(parents=True);(self.data/'blocks').mkdir(parents=True);(self.data/'blocks/example').write_bytes(b'fixtureblock');(self.data/'finalizers').mkdir();(self.data/'finalizers/safety.dat').write_bytes(b'fixturesafety');(self.cfg/'config.ini').write_text('producer-name = a\nsignature-provider = fixture\n');self.case=root/'case';self.case.mkdir();self.ops=[];self.row={'banned':True,'zero_windows':3,'ban_final_block':10};self.nodes={'a':{'unit':'fixture.service','config_dir':str(self.cfg),'data_dir':str(self.data),'config_sha256':self.sha(self.cfg/'config.ini')}}
  def rpc(n,d=None,port=8888):return {'last_irreversible_block_num':20}if n=='get_info'else{'id':'fixturecanonical'}
  self.g=SimpleNamespace(R=self.case,rows=lambda:{'a':self.row},rpc=rpc,sha=self.sha,state=lambda u:{'MainPID':'0'},run=lambda a,*t:self.ops.append(a),now=lambda:'fixturetime',save=self.save)
 def sha(self,p):return hashlib.sha256(p.read_bytes()).hexdigest()
 def save(self,n,d):p=self.case/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(d))
 def tearDown(self):self.tmp.cleanup()
 def test_confirmed_ban_deletes_only_own_blocks_config_and_preserves_safety_keys(self):
  self.assertEqual(retire_confirmed(self.g,self.nodes),['a']);self.assertFalse((self.data/'blocks').exists());self.assertFalse((self.cfg/'config.ini').exists());self.assertEqual((self.data/'finalizers/safety.dat').read_bytes(),b'fixturesafety');self.assertTrue((self.case/'private/automatic-retirement/a/credentials.json').exists());self.assertEqual(retire_confirmed(self.g,self.nodes),[])
 def test_unconfirmed_ban_no_mutation(self):self.row['ban_final_block']=21;self.assertRaises(AssertionError,retire_confirmed,self.g,self.nodes);self.assertEqual(self.ops,[]);self.assertTrue((self.cfg/'config.ini').exists())
 def test_different_canonical_sources_no_mutation(self):
  self.g.rpc=lambda n,d=None,port=8888:{'last_irreversible_block_num':20}if n=='get_info'else{'id':str(port)};self.assertRaises(AssertionError,retire_confirmed,self.g,self.nodes);self.assertEqual(self.ops,[])
 def test_changed_configuration_no_mutation(self):
  (self.cfg/'config.ini').write_text('producer-name = b');self.assertRaises(AssertionError,retire_confirmed,self.g,self.nodes);self.assertEqual(self.ops,[])
 def test_nonbanned_no_mutation(self):self.row['banned']=False;self.assertEqual(retire_confirmed(self.g,self.nodes),[]);self.assertEqual(self.ops,[])
 def test_open_file_blocks_deletion(self):
  with(self.data/'blocks/example').open('rb')as live:self.assertRaises(AssertionError,retire_confirmed,self.g,self.nodes);self.assertTrue((self.data/'blocks/example').exists())
 def test_interrupted_after_delete_reconciles_without_repeating_stop(self):
  retire_confirmed(self.g,self.nodes);(self.case/'auto-retired/a.json').unlink();n=len(self.ops);self.assertEqual(retire_confirmed(self.g,self.nodes),['a']);self.assertEqual(n,len(self.ops))
if __name__=='__main__':unittest.main()
