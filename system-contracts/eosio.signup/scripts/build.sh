#!/usr/bin/env bash
set -euo pipefail
signup_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
: "${CDT_ROOT:?Use the verified remote PureEOS CDT toolchain}"
signup_out="${1:?Specify an isolated output directory}"
mkdir -p "$signup_out"
"$CDT_ROOT/bin/cdt-cpp" -contract=eosio.signup -O2 -I"$signup_root/include" \
  -o "$signup_out/eosio.signup.wasm" "$signup_root/src/eosio.signup.cpp"
# This contract has only notifications, no directly callable actions or tables.
# CDT consequently emits no ABI; publish the explicit reviewed notification ABI.
cp "$signup_root/eosio.signup.abi" "$signup_out/eosio.signup.abi"
sha256sum "$signup_out/eosio.signup.wasm" "$signup_out/eosio.signup.abi"
