# PureEOS whole-chain post-quantum implementation plan

Date: 2026-11-04 · Revision: R1 · Status: implementation design; no PQ implementation or activation claimed.

Chinese counterpart: [完整中文计划](FULL_CHAIN_POST_QUANTUM_PLAN.zh-CN.md). Reproducible source inventory: [SOURCE_AUDIT.json](SOURCE_AUDIT.json).

## 1. Objective, meaning of completion, and present authorization

The objective is to make post-activation chain safety, asset authorization, governance and authenticated verification independent of an attacker being unable to forge K1, R1, WebAuthn elliptic-curve signatures, or BLS signatures. Adding a signature option to a wallet is an intermediate deliverable.

“Whole-chain PQ” has the following acceptance boundary:

1. Every external principal capable of changing protected state must satisfy a PQ authorization policy. This includes permission parents, delegated accounts, resource sponsors, BP administration, relay permissions, recovery and upgrade authority.
2. Proposer authentication and both active and pending finalizer policies must require PQ proofs. Classical signatures alone must not establish finality, replace a committee, or authorize an upgrade.
3. New security commitments must have an explicitly reviewed hash profile; a short legacy hash must not remain the only binding between a PQ signature and its actual transaction, code, policy or checkpoint.
4. Replay, bootstrap, snapshots, light-client verification and historical-policy transitions must preserve that boundary.
5. All contracts holding assets within the claimed scope must have their authorization paths reviewed. Arbitrary future WASM can implement unsafe authorization itself; native PQ permissions cannot prove the safety of every possible application.
6. Wallet distribution, administrative access, authenticated service responses and key custody must be assessed separately. A classical-only OS installer, certificate root or remote administration credential remains an external assumption until replaced or independently protected.

The deliverable authorized now is code review, two Markdown plans and publication to Git. This task does not authorize implementation, chain operations, a protocol activation, a new server, a TPS campaign or a production release. Future engineering stages below are a proposed sequence, not commands to execute now.

The current chain, all original 26 node processes, ordinary activity, history, the sole founder HTTP relay, existing bans, votes, bonds, token limits and release artifacts remain outside this documentation change. Existing proposal #3 remains pending/vetoed; do not cancel or overwrite it to make room for this plan.

## 2. Baselines and provenance limits

These are retained project records and inspected local source identities, not fresh live-node observations.

| Component | Inspected baseline / recorded identity | Qualification limit |
|---|---|---|
| TEST chain | `116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0` | Preserve chain identity and existing history; no reset proposed |
| Node source | `PureEOS-Test@057e1731660d3b8423c960cb81fe1758ae9c29f6` | Source review; no binary reproduced in this task |
| Recorded node binary | SHA-256 `0be5282d1d5a5eb72081e1d1374931d5e79cff139de728f8d230e29df50534e7` | From retained TEST artifact/handover records |
| Contract / SDK / relay source | `PureEOS-Key@23fa32adb307b2a2f756703f6faef16b14313ab2` | Matching current RAM-fee development lineage |
| Recorded `eosio.key` WASM | `bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1` | Recorded irreversible proposal #2 activation |
| Recorded `eosio` WASM | `b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170` | Exact current source-to-WASM rebuild mapping must be re-established in P0 |
| Recorded reviewed token WASM | `f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c` | AIC/USD identity, precision and code pins remain explicit |
| Retained system lineage | `PureEOS-deployed-system-20261102@b230c7e12d7cd0bc05ad8269140dc030f8b5fe79` | Useful rotation/finalizer source; not asserted to be the complete exact current `b591ca66` source |
| CDT reference | `cdt-v1-hardening@714543c5a001ac4a9f5b2baa4b55fb5064dadd62` | Selected crypto headers only; unrelated working-tree changes excluded |
| Android wallet | `PureEOS-Wallet@397e40a856e3f1b0f2d63b4d6e9825a0ddf7609c` | Private source inspected; existing instrumentation modification excluded |
| History projection | `PureHistory-key-history-20261103@83080a452b804424bf172e9f6ccb87543debec73` | Private source metadata only is published |
| Explorer | `PureEOS-explorer-key-history-20261103@8f601a7fef2f00285d5cb936d8da1fa0209659b4` | Private source metadata only is published |

The recorded runtime has 25 registered BPs, 24 eligible BPs, 25 BP processes plus one history process, and an effective 21-member/15-vote native policy. Germany's existing permanent ban is retained. Registration, eligibility, process count and effective policy size are different quantities.

The source inventory covers 80 selected files across seven groups. At capture time all 80 selected files matched their local checkout commits; subsequent edits by other ongoing tasks are outside this snapshot. Fifty node files have entries in retained `test-governance-30min-r1/INPUTS.json`: 49 match; `controller.cpp` matches that manifest only at the parent of `057e17316`. The final commit changes the TEST governance activation callback to the explicit 1,800-second build profile. The discrepancy is explained, but that earlier manifest is not a complete proof of the final binary. Four other inventoried node files have no entry in that comparison. Details and both SHA-256/SHA-512 archival digests are in `SOURCE_AUDIT.json`.

Some source identities are local archival commits and are not publicly resolvable GitHub commits. Use the recorded path, checkout identity and digest rather than assuming every commit has a public URL. This is a static review of security-sensitive paths and selected functions, not an exhaustive line-by-line audit of all dependencies or an external security assessment.

## 3. Threat model and security invariants

Assume a future adversary can forge the currently used elliptic-curve and BLS signatures. Also consider malicious relays/RPCs/peers, malformed encodings, downgrade attempts, compromised historical keys, process crashes, stale snapshots and incomplete index coverage. Do not assume that an old public key remains secret or that hashing a public key permanently removes exposure.

The design still assumes sufficient honest effective voting weight, correct PQ primitives, protected endpoint secrets, and the ordinary safety assumptions of the consensus protocol. PQ cryptography does not solve a malicious quorum, malware controlling an unlocked wallet, transaction censorship, or a contract's business-logic bug.

Required invariants:

- **I1 — PQ necessity:** classical-only evidence cannot authorize an operation covered by strict mode.
- **I2 — complete binding:** proofs bind the actual semantic message, suite, role, chain and activation domain.
- **I3 — immutable replay identity:** randomized proof bytes cannot create a second executable identity for the same unsigned intent.
- **I4 — authority closure:** no weaker parent, delegated permission, linked action, recovery path or code upgrade can remove PQ protection unilaterally.
- **I5 — consensus continuity:** weights, thresholds, strong/weak semantics, active/pending checks and durable voting locks are preserved.
- **I6 — bounded work:** lengths, counts, decompression, memory and cryptographic work are bounded before expensive verification.
- **I7 — historical honesty:** pre-migration history remains legacy history anchored at an adopted checkpoint; it is not retroactively described as PQ-signed history.
- **I8 — no silent downgrade:** unsupported clients fail explicitly; neither a timeout nor an emergency enables classical-only acceptance.

