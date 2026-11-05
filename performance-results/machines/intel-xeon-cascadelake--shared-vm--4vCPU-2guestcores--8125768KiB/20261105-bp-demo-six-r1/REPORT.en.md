# Six-BP functional demonstration: in progress

Current chain `09f10e50…`, on the retained shared Intel Cascadelake 4-vCPU / 8-GB TEST host. This is neither a capacity nor a throughput qualification.

Six new BPs have been created, synchronized, individually bonded with 10,000 AIC each, registered and self-voted. Six additional independent owned voting accounts provide other votes. All 30 registered vote totals are distinct; the original 24 BP totals and 90 business-account assignments are unchanged.

The new BPs are still waiting for native FIFO admission. No fault outage has been injected and no completed blacklist or maintenance result is claimed. The controller requires real irreversible production, simultaneous native proposer/finalizer membership with a healthy 21/15 quorum, zero prior miss streaks, and at least three completed personal production turns for the sixth BP. It then stops only the five authorized new fault targets and waits for the native three complete zero-output windows and irreversible ban/bond decisions. The sixth BP subsequently signs its own maintenance request and stays online through and after effective native handover.

New nodes bootstrap from the same-chain native irreversible checkpoint #48653, SHA256 `085701fecf378272856d1eecfc138397e5b1c014aea446eedd2360c2da95c021`, with full validation thereafter. The initial slow genesis-replay attempt is retained. No live raw-database copy, chain reset or original-node restart occurred. Raw checkpoints, configurations and private keys remain remote only.

The capacity caption is already public: next candidate-capacity increase estimated at `2026-11-12 04:09:14.000 UTC`, 210 → 211. It follows the existing native candidate rule and does not change the 21 active seats. Remote qualification passed 268 full frontend checks, 25 final related checks, TypeScript/build, four preview and four normal public Chrome variants, and six public artifact digests. The overly long initial caption, first build-helper syntax failure and transient vote-API HTTP503 in a preview screenshot are preserved. All four final public screenshots have been visually reviewed without that notice.

Static release `20261105T123459Z-next-capacity-time`; frontend functional commit `7c029f65e551e2b28e2ebb6da0c4a6b5ea5ffaf6`. Final native outcomes, public blacklist/maintenance browser checks, audit and bounded cleanup remain pending. Never replay completed creation, funding, bond or vote steps.
