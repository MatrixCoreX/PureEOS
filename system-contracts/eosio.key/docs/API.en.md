> Historical profile document. For the 2026-11-05 atomic-RAM candidate, use [current Chinese rules](ATOMIC_RAM_TRANSFERS.zh.md) or [current English rules](ATOMIC_RAM_TRANSFERS.en.md). Prepaid fees, PK memo deposits and manual ordinary-transfer opening are superseded; public activation remains pending.

# First-release API · owner protocol 2

2026-11-04 RAM 市场计费候选 / market-priced RAM candidate: see [规则、公式与兼容 / rules and compatibility](RAM_MARKET_FEES.md). New source prices approved bytes from current RAM reserves; old deployed hashes retain fixed row pricing. Public activation is not included in this development task.


The TEST allowlist is system AIC and standard `keyusdtoken1::4,USD`, pinned by full asset identity and reviewed code hash. Future additions require a protected eosio.key code proposal and BP veto review:1800seconds on this TEST chain, seven days for stable. Named USD recipients must fund their token balance row before withdrawal. RAM registration remains separate from code admission. Configurable relay rates are deployed through a mature native proposal; see the current deployment report linked from the module README and [token admission](TOKEN_ALLOWLIST.md).

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
| Claim old relay earnings | `claim(bp,to,quantity,max_fee)` | Full income, no application fee; BP pays native resources and pre-opens missing recipient RAM |
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
repeated creation charge; normal execution fees remain. Legacy code uses the policy
`new_row_fee`; the market-priced candidate uses the approved byte budget and current
RAM curve quote instead. Collected fees do not refill purchased-byte
budgets automatically. SDK `ramRegistrationActions` builds the purchase/admission
actions; see [CLEOS.md](CLEOS.md). This accounting does not isolate arbitrary
malicious issuer RAM authority. The qualified profile separately requires exact-identity/code-hash whitelist admission; arbitrary issuers remain excluded.

## Dynamic fees

The pinned PureEOS system code is read through `eosio::powup.state`, **scope 0**. CPU/NET `max(adjusted_utilization, utilization) / weight` measures committed resource-market occupancy. A fresh reservation can raise current utilization before the smoothed field catches up; that is valid on-chain state. It is **not actual block CPU/NET consumption, host CPU percentage or TPS**. V1 adds no nodeos meter and accepts no BP-reported congestion value.

```
u = max(floor(max(cpu.adjusted_utilization,cpu.utilization) * 10000 / cpu.weight),
        floor(max(net.adjusted_utilization,net.utilization) * 10000 / net.weight))
multiplier_bps = 10000 + floor((maximum_multiplier_bps - 10000) * u² / 100000000)
execution = ceil(unit_fee * work_units * multiplier_bps / 10000)
ram_net = new_bytes == 0 ? 0 : ceil(AIC_reserve * new_bytes / (RAM_reserve - new_bytes))
ram_fee = ceil(ram_net * 200 / 199)
base = max(minimum_fee, execution + ram_fee)
premium = commercial ? max(1, ceil(base * relay_fee_bps / 10000)) : 0
total = base + premium
```

The formula above is for the market-priced candidate. Legacy deployed hashes instead use `base = max(minimum_fee, ceil((unit_fee * work_units + new_row_fee * new_rows) * multiplier_bps / 10000))`. RAM is read fresh; the epoch cache below applies only to CPU/NET.

Arithmetic is integer-only. Ordinary deposits, self exits and refunds have no commercial premium. Income claims have zero application fee, independent of market occupancy. Base fees stay in network fee buckets; V1 has no base-fee withdrawal/distribution action. Self exits use a fixed bucket created at initialization, so new CPU sponsors cannot allocate unbounded rows or block exits at capacity. The sponsor receives no fee reward.

The first fee operation per consensus-time epoch refreshes pricing; others reuse it. Initialization chooses a 1–60 second epoch. Quotes are estimates; execution uses current cached pricing and actual new rows and cannot exceed the signed maximum. Test prices are functional fixtures, not production economic qualification. A nonzero floor, row costs and capacity limits impose initial spam costs/bounds without claiming resistance to arbitrary economic attacks.

Work units: deposit `2+ceil(memo_bytes/64)`, key send `4+ceil(memo_bytes/64)`, withdrawal `6+ceil(memo_bytes/64)`, cancellation `3`, refund `4`; income claim has no AIC fee or pricing refresh. New recipient wallets and asset-balance rows incur the sender-paid storage fee; token registration has its separate sponsored RAM purchase.

