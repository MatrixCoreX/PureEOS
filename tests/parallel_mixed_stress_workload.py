"""Independent integer ledger checks for the real-contract stress corpus."""
from collections import Counter
import re

from parallel_execution_test import require
from parallel_release_benchmark import action_delta


def balanced_profile(manifest, entries):
    """Validate actual row targets, not just advertised mix percentages.

    Independent means no shared business row within a supported batch. Rows
    can recur in later batches; signer/resource accounting remains canonical.
    """
    require(manifest['mode'] == 'balanced' and manifest['hot_percent'] == 50 and
            len(entries) >= 64 and len(entries) % 4 == 0, 'Invalid balanced profile')
    scopes = manifest['scopes']
    require(len(scopes) >= 128 and len(set(scopes)) == len(scopes), 'Invalid balanced scopes')
    counts = Counter()
    for index, entry in enumerate(entries):
        kind = 'simple' if index % 4 < 2 else 'complex'
        contention = 'shared' if index % 2 == 0 else 'independent'
        scope = scopes[0 if contention == 'shared' else 1 + (index // 4) % (len(scopes) - 1)]
        require(entry['kind'] == kind and entry['contention'] == contention and entry['scope'] == scope and
                entry['actions'] == (1 if kind == 'simple' else manifest['complex_roots']),
                'Balanced transaction target mismatch')
        counts[kind + '_' + contention] += 1
    require(set(counts.values()) == {len(entries) // 4}, 'Unequal balanced quadrants')
    return {'counts': dict(counts), 'transaction_percent_each': 25,
            'conflict_scope': 'two shared-row chains; cold business rows disjoint within <=64-TX batches; '
                              'not a promise that 50% of scheduling attempts will conflict'}


def independent_light_profile(manifest, entries):
    count = len(entries)
    require(manifest.get('unique_business_rows') is True and manifest['unique_row_count'] == count and
            manifest['mode'] == 'simple' and manifest['hot_percent'] == 0 and
            manifest['loop_iterations'] == 0 and manifest['complex_roots'] == 1,
            'Not the requested independent light profile')
    require(len({e['id'] for e in entries}) == count and
            all(type(e['row_key']) is int and e['row_key'] == i + 1 and
                e['kind'] == 'simple' and e['scope'] == 'simple' and e['actions'] == 1 and
                e['units'] == 0 and e['contention'] == 'independent' for i, e in enumerate(entries)),
            'Independent transactions do not own unique row keys')
    if manifest.get('globally_unique_accounts'):
        require(count == manifest['transfer_pairs'] and count in (64, 1024, 8192) and
                len({e['from'] for e in entries}) == count and
                all(e.get('action') == 'step' for e in entries),
                'Independent calls reuse callers or use an unexpected action')
    return {'unique_business_rows': count, 'row_key_first': 1, 'row_key_last': count,
            'actions_per_transaction': 1, 'compute_loop_iterations': 0,
            'globally_unique_callers': bool(manifest.get('globally_unique_accounts')),
            'scope': 'disjoint persistent business rows for the ENTIRE corpus; real signature/auth/DB writes; '
                     'shared authorization/table reads and canonical resource settlement still exist'}


def independent_transfer_profile(manifest, entries):
    count = manifest['transfer_pairs']
    global_accounts = manifest.get('globally_unique_accounts', False)
    require(type(global_accounts) is bool and manifest['mode'] == 'token' and manifest['hot_percent'] == 0 and
            (count in (64, 1024, 8192) and len(entries) == count if global_accounts else count == 1024 and len(entries) >= count),
            'Invalid independent transfer pool')
    first = entries[:count]
    sources, destinations = {e['from'] for e in first}, {e['to'] for e in first}
    require(len(sources) == len(destinations) == count and not sources & destinations,
            'Transfer accounts are not disjoint pairs')
    require(all(e['kind'] == 'token' and e['from'] == first[i % count]['from'] and
                e['to'] == first[i % count]['to'] and e['units'] == e['actions'] == 1 and
                e['contention'] == 'independent' for i, e in enumerate(entries)),
            'Transfer rotation shares accounts inside a supported consecutive batch')
    if global_accounts:
        require(len({e['id'] for e in entries}) == count and
                all(e.get('action') == 'transfer' for e in entries),
                'Globally independent transfer identity/action mismatch')
    return {'independent_account_pairs': count, 'distinct_accounts': 2 * count,
            'globally_unique_accounts': bool(global_accounts),
            'scope': ('EVERY transaction has distinct sender and recipient accounts across the ENTIRE corpus; '
                      'no account reuse; shared token stat/code reads and canonical settlement still exist' if global_accounts else
                     'disjoint accounts in any <=64 consecutive corpus TXs; pairs reused across distant batches; '
                     'actual scheduling conflict/fallback diagnostics reported separately; NOT globally unique accounts per TX')}


def units(text):
    require(isinstance(text, str) and re.fullmatch(r'-?\d+\.\d{4} SYS', text), 'Unexpected core asset')
    number = text.split()[0]
    negative = number.startswith('-')
    whole, fraction = number.lstrip('-').split('.')
    amount = int(whole) * 10000 + int(fraction)
    return -amount if negative else amount


def expected_effects(manifest, entries, executed):
    balances = {name: int(value) for name, value in manifest['initial_balances'].items()}
    rows, stakes = Counter(), Counter()
    delta = action_delta(manifest['loop_iterations'], True)
    require(len({e['id'] for e in entries}) == len(entries), 'Duplicate corpus identity')
    require(executed <= {e['id'] for e in entries}, 'Unknown executed transaction')
    for entry in entries:
        if entry['id'] not in executed:
            continue
        kind, sender, amount = entry['kind'], entry['from'], entry['units']
        require(sender in balances and 1 <= entry['actions'] <= 100, 'Invalid ledger entry')
        if kind in ('simple', 'complex'):
            if manifest.get('unique_business_rows') and kind == 'simple':
                require(entry['scope'] == 'simple' and amount == 0 and entry['actions'] == 1 and
                        type(entry['row_key']) is int and 1 <= entry['row_key'] <= manifest['unique_row_count'],
                        'Invalid independent row entry')
                rows['simple', entry['row_key']] += 1
            else:
                require(entry['scope'] in manifest['scopes'] and amount == 0, 'Invalid row entry')
                rows[('simple' if kind == 'simple' else 'test', entry['scope'])] += entry['actions'] * (1 if kind == 'simple' else delta)
        else:
            require(isinstance(amount, int) and amount > 0, 'Invalid transfer amount')
            if kind == 'token':
                require(entry['to'] in balances, 'Unknown transfer recipient')
                balances[sender] -= amount
                balances[entry['to']] += amount
            elif kind == 'systemstake':
                balances[sender] -= 2 * amount
                balances['eosio.stake'] += 2 * amount
                stakes[sender] += amount
            elif kind == 'systemram':
                fee = (amount + 199) // 200
                balances[sender] -= amount
                balances['eosio.ram'] += amount - fee
                balances['eosio.fees'] += fee
            else:
                require(False, 'Unknown business kind')
    require(all(value >= 0 for value in balances.values()), 'Insufficient corpus funding')
    # FC JSON can encode large int64 balances as decimal strings. Never use
    # float, and normalize the initial side just as we normalize the working map.
    require(sum(balances.values()) == sum(int(value) for value in manifest['initial_balances'].values()),
            'Token conservation failure')
    return balances, rows, stakes


def check_independent_rows(node, count, expected_updates):
    """Check EVERY row in bounded RPC pages, outside the measured CPU span."""
    seen, calls = 0, 0
    while True:
        page = node.rpc('get_table_rows', {'code': 'simple', 'scope': 'simple', 'table': 'rows',
                        'json': True, 'lower_bound': str(seen + 1), 'limit': 1024})
        calls += 1
        require(page['rows'] or seen == count, 'Missing independent rows')
        for row in page['rows']:
            seen += 1
            require(seen <= count and int(row['id']) == seen and
                    int(row['value']) == 10 + expected_updates['simple', seen],
                    'Independent row result/order/count mismatch')
        if not page['more']:
            break
        require(seen < count and calls <= count, 'Invalid independent pagination')
    require(seen == count, 'Independent row count mismatch')
    return calls


def check_ledger(node, manifest, entries, executed):
    balances, rows, stakes = expected_effects(manifest, entries, executed)
    for owner, expected in balances.items():
        found = node.rpc('get_table_rows', {'code': 'eosio.token', 'scope': owner,
                        'table': 'accounts', 'json': True, 'limit': 2})['rows']
        require(len(found) <= 1 and (units(found[0]['balance']) if found else 0) == expected,
                'Token balance mismatch: ' + owner)
    supply = node.rpc('get_table_rows', {'code': 'eosio.token', 'scope': 'SYS',
                      'table': 'stat', 'json': True, 'limit': 2})['rows']
    require(len(supply) == 1 and units(supply[0]['supply']) == sum(balances.values()), 'Supply conservation mismatch')
    for contract in ('simple', 'test'):
        if contract == 'simple' and manifest.get('unique_business_rows'):
            check_independent_rows(node, manifest['unique_row_count'], rows)
            continue
        for scope in manifest['scopes']:
            found = node.rpc('get_table_rows', {'code': contract, 'scope': scope,
                            'table': 'rows', 'json': True, 'limit': 2})['rows']
            require(len(found) == 1 and int(found[0]['value']) == 10 + rows[contract, scope],
                    'Application result mismatch: ' + contract + '/' + scope)
    for owner in manifest['users']:
        found = node.rpc('get_table_rows', {'code': 'eosio', 'scope': owner,
                        'table': 'userres', 'json': True, 'limit': 2})['rows']
        require(len(found) == 1 and all(units(found[0][key]) == int(manifest['initial_stake_units'][owner]) + stakes[owner]
                for key in ('net_weight', 'cpu_weight')) and int(found[0]['ram_bytes']) > 0,
                'Staking/resource mismatch: ' + owner)
    policy = node.rpc('get_table_rows', {'code': 'eosio', 'scope': 'eosio',
                      'table': 'respolicy', 'json': True, 'limit': 2})['rows']
    market = node.rpc('get_table_rows', {'code': 'eosio', 'scope': '0',
                      'table': 'powup.state', 'json': True, 'limit': 2})['rows']
    require(len(policy) == len(market) == 1 and all(int(market[0][kind]['weight']) ==
            9 * int(policy[0][kind + '_weight']) for kind in ('net', 'cpu')),
            '10/90 resource split mismatch')
    application_rows = (manifest['unique_row_count'] + len(manifest['scopes']) if manifest.get('unique_business_rows')
                        else 2 * len(manifest['scopes']))
    return {'token_accounts_checked': len(balances), 'application_rows_checked': application_rows,
            'resource_accounts_checked': len(manifest['users']), 'supply_units': sum(balances.values()),
            'resource_split': '10/90 real contract state checked'}
