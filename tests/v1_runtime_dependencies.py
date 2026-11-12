"""Explicit, hashed build-time test data required by relocated native executables."""
import hashlib
from pathlib import Path, PurePosixPath
import re
import shutil


HEADERS = ('test_contracts.hpp', 'snapshots.hpp', 'test-data.hpp', 'deep-mind.hpp')
ABSENT_ABI = {'test-contracts/reject_all/reject_all.abi', 'test-contracts/test_api/test_api.abi'}
FULL_CDT = 'build/unittests/test-contracts/test_api_multi_index/test_api_multi_index.wasm'
FULL_CDT_SHA = '93148d63a7d1b0b864d986cf97fe125136cf2334ba277951d307ab4d16471fac'
FULL_CDT_CASES = (
    'parallel_full_cdt_private_tests/unmodified_full_multi_index_module_is_admitted_without_removing_imports_or_functions',
    *('parallel_full_cdt_oracle_tests/' + name for name in (
        'original_five_family_actions_are_actually_adopted_in_production_and_block_validation',
        'original_post_store_assertion_undo_and_healthy_reuse_keep_full_module_and_original_trace',
        'original_multi_root_full_module_uses_one_bounded_receiver_catalog',
        'original_committed_store_query_and_autoincrement_use_real_sparse_discovery',
        'original_same_batch_store_then_check_uses_current_state_after_failed_private_attempt',
        'original_repeated_secondary_check_rejects_old_success_and_keeps_prior_commit',
        'root_only_capture_keeps_action_limits_and_separate_owned_catalog_budget',
        'original_cdt_sparse_private_discovery_reports_each_owned_request')))


def digest(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def regular(root, relative):
    part = PurePosixPath(relative)
    assert relative == str(part) and not part.is_absolute() and '..' not in part.parts, 'Unsafe dependency path'
    path = root / relative
    assert path.is_file() and not path.is_symlink(), 'Missing/nonregular dependency: ' + relative
    assert path.resolve().is_relative_to(root.resolve()), 'Dependency escapes candidate'
    assert not any(p.is_symlink() for p in path.parents if p != root and p.is_relative_to(root))
    assert path.stat().st_size > 0, 'Empty dependency: ' + relative
    return path


def contract_paths(header):
    assert '"/work/build/unittests/" #D "/" #C "/" #C ".wasm"' in header, 'Unknown compiled fixture layout'
    calls = re.findall(r'^\s*MAKE_READ_WASM_ABI\(\s*(\w+)\s*,\s*([\w.]+)\s*,\s*([\w/-]+)\s*\)', header, re.M)
    assert len(calls) == 33 and len({alias for alias, _, _ in calls}) == 33, 'Unexpected fixture catalog'
    result = set()
    for _, name, folder in calls:
        assert folder in ('contracts', 'test-contracts', 'test-contracts/savanna')
        assert name not in ('.', '..')
        for ext in ('wasm', 'abi'): result.add(f'{folder}/{name}/{name}.{ext}')
    return result


def inventory(root):
    root = Path(root)
    header = regular(root, 'build/unittests/include/test_contracts.hpp')
    paths = contract_paths(header.read_text())
    files = {'build/unittests/include/' + name for name in HEADERS}
    assert ABSENT_ABI <= paths
    for relative in sorted(paths):
        path = root / 'build/unittests' / relative
        if relative in ABSENT_ABI:
            assert not path.exists() and not path.is_symlink(), 'Known unused ABI changed; review catalog explicitly'
        else:
            key = 'build/unittests/' + relative
            regular(root, key); files.add(key)
    # Include extra generated contracts (including intentional negative ABI
    # fixtures), not CMake caches/objects or any node data/wallet directories.
    for folder in ('contracts', 'test-contracts'):
        for path in (root / 'build/unittests' / folder).rglob('*'):
            assert not path.is_symlink(), 'Symlink in fixture tree'
            if path.is_file() and path.suffix in ('.wasm', '.abi'):
                files.add(str(path.relative_to(root)))
    snapshots = root / 'build/unittests/snapshots'
    assert snapshots.is_dir() and not snapshots.is_symlink(), 'Missing snapshot fixture directory'
    snapshot_files = [p for p in snapshots.rglob('*') if p.is_file()]
    assert snapshot_files, 'Empty snapshot fixture catalog'
    files.update(str(p.relative_to(root)) for p in snapshot_files)
    result = {relative: digest(regular(root, relative)) for relative in sorted(files)}
    assert result.get(FULL_CDT) == FULL_CDT_SHA, 'Original full CDT WASM must not be substituted'
    return result


def verify(root, expected):
    assert expected and all(re.fullmatch('[0-9a-f]{64}', d) for d in expected.values())
    assert inventory(root) == expected, 'Missing, changed or unlisted native runtime dependency'


def copy_dependencies(source, target, expected):
    source, target = Path(source), Path(target)
    verify(source, expected)
    for relative, value in expected.items():
        path = target / relative
        assert not path.exists() and not path.is_symlink(), 'Never overwrite candidate dependency'
        path.parent.mkdir(parents=True, exist_ok=True)
        assert path.parent.resolve().is_relative_to(target.resolve()), 'Copy target escapes candidate'
        shutil.copyfile(regular(source, relative), path)
        assert digest(path) == value
    verify(target, expected)
