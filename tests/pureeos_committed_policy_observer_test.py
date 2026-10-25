"""Policy evidence controls. Synthetic headers are NOT cryptographic fixtures."""
from copy import deepcopy
from datetime import datetime, timedelta
import json
from pathlib import Path
import tempfile
import unittest

from pureeos_committed_policy_observer import (CommittedPolicyObserver, IncompletePolicyEvidence,
    apply_diff, names, single_seat)


def producer(name):
    return dict(producer_name=name, authority=[0, dict(threshold=1,
                keys=[dict(key='key-' + name, weight=1)])])


def finalizer(name):
    return dict(description=name, weight=1, public_key='bls-' + name)


GENESIS = dict(initial_timestamp='2000-01-01T00:00:00.000', initial_key='key-eosio',
               initial_savanna=dict(finalizer_key='bls-eosio'))
CHAIN = 'ab' * 32


def block_id(number):
    return f'{number:08x}' + '00' * 28


def timestamp(number):
    return (datetime(2000, 1, 1) + timedelta(milliseconds=500 * (number - 1))).isoformat(timespec='milliseconds')


def stream():
    result = []
    for number in range(1, 61):
        active = ['eosio'] if number < 25 else ['eosio', 'nodea'] if number < 49 else ['nodea', 'nodeb']
        owner = active[((number - 1) // 12) % len(active)]
        extension = dict(qc_claim=dict(block_num=max(1, number - 1), is_strong_qc=number > 2))
        if number == 1:
            extension['new_finalizer_policy_diff'] = dict(generation=1, threshold=1,
                finalizers_diff=dict(remove_indexes=[], insert_indexes=[[0, finalizer('eosio')]]))
        if number in (2, 25):
            version = 1 if number == 2 else 2
            removed = [] if number == 2 else [0]
            inserted_name = 'nodea' if number == 2 else 'nodeb'
            extension['new_proposer_policy_diff'] = dict(version=version, proposal_time=timestamp(number),
                producer_auth_diff=dict(remove_indexes=removed, insert_indexes=[[1, producer(inserted_name)]]))
            extension['new_finalizer_policy_diff'] = dict(generation=version + 1, threshold=2,
                finalizers_diff=dict(remove_indexes=removed, insert_indexes=[[1, finalizer(inserted_name)]]))
        result.append(dict(block_num=number, id=block_id(number), previous=block_id(number - 1),
            timestamp=timestamp(number), producer=owner, action_mroot=CHAIN,
            finality_extension=extension))
    return result


class ObserverTests(unittest.TestCase):
    def test_actual_multinode_observe_ignores_speculative_schedule_rpc_and_fills_gap(self):
        from pureeos_large_multinode_local_test import LargeMultiNodeTest
        owner = LargeMultiNodeTest.__new__(LargeMultiNodeTest)
        owner.report = dict(chain_id=CHAIN, committee_events=[], fault={}, proposals=[])
        owner.proposals, owner.observed = set(), set()
        owner.last_policy = owner.last_version = None
        owner.checkpoint = lambda: None
        blocks = stream()
        target = 25
        def rpc(method, data=None):
            if method == 'get_info':
                return dict(head_block_num=target + 2, last_irreversible_block_num=target,
                            last_irreversible_block_id=block_id(target))
            if method == 'get_block': return deepcopy(blocks[data['block_num_or_id'] - 1])
            self.fail('Observer must not treat another speculative RPC as an atomic policy view: ' + method)
        owner.rpc = rpc
        owner.table = lambda name: ([dict(stage=0)] if name == 'rotation' else [dict(owner='eosio', unpaid_blocks=1)])
        with tempfile.TemporaryDirectory() as temporary:
            owner.root = Path(temporary)
            (owner.root / 'genesis.json').write_text(json.dumps(GENESIS))
            owner.observe()
            self.assertEqual(owner.last_version, 1)
            target = 60
            owner.observe()
            self.assertEqual([e['schedule']['version'] for e in owner.report['committee_events']], [0, 1, 2])
            self.assertEqual(owner.report['policy_observer']['through'], 60)
            self.assertFalse(owner.report['policy_observer']['incomplete'])

    def test_gap_between_rpc_samples_is_reconstructed_in_order(self):
        observer = CommittedPolicyObserver(GENESIS, CHAIN)
        events = [event for block in stream() if (event := observer.feed(block, 60))]
        self.assertEqual([e['schedule']['version'] for e in events], [0, 1, 2])
        self.assertEqual([e['head'] for e in events], [1, 25, 49])
        self.assertEqual(events[-1]['rotation'], dict(stage=0, incoming='nodeb', outgoing='eosio'))
        self.assertEqual(names(observer.active), ['nodea', 'nodeb'])
        self.assertEqual(observer.finalizers['threshold'], 2)
        self.assertEqual({f['description'] for f in observer.finalizers['finalizers']}, {'nodea', 'nodeb'})
        self.assertEqual(observer.verified_blocks, 60)
        self.assertLessEqual(len(observer.headers), 4)

    def test_missing_or_reversible_block_is_incomplete_not_passed(self):
        for block, lib in ((stream()[1], 60), (stream()[0], 0)):
            observer = CommittedPolicyObserver(GENESIS, CHAIN)
            with self.assertRaises(IncompletePolicyEvidence): observer.feed(block, lib)
            self.assertEqual(observer.number, 0)

    def test_missing_evidence_does_not_advance_cursor_and_retry_is_safe(self):
        observer = CommittedPolicyObserver(GENESIS, CHAIN)
        observer.feed(stream()[0], 60)
        with self.assertRaises(IncompletePolicyEvidence): observer.feed(stream()[2], 60)
        self.assertEqual(observer.number, 1)
        observer.feed(stream()[1], 60)
        self.assertEqual(observer.number, 2)

    def test_real_single_step_two_seat_change_still_rejected(self):
        before = dict(version=51, producers=[producer(n) for n in ('nodea', 'nodeb', 'nodec')])
        after = dict(version=52, producers=[producer(n) for n in ('nodea', 'noded', 'nodee')])
        with self.assertRaises(AssertionError): single_seat(before, after)

    def test_single_eviction_is_valid_but_empty_bulk_or_noop_is_not(self):
        before = dict(version=21, producers=[producer(n) for n in ('nodea', 'nodeb', 'nodec')])
        after = dict(version=22, producers=[producer(n) for n in ('nodea', 'nodec')])
        self.assertEqual(single_seat(before, after), dict(incoming='', outgoing='nodeb'))
        for members in ([], ['nodea'], ['nodea', 'nodeb', 'nodec']):
            with self.assertRaises(AssertionError):
                single_seat(before, dict(version=22, producers=[producer(n) for n in members]))

    def test_real_key_only_policy_is_not_mistaken_for_an_empty_membership_change(self):
        before = dict(version=51, producers=[producer(n) for n in ('nodea', 'nodeb')])
        after = deepcopy(before)
        after['version'] = 52
        after['producers'][0]['authority'][1]['keys'][0]['key'] = 'rotated-key-nodea'
        self.assertEqual(single_seat(before, after), dict(incoming='', outgoing=''))
        unchanged = deepcopy(before)
        unchanged.update(version=52, proposal_slot=999)
        with self.assertRaises(AssertionError): single_seat(before, unchanged)
        reordered = deepcopy(after)
        reordered['producers'].reverse()
        with self.assertRaises(AssertionError): single_seat(before, reordered)

    def test_eviction_promotes_both_policies_from_consecutive_headers(self):
        blocks = stream()
        # After the 1->2 growth and ordinary replacement, propose a 2->1
        # removal. Synthetic control only; actual WASM/native tests are separate.
        for block in blocks[48:]:
            number = block['block_num']
            if number == 49:
                extension = block['finality_extension']
                extension['new_proposer_policy_diff'] = dict(version=3, proposal_time=timestamp(number),
                    producer_auth_diff=dict(remove_indexes=[0], insert_indexes=[]))
                extension['new_finalizer_policy_diff'] = dict(generation=4, threshold=1,
                    finalizers_diff=dict(remove_indexes=[0], insert_indexes=[]))
        for number in range(61, 86):
            blocks.append(dict(block_num=number, id=block_id(number), previous=block_id(number - 1),
                timestamp=timestamp(number), producer=('nodea' if number < 73 and (number-1)//12%2 == 0 else 'nodeb'),
                action_mroot=CHAIN, finality_extension=dict(qc_claim=dict(block_num=number-1, is_strong_qc=True))))
        observer = CommittedPolicyObserver(GENESIS, CHAIN)
        events = [e for block in blocks if (e := observer.feed(block, 85))]
        self.assertEqual([e['schedule']['version'] for e in events], [0, 1, 2, 3])
        self.assertEqual(events[-1]['head'], 73)
        self.assertEqual(events[-1]['rotation'], dict(stage=0, incoming='', outgoing='nodea'))
        self.assertEqual(names(observer.active), ['nodeb'])
        self.assertEqual(observer.finalizers['threshold'], 1)

    def test_version_gap_cannot_be_certified_without_headers(self):
        before = dict(version=51, producers=[producer('nodea')])
        after = dict(version=53, producers=[producer('nodea'), producer('nodeb')])
        with self.assertRaises(AssertionError): single_seat(before, after)

    def test_duplicate_accounts_and_twenty_two_members_rejected(self):
        for members in (['nodea', 'nodea'], ['node' + chr(97 + i) for i in range(22)]):
            with self.assertRaises(AssertionError):
                single_seat(dict(version=0, producers=[producer('nodea')]),
                            dict(version=1, producers=[producer(n) for n in members]))

    def test_bad_header_or_diff_rolls_back_observer_state(self):
        for field in ('previous', 'producer', 'quorum', 'two_seat', 'generation', 'timestamp', 'missing_extension'):
            with self.subTest(field=field):
                observer = CommittedPolicyObserver(GENESIS, CHAIN)
                observer.feed(stream()[0], 60)
                bad = deepcopy(stream()[1])
                if field == 'previous': bad['previous'] = 'ff' * 32
                elif field == 'producer': bad['producer'] = 'nodez'
                elif field == 'timestamp': bad['timestamp'] = timestamp(1)
                elif field == 'missing_extension': del bad['finality_extension']
                elif field == 'quorum': bad['finality_extension']['new_finalizer_policy_diff']['threshold'] = 1
                elif field == 'generation': bad['finality_extension']['new_finalizer_policy_diff']['generation'] = 1
                else:
                    bad['finality_extension']['new_proposer_policy_diff']['producer_auth_diff']['insert_indexes'].append([2, producer('nodeb')])
                with self.assertRaises((AssertionError, IncompletePolicyEvidence)): observer.feed(bad, 60)
                self.assertEqual(observer.number, 1)
                self.assertEqual(observer.active['version'], 0)
                observer.feed(stream()[1], 60)
                self.assertEqual(observer.number, 2)

    def test_bad_genesis_key_or_chain_rejected(self):
        for genesis, chain in (({**GENESIS, 'initial_timestamp': timestamp(2)}, CHAIN), (GENESIS, 'ff' * 32)):
            with self.assertRaises(AssertionError): CommittedPolicyObserver(genesis, chain).feed(stream()[0], 60)

    def test_diff_indexes_checked_before_mutation(self):
        for diff in (dict(remove_indexes=[1, 0], insert_indexes=[]),
                     dict(remove_indexes=[9], insert_indexes=[]),
                     dict(remove_indexes=[], insert_indexes=[[2, 'x']]),
                     dict(remove_indexes=[], insert_indexes=[[0, 'x'], [0, 'y']])):
            original = ['a']
            with self.assertRaises(AssertionError): apply_diff(original, diff)
            self.assertEqual(original, ['a'])


if __name__ == '__main__': unittest.main()
