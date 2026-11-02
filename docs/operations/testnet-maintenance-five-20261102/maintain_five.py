"""Owner-authorized maintenance, irreversible handoff, then exactly five stops."""
from pathlib import Path
import concurrent.futures
import datetime
import hashlib
import json
import os
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.request

sys.path.insert(0, '/opt/pureeos-test45-20261102-r1')
from campaign_helpers import R, load_chain, import_key, action

os.umask(0o077)
D = R / 'maintenance-five-20261102-r1'
PLAN = json.loads((D / 'PLAN.json').read_text())
CHAIN = '547e72843e450cbe4610d5f14241c9392e1195a93e8a64ad44779a8a08fc9284'
TARGETS = PLAN['targets']
assert len(TARGETS) == len(set(TARGETS)) == 5 and 'gptgenesisbp' not in TARGETS
OLD_OFFLINE = {'swissglacier', 'solarvoyager', 'radiantorbit', 'openaivision', 'oceanicbloom'}
assert not OLD_OFFLINE.intersection(TARGETS) and PLAN['chain_id'] == CHAIN
PORT = json.loads((R / 'FOUNDER_PORTS.json').read_text())['rpc']
STATE = {'status': 'PREFLIGHT', 'targets': TARGETS, 'chain_id': CHAIN}
f = None

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def write(name, value):
    p = D / (name + '.pending')
    p.write_text(json.dumps(value, indent=2) + '\n')
    p.replace(D / name)

def save(**changes):
    STATE.update(changes, UTC=now())
    write('STATE.json', STATE)
    print(json.dumps({k: v for k, v in STATE.items() if k != 'signed_packet'}), flush=True)

def rpc(method, data=None, port=8890):
    request = urllib.request.Request('http://127.0.0.1:' + str(port) + '/v1/chain/' + method,
        data=json.dumps(data or {}).encode(), headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)

def table(name):
    value = rpc('get_table_rows', {'json': True, 'code': 'eosio', 'scope': 'eosio',
                                 'table': name, 'limit': 1000})
    assert not value.get('more'), 'Truncated table: ' + name
    return value['rows']

def unit(name):
    return 'pureeos-test45-bp-' + name + '-20261102.service'

def unit_state(name):
    text = subprocess.check_output(['systemctl', 'show', unit(name), '-p', 'MainPID',
                                    '-p', 'ActiveState', '-p', 'UnitFileState'], text=True)
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)

def resources():
    nodes = threads = rss = 0
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            args = (p / 'cmdline').read_bytes().split(b'\0')
            if not any(a.endswith(b'/nodeos') for a in args) or not any(str(R).encode() in a for a in args):
                continue
            values = dict(s.split(':', 1) for s in (p / 'status').read_text().splitlines() if ':' in s)
            nodes += 1
            threads += int(values['Threads'].strip())
            rss += int(values.get('VmRSS', '0 kB').split()[0]) * 1024
        except (OSError, KeyError, ValueError):
            continue
    memory = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
    return {'UTC': now(), 'nodeos_processes': nodes, 'nodeos_threads': threads,
            'summed_RSS_bytes': rss, 'MemAvailable_bytes': int(memory['MemAvailable'].split()[0]) * 1024,
            'loadavg': Path('/proc/loadavg').read_text().strip(),
            'CPU_ticks': [int(v) for v in Path('/proc/stat').read_text().splitlines()[0].split()[1:9]]}

def duties(schedule, finalizers, rotation):
    names = set()
    for policy in schedule.values():
        if isinstance(policy, dict):
            names.update(row['producer_name'] for row in policy.get('producers', []))
    for policy in finalizers.values():
        if isinstance(policy, dict):
            names.update(row['description'] for row in policy.get('finalizers', []))
    for row in rotation:
        names.update(row['active'])
        names.update(row['target'])
    return names

def checkpoint():
    before = rpc('get_upgrade_state')
    maintenance = table('bpmaint')
    schedule = rpc('get_producer_schedule')
    finalizers = rpc('get_finalizer_info')
    rotation = table('rotation')
    after = rpc('get_upgrade_state')
    if before['head_block_id'] != after['head_block_id']:
        return None
    rows = {row['owner']: row for row in maintenance}
    if any(name not in rows or int(rows[name]['state']) != 2 or not int(rows[name]['effective_block']) for name in TARGETS):
        return None
    if set(TARGETS).intersection(duties(schedule, finalizers, rotation)):
        return None
    return {'UTC': now(), 'height': after['head_block_num'], 'id': after['head_block_id'],
            'maintenance': [rows[name] for name in TARGETS], 'schedule': schedule,
            'finalizers': finalizers, 'rotation': rotation}

