"""No network or real wait; verify retries cannot enclose a financial call."""
import unittest
from unittest.mock import patch
import bank_upgrade as b
import wait_review_and_finish as w

class WaitTests(unittest.TestCase):
    def test_waits_for_irreversible_not_wall_clock(self):
        with patch.object(b, 'rpc', return_value={}), patch.object(b, 'verify_proposal') as verify, \
             patch.object(b, 'observe', side_effect=[{'remaining_irreversible_seconds': 20}, {'remaining_irreversible_seconds': 0}]), \
             patch.object(w.time, 'sleep') as sleep:
            w.await_review(b.OLD, b.NEW, 6)
            sleep.assert_called_once_with(20)
            self.assertEqual(verify.call_count, 3)
            self.assertTrue(verify.call_args.kwargs['mature'])

    def test_three_read_failures_stop_without_a_financial_call(self):
        with patch.object(b, 'rpc', side_effect=RuntimeError('read failure')), patch.object(b, 'save'), \
             patch.object(w.time, 'sleep'), patch.object(b, 'execute_mint_restore_proposal') as mint:
            with self.assertRaises(RuntimeError): w.await_review(b.OLD, b.NEW, 6)
            mint.assert_not_called()

    def test_veto_failure_is_not_retried(self):
        with patch.object(b, 'rpc', return_value={}), patch.object(b, 'verify_proposal', side_effect=AssertionError('veto')), \
             patch.object(w.time, 'sleep') as sleep:
            with self.assertRaises(AssertionError): w.await_review(b.OLD, b.NEW, 6)
            sleep.assert_not_called()

if __name__ == '__main__': unittest.main()
