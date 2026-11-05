> Latest decision (2026-11-05): BP governance only vetoes developer proposals; capabilities expand gradually. No generic former-BP superuser executor or system key-rotation proposal is selected. USD/CNY become new ordinary issuers initially delegated to eosio.dev; eosio.key stays sealed. Visible reviewed upgrades remain permitted. Historical suggestions below are not grants; see [BP authority audit](bp-veto-only-audit.en.md).

# Developer capabilities audit and possible extensions (2026-11-05)

Selected scope: add dotless1–11character short service names, such as`usd`, to
reviewed native account creation. Other entries below are recommendations only,
not implemented or activated grants. The old chain still designates civicgovlead. The selected fresh genesis designates eosio.dev@upgrade, with account-based owner/active/upgrade delegation and normal underlying signatures. Initial single-account delegation can later become M/N, with management thresholds no weaker than upgrade. This grants no eosio@active authority.

## Existing and selected capabilities

| Capability | Implemented boundary |
|---|---|
| Propose protected contract WASM/ABI upgrades | Existing`devpropose/devexec`: exact target/base/payload/review hashes, complete review and effective BP vetoes. No direct setcode grant. |
| Cancel a pending upgrade | Existing`devcancel`; resubmission restarts review. The retained vetoed proposal3 must remain untouched in this task. |
| Create and deploy governed service accounts | Candidate`syspropose/sysexec`: permitted`eosio.*`and dotless1–11character short names. One proposal followed by mature atomic execution. |
| Cancel a pending creation | Candidate`syscancel`; independent of the existing upgrade lane. |
| Fund creation/RAM | Explicit additional sponsor`@active`authorization, with a bounded exact payment. A proposal cannot debit arbitrary users or treasury accounts. A bound is not spending approval. |
| One-time reviewed initialization | Version2 commits exact target::init arguments and the human review hash, executed between deployment and sealing; any failure rolls back, no direct admin grant. |
| Own account-based multisig management | Distinct ordinary account references, unit weights, no direct keys/waits; owner/active thresholds at least upgrade, upgrade cannot be deleted. Changes require the current management authority. |
| Query and prepare unsigned actions | Read-only inspection requires no new management authority; preparation tools neither sign nor broadcast. |

Short names are governed **contract service accounts**: nonprivileged, sealed
owner, code-only active, and future reviewed code updates. This does not hand
personal owner/active keys to the developer. Existing accounts, ordinary12letter
accounts, other owners' suffix namespaces and reserved governance accounts are
excluded. A`namebids`entry, including an unclaimed award, blocks proposal and
execution. A bid submitted during review therefore makes execution fail
atomically. Ordinary auction and12character account creation remain separate.

## Possible additional reviewed operations

| Proposal type | Purpose and required constraints | Status |
|---|---|---|
| Contract service parameters | Explicit resource policy, enabled state or application allowlists; fixed fields/bounds, old-state checks, fee impact, full review and vetoes. Signup currently exposes no admin configuration action. | Recommendation only |
| Resource market parameters | Dedicated bounded governance for`setram`,`setramrate`,`cfgpowerup`; explicit action allowlists, economic review and resource-split invariants. No arbitrary unlimited-resource grant. | Recommendation only |
| Protocol-feature activation | Exact digest/dependencies and all-node qualification; distinguish binary rollout from activation, retain review/vetoes and explain rollback limits. Existing loopback producer API is an operator interface, not developer on-chain authority. | Recommendation only |
| Designated developer succession | Explicit old/new authorization and acceptance, review/veto, one-time handover and old-role revocation. Never rerun govinit. Rotating the ordinary developer account's own upgrade key is distinct from replacing the chain's designated developer identity. | Recommendation only |
| Narrow reviewed action execution | Execute a fixed reviewed action without upgrading an entire contract. Requires native target/action allowlists, exact data hashes, single-use ancestry, sponsor budgets and atomic rollback. No arbitrary eosio-action forwarding. | Design recommendation only |

Each extension needs its own explicit proposal schema and remote qualification.
Current devexec installs reviewed code/ABI; it does not automatically configure
PowerUp, change native parameters, transfer funds or activate a protocol feature.
Reviewed contract code can affect future behavior, so code review remains a
substantial authority boundary even when a review period exists.

## Authorities to keep restricted

- External eosio owner/active management keys, arbitrary protected-account
  permission changes and general privileged grants.
- Spending users' funds, replacing their keys/state or signing BP votes,
  maintenance requests, production/finality keys on their behalf.
- Direct AIC issuance/cap changes, extracting locked bond/slash funds, refunds
  or bypassing exit cooldowns.
- Forcing BP schedules/quorum/bans/safety state or bypassing review, effective
  vetoes and empty-electorate rejection.
- Treating host SSH/systemd, site publication or history-database operation as
  the same role as the on-chain developer.

This audit does not authorize changes to live supply, bonds, votes, BP status or
consensus parameters.

## Examined implementation

Native`libraries/chain/upgrade_governance.cpp`enforces designated developer,
top-level authorization, hashes, review/veto, single-use installation and sealed
permissions.`system_account_proposal.cpp`enforces target/auction/sponsor and
continuation/sealing checks.`eosio_contract.cpp::apply_eosio_newaccount`retains
creator and authority validation and exact-target capability consumption.

The deployed system source snapshot is
`PureEOS-deployed-system-20261102/contracts/eosio.system/`: native::newaccount
skips auction checks only for creator eosio; administrative configuration still
requires system authorization. PowerUp, voting and bond implementations retain
their respective owner and protocol conditions. Reviewed system WASM hash is
`b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170`,
token hash`f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c`.

A source audit is not release qualification. A capability is live only after its
implementation, tests, validator rollout and required irreversible activation.
