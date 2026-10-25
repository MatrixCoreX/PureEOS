"""Prove pure acceptance helpers load without historical private/cloud state."""
import json
from pathlib import Path
import subprocess
import sys
import unittest


class OfflinePortability(unittest.TestCase):
    def test_isolated_import_cannot_load_cloud_helper_or_start_process_or_socket(self):
        tests = Path(__file__).resolve().parent
        code = '''import contextlib,importlib.abc,io,json,pathlib,socket,subprocess,sys
sys.path.insert(0,TESTS)
class DenyCloud(importlib.abc.MetaPathFinder):
 def find_spec(self,fullname,path=None,target=None):
  if fullname == 'cloud_round': raise RuntimeError('Historical cloud helper forbidden')
sys.meta_path.insert(0,DenyCloud())
def forbidden(*args,**kwargs):raise RuntimeError('Offline import must not start subprocesses or sockets')
class DenySocket(socket.socket):
 def __new__(cls,*args,**kwargs):return forbidden()
subprocess.Popen=forbidden;socket.socket=DenySocket
import v1_candidate_deployment_test as deploy
import v1_candidate_native_matrix as native
import v1_candidate_recovery_test as recovery
import archive_v1_candidate_sources as archive
import audit_v1_native_shards as shards
assert deploy.bounded_runtime(2700)==1500
assert len(native.selected_suites())==10
assert not recovery.complete({})
assert callable(archive.verify_source_archive) and callable(shards.check_details)
for module in (deploy,native,recovery,archive):
 sys.argv=[module.__name__,'--help']
 with contextlib.redirect_stdout(io.StringIO()) as output:
  try:module.main()
  except SystemExit as e:assert e.code == 0
  else:raise AssertionError('--help must exit without cloud work')
 assert 'usage:' in output.getvalue()
assert 'cloud_round' not in sys.modules
assert not any('unified-cdt5-20261021-TBMyRV' in p for p in sys.path)
try:__import__('cloud_round')
except RuntimeError:pass
else:raise AssertionError('Negative import control failed')
print(json.dumps(dict(status='passed',imports=5,cloud_transport='forbidden',scope='Isolated pure helper imports only')))
'''.replace('TESTS', repr(str(tests)))
        result = subprocess.run([sys.executable, '-I', '-B', '-c', code],
            text=True, capture_output=True, check=False, timeout=30, cwd='/')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['imports'], 5)


if __name__ == '__main__': unittest.main()
