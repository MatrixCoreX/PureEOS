"""Validate the CDT oracle against the current flat trace RPC schema."""
from copy import deepcopy
import unittest
from ubuntu_cdt_runtime_test import require_inline_bool_trace


def fixture():
    root = dict(action_ordinal=1, creator_action_ordinal=0, closest_unnotified_ancestor_action_ordinal=0,
                receiver='test', receipt={'receiver': 'test'},
                act={'account': 'test', 'name': 'sendbool', 'data': {'actor': 'alice'}})
    child = dict(action_ordinal=2, creator_action_ordinal=1, closest_unnotified_ancestor_action_ordinal=1,
                 receiver='test', receipt={'receiver': 'test'},
                 act={'account': 'test', 'name': 'boolfirst', 'data': {'enabled': 1, 'actor': 'alice'},
                      'hex_data': '010000000000855c34'})
    return {'processed': {'action_traces': [root, child]}}


class InlineBoolTraceTests(unittest.TestCase):
    def test_current_flat_linked_action_is_accepted(self):
        require_inline_bool_trace(fixture())

    def test_root_action_with_same_name_is_not_inline(self):
        value = fixture()
        value['processed']['action_traces'][1]['creator_action_ordinal'] = 0
        with self.assertRaises(RuntimeError): require_inline_bool_trace(value)

    def test_wrong_ancestor_is_rejected(self):
        value = fixture()
        value['processed']['action_traces'][1]['closest_unnotified_ancestor_action_ordinal'] = 0
        with self.assertRaises(RuntimeError): require_inline_bool_trace(value)

    def test_wrong_receiver_and_payload_are_rejected(self):
        for field in ('receiver', 'data', 'hex_data'):
            value = fixture()
            child = value['processed']['action_traces'][1]
            if field == 'receiver': child[field] = 'other'
            else: child['act'][field] = {} if field == 'data' else '00'
            with self.subTest(field=field), self.assertRaises(RuntimeError): require_inline_bool_trace(value)

    def test_exception_and_extra_action_are_rejected(self):
        value = fixture()
        value['processed']['action_traces'][1]['except'] = {'code': 1}
        with self.assertRaises(RuntimeError): require_inline_bool_trace(value)
        value = fixture()
        value['processed']['action_traces'].append(deepcopy(value['processed']['action_traces'][1]))
        with self.assertRaises(RuntimeError): require_inline_bool_trace(value)

    def test_nested_legacy_shape_does_not_satisfy_current_schema(self):
        value = fixture()
        root, child = value['processed']['action_traces']
        root['inline_traces'] = [child]
        value['processed']['action_traces'] = [root]
        with self.assertRaises(RuntimeError): require_inline_bool_trace(value)


if __name__ == '__main__': unittest.main()
