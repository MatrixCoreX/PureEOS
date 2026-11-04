# 部署与验证 / Deployment and qualification

2026-11-04 RAM 市场计费候选 / market-priced RAM candidate: see [规则、公式与兼容 / rules and compatibility](RAM_MARKET_FEES.md). New source prices approved bytes from current RAM reserves; old deployed hashes retain fixed row pricing. Public activation is not included in this development task.


2026-11-03 最新范围：测试白名单为系统 AIC 与标准 `keyusdtoken1::4,USD`，绑定完整币种和审核代码哈希；后续新增通过受保护 eosio.key 代码的原生七天开发者提案/BP 否决流程。第三方文本收款账号必须先付 RAM open。RAM 登记与白名单相互独立；独立远端正常系统74项、替代表夹具74项与SDK/中继14项通过，现已部署公开测试链；旧风险复现保留，稳定版本未更新。详见 [白名单提案流程](TOKEN_ALLOWLIST.md)。

此分支实现独立合约与客户端，不替换 eosio.system，不修改 nodeos，也不自动升级当前公开演示链。已封存治理的链禁止直接新增 eosio.* 短账号。当前验证使用独立新测试链，在治理封存前创建 eosio.key；不要绕过既有保护。

This branch adds a contract/client, without replacing eosio.system, changing nodeos or automatically upgrading the public demo. Sealed governance prohibits direct creation of new eosio.* accounts. Qualification uses a new isolated chain and provisions eosio.key before sealing; do not bypass existing protections.

## Build

Use the verified PureEOS CDT 1.0.0 package. Build the contract on the selected remote test host, in an independent directory. Use low-priority bounded compilation; no TPS workload or existing-node changes are needed.

```bash
export CDT_ROOT=/path/to/pureeos-cdt-1.0.0
bash scripts/build.sh ./artifacts
sha256sum artifacts/eosio.key.wasm artifacts/eosio.key.abi
npm ci --ignore-scripts
npm test

# Only when running the supplementary fixture suite:
KEY_BUILD_FIXTURES=1 bash scripts/build.sh ./artifacts
```

`artifacts/eosio.key.wasm` is the normal build using real system tables; the AIC/USD reviewed-code profile passed remote qualification and is deployed on the public TEST chain. Arbitrary issuer code remains excluded. The `test-only-*` directories are incompatible test/alternate-system builds and must never be substituted into a deployment package. The parent repository's normal CMake `cleos` target compiles the native client additions in `programs/cleos/key_wallet.hpp`; it adds no runtime dependency on Node.js to cleos.

## Bootstrap before governance sealing

Keep the qualified system WASM bytes and existing selected modes: single-choice voting, producer/finalizer/rotation selection, BP bonds and seven-day cooldown, maintenance/blacklist, 10%/90% resources, reward schedule, seven-day governance, RAM64GiB/setramrate1024 and both disabled deferred stages. Test-only CPU4/NET1MiB/4GiB-state settings belong to a separate TEST package; they do not change production defaults.

The current schema pin is the qualified system WASM SHA-256 `b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170`. Its PowerUp state version is **1** (PureEOS 10%/90% resource split); inner CPU/NET resource versions are **0**. Treat a different system hash/schema as an adaptation requiring review, rather than accepting upstream version-0 behavior silently.

