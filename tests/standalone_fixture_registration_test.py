"""Real CMake inventory controls; dedicated macro fixtures must not create phantom unit_test suites."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class StandaloneFixtureRegistration(unittest.TestCase):
    def test_real_cmake_removes_only_macro_guarded_standalone_sources(self):
        root = Path(__file__).resolve().parents[1]
        source = root / 'unittests'
        text = (source / 'CMakeLists.txt').read_text()
        first = text.index('set(PUREEOS_STANDALONE_FIXTURE_SOURCES')
        last = text.index('list(REMOVE_ITEM UNIT_TESTS ${PUREEOS_STANDALONE_FIXTURE_SOURCES})', first)
        snippet = text[first:last] + 'list(REMOVE_ITEM UNIT_TESTS ${PUREEOS_STANDALONE_FIXTURE_SOURCES})\n'
        guarded = {p.name for p in source.glob('*.cpp')
                   if re.search(r'^#ifdef PUREEOS_\w+_FIXTURE\s*$',
                                '\n'.join(p.read_text().splitlines()[:5]), re.M)}
        self.assertEqual(len(guarded), 8)
        self.assertIsNotNone(shutil.which('cmake'))
        with tempfile.TemporaryDirectory(prefix='aic-fixture-cmake-') as temporary:
            directory = Path(temporary)
            script = directory / 'inventory.cmake'
            output = directory / 'inventory.txt'
            script.write_text(f'set(CMAKE_CURRENT_SOURCE_DIR "{source}")\n'
                              'file(GLOB UNIT_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")\n'
                              + snippet + f'file(WRITE "{output}" "${{UNIT_TESTS}}")\n')
            result = subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            remaining = {Path(name).name for name in output.read_text().split(';')}
        self.assertEqual({p.name for p in source.glob('*.cpp')} - remaining, guarded)
        self.assertIn('pureeos_genesis_tests.cpp', remaining)
        self.assertIn('parallel_full_cdt_tests.cpp', remaining)
        self.assertLess(last, text.index('add_executable( unit_test'))
        self.assertLess(last, text.index('foreach(TEST_SUITE ${UNIT_TESTS})'))

    def test_excluded_fixture_sources_keep_dedicated_targets_and_macros(self):
        source = Path(__file__).resolve().parents[1] / 'unittests'
        text = (source / 'CMakeLists.txt').read_text()
        for path in source.glob('*.cpp'):
            match = re.search(r'^#ifdef (PUREEOS_\w+_FIXTURE)\s*$',
                              '\n'.join(path.read_text().splitlines()[:5]), re.M)
            if match:
                self.assertGreaterEqual(text.count(path.name), 2, path.name)
                self.assertIn(match[1], text, path.name)


if __name__ == '__main__':
    unittest.main()
