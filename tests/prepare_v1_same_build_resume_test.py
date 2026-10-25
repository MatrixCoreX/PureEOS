"""Resume is an exact verified green prefix, not a way to hide failed jobs."""
from copy import deepcopy
import unittest

from prepare_v1_same_build_resume import check_fixture_failure, check_prefix, job_order
from v1_runtime_dependencies import FULL_CDT, FULL_CDT_CASES
from v1_fast_regression_test import fixture
from v1_local_release_workflows import core_jobs


def resume_fixture():
    report=fixture();report['jobs']=[dict(name=n,exit_code=0) for n in job_order()[:32]]
    return (report,dict(status='passed',candidate_test_status='running'),
        dict(status='finished-and-archived',candidate_status='running'),
        dict(status='superseded-test-driver-only',production_changed=False,
             reason_code='sdk-exporter-owned-by-validation-not-worker',
             runtime_correction_status='runtime-preflight-passed-not-release'),'a'*64)


class SameBuildResumeControls(unittest.TestCase):
    def fixture_failure(self):
        report=fixture();report.update(status='failed',
            error='worker-round-1 failed; preserve this candidate and do not retry until green',
            jobs=[dict(name=n,exit_code=0) for n in job_order()[:34]])
        report['jobs'][-1]['exit_code']=201
        log=''.join('f: Failed to open /work/'+FULL_CDT+'\nunknown location(0): fatal error: in "'+n+
            '": std::runtime_error: Caught Unexpected Exception\nTest case "'+n+'" was aborted with:\n' for n in FULL_CDT_CASES)
        log+='Test module "Master Test Suite" has failed with:\n  403 test cases out of 420 passed\n  9 test cases out of 420 failed\n  8 test cases out of 420 skipped\n  9 test cases out of 420 aborted\n  305731 assertions out of 305740 passed\n  9 assertions out of 305740 failed'
        return [report,dict(status='passed',candidate_test_status='failed'),
            dict(status='finished-and-archived',candidate_status='failed'),log,'a'*64]
    def test_exact_fixture_failure_only_reuses_33_green_jobs_and_preserves_failure(self):
        values=self.fixture_failure();before=deepcopy(values)
        prefix=check_fixture_failure(*values)
        self.assertEqual(len(prefix['jobs']),33)
        self.assertTrue(all(j['exit_code']==0 for j in prefix['jobs']))
        self.assertEqual(values,before)
        self.assertEqual(values[0]['jobs'][-1]['exit_code'],201)
    def test_extra_missing_or_different_failure_is_not_classified_as_fixture_only(self):
        for mode in ('extra','missing','different-file','different-case','different-count','green-prefix-failure'):
            values=self.fixture_failure()
            if mode=='extra':values[3]+='\nother.cpp(1): error: unexpected\n'
            if mode=='missing':values[3]=values[3].replace('f: Failed to open','f: Not this',1)
            if mode=='different-file':values[3]=values[3].replace(FULL_CDT,'other.wasm',1)
            if mode=='different-case':values[3]=values[3].replace(FULL_CDT_CASES[0],'other/test',1)
            if mode=='different-count':values[3]=values[3].replace('403 test cases','402 test cases')
            if mode=='green-prefix-failure':values[0]['jobs'][10]['exit_code']=1
            with self.assertRaises(AssertionError):check_fixture_failure(*values)
    def test_unclosed_or_different_failed_boundary_refused(self):
        for mode in ('proof','monitor','later-job','exit','reason','identity'):
            values=self.fixture_failure()
            if mode=='proof':values[1]['candidate_test_status']='passed'
            if mode=='monitor':values[2]['status']='running'
            if mode=='later-job':values[0]['jobs'].append(dict(name='later',exit_code=0))
            if mode=='exit':values[0]['jobs'][-1]['exit_code']=1
            if mode=='reason':values[0]['error']='unknown'
            if mode=='identity':values[-1]='c'*64
            with self.assertRaises(AssertionError):check_fixture_failure(*values)
    def test_order_is_exact_59_job_coverage(self):
        self.assertEqual(len(job_order()),59);self.assertEqual(set(job_order()),core_jobs())
    def test_completed_third_native_boundary_can_resume(self):
        self.assertTrue(check_prefix(*resume_fixture()))
    def test_failure_unrun_duplicate_or_wrong_order_never_reused(self):
        for mode in ('failure','unrun','duplicate','order'):
            values=list(resume_fixture());jobs=values[0]['jobs']
            if mode=='failure':jobs[0]['exit_code']=1
            if mode=='unrun':jobs[0].pop('exit_code')
            if mode=='duplicate':jobs[0]=deepcopy(jobs[1])
            if mode=='order':jobs[0],jobs[1]=jobs[1],jobs[0]
            with self.assertRaises(AssertionError):check_prefix(*values)
    def test_incomplete_native_or_later_sdk_failure_not_a_valid_prefix(self):
        for count in (28,34,55,59):
            values=list(resume_fixture());values[0]['jobs']=[dict(name=n,exit_code=0) for n in job_order()[:count]]
            with self.assertRaises(AssertionError):check_prefix(*values)
    def test_open_unverified_or_failed_parent_closure_refused(self):
        for index,field,value in ((0,'status','failed'),(1,'status','failed'),
                (1,'candidate_test_status','failed'),(2,'status','running'),(2,'candidate_status','failed')):
            values=list(resume_fixture());values[index][field]=value
            with self.assertRaises(AssertionError):check_prefix(*values)
    def test_production_change_unknown_reason_or_uncorrected_runtime_refused(self):
        for field,value in (('production_changed',True),('reason_code','unknown'),
                            ('runtime_correction_status','failed')):
            values=list(resume_fixture());values[3][field]=value
            with self.assertRaises(AssertionError):check_prefix(*values)
    def test_changed_source_or_binary_shape_refused(self):
        values=list(resume_fixture());values[-1]='b'*64
        with self.assertRaises(AssertionError):check_prefix(*values)
        values=list(resume_fixture());values[0]['binaries_sha256'].pop('unittests/native_savanna_test')
        with self.assertRaises(AssertionError):check_prefix(*values)


if __name__=='__main__':unittest.main()
