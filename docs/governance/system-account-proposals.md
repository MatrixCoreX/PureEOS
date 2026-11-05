# System-account creation candidate / 系统账号创建候选

Status: **CONTINUATION REPAIR PASSED 38 REMOTE ISOLATED TEST CASES, NOT ACTIVATED**. This branch is based on
`057e1731660d3b8423c960cb81fe1758ae9c29f6`. Installing source or a binary is
insufficient to activate this protocol feature. No reset is part of this design.

## Purpose and privilege boundary

The current sealed chain rejects all new `eosio.*` accounts. Existing native
`devpropose/devexec` can replace reviewed code/ABI on an existing protected
account, but cannot create a missing account. This candidate adds a separate
creation lane while retaining the old pending upgrade, including proposal3.

`privileged` is an account-metadata flag enforced by native host interfaces; it
is independent of `owner`, `active`, and the account-name prefix. The new lane
always creates **nonprivileged** accounts. It does not grant a developer key
permission to create accounts directly or call privileged chain-management
interfaces. `eosio.signup` only needs ordinary newaccount/RAM/delegation actions.

## Reviewed manifest and actions

The new built-in feature is `PUREEOS_SYSTEM_ACCOUNT_GOVERNANCE` (enum500003),
dependent on `PUREEOS_SEVEN_DAY_GOVERNANCE`. The feature specification's digest
must be obtained from the qualified binary, never guessed from its codename.
Before activation the handlers preserve historical unknown-action behavior;
they grant no creation capability and change no creation-proposal state.

Only one creation proposal can be pending. The native singleton records:

- target, existing resource payer, exact RAM payment in4-decimal AIC;
- authority-schema version1, exact code/ABI sizes and SHA256 hashes;
- public review-document hash and the system/token code hashes at submission;
- monotonically increasing proposal id, native proposal/deadline times;
- frozen electorate, recorded vetoes, explicit completion status/time.

`syspropose` requires the designated `developer@upgrade`, immediately after
the real system contract's `refreshgov` in the same transaction. The candidate
bounds WASM at16MiB, raw ABI at1MiB, bootstrap payment at10000.0000AIC, and the
protected-account list at64. These are upper bounds, not approved spending.
The proposed account must be absent, named `eosio.*`, and at most12 characters.
Reserved legacy governance names cannot be recreated. A protected system
account cannot be selected as the resource payer.

`sysveto` requires the BP's authorization. The electorate is frozen at proposal
time; permanent native bans are excluded from both the electorate and veto
tally. The blocking threshold is `ceil(2*N/3)`, distinct from finality quorum.
An empty effective electorate blocks execution. Duplicate vetoes and vetoes at
or after the native deadline fail. `eosio` retains its narrow `veto` permission
only where it is itself an eligible frozen bootstrap voter.
After feature activation, the native minimum permission for eosio's `sysveto`
is explicitly its existing `veto` permission. Old sealed eosio accounts cannot
add a permission link themselves; this fixed action mapping neither rewrites
their permission rows nor authorizes another governance or management action.

`syscancel` requires `developer@upgrade`. Resubmission receives a new id and a
new full review period. The existing contract-upgrade lane is not cancelled.

`sysexec` requires both `developer@upgrade` and explicit
`resource_payer@active`. Its code and raw ABI must exactly match the proposal's
hashes and sizes. System/token code must still match the reviewed base. It
rejects execution before the complete native review period or after sufficient
effective BP opposition. TEST uses1800seconds only under the existing two TEST
build flags; production uses604800seconds. There is no shortened test clock on
the live chain and no affirmative-vote shortcut.

## Atomic creation and sealing

Execution grants a transaction-local, exact-parent, exact-target, single-use
native capability. Normal direct or privileged-forwarded `newaccount` calls
still cannot create `eosio.*` after sealing. The root schedules only
`newaccount` and native-only `sysstage`, both authorized by existing accounts.
The stage runs after the account exists, verifies its exact top-level `sysexec`
parent, proposal, payload, sponsor, empty code/ABI and temporary authorities,
then binds the capability to its own ordinal. It schedules:

1. the sponsor's exact token payment to the new account, before its code exists;
2. new-account-paid RAM purchase using that entire payment;
3. exact reviewed setcode and setabi;
4. native-only `sysfinish` verification and final owner sealing.

`sysfinish` requires that exact `sysstage` parent. Rebinding the ordinal consumes
the stage: another root child no longer matches, while a nested stage has the
wrong parent action type. No new transaction-context field or ordinary inline
actor/permission exception is introduced. The public proposal/execution wire
formats remain unchanged; `sysstage` is internal and requires the ephemeral
capability, never a developer permission link.

Final owner is threshold1 with no keys/accounts/waits (unsatisfiable). Active
has threshold1 and the existing governance model's `eosio@eosio.code` plus
`target@eosio.code` entries, no external management key. Native system permission
guards remain in force; future code/ABI replacement uses reviewed governance.
The new account must still be nonprivileged. Any child action, insufficient
balance/RAM, invalid WASM/ABI, or final-sealing failure rolls back the account,
protection-list addition, payment, deployment and proposal completion together.