## 4. Code review findings and required changes

Paths below are relative to the corresponding group in the source inventory. Findings describe observed code and resulting engineering requirements, not a claim that each is a presently exploitable bug.

| ID | Observed code / behavior | Consequence and required work |
|---|---|---|
| F01 | `libraries/libfc/include/fc/crypto/{public_key,signature}.hpp` contain K1/R1/WA variants; `src/crypto/public_key.cpp::recovery_visitor` calls `recover()` | ML-DSA must use explicit-key verification. Append versioned variants or add a v2 proof container; never reorder legacy tags or invent public-key recovery |
| F02 | `libraries/chain/transaction.cpp::{id,sig_digest,get_signature_keys}` use SHA-256 and a recovered-key set | Add a separate PQ transcript and authenticated-principal result, with a PQ replay identifier; retain old IDs as compatibility aliases |
| F03 | `transaction_metadata.{hpp,cpp}` asynchronously recovers signatures and estimates retained memory | PQ pre-verification must use immutable inputs, correct variable-byte accounting and context-complete caches; no mutable chainbase access from workers |
| F04 | `authority.hpp` has shared K1/R1/WA storage, key weights, account delegation and waits; `authorization_manager.cpp` gates key types | Update shared storage, billing, snapshots and permission traversal together. A hybrid principal counts once only after both components verify |
| F05 | `eosio_contract.cpp` handles `newaccount/updateauth/deleteauth/linkauth/unlinkauth`; governance seals protected permissions | Protect permission parents and relinking. Do not use ordinary `updateauth` to bypass sealed system authority |
| F06 | `webassembly/crypto.cpp` exposes recovery over a 32-byte digest and BLS primitives; CDT `crypto.hpp` mirrors legacy variants | Add a deterministic, bounded PQ verification intrinsic and matching CDT declarations; retain legacy behavior for historical replay |
| F07 | `producer_schedule.hpp`, `block_state.cpp::verify_signee`, `signed_block_header::producer_signature` use classical recovered keys | Version proposer policies and block proof encoding; add PQ-required producer verification, including additional signatures |
| F08 | `finalizer_authority.hpp` stores a BLS key; `finalizer_policy.hpp` diffs authorities and exports string keys | Version key registry, policy diff, string conversion, SHiP and snapshot formats together |
| F09 | `vote_message.hpp` carries block ID, strong/weak flag, BLS key and signature; `qc.cpp` aggregates keys | A first PQ QC should carry independent ML-DSA signatures indexed by policy members. Existing BLS aggregation is not a drop-in PQ construction |
| F10 | `qc.cpp::{verify_basic,verify_weights,verify_dual_finalizers_votes}` checks both policies and consistent dual-member votes | Preserve each effective policy's threshold and dual-member semantics. Bind proof vectors to exactly the authenticated bitsets |
| F11 | `finalizer.cpp` stores safety information by BLS identity with atomic write/sync/rename behavior | Introduce stable validator identity plus key epochs and a versioned safety file; transfer locks across rotation before publishing new votes |
| F12 | `types.hpp`, `block_header.cpp`, `merkle.hpp`, `block_header_state.cpp`, `finality_core.cpp` use SHA-256 commitments | Add a complete PQ commitment tree and parent linkage; signing a wider hash of an old 256-bit root does not restore lost binding strength |
| F13 | `upgrade_governance.cpp` authorizes `developer@upgrade`, BP vetoes, reviewed code/ABI hashes and a single-use execution capability | Migrate both authorization and payload commitments; preserve frozen electorates, ban exclusion, review periods and atomic execution |
| F14 | `protocol_feature_manager.cpp`, controller activation callbacks and `protocol_state_object` gate supported types/intrinsics | New node code and deterministic feature activation are required. `devexec` installs contract code/ABI; it does not install a node binary |
| F15 | Retained system `finalizer_key.cpp` registers BLS keys, validates possession and proposes a finalizer policy; `rotation.cpp` controls handover | Add role-separated PQ key registration/possession and policy formats while preserving rotation, maintenance, bond and ban state |
| F16 | `eosio.key.cpp::validate_key/prove/verify_envelope` require K1; proof CFD length is exactly 66 bytes, proof action data 102 bytes | Use a new protocol version with exact PQ lengths and a stateful required-suite check. Simply enlarging the old length check is insufficient |
| F17 | `eosio.key` stores full keys plus SHA-256 indexes; fees budget 1,024-byte wallet and 512-byte balance allocations | Recompute PQ storage admission and fee profiles; compare full-key storage against commitment-only storage plus repeated proof bytes |
| F18 | SDK/relay hard-code proof v2, K1 verification, code pins and a separately signed native resource packet; relay journals exact packets | Migrate owner and resource-payer proofs together; preserve immutable retries, rate/cap binding, owner epochs and sole-relay eligibility |
| F19 | Net handshake signs a SHA-256 hash of time; authenticated-peer mode recovers a classical key; `Any` mode permits public peers | Design fresh-challenge PQ peer authentication and version negotiation. Public peering is distinct from authenticated administrative identity |
| F20 | Wallet `Crypto/Wire/Vault/Network/TxStore` assume K1, fixed 32-byte IDs and pinned code; vault uses AES-256-GCM and PIN derivation | Version key/vault/wire/packet storage, preserve exact retries and report actual custody/signing capability separately |
| F21 | History/explorer enforce K1 formats, 120-character key limits, 64-hex hashes and current schemas | Version APIs, index mappings, identifiers, pagination and historical verification; never reinterpret old records under new rules |
| F22 | SHiP types, snapshots, block logs and genesis contain old IDs/policies; genesis validates BLS possession | Add versioned replay/bootstrap formats and an adopted PQ trust anchor. A fresh PQ genesis is a separate alternative, requiring separate authorization |
| F23 | Parallel input fingerprints and adoption identities include SHA-256 transaction/code digests | Audit every security-relevant cache/adoption binding. A hint may remain a hint only if collisions cannot affect authoritative validation |
| F24 | `config.hpp::max_finalizers` is 65,536; subjective variable-signature limits and chain resource caps already exist | Do not inherit this maximum blindly for a vector of kilobyte signatures. Define reviewed objective PQ budgets and qualify supported policy sizes |

## 5. Proposed cryptographic profiles

These are recommended design choices to review and freeze in P0/P1, not activated protocol assignments.

| Profile | Proposed use | Required property |
|---|---|---|
| `PQ-HYBRID-V1` | Migration of account and proposer principals | K1 AND ML-DSA-65, bound to the same registered composite principal and message |
| `PQ-STRICT-V1` | Strict account/proposer authorization | ML-DSA-65 is mandatory; a classical proof cannot substitute for it |
| `PQ-QC-V1` | Finalizer votes/certificates | Per-validator ML-DSA-65 proofs; temporary BLS compatibility must be conjunctive and use the same authenticated signer set |
| `PQ-COMMIT-V1` | New security commitments | Domain-separated SHA-512 of canonical full inputs |
| `PQ-RECOVERY-V1` | Optional offline recovery/release diversification | Separately reviewed SLH-DSA policy and parameters; not a weaker fallback accepted on failure |
| `PQ-CHANNEL-V1` | Authenticated confidential control channels | Reviewed hybrid key establishment using ML-KEM plus independent PQ authentication |

