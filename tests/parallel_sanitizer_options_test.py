"""Exercise the actual CMake profile, compiler and ELF dependency inspection."""
from pathlib import Path
import importlib.util
import json
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "sanitizer_dependency_check", ROOT / "scripts/parallel_sanitizer_check.py")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class SanitizerOptimizationTests(unittest.TestCase):
    def project(self, directory):
        source = directory / "source"
        source.mkdir()
        (source / "main.cpp").write_text(
            "int main(int argc,char**){volatile int n=argc;int v=n+1;return v==2?0:1;}\n")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(profile_probe LANGUAGES CXX)\n"
            "include(\"" + str(ROOT / "cmake/ParallelSanitizerOptions.cmake") + "\")\n"
            "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
            "add_executable(probe main.cpp)\n"
            "target_compile_options(probe PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} "
            "-g1 -fno-omit-frame-pointer -fsanitize=address,undefined)\n"
            "target_link_options(probe PRIVATE -fsanitize=address,undefined)\n")
        return source

    def test_both_profiles_really_compile_link_and_run_with_instrumentation(self):
        with tempfile.TemporaryDirectory(prefix="aic-sanitizer-profile-") as tmp:
            directory = Path(tmp)
            source = self.project(directory)
            for level in (1, 2):
                with self.subTest(level=level):
                    build = directory / str(level)
                    subprocess.run([
                        "cmake", "-G", "Ninja", "-S", str(source), "-B", str(build),
                        "-DCMAKE_BUILD_TYPE=Release",
                        f"-DPUREEOS_PARALLEL_SANITIZER_OPTIMIZATION={level}"],
                        check=True, capture_output=True, timeout=60)
                    command = json.loads((build / "compile_commands.json").read_text())[0]
                    argv = command.get("arguments") or shlex.split(command["command"])
                    optimizations = [v for v in argv if v.startswith("-O")]
                    self.assertEqual(optimizations[-1], f"-O{level}")
                    self.assertIn("-fsanitize=address,undefined", argv)
                    subprocess.run(["cmake", "--build", str(build), "--parallel", "1"],
                                   check=True, capture_output=True, timeout=60)
                    binary = build / "probe"
                    self.assertEqual(CHECKER.inspect(binary, "address,undefined")["runtimes"],
                                     ["asan", "ubsan"])
                    subprocess.run([str(binary)], check=True, capture_output=True, timeout=30)

    def test_invalid_levels_are_rejected_before_compilation(self):
        with tempfile.TemporaryDirectory(prefix="aic-sanitizer-invalid-") as tmp:
            directory = Path(tmp)
            source = self.project(directory)
            for number, level in enumerate(("", "0", "3", "-O2", "2;3", "2 -fsanitize=none")):
                with self.subTest(level=level):
                    result = subprocess.run([
                        "cmake", "-G", "Ninja", "-S", str(source), "-B", str(directory / str(number)),
                        f"-DPUREEOS_PARALLEL_SANITIZER_OPTIMIZATION={level}"],
                        capture_output=True, text=True, timeout=60)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("must be 1 or 2", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
