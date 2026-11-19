"""Replace only canonically banned, retired, mapped and owned TEST identities."""
from pathlib import Path
import datetime
import fcntl
import json
import os
import sys
import time
import traceback
from replacement_policy import admission, similar_name, job_id, validate_job, FUND_UNITS, RAM_UNITS
from replacement_store import save, load

ROOT = Path('/opt/pureeos-testnet-cpu-stability-20261119-r1')
GUARD = Path('/opt/pureeos-testnet-stall-recovery-20261116-r1')


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def spending(jobs):
    date = now()[:10]
    daily = [j for j in jobs if j['created_utc'][:10] == date]
    last = max((j['created_utc'] for j in jobs), default=None)
    return {'daily_jobs': len(daily), 'daily_units': sum(j['reserved_units'] for j in daily),
            'total_units': sum(j['reserved_units'] for j in jobs),
            'since_last': (datetime.datetime.now(datetime.timezone.utc) - datetime.datetime.fromisoformat(last)).total_seconds() if last else 1e9}


def run(adapter, root=ROOT):
    """The adapter is the only boundary capable of signing or controlling a node."""
    config = load(root / 'AUTO_REPLACEMENT_CONFIG.json')
    if config['enabled'] is not True or config['bank'] != 'civicbankers' or config['target_BPs'] != 25:
        raise RuntimeError('Automatic replacement disabled or policy differs')
    adapter.trust_checks()
    jobs = [load(p) for p in sorted((root / 'jobs').glob('*/STATE.json'))]
    for job in jobs:
        validate_job(job)
    unfinished = [j for j in jobs if j['stage'] != 'COMPLETE']
    if len(unfinished) > 1:
        raise RuntimeError('Multiple unfinished jobs require reconciliation')
    if unfinished:
        job = unfinished[0]
        adapter.verify_original_ban(job)
        return progress(adapter, root, job)
    context, retired = adapter.context()
    decision = admission(context, spending(jobs))
    if decision != 'QUALIFIED_ONE':
        return {'status': decision, 'utc': now(), 'eligible': context['eligible'],
                'bank_units': context['bank_units'], 'required_bank_units': FUND_UNITS + RAM_UNITS + 5000000,
                'CPU_pressure_avg60': context['cpu_pressure60']}
    completed_bans = {j['id'] for j in jobs}
    eligible = [r for r in retired if job_id(r['owner'], r['ban_block_id']) not in completed_bans]
    if not eligible:
        return {'status': 'WAIT_CONFIRMED_OWNED_RETIREMENT', 'utc': now()}
    retired_node = sorted(eligible, key=lambda r: (r['ban_block_num'], r['owner']))[0]
    old = retired_node['owner']
    if old == 'gptgenesisbp':
        raise RuntimeError('Shared founder requires reviewed role handoff')
    name = None
    for ordinal in range(31 ** 2):
        candidate = similar_name(old, ordinal)
        if adapter.account_absent(candidate) and all(j['name'] != candidate for j in jobs):
            name = candidate
            break
    if name is None:
        raise RuntimeError('No unused similar name')
    identifier = job_id(old, retired_node['ban_block_id'])
    job = {'id': identifier, 'replaces': old, 'name': name, 'stage': 'RESERVED',
           'created_utc': now(), 'reserved_units': FUND_UNITS + RAM_UNITS,
           'ban_block_num': retired_node['ban_block_num'], 'ban_block_id': retired_node['ban_block_id'],
           'cpu_units': 1000000 + 100 + len(jobs), 'ordinal': len(jobs)}
    # Persist reservation before key generation, name creation or signing.
    save(root / 'jobs' / identifier / 'STATE.json', job)
    return progress(adapter, root, job)


def progress(adapter, root, job):
    validate_job(job)
    path = root / 'jobs' / job['id'] / 'STATE.json'
    adapter.verify_original_ban(job)
    if job['stage'] == 'RESERVED':
        adapter.prepare(job)
        job['stage'] = 'PREPARED'
        save(path, job)
    stages = [('PREPARED', 'create', 'ACCOUNT_CREATED'),
              ('ACCOUNT_CREATED', 'fund', 'FUNDED')]
    for current, transaction, following in stages:
        if job['stage'] == current:
            adapter.health_before_step(job)
            receipt = adapter.transaction(job, transaction)
            if receipt is None:
                return {'status': 'WAIT_ORIGINAL_TRANSACTION_IRREVERSIBILITY', 'utc': now(), 'job': job['id'], 'stage': transaction}
            job[transaction + '_receipt'] = receipt
            job['stage'] = following
            save(path, job)
    if job['stage'] == 'FUNDED':
        adapter.health_before_step(job)
        if not adapter.start_and_sync(job):
            return {'status': 'WAIT_NEW_NODE_SYNC_NO_REGISTRATION', 'utc': now(), 'job': job['id']}
        job['stage'] = 'SYNCHRONIZED'
        save(path, job)
    if job['stage'] == 'SYNCHRONIZED':
        adapter.health_before_step(job)
        receipt = adapter.transaction(job, 'register')
        if receipt is None:
            return {'status': 'WAIT_ORIGINAL_TRANSACTION_IRREVERSIBILITY', 'utc': now(), 'job': job['id'], 'stage': 'register'}
        job['register_receipt'] = receipt
        job['stage'] = 'REGISTERED'
        save(path, job)
    if job['stage'] == 'REGISTERED':
        adapter.commit_roster(job)
        job.update(stage='COMPLETE', completed_utc=now())
        save(path, job)
        return {'status': 'PASS_ONE_AUTOMATIC_OWNED_REPLACEMENT', 'utc': now(), 'job': job['id'],
                'old': job['replaces'], 'new': job['name']}
    return {'status': 'COMPLETE', 'utc': now(), 'job': job['id']}


def main():
    os.umask(0o077)
    lock = (GUARD / 'native-operation.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        save(ROOT / 'AUTO_REPLACEMENT_LATEST.json', {'status': 'SKIP_OTHER_NATIVE_OPERATION', 'utc': now()})
        return
    from replacement_chain import ChainAdapter
    with ChainAdapter(ROOT) as adapter:
        result = run(adapter)
    save(ROOT / 'AUTO_REPLACEMENT_LATEST.json', result)
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception:
        save(ROOT / 'AUTO_REPLACEMENT_LATEST.json', {'status': 'HOLD_RECONCILE_NO_NEW_FINANCIAL_ACTION',
                                                   'utc': now(), 'error': traceback.format_exc()})
        raise
