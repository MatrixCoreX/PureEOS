"""Filesystem-copy, completeness and fail-fast controls; no chain evidence."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import v1_runtime_dependencies as deps


class RuntimeDependencyControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pureeos-deps-'); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'source'; self.root.mkdir()
        self.paths = {'contracts/token/token.wasm', 'contracts/token/token.abi', *deps.ABSENT_ABI}
        for relative, data in {
            **{'build/unittests/include/' + n: b'fixture header' for n in deps.HEADERS},
            'build/unittests/contracts/token/token.wasm': b'\0asmfixture',
            'build/unittests/contracts/token/token.abi': b'{}',
            deps.FULL_CDT: b'original full CDT fixture',
            'build/unittests/snapshots/snapshot.bin.gz': b'fixture snapshot',
        }.items():
            p = self.root / relative; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
        self.header = patch.object(deps, 'contract_paths', return_value=self.paths); self.header.start(); self.addCleanup(self.header.stop)
        self.full = patch.object(deps, 'FULL_CDT_SHA', deps.digest(self.root / deps.FULL_CDT)); self.full.start(); self.addCleanup(self.full.stop)

    def test_copy_recreates_exact_data_and_headers_without_overwriting(self):
        expected = deps.inventory(self.root); target = Path(self.temp.name) / 'new'; target.mkdir()
        deps.copy_dependencies(self.root, target, expected)
        self.assertEqual(deps.inventory(target), expected)
        with self.assertRaises(AssertionError): deps.copy_dependencies(self.root, target, expected)

    def test_missing_wasm_abi_header_or_snapshot_fails_before_execution(self):
        expected = deps.inventory(self.root)
        for relative in ('build/unittests/contracts/token/token.wasm', 'build/unittests/contracts/token/token.abi',
                         'build/unittests/include/test_contracts.hpp', 'build/unittests/snapshots/snapshot.bin.gz'):
            path = self.root / relative; data = path.read_bytes(); path.unlink()
            with self.assertRaises(AssertionError): deps.verify(self.root, expected)
            path.write_bytes(data)

    def test_changed_empty_extra_and_wrong_full_module_refused(self):
        expected = deps.inventory(self.root); path = self.root / 'build/unittests/contracts/token/token.abi'
        for data in (b'changed', b''):
            path.write_bytes(data)
            with self.assertRaises(AssertionError): deps.verify(self.root, expected)
        path.write_bytes(b'{}')
        extra = path.with_name('extra.abi'); extra.write_bytes(b'{}')
        with self.assertRaises(AssertionError): deps.verify(self.root, expected)
        extra.unlink(); (self.root / deps.FULL_CDT).write_bytes(b'wrong module')
        with self.assertRaises(AssertionError): deps.inventory(self.root)

    def test_symlinks_and_path_traversal_refused(self):
        path = self.root / 'build/unittests/contracts/token/token.abi'; path.unlink(); path.symlink_to(self.root / deps.FULL_CDT)
        with self.assertRaises(AssertionError): deps.inventory(self.root)
        for relative in ('../outside', '/absolute', 'a/../b'):
            with self.assertRaises(AssertionError): deps.regular(self.root, relative)

    def test_two_unused_abis_are_explicit_not_a_general_missing_file_exception(self):
        path = self.root / 'build/unittests' / sorted(deps.ABSENT_ABI)[0]
        path.parent.mkdir(parents=True); path.write_bytes(b'{}')
        with self.assertRaises(AssertionError): deps.inventory(self.root)

    def test_actual_header_layout_parses_all_33_contracts(self):
        self.header.stop()
        text = (Path(__file__).resolve().parents[1] / 'unittests/test_contracts.hpp.in').read_text()
        text = text.replace('${CMAKE_BINARY_DIR}', '/work/build')
        paths = deps.contract_paths(text)
        self.assertEqual(len(paths), 66)
        self.assertIn('test-contracts/test_api_multi_index/test_api_multi_index.wasm', paths)
        with self.assertRaises(AssertionError): deps.contract_paths(text.replace('/work/build', '/elsewhere'))


if __name__ == '__main__': unittest.main()
