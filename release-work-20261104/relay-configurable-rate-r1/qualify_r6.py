"""Remote private qualification of the corrected wallet and native cleos."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import time
import traceback

K = Path('/opt/pureeos-key-rate-20261104-r1')
R = Path('/opt/pureeos-keynet-20261103-r2')
S = K / 'source-r6'
try:
    deadline = time.monotonic() + 2400
    frontend = Path('/opt/pureeos-explorer-key-rate-20261104-r3/BUILD_RESULT.json')
    while not frontend.exists():
        assert time.monotonic() < deadline, 'Wait for frontend build to finish before isolated retry'
        time.sleep(10)
    assert json.loads(frontend.read_text())['status'] == 'PASS'
    for filename in ['BUILD_RESULT_r3.json']:
        while not (K / filename).exists():
            assert time.monotonic() < deadline, 'Qualification prerequisite did not finish'
            time.sleep(10)
        assert json.loads((K / filename).read_text())['status'] == 'PASS'
    work = Path('/opt/krate-q/r6')
    assert not work.exists(), 'Preserve every earlier test attempt'
    env = {**os.environ, 'PATH': str(R / 'tools/node-current/bin') + ':' + os.environ['PATH'],
           'PYTHONUNBUFFERED': '1'}
    command = ['python3', '-B', str(S / 'tests/qualify.py'), '--package',
               str(R / 'pureeos-test-1.0.0-test.1'), '--work', str(work),
               '--artifacts', str(K / 'artifacts-r3'), '--native-cleos',
               str(R / 'native-client/cleos'), '--review-seconds', '1800']
    completed = subprocess.run(command, cwd=S, env=env)
    result = json.loads((work / 'RESULT.json').read_text())
    assert completed.returncode == 0 and result['status'] == 'PASS'
    abi_sources = list(work.glob('local-tests/*/candidate.abi.bin'))
    assert len(abi_sources) == 1
    shutil.copyfile(abi_sources[0], work / 'candidate.abi.bin')
    record = {'status': 'PASS', 'work': str(work), 'checks': len(result['checks']),
              'relay_rates': result['relay_rates'], 'review_seconds': 1800,
              'native': True, 'new_live_peers': 0, 'TPS': False,
              'resource_cpu_retries': result.get('resource_cpu_retries', []),
              'CPU_cap_us': 150000, 'scope': 'Functional checks; resource rejections retained, no capacity qualification',
              'ABI_binary_sha256': hashlib.sha256((work / 'candidate.abi.bin').read_bytes()).hexdigest()}
    (K / 'QUALIFICATION_r6.json').write_text(json.dumps(record, indent=2) + '\n')
    print('RATE QUALIFICATION R6 PASS', flush=True)
except BaseException:
    (K / 'QUALIFICATION_FAILURE_r6.json').write_text(json.dumps(
        {'status': 'FAIL', 'error': traceback.format_exc()}, indent=2) + '\n')
    raise
