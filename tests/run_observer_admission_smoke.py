"""Real 1->4 keyed processes validate the new committed observer; not 40 nodes/TPS."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import tempfile

from pureeos_large_multinode_local_test import LargeMultiNodeTest
from pureeos_multinode_local_test import MultiNodeTest


class ObserverAdmissionSmoke(LargeMultiNodeTest):
    names = ('eosio', 'nodea', 'nodeb', 'nodec')
    parallel_workers = 0
    full_workflow = False

    def spawn(self, command, log_name):
        if command[0] == str(self.nodeos) and self.parallel_workers:
            command = [*command, '--wasm-runtime', 'eos-vm-jit',
                '--experimental-parallel-engine', 'jit',
                '--experimental-parallel-validation-threads', str(self.parallel_workers),
                '--experimental-parallel-validation-batch-size', '8',
                '--experimental-parallel-production']
            for name in ('eosio.token', 'eosio.system'):
                digest = hashlib.sha256((self.contracts / name / (name + '.wasm')).read_bytes()).hexdigest()
                command += ['--experimental-parallel-code-hash', digest]
        return super().spawn(command, log_name)

    def tests(self):
        self.report['scope'] = __doc__
        self.report.update(parallel_workers=self.parallel_workers,
            engine='jit' if self.parallel_workers else 'default',
            workflow='full-ranking-rentals-outages' if self.full_workflow else 'admission',
            note='Workers configured; this functional P2P workflow is not an adoption counter or TPS measurement')
        if self.full_workflow:
            MultiNodeTest.tests(self)
            self.observe()
            self.check({len(e['schedule']['producers']) for e in self.report['committee_events']} == {1, 2, 3, 4},
                'Every actual committee size 1..4 reconstructed after ranking and outage workflow')
            self.check(len({p['pid'] for p in self.process_evidence().values()}) == 4,
                'Four independently keyed actual nodeos processes')
            return
        self.tx(['system', 'voteproducer', 'approve', 'alice', 'eosio'])
        self.action('deposit', {'owner': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.action('buyrex', {'from': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.observe()
        for index, name in enumerate(self.names[1:], 1):
            self.start_peer(name)
            def ready():
                self.observe()
                return self.synced(['eosio', name])
            self.wait_for(name + ' independent ledger synchronization', ready, 300)
            self.register(name)
            self.stake(name, 100 + index, 0)
            self.tx(['system', 'voteproducer', 'approve', name, name])
            expected = set(self.names[:index + 1])
            self.wait_observing(name + ' actual committed producer-policy admission',
                lambda _i, members, r: set(members) == expected and r['stage'] == 0, 900)
            self.check(set(self.last_policy) == expected, f'{index + 1} committed members reconstructed from irreversible blocks')
        self.sample_blocks(self.names, 'New observer, real 4-node slot order and independent finality', rounds=1)
        self.observe()
        self.check({len(e['schedule']['producers']) for e in self.report['committee_events']} == {1, 2, 3, 4},
            'Every actual committee size 1..4 observed without speculative RPC snapshots')
        self.check(len({p['pid'] for p in self.process_evidence().values()}) == 4, 'Four independently keyed actual nodeos processes')
        self.report['final_synchronization'] = self.synced(self.names)
        self.check(bool(self.report['final_synchronization']), 'Four independent ledgers agree on the same irreversible block ID')
        self.check_resource_split('Observer admission smoke, unchanged resource model')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--programs', type=Path, required=True)
    parser.add_argument('--contracts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workers', type=int, choices=(0, 1, 4, 8, 12, 16), default=0)
    parser.add_argument('--workflow', choices=('admission', 'full'), default='admission')
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    actual_build = args.output / 'build'
    (actual_build / 'programs').mkdir(parents=True)
    alias = Path(tempfile.mkdtemp(prefix='pv1-obs-')) / 'b'
    alias.symlink_to(actual_build)
    for name in ('nodeos', 'cleos', 'keosd', 'helios-util'):
        target = args.programs.resolve() / (name + '.elf')
        assert target.is_file()
        directory = actual_build / 'programs' / name
        directory.mkdir(); (directory / name).symlink_to(target)
    source = Path(__file__).parent
    files = [source / n for n in ('run_observer_admission_smoke.py', 'pureeos_committed_policy_observer.py',
        'pureeos_large_multinode_local_test.py', 'pureeos_multinode_local_test.py', 'pureeos_single_choice_local_test.py',
        'native_savanna_genesis_test.py')]
    files += sorted(args.programs.resolve().glob('*.elf'))
    files += sorted(args.contracts.resolve().rglob('*.wasm')) + sorted(args.contracts.resolve().rglob('*.abi'))
    def sha(p):
        with Path(p).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()
    frozen = {str(p): sha(p) for p in files}
    def stop(signum, frame): raise KeyboardInterrupt('Stop private observer smoke')
    signal.signal(signal.SIGTERM, stop)
    state = args.output / 'state'; state.mkdir()
    test = ObserverAdmissionSmoke(alias, args.contracts.resolve(), state)
    test.parallel_workers = args.workers
    test.full_workflow = args.workflow == 'full'
    code = test.execute()
    if any(sha(p) != value for p, value in frozen.items()): code = 1
    report = dict(status='passed' if code == 0 else 'failed', report=str((test.root / 'report.json').resolve()),
        inputs_sha256=frozen, finished_utc=datetime.now(timezone.utc).isoformat(),
        scope=__doc__, short_alias=str(alias), release_ready=False)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    return code


if __name__ == '__main__': raise SystemExit(main())