try:
    info = rpc('get_info')
    assert info['chain_id'] == CHAIN
    candidates = table('rotcands')
    producers = table('producersv2')
    bonds = table('bpbonds')
    banned = {r['owner'] for r in candidates if r['banned']}
    assert banned == OLD_OFFLINE
    assert all(unit_state(n)['ActiveState'] == 'active' for n in TARGETS)
    assert all(unit_state(n)['ActiveState'] == 'inactive' for n in OLD_OFFLINE)
    before = {'UTC': now(), 'info': info, 'resources': resources(), 'producers': producers,
              'candidates': candidates, 'bonds': bonds, 'maintenance': table('bpmaint'),
              'escrow': rpc('get_currency_balance', {'code': 'eosio.token', 'account': 'eosio.bond', 'symbol': 'AIC'}),
              'pool': rpc('get_currency_balance', {'code': 'eosio.token', 'account': 'eosio.bslash', 'symbol': 'AIC'})}
    write('BEFORE.json', before)
    assert before['resources']['nodeos_processes'] == 31
    assert not any(r['owner'] in TARGETS and int(r['state']) != 0 for r in before['maintenance'])
    by_producer = {r['owner']: r for r in producers}
    by_candidate = {r['owner']: r for r in candidates}
    by_bond = {r['owner']: r for r in bonds}
    assert all(by_producer[n]['is_active'] and not by_candidate[n]['banned'] for n in TARGETS)
    assert all(int(by_bond[n]['balance']) == 100000000 and int(by_bond[n]['state']) == 0 for n in TARGETS)
    assert before['escrow'] == ['300000.0000 AIC'] and before['pool'] == ['50000.0000 AIC']
    assert not (D / 'SIGNED_REQUEST.json').exists(), 'Existing signed request: reconcile before resubmitting'
    f = load_chain()
    keys = json.loads((R / 'private/bp-keys.json').read_text())
    for name in TARGETS:
        import_key(f, keys[name]['account'])
    live = rpc('get_info', port=PORT)
    bid = bytes.fromhex(live['last_irreversible_block_id'])
    tx = {'expiration': (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=1800)).strftime('%Y-%m-%dT%H:%M:%S'),
          'ref_block_num': live['last_irreversible_block_num'] & 65535,
          'ref_block_prefix': int.from_bytes(bid[8:12], 'little'),
          'max_net_usage_words': 0, 'max_cpu_usage_ms': 0, 'delay_sec': 0,
          'context_free_actions': [],
          'actions': [action('setmaint', name, {'producer': name, 'maintenance': True}) for name in TARGETS],
          'transaction_extensions': []}
    packed = json.loads(f.run(['push', 'transaction', json.dumps(tx), '-d', '--return-packed', '-j', '--expiration', '1800']))
    assert packed['compression'] in ['none', 0]
    tid = hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()
    write('SIGNED_REQUEST.json', {'UTC': now(), 'transaction_id': tid, 'packed': packed,
                                 'actions': tx['actions'], 'start_LIB': live['last_irreversible_block_num']})
    save(status='SUBMITTING_OWNER_AUTHORIZED_MAINTENANCE', transaction_id=tid)
    try:
        result = rpc('push_transaction', packed, port=PORT)
        assert result['transaction_id'] == tid and result['processed']['receipt']['status'] == 'executed'
        write('BROADCAST.json', {'UTC': now(), 'transaction_id': tid, 'status': 'accepted'})
    except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
        if isinstance(error, urllib.error.HTTPError):
            write('BROADCAST_ERROR.json', {'UTC': now(), 'HTTP': error.code, 'body': error.read().decode(errors='replace')})
            raise
        write('BROADCAST.json', {'UTC': now(), 'transaction_id': tid, 'status': 'uncertain', 'error': str(error)})
    save(status='WAITING_REQUEST_IRREVERSIBLE', transaction_id=tid)
    cursor = live['last_irreversible_block_num'] + 1
    deadline = time.monotonic() + 900
    confirmation = None
    while not confirmation:
        current = rpc('get_info')
        while cursor <= current['last_irreversible_block_num']:
            block = rpc('get_block', {'block_num_or_id': cursor})
            for receipt in block['transactions']:
                actual = receipt['trx'] if isinstance(receipt['trx'], str) else receipt['trx']['id']
                if actual == tid:
                    assert receipt['status'] == 'executed'
                    confirmation = {'transaction_id': tid, 'height': cursor, 'block_id': block['id'], 'UTC': now()}
                    write('REQUEST_IRREVERSIBLE.json', confirmation)
            cursor += 1
        assert time.monotonic() < deadline, 'Request finality not proven; no BP stopped'
        if not confirmation:
            time.sleep(2)
    save(status='WAITING_MAINTENANCE_HANDOFF', request=confirmation)
    deadline = time.monotonic() + 3600
    anchor = None
    while True:
        if anchor is None:
            anchor = checkpoint()
            if anchor:
                write('HANDOFF_CHECKPOINT.json', anchor)
        else:
            current = rpc('get_info')
            if current['last_irreversible_block_num'] >= max(anchor['height'], *(int(r['effective_block']) for r in anchor['maintenance'])):
                block = rpc('get_block', {'block_num_or_id': anchor['height']})
                if block['id'] == anchor['id']:
                    # Monotonic maintenance rows remain excluded unless their owner resumes them.
                    rows = {r['owner']: r for r in table('bpmaint')}
                    assert all(int(rows[n]['state']) == 2 for n in TARGETS)
                    assert not set(TARGETS).intersection(duties(rpc('get_producer_schedule'), rpc('get_finalizer_info'), table('rotation')))
                    write('HANDOFF_IRREVERSIBLE.json', {'checkpoint': anchor, 'verified_at_LIB': current['last_irreversible_block_num'], 'UTC': now()})
                    break
                anchor = None
        rows = {r['owner']: r for r in table('bpmaint')}
        save(status='WAITING_MAINTENANCE_HANDOFF', states={n: rows.get(n, {}).get('state') for n in TARGETS})
        assert time.monotonic() < deadline, 'Maintenance not safely effective; no BP stopped'
        time.sleep(2)
    # Deliberately shut down only after irreversible maintenance and policy handoff.
    save(status='STOPPING_FIVE_MAINTENANCE_SERVICES')
    subprocess.run(['systemctl', 'disable', '--now', *[unit(n) for n in TARGETS]], check=True, capture_output=True, timeout=180)
    stopped = {n: unit_state(n) for n in TARGETS}
    assert all(v['ActiveState'] == 'inactive' and v['MainPID'] == '0' and v['UnitFileState'] == 'disabled' for v in stopped.values())
    write('STOPPED.json', {'UTC': now(), 'targets': TARGETS, 'units': stopped})
    first = rpc('get_info')
    start = resources()
    deadline = time.monotonic() + 90
    samples = []
    while time.monotonic() < deadline:
        current = rpc('get_info')
        sample = {'UTC': now(), 'head': current['head_block_num'], 'LIB': current['last_irreversible_block_num'],
                  'head_time': current['head_block_time']}
        samples.append(sample)
        with (D / 'POST_STOP_SAMPLES.jsonl').open('a') as out:
            out.write(json.dumps(sample) + '\n')
        time.sleep(5)
    after = {'info': rpc('get_info'), 'resources': resources(), 'maintenance': table('bpmaint'),
             'candidates': table('rotcands'), 'bonds': table('bpbonds'), 'producers': table('producersv2'),
             'schedule': rpc('get_producer_schedule'), 'finalizers': rpc('get_finalizer_info')}
    assert after['info']['head_block_num'] > first['head_block_num']
    assert after['info']['last_irreversible_block_num'] > first['last_irreversible_block_num']
    assert after['resources']['nodeos_processes'] == 26
    assert {r['owner'] for r in after['candidates'] if r['banned']} == banned
    assert {r['owner']: r['total_votes'] for r in after['producers']} == {r['owner']: r['total_votes'] for r in producers}
    assert after['bonds'] == bonds
    assert all(int(r['state']) == 2 for r in after['maintenance'] if r['owner'] in TARGETS)
    assert len(after['schedule']['active']['producers']) == 21
    assert len(after['finalizers']['active_finalizer_policy']['finalizers']) == 21
    assert after['finalizers']['active_finalizer_policy']['threshold'] == 15
    assert not set(TARGETS).intersection(duties(after['schedule'], after['finalizers'], table('rotation')))
    end = after['resources']
    delta = [b - a for a, b in zip(start['CPU_ticks'], end['CPU_ticks'])]
    after['observed_host_CPU_percent'] = 100 * (sum(delta) - delta[3] - delta[4]) / sum(delta)
    write('AFTER.json', after)
    save(status='PASS_FIVE_MAINTENANCE_AND_STOPPED', transaction_id=tid,
         remaining_online_BPs=25, history_nodes=1, nodeos_processes=26,
         active_proposers=21, active_finalizers=21, threshold=15,
         new_blacklists=0, bonds_and_votes_unchanged=True,
         before_resources=before['resources'], after_resources=end,
         observed_post_stop_host_CPU_percent=after['observed_host_CPU_percent'])
except BaseException:
    error = traceback.format_exc()
    if f:
        for secret in f.secrets:
            error = error.replace(secret, '<redacted>')
    save(status='FAILED_RETAIN_STATE_RECONCILE_BEFORE_ANY_RETRY', error=error)
    raise SystemExit(1)
