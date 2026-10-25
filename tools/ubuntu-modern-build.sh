#!/usr/bin/env bash
# Isolated modern-Ubuntu build; no apt/sudo, existing-build mutation or OC shim.
set -euo pipefail

ubuntu_action=check
ubuntu_mode_seen=false
ubuntu_jobs=2
ubuntu_build_dir=
ubuntu_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
for ubuntu_arg in "$@"; do
  case "$ubuntu_arg" in
    --check|--configure|--build|--test)
      if "$ubuntu_mode_seen"; then printf '%s\n' 'Select exactly one action.' >&2; exit 2; fi
      ubuntu_action=${ubuntu_arg#--}; ubuntu_mode_seen=true ;;
    --jobs=*) ubuntu_jobs=${ubuntu_arg#--jobs=} ;;
    --build-dir=*) ubuntu_build_dir=${ubuntu_arg#--build-dir=} ;;
    --help)
      printf '%s\n' 'Usage: bash tools/ubuntu-modern-build.sh [--check|--configure|--build|--test] [--jobs=2] [--build-dir=PATH]' \
        'Ubuntu 26.04/26.10, x86_64 parallel JIT profile. Default: read-only prerequisite check.' \
        '--build builds nodeos, cleos, keosd and helios-util in the isolated directory.' \
        '--test runs queue, timer and real producer checks; it is not full release acceptance.'
      exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$ubuntu_arg" >&2; exit 2 ;;
  esac
done
if [[ ! "$ubuntu_jobs" =~ ^[1-9][0-9]{0,2}$ ]] || (( ubuntu_jobs > 128 )); then
  printf '%s\n' '--jobs must be an integer between 1 and 128 (compilation jobs, not WASM workers).' >&2
  exit 2
fi
for ubuntu_tool in python3 cmake c++ git; do
  command -v "$ubuntu_tool" >/dev/null || { printf 'Missing prerequisite: %s\n' "$ubuntu_tool" >&2; exit 2; }
done
ubuntu_generator='Unix Makefiles'
if command -v ninja >/dev/null; then ubuntu_generator=Ninja
elif ! command -v make >/dev/null; then printf '%s\n' 'Install ninja-build or make.' >&2; exit 2
fi
ubuntu_release=$(python3 - <<'PY'
import platform
data = platform.freedesktop_os_release()
if data.get('ID') != 'ubuntu' or data.get('VERSION_ID') not in ('26.04', '26.10'):
    raise SystemExit('This profile targets actual Ubuntu 26.04 or 26.10; do not relabel another OS.')
if platform.machine() not in ('x86_64', 'amd64'):
    raise SystemExit('The parallel JIT profile is currently x86_64 only.')
print(data['VERSION_ID'])
PY
)
printf 'Platform: Ubuntu %s, x86_64; profile: parallel JIT, ENABLE_OC=OFF\n' "$ubuntu_release"
printf 'Generator: %s; compilation jobs: %s\n' "$ubuntu_generator" "$ubuntu_jobs"
c++ --version
cmake --version
if command -v cdt-cpp >/dev/null; then cdt-cpp --version; else
  printf '%s\n' 'CDT not on PATH; install an isolated validated CDT to compile contracts.'
fi
[[ "$ubuntu_action" != check ]] || exit 0

ubuntu_build_dir=${ubuntu_build_dir:-"$ubuntu_repo/build/ubuntu-$ubuntu_release-jit"}
ubuntu_build_dir=$(realpath -m -- "$ubuntu_build_dir")
ubuntu_account_home=$(python3 -c 'import os,pwd; print(os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir))')
ubuntu_environment_home=
if [[ -n "${HOME:-}" ]]; then ubuntu_environment_home=$(realpath -m -- "$HOME"); fi
if [[ "$ubuntu_build_dir" == / || "$ubuntu_build_dir" == "$ubuntu_repo" || "$ubuntu_build_dir" == "$ubuntu_repo/build" || "$ubuntu_build_dir" == "$ubuntu_account_home" || "$ubuntu_build_dir" == "$ubuntu_environment_home" ]]; then
  printf '%s\n' 'Use an isolated build subdirectory, not a root or the existing default build.' >&2
  exit 2
fi
if [[ -f "$ubuntu_build_dir/CMakeCache.txt" ]]; then
  ubuntu_cached_node=$(sed -n 's/^NODE_EXECUTABLE_NAME:STRING=//p' "$ubuntu_build_dir/CMakeCache.txt")
  if [[ "$ubuntu_cached_node" != pureeos-ubuntu-nodeos ]]; then
    printf '%s\n' 'Refusing to reconfigure an existing build not owned by this profile.' >&2
    exit 2
  fi
  ubuntu_cached_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$ubuntu_build_dir/CMakeCache.txt")
  if [[ "$ubuntu_cached_source" != "$ubuntu_repo" ]]; then
    printf '%s\n' 'Refusing to reuse a build belonging to a different source tree.' >&2
    exit 2
  fi
  ubuntu_generator=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$ubuntu_build_dir/CMakeCache.txt")
  case "$ubuntu_generator" in Ninja|'Unix Makefiles') ;; *) printf '%s\n' 'Unsupported existing generator.' >&2; exit 2 ;; esac
fi
python3 -I -B "$ubuntu_repo/tools/prepare_vm_vendor.py" --check
cmake -S "$ubuntu_repo" -B "$ubuntu_build_dir" -G "$ubuntu_generator" \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_OC=OFF -DNODE_EXECUTABLE_NAME=pureeos-ubuntu-nodeos
[[ "$ubuntu_action" != configure ]] || exit 0
if [[ "$ubuntu_action" == build ]]; then
  cmake --build "$ubuntu_build_dir" --target pureeos-ubuntu-nodeos cleos keosd helios-util --parallel "$ubuntu_jobs"
else
  cmake --build "$ubuntu_build_dir" --target test_incoming_transaction_batch test_production_timer_guard \
    test_producer_plugin_integration --parallel "$ubuntu_jobs"
  for ubuntu_test in test_incoming_transaction_batch test_production_timer_guard test_producer_plugin_integration; do
    "$ubuntu_build_dir/plugins/producer_plugin/test/$ubuntu_test" --report_level=detailed --color_output=no
  done
fi
