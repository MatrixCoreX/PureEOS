"""Use only the existing exact C VM; no create/upgrade/deadline extension API."""
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile

SOURCE = Path(__file__).resolve().parents[1]
TASK = SOURCE.parent / 'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'
sys.path.insert(0, str(TASK))
import cloud_round as c


def main():
    assert (datetime.fromisoformat(c.DEADLINE) - c.now()).total_seconds() > 3600
    output = Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022/n4-cloud')
    output.mkdir(exist_ok=False)
    root = '/opt/pureeos-v1-hardening-20261022-n4'
    files = ['libraries/chain/parallel_worker_pool.cpp',
             'libraries/chain/include/eosio/chain/parallel_worker_signals.hpp',
             'tests/parallel_pool_cloud_guest.py']
    def sha(path):
        with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()
    plan = dict(sha256={name: sha(SOURCE / name) for name in files},
        baseline_sha256={name: sha(SOURCE.parent / 'PureEOS' / relative) for name, relative in (
            ('parallel_worker_pool.cpp', 'libraries/chain/parallel_worker_pool.cpp'),
            ('parallel_module_cache_tests.cpp', 'unittests/parallel_module_cache_tests.cpp'))},
        scope='Reuse exact existing C only; no TPS/new server/extension; frozen inputs untouched')
    (output / 'input.json').write_text(json.dumps(plan, indent=2))
    archive = output / 'payload.tar.gz'
    with tarfile.open(archive, 'x:gz') as stream:
        for name in files: stream.add(SOURCE / name, arcname=name)
        stream.add(output / 'input.json', arcname='input.json')
    c.remote('c', ['python3', '-c', 'import pathlib;p=pathlib.Path(' + repr(root) + ');p.mkdir(mode=0o700)'], 30)
    ssh = c.old.ssh_args('c')
    subprocess.run(['rsync', '-a', '--chmod=F600', '-e', shlex.join(ssh[:-1]), str(archive), ssh[-1] + ':' + root + '/'], check=True, timeout=60)
    extract = ('import tarfile,pathlib,hashlib;p=pathlib.Path(' + repr(root) + ');'
        'a=p/"payload.tar.gz";assert hashlib.sha256(a.read_bytes()).hexdigest()==' + repr(sha(archive)) + ';'
        't=tarfile.open(a);m=t.getmembers();assert all(x.isfile() and not pathlib.Path(x.name).is_absolute() and ".." not in pathlib.Path(x.name).parts for x in m);'
        't.extractall(p,members=m,filter="data")')
    c.remote('c', ['python3', '-c', extract], 30)
    unit = c.PREFIX + '-hardening-n4'
    c.remote('c', ['systemd-run', '--quiet', '--unit=' + unit, '--service-type=exec',
        '--property=RuntimeMaxSec=1800', '--property=MemoryMax=6000M', '--property=MemorySwapMax=512M',
        '--property=KillMode=control-group', '--property=StandardOutput=append:' + root + '/driver.log',
        '--property=StandardError=inherit', 'python3', root + '/tests/parallel_pool_cloud_guest.py'], 30)
    (output / 'launch.json').write_text(json.dumps(dict(unit=unit, role='c', instance_id=c.IDS['c'],
        root=root, utc=c.now().isoformat(), old_deadline_unchanged=c.DEADLINE), indent=2))
    print(json.dumps(dict(status='started', role='c', unit=unit, root=root)))


if __name__ == '__main__':
    main()
