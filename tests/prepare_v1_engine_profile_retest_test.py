"""Preparation guard controls only; these are not native runtime pass evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from prepare_v1_engine_profile_retest import OVERLAY, profile_audit, require_absent, safe_relative, verify_tree
from v1_local_release_workflows import sha


class PreparationControls(unittest.TestCase):
    def test_overlay_is_explicit_test_only(self):
        self.assertEqual(len(OVERLAY), 10)
        self.assertEqual(len(set(OVERLAY)), 10)
        self.assertTrue(all(n.startswith(('tests/', 'unittests/')) for n in OVERLAY))

    def test_unsafe_manifest_paths_refused(self):
        for n in ('', '.', '../a', '/tmp/a', 'tests/../../a'):
            self.assertFalse(safe_relative(n), n)
        self.assertTrue(safe_relative('unittests/a.cpp'))

    def test_missing_extra_changed_and_symlink_source_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); p = root/'a'; p.write_text('source')
            expected = {'a': sha(p)}; verify_tree(root, expected)
            p.write_text('changed')
            with self.assertRaises(AssertionError): verify_tree(root, expected)
            p.write_text('source'); (root/'extra').write_text('extra')
            with self.assertRaises(AssertionError): verify_tree(root, expected)
            (root/'extra').unlink(); p.unlink()
            with self.assertRaises(AssertionError): verify_tree(root, expected)
            p.symlink_to('/dev/null')
            with self.assertRaises(AssertionError): verify_tree(root, expected)

    @patch('prepare_v1_engine_profile_retest.subprocess.run')
    def test_only_real_docker_absence_is_accepted(self, run):
        run.return_value.returncode = 1
        run.return_value.stderr = 'Error response from daemon: No such container: test'
        require_absent('test')
        for message in ('permission denied', 'Cannot connect to Docker daemon', ''):
            run.return_value.stderr = message
            with self.assertRaises(AssertionError): require_absent('test')
        run.return_value.returncode = 0
        with self.assertRaises(RuntimeError): require_absent('test')

    def test_current_profile_routes_have_no_hardwired_interpreter(self):
        result = profile_audit(Path(__file__).resolve().parents[1])
        self.assertEqual(len(result['calls']), 6)
        self.assertIn('NOT runtime', result['scope'])

    def test_guest_rechecks_all_nine_focused_cases_and_program_hashes(self):
        import v1_engine_profile_retest_guest as guest
        self.assertEqual(len(guest.FOCUSED), 9)
        self.assertEqual(len(set(guest.FOCUSED)), 9)
        text = Path(guest.__file__).read_text()
        self.assertIn('set(FOCUSED)<=actual', text)
        self.assertIn("plan['parent_programs_sha256'][p]", text)


if __name__ == '__main__': unittest.main()
