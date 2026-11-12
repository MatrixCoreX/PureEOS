"""Build the real version library without Git, and verify honest source identity."""
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
VERSION = REPO / 'libraries/version'


class BuildVersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pureeos-version-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21...4.0)\n'
            'project(version_probe LANGUAGES CXX)\n'
            'set(VERSION_MAJOR 2)\nset(VERSION_MINOR 0)\nset(VERSION_PATCH 0)\n'
            'set(VERSION_SUFFIX "")\n'
            'add_subdirectory([==[' + str(VERSION) + ']==] version)\n'
            'add_executable(probe main.cpp)\ntarget_link_libraries(probe PRIVATE version)\n')
        (self.root / 'main.cpp').write_text(
            '#include <eosio/version/version.hpp>\n#include <iostream>\n'
            'int main() { std::cout << eosio::version::version_client() << "\\n"'
            ' << eosio::version::version_full() << "\\n"'
            ' << eosio::version::version_hash() << "\\n"; }\n')

    def command(self, arguments, passed=True):
        result = subprocess.run(arguments, text=True, capture_output=True,
                                cwd=self.root, timeout=90)
        if passed:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def configure(self, source_id=None, passed=True, directory='build'):
        command = ['cmake', '-S', str(self.root), '-B', str(self.root / directory)]
        if source_id is not None:
            command.append('-DPUREEOS_SOURCE_ID=' + source_id)
        return self.command(command, passed)

    def build_and_read(self):
        self.command(['cmake', '--build', str(self.root / 'build'), '--parallel', '1'])
        return self.command([str(self.root / 'build/probe')]).splitlines()

    def test_exported_source_has_exact_version_and_distinct_manifest_identity(self):
        identity = 'a' * 64
        self.configure(identity)
        self.assertEqual(self.build_and_read(), ['v2.0.0', 'v2.0.0-src-' + identity, identity])

    def test_hash_remains_raw_hex_for_nodeos_numeric_version_consumer(self):
        identity = 'a' * 64
        self.configure(identity)
        values = self.build_and_read()
        self.assertEqual(values[2], identity)
        self.assertEqual(int(values[2][:8], 16), 0xaaaaaaaa)

    def test_unidentified_archive_is_explicit_not_a_fake_git_commit(self):
        self.configure()
        self.assertEqual(self.build_and_read(), ['v2.0.0', 'v2.0.0-unidentified-source', ''])

    def test_invalid_source_identifiers_fail_before_build(self):
        for index, identity in enumerate(('a' * 63, 'a' * 65, 'A' * 64, 'g' * 64,
                                          'a' * 32 + ';' + 'a' * 31, 'a' * 63 + '\n')):
            with self.subTest(identity=identity):
                text = self.configure(identity, passed=False, directory='bad-' + str(index))
                self.assertIn('lowercase 64-character SHA256', text)

    def test_git_checkout_cannot_claim_exported_manifest_provenance(self):
        (self.root / '.git').mkdir()
        text = self.configure('a' * 64, passed=False)
        self.assertIn('Git checkouts use their real commit', ' '.join(text.split()))

    def test_real_git_metadata_and_dirty_marker_are_retained(self):
        modules = self.root / 'CMakeModules'
        modules.mkdir()
        source = subprocess.check_output(['git', '-C', str(REPO), 'show',
                                          'HEAD:CMakeModules/VersionUtils.cmake'])
        (modules / 'VersionUtils.cmake').write_bytes(source)
        self.command(['git', 'init', '--quiet'])
        (self.root / 'README').write_text('Fixture only\n')
        self.command(['git', 'add', 'CMakeLists.txt', 'main.cpp', 'CMakeModules/VersionUtils.cmake', 'README'])
        self.command(['git', '-c', 'user.name=PureEOS Test', '-c', 'user.email=test@example.invalid',
                      '-c', 'commit.gpgsign=false', 'commit', '--quiet', '-m', 'Disposable version fixture'])
        commit = self.command(['git', 'rev-parse', 'HEAD']).strip()
        self.configure()
        self.assertEqual(self.build_and_read(), ['v2.0.0', 'v2.0.0-' + commit, commit])
        (self.root / 'README').write_text('Fixture changed\n')
        self.assertEqual(self.build_and_read(), ['v2.0.0', 'v2.0.0-' + commit + '-dirty', commit])

    def test_root_release_version_is_2_0_0_without_inherited_dev_suffix(self):
        source = (REPO / 'CMakeLists.txt').read_text()
        for line in ('set(VERSION_MAJOR 2)', 'set(VERSION_MINOR 0)',
                     'set(VERSION_PATCH 0)', 'set(VERSION_SUFFIX "")'):
            self.assertIn(line, source)


if __name__ == '__main__':
    unittest.main()
