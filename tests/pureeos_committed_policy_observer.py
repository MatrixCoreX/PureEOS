"""Sequential native-Savanna header observer for disposable integration tests.

Consume ONLY blocks at/below a previously fetched LIB, from the genesis anchor.
This reconstructs policy promotion, not BLS/signature validation: independent
node validation and cross-node irreversible-ID agreement remain mandatory.
RPC producer schedules may describe a pending block and are not an anchor.
"""
from copy import deepcopy
from datetime import datetime


class IncompletePolicyEvidence(RuntimeError):
    pass


def slot(timestamp):
    delta = datetime.fromisoformat(timestamp) - datetime(2000, 1, 1)
    micros = (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
    if micros < 0 or micros % 500000:
        raise AssertionError('Invalid native block timestamp')
    return micros // 500000


def apply_diff(items, diff):
    result = deepcopy(items)
    removed, inserted = diff['remove_indexes'], diff['insert_indexes']
    if removed != sorted(set(removed)) or [p[0] for p in inserted] != sorted({p[0] for p in inserted}):
        raise AssertionError('Non-canonical policy diff indexes')
    for consumed, index in enumerate(removed):
        if not isinstance(index, int) or not 0 <= index - consumed < len(result):
            raise AssertionError('Policy remove index out of range')
        result.pop(index - consumed)
    for index, value in inserted:
        if not isinstance(index, int) or not 0 <= index <= len(result):
            raise AssertionError('Policy insert index out of range')
        result.insert(index, deepcopy(value))
    return result


def names(policy):
    return [p['producer_name'] for p in policy['producers']]


def single_seat(before, after):
    old, new = set(names(before)), set(names(after))
    if len(new) != len(names(after)) or not 1 <= len(new) <= 21:
        raise AssertionError('Invalid active committee size or duplicate account')
    removed, added = old - new, new - old
    key_only = (not (removed or added) and names(before) == names(after) and
        any(a.get('authority') != b.get('authority')
            for a, b in zip(before['producers'], after['producers'])))
    if (after['version'] != before['version'] + 1 or len(removed) > 1 or len(added) > 1 or
            not (removed or added or key_only)):
        raise AssertionError('Policy transition was not a single-seat or key-only change')
    return dict(incoming=next(iter(added), ''), outgoing=next(iter(removed), ''))


def finalizer_policy(previous, diff):
    result = dict(generation=diff['generation'], threshold=diff['threshold'],
                  finalizers=apply_diff(previous['finalizers'], diff['finalizers_diff']))
    members = [f['description'] for f in result['finalizers']]
    if (result['generation'] <= previous['generation'] or not 1 <= len(members) <= 21 or
            len(set(members)) != len(members) or any(f['weight'] != 1 for f in result['finalizers']) or
            result['threshold'] != len(members) * 2 // 3 + 1):
        raise AssertionError('Invalid finalizer generation, membership, weight or quorum')
    return result


class CommittedPolicyObserver:
    max_unfinalized_headers = 32768

    def __init__(self, genesis, chain_id):
        self.chain_id = chain_id
        self.genesis = deepcopy(genesis)
        self.number = 0
        self.block_id = None
        self.previous_slot = None
        self.last_final = 1
        self.latest_qc = 1
        self.headers = {}  # Unfinalized QC links/timestamps only; prune below final.
        self.active = dict(version=0, producers=[dict(producer_name='eosio', authority=[0,
            dict(threshold=1, keys=[dict(key=genesis['initial_key'], weight=1)])])], proposal_slot=0)
        self.proposed = self.pending = None
        self.finalizers = None
        self.finalizer_pending = None
        self.finalizer_proposed = []
        self.finalizer_generation = 0
        self.verified_blocks = 0

    def feed(self, block, observed_lib):
        # Publish atomically: a malformed/missing header never advances the
        # cursor, so callers cannot accidentally skip the failed evidence.
        candidate = deepcopy(self)
        event = candidate._feed(block, observed_lib)
        self.__dict__.update(candidate.__dict__)
        return event

    def _feed(self, block, observed_lib):
        number = int(block['block_num'])
        if number != self.number + 1 or number > observed_lib:
            raise IncompletePolicyEvidence('Need the next consecutive irreversible block')
        if int(block['id'][:8], 16) != number or (self.number and block['previous'] != self.block_id):
            raise AssertionError('Irreversible block ID/previous-link mismatch')
        current_slot = slot(block['timestamp'])
        extension = block.get('finality_extension')
        if extension is None:
            raise IncompletePolicyEvidence('Native finality extension missing')
        claim = extension['qc_claim']
        old_active = self.active
        if self.number == 0:
            if (number != 1 or block['action_mroot'] != self.chain_id or
                    block['timestamp'] != self.genesis['initial_timestamp'] or block['producer'] != 'eosio' or
                    claim != dict(block_num=1, is_strong_qc=False) or extension.get('new_proposer_policy_diff')):
                raise AssertionError('Native genesis anchor mismatch')
            self.finalizers = finalizer_policy(dict(generation=0, finalizers=[]),
                                                extension['new_finalizer_policy_diff'])
            if self.finalizers != dict(generation=1, threshold=1, finalizers=[dict(description='eosio',
                    weight=1, public_key=self.genesis['initial_savanna']['finalizer_key'])]):
                raise AssertionError('Native genesis finalizer mismatch')
            self.finalizer_generation = 1
        else:
            if current_slot <= self.previous_slot:
                raise AssertionError('Block timestamps did not increase')
            # Capture PREVIOUS last-proposed values before promotion, matching
            # finish_next() in block_header_state.cpp, including simultaneous diffs.
            previous_last = self.proposed or self.pending or self.active
            previous_finalizer = (self.finalizer_proposed[-1][1] if self.finalizer_proposed else
                self.finalizer_pending[1] if self.finalizer_pending else self.finalizers)
            if current_slot // 12 != self.previous_slot // 12:
                prior_start = self.previous_slot - self.previous_slot % 12 - 12
                final_slot = self.headers[self.last_final][0]
                chosen = None
                if (self.proposed and self.proposed['proposal_slot'] < prior_start and
                        self.proposed['proposal_slot'] <= final_slot):
                    chosen = self.proposed
                elif self.pending and self.pending['proposal_slot'] <= final_slot:
                    chosen = self.pending
                if chosen:
                    single_seat(self.active, chosen)
                    self.active = chosen
                    if chosen is self.proposed:
                        self.proposed = self.pending = None
                    else:
                        self.pending = None
                if self.proposed and self.pending is None:
                    self.pending, self.proposed = self.proposed, None
            proposer_diff = extension.get('new_proposer_policy_diff')
            if proposer_diff:
                proposed = dict(version=proposer_diff['version'], proposal_slot=slot(proposer_diff['proposal_time']),
                    producers=apply_diff(previous_last['producers'], proposer_diff['producer_auth_diff']))
                single_seat(previous_last, proposed)
                if proposed['proposal_slot'] != current_slot:
                    raise AssertionError('Policy proposal timestamp differs from proposing block')
                self.proposed = proposed
            target = claim['block_num']
            if not self.latest_qc <= target < number or target not in self.headers:
                raise IncompletePolicyEvidence('QC ancestor unavailable or non-monotonic')
            if claim['is_strong_qc']:
                finalized = self.headers[target][1]
                if not self.last_final <= finalized <= target:
                    raise AssertionError('QC finality link regressed')
                self.last_final = finalized
            self.latest_qc = target
            # Promote finalizers using this block's newly computed last-final,
            # NOT the outer RPC LIB or an assumed head-minus-two offset.
            if self.finalizer_pending and self.finalizer_pending[0] <= self.last_final:
                self.finalizers = self.finalizer_pending[1]
                self.finalizer_pending = None
            eligible = [p for p in self.finalizer_proposed if p[0] <= self.last_final]
            retained = [p for p in self.finalizer_proposed if p[0] > self.last_final]
            if eligible:
                if self.finalizer_pending is None:
                    self.finalizer_pending = (number, eligible[-1][1])
                else:
                    retained.insert(0, eligible[-1])
            self.finalizer_proposed = retained
            diff = extension.get('new_finalizer_policy_diff')
            if diff:
                proposed_finalizer = finalizer_policy(previous_finalizer, diff)
                if proposed_finalizer['generation'] <= self.finalizer_generation:
                    raise AssertionError('Finalizer generation regressed')
                if proposer_diff and {f['description'] for f in proposed_finalizer['finalizers']} != set(names(self.proposed)):
                    raise AssertionError('Co-proposed producer/finalizer memberships differ')
                self.finalizer_generation = proposed_finalizer['generation']
                self.finalizer_proposed.append((number, proposed_finalizer))
            expected = names(self.active)[current_slot // 12 % len(self.active['producers'])]
            if block['producer'] != expected:
                raise AssertionError('Actual block producer disagrees with committed policy')
        self.number, self.block_id, self.previous_slot = number, block['id'], current_slot
        self.headers[number] = (current_slot, claim['block_num'])
        self.headers = {n: value for n, value in self.headers.items() if n >= self.last_final}
        if len(self.headers) > self.max_unfinalized_headers:
            raise IncompletePolicyEvidence('Bounded finality-link evidence exhausted')
        self.verified_blocks += 1
        if number != 1 and old_active['version'] == self.active['version']:
            return None
        delta = dict(incoming='', outgoing='') if number == 1 else single_seat(old_active, self.active)
        return dict(head=number, lib=observed_lib, block_id=self.block_id,
            schedule={k: deepcopy(self.active[k]) for k in ('version', 'producers')},
            finalizers=deepcopy(self.finalizers), rotation=dict(stage=0, **delta),
            evidence='consecutive-irreversible-native-headers', reconstructed_not_signature_audit=True)
