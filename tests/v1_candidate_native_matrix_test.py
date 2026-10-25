"""Pure shard/config controls; no SSH, native chain, purchases or TPS."""
import unittest

from v1_candidate_native_matrix import SHARDS, SUITES, contract_root, selected_suites


class NativeMatrixControls(unittest.TestCase):
    def test_shards_cover_every_suite_exactly_once(self):
        actual = [suite for role in SHARDS for suite in selected_suites(role)]
        self.assertCountEqual(actual, SUITES)
        self.assertEqual(len(actual), len(set(actual)))
        self.assertEqual(selected_suites(), SUITES)

    def test_heavy_rotation_has_dedicated_existing_role(self):
        self.assertEqual(selected_suites('d'), ('pureeos_sequential_rotation_tests',))
        self.assertEqual(set(SHARDS), {'b', 'c', 'd'})
        with self.assertRaises(KeyError):
            selected_suites('a')

    def test_contract_path_keeps_compiled_absolute_location(self):
        for suffix in ('-v4', '-v5', '-v6', '-v7', '-v8'):
            root = '/opt/pureeos-v1-integrated-20261022' + suffix
            self.assertEqual(contract_root(root), root + '/contracts')
        for root in ('/tmp/new', '/opt/pureeos-v1-integrated-20261022-v6/../other',
                     '/opt/pureeos-v1-integrated-20261022-v9'):
            with self.assertRaises(AssertionError):
                contract_root(root)


if __name__ == '__main__':
    unittest.main()
