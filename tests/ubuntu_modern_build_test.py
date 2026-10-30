"""Read-only profile/guard tests; actual Ubuntu compatibility checks are recorded separately."""
from pathlib import Path
import os
import platform
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'tools/ubuntu-modern-build.sh'


class UbuntuProfileTests(unittest.TestCase):
    def run_profile(self, *arguments, env=None):
        return subprocess.run(['bash', str(SCRIPT), *arguments], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20, env=env)

    def require_native_prerequisites(self):
        data = platform.freedesktop_os_release()
        if data.get('ID') != 'ubuntu' or data.get('VERSION_ID') not in ('24.04', '26.04', '26.10'):
            self.skipTest('Actual supported Ubuntu host required')
        if not all(shutil.which(tool) for tool in ('cmake', 'c++', 'git', 'python3')):
            self.skipTest('Build tools not installed')

    def test_help(self):
        result = self.run_profile('--help')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('not full release acceptance', result.stdout)

    def test_build_checks_exact_vm_patch_before_cmake(self):
        script = SCRIPT.read_text()
        check = 'python3 -I -B "$ubuntu_repo/tools/prepare_vm_vendor.py" --check'
        self.assertIn(check, script)
        self.assertLess(script.index(check), script.index('cmake -S'))
        self.assertNotIn('prepare_vm_vendor.py" --apply', script)

    def test_operator_build_includes_wallet_and_native_finalizer_key_tool(self):
        script=SCRIPT.read_text()
        self.assertIn('--target pureeos-ubuntu-nodeos cleos keosd helios-util --parallel "$ubuntu_jobs"',script)
        result=self.run_profile('--help')
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertIn('nodeos, cleos, keosd and helios-util',result.stdout)

    def test_ci_prepares_the_pinned_vm_patch_before_the_native_build(self):
        base = (REPO / '.github/workflows/build_base.yaml').read_text()
        prepare = 'python3 -I -B tools/prepare_vm_vendor.py --apply'
        self.assertLess(base.index(prepare), base.index('ubuntu-modern-build.sh --build'))
        workflow = (REPO / '.github/workflows/build.yaml').read_text()
        self.assertLess(workflow.index(prepare), workflow.index('run_v1_candidate_controls.py'))
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('persist-credentials: false', base)

    def test_invalid_arguments_do_not_build(self):
        for argument in ('--unknown', '--jobs=0', '--jobs=-1', '--jobs=129', '--jobs=2x', '--jobs=99999'):
            with self.subTest(argument=argument):
                self.assertEqual(self.run_profile(argument).returncode, 2)

    def test_actions_are_mutually_exclusive(self):
        result = self.run_profile('--check', '--build')
        self.assertEqual(result.returncode, 2)
        self.assertIn('exactly one action', result.stdout)

    def test_actual_host_preflight(self):
        self.require_native_prerequisites()
        result = self.run_profile('--check')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('ENABLE_OC=OFF', result.stdout)
        self.assertIn('Generator:', result.stdout)

    def test_broad_roots_are_rejected(self):
        self.require_native_prerequisites()
        for directory in ('/', str(REPO), str(REPO / 'build'), str(Path.home())):
            with self.subTest(directory=directory):
                result = self.run_profile('--configure', '--build-dir=' + directory)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn('isolated build', result.stdout)

    def test_foreign_build_cache_is_not_overwritten(self):
        self.require_native_prerequisites()
        with tempfile.TemporaryDirectory(prefix='pureeos-ubuntu-guard-') as temporary:
            cache = Path(temporary) / 'CMakeCache.txt'
            original = 'NODE_EXECUTABLE_NAME:STRING=nodeos\n'
            cache.write_text(original)
            result = self.run_profile('--configure', '--build-dir=' + temporary)
            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertIn('Refusing', result.stdout)
            self.assertEqual(cache.read_text(), original)

    def test_account_home_is_rejected_without_home_environment(self):
        self.require_native_prerequisites()
        import pwd
        account_home = pwd.getpwuid(os.getuid()).pw_dir
        environment = {key: value for key, value in os.environ.items() if key != 'HOME'}
        result = self.run_profile('--configure', '--build-dir=' + account_home, env=environment)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn('isolated build', result.stdout)

    def test_home_symlink_alias_is_also_rejected(self):
        self.require_native_prerequisites()
        with tempfile.TemporaryDirectory(prefix='pureeos-ubuntu-home-guard-') as temporary:
            alias = Path(temporary) / 'home-alias'
            alias.symlink_to(Path.home(), target_is_directory=True)
            result = self.run_profile('--configure', '--build-dir=' + str(alias))
            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertIn('isolated build', result.stdout)

    def test_other_checkout_with_same_node_name_is_not_overwritten(self):
        self.require_native_prerequisites()
        with tempfile.TemporaryDirectory(prefix='pureeos-ubuntu-guard-') as temporary:
            cache = Path(temporary) / 'CMakeCache.txt'
            original = 'NODE_EXECUTABLE_NAME:STRING=pureeos-ubuntu-nodeos\nCMAKE_HOME_DIRECTORY:INTERNAL=/another/project\n'
            cache.write_text(original)
            result = self.run_profile('--configure', '--build-dir=' + temporary)
            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertIn('different source tree', result.stdout)
            self.assertEqual(cache.read_text(), original)

    def test_modern_ci_platforms_do_not_install_legacy_oc_llvm(self):
        for release in ('24.04', '26.04', '26.10'):
            text = (REPO / '.cicd/platforms' / ('ubuntu' + release + '.Dockerfile')).read_text()
            self.assertIn('FROM ubuntu:' + release, text)
            self.assertIn('HELIOS_MODERN_JIT_PROFILE=1', text)
            self.assertNotIn('llvm-11-dev', text)
        workflow = (REPO / '.github/workflows/build_base.yaml').read_text()
        self.assertIn('bash tools/ubuntu-modern-build.sh --build', workflow)
        self.assertIn('-DENABLE_OC=OFF', SCRIPT.read_text())

    def test_modern_platforms_have_bounded_native_component_ci(self):
        workflow = (REPO / '.github/workflows/build.yaml').read_text()
        self.assertIn('ubuntu: ["24.04", "26.04", "26.10"]', workflow)
        base = (REPO / '.github/workflows/build_base.yaml').read_text()
        self.assertIn('runs-on: ubuntu-24.04', base)
        self.assertIn('container: ubuntu:${{ inputs.ubuntu-release }}', base)
        self.assertIn('test_incoming_transaction_batch test_production_timer_guard', base)
        self.assertIn('tools/ci_native_checks.py', base)


if __name__ == '__main__':
    unittest.main()
