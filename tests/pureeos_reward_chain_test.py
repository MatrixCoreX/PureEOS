#!/usr/bin/env python3
"""Non-mutating process-identity regression tests for the isolated chain helper."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

import pureeos_reward_chain as helper


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.command = [sys.executable, '-c', 'pass']
        self.record = {'pid': 12345, 'start_ticks': '6789',
                       'exe': str(Path(sys.executable).resolve()), 'args': self.command}

    def test_execve_transient_empty_argv_is_not_recorded(self):
        process = Mock(pid=12345)
        process.poll.return_value = None
        with patch.object(helper, 'identity', side_effect=[{**self.record, 'args': []}, self.record]), \
             patch.object(helper.time, 'sleep') as sleep:
            self.assertEqual(helper.await_identity(process, self.command), self.record)
            sleep.assert_called_once_with(.01)
        process.terminate.assert_not_called()

    def test_exited_child_is_not_recorded(self):
        process = Mock(pid=12345)
        process.poll.return_value = 1
        with patch.object(helper, 'identity', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'exited'):
                helper.await_identity(process, self.command)

    def test_changed_pid_identity_never_matches(self):
        for change in ({'start_ticks': '6790'}, {'args': []}, {'exe': '/different/nodeos'}):
            with patch.object(helper, 'identity', return_value={**self.record, **change}):
                self.assertFalse(helper.matching(self.record))
        with patch.object(helper, 'identity', return_value=self.record):
            self.assertTrue(helper.matching(self.record))
        self.assertFalse(helper.matching(None))


if __name__ == '__main__':
    unittest.main()
