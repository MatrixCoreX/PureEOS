#!/usr/bin/env python3
"""PX0 review aid, NOT a C++ parser or a parallel-execution allowlist.

Read production source files, inventory possible state accesses and EOS VM host
registrations, and detect drift against a reviewed snapshot. Nothing is enabled
or written by this tool. Aliases, templates, macros and indirect calls still
require manual review; a matching snapshot is not a thread-safety proof.
"""

import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys


SCHEMA_VERSION = 2
SOURCE_ROOTS = (
    "libraries/chain",
    "libraries/chaindb",
    "libraries/softfloat/source",
    "plugins/chain_plugin",
    "plugins/producer_plugin",
    "plugins/state_history_plugin",
    "plugins/trace_api_plugin",
)
EXPLICIT_FILES = (
    "libraries/CMakeLists.txt",
    "libraries/chain/CMakeLists.txt",
    "libraries/softfloat/CMakeLists.txt",
    # This is an active source header, not generated build output.
    "libraries/softfloat/build/Linux-x86_64-GCC/platform.h",
)
REQUIRED_FILES = (
    "libraries/chain/controller.cpp",
    "libraries/chain/host_context.cpp",
    "libraries/chain/webassembly/runtimes/eos-vm.cpp",
) + EXPLICIT_FILES
EXCLUDED_PARTS = {"test", "tests", "CMakeFiles", "Testing", "__pycache__"}

