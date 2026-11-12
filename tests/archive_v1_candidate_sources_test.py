"""Tiny archive integrity fixtures; never read the real user source or cloud."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from archive_v1_candidate_sources import verify_source_archive


class SourceArchiveTests(unittest.TestCase):
    def archive(self, directory, mode='valid'):
        path=Path(directory)/'fixture.tar.gz'
        files={'libraries/z.cpp':b'last','CMakeLists.txt':b'first'}
        identity=dict(core_archive_sha256='core',files_sha256={n:hashlib.sha256(v).hexdigest() for n,v in files.items()},
                      relative_symlinks={'libraries/link':'../CMakeLists.txt'})
        with tarfile.open(path,'w:gz') as archive:
            for name,data in sorted(files.items()):
                info=tarfile.TarInfo('source/'+name);info.size=len(data)
                archive.addfile(info,io.BytesIO(data))
            link=tarfile.TarInfo('source/libraries/link');link.type=tarfile.SYMTYPE
            link.linkname='../../escape' if mode=='escape' else '../CMakeLists.txt';archive.addfile(link)
            if mode=='wrong-hash':identity['files_sha256']['CMakeLists.txt']='bad'
            data=json.dumps(identity).encode();info=tarfile.TarInfo('source-identity.json');info.size=len(data)
            archive.addfile(info,io.BytesIO(data))
            if mode=='duplicate':archive.addfile(info,io.BytesIO(data))
        return path,identity

    def test_manifest_order_does_not_require_random_gzip_seeks(self):
        with tempfile.TemporaryDirectory() as directory:
            path,identity=self.archive(directory)
            self.assertEqual(verify_source_archive(path,'core'),identity)

    def test_changed_bytes_wrong_candidate_duplicate_or_escaping_link_fail(self):
        for mode in ('wrong-hash','duplicate','escape','valid'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as directory:
                path,_=self.archive(directory,mode)
                with self.assertRaises(AssertionError):
                    verify_source_archive(path,'other-core' if mode=='valid' else 'core')


if __name__=='__main__':unittest.main()
