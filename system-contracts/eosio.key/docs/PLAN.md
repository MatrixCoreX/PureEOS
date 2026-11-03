# eosio.key v1

Status: implementation and isolated qualification, not deployed to the public testnet.

The wallet accepts standard native-token transfers, identifies each asset by
issuer contract plus full symbol/precision, and credits canonical K1 public keys.
It never issues replacement tokens or changes eosio.system. Tokens with transfer
taxes/rebases or incompatible notifications fail closed pending a dedicated adapter.

Native deposits require a separately funded AIC fee credit and an explicit total
fee cap in memo. A wallet may atomically bundle fee funding and the principal
transfer. Recipient wallets and per-asset rows are created on demand; there is no
recipient registration/signature prerequisite. New storage is charged, bounded,
and never granted unlimited contract RAM. Public-key balances/epochs survive zero
balances; keys and asset IDs are never recycled.

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
remain unchanged. No live reset/deployment/TPS is authorized by this development.
