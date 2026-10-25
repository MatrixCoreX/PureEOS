"""Candidate CI wiring and real subprocess controls, not native chain acceptance."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

import yaml


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('ci_native_checks', REPO / 'tools/ci_native_checks.py')
CI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CI)
WF_SPEC = importlib.util.spec_from_file_location('workflow_inventory', REPO / 'tools/workflow_inventory.py')
WF = importlib.util.module_from_spec(WF_SPEC)
WF_SPEC.loader.exec_module(WF)
VERSION = 'v2.0.0-' + 'a' * 40


class WorkflowDeliveryTests(unittest.TestCase):
    def test_active_workflows_have_only_resolvable_local_or_pinned_official_actions(self):
        workflows = REPO / '.github/workflows'
        current = WF.require_workflows(REPO, {'build.yaml', 'build_base.yaml'})
        for name, record in current.items():
            path = workflows / name
            text = record['content'].decode()
            document = yaml.load(text, Loader=yaml.BaseLoader)
            self.assertEqual(document['permissions'], {'contents': 'read'})
            self.assertEqual(set(re.findall(r'secrets\.([A-Za-z_][A-Za-z_0-9]*)', text)),
                             {'AIC_VM_DEPLOY_KEY'})
            for obsolete in ('PureEOSIO/', 'enf-x86-', 'pull_request_target', 'gh release', 'docker/login'):
                self.assertNotIn(obsolete, text)
            for job in document['jobs'].values():
                uses = job.get('uses')
                if uses:
                    self.assertTrue((REPO / uses.removeprefix('./')).is_file())
                else:
                    self.assertIn(job['runs-on'], ('ubuntu-24.04', 'ubuntu-26.04'))
                    self.assertLessEqual(int(job['timeout-minutes']), 300)
                for step in job.get('steps', []):
                    if 'uses' in step:
                        self.assertRegex(step['uses'], r'^actions/(checkout|upload-artifact)@[a-f0-9]{40}$')

    def test_both_modern_user_spaces_are_required_native_jobs(self):
        workflow = yaml.load((REPO / '.github/workflows/build.yaml').read_text(), Loader=yaml.BaseLoader)
        self.assertEqual(workflow['jobs']['native']['strategy']['matrix']['ubuntu'], ['26.04', '26.10'])
        self.assertEqual(workflow['jobs']['native']['uses'], './.github/workflows/build_base.yaml')
        base = yaml.load((REPO / '.github/workflows/build_base.yaml').read_text(), Loader=yaml.BaseLoader)
        self.assertIn('ubuntu-release', base['on']['workflow_call']['inputs'])
        native = base['jobs']['native']
        self.assertEqual(native['container'], 'ubuntu:${{ inputs.ubuntu-release }}')
        scripts = '\n'.join(step.get('run', '') for step in native['steps'])
        self.assertIn('parallel_state_view_test parallel_secondary_state_test', scripts)
        self.assertIn('test_incoming_transaction_batch test_production_timer_guard', scripts)
        self.assertIn('tools/ci_native_checks.py', scripts)

    def test_container_git_trust_is_limited_to_the_explicit_workspace(self):
        for filename in ('build.yaml', 'build_base.yaml'):
            text = (REPO / '.github/workflows' / filename).read_text()
            self.assertIn('git config --global --add safe.directory "$GITHUB_WORKSPACE"', text)
            self.assertNotIn('safe.directory "*"', text)

    def test_native_skip_requires_an_explicit_manual_controls_only_request(self):
        workflow = yaml.load((REPO / '.github/workflows/build.yaml').read_text(), Loader=yaml.BaseLoader)
        request = workflow['on']['workflow_dispatch']['inputs']['controls-only']
        self.assertEqual(request['type'], 'boolean')
        self.assertEqual(request['default'], 'false')
        self.assertEqual(workflow['jobs']['native']['if'],
                         "${{ github.event_name != 'workflow_dispatch' || !inputs.controls-only }}")

    def test_vm_secret_is_forwarded_explicitly_and_only_used_during_checkout(self):
        workflow = yaml.load((REPO / '.github/workflows/build.yaml').read_text(), Loader=yaml.BaseLoader)
        expected = {'AIC_VM_DEPLOY_KEY': '${{ secrets.AIC_VM_DEPLOY_KEY }}'}
        self.assertEqual(workflow['jobs']['native']['secrets'], expected)
        base = yaml.load((REPO / '.github/workflows/build_base.yaml').read_text(), Loader=yaml.BaseLoader)
        self.assertEqual(base['on']['workflow_call']['secrets']['AIC_VM_DEPLOY_KEY']['required'], 'false')
        for job in (workflow['jobs']['controls'], base['jobs']['native']):
            uses = [step for step in job['steps'] if 'uses' in step and 'actions/checkout@' in step['uses']]
            self.assertEqual(len(uses), 1)
            self.assertEqual(uses[0]['with'], {'submodules': 'false', 'persist-credentials': 'false'})
            keyed = [step for step in job['steps'] if 'AIC_VM_DEPLOY_KEY' in step.get('env', {})]
            self.assertEqual(len(keyed), 1)
            self.assertEqual(keyed[0]['env'], expected)
            self.assertEqual(keyed[0]['run'], 'python3 -I -B tools/checkout_ci_dependencies.py')

    def test_all_previous_workflow_bytes_are_preserved_outside_active_discovery(self):
        archive = REPO / 'docs/parallel-execution/ci-reference-20261024'
        manifest = json.loads((archive / 'manifest.json').read_text())
        self.assertEqual(len(manifest['files']), 10)
        for old_name, record in manifest['files'].items():
            self.assertEqual(record['archive'], old_name + '.txt')
            self.assertEqual(hashlib.sha256((archive / record['archive']).read_bytes()).hexdigest(), record['sha256'])


class NativeEvidenceValidationTests(unittest.TestCase):
    def test_wrong_or_duplicated_inventory_and_other_executable_are_rejected(self):
        build = Path('/isolated-build')
        correct = {'tests': [{'name': name, 'command': [str(build / path)]} for name, path in CI.TESTS.items()]}
        CI.validate_inventory(correct, build)
        for tests in (correct['tests'][:-1], correct['tests'] + correct['tests'][:1],
                      [{'name': name, 'command': ['/usr/bin/true']} for name in CI.TESTS]):
            with self.subTest(tests=tests), self.assertRaises(ValueError):
                CI.validate_inventory({'tests': tests}, build)

    def test_skipped_failed_missing_and_duplicated_junit_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix='pureeos-ci-junit-') as temporary:
            path = Path(temporary) / 'junit.xml'
            for problem in ('skipped', 'failure', 'error', 'missing', 'duplicate', 'disabled'):
                root = ET.Element('testsuite')
                names = list(CI.TESTS)
                if problem == 'missing': names.pop()
                if problem == 'duplicate': names[-1] = names[0]
                for index, name in enumerate(names):
                    case = ET.SubElement(root, 'testcase', name=name, status='run')
                    if index == 0 and problem in ('skipped', 'failure', 'error'):
                        ET.SubElement(case, problem)
                    if index == 0 and problem == 'disabled':
                        case.set('status', 'disabled')
                ET.ElementTree(root).write(path)
                with self.subTest(problem=problem), self.assertRaises(ValueError):
                    CI.validate_junit(path)

    def test_version_must_bind_to_the_expected_source(self):
        for text in (VERSION + '\n', VERSION + '-dirty\n'):
            CI.validate_version(text, VERSION)
        for text in ('unknown', 'v2.0.0', 'prefix ' + VERSION, VERSION + 'modified'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                CI.validate_version(text, VERSION)


class NativeEvidenceProcessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for name in ('c++', 'ctest'):
            if shutil.which(name) is None:
                raise RuntimeError('CI subprocess controls require ' + name)
        cls.temporary = tempfile.TemporaryDirectory(prefix='pureeos-ci-process-controls-')
        cls.addClassCleanup(cls.temporary.cleanup)
        root = Path(cls.temporary.name)
        source = root / 'control.cpp'
        source.write_text('#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n#include <fstream>\n'
                          'int main(int argc,char** argv){'
                          'if(const char* p=std::getenv("PUREEOS_CI_CONTROL_MUTATE")){std::ofstream(p,std::ios::app)<<"x";}'
                          'if(argc>1){if(std::strcmp(argv[1],"--help")==0)std::puts("Usage: control tool");'
                          'else std::puts("' + VERSION + '");}return 0;}\n')
        cls.elf = root / 'control'
        subprocess.run(['c++', str(source), '-o', str(cls.elf)], check=True, capture_output=True, timeout=60)

    def execute(self, *, failed=False, skipped=False, missing=False, mutate=False):
        with tempfile.TemporaryDirectory(prefix='pureeos-ci-native-check-') as temporary:
            root = Path(temporary);build = root / 'build';build.mkdir()
            for relative in (*CI.TESTS.values(), *(record[0] for record in CI.TOOLS.values())):
                target = build / relative;target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(self.elf, target)
            lines = []
            for index, (name, relative) in enumerate(CI.TESTS.items()):
                if missing and index == 0: continue
                if failed and index == 0: shutil.copy2(shutil.which('false'), build / relative)
                lines.append('add_test(' + name + ' "' + str(build / relative) + '")')
                if skipped and index == 0:
                    lines.append('set_tests_properties(' + name + ' PROPERTIES DISABLED TRUE)')
            (build / 'CTestTestfile.cmake').write_text('\n'.join(lines) + '\n')
            environment = dict(os.environ)
            if mutate:
                environment['PUREEOS_CI_CONTROL_MUTATE'] = str(build / CI.TOOLS['helios-util'][0])
            result = subprocess.run(['python3', '-I', '-B', str(REPO / 'tools/ci_native_checks.py'),
                '--build-dir', str(build), '--output', str(root / 'output'), '--expected-version', VERSION],
                text=True, capture_output=True, env=environment, timeout=60)
            report = json.loads((root / 'output/report.json').read_text())
            self.assertFalse(report['release_ready'])
            return result, report

    def test_real_process_controls_close_success_after_all_four_tests(self):
        result, report = self.execute()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(report['status'], 'passed')
        self.assertEqual(set(report['passed_tests']), set(CI.TESTS))
        self.assertTrue(report['inputs_unchanged'])

    def test_real_ctest_failure_cannot_pass(self):
        result, report = self.execute(failed=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['jobs'][-1]['exit_code'], 8)

    def test_real_ctest_disabled_test_cannot_pass_even_when_ctest_exits_zero(self):
        result, report = self.execute(skipped=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report['jobs'][-1]['exit_code'], 0)
        self.assertIn('Skipped', report['error'])

    def test_real_ctest_missing_test_cannot_pass(self):
        result, report = self.execute(missing=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report['jobs'][-1]['name'], 'ctest-inventory')
        self.assertIn('inventory', report['error'])

    def test_real_executable_change_cannot_pass(self):
        result, report = self.execute(mutate=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('changed during checks', report['error'])
        self.assertFalse(report['inputs_unchanged'])


if __name__ == '__main__':
    unittest.main()