ML-DSA-65 is the proposed main signature algorithm. ML-DSA-44 is a size/cost comparison; SLH-DSA is a diversification candidate. ML-KEM establishes secrets and is not a transaction signature. Falcon/FN-DSA and research PQ aggregation are not prerequisites for the first implementation. NIST's current project page still identifies Falcon standardization as ongoing. Standards and errata must be pinned before implementation. [S1–S4]

Use a narrow library adapter with fixed algorithms and explicit errors. Evaluate the already vendored BoringSSL `MLDSA65_*` interface, a Java implementation suitable for the wallet, and an independent implementation for differential checks. A header's presence proves neither deployed support nor a validated cryptographic module. `liboqs` is useful for research; its own support statement limits production reliance. [S7–S8]

Signing may use approved randomized generation; validation must be deterministic for the same bytes. Do not demand identical randomized signatures across implementations. Use official test modes/vectors for exact-output comparisons and cross-verify independently generated valid signatures.

## 6. Hashes, identifiers and complete transcripts

SHA-256 is not broken in the same way as elliptic-curve signatures. Nevertheless, preimage and collision security are different properties, and quantum collision analysis cannot be reduced to “256 bits becomes 128 bits everywhere.” This plan recommends SHA-512 for new security commitments, subject to review of actual uses and implementation costs. It does not claim a blanket end-to-end bit-security proof. [S5–S6]

Required design:

1. Keep historical chain ID, block IDs, transaction IDs and old table indexes readable. Introduce typed `pq_block_commitment`, `pq_transaction_id`, `pq_policy_digest`, `pq_code_digest`, `pq_abi_digest`, `pq_review_digest` and `pq_checkpoint_digest` fields. New digests are 64 bytes; do not squeeze them into existing 32-byte types.
2. Each new hash starts with an unambiguous domain, suite/version, role and length-delimited canonical input. Merkle leaves, internal nodes, empty trees and collection lengths need distinct encodings. Bind order, indexes and proof lengths.
3. A PQ transaction identity covers the complete unsigned semantic transaction, full action/CFA descriptors and all semantic non-proof CFD. Authorization signature blobs are excluded to avoid signing cycles and replay identities varying with randomized signatures.
4. Owner proof and native resource-payer proof need explicit layering: the owner commits to the unsigned operation; the resource payer also binds the submitted owner proof and all charged data. No signature is included in its own signed input.
5. Native duplicate detection (`transaction_object`) must use the new authoritative identity for new transactions. Legacy aliases are secondary; an alias collision must never select or authorize the wrong transaction. Reject ambiguous legacy lookups explicitly.
6. New block security must commit to the raw/canonical transaction and receipt contents, full new policy descriptions and the previous PQ block commitment. Rehashing only an existing SHA-256 transaction ID, Merkle root or code hash is insufficient.
7. Extend finality validation trees, reversible-block commitments and proof verification to the new profile. Cover the fact that `action_mroot` has different legacy and Savanna meanings. Do not assume it is universally an action-receipt root.
8. TAPOS compatibility fields remain readable, but new authorization also binds an irreversible PQ checkpoint reference and its chain domain. Existing short prefixes cannot be the sole PQ replay/domain defense.
9. Treat RIPEMD-based four-byte key checksums as typo detection only. New address/key encodings need version, suite, network context, bounded parsing and an explicitly defined checksum; identity comparison uses the full canonical key or strong commitment.

Preferred signing API: pure ML-DSA over a canonical application envelope containing the SHA-512 commitments and context. If HashML-DSA is selected for an HSM/provider, assign a separate suite and implement its specified prehash identifier/context rules. Feeding a digest to pure ML-DSA must not be mislabeled HashML-DSA. [S1]

## 7. Native verification, encoding and resource accounting

Add explicit-key verification rather than extending the old recovery visitor with a fictitious recovery operation. Preserve all existing variant tags and serialized legacy bytes. A new proof includes a suite, principal/key epoch, canonical public-key material or an authenticated immutable key reference, and exact-sized signature components.

For the initial native transaction implementation, carrying the explicit public key with the proof is the simpler asynchronous-verification design. An authority-resolved key reference is an optimization only after a safe immutable main-thread capture has been specified. No worker may query mutable chainbase state to resolve a key.

Return verified principals to the authorization layer. A composite hybrid principal becomes valid only if both signatures verify against the registered pair; its weight is counted once. Do not put independently verified K1 and PQ keys into an ordinary weighted set and assume that implies an AND policy.

Add a context-free-safe PQ verification host function for pure byte verification and a matching CDT API. It must reject unknown suites, wrong fixed lengths, trailing bytes and noncanonical encodings; enforce bounded allocations before parsing; expose deterministic return semantics; check execution budgets; and perform no I/O or host randomness during verification. Keep policy lookup and mandatory-suite enforcement in the stateful authorization path.

Protocol activation must gate key types, authority storage, intrinsics, ABI codecs, snapshots and feature validation together. Review `num_supported_key_types`, signature CPU billing, variable-size accounting, queue estimates, packed transaction compression and uncompressed limits. Local admission policies may be stricter, but must not redefine consensus validity differently on different nodes.

Cache verification only with a key covering suite, full message/PQ commitment, full key identity, chain/activation domain, role and relevant key/policy epoch. After reorgs or activation transitions, stale policy-dependent results must not grant authorization. Never cache “valid for this SHA-256 transaction ID” alone.

## 8. Named-account authority closure

Migrate `owner`, `active`, all custom linked permissions, recovery authorities and delegated-account graphs as one authorization closure. An old owner that can replace a PQ active key defeats strict protection.

Introduce protocol-enforced account protection state and monotonic key epochs. During opt-in hybrid mode, all control-changing operations for a protected account require its protected policy. At strict activation, external state-changing authorization must satisfy PQ-required closure. Pure waits, classical delegates or a lower permission cannot satisfy a PQ-required threshold by themselves.

Preserve legitimate contract-code delegation: a system inline action does not need a new human signature for each call, but its authority must follow reviewed code and an authenticated PQ root or explicitly specified consensus-generated system action. Reject fabricated `eosio.code` authorization, arbitrary privileged forwarding and classic-only admin calls. Distinguish implicit `onblock` from externally signed transactions.

Sealed system accounts are special: the current native governance intentionally makes owner/custom authorities unsatisfiable. Do not reopen them or inject bootstrap keys. Add any necessary PQ policy or migration data through a separately reviewed native transition that preserves the sealed authority model.