## Complete transaction owner proof (protocol 2)

The holder prepares a complete native transaction, with one context-free `eosio.key::prove` action and one ordinary root operation. No delay, extra root actions or transaction extensions are accepted. The native authorization names exactly one selected BP child permission, or one explicit self payer. Native expiration is at most one hour. The signed full reference block number must agree with the native TAPOS low 16 bits; relay admission additionally checks the actual irreversible reference ID/prefix.

Owner digest, using fixed protocol binary types, is:

```
sha256(pack(string("PureEOS:eosio.key:transaction"), uint32(2),
  checksum256(chain_id), name("eosio.key"), checksum256(deployment),
  checksum256(native_transaction_id)))
```

`prove` data is `(public_key owner, checksum256 chain_id, checksum256 deployment, uint32 reference_block_num)`. Its sole CFD entry is a serialized K1 signature (66 bytes); extra CFD is rejected. Native transaction ID excludes signatures and CFD, avoiding a circular signature. The resource payer separately signs the native transaction **including the CFD hash**. `prove` verifies recovery without state access; the ordinary operation validates proof domain, exact action bytes, top-level execution and current revocation epoch. Inline calls cannot reuse a proof. The holder signature does not replace native BP/payer authorization.

Request field order follows the committed ABI: legacy key send `(from, to, token_contract, quantity, memo, auth_epoch:uint64, request_id:checksum256, relayer, reward_to, max_fee)`; withdrawal places `token_contract` before native `to`; cancellation `(from, auth_epoch, next_epoch, request_id, relayer, reward_to, max_fee)`. Each request now permits a trailing `relay_fee_bps:uint16$` extension. Self wrappers prefix native `payer`. The SDK and cleos pack fixed signing schemas rather than trusting fetched RPC ABI definitions.

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

## Fee-free income claim (qualified TEST deployment, 2026-11-03)

`claim(bp,to,quantity,max_fee)` pays the full positive earned AIC quantity. It uses no fee quote,
charges no AIC fee, never requires/debits/allocates `nativefees`, never credits a network-fee bucket
and does not refresh pricing. The legacy `max_fee` field remains valid and may be zero.
The BP's native authority supplies CPU/NET. A missing recipient AIC row is rejected: the claimant
must authorize standard token `open(owner=to,symbol=4,AIC,ram_payer=bp)` before claim, optionally
as the immediately preceding root action in the same atomic transaction. Custody RAM and byte
budgets must not grow. Existing rows require no new RAM. Native authorization, reserve equations,
income bounds, revocation-independent income exit and complete rollback remain. The receipt records
full quantity and zero fee. No ABI/table migration. Other transfer/refund paths retain existing fees.
The matching SDK quote assumes this qualified code profile. For the specifically authorized
2026-11-03 TEST rebuild, qualify remotely and install before governance sealing; its independently
built TEST native review is 1,800 seconds. Stable/production remains 604,800 seconds. Other sealed
chains must follow their existing native upgrade rules.

## Operator-set relay rates

The BP owner authorizes `setrate(bp,fee_bps)` with1000–3000 basis points (10%–30% of the dynamic base fee). The BP pays RAM for its separate `relayrates` row. Missing rate rows preserve the legacy30% rate. Re-registration or service toggles retain the selected rate; qualified BP eligibility remains mandatory.

`regrelay` accepts an empty endpoint for inactive comparison metadata. Such entries cannot relay transfers, and `setrelay(true)` rejects until an HTTPS endpoint is registered. The current TEST selection operates only the founder HTTP relay; the other four entries remain inactive, without extra keys or permissions.

`service_atoms = max(1, ceil(base_atoms * fee_bps / 10000))`; `total_atoms = base_atoms + service_atoms`. Amounts round upward to0.0001 AIC, with a minimum service fee of0.0001 AIC. Self-service operations carry no relay fee; income claims remain application-fee-free.

The optional trailing `relay_fee_bps:uint16$` request extension binds the selected quote rate into the owner's signed native transaction. A changed rate rejects execution, and the existing signed total fee cap still applies. Legacy requests without the extension retain the original30% rate ceiling and signed total fee cap, and can execute at cheaper configured rates. The existing native cleos remains compatible; exact quote-rate binding requires the new SDK extension. This revision becomes active only after remote qualification and the native review/execution process; consult its deployment report for the actual chain/code state.

An empty registration endpoint creates an inactive comparison entry. Such an entry cannot be enabled until an HTTPS endpoint is configured. Only the founder HTTP service is operated in this test; four other entries have no additional endpoints, services, permissions or relay keys.
