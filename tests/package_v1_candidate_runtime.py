"""Stage exact passed-core programs/contracts as a candidate, without installing or releasing."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import sys
import tarfile

# Keep the documented isolated CLI independent of PYTHONPATH and the caller's cwd.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from v1_local_release_workflows import PROGRAMS, core_passed


CONTRACTS = tuple(f'{name}/{name}.{ext}' for name in
    ('eosio.bios', 'eosio.boot', 'eosio.bpay', 'eosio.fees', 'eosio.system', 'eosio.token')
    for ext in ('wasm', 'abi')) + tuple(f'eosio.system/.{kind}/{name}.results.{ext}'
    for kind, name in (('powerup', 'powup'), ('rex', 'rex')) for ext in ('wasm', 'abi'))
DOCS = {'docs/CANDIDATE_GUIDE.txt': 'PX7_V1_CANDIDATE_GUIDE.txt',
        'docs/RELEASE_CHECKLIST.txt': 'PX7_V1_RELEASE_CHECKLIST.txt'}
DELIVERY_NOTICES = ('AIC_LICENSE.md', 'LICENSING.md',
                    'licenses/UPSTREAM_SPRING_88e9578_LICENSE.txt')
# Bind notices to the exact source that produced the passed ELF, including the
# build's inherited copy and the current pinned CLI11 dependency's own notice.
NOTICE_SOURCES = {
    'licenses/LICENSE.softfloat': 'libraries/softfloat/COPYING.txt',
    'licenses/LICENSE.wavm': 'libraries/wasm-jit/LICENSE',
    'licenses/LICENSE.secp256k1': 'libraries/libfc/secp256k1/secp256k1/COPYING',
    'licenses/LICENSE.webauthn-json': 'libraries/libfc/include/fc/crypto/webauthn_json/license.txt',
    'licenses/LICENSE.eos-vm': 'libraries/eos-vm/LICENSE',
    'licenses/LICENSE.prometheus': 'libraries/prometheus/prometheus-cpp/LICENSE',
    'licenses/LICENSE.CLI11-build': 'programs/cleos/LICENSE.CLI11',
    'licenses/LICENSE.CLI11-pinned': 'libraries/cli11/cli11/LICENSE',
    'licenses/LICENSE.bls12-381': 'libraries/libfc/libraries/bls12-381/LICENSE',
    'licenses/LICENSE.boringssl': 'libraries/libfc/libraries/boringssl/bssl/LICENSE',
    'licenses/LICENSE.appbase': 'libraries/appbase/LICENSE.md',
    'licenses/LICENSE.boost': 'libraries/boost/LICENSE_1_0.txt',
    'licenses/LICENSE.rapidjson': 'libraries/rapidjson/license.txt',
    'licenses/LICENSE.bn256': 'libraries/libfc/libraries/bn256/LICENSE',
}
ALLOWED = ({'bin/'+name for name in PROGRAMS} | {'contracts/'+name for name in CONTRACTS}
           | set(DOCS) | {'LICENSE', *DELIVERY_NOTICES} | set(NOTICE_SOURCES))


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def regular_path(root, relative):
    part = PurePosixPath(relative)
    assert relative == str(part) and not part.is_absolute() and '..' not in part.parts
    root = Path(root).resolve(strict=True)
    path = root / relative
    assert path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root)
    assert not any(p.is_symlink() for p in path.parents if p != root and p.is_relative_to(root))
    return path


def dependency_notices(source, identity):
    files = {}
    for name, relative in NOTICE_SOURCES.items():
        path = regular_path(source, relative)
        expected = identity['files_sha256'][relative]
        assert sha(path) == expected, 'Notice differs from passed build source: ' + relative
        files[name] = (path, expected)
    return files


def verify_package(archive, expected):
    seen = set(); actual = {}; provenance = None; sums = None
    with tarfile.open(archive, 'r|gz') as stream:
        for member in stream:
            assert member.isfile() and member.name not in seen
            seen.add(member.name)
            assert member.name in ALLOWED | {'BUILD_PROVENANCE.json', 'SHA256SUMS'}
            assert member.mode == (0o755 if member.name.startswith('bin/') else 0o644)
            assert member.uid == member.gid == member.mtime == 0
            data = stream.extractfile(member)
            if member.name == 'BUILD_PROVENANCE.json':
                raw = data.read(); provenance = json.loads(raw)
                actual[member.name] = hashlib.sha256(raw).hexdigest()
            elif member.name == 'SHA256SUMS':
                sums = data.read().decode()
            else:
                actual[member.name] = hashlib.file_digest(data, 'sha256').hexdigest()
    assert provenance == expected and provenance['release_ready'] is False
    assert set(provenance['files_sha256']) == ALLOWED
    assert set(actual) == ALLOWED | {'BUILD_PROVENANCE.json'}
    assert {name: actual[name] for name in ALLOWED} == provenance['files_sha256']
    assert sums == ''.join(f'{digest}  {name}\n' for name, digest in sorted(actual.items()))
    return len(actual) + 1


def package(files, provenance, target):
    assert set(files) == ALLOWED
    assert provenance['release_ready'] is False
    assert provenance['files_sha256'] == {name: digest for name, (_, digest) in files.items()}
    assert all(Path(path).is_file() and not Path(path).is_symlink() and sha(path) == digest
               for path, digest in files.values())
    encoded = json.dumps(provenance, sort_keys=True, indent=2).encode()
    sums = {name: digest for name, (_, digest) in files.items()}
    sums['BUILD_PROVENANCE.json'] = hashlib.sha256(encoded).hexdigest()
    extras = {'BUILD_PROVENANCE.json': encoded,
              'SHA256SUMS': ''.join(f'{digest}  {name}\n' for name, digest in sorted(sums.items())).encode()}
    with Path(target).open('xb') as output, gzip.GzipFile(filename='', mode='wb', fileobj=output,
            mtime=0, compresslevel=1) as zipped, tarfile.open(fileobj=zipped, mode='w|',
            format=tarfile.PAX_FORMAT) as stream:
        for name in sorted(set(files) | set(extras)):
            info = tarfile.TarInfo(name)
            info.mode = 0o755 if name.startswith('bin/') else 0o644
            if name in files:
                path, _ = files[name]; info.size = Path(path).stat().st_size
                with Path(path).open('rb') as data:
                    stream.addfile(info, data)
            else:
                info.size = len(extras[name]); stream.addfile(info, io.BytesIO(extras[name]))
    count = verify_package(target, provenance)
    assert all(sha(path) == digest for path, digest in files.values()), 'Inputs changed during packaging'
    return dict(status='verified-runtime-candidate-not-release', release_ready=False, files=count,
        archive=str(target), bytes=Path(target).stat().st_size, sha256=sha(target), inputs_unchanged=True)


def main():
    os.umask(0o077); os.nice(10)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--contracts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); core = args.candidate.resolve(strict=True)
    output = args.output.absolute()
    assert output.parent == core.parent and not output.exists() and not output.is_symlink()
    paths = {name: regular_path(core, name) for name in
             ('input.json', 'report.json', 'monitor.json', 'download-verified.json', 'source-identity-final.json')}
    frozen = {name: sha(path) for name, path in paths.items()}
    plan, report, monitor, proof = (json.loads(paths[name].read_text()) for name in
        ('input.json', 'report.json', 'monitor.json', 'download-verified.json'))
    assert core_passed(report, proof, monitor), 'Full core must be passed, closed and archived'
    assert frozen['input.json'] == report['input_sha256']
    assert frozen['source-identity-final.json'] == report['source_identity_sha256']
    archive = regular_path(core, 'candidate-evidence.tar.gz')
    assert Path(proof['local']) == archive and archive.stat().st_size == proof['bytes'] and sha(archive) == proof['sha256']
    files = {}
    for name in PROGRAMS:
        relative = f'programs/{name}/{name}'
        files['bin/'+name] = (regular_path(core/'build', relative), report['binaries_sha256'][relative])
    for relative in CONTRACTS:
        files['contracts/'+relative] = (regular_path(args.contracts, relative), plan['contracts_sha256'][relative])
    source = Path(__file__).resolve().parents[1]
    for relative, original in DOCS.items():
        path = regular_path(source/'docs/parallel-execution', original)
        files[relative] = (path, sha(path))
    identity = json.loads(paths['source-identity-final.json'].read_text())
    files['LICENSE'] = (regular_path(core/'source', 'LICENSE'), identity['files_sha256']['LICENSE'])
    files.update(dependency_notices(core/'source', identity))
    for name in DELIVERY_NOTICES:
        path = regular_path(source, name)
        files[name] = (path, sha(path))
    provenance = dict(scope=__doc__, release_ready=False,
        source_identity_sha256=report['source_identity_sha256'], core_archive_sha256=proof['sha256'],
        version=report['build_version_provenance'], toolchain=report['toolchain'], build_image=report['image'],
        runtime_scope='Ubuntu 26.10 x86_64 userspace; same-image extraction smoke still required. No general Linux binary portability claim.',
        contracts_scope='16 explicitly selected bootstrap/system/resource/token artifacts. No legacy msig/wrap or test/build-probe deployment.',
        notices_scope='Inherited build-source LICENSE, 14 fixed build-source dependency notices, the unchanged fixed upstream license supplement and current AIC boundaries; no whole-repository relicensing claim.',
        notice_sources={name: dict(source=relative, sha256=files[name][1]) for name, relative in NOTICE_SOURCES.items()},
        files_sha256={name: digest for name, (_, digest) in files.items()},
        remaining_gates=['real-day payouts', 'complete local P2P/recovery/cohort',
            'host-level faults', 'joint node/CDT release'],
        post_publication_validation=['single-node TPS comparison on the published programs'])
    output.mkdir(mode=0o700)
    result = package(files, provenance, output/'pureeos-runtime-candidate.tar.gz')
    assert all(sha(path) == frozen[name] for name, path in paths.items())
    result.update(created_utc=datetime.now(timezone.utc).isoformat(), script_sha256=sha(__file__),
        core_proof_sha256=frozen, provenance=provenance)
    (output/'report.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: result[k] for k in ('status', 'files', 'bytes', 'sha256', 'inputs_unchanged')}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
