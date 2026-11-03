# First-release API · owner protocol 2

2026-11-03 最新范围：测试白名单为系统 AIC 与标准 `keyusdtoken1::4,USD`，绑定完整币种和审核代码哈希；后续新增通过受保护 eosio.key 代码的原生七天开发者提案/BP 否决流程。第三方文本收款账号必须先付 RAM open。RAM 登记与白名单相互独立；独立远端正常系统74项、替代表夹具74项与SDK/中继14项通过，现已部署公开测试链；旧风险复现保留，稳定版本未更新。详见 [白名单提案流程](TOKEN_ALLOWLIST.md)。

## Funds and supported tokens

`eosio.key` is the custody account, distinct from the issuing token contract and from BP escrow/slash pools. Initialization selects the 4-decimal AIC issuing interface. Other fungible tokens must provide standard `transfer(from,to,quantity,memo)`, `stat` and `accounts` semantics. Identity includes issuer contract and complete symbol/precision; identically named tokens remain distinct.

The token issuer remains a trust boundary. Reserve checks cannot stop an issuer from lying consistently in all its own tables or changing its token code later. Taxed/rebasing tokens and missing standard notifications are incompatible when they change full-principal delivery. Detected discrepancies cause atomic rollback.

| Operation | Interface | Payment |
|---|---|---|
| Prepay native fee credit | AIC transfer to wallet, memo `fee` | Full quantity credited to native sender |
| Native account → key | Token transfer, memo `PK2\|PUBLIC_KEY\|maximum_fee_atoms\|random_32byte_hex` | Full principal to key; extra AIC from sender fee credit |
| Key → key | `ksend(request)` | Principal and extra AIC from sender key |
| Key → native account | `withdraw(request)` | Full principal transferred atomically to existing account |
| Owner self withdrawal | `selfout(payer,request)` | Native payer authorizes CPU/NET; key pays base AIC fee, no markup |
| Invalidate old intents | `cancel` / `selfcancel` | Owner signs advancement to a larger next_epoch |
| Claim old relay earnings | `claim(bp,to,quantity,max_fee)` | BP authorizes; separate prepaid base fee; no new relay eligibility requirement |
| Refund unused credit | `refundfee(owner,to,quantity,max_fee)` | Native owner authorizes; refund principal and fee are separate |

A key sending another token still needs additional AIC. No automatic conversion is performed. Sending 10 tokens with a 0.1 AIC fee delivers all 10 tokens. AIC transfers require both principal and fee in the sender balance.

Non-AIC reserves equal the sum of that asset's key balances. AIC reserves equal key balances plus native fee credits, network base-fee buckets and earned BP premiums. Outgoing transfers require a matching sender notification, exact reserve change, and full recipient increase during the standard sender-notification phase. A later normal recipient callback can use its received tokens. A missing step rolls back epoch, fees, ledger and external token effects.

## Paid RAM admission and sender fees

Every asset, including AIC principal and AIC fee deposits, requires paid
`regtoken(payer, token_contract, token_symbol, min_bytes)` admission. Send exactly
two ordinary root actions in one atomic transaction: `eosio::buyram` to eosio.key,
then `eosio.key::regtoken`, both authorized by the same native payer. The exact AIC
spend is bounded by the purchase amount, and minimum bytes must be at least 4096.
Only actual native `logbuyram` bytes grant capacity. Failed admission rolls the
purchase back. Any native account may top up; first-sponsor attribution remains.
There is no sponsor refund, RAM sale or token deregistration interface.

Per-token `ram_purchased` is real purchased bytes; `ram_used` is a conservative
allocation budget rather than actual native RAM usage. Reserve 2048 bytes per
asset, 1024 per new key, 512 per new internal asset-balance/native recipient row,
and 1024 per native fee payer for its credit/bucket rows. Exhausted budgets reject
new allocation while existing rows can receive and no-new-row exits still work.
Zero balances and recipient close/reopen never reset budgets.

The transfer sender pays extra recipient-creation fees: native sender prepaid AIC
for deposits, sending-key AIC for key transfers/withdrawals. The recipient needs
no account or opening signature and gets full principal. Existing records pay no
repeated creation charge; normal execution fees remain. `new_row_fee` is a policy
price, not a live RAM byte quote. Collected fees do not refill purchased-byte
budgets automatically. SDK `ramRegistrationActions` builds the purchase/admission
actions; see [CLEOS.md](CLEOS.md). This accounting does not isolate arbitrary
malicious issuer RAM authority. The qualified profile separately requires exact-identity/code-hash whitelist admission; arbitrary issuers remain excluded.

