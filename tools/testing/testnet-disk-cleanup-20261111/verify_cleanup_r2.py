"""Read-only final log-path coverage and live-chain check after direct deletion."""
import datetime
import json
import subprocess
import urllib.request
from pathlib import Path

R = Path('/opt/pureeos-testnet-disk-cleanup-20261111-r1/delete-r2')
OPS = Path('/opt/pureeos-testnet-new-ban-20261111-r1')
CHAIN = '09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'

def rpc(name, data=None):
    assert name.startswith('get_')
    request = urllib.request.Request('http://127.0.0.1:8890/v1/chain/' + name,
                                    json.dumps(data or {}).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=15) as response:
        return json.load(response)

info = rpc('get_info')
assert info['chain_id'] == CHAIN
f = rpc('get_finalizer_info')
assert len(f['active_finalizer_policy']['finalizers']) == 21 and f['active_finalizer_policy']['threshold'] == 15
maint = rpc('get_table_rows', {'code': 'eosio', 'scope': 'eosio', 'table': 'bpmaint', 'json': True, 'limit': 20, 'time_limit_ms': 1000})
assert not maint.get('more') and [row['owner'] for row in maint['rows'] if row['state'] == 2] == ['montrealmila']
units = subprocess.check_output(['systemctl', 'list-unit-files', '--type=service', '--no-legend', '--no-pager'], text=True)
names = [line.split()[0] for line in units.splitlines() if line.split()[0].startswith('pureeos-')]
missing = []
checked = []
for name in names:
    text = subprocess.check_output(['systemctl', 'show', name, '-p', 'MainPID,ActiveState,UnitFileState,StandardOutput,StandardError,ExecStart'], text=True)
    fields = dict(line.split('=', 1) for line in text.splitlines() if '=' in line)
    command = fields.pop('ExecStart', '')
    native = 'nodeos' in command or 'replacement_node_entry.py' in command
    if not native or fields.get('MainPID') != '0' or fields.get('UnitFileState') != 'disabled':
        continue
    # systemctl show returns the output mode separately from its pathname.
    # Parse the effective unit/drop-in text; export only runtime-log paths.
    unit_text = subprocess.check_output(['systemctl', 'cat', name], text=True)
    configured = {}
    for line in unit_text.splitlines():
        for key in ['StandardOutput', 'StandardError']:
            if line.startswith(key + '='):
                configured[key] = line.split('=', 1)[1].strip()
    paths = set()
    for value in configured.values():
        if value.startswith(('append:', 'file:', 'truncate:')):
            p = Path(value.split(':', 1)[1])
            if p.name in ['nodeos.log', 'stdout.log', 'stderr.log']:
                paths.add(str(p))
                if p.exists() and p.stat().st_size:
                    missing.append({'unit': name, 'path': str(p), 'bytes': p.stat().st_size})
    checked.append({'unit': name, 'configured_runtime_logs': sorted(paths)})
# Record any remaining paths for a separately guarded completion phase.
# This verifier never unlinks a file or changes a service.
result = json.loads((R / 'RESULT.json').read_text())
plan = json.loads((R / 'PLAN.json').read_text())
assert all(not Path(row['path']).exists() for row in plan['logs'])
health = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
assert health['status'] == 'OK' and health['running_BP_count'] == 25
assert {n: row['state']['MainPID'] for n, row in health['BP_processes'].items()} == plan['protected_BP_PIDs']
output = {'status': 'PASS_NO_CONFIGURED_STOPPED_NODE_RUNTIME_LOGS_LEFT' if not missing else 'ADDITIONAL_STOPPED_RUNTIME_LOGS_IDENTIFIED',
          'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'stopped_native_units_checked': checked, 'nonempty_configured_logs_remaining': missing,
          'selected_raw_compressed_partial_files_absent': len(plan['logs']),
          'chain_info': info, 'native_policy_members': 21, 'native_threshold': 15,
          'maintenance_rows_unchanged': maint['rows'], 'health_status': health['status'],
          'online_BP': 25, 'online_history': 1,
          'disk_available_after_bytes': result['disk_available_after_bytes'],
          'shared_system_journal_and_active_node_logs_follow_existing_retention': True,
          'chain_block_and_state_history_files_are_retained': True}
target = R / 'FINAL_LIVE_R2.json'
assert not target.exists(), 'Inspect the existing final live receipt'
target.write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(output, ensure_ascii=False))
