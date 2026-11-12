"""Package the exact local build source, including vendors; never certify a release."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import tarfile

from archive_v1_candidate_sources import TOP


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def checked_path(source, relative):
    part=PurePosixPath(relative)
    assert relative==str(part) and not part.is_absolute() and '..' not in part.parts
    assert part.parts and part.parts[0] in TOP
    assert not {'.git','__pycache__','config-dir','.ssh','wallet','wallets'}.intersection(part.parts)
    path=source/relative
    assert path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(source.resolve())
    assert not any(p.is_symlink() for p in path.parents if p!=source and p.is_relative_to(source))
    return path


def verify_package(archive, identity_sha, binaries):
    files={};seen=set();identity=None;provenance=None
    with tarfile.open(archive,'r|gz') as stream:
        for member in stream:
            assert member.isfile() and member.name not in seen
            seen.add(member.name)
            assert not PurePosixPath(member.name).is_absolute() and '..' not in PurePosixPath(member.name).parts
            assert member.uid==member.gid==0 and member.mtime==0 and member.mode in (0o644,0o755)
            data=stream.extractfile(member)
            if member.name=='source-identity.json':
                raw=data.read();assert hashlib.sha256(raw).hexdigest()==identity_sha
                identity=json.loads(raw)
            elif member.name=='BUILD_PROVENANCE.json':provenance=json.load(data)
            else:
                assert member.name.startswith('source/')
                files[member.name.removeprefix('source/')]=hashlib.file_digest(data,'sha256').hexdigest()
    assert identity and identity.get('relative_symlinks')=={}, 'Review links explicitly rather than dereferencing unknown input'
    assert files==identity['files_sha256']
    assert provenance and provenance['source_identity_sha256']==identity_sha
    assert provenance['binaries_sha256']==binaries and provenance['release_ready'] is False
    return len(files)


def package(source, identity_path, binaries, target):
    source,identity_path=Path(source),Path(identity_path)
    identity=json.loads(identity_path.read_text());identity_sha=sha(identity_path)
    assert identity.get('relative_symlinks')=={}
    paths={relative:checked_path(source,relative) for relative in identity['files_sha256']}
    assert paths and all(sha(p)==identity['files_sha256'][n] for n,p in paths.items())
    provenance=dict(scope=__doc__,source_identity_sha256=identity_sha,binaries_sha256=binaries,
        toolchain=identity['toolchain'],release_ready=False,
        note='Only exact build-source integrity. Functional, performance and joint-release gates remain independent.')
    with Path(target).open('xb') as output, gzip.GzipFile(filename='',mode='wb',fileobj=output,mtime=0,compresslevel=1) as zipped:
        with tarfile.open(fileobj=zipped,mode='w|',format=tarfile.PAX_FORMAT) as stream:
            for name,path in sorted(paths.items()):
                info=tarfile.TarInfo('source/'+name);info.size=path.stat().st_size
                info.mode=0o755 if path.stat().st_mode&0o111 else 0o644
                with path.open('rb') as data:stream.addfile(info,data)
            for name,raw in (('source-identity.json',identity_path.read_bytes()),
                    ('BUILD_PROVENANCE.json',json.dumps(provenance,sort_keys=True,indent=2).encode())):
                info=tarfile.TarInfo(name);info.size=len(raw);info.mode=0o644
                stream.addfile(info,io.BytesIO(raw))
    count=verify_package(target,identity_sha,binaries)
    assert sha(identity_path)==identity_sha and all(sha(p)==identity['files_sha256'][n] for n,p in paths.items())
    return dict(status='verified-build-source-not-release',release_ready=False,files=count,
        source_identity_sha256=identity_sha,binaries_sha256=binaries,
        archive=str(target),bytes=Path(target).stat().st_size,sha256=sha(target),inputs_unchanged=True)


def main():
    os.umask(0o077);os.nice(10)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();core=args.candidate.resolve(strict=True);output=args.output.absolute()
    assert core.name=='local-native-profile-retest-attempt-9' and output.parent==core.parent
    assert not output.exists() and not output.is_symlink()
    plan=json.loads((core/'input.json').read_text());actual=json.loads((core/'report.json').read_text())
    identity=sha(core/'source-identity-final.json');binaries=plan['same_build_resume']['binaries_sha256']
    assert actual['source_identity_sha256']==identity and actual['binaries_sha256']==binaries
    assert all(sha(core/'build'/n)==d for n,d in binaries.items())
    output.mkdir(mode=0o700)
    result=package(core/'source',core/'source-identity-final.json',binaries,output/'pureeos-exact-build-source.tar.gz')
    result.update(created_utc=datetime.now(timezone.utc).isoformat(),script_sha256=sha(__file__))
    (output/'report.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())
