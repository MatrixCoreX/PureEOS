"""Real Git checkout controls and deployment-key lifetime/error controls."""
from pathlib import Path
import importlib.util
import base64
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("checkout_ci_dependencies", ROOT / "tools/checkout_ci_dependencies.py")
CI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CI)


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True,
                                   stderr=subprocess.PIPE, timeout=30).strip()


class CheckoutTests(unittest.TestCase):
    def test_diagnostics_report_only_fixed_categories_without_git_stderr(self):
        for stderr, category in (('fatal: dubious ownership SECRET_FIXTURE', 'unsafe-worktree-owner'),
                                 ('Permission denied (publickey) SECRET_FIXTURE', 'ssh-read-permission'),
                                 ('SECRET_FIXTURE unknown error', 'unclassified-git-error')):
            with self.subTest(category=category), mock.patch.object(CI.subprocess, 'run',
                    side_effect=subprocess.CalledProcessError(128, ['git'], stderr=stderr)):
                with self.assertRaises(CI.DependencyGitError) as failure:
                    CI.git(ROOT, 'submodule', 'init')
                self.assertEqual(str(failure.exception), 'Dependency checkout failed: ' + category)
                self.assertNotIn('SECRET_FIXTURE', str(failure.exception))

    def test_real_container_ownership_mismatch_needs_exact_root_trust(self):
        with tempfile.TemporaryDirectory(prefix='aic-ci-owner-') as temporary:
            parent = Path(temporary)
            project, expected = self.fixture(parent)
            global_config = parent / 'isolated-global-config'
            env = dict(os.environ, GIT_TEST_ASSUME_DIFFERENT_OWNER='1',
                       GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=str(global_config))
            with self.assertRaisesRegex(CI.DependencyGitError, 'unsafe-worktree-owner'):
                CI.git(project, 'submodule', 'init', env=env)
            subprocess.run(['git', 'config', '--file', str(global_config), '--add',
                            'safe.directory', str(project)], check=True, capture_output=True)
            self.assertIn('libraries/eos-vm', CI.git(project, 'submodule', 'status', env=env))

    def test_frozen_host_keys_match_official_documented_fingerprints(self):
        inventory = json.loads((ROOT / 'tools/vendor/github-ssh-host-keys.json').read_text())
        actual = {'SHA256:' + base64.b64encode(hashlib.sha256(base64.b64decode(key.split()[1])).digest())
                  .decode().rstrip('=') for key in inventory['ssh_keys']}
        self.assertEqual(actual, {
            'SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU',
            'SHA256:p2QAMXNIC1TJYWeIOttrVc98/R1BUFWu3/LiyKgUfQM',
            'SHA256:uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s'})

    def fixture(self, parent):
        dependency = parent / "vm"
        dependency.mkdir()
        git(dependency, "init", "-q")
        for name, value in (("user.name", "Fixture"), ("user.email", "fixture@invalid.example")):
            git(dependency, "config", name, value)
        (dependency / "file.txt").write_text("fixed source\n")
        git(dependency, "add", "file.txt")
        git(dependency, "commit", "-qm", "fixed VM fixture")
        expected = git(dependency, "rev-parse", "HEAD")
        project = parent / "project"
        project.mkdir()
        git(project, "init", "-q")
        git(project, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
            str(dependency), "libraries/eos-vm")
        git(project, "submodule", "deinit", "-f", "--all")
        return project, expected

    def test_real_fixed_checkout_without_private_credentials(self):
        with tempfile.TemporaryDirectory(prefix="aic-ci-git-") as tmp:
            project, expected = self.fixture(Path(tmp))
            with mock.patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}):
                result = CI.checkout(project, expected)
            self.assertEqual(result["vm_commit"], expected)
            self.assertEqual(result["recursive_modules"], 1)
            self.assertFalse(result["VM_deployment_key_used"])

    def test_wrong_fixed_commit_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="aic-ci-pin-") as tmp:
            project, _ = self.fixture(Path(tmp))
            with mock.patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}):
                with self.assertRaisesRegex(RuntimeError, "fixed source commit"):
                    CI.checkout(project, "0" * 40)

    def test_key_is_outside_checkout_private_and_removed_on_failure(self):
        key = "-----BEGIN " + "OPENSSH PRIVATE KEY-----\nunit-test-placeholder\n-----END " + "OPENSSH PRIVATE KEY-----\n"
        identity = None
        with mock.patch.dict(os.environ, {"AIC_VM_DEPLOY_KEY": key}):
            with self.assertRaisesRegex(RuntimeError, "simulated checkout failure"):
                with CI.vm_key_environment(key, "AIC_VM_DEPLOY_KEY",
                                           lambda: ["ssh-ed25519 AAAAtestfixture"]) as env:
                    self.assertNotIn("AIC_VM_DEPLOY_KEY", env)
                    argv = shlex.split(env["GIT_SSH_COMMAND"])
                    identity = Path(argv[argv.index("-i") + 1])
                    self.assertEqual(identity.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(identity.read_text(), key)
                    self.assertNotIn(key, env["GIT_SSH_COMMAND"])
                    self.assertIn("StrictHostKeyChecking=yes", argv)
                    raise RuntimeError("simulated checkout failure")
        self.assertIsNotNone(identity)
        self.assertFalse(identity.exists())


if __name__ == "__main__":
    unittest.main()
