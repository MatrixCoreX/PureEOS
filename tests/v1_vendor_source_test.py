"""Offline replay of the real V1 VM patch, plus fail-closed input checks."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('prepare_vm_vendor', REPO / 'tools/prepare_vm_vendor.py')
VM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VM)


class VendorSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pureeos-v1-vendor-')
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / 'source'
        self.vendor = self.repo / 'libraries/eos-vm'
        self.vendor.mkdir(parents=True)
        destination = self.repo / 'tools/vendor'
        destination.mkdir(parents=True)
        for name in ('eos-vm-parallel-v1.json', 'eos-vm-parallel-v1.patch'):
            shutil.copy2(REPO / 'tools/vendor' / name, destination / name)
        # Actual pinned Git objects, no downloaded/upstream replacement fixture.
        subprocess.run(['git', 'clone', '--no-hardlinks', '--shared', '--no-checkout',
                        str(REPO / 'libraries/eos-vm'), str(self.vendor)],
                       check=True, capture_output=True, timeout=30)
        subprocess.run(['git', '-C', str(self.vendor), 'checkout', '--detach', VM.BASE_COMMIT],
                       check=True, capture_output=True, timeout=30)
        self.original = self.contents()

    def contents(self):
        return {name: (self.vendor / name).read_bytes() for name in VM.FILES}

    def test_unprepared_check_is_read_only(self):
        with self.assertRaisesRegex(RuntimeError, 'not prepared'):
            VM.prepare(self.repo)
        self.assertEqual(self.contents(), self.original)

    def test_real_patch_recreates_exact_four_tested_headers_and_is_idempotent(self):
        report = VM.prepare(self.repo, apply=True)
        self.assertEqual(report['operation'], 'applied')
        for name in VM.FILES:
            self.assertEqual((self.vendor / name).read_bytes(),
                             (REPO / 'libraries/eos-vm' / name).read_bytes())
        expected = self.contents()
        self.assertEqual(VM.prepare(self.repo)['operation'], 'already-prepared')
        self.assertEqual(VM.prepare(self.repo, apply=True)['operation'], 'already-prepared')
        self.assertEqual(self.contents(), expected)
        self.assertEqual(VM.git(self.vendor, 'rev-parse', 'HEAD'), VM.BASE_COMMIT)
        self.assertEqual(VM.git(self.vendor, 'diff', '--cached'), '')
        self.assertEqual(set(VM.git(self.vendor, 'diff', '--name-only').splitlines()), set(VM.FILES))

    def test_unknown_user_edit_is_not_overwritten(self):
        path = self.vendor / VM.FILES[0]
        path.write_bytes(path.read_bytes() + b'// USER EDIT\n')
        expected = self.contents()
        with self.assertRaisesRegex(RuntimeError, 'Unknown or partially modified'):
            VM.prepare(self.repo, apply=True)
        self.assertEqual(self.contents(), expected)

    def test_partial_patch_is_not_silently_repaired(self):
        name = VM.FILES[0]
        (self.vendor / name).write_bytes((REPO / 'libraries/eos-vm' / name).read_bytes())
        expected = self.contents()
        with self.assertRaisesRegex(RuntimeError, 'Unknown or partially modified'):
            VM.prepare(self.repo, apply=True)
        self.assertEqual(self.contents(), expected)

    def test_corrupt_patch_is_rejected_before_write(self):
        path = self.repo / 'tools/vendor/eos-vm-parallel-v1.patch'
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            VM.prepare(self.repo, apply=True)
        self.assertEqual(self.contents(), self.original)

    def test_unknown_base_is_rejected_even_if_files_match(self):
        with mock.patch.object(VM, 'git', side_effect=[str(self.vendor), '0' * 40]):
            with self.assertRaisesRegex(RuntimeError, 'base commit mismatch'):
                VM.prepare(self.repo, apply=True)
        self.assertEqual(self.contents(), self.original)

    def test_vendor_symlink_is_rejected(self):
        alias = self.repo / 'alias'
        alias.symlink_to(self.repo, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'real, initialized'):
            VM.prepare(alias, apply=True)
        self.assertEqual(self.contents(), self.original)

    def test_header_symlink_is_rejected(self):
        name = VM.FILES[0]
        target = self.repo / 'user-owned.hpp'
        target.write_bytes(self.original[name])
        (self.vendor / name).unlink()
        (self.vendor / name).symlink_to(target)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            VM.prepare(self.repo, apply=True)
        self.assertEqual(target.read_bytes(), self.original[name])

    def test_source_archive_check_needs_no_git_but_cannot_apply(self):
        VM.prepare(self.repo, apply=True)
        git_dir = self.vendor / '.git'
        git_dir.rename(self.repo / 'detached-test-metadata')
        self.assertIsNone(VM.prepare(self.repo)['checkout_head'])
        for name, content in self.original.items():
            (self.vendor / name).write_bytes(content)
        with self.assertRaisesRegex(RuntimeError, 'requires the pinned'):
            VM.prepare(self.repo, apply=True)
        self.assertEqual(self.contents(), self.original)


if __name__ == '__main__':
    unittest.main()
