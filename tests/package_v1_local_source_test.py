"""Source bundle controls use tiny synthetic files, not production pass claims."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from package_v1_local_source import checked_path, package, sha, verify_package


class LocalSourcePackageControls(unittest.TestCase):
    def fixture(self,root):
        source=root/'source';source.mkdir();(source/'LICENSE').write_text('test license')
        (source/'CMakeLists.txt').write_text('test source')
        identity=root/'identity.json'
        identity.write_text(json.dumps(dict(relative_symlinks={},toolchain={'compiler':'synthetic'},
            files_sha256={p.name:sha(p) for p in source.iterdir()})))
        return source,identity,{'programs/nodeos/nodeos':'a'*64}
    def test_exact_source_package_is_deterministic_and_not_a_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source,identity,binaries=self.fixture(root)
            first=package(source,identity,binaries,root/'one.tar.gz')
            second=package(source,identity,binaries,root/'two.tar.gz')
            self.assertEqual(first['sha256'],second['sha256'])
            self.assertEqual(first['files'],2);self.assertFalse(first['release_ready'])
            self.assertTrue(first['inputs_unchanged'])
    def test_unknown_private_absolute_escaping_or_link_paths_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source,identity,_=self.fixture(root)
            (source/'libraries').mkdir();(source/'libraries/link').symlink_to(source/'LICENSE')
            for name in ('../LICENSE','/LICENSE','config-dir/config.ini','libraries/wallet/keys',
                         'unknown.cpp','libraries/link'):
                with self.assertRaises(AssertionError):checked_path(source,name)
    def test_changed_manifest_source_or_existing_output_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source,identity,binaries=self.fixture(root)
            package(source,identity,binaries,root/'one.tar.gz')
            with self.assertRaises(FileExistsError):package(source,identity,binaries,root/'one.tar.gz')
            (source/'LICENSE').write_text('changed')
            with self.assertRaises(AssertionError):package(source,identity,binaries,root/'bad.tar.gz')
    def test_wrong_source_identity_or_binary_binding_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source,identity,binaries=self.fixture(root)
            package(source,identity,binaries,root/'one.tar.gz')
            with self.assertRaises(AssertionError):verify_package(root/'one.tar.gz','b'*64,binaries)
            with self.assertRaises(AssertionError):verify_package(root/'one.tar.gz',sha(identity),{})


if __name__=='__main__':unittest.main()
