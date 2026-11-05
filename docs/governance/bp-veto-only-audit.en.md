# Read-only BP authority audit and developer boundaries

Date: 2026-11-05. Observations concern the still-running chain `116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0`, not the planned replacement chain.

## Final selected policy

BPs provide block production, validation, finality and reliable node service, and earn protocol rewards. Their governance role is limited to vetoing developer proposals. Developer capabilities expand gradually; no general superuser executor or legacy producer multisig administrator is being added.

Do not introduce forced changes to another user's authorities or assets. Holder-authorized key changes and transfers remain valid. System authorities stay sealed; no system key-rotation proposal is selected. `eosio.key` is an explicit exception to ordinary account delegation: its owner/custody must not be handed to an external key or account, and its user liabilities are not discretionary system funds.

The latest USD/CNY choice supersedes system custody: create ordinary issuer accounts initially delegated to `eosio.dev@owner`, with active authorized by `eosio.dev@active` or the issuer's own code. Do not copy private keys. A legitimate owner may later authorize a handover. This is voluntary initial ownership of NEW accounts, not authority over existing users.

Reviewed contract upgrades remain permitted. The selected safeguard is public exact code/parameters/impact/hashes, the complete review period, effective BP vetoes and matching execution bytes. It is not permanent mathematical immutability of asset-contract code.

## Live findings

Read-only RPC observations beginning at 04:00 UTC cover all **25 registered BP accounts**; every one has `privileged=false`. Registration does not imply eligibility or health: 24 BPs remain eligible and the pre-existing Germany permanent ban remains intact.

| Capability | Observed result | Enforcement |
|---|---|---|
| BP control of system accounts | No usable administrator path found | `eosio` owner has threshold 1 and no members; active contains only its own code, with no BP delegation |
| Legacy BP multisig through `eosio.prods` | Disabled | Owner, active, major and minor authorities are unsatisfiable; native producer-authority refresh returns under governance profile |
| Authorization bypass through wrap/msig | Disabled | Both accounts are nonprivileged and sealed; native execution rejects their contracts after governance sealing |
| Direct system authority/code changes | Unavailable to BPs | Protected authority changes are rejected; code installation requires the designated developer's reviewed single-use native capability |
| Resource freezing of another account | No BP administrator authorization | Resource-setting actions require `eosio`; BP membership and combined BP signatures do not satisfy it |
| Forced user owner/active change | No BP-specific route | Native account authorization remains required and no privileged BP forwarding path exists |
| Forced token movement or KEY ledger debit | No BP-specific route | Deployed token transfers require sender authorization; KEY exits verify user proofs, independently of relay submission |
| Granting a BP privileged status | Natively blocked after sealing | Only `eosio` may remain privileged |
| Developer-proposal veto | Retained | Full TEST 1800-second review and existing at-least-two-thirds effective frozen electorate threshold; proposal 3 retains its 24/24 vetoes |

A user's voluntary delegation to a BP is authority granted by that holder, not a native BP administrator privilege. Permissionless operations constrained by protocol conditions, such as eligible bond settlement, are not discretionary confiscation powers.

Live code hashes were checked: system `b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170`, token `f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c`, KEY `bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1`.

Code review covered native upgrade governance, authorization, action execution, producer-authority refresh and privileged intrinsics; deployed system resource, voting, authority-limit, reward schedule and bond code; and the deployed token. Relevant differences from old native source `057e173` were reviewed separately. The creation candidate cannot replace an existing account or restore producer administrator rights.

Public evidence: workspace `release-work-20261105/dev-account-genesis-r1/BP_READONLY_PERMISSION_AUDIT_LIVE.json`, also retained in the separate remote work root. It contains public permissions, governance and chain observations, not private keys. No freeze, authority-change or asset transaction was broadcast for this audit.

## Limits

BPs still control admission to their own nodes and may withhold production/finality signatures. Collusion may cause censorship or a network halt; removing account-management authority does not eliminate those consensus risks. Existing rotation, fault and bond rules remain unchanged.

System and asset contracts remain upgradeable through developer governance. Malicious replacements could add freezing, ledger changes or authorization bypasses. This audit therefore does not claim that no actor could ever indirectly change user assets. The user selected visible review and BP vetoes as the safeguard, not an asset-code freeze in this task.

Native checks bind bytes, timing, electorate and veto result; they do not prove benevolent code intent. Silence is not affirmative approval: a proposal below the veto threshold may be executed by the authorized developer after maturity. New capabilities and replacement-chain deployment still require their own remote qualification and activation evidence.
