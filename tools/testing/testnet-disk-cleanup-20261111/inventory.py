"""Bounded read-only server disk inventory; no cleanup or service actions."""
import datetime
import json
import shutil
import subprocess
from pathlib import Path

def run(args, timeout=45):
    try:
        q = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return {'returncode': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'error': str(error)}

args = ['nice', '-n', '19', 'du', '-x', '-B1', '--max-depth=1',
        '/opt', '/var/log', '/var/cache', '/tmp', '/var/tmp']
disk = run(args, timeout=55)
if 'stdout' in disk:
    def amount(line):
        first = line.split()[0] if line.split() else ''
        return int(first) if first.isdigit() else 0
    disk['largest_lines'] = sorted(disk['stdout'].splitlines(), key=amount, reverse=True)[:100]

result = {
    'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'read_only': True,
    'df': run(['df', '-B1', '/']),
    'usage': disk,
    'deleted_open': run(['lsof', '-nP', '+L1'], timeout=25) if shutil.which('lsof') else {'tool_absent': True},
    'running_services': run(['systemctl', 'list-units', '--type=service', '--state=running', '--no-legend', '--no-pager']),
    'cache_directory_metadata': [
        {'path': str(p), 'is_dir': p.is_dir(), 'size': p.lstat().st_size}
        for p in [Path('/root/.cache'), Path('/root/.m2/repository'), Path('/var/lib/apt/lists')]
        if p.exists()
    ],
}
print(json.dumps(result, ensure_ascii=False))
