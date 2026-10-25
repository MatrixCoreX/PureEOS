"""Check actual fixture wrapper registration guards; no VM/core correctness claim."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class FixtureRegistration(unittest.TestCase):
    def test_blacklist_cases_require_all_contract_fixtures(self):
        wrapper=Path(__file__).with_name('native_policy_boundary_fixture.cpp')
        compiler=shutil.which('c++')
        self.assertIsNotNone(compiler)
        flags=('PUREEOS_ROTATION_CONTRACTS_DIR','PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR',
            'PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR','PUREEOS_GOVERNANCE_CONTRACTS_DIR')
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'tests').mkdir();(root/'unittests').mkdir()
            shutil.copyfile(wrapper,root/'tests/fixture.cpp')
            (root/'unittests/pureeos_governance_tests.cpp').write_text('int base_fixture_registered;\n')
            (root/'unittests/pureeos_blacklist_current_policy_tests.inc').write_text('int blacklist_fixture_registered;\n')
            for mask in range(16):
                args=[compiler,'-E','-P',str(root/'tests/fixture.cpp')]
                args += ['-D'+name for i,name in enumerate(flags) if mask&(1<<i)]
                result=subprocess.run(args,capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertIn('int base_fixture_registered;',result.stdout)
                self.assertEqual('int blacklist_fixture_registered;' in result.stdout,mask==15)


if __name__=='__main__':unittest.main()