The closure inventory must include permissions controlling token issuance, cap-changing code, account creation, BP registration/key changes, votes, bond withdrawal/settlement administration, maintenance, relay configuration and upgrade/veto actions. Changing signature rules must not change their economic or eligibility semantics.

## 9. `eosio.key` v3, receiving and fee migration

Use a new v3 proof/action/schema family or append-only extension tables, with an explicit required authorization suite per wallet and key epoch. Existing v2 rows and receipt interpretation remain valid under historical rules.

The current `prove()` is context-free and cannot read wallet policy. A v3 proof must authenticate the full descriptor supplied in canonical transaction data; stateful `verify_envelope()` must then match that descriptor and suite to the wallet's stored policy. Reject v2 `ksend`, `withdraw`, `selfout`, `cancel`, `selfcancel` and policy-changing bypasses for a wallet once PQ protection requires v3.

Preserve no shared sequential nonce allocator. Multiple independent devices continue to sign distinct complete transactions. Keep revocation epochs, exact native/PQ transaction identity, expiry and atomic replay rejection. Re-signing the same unsigned intent must not create another payment. Treat request IDs as bound correlation values, not a replacement for complete native duplicate protection.

Compare two storage designs before freezing v3:

| Design | Benefit | Cost / condition |
|---|---|---|
| Full canonical PQ key in authorization table | Resolve a stable key without retransmitting it for every operation | Higher sponsored RAM; bounded migration storage must be funded |
| Strong key commitment in table; key supplied per proof | Smaller persistent row | Additional 1,952-byte ML-DSA-65 public key in each proof; full-key comparison and commitment verification mandatory |

Locally generating a PQ wallet remains free and requires no named account. A first actual receipt still requires funded on-chain RAM admission and sender-paid costs. The old 1,536-byte new-key/balance estimate cannot be assumed sufficient for a full 1,952-byte PQ public key plus row metadata.

Receiving also needs a concrete protocol change: current memo/key parsers cannot carry a kilobyte key as if it were a short K1 string. Design a reviewed deposit envelope that places the complete descriptor in action data and uses a short, versioned reference in the transfer memo, with atomic sender authorization, admission and principal credit. Preserve receipt-without-recipient-signature semantics. Define tests for ambiguous notifications, inline calls, mismatched descriptors and rollback; do not rely on an unverified preceding registration action.

Requalify allocation bounds, RAM-curve quote, execution work and signed fee caps. Keep the current 0.0100 AIC floor, approved relay range, full principal credit, reviewed AIC/USD identities, sponsored budgets, native USD pre-open rule and application-fee-free income claims unless a separate user-approved economic change occurs. A cryptographic migration is not permission to silently raise fee rules or buy RAM automatically.

Existing system/token/code pins must be changed only with matching reviewed versions in contract, SDK, relay, wallet and history projection. A system upgrade may otherwise intentionally make current clients fail closed. Plan the overlap and user-facing availability before activation.

## 10. Proposer keys and signer custody

Add distinct PQ roles for account management, block proposals, finalizer votes, P2P identity, relay resource signing and releases. Use independent randomness and keys; do not derive PQ secret keys from a legacy elliptic-curve secret that a future adversary could recover.

Version proposer authorities and their shared representation. Bind block signatures to the new block commitment, chain/activation domain, proposer identity, schedule version, key epoch and suite. Validate additional-signature extensions and weighted multi-key rules consistently. Old blocks must replay with their original rules; new strict-era blocks must never pass using the old producer signature alone.

Extend signature-provider/keosd interfaces beyond an implicit 32-byte K1 digest. Use authenticated, domain-specific signing requests so a remote signer cannot be turned into a cross-protocol signing oracle. Hardware support must be measured per provider; software signing with encrypted storage must not be described as hardware-contained signing.

Preserve producer watermarks, anti-double-sign behavior and finalizer durable locks. Rotation to a PQ key must inherit the validator's relevant safety history. Key generation is not a reason to initialize a live validator with an empty safety record.

## 11. PQ finality and certificate design

The first implementation should retain the consensus algorithm and represent a QC as individually verifiable ML-DSA signatures plus policy-indexed voter bitsets. Research aggregation/threshold signatures separately; do not make an unqualified construction a prerequisite for correctness.

Proposed vote transcript fields: chain domain, activation checkpoint, suite/version, stable validator identity, signing-key epoch, block height, PQ block/finality commitment, active policy generation/digest, pending policy generation/digest if present, and strong/weak role. Precisely specify whether a single vote transcript authenticates both policies; this is the recommended approach for dual members and must be proved consistent with existing vote semantics.

Required QC checks:

1. Select the historical active and pending policies associated with the certified block, not current head membership.
2. Require exact bitset lengths, canonical policy ordering and one authenticated proof per represented member. Reject duplicate indexes, substituted keys, extra signatures and strong/weak overlap.
3. In hybrid mode, BLS and PQ components must authenticate the same selected member set and vote roles. Separate unrelated quorums are not the proposed hybrid rule.
4. Sum verified weights once per member; compare with each policy's recorded threshold. Normal equal-weight finality is `floor(2N/3)+1`, currently 15 of 21. Governance veto uses a different formula, `ceil(2N/3)`.
5. If a pending policy exists, require its proof and threshold as well. Preserve consistency for validators appearing in both policies.
6. Include the new policy/key commitments in the signed finality dependency chain; a PQ signature must not bind only a legacy BLS-key policy hash.
7. Preserve weak-vote behavior, locking, liveness tests and irreversible handover. A missing PQ proof must not be reclassified as a valid weak vote.
8. Verify and persist safety-state changes before publishing votes, with fault-tested atomic replacement, directory synchronization and version checks. Preserve inactive-key safety records and key-epoch history.

`max_finalizers=65,536` is a legacy structural maximum, not a qualified PQ operating size. At that count, signatures alone would be 216,858,624 bytes per full ML-DSA-65 policy certificate. Define objective per-feature proof/count/byte budgets and qualify actual operating sizes. Any new consensus limit requires explicit protocol review; do not silently change the current candidate-capacity or future active-BP-growth policy.

## 12. Governance and protocol upgrade bootstrap

Review the entire control path: `developer@upgrade`, its parents, BP veto authorizations, `refreshgov`, `devpropose`, `devcancel`, `devexec`, protected code/ABI capability checks, feature preactivation and node release distribution.

Current governance binds old/new code, ABI and review-document SHA-256 hashes and executes contract `setcode/setabi` through a single-use capability. Add separately versioned strong payload commitments; retain exact-byte checks and atomic rollback. Preserve existing proposal deadlines and frozen electorates, banned-voter exclusion, duplicate-veto rejection and empty-effective-electorate blocking. Production review stays seven days; the selected TEST build stays 1,800 seconds. Legacy historical periods remain historical facts.

