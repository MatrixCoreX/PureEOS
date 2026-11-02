"""Prepare the historical profile on a NEW host; never start a chain implicitly."""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess, tarfile

HERE = Path(__file__).resolve().parent
NODE_HASH = '05d62c4a92497153e21833be7d2796d1d1fc9eeb09ebeb4dc1034f41c72ecf67'
WASM_HASH = 'b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prepare-new-host', action='store_true')
args = parser.parse_args()
manifest = json.loads((HERE / 'REBUILD_SHA256.json').read_text())
for name, digest in manifest.items():
    assert hashlib.sha256((HERE / name).read_bytes()).hexdigest() == digest, name
print('PASS: rebuild-kit file hashes')
if not args.prepare_new_host:
    print('Check only. To prepare a new dedicated host, use --prepare-new-host as root.')
    raise SystemExit(0)

assert os.geteuid() == 0, 'Run preparation as root on the new dedicated host'
cpu = subprocess.check_output(['lscpu'], text=True)
assert '4345P' in cpu, 'Historical profile requires the selected EPYC 4345P model'
cores = set()
for topology in Path('/sys/devices/system/cpu').glob('cpu[0-9]*/topology'):
    cores.add(((topology / 'physical_package_id').read_text().strip(),
               (topology / 'core_id').read_text().strip()))
assert len(cores) == 8 and len(os.sched_getaffinity(0)) >= 16, 'Expected eight physical cores and sixteen allowed hardware threads'
memory = int(re.search(r'MemTotal:\s+(\d+)', Path('/proc/meminfo').read_text())[1]) * 1024
assert memory >= 60 * 1024**3, 'Historical profile requires about 64 GiB RAM'
assert shutil.disk_usage('/opt').free >= 100 * 1024**3, 'Reserve at least 100 GiB'
for entry in Path('/proc').iterdir():
    if entry.name.isdigit():
        try:
            assert (entry / 'comm').read_text().strip() != 'nodeos', 'An existing node is running'
        except (FileNotFoundError, ProcessLookupError):
            pass
B = Path('/opt/pureeos-bp-bond-newchain-20261102-r1')
E = Path('/opt/pureeos-bp-bond-offline-five-20261102-r1')
R = Path('/opt/pureeos-v1-release-20261101-r1/runtime-package')
C = Path('/opt/pureeos-bp-bond-20261102-r1/contracts')
T = Path('/opt/pureeos-bp-missing-peer-key-fix-20261030/source/tests')
assert not any(p.exists() for p in [B, E, R, C, T]), 'Historical paths already exist; refusing to overwrite'
os.umask(0o077)
R.mkdir(parents=True)
with tarfile.open(HERE / 'pureeos-1.0.0-linux-x86_64.tar.gz') as archive:
    archive.extractall(R, filter='data')
R.chmod(0o755)
C.mkdir(parents=True)
with tarfile.open(HERE / 'qualified-contracts.tar.gz') as archive:
    archive.extractall(C, filter='data')
assert hashlib.sha256((R / 'libexec/nodeos').read_bytes()).hexdigest() == NODE_HASH
assert hashlib.sha256((C / 'eosio.system/eosio.system.wasm').read_bytes()).hexdigest() == WASM_HASH
shutil.copytree(HERE / 'bootstrap-tests', T)
for name, destination in [('newchain', B), ('offline-five', E)]:
    shutil.copytree(HERE / name, destination)
for name in ['pressure', 'voting', 'activity']:
    destination = B / name if name == 'pressure' else E / name
    shutil.copytree(HERE / name, destination)

def key(bls=False):
    command = [str(R / 'bin/helios-util'), 'bls', 'create', 'key', '--to-console'] if bls else [str(R / 'bin/cleos'), 'create', 'key', '--to-console']
    result = subprocess.run(command, text=True, capture_output=True, check=True)
    data = dict(line.split(': ', 1) for line in result.stdout.splitlines() if ': ' in line)
    assert 'Public key' in data and 'Private key' in data
    if bls:
        assert 'Proof of Possession' in data
    return data

names = [row['name'] for row in json.loads((B / 'PLAN.json').read_text())['nodes']]
extra = ['koreamoonlit', 'indiasunrise', 'icelandcrown', 'aurorashield', 'quantumhaven']
assert len(names) == 40 and len(set(names + extra)) == 45
assert all(re.fullmatch('[a-z1-5]{12}', name) for name in names + extra)
generated = {name: {'account': key(), 'production': key(), 'peer': key(), 'bls': key(True)} for name in names + extra}
for destination, selected in [(B / 'private-keys.json', names), (E / 'private-keys.json', extra)]:
    destination.write_text(json.dumps({name: generated[name] for name in selected}, indent=2) + '\n')
    destination.chmod(0o600)
print('Prepared original runtime/contracts/bootstrap scripts and fresh BP keys.')
print('No node was started, no TPS run was made. Next: read REBUILD_HIGH_SPEC.md, then explicitly run founder bootstrap.')
