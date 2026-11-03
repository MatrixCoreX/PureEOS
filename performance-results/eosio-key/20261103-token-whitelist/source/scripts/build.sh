#!/usr/bin/env bash
set -euo pipefail
key_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
: "${CDT_ROOT:?Set CDT_ROOT to the verified PureEOS CDT 1.0.0 directory}"
key_out="${1:-$key_root/artifacts}"
mkdir -p "$key_out"
key_temp="$(mktemp -d "$key_out/.build.XXXXXX")"
trap 'rm -rf -- "$key_temp"' EXIT
cd "$key_root"
python3 -B scripts/generate_allowlist.py token_allowlist.json "$key_temp/token_allowlist.hpp"
"$CDT_ROOT/bin/cdt-cpp" -abigen -contract=eosio.key -O2 -Iinclude -I"$key_temp" \
  -o "$key_temp/eosio.key.wasm" src/eosio.key.cpp
test -s "$key_temp/eosio.key.abi"
if [[ -f abi/eosio.key.abi && "${KEY_UPDATE_ABI:-0}" != 1 ]]; then cmp -- abi/eosio.key.abi "$key_temp/eosio.key.abi"; fi
mv -- "$key_temp/eosio.key.wasm" "$key_out/eosio.key.wasm"
mv -- "$key_temp/eosio.key.abi" "$key_out/eosio.key.abi"
cp -- token_allowlist.json "$key_out/token_allowlist.json"
sha256sum "$key_out/eosio.key.wasm" "$key_out/eosio.key.abi"
if [[ "${KEY_BUILD_FIXTURES:-0}" == 1 ]]; then
  mkdir -p "$key_out/test-only-system" "$key_out/test-only-wallet" "$key_out/test-only-token" "$key_out/test-only-recipient"
  "$CDT_ROOT/bin/cdt-cpp" -abigen -contract=keysysmockup -O2 -Iinclude \
    -o "$key_out/test-only-system/keysysmockup.wasm" tests/fixtures/system_mock.cpp
  "$CDT_ROOT/bin/cdt-cpp" -abigen -contract=eosio.key -O2 -Iinclude -I"$key_temp" '-DKEY_SYSTEM_ACCOUNT="keysysmockup"' \
    -o "$key_out/test-only-wallet/eosio.key.wasm" src/eosio.key.cpp
  "$CDT_ROOT/bin/cdt-cpp" -abigen -contract=keybadtokenx -O2 \
    -o "$key_out/test-only-token/keybadtokenx.wasm" tests/fixtures/badtoken.cpp
  "$CDT_ROOT/bin/cdt-cpp" -abigen -contract=keyramattack -O2 \
    -o "$key_out/test-only-recipient/keyramattack.wasm" tests/probes/ram_authority.cpp
fi
