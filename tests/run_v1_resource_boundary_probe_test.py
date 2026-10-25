"""Pure transport/command controls, not resource-manager execution evidence."""
from pathlib import Path
import unittest

from run_v1_resource_boundary_probe import FAILURES, compile_command, host, link_command, result_cases


class ResourceBoundaryProbeControls(unittest.TestCase):
    def test_compile_preserves_flags_but_not_cached_outputs_or_module_mappers(self):
        entry = dict(file='/work/source/tests/native_policy_boundary_fixture.cpp', command=
            '/usr/bin/c++ -O3 -I/work/source/include -MD -MT old.o -MF old.d -o old.o '
            '-fmodules-ts -fmodule-mapper=old.modmap -fdeps-format=p1689r5 -c '
            '/work/source/tests/native_policy_boundary_fixture.cpp')
        command = compile_command(entry, Path('/probe/test.cpp'), Path('/probe/test.o'))
        self.assertEqual(command, ['/usr/bin/c++', '-O3', '-I/work/source/include',
            '-c', '/probe/test.cpp', '-o', '/probe/test.o'])

    def test_link_never_writes_cached_dependency_file(self):
        original = ': && /usr/bin/c++ -O3 main.o old.o -o unittests/native_savanna_test '
        for dependency in ('-Wl,--dependency-file=unittests/cached/link.d',
                '-Wl,--dependency-file,unittests/cached/link.d'):
            command = link_command(original+dependency+' -latomic libraries/chain/libeosio_chain.a && :',
                Path('/probe/fixed'), [Path('/cache/main.o'), Path('/probe/test.o')])
            self.assertEqual(command, ['/usr/bin/c++', '-O3', '-latomic', '/cache/main.o',
                '/probe/test.o', 'libraries/chain/libeosio_chain.a', '-o', '/probe/fixed'])

    def test_missing_or_ambiguous_cached_target_refused(self):
        command = '/usr/bin/c++ main.o -o unittests/native_savanna_test lib.a '
        for text in ('', command+'\n'+command):
            with self.assertRaises(AssertionError):
                link_command(text, Path('/probe/out'), [])

    def test_original_failure_names_are_actual_new_regressions(self):
        source = (Path(__file__).parents[1]/'unittests/resource_limits_test.cpp').read_text()
        self.assertEqual(len(FAILURES), 2)
        for name in FAILURES:
            self.assertIn('BOOST_FIXTURE_TEST_CASE('+name.split('/')[1]+',', source)
        self.assertIn('representable_pending_limits_keep_original_exact_formula', source)

    def test_out_of_scope_attempts_rejected_before_transport(self):
        for attempt in (0, 10, '1'):
            with self.assertRaises(AssertionError):
                host('before', attempt)

    def test_aborted_exception_is_not_misread_as_missing_failure(self):
        failed, passed = result_cases('Test suite "resource_limits_test" has failed with:\n'
            'Test case "resource_limits_test/overflow" was aborted with:\n'
            'Test case "resource_limits_test/other" has failed with:\n'
            'Test case "resource_limits_test/ordinary" has passed with:\n')
        self.assertEqual(failed, {'resource_limits_test/overflow', 'resource_limits_test/other'})
        self.assertEqual(passed, {'resource_limits_test/ordinary'})


if __name__ == '__main__':
    unittest.main()