## Dynamic fees

The pinned PureEOS system code is read through `eosio::powup.state`, **scope 0**. CPU/NET `max(adjusted_utilization, utilization) / weight` measures committed resource-market occupancy. A fresh reservation can raise current utilization before the smoothed field catches up; that is valid on-chain state. It is **not actual block CPU/NET consumption, host CPU percentage or TPS**. V1 adds no nodeos meter and accepts no BP-reported congestion value.

```
u = max(floor(max(cpu.adjusted_utilization,cpu.utilization) * 10000 / cpu.weight),
        floor(max(net.adjusted_utilization,net.utilization) * 10000 / net.weight))
multiplier_bps = 10000 + floor((maximum_multiplier_bps - 10000) * u² / 100000000)
base = max(minimum_fee, ceil((unit_fee * work_units + new_row_fee * new_rows) * multiplier_bps / 10000))
premium = commercial ? ceil(base * 30 / 100) : 0
total = base + premium
```

Arithmetic is integer-only. Ordinary deposits, self exits, claims and refunds have no commercial premium. Base fees stay in network fee buckets; V1 has no base-fee withdrawal/distribution action. Self exits use a fixed bucket created at initialization, so new CPU sponsors cannot allocate unbounded rows or block exits at capacity. The sponsor receives no fee reward.

The first fee operation per consensus-time epoch refreshes pricing; others reuse it. Initialization chooses a 1–60 second epoch. Quotes are estimates; execution uses current cached pricing and actual new rows and cannot exceed the signed maximum. Test prices are functional fixtures, not production economic qualification. A nonzero floor, row costs and capacity limits impose initial spam costs/bounds without claiming resistance to arbitrary economic attacks.

Work units: deposit `2+ceil(memo_bytes/64)`, key send `4+ceil(memo_bytes/64)`, withdrawal `6+ceil(memo_bytes/64)`, cancellation `3`, claim/refund `4`. New recipient wallets and asset-balance rows incur the sender-paid row fee; token registration has its separate sponsored RAM purchase.

## Complete transaction owner proof (protocol 2)

The holder prepares a complete native transaction, with one context-free `eosio.key::prove` action and one ordinary root operation. No delay, extra root actions or transaction extensions are accepted. The native authorization names exactly one selected BP child permission, or one explicit self payer. Native expiration is at most one hour. The signed full reference block number must agree with the native TAPOS low 16 bits; relay admission additionally checks the actual irreversible reference ID/prefix.

Owner digest, using fixed protocol binary types, is:

```
sha256(pack(string("PureEOS:eosio.key:transaction"), uint32(2),
  checksum256(chain_id), name("eosio.key"), checksum256(deployment),
  checksum256(native_transaction_id)))
```

`prove` data is `(public_key owner, checksum256 chain_id, checksum256 deployment, uint32 reference_block_num)`. Its sole CFD entry is a serialized K1 signature (66 bytes); extra CFD is rejected. Native transaction ID excludes signatures and CFD, avoiding a circular signature. The resource payer separately signs the native transaction **including the CFD hash**. `prove` verifies recovery without state access; the ordinary operation validates proof domain, exact action bytes, top-level execution and current revocation epoch. Inline calls cannot reuse a proof. The holder signature does not replace native BP/payer authorization.

Request field order follows the committed ABI: key send `(from, to, token_contract, quantity, memo, auth_epoch:uint64, request_id:checksum256, relayer, reward_to, max_fee)`; withdrawal places `token_contract` before native `to`; cancellation `(from, auth_epoch, next_epoch, request_id, relayer, reward_to, max_fee)`. Self wrappers prefix native `payer`. The SDK and cleos pack fixed signing schemas rather than trusting fetched RPC ABI definitions.

Each device chooses an independent cryptographic 32-byte request ID. Different transfers with the same `auth_epoch` can execute concurrently; ordinary sends never consume that epoch. Only explicit cancellation advances it. A cancellation and an old transfer racing are resolved by actual chain execution order; already finalized transfers cannot be revoked. Insufficient combined funds can still reject a concurrent transfer atomically.