```bash
# After eosio.system/PowerUp initialization, BEFORE governance seal.
# New accounts have no initial RAM: create and fund in one atomic transaction.
cleos push transaction '{"actions":[
 {"account":"eosio","name":"newaccount","authorization":[{"actor":"eosio","permission":"active"}],
  "data":{"creator":"eosio","name":"eosio.key",
   "owner":{"threshold":1,"keys":[{"key":"OWNER_PUBLIC_KEY","weight":1}],"accounts":[],"waits":[]},
   "active":{"threshold":1,"keys":[{"key":"ACTIVE_PUBLIC_KEY","weight":1}],"accounts":[],"waits":[]}}},
 {"account":"eosio","name":"buyram","authorization":[{"actor":"eosio","permission":"active"}],
  "data":{"payer":"eosio","receiver":"eosio.key","quant":"100.0000 AIC"}}
]}' --expiration 1800

# Allocate self-owned resources before EOSKEY becomes code-only. The funding
# and stake are atomic and leave ZERO liquid AIC, as required by init.
cleos push transaction '{"actions":[
 {"account":"eosio.token","name":"transfer","authorization":[{"actor":"eosio","permission":"active"}],
  "data":{"from":"eosio","to":"eosio.key","quantity":"110.0000 AIC","memo":"bootstrap own resources"}},
 {"account":"eosio","name":"delegatebw","authorization":[{"actor":"eosio.key","permission":"active"}],
  "data":{"from":"eosio.key","receiver":"eosio.key","stake_net_quantity":"10.0000 AIC","stake_cpu_quantity":"100.0000 AIC","transfer":false}}
]}' --expiration 1800

# Contract-only active permission; use owner for this initial deployment/init.
cleos set account permission eosio.key active \
  '{"threshold":1,"keys":[],"accounts":[{"permission":{"actor":"eosio.key","permission":"eosio.code"},"weight":1}],"waits":[]}' \
  owner -p eosio.key@owner
cleos set contract eosio.key artifacts eosio.key.wasm eosio.key.abi -p eosio.key@owner
cleos push action eosio.key init init.json -p eosio.key@owner
```

Construct `init.json` from the actual RPC chain ID, a fresh public 32-byte deployment salt, actual system hash and reviewed fee policy. `fee_contract` is the AIC issuer interface, not custody. Example **functional-test parameters only**:

```json
{
  "chain_id": "ACTUAL_64_HEX_CHAIN_ID",
  "deployment": "FRESH_64_HEX_DOMAIN_SALT",
  "fee_contract": "eosio.token",
  "fee_symbol": "4,AIC",
  "system_hash": "b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170",
  "policy": {
    "minimum_fee": 100, "unit_fee": 10, "new_row_fee": 500,
    "maximum_multiplier_bps": 100000, "epoch_seconds": 1,
    "max_wallets": 10, "max_balances": 30, "max_assets": 10,
    "max_native_payers": 20
  }
}
```

The deployment salt is public, distinct across deployments and immutable after initialization. Verify config.chain_id against get_info. The wallet must have no pre-existing unattributed AIC balance. Initialize PowerUp first. Buy enough real RAM for reviewed row limits plus outgoing token balance rows and safety reserve; the example RAM purchase and tiny test caps are not a production capacity estimate. If contract RAM is scarce, recipients can pre-open supported token balance rows with their own RAM before withdrawal.

Seal governance through the existing procedure. Confirm eosio.key is protected, owner/active deployment keys cannot directly move funds or update code, the deployed code hash equals the qualified artifact, and global2.new_ram_per_block is 1024. Later wallet upgrades go through existing governance; V1 has no admin sweep or unlocked network-fee payout.

## Paid token RAM admission

Register `eosio.token::4,AIC` before accepting principal or fee credit; every other
issuer/symbol/precision follows the same rule. Use the atomic `buyram` + `regtoken`
example in [CLEOS.md](CLEOS.md). The initialization RAM purchase covers deployment
and configuration but grants no asset budget. Only matching new native purchase
receipts grant token capacity. Any signing native sponsor may top up without
granting account permissions to eosio.key. Transfer senders additionally pay new
recipient row fees; received principal stays whole.

Asset RAM fields change the development table schema. Use a fresh isolated
deployment; an existing deployment needs a reviewed migration. Owner signing
protocol stays at version 2. Paid admission does not resolve malicious issuer
authority over shared custody RAM. The separate qualified code-hash whitelist excludes arbitrary issuer programs.

## BP relay provisioning

A registered BP must have a normal 10000 AIC bond and no maintenance/blacklist flag. Use a newly generated separate relay key imported into the BP's local keosd; do not reuse account, production or finalizer keys.

