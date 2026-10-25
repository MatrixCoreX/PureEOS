"""Audit split regression evidence without rewriting any failed attempt.

The surrounding-test snapshots differ only in the obsolete static-quorum test.
The later multi-eviction case is an append-only, separately executed addition.
Prove existing fixture bodies and production WASM/native cache stayed identical.
This is not a single green full-suite run or a first-release acceptance gate.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import traceback

from run_safe_eviction_regression import CASES


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(exist_ok=False)
    root=Path(__file__).resolve().parents[1];evidence=args.evidence_root
    baseline=evidence/'safe-eviction-fixtures-before-quorum-correction'
    fixed=evidence/'safe-eviction-fixed-quorum-fixtures'
    primary=evidence/'native-safe-eviction-attempt-2'
    matrix=evidence/'safe-eviction-regression-attempt-2'
    corrected=evidence/'native-static-quorum-correction-attempt-1'
    multiple=evidence/'native-six-safe-evictions-attempt-1'
    observer=evidence/'observer-safe-eviction-recording-attempt-1'
    reports=[p/'report.json' for p in (primary,matrix,corrected,observer,evidence/'contracts-safe-eviction-attempt-2',multiple)]
    report=dict(status='running',release_ready=False,scope=__doc__,
        started_utc=datetime.now(timezone.utc).isoformat(),proof_sha256={str(p):sha(p) for p in reports})
    def save(): (args.output/'report.json').write_text(json.dumps(report,indent=2))
    save()
    def validated_inputs(data):
        for filename,expected in data['inputs_sha256'].items():
            path=Path(filename)
            archived=baseline/path.relative_to(root) if path.is_relative_to(root) else path
            actual=archived if archived.is_file() and sha(archived)==expected else path
            assert sha(actual)==expected, 'Frozen source/artifact mismatch: '+filename
    try:
        p,m,q,o,b,s=[json.loads(path.read_text()) for path in reports]
        assert p['status']==q['status']==o['status']==b['status']==s['status']=='passed'
        assert p['rounds']==q['rounds']==s['rounds']==3
        for data in (p,m,q,o,b,s):validated_inputs(data)
        for number in range(1,4):
            assert '36255 assertions out of 36255 passed' in (primary/f'boundary-round-{number}.log').read_text()
            assert '11647 assertions out of 11647 passed' in (corrected/f'boundary-round-{number}.log').read_text()
            assert '8035 assertions out of 8035 passed' in (multiple/f'boundary-round-{number}.log').read_text()
        obsolete='rotation_committee_six_offline_seven_offline_and_recovery'
        assert q['selection']=='pureeos_sequential_rotation_tests/'+obsolete
        assert s['selection']=='pureeos_blacklist_current_policy_tests/six_finalized_bans_exit_sequentially_even_after_committee_drops_below21'
        text=(matrix/'fixed-policy-regression.log').read_text()
        passed=set(re.findall(r'Test case "pureeos_sequential_rotation_tests/([^"]+)" has passed with:',text))
        failed=set(re.findall(r'Test case "pureeos_sequential_rotation_tests/([^"]+)" has failed with:',text))
        assert m['status']=='failed' and failed=={obsolete} and passed==set(CASES)-{obsolete}
        assert m['jobs'][0]['expected_old_no_replacement_failure_verified'] is True
        # Split at exact test boundaries, not a broad whitespace-normalizing diff.
        def parts(value):
            start=value.index('BOOST_AUTO_TEST_CASE('+obsolete+')')
            end=value.index('\nBOOST_AUTO_TEST_CASE(',start+1)
            return value[:start],value[start:end],value[end:]
        old=baseline/'unittests/pureeos_sequential_rotation_tests.inc'
        new=fixed/'unittests/pureeos_sequential_rotation_tests.inc'
        a,c=parts(old.read_text()),parts(new.read_text())
        assert a[0]==c[0] and a[2]==c[2] and a[1]!=c[1], 'More than the obsolete test changed'
        for path in (fixed/'unittests').iterdir():
            if not path.is_file():continue
            current=root/'unittests'/path.name
            if path.name=='pureeos_blacklist_current_policy_tests.inc':
                # The additional multi-eviction case is separately executed
                # three times; the original single-eviction body stays exact.
                marker='BOOST_AUTO_TEST_SUITE_END()'
                prefix=path.read_text().rsplit(marker,1)[0]
                value=current.read_text()
                assert value.startswith(prefix) and value.endswith(marker+'\n')
                extra=value[len(prefix):-len(marker+'\n')]
                assert extra.count('BOOST_AUTO_TEST_CASE(')==1 and 'six_finalized_bans_exit_sequentially_even_after_committee_drops_below21' in extra
            else:
                assert sha(path)==sha(current), 'Working fixture differs from verified snapshot'
        wasm='eosio.system/eosio.system.wasm'
        assert b['artifacts_sha256'][wasm]=='9418bcd4bc4a8a0f685f8b2752c3ce35f5d33d83281b16e7b19f3a60236e61b6'
        old_abi=evidence/'contracts-attempt-2/contracts/eosio.system/eosio.system.abi'
        new_abi=evidence/'contracts-safe-eviction-attempt-2/contracts/eosio.system/eosio.system.abi'
        assert sha(old_abi)==sha(new_abi)
        assert o['verified_blocks']==34892 and o['observed_versions']==list(range(54))
        with (args.output/'observer-controls.log').open('x') as log:
            result=subprocess.run([sys.executable,'-B','-m','unittest','pureeos_committed_policy_observer_test',
                'pureeos_rotation_lifecycle_test'],cwd=root/'tests',stdout=log,stderr=subprocess.STDOUT,timeout=60)
        assert result.returncode==0 and 'Ran 26 tests' in (args.output/'observer-controls.log').read_text()
        files=[Path(__file__),root/'tests/pureeos_committed_policy_observer.py',
            root/'tests/pureeos_committed_policy_observer_test.py',root/'tests/pureeos_rotation_lifecycle_test.py']
        report.update(status='passed',primary_scenarios=2,primary_rounds=3,primary_assertions=108765,
            unchanged_surrounding_cases_passed=sorted(passed),corrected_quorum_case=obsolete,corrected_quorum_rounds=3,
            total_surrounding_scenarios=16,observer_controls=26,observer_recorded_blocks=34892,
            simultaneous_finalized_bans=6,multi_eviction_rounds=3,multi_eviction_final_size=15,
            critical_scenarios=4,critical_rounds_each=3,critical_assertions=167811,
            obsolete_failed_matrix_preserved=str(matrix/'report.json'),single_full_suite_green_run=False,
            source_snapshot_matches_current_fixtures=True,production_abi_sha256=sha(new_abi),
            audit_and_observer_sources_sha256={str(f):sha(f) for f in files},
            full_new_core_and_multinode_release_acceptance_pending=True)
    except Exception as error: report.update(status='failed',error=str(error),traceback=traceback.format_exc())
    report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k not in ('proof_sha256','audit_and_observer_sources_sha256')},indent=2))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
