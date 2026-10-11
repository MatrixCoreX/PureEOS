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


SCHEMA_VERSION = 1
SOURCE_ROOTS = (
    "libraries/chain",
    "libraries/chaindb",
    "plugins/chain_plugin",
    "plugins/producer_plugin",
    "plugins/state_history_plugin",
    "plugins/trace_api_plugin",
)
REQUIRED_FILES = (
    "libraries/chain/controller.cpp",
    "libraries/chain/host_context.cpp",
    "libraries/chain/webassembly/runtimes/eos-vm.cpp",
)
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
    for match in HOST_REGISTRATION.finditer(code):
        registrations.append({
            "line": bisect_right(newlines, match.start()) + 1,
            "macro": match.group(1),
            "name": match.group(2),
            "preconditions": re.findall(r"\b[A-Za-z_]\w*\b", match.group(3)),
            "speculation_review": "pending",
        })
    return sorted(accesses, key=lambda item: (item["line"], item["kind"], item["operation"])), registrations


def review_area(path):
    """Ownership hint only; a known area is NOT a reviewed/safe access."""
    if path.startswith("libraries/chaindb/"):
        return "database_indexes_and_undo"
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
    for directory in SOURCE_ROOTS:
        for path in (root / directory).rglob("*"):
            relative = path.relative_to(root)
            if any(part in EXCLUDED_PARTS or part.lower() == "build" or
                   part.lower().startswith(("build-", "build_")) for part in relative.parts[:-1]):
                continue
            if path.suffix not in {".cpp", ".hpp", ".h", ".ipp", ".tpp", ".inl"} or not path.is_file():
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
    return {
        "schema_version": SCHEMA_VERSION,
        "scanner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scope": list(SOURCE_ROOTS),
        "limitations": "Lexical review aid only; not exhaustive alias/macro/dataflow analysis. No runtime paths are approved.",
        "summary": {"source_files": len(files),
                    "files_with_candidates": sum(bool(f["state_access_candidates"]) for f in files),
                    "candidate_counts": dict(sorted(counts.items())),
                    "host_registrations": sum(len(f["host_registrations"]) for f in files),
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
            return 0 if comparison["matches"] else 1
        print(json.dumps(inventory if args.format == "json" else inventory["summary"],
                         indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print(f"parallel access audit: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
