"""Join only three verified same-candidate native shards; never starts cloud work."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

from v1_candidate_native_matrix import SHARDS, SUITES, selected_suites


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def check_details(details):
    assert set(details)==set(SHARDS), 'Every declared shard is mandatory'
    baseline=details['c'];all_suites=[]
    for role,detail in details.items():
        expected=list(selected_suites(role))
        assert detail['role']==role and detail['status']=='passed'
        assert detail['suites']==expected and [j['suite'] for j in detail['jobs']]==expected
        assert all(j['exit_code']==0 for j in detail['jobs'])
        for key in ('core_archive_sha256','program_sha256','contracts_sha256'):
            assert detail[key]==baseline[key], 'Mixed candidate artifacts: '+key
        all_suites.extend(expected)
    assert sorted(all_suites)==sorted(SUITES) and len(all_suites)==len(set(all_suites))
    return dict(status='passed',scope='All ten same-candidate native suites; not P2P/TPS or real 24h',
                release_ready=False,suites=all_suites,core_archive_sha256=baseline['core_archive_sha256'],
                program_sha256=baseline['program_sha256'],contracts_sha256=baseline['contracts_sha256'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for role in SHARDS:parser.add_argument('--'+role,type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    details={};proofs={}
    for role in SHARDS:
        outer=json.loads((getattr(args,role)/'report.json').read_text())
        assert outer['status']=='passed' and outer['shard']==role
        proof=outer['evidence_archive'];assert sha(proof['local'])==proof['sha256']
        with tarfile.open(proof['local']) as archive:
            assert all(m.isfile() and '/' not in m.name and m.name not in ('.','..') for m in archive)
            actual=json.load(archive.extractfile('report.json'))
        for key in ('core_archive_sha256','program_sha256','contracts_sha256'):
            assert actual[key]==outer[key]
        details[role]=actual;proofs[role]=proof
    report={**check_details(details),'archives':proofs}
    with args.output.open('x') as stream:json.dump(report,stream,indent=2)
    print(json.dumps(dict(status=report['status'],suites=len(report['suites']),release_ready=False)))


if __name__=='__main__':main()
