# CDT parallel execution fixture

This is a test contract, not `eosio.token` and not a deployment artifact. It uses
the ordinary CDT dispatcher, `multi_index`, authorization/checks and C++ dynamic
allocation. `init` creates rows through the original serial path. `run` updates
existing rows, returns the resulting value, and optionally allocates buffers
after the write. A 2 MiB buffer exceeds the private worker's 16-page limit and
must cause whole-attempt fallback, not a second canonical write. Only the first
4 KiB of each buffer is touched/compared: the fixture must not exhaust the
original CPU limit just by interpreting millions of comparisons in serial.

Rebuild from the repository root with CDT 4.1.1:

```sh
cdt-cpp -O2 -abigen -contract parallelcdt \
  -o unittests/test-contracts/parallel_cdt/parallel_cdt.wasm \
  unittests/test-contracts/parallel_cdt/parallel_cdt.cpp
```

The source, ABI and generated WASM are versioned. Tests read this specific WASM
and check its SHA-256; no locally rebuilt system/token contract is substituted.

- Source: `a3d9c898dff36052380e798622d303e798493af03dd1cca2aab2f059e450c2be`
- WASM: `0365f466b059f6985baa7a1aa1415e59139e58bdb10344a9c457b59909fe8de8`
- ABI: `b7490ccb1ee801457a00bdfdbb6c607ee1828918d74b7b89e751c9c19e779c19`

The module has 20 imports, 24 local functions and one initial memory page with
no declared maximum. Several CDT library imports are unused by `run`; linking
them is not permission to execute them. The explicit fallback-only host tests
cover every such registered stub, including calls after private writes and in
module start functions.
