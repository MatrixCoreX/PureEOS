# V1 API and security model

## Funds and supported tokens

`eosio.key` is the custody account, distinct from the issuing token contract and from BP escrow/slash pools. Initialization selects the 4-decimal AIC issuing interface. Other fungible tokens must provide standard `transfer(from,to,quantity,memo)`, `stat` and `accounts` semantics. Identity includes issuer contract and complete symbol/precision; identically named tokens remain distinct.

The token issuer remains a trust boundary. Reserve checks cannot stop an issuer from lying consistently in all its own tables or changing its token code later. Taxed/rebasing tokens and missing standard notifications are incompatible when they change full-principal delivery. Detected discrepancies cause atomic rollback.

| Operation | Interface | Payment |
|---|---|---|
| Prepay native fee credit | AIC transfer to wallet, memo `fee` | Full quantity credited to native sender |
| Native account → key | Token transfer, memo `PK1\|PUBLIC_KEY\|maximum_fee_atoms` | Full principal to key; extra AIC from sender fee credit |
| Key → key | `ksend(request,sig)` | Principal and extra AIC from sender key |
| Key → native account | `withdraw(request,sig)` | Full principal transferred atomically to existing account |
| Owner self withdrawal | `selfout(payer,request,sig)` | Native payer authorizes CPU/NET; key pays base AIC fee, no markup |
| Invalidate old intents | `cancel` / `selfcancel` | Owner signs advancement to a larger next_nonce |
| Claim old relay earnings | `claim(bp,to,quantity,max_fee)` | BP authorizes; separate prepaid base fee; no new relay eligibility requirement |
| Refund unused credit | `refundfee(owner,to,quantity,max_fee)` | Native owner authorizes; refund principal and fee are separate |

A key sending another token still needs additional AIC. No automatic conversion is performed. Sending 10 tokens with a 0.1 AIC fee delivers all 10 tokens. AIC transfers require both principal and fee in the sender balance.

Non-AIC reserves equal the sum of that asset's key balances. AIC reserves equal key balances plus native fee credits, network base-fee buckets and earned BP premiums. Outgoing transfers require a matching sender notification, exact reserve change, and full recipient increase during the standard sender-notification phase. A later normal recipient callback can use its received tokens. A missing step rolls back nonce, fees, ledger and external token effects.

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

Work units: deposit `2+ceil(memo_bytes/64)`, key send `4+ceil(memo_bytes/64)`, withdrawal `6+ceil(memo_bytes/64)`, cancellation `3`, claim/refund `4`. New key wallets, asset-balance rows and asset registrations add row costs.

## Signed envelope and relay permissions

Digest is SHA-256 of the ABI binary sequence:

```
string("PureEOS:eosio.key"), uint32(1), checksum256(chain_id),
name("eosio.key"), checksum256(deployment), name(operation), request
```

`ksend` request field order: from key, to key, token_contract, quantity, memo, nonce(uint64), expires(uint32), relayer, reward_to, max_fee. Withdrawal/selfout order: from key, token_contract, to native name, quantity, memo, nonce, expires, relayer, reward_to, max_fee. Cancellation: from, nonce, next_nonce, expires, relayer, reward_to, max_fee. The contract ABI is authoritative.

Only K1 keys/signatures are supported. Legacy EOS and PUB_K1 encodings resolve to one binary identity. New recipient points are checked on secp256k1. Native recovery verifies owner signatures. Persistent nonces never reset at zero balance; expiration uses consensus time and cannot exceed one hour. Failed operations roll back all effects. JS uses strings/BigInt for uint64 and rejects unsafe numeric nonces.

A commercial relay registers a dedicated child permission under its parent BP. Every execution requires the exact registered permission and rechecks active registration, normal full 10000 AIC bond, no permanent blacklist, no pending/effective maintenance, and enabled opt-in registry. Relayer and reward_to must name the same selected BP. Changing workers, keys or registration cannot bypass a parent BP ban. Standby BPs may relay; the service is not restricted to the active 21.

Relay keys must be separate from owner/active, production and BLS keys. Link the child permission to ksend, withdraw and cancel. The BP signs the native resource-paying transaction separately. Its 30% markup is gross service revenue, not guaranteed profit. Revocation does not confiscate previously earned income and adds no BP slashing rule.

Self operations have empty relayer/reward_to, an owner signature and a native payer authorization. When the pinned system code changes, commercial operations and new principal deposits fail closed pending review. Existing-asset self exits, cancellation, old claims and credit refunds use the base multiplier to preserve exit paths. Broken token contracts or insufficient AIC can still prevent corresponding operations.

## Queries and reference HTTP service

`KeyWalletClient` reads the actual Chain ID, ABI/config, persistent nonce, complete asset identity and balances. Use a trusted RPC and verified deployment/code hashes before signing; inspect prepared JSON rather than signing untrusted files. Initialization must bind the actual `get_info` Chain ID.

Tables: config; wallets (bykey SHA256 secondary index); balances (scope wallet id); assets (byidentity); nativefees; feebuckets; relays; pricing. Internal-only stateless `receipt` actions can be indexed by a history service; V1 stores no unbounded event history in RAM.

`POST /submit` accepts operation/request/signature only. `GET /status/{intent_digest}` reports pending/submitted/finalized/expired for one native packet. `GET /health` exposes public BP identity. No user private key is accepted. The relay durably saves the exact packet before broadcasting, retries that packet, and scans canonical irreversible receipts. A timeout is not settlement and does not immediately create a new TAPOS/signature. Expiration requires both complete LIB scanning and irreversible time beyond native transaction expiration.

The reference service has a serialized submission queue, 8 KiB bodies, connection/rate limits and a 100000-job journal cap. Expose it through HTTPS. Configure real-client limits in the proxy; the service does not trust forwarding headers and shares its built-in quota across connections from one proxy. Remove a stale crash lock only after verifying the previous process is stopped and reviewing its journal. Never run two instances against one journal directory.