There is a bootstrap dependency: deployed old binaries do not understand PQ native permissions or a new QC. The proposed in-place route is:

1. Qualify a preparatory node release with inactive PQ features, exact compatibility behavior and explicit migration/activation rules. Operators install it only in a separately authorized rollout.
2. Migrate developer and BP control identities before PQ enforcement, using existing legitimate authority and PQ possession proofs, while classical authority is still assumed safe. The enrollment interval is not a defense after classical forgery already exists.
3. Where a sealed contract needs new actions, qualify the exact system/key candidates and use the existing reviewed governance route. Do not restore `eosio` owner, legacy multisig or a direct `setcode` bypass.
4. Define a native, reviewed activation record that binds the full release manifest, feature set, checkpoint, required registries, resource profile and irreversible activation condition. Its authorization and veto procedure must be specified and audited; it does not currently exist merely because this plan names it.
5. Require PQ-ready active and pending policies and upgraded validators before enforcing new proofs. Unsupported binaries cannot be counted as ready based only on an operator's statement.

`devexec` does not update node binaries, and a Git push does not approve protocol activation. Any native migration capability must be narrowly scoped, phase-gated, replay-safe, auditable and unavailable as an emergency arbitrary-state editor.

## 13. Migration state machine and unmigrated assets

Use on-chain irreversible state and feature dependencies, not local clocks or a browser setting.

| State | Accepted behavior | Required transition condition |
|---|---|---|
| `LEGACY` | Current rules; no PQ claims | Preparatory releases and exact source provenance qualified |
| `REGISTRY_READY` | PQ keys/possession records can be enrolled | Developer/BP roles registered; clients decode new versions |
| `HYBRID_OPT_IN` | Explicitly protected accounts require AND proofs; others remain labeled legacy | No downgrade or owner/delegate bypass for protected accounts |
| `CONSENSUS_HYBRID` | Proposals/QCs require matched classical and PQ evidence | Both effective policies ready; safety records migrated; all validator checks qualified |
| `STRICT_SCHEDULED` | Exact checkpoint/activation condition and policy snapshot published | Governance, contracts, clients, resource limits and recovery plans meet all gates |
| `PQ_STRICT` | PQ proof mandatory for all covered external control and consensus operations | Irreversible activation; no classic-only write path |
| `PQ_AGILE` | A later reviewed PQ suite can replace/augment the first | Equally strong authenticated transition; no catch-all fallback |

Activation heights/times, enrollment duration and concrete rollout dates remain unselected. The proposal must specify handling of pending transactions, queued relay jobs, cross-boundary expirations, reversible blocks and accounts changed during the readiness census.

Unmigrated accounts cannot be assigned a new PQ secret by the chain. Recommended strict-mode policy: preserve balances and ownership records, but reject legacy-only spending/control until an independently pre-enrolled PQ recovery policy can be satisfied. This may freeze abandoned assets; the economic/user policy needs explicit approval before strict activation. Do not confiscate, silently sweep, auto-register substitute keys or retain a classical-only recovery door. Mere possession of a legacy private key after classical forgery is possible is not a trustworthy recovery proof.

Existing smart-contract authorization also needs a census. A contract that implements its own K1 recovery may expose assets despite PQ native accounts. Qualify all deployed contracts within the security claim, migrate their control paths and label unsupported applications. Absolute safety of arbitrary new contract logic is outside the guarantee of a signature-layer upgrade.

## 14. Historical checkpoint, replay and light clients

Create an independently adopted PQ checkpoint containing: legacy chain ID/height/block ID, new full checkpoint commitment, deterministic state commitment, active/pending proposer and finalizer policies, key epochs, activated feature set, code/ABI manifest, archive coverage and the new trust roots. Obtain PQ-required authorization under the reviewed transition and distribute the exact checkpoint through independently authenticated channels.

A native authenticated state commitment is required work; do not assume the existing node snapshot is already a canonical state-proof tree. Specify account/permission/table/code/system-state ordering, resource state, pending proposals, schedules and exclusions. Cross-check deterministic snapshot export against full replay before trusting it for state verification.

Maintain versioned decoding and validity rules by block range. Replaying a legacy block with legacy signatures is historical validation, not permission for a new strict-era legacy transaction. A new node must reject a forged post-checkpoint fork even if all classical keys on that fork appear valid.

For old history, a PQ checkpoint authenticates the state/archive adopted at that point, under the checkpoint trust assumption. It cannot prove that every pre-checkpoint classical signature was quantum-resistant, reconstruct missing raw history or recover authenticity already lost before enrollment. Retired high-spec raw databases were intentionally not retained; do not claim they can be recreated from the evidence kit.

Add new block/finality/receipt proof formats and light-client verification of policy handovers from the pinned checkpoint. RPC `get_info`, an index's “irreversible” label or a code-hash response alone is not a cryptographic proof. Browser consumers unable to verify the new chain must show a bounded trust/unsupported state rather than claim independent PQ verification.

## 15. P2P, TLS, RPC, relay and administrative channels

P2P consensus validation and transport confidentiality are different workstreams. A forged peer identity must not make an invalid block/QC acceptable. Retain resource limits for anonymous/public peers even after authenticated peer mode is added.

Replace timestamp-only authentication with a transcript binding fresh mutual challenges, both endpoints, chain/activation domain, supported protocol versions, chosen suite, key epoch and ephemeral key-exchange material. Authenticate suite negotiation to prevent stripping PQ support. Define replay windows, key revocation, rate limits and bounded message frames.

For confidential channels, evaluate a standard, reviewed hybrid key establishment using ML-KEM and a classical component. Do not invent a KEM combiner or treat ML-KEM as peer authentication. Protect against active forgery as well as passive record-now/decrypt-later attacks. Pin the actual standardized transport specification and implementation revision during P1/P7; this plan does not assert that every browser/TLS stack provides the same PQ capabilities. [S3–S4]

RPC/relay readiness should include PQ-authenticated application manifests and proofs bound to client challenges and verified chain checkpoints. HTTPS with a PQ key exchange but a classical-only server-authentication chain is not automatically fully PQ-authenticated against an active quantum adversary.

Keep the founder as the only active HTTP relay during the current project. A future PQ relay version must migrate its resource permission, parent authority, exact-packet journal, fee/profile pins, request size limits and status identity together. No additional relay service is needed for this documentation task.

Review SSH/CI/CD/cloud credentials, backup encryption, remote signer access, DNS/certificate dependencies and deployment manifests. If a deployment path still permits a classical-only attacker to replace a validator or wallet, record that as a blocker to an end-to-end claim, even when the chain protocol itself passes strict-mode tests.

## 16. Wallet, backup and software distribution

The Android wallet currently supports K1 encodings and API 26+, with target SDK 36 in the inspected source. New key records require suite, key epoch, public descriptor, encrypted seed/private format, binding status and verified on-chain policy. Preserve multiple independent local wallets and the public-key/named-account UI.