Replay protection uses native transaction ID deduplication while valid. Native cleanup requires `now > expiration`, while native acceptance requires `expiration >= pending_block_time`: there is no boundary gap. Re-signing or recompressing does not change transaction ID. Changing amount, destination, request ID, TAPOS or expiration changes the ID and invalidates the holder proof. After pruning, the immutable old transaction is expired. No per-request permanent RAM table is allocated. Never auto-reprepare on an ambiguous RPC timeout; the holder already knows the transaction ID before submission. An intentionally fresh holder signature authorizes a new payment even if economically similar.

Only K1 is supported. Public-key wallet identity and epoch survive zero balance. JavaScript preserves uint64 as strings/BigInt. PK2 routing memos provide random IDs for otherwise identical native deposits too; legacy PK1 remains accepted for compatibility but clients default to PK2.

Commercial execution checks registered active BP status, normal full 10,000 AIC bond, no ban/maintenance, enabled registry and exact dedicated child permission every time. Worker/key rotation does not evade parent BP revocation. Standby BPs may relay. Previously earned premiums remain claimable. Self exits/cancellation have no BP requirement or markup; a native CPU/NET payer and additional AIC are still needed. System-code changes fail closed for commercial sends/new principal while retaining reviewed existing-asset exit behavior. There is no automatic key recovery or new BP slashing rule.

ding operations.

## Queries and reference HTTP service

`KeyWalletClient` reads the actual Chain ID and config (signing schemas are fixed locally), revocation epoch, complete asset identity and balances. Use a trusted RPC and verified deployment/code hashes before signing; inspect prepared JSON rather than signing untrusted files. Initialization must bind the actual `get_info` Chain ID.

Tables: config; wallets (bykey SHA256 secondary index); balances (scope wallet id); assets (byidentity); nativefees; feebuckets; relays; pricing. Internal-only stateless `receipt` actions can be indexed by a history service; V1 stores no unbounded event history in RAM.

`POST /submit` accepts `{prepared, owner_signature}` only. `GET /status/{native_transaction_id}` reports pending/submitted/finalized/expired for one native packet. `GET /health` exposes public BP identity. No user private key is accepted. The relay durably saves the exact packet before broadcasting, retries that packet, and scans canonical irreversible receipts. A timeout is not settlement and does not immediately create a new TAPOS/signature. Expiration requires both complete LIB scanning and irreversible time beyond native transaction expiration.

The reference service has a serialized submission queue, 8 KiB bodies, connection/rate limits and a 10000-job journal cap. Expose it through HTTPS. Configure real-client limits in the proxy; the service does not trust forwarding headers and shares its built-in quota across connections from one proxy. Remove a stale crash lock only after verifying the previous process is stopped and reviewing its journal. Never run two instances against one journal directory.

Native recipient token rows newly funded by the wallet also consume the shared balance-row budget and pay the new-row fee; closing/reopening them cannot reset that budget. Initial limits must fit purchased RAM with an operating reserve. Malicious issuers that fabricate state or abuse native RAM billing remain outside the standard-token trust profile.

原生收款方首次建立代币余额行、且由钱包承担 RAM 时，也计入共用余额行预算并收新行费；关闭再重建不会重置预算。初始化上限须按实际已购买 RAM 留出运行余量。恶意发行方伪造状态或滥用原生 RAM 计费仍属于发行方信任边界。

Reference relay admission: at most 128 pending jobs, 8 per holder, 10,000 durable jobs total and 3 automatic broadcasts per immutable transaction. Reconciliation continues after retry exhaustion; unknown outcomes never become an automatic new authorization. It accepts the configured fee issuer by default. Set `RELAY_ALLOWED_TOKENS=keyusdtoken1,...` only after operator review for additional issuers. Operator issuer review can only narrow the compiled on-chain whitelist; it cannot add an unapproved identity or code hash. Independent-device signing is unchanged.

参考中继最多保留 128 笔待定任务、每公钥 8 笔、总日志 1 万笔；每原始交易最多自动广播 3 次，之后继续查不可逆回执，不自动生成新授权。默认接收配置 AIC 发行合约，其他发行方由运营者审核后写入 `RELAY_ALLOWED_TOKENS`。这些是中继服务限额，不改变链上多币种支持或多设备签名规则。
