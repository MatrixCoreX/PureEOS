"""Remote-only read-helper regressions: incomplete pages never duplicate or truncate."""
import common as c
import tempfile,pathlib,json,unittest

class Pagination(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.original=(c.R,c.rpc,c.time.sleep,c.time.monotonic);c.R=pathlib.Path(self.temp.name);self.calls=[];self.clock=0
  c.time.sleep=lambda seconds:None
  def tick():self.clock+=1;return self.clock
  c.time.monotonic=tick
 def tearDown(self):
  c.R,c.rpc,c.time.sleep,c.time.monotonic=self.original;self.temp.cleanup()
 def run_pages(self,pages):
  it=iter(pages)
  def query(method,args):self.calls.append(args['lower_bound']);return next(it)
  c.rpc=query;return c.table('bpmaint')
 def test_complete_singleton(self):
  self.assertEqual(self.run_pages([{'rows':[{'id':1}],'more':False,'next_key':''}]),[{'id':1}])
 def test_complete_empty(self):self.assertEqual(self.run_pages([{'rows':[],'more':False}]),[])
 def test_nonempty_missing_continuation_retried_without_duplicates(self):
  result=self.run_pages([{'rows':[{'id':1}],'more':True,'next_key':''},{'rows':[{'id':1}],'more':False}]);self.assertEqual(result,[{'id':1}]);self.assertEqual(self.calls,['',''])
 def test_empty_missing_continuation_retried(self):
  self.assertEqual(self.run_pages([{'rows':[],'more':True,'next_key':''},{'rows':[{'id':1}],'more':False}]),[{'id':1}])
 def test_retry_mid_page_preserves_previous_page(self):
  result=self.run_pages([{'rows':[{'id':1}],'more':True,'next_key':'2'},{'rows':[{'id':2}],'more':True,'next_key':'2'},{'rows':[{'id':2},{'id':3}],'more':False}]);self.assertEqual(result,[{'id':1},{'id':2},{'id':3}]);self.assertEqual(self.calls,['','2','2'])
 def test_valid_multiple_pages(self):
  self.assertEqual(self.run_pages([{'rows':[1],'more':True,'next_key':'2'},{'rows':[2],'more':True,'next_key':'3'},{'rows':[3],'more':False}]),[1,2,3])
 def test_deadline_rejects_partial_result(self):
  def query(method,args):return {'rows':[1],'more':True,'next_key':''}
  c.rpc=query
  with self.assertRaisesRegex(RuntimeError,'Table pagination deadline'):c.table('bpmaint')

if __name__=='__main__':unittest.main(verbosity=2)
