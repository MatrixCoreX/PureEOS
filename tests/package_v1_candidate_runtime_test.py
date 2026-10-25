"""Candidate delivery controls; synthetic files never stand in for node acceptance."""
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

from package_v1_candidate_runtime import ALLOWED, NOTICE_SOURCES, dependency_notices, package, regular_path, sha, verify_package


class RuntimePackageControls(unittest.TestCase):
    def test_isolated_cli_help_loads_its_local_support_without_side_effects(self):
        script = Path(__file__).with_name('package_v1_candidate_runtime.py')
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([sys.executable, '-I', '-B', str(script), '--help'],
                cwd=tmp, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('--candidate', result.stdout)
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def fixture(self, root):
        inputs = root/'inputs'; inputs.mkdir()
        files = {}
        for number, name in enumerate(sorted(ALLOWED)):
            path = inputs/str(number); path.write_text('fixture '+name)
            files[name] = (path, sha(path))
        provenance = dict(release_ready=False, files_sha256={n: d for n, (_, d) in files.items()})
        return files, provenance

    def test_exact_roundtrip_deterministic_and_exclusive_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); files, provenance = self.fixture(root)
            a = package(files, provenance, root/'a.tar.gz')
            b = package(files, provenance, root/'b.tar.gz')
            self.assertEqual(a['sha256'], b['sha256'])
            self.assertEqual(a['files'], len(ALLOWED)+2)
            self.assertFalse(a['release_ready'])
            with self.assertRaises(FileExistsError):
                package(files, provenance, root/'a.tar.gz')

    def test_changed_inputs_incomplete_set_and_release_claim_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); files, provenance = self.fixture(root)
            with self.assertRaises(AssertionError):
                package({}, provenance, root/'empty.tar.gz')
            with self.assertRaises(AssertionError):
                package(files, {**provenance, 'release_ready': True}, root/'release.tar.gz')
            next(iter(files.values()))[0].write_text('tampered')
            with self.assertRaises(AssertionError):
                package(files, provenance, root/'changed.tar.gz')
            self.assertFalse((root/'changed.tar.gz').exists())

    def test_parent_links_and_escaping_paths_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'file').write_text('fixture')
            (root/'alias').symlink_to(root, target_is_directory=True)
            for name in ('../file', '/file', 'alias/file', './file'):
                with self.assertRaises(AssertionError): regular_path(root, name)

    def test_all_required_notices_bind_to_exact_frozen_source_and_reject_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); identity = {'files_sha256': {}}
            for name, relative in NOTICE_SOURCES.items():
                path = root/relative; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('source notice ' + relative)
                identity['files_sha256'][relative] = sha(path)
            actual = dependency_notices(root, identity)
            self.assertEqual(set(actual), set(NOTICE_SOURCES))
            self.assertEqual(len(actual), 14)
            path = actual['licenses/LICENSE.CLI11-pinned'][0]
            path.write_text('different checkout notice')
            with self.assertRaises(AssertionError): dependency_notices(root, identity)
            path.unlink()
            with self.assertRaises(AssertionError): dependency_notices(root, identity)

    def test_missing_notice_cannot_be_replaced_by_a_different_current_license(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); files, provenance = self.fixture(root)
            files.pop(next(iter(NOTICE_SOURCES)))
            with self.assertRaises(AssertionError): package(files, provenance, root/'missing-notice.tar.gz')
            self.assertFalse((root/'missing-notice.tar.gz').exists())

    def test_archive_tampering_extra_path_duplicate_and_link_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); files, provenance = self.fixture(root)
            package(files, provenance, root/'original.tar.gz')
            for kind in ('changed', 'extra', 'duplicate', 'link', 'missing-notice'):
                target = root/(kind+'.tar.gz')
                with tarfile.open(root/'original.tar.gz') as src, tarfile.open(target, 'w:gz') as dst:
                    for member in src:
                        raw = src.extractfile(member).read()
                        if kind == 'missing-notice' and member.name == next(iter(NOTICE_SOURCES)):
                            continue
                        if kind == 'changed' and member.name == 'bin/nodeos':
                            raw = b'bad binary'; member.size = len(raw)
                        dst.addfile(member, io.BytesIO(raw))
                    if kind not in ('changed', 'missing-notice'):
                        member = tarfile.TarInfo('bin/nodeos' if kind == 'duplicate' else '../wallet/key')
                        member.mode = 0o755 if member.name.startswith('bin/') else 0o644
                        if kind == 'link': member.type = tarfile.SYMTYPE; member.linkname = '/tmp/outside'
                        dst.addfile(member, io.BytesIO(b''))
                with self.assertRaises(AssertionError): verify_package(target, provenance)


if __name__ == '__main__':
    unittest.main()
