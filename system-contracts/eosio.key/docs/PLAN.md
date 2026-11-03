# eosio.key v1

2026-11-03 最新范围：测试白名单为系统 AIC 与标准 `keyusdtoken1::4,USD`，绑定完整币种和审核代码哈希；后续新增通过受保护 eosio.key 代码的原生七天开发者提案/BP 否决流程。第三方文本收款账号必须先付 RAM open。RAM 登记与白名单相互独立；独立远端正常系统74项、替代表夹具74项与SDK/中继14项通过，现已部署公开测试链；旧风险复现保留，稳定版本未更新。详见 [白名单提案流程](TOKEN_ALLOWLIST.md)。

Status: AIC/USD whitelist qualified remotely and deployed to the public TEST chain on 2026-11-03; stable release unchanged.

2026-11-03 relay surface selection: keep a standalone HTTP service with its own
HTTPS endpoint; do not integrate relay handlers into nodeos RPC. Public-key
balance browsing uses read-only chain RPC and remains independent of relays.
Future scope selected for planning: ordinary native accounts may also operate
relays, fund CPU/NET and earn successful-execution service fees. This is not
enabled in the current BP-only contract. General relay admission, permission
revocation, abuse controls and any deposit policy require a separate implementation
and qualification; do not reuse or alter BP bond/slash pools automatically.

The wallet accepts standard native-token transfers, identifies each asset by
issuer contract plus full symbol/precision, and credits canonical K1 public keys.
It never issues replacement tokens or changes eosio.system. Tokens with transfer
taxes/rebases or incompatible notifications fail closed pending a dedicated adapter.

2026-11-03 selected RAM admission supersedes automatic asset admission: every
full token identity, including AIC fee funding, requires an atomic native
`eosio::buyram` purchase followed by `eosio.key::regtoken`. Actual system purchase
receipts credit only that token's RAM allocation budget. Any native sponsor may
top up; no sponsor withdrawal or token deregistration is provided. New key,
balance and payer rows consume conservative byte budgets; exhausted budgets
reject new allocations while existing rows and no-new-row exits remain usable.

Native deposits also require separately funded AIC fee credit and an explicit
total fee cap in memo. Recipient wallets and per-asset rows remain created on
demand without recipient registration/signature. The sender pays new-recipient
row fees: the native sender's prepaid AIC for deposits, or the sending key's AIC
for key sends. Fees are separate from full principal; existing rows pay no
repeated creation fee. Public-key balances/epochs survive zero balances; keys and
asset IDs are never recycled. This does not resolve malicious issuer authority
over shared custody RAM. The qualified profile additionally enforces the reviewed code-hash whitelist.

Fee inputs are committed PowerUp market adjusted/current occupancy and capacity,
not machine load or measured block consumption. All arithmetic uses integers.
Immutable initial policy sets minimum fees, per-operation work, row charges,
maximum multiplier and limits. A short on-chain epoch cache fixes one price per
epoch. Users sign a maximum total fee and expiration. The BP markup is exactly
30% before documented atomic-unit rounding. Base fees are segregated from relay
earnings; no existing BP bond/slash pool is reused. Relay income is not a promise
of net profit or automatic reimbursement of operating costs.

Commercial relay calls require a registered active producer, a full normal
10000 AIC bond, no permanent ban or maintenance, and opt-in relay registration.
Only a dedicated child permission is accepted. Only the actual successfully
executing selected BP receives its markup. Previously earned balances remain
claimable after role revocation. No new BP-slash condition is introduced.

Owner protocol 2 binds the complete immutable native transaction ID through a context-free proof signature. Native dedup/TAPOS/expiration replace per-transfer sequence allocation. Independent devices use cryptographic random request IDs and a shared read-only revocation epoch; only explicit cancellation increments the epoch. No allocator or permanent per-request RAM table is required. One root operation is enforced; inline proof reuse is rejected. Native deposits default to PK2 random routing IDs. Self exit still requires owner proof plus an explicit native payer.

Assets maintain a reserve invariant. Outgoing token operations use checks/effects
before interactions, expected sender notifications and a final inline reserve
check, so a failed or incompatible token operation rolls the whole transaction
back. Global fee totals are avoided on routine internal transfers; fee buckets
are partitioned by native payer/BP. Network-fee withdrawal policy is deliberately
absent in v1; any future distribution requires a reviewed upgrade.

Deployment requires creating eosio.key before governance sealing. Sealing must
include it in the native protected-account set and leave only intended code
authorities. Released system WASM, 7-day review, single-choice voting, 10/90 split,
halving, rotation/finalizers, RAM64GiB/setramrate1024 and deferred-disable stages
remain unchanged. The separately user-authorized 2026-11-03 public TEST rebuild carries this qualified contract; no TPS is authorized.

2026-11-03 selected claim update: gross earnings cover their own actual base withdrawal
fee, only the positive net is paid, and native fee deposits remain untouched. Preserve
all other paths and schema. Build/test remotely in a private temporary namespace on
the retained TEST host, no new server or live reset. Qualify before publishing the native
seven-day protected-code proposal; execution cannot precede review or bypass BP veto.

Latest user correction supersedes the previous gross-minus-fee selection: income claim is
application-fee-free, full earned quantity paid, with native CPU/NET and missing recipient
RAM supplied by the claimant. Keep all transfer fees, including internal key sends, dynamic.
The legacy max_fee ABI field supports zero; no schema migration. Remote isolated tests and
seven-day protected governance still apply. New work: claim-native-resources-r1.

Latest redeployment authorization supersedes the preceding no-reset/seven-day TEST path:
qualify remotely, bootstrap fresh TEST genesis with the fee-free claim before sealing, and use
the separately opt-in native 1,800-second TEST review profile on all25BPs plus history. Stable
release defaults and old14-day protocol replay remain unchanged. Remove old cheap TEST raw
blocks/state/history indices without backup, retaining source/build/reports and unrelated
immutable retired high-spec evidence. Keep the same25names and90ordinary low-rate business.
No new server/TPS. Work: keynet-redeploy-r2 and test-governance-30min-r1.