The sponsor payment must cover native account/authority RAM, deployment's code
RAM multiplier, raw ABI, system resource rows and an operating reserve. It is
not calculated from the WASM byte count alone. A live market quote plus isolated
real-contract admission tests must establish the exact proposed payment.

## Storage, snapshots, replay and clients

The new object type is appended; existing object type ids and governance wire
layouts stay fixed. Snapshot format11 adds the new singleton section. Old
snapshots can restore an empty lane before feature activation. An old-format
header combined with activated creation governance is rejected rather than
silently dropping state. Format11 permits at most one creation-state row.
Cross-version snapshot integrity hashes are not claimed identical. Old binaries
cannot read format11; rollback after feature activation is not a safe fallback.

`get_system_account_state` is a read-only native endpoint with raw proposal data,
feature status, effective tally and head identity. It is a head-state observation,
not irreversible history. Canonical irreversible action receipts are required
for public history and execution evidence. Do not infer historical electorates
or deadlines from today's ranking.

The old on-chain eosio ABI is not rewritten during node installation or genesis
replay. `tools/governance/system_account_bundle.py` produces unsigned raw action
data for review. The developer must authorize ordinary linkauth entries for
`syspropose`, `syscancel`, `sysexec` to its existing `upgrade` permission. That
permission setup does not create a system-account bypass. Private keys never
belong in the helper, proposal memo, Git or evidence.

## Qualification and rollout gates

The added native tests cover feature inactivity, direct/forwarded bypasses,
manifest bounds, exact electorate refresh, pending-lane isolation, full review,
sponsor authorization, mismatched payloads, invalid-WASM rollback, vetoes,
cancellation, final nonprivileged sealing, snapshot/restart and genesis replay.
Their permissive token/RAM fixture cannot qualify production economics.
The retained previous O1 TEST compilation rebuilt144 project translation units using verified
retained dependencies. In the final isolated attempt7,31/32 governance cases,
2/3 real-contract cases and1/1 unsigned wire case passed. The successful creation
case and real signup case failed before deployment because inline authorization
is checked while the new account is still queued for creation. No actual K1/R1
registration result can be inferred from that attempt. The continuation repair
and added direct/missing-actor/missing-permission, order, nested/duplicate-stage
and rollback regressions passed in the November5 qualification2:34/34 governance,
3/3 real-contract and1/1 unsigned-wire cases. Real pinned signup WASM now executes
K1/R1/legacy registration, separate owner/active signatures, actual RAM/stake
ownership and atomic rollback. Build4 is an incremental mixed O0/O1 TEST
functional build. The user approved a60-minute maintenance window for this
build/isolated validation only; original programs are restored without installing
the candidate. The first repair qualification's missing helper-copy failure is
retained separately from the passing second attempt. See the
[failure and repair record](system-account-execution-followup.md).
The candidate is not qualified for rollout. The production review profile,
production build and broader current-chain compatibility remain unqualified.

Before any rollout:

1. Retain the now-tested continuation ordering and missing-actor/permission
   guards. Complete the production build/review profile and broader compatibility
   qualification, retaining the passed TEST legacy replay, pre-feature behavior,
   current-format snapshots and downgraded-header rejection. Simulated ban-reader cases are
   distinct from real-contract permanent-ban/empty-electorate and resource
   admission/failure qualification, which must also be completed.
2. Preserve the passed CDT/1760 independent parser/curve checks and real pinned
   native create/transfer/RAM/stake/signature/rollback results. Extend the
   isolated compatibility and adversarial qualification for the intended release.
3. Pin source, toolchains, node/WASM/raw-ABI/review hashes. Audit the original26
   processes, current chain id, head/LIB, votes, bonds, bans, history, sole relay
   and proposal3. Preserve failures and original evidence.
4. Obtain the user's explicit exception to the original26-PID retention rule
   before a controlled binary rollout. Preserve data/config/keys and never reset.
   Upgrade every validating node before scheduling the new protocol feature;
   verify its irreversible activation. A native contract-upgrade proposal cannot
   itself replace node binaries.
5. Verify target absence through an unambiguous authenticated RPC response, fund
   and explicitly authorize the selected sponsor, publish the exact review,
   submit only `refreshgov + syspropose`, and wait the complete native1800seconds
   on TEST (seven days on production). Preserve proposal3 untouched.
6. Attempt the matching execution only if mature and below the effective veto
   threshold. Verify irreversible creation/deployment, permissions,
   nonprivileged flag, hashes, resource payment and actual registration behavior.

No live transaction, binary rollout, feature activation or deployment is implied
by this document. No new server, TPS run, cap/vote/bond/ban mutation, key change or
BP interruption is authorized by staging the candidate.
