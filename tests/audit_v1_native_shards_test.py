"""Pure negative controls, not native chain test evidence."""
from copy import deepcopy
import unittest

from audit_v1_native_shards import check_details
from v1_candidate_native_matrix import SHARDS, selected_suites


class ShardAuditTests(unittest.TestCase):
    def setUp(self):
        self.details={r:dict(role=r,status='passed',suites=list(selected_suites(r)),
            jobs=[dict(suite=s,exit_code=0) for s in selected_suites(r)],
            core_archive_sha256='core',program_sha256='program',contracts_sha256={'system':'wasm'}) for r in SHARDS}

    def test_exact_full_coverage(self):self.assertEqual(len(check_details(self.details)['suites']),10)

    def test_missing_failed_or_duplicate_suites_are_not_complete(self):
        for role in SHARDS:
            bad=deepcopy(self.details);del bad[role]
            with self.assertRaises(AssertionError):check_details(bad)
            bad=deepcopy(self.details);bad[role]['jobs'][0]['exit_code']=1
            with self.assertRaises(AssertionError):check_details(bad)
            bad=deepcopy(self.details);bad[role]['jobs'].append(bad[role]['jobs'][0])
            with self.assertRaises(AssertionError):check_details(bad)

    def test_mixed_binary_contract_or_core_evidence_is_rejected(self):
        for key in ('core_archive_sha256','program_sha256','contracts_sha256'):
            bad=deepcopy(self.details);bad['b'][key]='different'
            with self.assertRaises(AssertionError):check_details(bad)


if __name__=='__main__':unittest.main()