# Keep offsets/newlines so review locations refer to the original source. This
# is deliberately lexical, not preprocessing: inactive #if branches are kept.
NON_CODE = re.compile(
    r'(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)")'
    r'|//(?:\\\r?\n|[^\n])*'
    r'|/\*.*?\*/'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?<![\w'])'(?:\\.|[^'\\\n])'",
    re.DOTALL,
)
ACCESS_PATTERNS = (
    ("database_handle", re.compile(r"\bchainbase\s*::\s*database\b")),
    ("state_accessor", re.compile(
        r"\b(?:mutable_db|db|get_mutable_resource_limits_manager|"
        r"get_resource_limits_manager|get_authorization_manager|"
        r"get_mutable_authorization_manager|get_global_properties|"
        r"get_dynamic_global_properties|get_protocol_state_object|"
        r"active_producers|pending_block_time)\s*\(")),
    ("possible_write", re.compile(
        r"(?:\.|->)\s*(?:template\s+)?"
        r"(?P<operation>create|modify|remove|get_mutable_index)\b")),
    ("possible_read", re.compile(
        r"(?:\.|->)\s*(?:template\s+)?"
        r"(?P<operation>get|find|get_index|lower_bound|upper_bound|equal_range)\b")),
    ("session_or_mode", re.compile(
        r"(?:\.|->)\s*(?:template\s+)?"
        r"(?P<operation>start_undo_session|undo|squash|commit|set_revision|set_read_only_mode)\b")),
)
HOST_REGISTRATION = re.compile(
    r"^[ \t]*(REGISTER_(?:[A-Z]+_)*HOST_FUNCTION)\s*\(\s*"
    r"([A-Za-z_]\w*)\b([^)]*)\)", re.MULTILINE)

# Explicit design classification, NOT a runtime whitelist. New names must be
# reviewed; broad prefix matching would silently classify future intrinsics.
# Even local computation still needs pointer/precondition/timer/TLS auditing.
HOST_GROUPS = {
    "fixed_input_or_context": """get_context_free_data require_auth require_auth2 has_auth
        read_action_data action_data_size current_receiver read_transaction transaction_size
        expiration tapos_block_num tapos_block_prefix get_action current_time publication_time
        get_sender get_block_num""".split(),
    "tracked_state_read": """is_feature_active get_resource_limits get_parameters_packed
        get_wasm_parameters_packed get_blockchain_parameters_packed is_privileged
        get_upgrade_developer get_rotation_context_v1 get_active_producers
        check_transaction_authorization check_permission_authorization get_permission_last_used
        get_account_creation_time is_account get_code_hash is_feature_activated
        assert_recover_key recover_key""".split(),
    "serial_privileged_barrier": """activate_feature preactivate_feature set_resource_limits
        set_parameters_packed set_wasm_parameters_packed set_proposed_producers set_proposed_producers_ex
        set_blockchain_parameters_packed set_privileged set_finalizers set_upgrade_voters""".split(),
    "serial_nested_or_scheduled_barrier": """require_recipient call get_call_return_value get_call_data
        set_call_return_value send_inline send_context_free_inline send_deferred cancel_deferred""".split(),
    "private_output_then_ordered_publication": """set_action_return_value prints prints_l printi printui
        printi128 printui128 printsf printdf printqf printn printhex""".split(),
    "attempt_local_control_or_memory": """abort eosio_assert eosio_assert_message eosio_assert_code eosio_exit
        memcpy memmove memcmp memset""".split(),
    "local_crypto_with_budget_and_dependency_review": """assert_sha256 assert_sha1 assert_sha512 assert_ripemd160
        sha256 sha1 sha512 ripemd160 alt_bn128_add alt_bn128_mul alt_bn128_pair mod_exp blake2_f sha3 k1_recover
        bls_g1_add bls_g2_add bls_g1_weighted_sum bls_g2_weighted_sum bls_pairing bls_g1_map bls_g2_map
        bls_fp_mod bls_fp_mul bls_fp_exp""".split(),
    "attempt_local_compiler_builtins_with_tls": """__ashlti3 __ashrti3 __lshlti3 __lshrti3 __divti3 __udivti3
        __multi3 __modti3 __umodti3 __addtf3 __subtf3 __multf3 __divtf3 __negtf2 __extendsftf2 __extenddftf2
        __trunctfdf2 __trunctfsf2 __fixtfsi __fixtfdi __fixtfti __fixunstfsi __fixunstfdi __fixunstfti
        __fixsfti __fixdfti __fixunssfti __fixunsdfti __floatsidf __floatsitf __floatditf __floatunsitf
        __floatunditf __floattidf __floatuntidf __cmptf2 __eqtf2 __netf2 __getf2 __gttf2 __letf2 __lttf2
        __unordtf2""".split(),
    "attempt_local_softfloat_with_tls": [
        f"_eosio_f{bits}_{op}" for bits in (32, 64)
        for op in ("add", "sub", "div", "mul", "min", "max", "copysign", "abs", "neg", "sqrt",
                   "ceil", "floor", "trunc", "nearest", "eq", "ne", "lt", "le", "gt", "ge")
    ] + ["_eosio_f32_promote", "_eosio_f64_demote"] + [
        f"_eosio_f{bits}_trunc_{integer}" for bits in (32, 64) for integer in ("i32s", "i32u", "i64s", "i64u")
    ] + [f"_eosio_{integer}_to_f{bits}" for bits in (32, 64) for integer in ("i32", "i64", "ui32", "ui64")],
    "tracked_primary_or_range_read": [f"db_{op}_i64" for op in ("get", "next", "previous", "find", "lowerbound", "upperbound", "end")]
        + [f"db_idx{kind}_{op}" for kind in ("64", "128", "256", "_double", "_long_double")
           for op in ("find_secondary", "find_primary", "lowerbound", "upperbound", "end", "next", "previous")],
    "private_state_write_with_ram_and_lifecycle": [f"db_{op}_i64" for op in ("store", "update", "remove")]
        + [f"db_idx{kind}_{op}" for kind in ("64", "128", "256", "_double", "_long_double")
           for op in ("store", "update", "remove")],
}


def host_classifications():
    result = {}
    for category, names in HOST_GROUPS.items():
        for name in names:
            if name in result:
                raise ValueError(f"duplicate host classification: {name}")
            result[name] = category
    return result


def mask_non_code(source):
    return NON_CODE.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), source)


def scan_source(source):
    """Return candidates, never an authorization to speculate."""
    code = mask_non_code(source)
    newlines = [i for i, char in enumerate(code) if char == "\n"]
    accesses = []
    for kind, pattern in ACCESS_PATTERNS:
        for match in pattern.finditer(code):
            operation = match.groupdict().get("operation") or re.sub(r"\s+", "", match.group()).rstrip("(")
            accesses.append({"line": bisect_right(newlines, match.start()) + 1,
                             "kind": kind, "operation": operation})
    registrations = []
    categories = host_classifications()
    for match in HOST_REGISTRATION.finditer(code):
        registrations.append({
            "line": bisect_right(newlines, match.start()) + 1,
            "macro": match.group(1),
            "name": match.group(2),
            "preconditions": re.findall(r"\b[A-Za-z_]\w*\b", match.group(3)),
            "speculation_review": "pending",
            "access_class": categories.get(match.group(2), "unclassified"),
            "initial_route": "serial_only",
        })
    return sorted(accesses, key=lambda item: (item["line"], item["kind"], item["operation"])), registrations


def review_area(path):
    """Ownership hint only; a known area is NOT a reviewed/safe access."""
    if path.startswith("libraries/chaindb/"):
        return "database_indexes_and_undo"
    if path.startswith("libraries/softfloat/"):
        return "floating_point_tls_and_build_contract"
    if path.endswith("CMakeLists.txt"):
        return "build_definitions_and_dependency_contract"
    if path.startswith("plugins/"):
        return "queues_signals_and_external_output"
    filename = Path(path).name
    if filename in {"host_context.cpp", "host_context.hpp", "database.cpp"}:
        return "tables_iterators_and_ram"
    if filename.startswith("authorization_manager") or filename in {"authorization.cpp", "permission.cpp"}:
        return "authorization_and_permission_usage"
    if filename.startswith("resource_limits"):
        return "account_and_block_resource_limits"
    if filename in {"eosio_contract.cpp", "upgrade_governance.cpp", "privileged.cpp"}:
        return "native_privileged_and_governance_barriers"
    if filename.startswith("controller") or filename.startswith("transaction_context"):
        return "execution_commit_and_lifecycle"
    if filename.startswith("apply_context") or filename.startswith("sync_call_context"):
        return "actions_receipts_and_nested_calls"
    if "/runtimes/" in path or "wasm_interface" in filename:
        return "runtime_tls_and_code_cache"
    return "manual_review_required"


def build_inventory(root):
    root = Path(root).resolve()
    for directory in SOURCE_ROOTS:
        if not (root / directory).is_dir():
            raise ValueError(f"missing source directory: {directory}")
    for name in REQUIRED_FILES:
        if not (root / name).is_file():
            raise ValueError(f"missing required source: {name}")

    paths = set()
    for filename in EXPLICIT_FILES:
        path = root / filename
        if not path.resolve().is_relative_to(root):
            raise ValueError(f"source symlink leaves repository: {filename}")
        paths.add(path)
    for directory in SOURCE_ROOTS:
        for path in (root / directory).rglob("*"):
            relative = path.relative_to(root)
            if any(part in EXCLUDED_PARTS or part.lower() == "build" or
                   part.lower().startswith(("build-", "build_")) for part in relative.parts[:-1]):
                continue
            if path.suffix not in {".c", ".cpp", ".hpp", ".h", ".ipp", ".tpp", ".inl"} or not path.is_file():
                continue
            if not path.resolve().is_relative_to(root):
                raise ValueError(f"source symlink leaves repository: {relative.as_posix()}")
            paths.add(path)

    files = []
    for path in sorted(paths):
        data = path.read_bytes()
        accesses, registrations = scan_source(data.decode("utf-8"))
        relative = path.relative_to(root).as_posix()
        files.append({"path": relative, "sha256": hashlib.sha256(data).hexdigest(),
                      "review_area": review_area(relative),
                      "state_access_candidates": accesses, "host_registrations": registrations})

    counts = Counter(a["kind"] for f in files for a in f["state_access_candidates"])
    hosts = [r for f in files for r in f["host_registrations"]]
    return {
        "schema_version": SCHEMA_VERSION,
        "scanner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scope": list(SOURCE_ROOTS),
        "explicit_files": list(EXPLICIT_FILES),
        "limitations": "Lexical review aid only; not exhaustive alias/macro/dataflow analysis. No runtime paths are approved.",
        "summary": {"source_files": len(files),
                    "files_with_candidates": sum(bool(f["state_access_candidates"]) for f in files),
                    "candidate_counts": dict(sorted(counts.items())),
                    "host_registrations": len(hosts),
                    "host_classes": dict(sorted(Counter(r["access_class"] for r in hosts).items())),
                    "approved_parallel_paths": 0},
        "files": files,
    }


def compare_inventory(expected, actual):
    """Compare the complete manifest, including zero-hit files and scanner."""
    if not isinstance(expected, dict) or expected.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported or malformed inventory schema")
    if not isinstance(expected.get("files"), list):
        raise ValueError("inventory files must be a list")
    previous = {}
    for item in expected["files"]:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str) or item["path"] in previous:
            raise ValueError("invalid or duplicate inventory path")
        previous[item["path"]] = item
    current = {item["path"]: item for item in actual["files"]}
    return {
        "matches": expected == actual,
        "added": sorted(current.keys() - previous.keys()),
        "removed": sorted(previous.keys() - current.keys()),
        "changed": sorted(path for path in current.keys() & previous.keys() if current[path] != previous[path]),
        "metadata_changed": {key: expected.get(key) != actual.get(key)
                             for key in (expected.keys() | actual.keys()) - {"files"}
                             if expected.get(key) != actual.get(key)},
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--format", choices=("summary", "json"), default="summary")
    parser.add_argument("--check", type=Path, help="read-only comparison; 1 means source/scanner drift, 2 means input error")
    args = parser.parse_args(argv)
    try:
        inventory = build_inventory(args.root)
        if args.check:
            comparison = compare_inventory(json.loads(args.check.read_text(encoding="utf-8")), inventory)
            print(json.dumps(comparison, indent=2, sort_keys=True))
            return 0 if comparison["matches"] and not inventory["summary"]["host_classes"].get("unclassified", 0) else 1
        print(json.dumps(inventory if args.format == "json" else inventory["summary"],
                         indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print(f"parallel access audit: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
