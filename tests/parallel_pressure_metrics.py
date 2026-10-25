"""Additional diagnostics; never replace/relax fixed-window finality counting."""
from datetime import datetime, timezone
from parallel_execution_test import require


def corpus_completion(events):
    events = sorted(events, key=lambda item: item.get('at_ns', 0))
    blocks = {e['id']: e for e in events if e['type'] == 'block'}
    anchor = next(e['id'] for e in events if e['type'] == 'anchor')
    sends = {}
    for event in events:
        if event['type'] == 'sent':
            sends.setdefault(event['id'], event['at_ns'])
    require(bool(sends), 'Empty completion cohort')
    seen, finalized = {anchor}, {}
    for event in events:
        if event['type'] != 'sample':
            continue
        head = event['lib_id']
        branch = set()
        while head not in seen:
            require(head in blocks and head not in branch, 'Incomplete completion ancestry')
            branch.add(head)
            block = blocks[head]
            for receipt in block['receipts']:
                if receipt['status'] == 'executed' and receipt['id'] in sends:
                    finalized.setdefault(receipt['id'], event['at_ns'])
            head = block['previous']
        seen.update(branch)
    require(finalized.keys() == sends.keys(), 'Completion cohort is not fully finalized')
    require(all(finalized[key] >= at for key, at in sends.items()), 'Finalization before send')
    start, end = min(sends.values()), max(finalized.values())
    require(end > start, 'Invalid completion duration')
    return {'transactions': len(sends), 'first_send_ns': start, 'last_send_ns': max(sends.values()),
            'last_finalization_ns': end, 'seconds': (end-start)/1e9,
            'tps': len(sends)*1e9/(end-start),
            'scope': 'Whole sent corpus / first send to LAST successful finalization; includes finality latency, not a steady-state capacity proof'}


def timestamp_ns(value):
    stamp = datetime.fromisoformat(value.replace('Z', '+00:00'))
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=timezone.utc)
    delta = stamp - datetime(1970, 1, 1, tzinfo=timezone.utc)
    return (delta.days*86400 + delta.seconds)*1000000000 + delta.microseconds*1000


def block_time_metrics(node, rows, events, clock_correlation):
    nonempty = [row for row in rows if row['transactions']]
    require(bool(nonempty), 'No business blocks')
    observed = {event['id']: event['at_ns'] for event in events if event['type'] == 'block'}
    offset = clock_correlation['unix_time_ns'] - clock_correlation['monotonic_ns']
    times, leads = [], []
    for row in nonempty:
        block = node.rpc('get_block', {'block_num_or_id': row['id']})
        require(block['id'] == row['id'], 'Business block identity changed')
        stamp = timestamp_ns(block['timestamp'])
        times.append(stamp)
        if row['id'] in observed:
            leads.append((stamp - (observed[row['id']] + offset))/1e6)
    first, last = min(times), max(times)
    require((last-first) % 500000000 == 0, 'Unexpected block slot interval')
    seconds = (last-first+500000000)/1e9
    count = sum(row['transactions'] for row in nonempty)
    return {'business_transactions': count, 'business_blocks': len(nonempty),
            'chain_time_seconds': seconds, 'chain_time_tps': count/seconds,
            'first_block_unix_ns': first, 'last_block_unix_ns': last,
            'max_first_observed_block_lead_ms': max(leads) if leads else None,
            'scope': 'Block timestamp span including gaps and final 0.5s slot; NOT observed-finality TPS. Lead uses before-test clock correlation.'}
