"""FC console syscall regression; affects only the fixture process, never a real TTY."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]


class ConsoleEchoRegression(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux syscall-wrap fixture')
    def test_pipe_terminal_failure_and_echo_bits(self):
        compiler = shutil.which('c++')
        if compiler is None: self.skipTest('C++ compiler is required')
        with tempfile.TemporaryDirectory(prefix='pureeos-console-regression-') as directory:
            binary = Path(directory) / 'console-echo-test'
            built = subprocess.run([compiler, '-std=c++20', '-O1', '-g1',
                str(REPO / 'tests/fixtures/console_echo_regression.cpp'),
                str(REPO / 'libraries/libfc/src/io/console.cpp'),
                '-Wl,--wrap=isatty', '-Wl,--wrap=tcgetattr', '-Wl,--wrap=tcsetattr',
                '-o', str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout)
            tested = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stdout)
            self.assertIn('cases=8 passed=8', tested.stdout)


if __name__ == '__main__':
    unittest.main()
