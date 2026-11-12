"""Offline gate/transport controls, not actual node correctness evidence."""
import unittest
from pathlib import Path
import tempfile
from unittest.mock import patch

from v1_local_release_workflows import IMAGE, core_jobs, core_passed, owned_container, public_report, read_progress, recovery_passed


def core_fixture():
    return ({'status': 'passed', 'jobs': [dict(name=n, exit_code=0) for n in sorted(core_jobs())],
             'binaries_sha256': {str(n): 'abc' for n in range(11)}, 'source_identity_sha256': 'a'*64,
             'runtime_dependencies_verified':107,'runtime_dependencies_unchanged':True,
             'full_cdt_fixture_preflight':dict(status='passed',exit_code=0),
             'build_version_provenance': dict(verified=True,client='v2.0.0',
                 full='v2.0.0-src-'+'a'*64,source_manifest_sha256='a'*64)},
            dict(status='passed', candidate_test_status='passed'),
            dict(status='finished-and-archived', candidate_status='passed'))


class LocalWorkflowControls(unittest.TestCase):
    def test_fixture_repair_preflight_and_final_unchanged_manifest_required(self):
        for field,value in (('runtime_dependencies_verified',0),('runtime_dependencies_unchanged',False),
                            ('full_cdt_fixture_preflight',dict(status='failed',exit_code=201))):
            report,proof,monitor=core_fixture();report[field]=value
            self.assertFalse(core_passed(report,proof,monitor))
    def test_private_dirs_and_report_symlinks_never_archived(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = root/'report.json'; report.write_text('{}')
            self.assertTrue(public_report(report, root))
            for name in ('wallet', 'config', 'data', 'state'):
                folder = root/name; folder.mkdir(); path = folder/'report.json'; path.write_text('{}')
                self.assertFalse(public_report(path, root))
            alias = root/'alias'; alias.mkdir(); link = alias/'report.json'; link.symlink_to(report)
            self.assertFalse(public_report(link, root))

    @patch('v1_local_release_workflows.time.sleep')
    def test_only_transient_json_write_is_read_again(self, sleep):
        path = unittest.mock.Mock()
        path.read_text.side_effect = ['{', '{"status":"running"}']
        self.assertEqual(read_progress(path), {'status': 'running'})
        self.assertEqual(sleep.call_count, 1)
        path.read_text.side_effect = ['{"status":"failed"}']
        self.assertEqual(read_progress(path), {'status': 'failed'})

    def test_all_59_fixed_core_jobs_required(self):
        self.assertEqual(len(core_jobs()), 59)
        self.assertTrue(core_passed(*core_fixture()))

    def test_unknown_or_mismatched_source_version_is_not_release_provenance(self):
        for field,value in (('verified',False),('client','unknown'),('full','v2.0.0-src-'+'b'*64),
                            ('source_manifest_sha256','b'*64)):
            report,proof,monitor=core_fixture()
            report['build_version_provenance'][field]=value
            self.assertFalse(core_passed(report,proof,monitor))
        report,proof,monitor=core_fixture();report.pop('build_version_provenance')
        self.assertFalse(core_passed(report,proof,monitor))

    def test_new_native_profile_preflight_cannot_be_omitted(self):
        report, proof, monitor = core_fixture()
        report['jobs'] = [j for j in report['jobs'] if j['name'] != 'native-profile-preflight']
        self.assertFalse(core_passed(report, proof, monitor))

    def test_resource_manager_all_three_rounds_cannot_be_omitted(self):
        for n in (1, 2, 3):
            report, proof, monitor = core_fixture()
            report['jobs'] = [j for j in report['jobs'] if j['name'] != f'resource-limits-round-{n}']
            self.assertFalse(core_passed(report, proof, monitor))

    def test_archived_failed_candidate_not_relabelled(self):
        report, proof, monitor = core_fixture()
        proof['candidate_test_status'] = 'failed'
        self.assertFalse(core_passed(report, proof, monitor))

    def test_partial_running_and_failed_core_refused(self):
        for status in ('failed', 'running', 'failed-or-not-started'):
            report, proof, monitor = core_fixture(); report['status'] = status
            self.assertFalse(core_passed(report, proof, monitor))

    def test_missing_or_duplicate_case_refused(self):
        report, proof, monitor = core_fixture(); report['jobs'].pop()
        self.assertFalse(core_passed(report, proof, monitor))
        report, proof, monitor = core_fixture(); report['jobs'][0] = report['jobs'][1]
        self.assertFalse(core_passed(report, proof, monitor))

    def test_any_failed_core_job_refused(self):
        report, proof, monitor = core_fixture(); report['jobs'][7]['exit_code'] = 1
        self.assertFalse(core_passed(report, proof, monitor))

    def test_core_must_be_finished_not_just_in_memory_pass(self):
        report, proof, monitor = core_fixture(); monitor['status'] = 'running'
        self.assertFalse(core_passed(report, proof, monitor))

    def test_missing_binaries_refused(self):
        report, proof, monitor = core_fixture(); report['binaries_sha256'].pop('10')
        self.assertFalse(core_passed(report, proof, monitor))

    @patch('pureeos_historical_edge_cases_test.verify_case_set', return_value=True)
    def test_three_distinct_recovery_rounds_required(self, verify):
        report = dict(status='passed', parallel_workers=4,
            rounds=[dict(round=n, parallel_workers=4) for n in (1, 2, 3)])
        self.assertTrue(recovery_passed(report))
        report['rounds'][2]['round'] = 2; self.assertFalse(recovery_passed(report))
        report['rounds'].pop(); self.assertFalse(recovery_passed(report))

    @patch('pureeos_historical_edge_cases_test.verify_case_set', return_value=False)
    def test_outer_recovery_pass_cannot_hide_missing_adoption_or_case(self, verify):
        report = dict(status='passed', parallel_workers=4,
            rounds=[dict(round=n, parallel_workers=4) for n in (1, 2, 3)])
        self.assertFalse(recovery_passed(report))

    @patch('v1_local_release_workflows.subprocess.run')
    def test_unknown_container_label_never_stopped(self, run):
        run.return_value.returncode = 0
        run.return_value.stdout = '[{"Image": "'+IMAGE+'", "Config": {"Labels": {"org.pureeos.task": "other"}}}]'
        with self.assertRaises(AssertionError): owned_container('expected', IMAGE)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0][-3:], ['container', 'inspect', 'expected'])

    @patch('v1_local_release_workflows.subprocess.run')
    def test_wrong_image_never_stopped(self, run):
        run.return_value.returncode = 0
        run.return_value.stdout = '[{"Image": "other", "Config": {"Labels": {"org.pureeos.task": "expected"}}}]'
        with self.assertRaises(AssertionError): owned_container('expected', IMAGE)
        self.assertEqual(run.call_count, 1)

    @patch('v1_local_release_workflows.subprocess.run')
    def test_absent_container_is_not_an_error_or_stop(self, run):
        run.return_value.returncode = 1
        run.return_value.stderr = 'No such container: expected'
        self.assertFalse(owned_container('expected', IMAGE)); self.assertEqual(run.call_count, 1)

    @patch('v1_local_release_workflows.subprocess.run')
    def test_daemon_failure_is_not_proof_of_absence(self, run):
        run.return_value.returncode = 1
        for error in ('permission denied', 'Cannot connect to Docker daemon', 'No such container: other'):
            run.return_value.stderr = error
            with self.assertRaises(AssertionError): owned_container('expected', IMAGE)


if __name__ == '__main__': unittest.main()
