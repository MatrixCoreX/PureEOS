# Fresh-chain BP bonds

[中文](bp-bond.md)

This extension starts from qualified system contract `15dea1aa8a4b158eb771c5b13947ddeaafd28910` and is for fresh-chain deployment. It preserves integer single-choice voting, native proposer/finalizer rotation, the 10%/90% resource split, exact 1% rewards and halving, seven-day upgrade review, permanent bans and global-interruption safeguards.

## Fixed policy

- Every ordinary BP authorizes exactly `10000.0000 AIC` before either producer registration action or admission. CPU/NET stake, partial payments and overpayment cannot replace the bond.
- `eosio.system::init` automatically initializes bonds and creates independent `eosio.bond` and `eosio.bslash` custody accounts with RAM before governance sealing. `eosio.token` remains only the token contract, not custody. Initialization is atomic.
- Deposits buy neither vote weight nor rotation priority; existing votes, signing keys, maintenance and ban conditions still apply.
- Exit deactivates registration but retains existing native proposer/finalizer and rotation active/target duties until safe handoff. No replacement means duties and funds remain; withdrawal does not lower quorum.
- After duties and pending windows are settled, a fixed `604800`-second cooldown starts. Refund also needs an irreversible exit checkpoint and no permanent ban. The BP explicitly claims it; no deferred execution is used.
- A permanent-ban decision immediately forfeits the bond and removes refund rights. After that decision block becomes irreversible, anyone can settle its fixed amount into `eosio.bslash`. Before settlement it remains an irrevocable pending-forfeiture liability in `eosio.bond`; the caller does not decide whether forfeiture applies.
- This version exposes no pool withdrawal, distribution or burn action. Ordinary signing keys cannot drain custody; existing native governance still protects its permissions and code.
- Vote withdrawal, ordinary rotation and safely completed maintenance do not themselves forfeit bonds. Exit, redeposit, registration and key changes never erase valid violations or bans. Actual historical production rewards remain claimable after exit; a deposit itself earns no rewards.

## Actions and state

| Action | Effect |
|---|---|
| `initbpbond()` | Automatically called during fresh system initialization, once, with system authority before sealing |
| `bondbp(producer)` | BP-authorized atomic deposit of exactly 10,000 AIC |
| `exitbond(producer)` | BP requests exit and waits for safe native handoff |
| `refundbond(producer)` | BP claims after the fixed cooldown and all finality/duty conditions |
| `settlebond(producer)` | Permissionless, once-only settlement into the fixed pool; no caller-selected amount or recipient |

`bpbondcfg` tracks enforcement, cooldown and locked/pending/settled/refunded totals. `bpbonds` retains one BP-paid RAM row per historical identity, with bonded/exiting/cooling/refunded/slashed states (0/1/2/3/4); no per-block history table is appended.

Without unsolicited transfers, custody balances equal `eosio.bond = locked + slash_pending` and `eosio.bslash = slash_settled`. Direct transfers do not create a deposit record or registration eligibility; excess custody is not credited as a bond.

Forfeiture accounting is atomic with the blacklist flag and sends no external notification in its penalty path. Failed settlement never restores refund rights. Refund and pool settlement are atomic; a normal chain rollback reverses the corresponding state and transfers together.

## Rollout and scope

This is a fresh-chain profile, without an old-chain grace period or enforcement-disable action. It does not bypass the native prohibition on creating eosio.* accounts after governance sealing. The protocol genesis signer only bootstraps; the first real BP pays the same bond before registration.

After remote build and real WASM/native functional qualification, archive the previous chain and deploy on the retained test server. Run one pure-AIC-transfer TPS window with one real BP and retain auditable evidence. Then start the dedicated nonproducing SHIP history node and PureHistory, followed by gradual admission of the remaining healthy BPs. Record actual source, artifacts, configurations and evidence hashes.

No finalizer-refusal penalties, enlarged committee or timeout takeover are added. Loss of finality never lowers quorum or invents personal faults from heartbeats/logs. Bonds cannot guarantee recovery beyond existing fault tolerance.

## Account authority after sealing

The native seal includes both custody accounts. Owner has threshold1 with no keys/accounts/waits; active has threshold1, no ordinary keys/waits, and weight1 each for eosio@eosio.code and the custody account itself @eosio.code. The pool initially has no spending code. Developer upgrade keys are not direct custody authorities; future code changes still use the seven-day review and BP veto process.
