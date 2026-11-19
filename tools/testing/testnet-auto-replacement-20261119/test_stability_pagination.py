import unittest
from unittest.mock import patch
import stability_guard as g


class PaginationTest(unittest.TestCase):
    def row(self, owner, banned=False):
        return {'owner': owner, 'banned': banned, 'zero_windows': 3 if banned else 0}

    def test_complete_page(self):
        with patch.object(g, 'rpc', return_value={'rows': [self.row('a')], 'more': False}) as call:
            self.assertEqual(set(g.rows()), {'a'})
            self.assertNotIn('lower_bound', call.call_args.args[1])

    def test_owner_primary_encoding_known_vectors(self):
        self.assertEqual(g.owner_primary_key('a'), 6 << 59)
        self.assertEqual(g.owner_primary_key('eosio'), 6138663577826885632)
        self.assertRaises(AssertionError, g.owner_primary_key, 'bad0owner')

    def test_normal_next_unreturned_key_does_not_skip_it(self):
        pages = [{'rows': [self.row('a')], 'more': True, 'next_key': str(g.owner_primary_key('b'))},
                 {'rows': [self.row('b', True)], 'more': False}]
        with patch.object(g, 'rpc', side_effect=pages) as call:
            self.assertEqual(set(g.rows()), {'a', 'b'})
            self.assertEqual(call.call_args_list[1].args[1]['lower_bound'], str(g.owner_primary_key('a') + 1))

    def test_time_budget_partial_pages_include_later_bans(self):
        pages = [
            {'rows': [self.row('a')], 'more': True, 'next_key': str(g.owner_primary_key('a'))},
            {'rows': [self.row('b', True)], 'more': True, 'next_key': str(g.owner_primary_key('b'))},
            {'rows': [self.row('c')], 'more': False},
        ]
        with patch.object(g, 'rpc', side_effect=pages) as call:
            result = g.rows()
            self.assertEqual(set(result), {'a', 'b', 'c'})
            self.assertTrue(result['b']['banned'])
            self.assertEqual([c.args[1].get('lower_bound') for c in call.call_args_list], [None, str(g.owner_primary_key('a')+1), str(g.owner_primary_key('b')+1)])

    def test_missing_cursor_rejects_partial_result(self):
        with patch.object(g, 'rpc', return_value={'rows': [self.row('a')], 'more': True}):
            self.assertRaises(AssertionError, g.rows)

    def test_cursor_cycle_rejected(self):
        pages = [{'rows': [self.row(n)], 'more': True, 'next_key': key}
                 for n, key in [('a', '123'), ('b', '456'), ('c', '123')]]
        with patch.object(g, 'rpc', side_effect=pages):
            self.assertRaises(AssertionError, g.rows)

    def test_empty_nonterminal_page_rejected(self):
        with patch.object(g, 'rpc', return_value={'rows': [], 'more': True, 'next_key': str(g.owner_primary_key('a'))}):
            self.assertRaises(AssertionError, g.rows)

    def test_duplicate_owner_rejected(self):
        pages = [{'rows': [self.row('a')], 'more': True, 'next_key': str(g.owner_primary_key('a'))},
                 {'rows': [self.row('a', True)], 'more': False}]
        with patch.object(g, 'rpc', side_effect=pages):
            self.assertRaises(AssertionError, g.rows)

    def test_rpc_error_never_returns_partial_list(self):
        with patch.object(g, 'rpc', side_effect=[{'rows': [self.row('a')], 'more': True, 'next_key': str(g.owner_primary_key('a'))}, OSError('fixture timeout')]):
            self.assertRaises(OSError, g.rows)

    def test_page_bound(self):
        names = ['aa'+'12345abcdefghijklmnopqrstuvwxyz'[n//31]+'12345abcdefghijklmnopqrstuvwxyz'[n%31] for n in range(64)]
        pages = [{'rows': [self.row(name)], 'more': True, 'next_key': str(g.owner_primary_key(name))} for name in names]
        with patch.object(g, 'rpc', side_effect=pages) as call:
            self.assertRaises(AssertionError, g.rows)
            self.assertEqual(call.call_count, 64)

    def test_row_bound(self):
        pages = [{'rows': [self.row('a')]*21, 'more': False}]
        with patch.object(g, 'rpc', side_effect=pages):
            self.assertRaises(AssertionError, g.rows)


if __name__ == '__main__':
    unittest.main()
