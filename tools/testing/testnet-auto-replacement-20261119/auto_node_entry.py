"""Bootstrap one fresh node once; subsequent starts require its existing database."""
from pathlib import Path
import hashlib
import json
import os
import re
import sys

ROOT = Path('/opt/pureeos-testnet-cpu-stability-20261119-r1')
PIN = '946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'


def arguments(name, root=ROOT):
    if not re.fullmatch('[a-z1-5]{12}', name):
        raise ValueError('Unknown node name')
    node = root / 'nodes' / name
    if node.resolve() != node or node.is_symlink():
        raise ValueError('Unexpected node root')
    metadata = json.loads((node / 'bootstrap.json').read_text())
    if metadata['name'] != name:
        raise ValueError('Bootstrap identity differs')
    data = node / 'data'
    args = ['/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/bin/nodeos',
            '--config-dir', str(node / 'config'), '--data-dir', str(data)]
    initialized = any((data / 'state' / n).is_file() for n in ['shared_memory.bin', 'chain_head.dat'])
    if not initialized:
        if (node / 'bootstrap-attempted').exists() or (node / 'bootstrap-complete').exists():
            raise RuntimeError('Refuse automatic empty-DB reset or repeated bootstrap')
        snapshot = Path(metadata['snapshot'])
        if snapshot != node / 'bootstrap-snapshot.bin' or snapshot.is_symlink():
            raise ValueError('Snapshot binding differs')
        if hashlib.sha256(snapshot.read_bytes()).hexdigest() != metadata['SHA256']:
            raise ValueError('Snapshot hash differs')
        with (node / 'bootstrap-attempted').open('x') as stream:
            stream.write('One initial snapshot attempt only\n')
            stream.flush()
            os.fsync(stream.fileno())
        args += ['--snapshot', str(snapshot)]
    return args


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('Expected exact prepared node name')
        native = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
        if hashlib.sha256(native.read_bytes()).hexdigest() != PIN:
            raise RuntimeError('Native pin changed')
        args = arguments(sys.argv[1])
    except Exception:
        print('Prepared TEST node invariant failed; preserve database and reconcile.', file=sys.stderr)
        sys.exit(78)
    os.execv(args[0], args)
