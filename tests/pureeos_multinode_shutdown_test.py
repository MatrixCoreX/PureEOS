"""Real disposable child shutdown and failure accounting; never chain acceptance."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from pureeos_multinode_local_test import stop_owned_children
from pureeos_single_choice_local_test import LocalTest


class OwnedShutdownTests(unittest.TestCase):
    def test_all_owned_children_get_graceful_signal_without_stopping_unrelated_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); children = []
            other = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
            code = '''from pathlib import Path
import signal,sys,time
root=Path(sys.argv[1]); name=sys.argv[2]
def done(_s,_f):
 (root/(name+'.signaled')).touch()
 deadline=time.monotonic()+10
 while len(list(root.glob('*.signaled')))<3:
  if time.monotonic()>deadline:sys.exit(9)
  time.sleep(.01)
 sys.exit(0)
signal.signal(signal.SIGTERM,done)
(root/(name+'.ready')).touch()
while True:time.sleep(.05)
'''
            try:
                children = [subprocess.Popen([sys.executable, '-c', code, str(root), str(i)]) for i in range(3)]
                deadline = time.monotonic()+10
                while len(list(root.glob('*.ready'))) < 3:
                    self.assertLess(time.monotonic(), deadline); time.sleep(.02)
                self.assertEqual(stop_owned_children([None, *children, children[0]], LocalTest.stop), [])
                self.assertTrue(all(p.returncode == 0 for p in children))
                self.assertIsNone(other.poll())
            finally:
                for p in [*children, other]:
                    if p.poll() is None: p.terminate()
                    try: p.wait(timeout=12)
                    except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=5)

    def test_nonzero_exit_remains_failure_and_other_children_are_reaped(self):
        children = [subprocess.Popen([sys.executable, '-c', 'raise SystemExit('+str(n)+')']) for n in (0, 7)]
        try:
            for p in children: p.wait(timeout=10)
            errors = stop_owned_children(children, LocalTest.stop)
            self.assertEqual([e['pid'] for e in errors], [children[1].pid])
            self.assertIn('7', errors[0]['error'])
            self.assertTrue(all(p.poll() is not None for p in children))
            self.assertEqual(stop_owned_children([], LocalTest.stop), [])
        finally:
            for p in children:
                if p.poll() is None: p.kill(); p.wait(timeout=5)


if __name__ == '__main__': unittest.main()
