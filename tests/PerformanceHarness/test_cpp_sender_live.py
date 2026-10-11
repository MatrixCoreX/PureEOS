#!/usr/bin/env python3
"""Exercise the real C++ generator against a loopback wire sink, NOT a chain.

No wallet or nodeos required; generated throwaway signing keys stay in memory or
0600 test files. Wire completion is deliberately never called finality.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

from finality_cpp_benchmark import validate_sends
from finality_tps import read_events

GENERATOR = CLEOS = None


class LiveSender(unittest.TestCase):
    def test_real_cpp_wire_journal_and_redaction(self):
        if GENERATOR is None or CLEOS is None:
            self.skipTest('Explicit --generator and --cleos binaries required')
        with tempfile.TemporaryDirectory() as directory, socket.socket() as server:
            root = Path(directory)
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            server.settimeout(10)
            keys = []
            for _ in range(2):
                result = subprocess.run([str(CLEOS), 'create', 'key', '--to-console'], capture_output=True,
                                        text=True, timeout=10, check=True)
                key = re.search(r'Private key:\s*(\S+)', result.stdout + result.stderr)
                self.assertIsNotNone(key, 'No generated key returned')
                keys.append(key.group(1))
            key_file = root / 'keys.json'
            key_file.write_text(json.dumps(keys))
            key_file.chmod(0o600)
            received, failures = [], []

            def sink():
                try:
                    connection, _ = server.accept()
                    with connection:
                        connection.settimeout(10)

                        def read(size):
                            data = b''
                            while len(data) < size:
                                part = connection.recv(size - len(data))
                                if not part:
                                    raise RuntimeError('Truncated wire frame')
                                data += part
                            return data

                        for _ in range(5):
                            size = struct.unpack('<I', read(4))[0]
                            if not 0 < size < 65536:
                                raise RuntimeError('Invalid wire frame size')
                            received.append(read(size))
                except Exception as error:
                    failures.append(type(error).__name__)

            thread = threading.Thread(target=sink)
            thread.start()
            command = [str(GENERATOR), '--chain-id', 'a' * 64, '--last-irreversible-block-id', 'b' * 64,
                '--contract-owner-account', 'eosio.token', '--accounts', 'alice,bob',
                '--transfer-quantity', '1.0000 SYS', '--priv-keys-file', str(key_file),
                '--trx-gen-duration', '1', '--target-tps', '5', '--port', str(server.getsockname()[1]),
                '--log-dir', str(root), '--evidence-log', str(root / 'sends.jsonl'), '--max-pending-sends', '16']
            try:
                result = subprocess.run(command, capture_output=True, timeout=15)
            finally:
                thread.join(timeout=12)
            self.assertFalse(thread.is_alive(), 'Wire reader failed to stop')
            self.assertFalse(failures, 'Wire sink reported failure')
            self.assertEqual(result.returncode, 0, 'Generator failed; secret-bearing output suppressed')
            output = result.stdout + result.stderr
            self.assertTrue(all(key.encode() not in output for key in keys), 'Generator leaked a signing key')
            self.assertEqual(len(received), 5)
            self.assertTrue(all(packet[0] == 8 for packet in received), 'Wire variant changed')
            journal = list(read_events(root / 'sends.jsonl'))
            self.assertEqual(len(validate_sends(journal, Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 5)), 5)
            self.assertTrue(all(e['actions'] == 1 for e in journal if e['type'] == 'sent'))
            # Bad key parsing must fail without reflecting the supplied secret.
            marker = 'PRIVATE_INPUT_MUST_NOT_APPEAR_IN_ERROR'
            key_file.write_text(json.dumps([marker, marker]))
            bad = subprocess.run(command, capture_output=True, timeout=10)
            self.assertNotEqual(bad.returncode, 0)
            self.assertNotIn(marker.encode(), bad.stdout + bad.stderr)


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generator', type=Path, required=True)
    parser.add_argument('--cleos', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    GENERATOR, CLEOS = args.generator.resolve(strict=True), args.cleos.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
