"""Verify or explicitly apply the exact, versioned PureEOS V1 EOS VM patch."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


REPO = Path(__file__).resolve().parents[1]
BASE_COMMIT = '16f9c4c3ea7a172716d0bf68c60806e499f5988a'
FILES = tuple('include/eosio/vm/' + name for name in
              ('allocator.hpp', 'parser.hpp', 'x86_64.hpp', 'execution_context.hpp'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_file(root, relative):
    current = root
    for part in Path(relative).parts:
        current = current / part
        if current.is_symlink():
            raise RuntimeError('Refusing symlink in vendor input: ' + relative)
    if not current.is_file():
        raise RuntimeError('Missing vendor input: ' + relative)
    return current


def git(directory, *arguments):
    result = subprocess.run(['git', '-C', str(directory), *arguments],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=30)
    if result.returncode:
        raise RuntimeError('Git vendor validation failed: ' + result.stderr.strip())
    return result.stdout.strip()


def prepare(repo=REPO, *, apply=False):
    repo = Path(repo).absolute()
    vendor = repo / 'libraries/eos-vm'
    for directory in (repo, repo / 'libraries', vendor):
        if directory.is_symlink() or not directory.is_dir():
            raise RuntimeError('Expected a real, initialized vendor directory')
    manifest_path = checked_file(repo, 'tools/vendor/eos-vm-parallel-v1.json')
    patch = checked_file(repo, 'tools/vendor/eos-vm-parallel-v1.patch')
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get('schema') != 1 or manifest.get('base_commit') != BASE_COMMIT
            or set(manifest.get('files', {})) != set(FILES)):
        raise RuntimeError('Unexpected EOS VM patch manifest')
    if digest(patch) != manifest['patch_sha256']:
        raise RuntimeError('EOS VM patch checksum mismatch')
    paths = {name: checked_file(vendor, name) for name in FILES}
    hashes = {name: digest(path) for name, path in paths.items()}
    patched = all(hashes[name] == manifest['files'][name]['patched_sha256'] for name in FILES)
    original = all(hashes[name] == manifest['files'][name]['base_sha256'] for name in FILES)
    if not patched and not original:
        raise RuntimeError('Unknown or partially modified EOS VM files; no files were changed')
    # A source archive can be checked by content without Git metadata. Applying
    # requires the exact initialized repository, never an enclosing parent repo.
    head = None
    if (vendor / '.git').exists():
        if Path(git(vendor, 'rev-parse', '--show-toplevel')).resolve() != vendor.resolve():
            raise RuntimeError('Vendor Git root mismatch')
        head = git(vendor, 'rev-parse', 'HEAD')
        if head != BASE_COMMIT:
            raise RuntimeError('EOS VM base commit mismatch')
    if not patched:
        if not apply:
            raise RuntimeError('EOS VM is not prepared; explicitly run tools/prepare_vm_vendor.py --apply')
        if head is None:
            raise RuntimeError('Applying requires the pinned EOS VM Git checkout')
        git(vendor, 'apply', '--check', '--whitespace=error', str(patch))
        # Recheck immediately before the scoped write; never reset or overwrite
        # unknown user edits, change Git refs, stage files, or access the network.
        if any(digest(paths[name]) != hashes[name] for name in FILES):
            raise RuntimeError('Vendor input changed during validation')
        git(vendor, 'apply', '--whitespace=error', str(patch))
        hashes = {name: digest(path) for name, path in paths.items()}
        if not all(hashes[name] == manifest['files'][name]['patched_sha256'] for name in FILES):
            raise RuntimeError('Post-apply checksum mismatch; preserve this checkout for diagnosis')
    return dict(status='verified', operation='already-prepared' if patched else 'applied',
                base_commit=BASE_COMMIT, checkout_head=head, files_sha256=hashes,
                patch_sha256=manifest['patch_sha256'], network_access=False,
                scope='Four EOS VM headers only; not a full-source or release acceptance')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='Read-only verification (default)')
    mode.add_argument('--apply', action='store_true', help='Explicitly prepare the pinned Git checkout')
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(apply=args.apply), indent=2))
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    main()