Use Java/native interoperability tests for the same canonical message, key and context. Android's current reference documents ML-DSA identifiers beginning at API 37; that is not proof of hardware support on existing devices. Provide software signing with encrypted custody only where qualified, and label signing location separately from the hardware/software security of the wrapping AES key. [S9]

Version the vault and backup format. Independently generated PQ secrets need a real recovery/export design; the old 32-byte WIF format cannot simply carry an arbitrary PQ secret. A compact seed backup is acceptable only with a pinned algorithm/derivation version, sufficient entropy, cross-implementation recovery tests and explicit user explanation. Never derive the new recovery seed from a K1 secret.

Retain AES-256-GCM, unique nonces, authenticated metadata, PIN hardening, lock-on-background, `FLAG_SECURE`, no system/cloud backup and encrypted pending packets. A short PIN does not become a high-entropy PQ key. Preserve failure/backoff semantics and honest reporting of the device's actual protection level. Secure erasure limitations of managed-language copies remain an endpoint concern.

`TxStore` must retain suite, authoritative PQ ID, legacy alias, exact signed packet, expiry, reference checkpoint and unresolved status. Never regenerate a transaction merely because a new signer produces different randomized bytes or a relay times out. Cross-activation retry rules need explicit reconciliation.

Publish PQ-signed release manifests covering APK/node/toolchain/dependency hashes, versions and update constraints, with pre-established trust roots. Existing Git/PGP/APK signatures may remain platform compatibility mechanisms. A standard Android package installer or OS verified-boot chain is not made PQ-aware by an app-level manifest; document that external boundary and qualify the independent update verifier/managed distribution path before an end-to-end claim.

## 17. History, APIs, explorer and external consumers

Version native RPC, SHiP ABI/messages, finality-data export, index schemas, receipt APIs and SDK types. Do not change a 64-hex field's meaning in place. Return explicit `legacy_id`, authoritative PQ identifier, suite, key/policy epoch, block range and proof/coverage metadata.

History stores and serves canonical native records. Reindex new fields into a separate versioned projection without mixing chain IDs or rewriting old asset events. Preserve exact integers, fee attribution and all existing history. A complete security claim requires actual canonical coverage, not an indexer's head height.

The explorer must identify legacy, hybrid and strict-era records, distinguish a current policy from the policy at execution, and avoid labeling historical unknowns as PQ-protected. Key search, QR/address parsing, table pagination, code pins, export limits and bilingual messages need new schema support. Keep private signing material out of read-only explorer/history services.

External SDKs, exchanges, bridges and light clients must acknowledge the new proof format and checkpoint trust boundary. A bridge that still trusts only an old BLS certificate remains a classical dependency. Unsupported consumers must not silently fall back to a legacy-only confirmation rule.

## 18. Cost model and bounded qualification

The following are byte arithmetic from standardized ML-DSA sizes, not measured project throughput or latency. [S1]

| Item | ML-DSA-65 bytes / consequence |
|---|---|
| Public key | 1,952 |
| Signature | 3,309 |
| Public key plus signature | 5,261, before framing/classical proof |
| 15 independent finalizer signatures | 49,635, about 48.5 KiB |
| 21 independent finalizer signatures | 69,489, about 67.9 KiB |
| Two policies, 15 signatures each | 99,270 before framing; overlap reuse needs an exact transcript rule |
| Two full 21-member signature vectors | 138,978 before framing |
| One million signatures | 3.309 GB decimal per retained copy |
| One million public-key-plus-signature packets | 5.261 GB decimal before other transaction data |

Measure actual framing, policy deltas, proposer proofs, CFD/native NET accounting, disk/index amplification and connection fan-out. Current recorded block NET is 1 MiB and transaction CPU limit is 150,000 microseconds; those are existing settings, not authorization to raise them. Establish exactly where QC bytes are charged. Do not infer a TPS figure by dividing one resource cap by signature size.

The retained host's recorded hardware is a shared Intel Cascadelake VM with four vCPUs, two guest-visible cores and 8,125,768 KiB RAM. Dedicated physical-core ownership is not established. The external Intel benchmark in S10 uses different hardware and cannot qualify this host, the Android Java path or contract WASM.

Approximate accounting model:

```text
CPU per node = transaction_count × verified_proofs_per_transaction × measured_verify_cost
             + vote/QC verification + hashing/serialization + execution + persistence
CPU on shared host = sum(actual work of each node process) + history/relay/business work
network/disk cost = actual encoded bytes × actual fan-out/retained-copy count
```

A transaction is not necessarily verified 26 times on every path, but co-located nodes often repeat substantial validation; profile actual paths rather than assuming shared work. Include invalid proofs, rejection-sampling signing tails, p50/p95/p99, queue wait, peak RSS, CPU cycles, wall time and finality latency. A fast optimized native C benchmark is not the cost of Java or WASM verification.

Future measurements run remotely only, in isolated directories/ports with explicit CPU/memory/time limits, low priority and separate disposable keys. No new server or live TPS test is authorized. If the retained cheap host lacks safe spare capacity, stop the qualification stage and report the resource dependency; do not pause BPs or hide overload by changing caps. Historical 35-BP capacity failure is not requalified by this plan.

## 19. Work packages and dependency gates

No calendar promise is made before provenance, protocol design and resource feasibility are resolved. Each package ends with reviewable source, hashes, test evidence and a precise remaining-risk statement.

| Package | Main work / deliverable | Depends on | Exit gate |
|---|---|---|---|
| P0 | Freeze exact node/system/key/CDT/client sources, dependencies, live-state census and decisions | Separate engineering authorization | G0: reproducible baseline, complete authority/contract inventory; resolve exact system mapping |
| P1 | PQ library adapter, vectors, independent C++/Java cross-verification, signer custody capability matrix | P0 | G1: fixed suite/errata/encoding; malformed-input and interoperability checks pass |
| P2 | SHA-512 commitment graph, canonical transcripts, new IDs, replay index and wire/ABI formats | P1 | G2: full-input binding and no signature-cycle/replay ambiguity |
| P3 | Native permission closure, activation gates, deterministic host API and CDT types | P2 | G3: owner/delegate/link/code paths cannot bypass PQ requirement |
| P4 | `eosio.key` v3, deposits, fees, SDK/relay/resource-sponsor migration and wallet prototype | P3 | G4: all asset paths reconcile exactly; no v2 bypass or duplicate payment |
| P5 | PQ proposer/finalizer registry, matched hybrid QC, policy handover and safety-file migration | P2–P3 | G5: safety/liveness and active/pending transition evidence; qualified proof budgets |
| P6 | Governance PQ closure, strong payload review, preparatory node activation state machine and unmigrated-asset rules | P3–P5 | G6: no classic-only upgrade/recovery; activation and failure semantics reviewed |
| P7 | Checkpoint/state proofs, SHiP/history/explorer/SDKs, authenticated channels and PQ release manifests | P2, P4–P6 | G7: replay/light-client and cross-service proof verification; dependencies honestly labeled |
| P8 | Isolated multi-node adversarial qualification, bounded resource observations and independent security review | P1–P7 | G8: all required test families, findings closure and exact release candidates |
| P9 | Separately authorized staged rollout, hybrid observation, strict activation and post-activation evidence | G0–G8 | G9: irreversible strict operation and no legacy-only path within claimed scope |