```bash
cleos set account permission keyrelaymoon keyrelay RELAY_PUBLIC_KEY active -p keyrelaymoon
cleos set action permission keyrelaymoon eosio.key ksend keyrelay -p keyrelaymoon
cleos set action permission keyrelaymoon eosio.key withdraw keyrelay -p keyrelaymoon
cleos set action permission keyrelaymoon eosio.key cancel keyrelay -p keyrelaymoon
cleos push action eosio.key regrelay \
  '{"bp":"keyrelaymoon","permission":"keyrelay","endpoint":"https://YOUR-RELAY"}' -p keyrelaymoon
```

The reference service additionally checks a single-key child permission whose parent is active. It requires an explicit qualified wallet hash and a private key **file**, not a command-line private key:

```bash
export NODEOS_URL=https://YOUR-TRUSTED-RPC
export BP_ACCOUNT=keyrelaymoon BP_PERMISSION=keyrelay
export RELAY_KEY_FILE=/private/keyrelay.key
export RELAY_STATE=/private/keyrelay-state
export KEY_CODE_HASH=ACTUAL_QUALIFIED_WALLET_WASM_SHA256
chmod 600 "$RELAY_KEY_FILE"
mkdir -m 700 "$RELAY_STATE"
npm run relay
```

Only loopback port8787 is opened by default; publish through HTTPS with request limits. A compromised relay key cannot replace the holder's signature. Native `setrelay(false)`, maintenance, bond exit or blacklist prevents new commercial executions; prior earnings remain claimable. Never put private wallets, keys, relay state or test chain databases in Git.

## Isolated tests

`tests/chain_test.py` uses the pinned SDK LocalTest helpers plus an independent data/config/wallet tree. Provide the SDK helpers as `sdk-tests/pureeos_single_choice_local_test.py` and `sdk-tests/native_savanna_genesis_test.py`; their hashes are recorded with the orchestration inputs. The TEST runtime supplies contracts and the 4-worker parallel configuration. No static peer is configured, and all test child processes are stopped in finally.

```bash
python3 -B tests/chain_test.py --package /path/to/pureeos-test-1.0.0-test.1 \
  --work /private/new-key-qualification --artifacts /path/to/artifacts \
  --native-cleos /path/to/qualified-cleos-wrapper

# Separate fresh chain, explicitly marked table fixtures:
python3 -B tests/chain_test.py --fixture --package /path/to/pureeos-test-1.0.0-test.1 \
  --work /private/new-key-fixture-qualification --artifacts /path/to/artifacts
```

Production integration covers real system-table prefixes, text/key/token paths, fees, original cleos transfers, native cleos/SDK signing agreement, real BP relay HTTP/canonical confirmation, maintenance, atomic reserve handling, unsupported token behavior, revocation epoch and independent-device concurrency and RAM caps. A real PowerUp reservation checks dynamic pricing. Alternate-system fixtures cover deterministic blacklist/key-change rejection, bond/active failures, extreme occupancy and schema-change exits; they are **wallet logic tests, not native consensus blacklist qualification**. No TPS is measured, and shared VM results are not bare-metal performance claims.

Retain all failed attempts and retries. Bootstrap transactions that demonstrably roll back at the unchanged 150ms CPU limit may be retried with a new native header on a busy shared host; authorization/business failures are never retried as successes. Verify actual WASM, ABI, client binary, source inputs, hardware, chain parameters and canonical receipts in the final qualification record.

Owner proof protocol is version **2**; never initialize a new deployment with the superseded sequential-nonce ABI. Use the committed matching ABI/WASM, and require replay, inline-proof, envelope-tampering and concurrent same-epoch tests before promotion. See [risk controls](RISKS.md).

2026-11-03 execution policy: future builds and all test execution belong on an independently provisioned Vultr host. The local workstation is for editing/orchestration and receiving/verifying evidence only. Keep the public low-cost demonstration host separate.
