"""Readiness-test helper pipe boundaries; actual C++/wire checks run separately."""
import os
from types import SimpleNamespace
import unittest

from presigned_sender_start_test import read_ready


class ReadyLineTests(unittest.TestCase):
    def read_pipe(self, data, close_writer=True):
        reader, writer = os.pipe()
        try:
            os.write(writer, data)
            if close_writer:
                os.close(writer); writer = None
            with os.fdopen(reader, 'rb', buffering=0) as source:
                reader = None
                return read_ready(SimpleNamespace(stdout=source), timeout=.05)
        finally:
            if reader is not None: os.close(reader)
            if writer is not None: os.close(writer)

    def test_complete_line(self):
        self.assertEqual(self.read_pipe(b'{"status":"ready"}\n'), b'{"status":"ready"}\n')

    def test_eof_before_any_data(self):
        with self.assertRaisesRegex(RuntimeError, 'closed stdout'):
            self.read_pipe(b'')

    def test_partial_then_eof(self):
        with self.assertRaisesRegex(RuntimeError, 'closed stdout'):
            self.read_pipe(b'{')

    def test_partial_without_eof_has_total_timeout(self):
        with self.assertRaisesRegex(RuntimeError, 'No complete ready'):
            self.read_pipe(b'{', close_writer=False)

    def test_oversized_line(self):
        with self.assertRaisesRegex(RuntimeError, 'length/shape'):
            self.read_pipe(b'x' * 4096 + b'\n')

    def test_multiple_lines(self):
        with self.assertRaisesRegex(RuntimeError, 'length/shape'):
            self.read_pipe(b'one\ntwo\n')


if __name__ == '__main__':
    unittest.main()