P0's documentation subset is completed by this R1 review. Engineering baseline reproduction, live census and all P1–P9 implementation/qualification remain unperformed in this task. Functional pass counts from earlier unrelated campaigns are not PQ qualification.

## 20. Required verification matrix

All entries below are future acceptance tests, not executed results.

| ID | Test family | Required evidence |
|---|---|---|
| T01 | Official vectors and current errata | Pinned versions; positive/negative vectors; no draft-Dilithium/final-ML-DSA confusion |
| T02 | Independent C++/Java/device implementations | Cross-verification of key generation, contexts, seeds where standardized, canonical messages and valid randomized signatures |
| T03 | Parser limits | Wrong/truncated/oversized lengths, trailing bytes, unknown tags, duplicate fields, integer/varint boundaries and compressed bombs rejected before excessive allocation |
| T04 | Hybrid downgrade | Missing either component, swapped key pairs, OR-threshold tricks, suite stripping and legacy endpoint retries rejected |
| T05 | Domain substitution | Cross-chain, activation, deployment, role, operation and checkpoint swaps rejected |
| T06 | Semantic tampering | Recipient, token/precision, amount, memo, selected BP, reward destination, fee/rate cap and expiry changes rejected |
| T07 | Replay and malleability | Same unsigned intent with different randomized proof rejected as duplicate; exact packet retry reconciles once |
| T08 | Independent devices | Concurrent distinct transfers succeed without common sequential nonce allocation; epoch revocation invalidates old authorization |
| T09 | Permission closure | Old owner, delegated account, waits, custom links, deletion/relinking and nested authorities cannot bypass strict mode |
| T10 | Contract execution | Legitimate `eosio.code`/implicit system paths work; forged authorization, privileged forwarding, inline and synchronous-call bypasses fail |
| T11 | Protected system migration | Sealed permissions stay sealed; native migration capability is exact, scoped, single-use where applicable and replay-safe |
| T12 | Public-key deposits | First receipt, full-key/commitment mismatch, notification ambiguity, sponsored RAM exhaustion and complete atomic rollback |
| T13 | Asset invariants | AIC/USD identity, principal, balances, reserves, fee caps, rounding, RAM admission and free claims preserved |
| T14 | Native resource sponsor | Resource packet binds owner proof and data; sponsor/parent permissions require PQ in strict mode |
| T15 | Proposer authentication | Wrong schedule/key epoch, missing proof, additional-signature substitution and classical-only new block rejected |
| T16 | QC correctness | Actual weights, exact bitsets, duplicate/missing/extra proofs, zero/overflow conditions and signer-set mismatch |
| T17 | Active/pending handover | Both thresholds, consistent dual-member roles, old/new key epochs, one-seat rotation and no reduced quorum |
| T18 | Vote safety / liveness | Strong/weak substitution, conflicting forks, withheld PQ components, delayed votes and preserved locking behavior |
| T19 | Crash durability | Kill before/after write, fsync, rename and publication; key rotation/restart never resets safety or double-sign protection |
| T20 | Governance | Mature execution only; exact payloads; frozen electorate; current ban exclusion; duplicate/late veto; empty electorate; existing deadlines preserved |
| T21 | Feature boundaries | Pre/post activation transactions, reorgs, pending jobs, expirations, unsupported binaries and no local-clock consensus decisions |
| T22 | Historical replay / snapshots | Legacy replay, versioned PQ replay, checkpoint root agreement, corrupt snapshots and substituted policy/history rejection |
| T23 | Long-range / light client | Forged classical post-checkpoint history rejected; policy changes and receipts verified from the adopted PQ anchor |
| T24 | Parallel execution / caches | Serial-vs-parallel equivalence; stale key/policy cache; SHA-256 alias collision fixtures; adoption cannot bypass verification |
| T25 | Peer/channel authentication | Freshness, replay, downgrade, revoked identity, MITM and invalid-message flooding under bounded queues |
| T26 | RPC/history/explorer | Schema/pin transitions, chain mismatch, missing coverage, authoritative PQ IDs, exact integers and unknown-state handling |
| T27 | Wallet custody / recovery | Real Android capability, vault upgrade/rollback, backup restore, lock/background behavior and protected pending packets |
| T28 | Release/update trust | PQ manifest verification, rollback/substitution, compromised classic-only distribution path and honest platform limitations |
| T29 | Bounded resources | Native/Java/WASM valid/invalid costs, p99 signing/verification, queues/RSS/disk/network and finality under approved low-rate work |
| T30 | Existing protocol regression | Votes/bonds/bans/maintenance/rewards/caps/deferred settings, ordinary activity, fee rules and historical records unchanged |

Use synthetic collision/forgery fixtures to exercise control flow; do not claim to have broken real cryptography. Isolated fault tests must never stop, reset or corrupt the live 26-node chain. A real performance/TPS campaign, extra server or deliberately disrupted public chain needs its own explicit authorization.

## 21. Future rollout runbook and failure handling

This is a review sequence for a later authorized deployment, not an executable controller.

1. Reconfirm exact runtime/source mappings and current service/PID, policy, key-role, supply, bond, ban and history baselines without exporting secrets.
2. Freeze the suite, encodings, new proof budgets, supported resource profile, migration interval and treatment of unmigrated assets.
3. Publish exact candidate sources, build/toolchain/dependency manifests, PQ release proofs, test evidence and independent review findings.
4. Verify all nodes/clients/history readers can replay old data and decode the prepared formats with features inactive.
5. Use a separately reviewed operator rollout that retains quorum and safety files. Do not extrapolate the current “preserve PIDs” documentation constraint into a claim that a binary can be upgraded without process replacement.
6. Enroll independent PQ keys and possession proofs for every required BP/control role; inspect parent/delegation closure and any pending policy.
7. Complete the applicable native review/veto process for exact contract candidates. Resolve any existing pending-proposal conflict only through a separately authorized action; do not alter proposal #3 here.
8. Run stateful readiness checks and compare deterministic checkpoint/state commitments across validators.
9. Enter opt-in/hybrid operation only after the specified irreversible prerequisites. Report actual coverage; remaining legacy accounts are not PQ-protected.
10. Verify hybrid proposer/QC behavior, real irreversible handovers, key-epoch inheritance and client verification over the approved observation interval.
11. Obtain the separately approved strict activation record through its reviewed governance process. Recheck live permissions/policies at the transition, not just an earlier readiness snapshot.
12. Activate according to the committed chain condition. Reject classic-only writes/proposals/QCs in the strict range, while retaining historical decoders.
13. Reconcile all pending packets, balances, reserves, proposals, votes, bans and histories; retain failures and raw evidence before bounded cleanup.
14. Publish final source/artifact digests, exact activation block/checkpoint, tested hardware/scope, unresolved external assumptions and verification instructions.

