#!/usr/bin/env python3
"""Tests for the PX0 read-only source inventory; no nodeos or socket needed."""

from contextlib import redirect_stderr, redirect_stdout
import copy
import importlib.util
import io
import json
import re
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "parallel_access_audit", Path(__file__).resolve().parents[1] / "scripts/parallel_access_audit.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ScannerTests(unittest.TestCase):
    def test_denied_import_fixture_covers_current_production_inventory(self):
        root = Path(__file__).resolve().parents[1]
        inventory = audit.build_inventory(root)
        production = {host["name"] for item in inventory["files"] for host in item["host_registrations"]}
        fixture = (root / "unittests/parallel_audited_intrinsics.hpp").read_text()
        declared = set(re.findall(r'^\s+"([A-Za-z_]\w*)",$', fixture, re.MULTILINE))
        self.assertEqual(declared, production)
        self.assertEqual(declared, set(audit.host_classifications()))
        implementation = (root / "libraries/chain/parallel_wasm_worker.cpp").read_text()
        supported = set(re.findall(r'^\s+ISOLATED_HOST\((\w+)\);$', implementation, re.MULTILINE))
        tests = (root / "unittests/parallel_wasm_worker_tests.cpp").read_text()
        allowlist = tests.split("const std::set<std::string_view> supported{", 1)[1].split("};", 1)[0]
        expected = set(re.findall(r'"(\w+)"', allowlist))
        self.assertEqual(supported, expected)
        self.assertEqual(len(supported), 27)
        fallback = set(re.findall(r'^\s+ISOLATED_FALLBACK\((\w+)\);$', implementation, re.MULTILINE))
        fallback_list = tests.split("const std::set<std::string_view> fallback_only{", 1)[1].split("};", 1)[0]
        self.assertEqual(fallback, set(re.findall(r'"(\w+)"', fallback_list)))
        self.assertEqual(len(fallback), 7)
        self.assertFalse(supported & fallback)
        self.assertTrue((supported | fallback) <= production)
        # Every link-only host also has a correctly typed executable fixture;
        # simply excluding its name from the denied-import loop is not enough.
        stub_cases = tests.split("const std::array fallback_imports{", 1)[1].split("\n};", 1)[0]
        self.assertEqual(fallback, set(re.findall(r'fallback_import\{"(\w+)"', stub_cases)))

    def test_comments_strings_and_raw_strings_are_not_accesses(self):
        source = '''// db.modify(fake);
/* db.create<fake>(); */
auto a = "db.find<fake>();";
auto b = R"tag(db.remove(fake); "quoted")tag";
auto c = '\\'';
db.modify(real);
'''
        accesses, hosts = audit.scan_source(source)
        self.assertEqual(accesses, [{"line": 6, "kind": "possible_write", "operation": "modify"}])
        self.assertEqual(hosts, [])

    def test_multiline_comment_and_line_splice_keep_line_numbers(self):
        source = "/* ignored\n db.find(no); */\n// ignore\\\n db.remove(no);\nptr->get_index<index>();"
        accesses, _ = audit.scan_source(source)
        self.assertEqual(accesses, [{"line": 5, "kind": "possible_read", "operation": "get_index"}])

    def test_database_alias_member_calls_are_candidates(self):
        accesses, _ = audit.scan_source("alias.template create<T>(); ptr -> find<T>(); control.mutable_db(); chainbase::database& db;")
        self.assertEqual({a["kind"] for a in accesses},
                         {"possible_write", "possible_read", "state_accessor", "database_handle"})

    def test_ranges_sessions_and_inactive_branches(self):
        accesses, _ = audit.scan_source("#if 0\nidx.lower_bound(k); db.start_undo_session(true);\n#endif")
        self.assertEqual([a["operation"] for a in accesses], ["lower_bound", "start_undo_session"])

    def test_host_macro_definitions_ignored_and_calls_pending(self):
        source = """#define REGISTER_HOST_FUNCTION(NAME, ...) ignored
REGISTER_LEGACY_HOST_FUNCTION(db_store_i64, read_only_check);
REGISTER_CF_HOST_FUNCTION(
    unknown_new_intrinsic);
// REGISTER_HOST_FUNCTION(fake);
"""
        _, registrations = audit.scan_source(source)
        self.assertEqual([r["name"] for r in registrations], ["db_store_i64", "unknown_new_intrinsic"])
        self.assertEqual(registrations[0]["preconditions"], ["read_only_check"])
        self.assertTrue(all(r["speculation_review"] == "pending" for r in registrations))
        self.assertEqual(registrations[1]["access_class"], "unclassified")
        self.assertTrue(all(r["initial_route"] == "serial_only" for r in registrations))

    def test_host_classes_are_explicit_unique_and_not_runtime_approval(self):
        categories = audit.host_classifications()
        self.assertEqual(len(categories), 262)
        self.assertEqual(categories["recover_key"], "tracked_state_read")
        self.assertEqual(categories["db_update_i64"], "private_state_write_with_ram_and_lifecycle")
        self.assertNotIn("db_idx64_new_operation", categories)
        _, hosts = audit.scan_source("REGISTER_HOST_FUNCTION(db_idx64_new_operation);")
        self.assertEqual(hosts[0]["access_class"], "unclassified")

    def test_mask_preserves_offsets_and_digit_separators(self):
        source = 'auto n=1\'000; /* hi\nthere */ db.find<T>(); auto s="escape\\\"quote";'
        masked = audit.mask_non_code(source)
        self.assertEqual(len(masked), len(source))
        self.assertEqual(masked.count("\n"), source.count("\n"))
        self.assertEqual(audit.scan_source(source)[0][0]["operation"], "find")


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pureeos-px0-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for directory in audit.SOURCE_ROOTS:
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        for filename in audit.REQUIRED_FILES:
            self.write(filename, "// fixture\n")
        self.write("libraries/chain/host_context.cpp", "db.find<T>();\n")
        self.write("libraries/chain/webassembly/runtimes/eos-vm.cpp",
                   "REGISTER_HOST_FUNCTION(db_get_i64);\n")

    def write(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        return path

    def inventory(self):
        return audit.build_inventory(self.root)

    def test_deterministic_and_no_runtime_approval(self):
        first, second = self.inventory(), self.inventory()
        self.assertEqual(first, second)
        self.assertTrue(audit.compare_inventory(first, second)["matches"])
        self.assertEqual(first["summary"]["approved_parallel_paths"], 0)
        self.assertEqual(first["summary"]["host_registrations"], 1)

    def test_detects_added_removed_and_changed_files(self):
        before = self.inventory()
        self.write("libraries/chain/new.cpp", "ptr->modify(x);\n")
        self.write("libraries/chain/host_context.cpp", "db.find<T>(); db.remove(x);\n")
        changes = audit.compare_inventory(before, self.inventory())
        self.assertFalse(changes["matches"])
        self.assertEqual(changes["added"], ["libraries/chain/new.cpp"])
        self.assertEqual(changes["changed"], ["libraries/chain/host_context.cpp"])
        after = self.inventory()
        (self.root / "libraries/chain/new.cpp").unlink()
        self.assertEqual(audit.compare_inventory(after, self.inventory())["removed"], ["libraries/chain/new.cpp"])

    def test_even_zero_hit_body_changes_require_review(self):
        before = self.inventory()
        self.write("libraries/chain/controller.cpp", "some_indirect_effect();\n")
        self.assertEqual(audit.compare_inventory(before, self.inventory())["changed"],
                         ["libraries/chain/controller.cpp"])

    def test_scanner_and_manifest_metadata_changes_fail_check(self):
        actual = self.inventory()
        for key, value in [("scanner_sha256", "different"), ("scope", []), ("summary", {})]:
            with self.subTest(key=key):
                expected = copy.deepcopy(actual)
                expected[key] = value
                result = audit.compare_inventory(expected, actual)
                self.assertFalse(result["matches"])
                self.assertTrue(result["metadata_changed"][key])

    def test_config_builds_tests_and_symlink_escapes(self):
        before = self.inventory()
        self.write("config-dir/config.ini", "private local configuration")
        self.write("libraries/chain/build-local/ignored.cpp", "db.remove(x);")
        self.write("plugins/producer_plugin/test/ignored.cpp", "db.remove(x);")
        self.assertEqual(before, self.inventory())
        with tempfile.TemporaryDirectory(prefix="pureeos-px0-outside-") as outside:
            target = Path(outside) / "external.cpp"
            target.write_text("do_not_read();", encoding="utf-8")
            (self.root / "libraries/chain/escape.cpp").symlink_to(target)
            with self.assertRaisesRegex(ValueError, "leaves repository"):
                self.inventory()

    def test_missing_required_source_is_not_empty_success(self):
        (self.root / audit.REQUIRED_FILES[0]).unlink()
        with self.assertRaisesRegex(ValueError, "missing required source"):
            self.inventory()

    def test_source_name_starting_with_build_is_not_excluded(self):
        self.write("libraries/chain/build_block.cpp", "db.modify(x);")
        self.assertIn("libraries/chain/build_block.cpp", [f["path"] for f in self.inventory()["files"]])

    def test_invalid_schema_and_duplicate_entries_rejected(self):
        actual = self.inventory()
        for malformed in [[], {}, {"schema_version": 999},
                          {"schema_version": audit.SCHEMA_VERSION, "files": "bad"},
                          {"schema_version": audit.SCHEMA_VERSION, "files": [None]},
                          {"schema_version": audit.SCHEMA_VERSION, "files": [{"path": "a"}, {"path": "a"}]}]:
            with self.subTest(manifest=malformed), self.assertRaises(ValueError):
                audit.compare_inventory(malformed, actual)

    def test_c_dependency_state_changes_require_review(self):
        source = "libraries/softfloat/source/softfloat_state.c"
        self.write(source, "uint_fast8_t flags = 0;\n")
        before = self.inventory()
        self.write(source, "THREAD_LOCAL uint_fast8_t flags = 0;\n")
        self.assertEqual(audit.compare_inventory(before, self.inventory())["changed"], [source])

    def test_vm_timeout_and_allocator_changes_require_review_even_without_db_access(self):
        source = "libraries/eos-vm/include/eosio/vm/backend.hpp"
        self.write(source, "module.disable_code();\n")
        before = self.inventory()
        self.write(source, "shared_module.disable_code();\n")
        after = self.inventory()
        self.assertEqual(audit.compare_inventory(before, after)["changed"], [source])
        item = next(item for item in after["files"] if item["path"] == source)
        self.assertEqual(item["review_area"], "vm_module_memory_timeout_and_host_dispatch")
        self.assertEqual(item["state_access_candidates"], [])

    def test_build_definitions_and_active_platform_header_are_fingerprinted(self):
        for source in audit.EXPLICIT_FILES:
            with self.subTest(source=source):
                before = self.inventory()
                self.write(source, "// changed dependency configuration\n")
                self.assertEqual(audit.compare_inventory(before, self.inventory())["changed"], [source])

    def test_explicit_dependency_symlink_cannot_escape(self):
        source = self.root / audit.EXPLICIT_FILES[0]
        source.unlink()
        with tempfile.TemporaryDirectory(prefix="pureeos-px0-dep-") as outside:
            target = Path(outside) / "CMakeLists.txt"
            target.write_text("do_not_read();", encoding="utf-8")
            source.symlink_to(target)
            with self.assertRaisesRegex(ValueError, "leaves repository"):
                self.inventory()

    def test_missing_explicit_dependency_is_not_silently_ignored(self):
        (self.root / audit.EXPLICIT_FILES[-1]).unlink()
        with self.assertRaisesRegex(ValueError, "missing required source"):
            self.inventory()

    def test_cli_summary_json_and_read_only_check(self):
        snapshot = self.write("snapshot.json", json.dumps(self.inventory()))
        for extra in [[], ["--format", "json"], ["--check", str(snapshot)]]:
            with self.subTest(args=extra), redirect_stdout(io.StringIO()) as output:
                self.assertEqual(audit.main(["--root", str(self.root)] + extra), 0)
                self.assertIsInstance(json.loads(output.getvalue()), dict)
        saved = snapshot.read_bytes()
        self.write("libraries/chain/host_context.cpp", "db.modify(x);")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(audit.main(["--root", str(self.root), "--check", str(snapshot)]), 1)
        self.assertEqual(snapshot.read_bytes(), saved)

    def test_cli_bad_json_and_missing_root_return_input_error(self):
        snapshot = self.write("snapshot.json", "{broken")
        with redirect_stderr(io.StringIO()):
            self.assertEqual(audit.main(["--root", str(self.root), "--check", str(snapshot)]), 2)
            self.assertEqual(audit.main(["--root", str(self.root / "missing")]), 2)

    def test_matching_snapshot_does_not_approve_unclassified_host(self):
        self.write("libraries/chain/webassembly/runtimes/eos-vm.cpp", "REGISTER_HOST_FUNCTION(new_host);\n")
        snapshot = self.write("snapshot.json", json.dumps(self.inventory()))
        with redirect_stdout(io.StringIO()) as output:
            self.assertEqual(audit.main(["--root", str(self.root), "--check", str(snapshot)]), 1)
            self.assertTrue(json.loads(output.getvalue())["matches"])


if __name__ == "__main__":
    unittest.main()
