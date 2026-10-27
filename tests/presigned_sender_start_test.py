#!/usr/bin/env python3
"""Real C++ start-control checks against a loopback byte sink, NOT a chain/TPS test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import threading
import time


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_ready(process, timeout=15):
    """Bound the entire line, not just the arrival of its first byte."""
    deadline = time.monotonic() + timeout
    line = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while not line.endswith(b'\n'):
            remaining = deadline - time.monotonic()
            check(remaining > 0 and selector.select(remaining), 'No complete ready record from real C++ sender')
            data = os.read(process.stdout.fileno(), 4097 - len(line))
            check(data, 'C++ sender closed stdout before readiness')
            line.extend(data)
            check(len(line) <= 4096 and line.count(b'\n') <= 1, 'Invalid ready line length/shape')
    return bytes(line)


def run_case(sender, corpus, directory, label, payload, success):
    directory.mkdir()
    received, errors = [], []
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(20)
    port = listener.getsockname()[1]

    def sink():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(20)
                total = 0
                while True:
                    data = connection.recv(65536)
                    if not data: break
                    total += len(data)
                    check(total <= 4 * 1024 * 1024, 'Unexpected sink volume')
                received.append(total)
        except Exception as error:
            errors.append(type(error).__name__)
        finally:
            listener.close()

    thread = threading.Thread(target=sink); thread.start()
    process = None
    try:
        with (directory / 'stderr.log').open('x') as log:
            process = subprocess.Popen([str(sender), '--transactions', str(corpus), '--evidence',
                str(directory / 'sends.jsonl'), '--port', str(port), '--rate', '10', '--count', '1',
                '--start-stdin', '1'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
            line = read_ready(process)
            ready = json.loads(line)
            check(ready.get('status') == 'ready' and ready.get('protocol') == 1 and ready.get('count') == 1,
                  'Invalid real C++ readiness')
            # Ready must be emitted BEFORE any send, even though TCP is open.
            if payload is not None:
                process.stdin.write(payload()); process.stdin.flush()
            process.stdin.close()
            code = process.wait(timeout=15)
            output = process.stdout.read(4097)
            process.stdout.close()
            (directory / 'stdout.log').write_bytes(line + output)
            check(code == (0 if success else 2), 'Unexpected sender status')
            events = [json.loads(item) for item in (directory / 'sends.jsonl').read_text().splitlines()]
            sent = [event for event in events if event['type'] == 'sent']
            check(len(sent) == (1 if success else 0), 'Malformed start emitted a transaction')
            if success:
                check(sent[0]['queued_ns'] >= ready['ready_ns'], 'Send preceded readiness')
                check(events[-1]['type'] == 'generator_end' and events[-1]['completed'] == 1 and
                      events[-1]['errors'] == 0, 'Incomplete successful sender journal')
            else:
                check('failed in start-control' in (directory / 'stderr.log').read_text() or
                      'failed in fixed-origin' in (directory / 'stderr.log').read_text(),
                      'Rejected for an unrelated reason')
        thread.join(timeout=20)
        check(not thread.is_alive() and not errors and len(received) == 1, 'Loopback sink failed')
        check((received[0] > 0) if success else received[0] == 0, 'Unexpected wire send')
        return dict(case=label, passed=True, exit_code=code, sent=len(sent), wire_bytes=received[0])
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        if process is not None:
            for stream in (process.stdin, process.stdout):
                try: stream.close()
                except OSError: pass
        thread.join(timeout=21)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True, help='Fresh, still-unexpired signed transaction JSON')
    parser.add_argument('--work-dir', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    root = args.work_dir.resolve(); root.mkdir()
    sender, corpus = args.sender.resolve(strict=True), args.corpus.resolve(strict=True)
    with sender.open('rb') as binary: sender_hash = hashlib.file_digest(binary, 'sha256').hexdigest()
    report = dict(status='running', sender_sha256=sender_hash, cases=[],
                  scope='Real C++ stdio protocol and TCP byte-sink test; NOT acceptance, finality or TPS')
    cases = [('ready-then-send', lambda: (str(time.monotonic_ns() + 200_000_000) + '\n').encode(), True),
             ('closed-control', None, False), ('empty-control', lambda: b'\n', False),
             ('junk-control', lambda: b'abc\n', False), ('oversized-control', lambda: b'1' * 21 + b'\n', False),
             ('past-origin', lambda: b'1\n', False), ('overflow-origin', lambda: b'18446744073709551615\n', False),
             ('partial-then-eof', lambda: b'123', False)]
    try:
        for label, payload, success in cases:
            row = run_case(sender, corpus, root / label, label, payload, success)
            report['cases'].append(row)
            print(json.dumps(row), flush=True)
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
    with (root / 'report.json').open('x') as output: json.dump(report, output, indent=2)
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