Before enforcement, a rollout may be deferred by the prescribed governance procedure without falsifying PQ readiness. After irreversible enforcement, recovery must preserve PQ-required authorization and the finalized history; ship a reviewed forward fix. An old binary rollback, lowered quorum, erased safety file or classic-only emergency key is not a safe universal recovery mechanism. Define a PQ-protected recovery policy in advance. If quorum/key custody cannot be recovered safely, report the halt rather than inventing a bypass.

## 22. Decisions and blockers before engineering/activation

| ID | Decision / blocker | Recommended position and required resolution |
|---|---|---|
| D01 | Exact deployed system provenance | Reconstruct and reproduce the `b591ca66` source/toolchain mapping before implementation baseline approval |
| D02 | Final suite and security claim | Review ML-DSA-65 plus SHA-512 profile, contexts and current errata; distinguish primitive category from system security |
| D03 | Hybrid principal / explicit-key wire format | Prefer atomic composite principal semantics and explicit-key first implementation; freeze canonical bytes and bounds |
| D04 | PQ authority closure semantics | Specify parent/delegate/wait/code rules, sealed accounts and audit all protected state-changing roles |
| D05 | PQ QC and supported policy size | Independent signatures first; prove dual-policy safety and choose objective budgets from real measurements |
| D06 | Activation/governance bootstrap | Specify inactive preparatory binaries and a narrow native transition; preserve current review/veto guarantees |
| D07 | Unmigrated assets / enrollment window | Preserve funds; no classic-only recovery after strict cutoff; obtain explicit policy approval before scheduling |
| D08 | State checkpoint / history trust | Implement deterministic state commitment and independent anchor adoption; state missing-history limits honestly |
| D09 | Fee/RAM admission | Qualify new row sizes and proof costs; preserve existing economics unless separately changed |
| D10 | Custody and recovery | Qualify software/hardware signers, independent PQ backups and a PQ-authorized emergency policy |
| D11 | Transport/platform dependencies | Pin interoperable channel specs; resolve classic-only update/admin dependencies or narrow the claim |
| D12 | Safe remote resources / independent review | No implementation capacity or audit is assumed available; qualify without disturbing current services |

No activation date is selected. Future active-BP growth parameters are also unselected and must not be coupled to this cryptographic migration. Existing 21/15 evidence does not qualify a larger committee or a new resource cap.

## 23. Definition of done and required artifacts

Strict protocol completion requires G0–G9 and T01–T30 evidence for the selected scope, no unresolved critical/high security finding, no accepted classical-only control/consensus route, canonical PQ checkpoint/proofs, and verified history/client behavior across activation. Any approved exception must narrow the published claim; it cannot be hidden behind “fully PQ.”

The handover kit must include: bilingual protocol/encoding specifications; source and dependency identities; authority/contract census; suite registry and vectors; reproducible node/CDT/contracts/wallet artifacts; migration/checkpoint manifests; key-custody capability matrix without secrets; complete positive/negative and fault-test evidence; resource reports on actual hardware; independent review and disposition; approved activation record; final canonical receipts; and a PQ-preserving recovery guide.

For an end-to-end claim, additionally resolve update/install/admin trust, service authentication and external verifier dependencies. Otherwise publish the narrower, accurate statement that the chain's specified strict protocol boundary is PQ-required, with explicit endpoint/platform assumptions.

For this R1 documentation delivery, completion means both plans and the metadata inventory are reviewed for consistency, only intended documentation is committed, the new branch is pushed to `MatrixCoreX/PureEOS`, and the remote commit is verified. No benchmark, APK modification, live-chain change or PQ production claim belongs to that completion statement.

## 24. Primary references and project evidence

Standards and primary references were checked on 2026-11-04. Engineering must pin their exact revisions and errata again when freezing a suite.

- **S1:** [NIST FIPS 204 — ML-DSA](https://csrc.nist.gov/pubs/fips/204/final), including the current errata notice; [full standard](https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.204.pdf), especially size table and pure/prehash distinction.
- **S2:** [NIST FIPS 205 — SLH-DSA](https://csrc.nist.gov/pubs/fips/205/final).
- **S3:** [NIST FIPS 203 — ML-KEM](https://csrc.nist.gov/pubs/fips/203/final), including its errata notice.
- **S4:** [NIST PQC project status](https://csrc.nist.gov/projects/post-quantum-cryptography); [RFC 9794 — PQ/traditional hybrid terminology](https://www.rfc-editor.org/rfc/rfc9794.html). RFC 9794 defines terminology; it is not a complete PureEOS construction or security proof.
- **S5:** [NIST FIPS 180-4 — Secure Hash Standard](https://csrc.nist.gov/pubs/fips/180-4/upd1/final); [NIST key-management guidance](https://csrc.nist.gov/pubs/sp/800/57/pt1/r5/final).
- **S6:** [Brassard, Høyer and Tapp — Quantum Algorithm for the Collision Problem](https://arxiv.org/abs/quant-ph/9705002). Its query-model result is not a real-world attack-cost estimate for this chain.
- **S7:** [Open Quantum Safe ML-DSA implementation/size information](https://openquantumsafe.org/liboqs/algorithms/sig/ml-dsa.html); [liboqs support limitations](https://github.com/open-quantum-safe/liboqs#limitations-and-security).
- **S8:** [Bouncy Castle Java ML-DSA implementation documentation](https://downloads.bouncycastle.org/java/docs/bcprov-jdk15to18-javadoc/org/bouncycastle/pqc/crypto/mldsa/package-summary.html). Qualify the exact wallet dependency revision rather than assuming this documentation is a device certification.
- **S9:** [Android KeyProperties ML-DSA identifiers](https://developer.android.com/reference/android/security/keystore/KeyProperties#KEY_ALGORITHM_ML_DSA_65).
- **S10:** [Intel ML-DSA optimization measurements](https://builders.intel.com/docs/networkbuilders/post-quantum-cryptography-accelerating-open-quantum-safe-library-with-intel-avx-512-keccak-1600-implementation-1762253806.pdf). External implementation/hardware reference only; not project qualification.

Project evidence is the source inventory plus retained `test-governance-30min-r1/{INPUTS,NATIVE_ARTIFACT,ARTIFACT_RECEIVED}.json`, the `057e17316` controller correction, and the existing actual-Cascadelake RAM-activation/proposal-history reports. Preserve their original scope and hashes. Private application source, private keys, demo fixtures and APK signing material are not included in this documentation branch.
