#!/usr/bin/env python3
"""Fail closed on stale sanitizer executables after object-cache switches.

Checks dynamic GNU ELF runtime dependencies, NOT whole-program instrumentation.
It never executes the inspected program or treats CMake flags as evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def check_dependencies(text, sanitizer):
    if sanitizer not in ("address,undefined", "thread"):
        raise ValueError("Unsupported sanitizer mode")
    needed = re.findall(r"\(NEEDED\)[^\n]*\[([^\]\n]+)\]", text)
    runtimes = {kind for kind in ("asan", "ubsan", "tsan")
                if any(re.fullmatch(r"lib" + kind + r"\.so(?:\.\d+)*", lib) for lib in needed)}
    expected = {"asan", "ubsan"} if sanitizer == "address,undefined" else {"tsan"}
    if runtimes != expected:
        raise RuntimeError(f"Sanitizer runtime mismatch: expected {sorted(expected)}, found {sorted(runtimes)}; "
                           "relink the executable, not just its cached objects")
    return sorted(runtimes)


def inspect(binary, sanitizer):
    path = Path(binary).resolve(strict=True)
    if not path.is_file():
        raise RuntimeError("Expected an ELF executable file")
    # Hash before and after inspection to reject replacement during the check.
    def digest():
        value = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                value.update(chunk)
        return value.hexdigest()
    before = digest()
    result = subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True,
                            check=True, timeout=30)
    runtimes = check_dependencies(result.stdout, sanitizer)
    if digest() != before:
        raise RuntimeError("Executable changed during sanitizer inspection")
    return {"binary": str(path), "sha256": before, "runtimes": runtimes,
            "scope": "dynamic runtime dependencies only; not proof of complete instrumentation"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitizer", choices=("address,undefined", "thread"), required=True)
    parser.add_argument("binary", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        # No success report is emitted until every requested executable passes.
        report = [inspect(binary, args.sanitizer) for binary in args.binary]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Sanitizer runtime check failed: {error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
