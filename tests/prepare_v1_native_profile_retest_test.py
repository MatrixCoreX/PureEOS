"""Cache-is-not-a-pass and new profile registration controls, no runtime proof."""
from copy import deepcopy
from pathlib import Path
import unittest

from prepare_v1_native_profile_retest import OVERLAY, failed_parent_is_cache_only, failed_parent_jobs
from v1_native_profile_retest_guest import PREFLIGHT, SUITES


def fixture():
    return (dict(status='failed',jobs=[dict(name=n,exit_code=0) for n in failed_parent_jobs()[:-1]]+[
        dict(name='build-version-provenance',exit_code=1)],binaries_sha256={str(n):'sha' for n in range(11)}),
        dict(status='passed',candidate_test_status='failed'),
        dict(status='finished-and-archived',candidate_status='failed'))


class NativeRepairControls(unittest.TestCase):
    def test_failed_parent_only_admitted_as_explicit_cache(self):
        self.assertTrue(failed_parent_is_cache_only(*fixture()))
        for index,key,value in ((0,'status','passed'),(1,'candidate_test_status','passed'),
                (2,'candidate_status','passed'),(2,'status','running')):
            values=list(fixture());values[index][key]=value
            self.assertFalse(failed_parent_is_cache_only(*values))

    def test_unknown_failure_incomplete_or_duplicate_parent_refused(self):
        for mode in ('missing','duplicate','earlier-failed','wrong-stage','missing-binary'):
            a,b,c=fixture()
            if mode=='missing':a['jobs'].pop(0)
            if mode=='duplicate':a['jobs'][1]=deepcopy(a['jobs'][0])
            if mode=='earlier-failed':a['jobs'][0]['exit_code']=1
            if mode=='wrong-stage':a['jobs'][-1]['name']='worker-round-3'
            if mode=='missing-binary':a['binaries_sha256'].pop('0')
            self.assertFalse(failed_parent_is_cache_only(a,b,c))

    def test_20_explicit_overlays_allow_only_resource_fix_and_build_metadata(self):
        self.assertEqual(len(OVERLAY),20);self.assertEqual(len(set(OVERLAY)),20)
        production=[n for n in OVERLAY if not n.startswith(('unittests/','tests/'))]
        self.assertEqual(production,['libraries/chain/resource_limits.cpp','CMakeLists.txt',
            'libraries/version/CMakeLists.txt','libraries/version/include/eosio/version/version.hpp',
            'libraries/version/src/version_impl.cpp.in'])
        self.assertIn('unittests/resource_limits_test.cpp',OVERLAY)

    def test_fourteen_preflights_cover_every_previously_broken_fixture_family(self):
        self.assertEqual(len(PREFLIGHT),14);self.assertEqual(len(set(PREFLIGHT)),14)
        self.assertEqual({n.split('/')[0] for n in PREFLIGHT},
            {'pureeos_governance_contract_tests','pureeos_single_choice_contract_tests',
             'pureeos_resource_split_tests','pureeos_committee_boundary_tests'})
        self.assertEqual(len(SUITES),10)
        source=(Path(__file__).parent/'v1_native_profile_retest_guest.py').read_text()
        self.assertIn('set(PREFLIGHT)<=actual',source)
        self.assertIn("plan['production_resource_cpp_sha256']",source)
        self.assertIn("!= plan['parent_programs_sha256']['nodeos']",source)

    def test_uninitialized_case_is_fresh_profile_not_same_wasm_upgrade(self):
        source=(Path(__file__).parents[1]/'unittests/pureeos_governance_tests.cpp').read_text()
        name='uninitialized_current_profile_rejects_votes_and_initializes_only_once'
        self.assertIn('pureeos_single_choice_contract_tests/'+name,PREFLIGHT)
        self.assertIn('BOOST_AUTO_TEST_CASE('+name+')',source)
        self.assertIn('bool initialize_system = true',source)
        self.assertIn('system contract has already been initialized',source)
        self.assertNotIn('BOOST_AUTO_TEST_CASE(uninitialized_upgrade_cannot_silently_enable_new_policy)',source)

    def test_boundary_cases_verify_current_policy_not_permanent_standbys(self):
        source=(Path(__file__).parents[1]/'unittests/pureeos_committee_boundary_tests.inc').read_text()
        for path in (n for n in PREFLIGHT if n.startswith('pureeos_committee_boundary_tests/')):
            self.assertIn('BOOST_AUTO_TEST_CASE('+path.split('/')[1]+')',source)
        self.assertIn('f.observer_replay()',source)
        self.assertIn('f.check_split()',source)
        self.assertNotIn('no automatic 210 rotation exists',source)

    def test_bootstrap_funding_precedes_seal_but_self_votes_follow_it(self):
        source=(Path(__file__).parents[1]/'unittests/pureeos_governance_tests.cpp').read_text()
        body=source.split('BOOST_AUTO_TEST_CASE(production_bootstrap_and_electorate_ranking)',1)[1]
        body=body.split('BOOST_AUTO_TEST_CASE(',1)[0]
        seal=body.index('f.seal();')
        self.assertLess(body.index('"one-choice electorate"'),seal)
        self.assertGreater(body.index('f.register_node(n, i != 3)'),seal)
        self.assertGreater(body.index('"delegatebw"_n'),seal)
        self.assertIn('unsatisfied_authorization',body)
        self.assertNotIn('set_authority',body)

    def test_rex_fee_helper_cannot_override_current_profile_to_legacy(self):
        source=(Path(__file__).parents[1]/'unittests/pureeos_resource_split_tests.inc').read_text()
        signature=source.split('void check_rex_rental_fee_custody(',1)[1].split('{',1)[0]
        self.assertIn('bool rotation = true',signature)
        for case in ('with_forwarder','empty_treasury','without_forwarder'):
            self.assertIn('pureeos_resource_split_tests/rex_rental_fee_custody_'+case,PREFLIGHT)

    def test_failed_parent_must_have_built_all_11_programs_before_failed_metadata(self):
        self.assertEqual(len(failed_parent_jobs()),3)
        values=list(fixture());values[0]['jobs'][1]['exit_code']=1
        self.assertFalse(failed_parent_is_cache_only(*values))
        self.assertIn('pureeos_single_choice_contract_tests/queued_sale_blocks_clear_and_closed_sale_settles_once_before_switch',PREFLIGHT)
        self.assertIn('pureeos_single_choice_contract_tests/integer_rpc_preserves_above_double_precision_and_pagination',PREFLIGHT)


if __name__=='__main__':unittest.main()
