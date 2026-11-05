# Relay execution-income upgrade review

This is a candidate review. Activation requires native devpropose and matching irreversible devexec on the exact chain identified in REVIEW.json.

Commercial ksend/withdraw/cancel credit newly earned execution fees plus relay fees to the actual submitting BP. Exclude actual native RAM spending first. Preserve sender totals and full principal, 10–30% upward-rounded relay fees, signed rates and total caps. Execution fees remain market-based quotes rather than measured per-transaction reimbursement; relay revenue is not guaranteed profit.

Keep named deposits, selfout, selfcancel and legacy refund destinations. Never migrate or retroactively distribute historical network reserves:2352 atoms observed,672 under KEY and1680 under founder. Historical BP-indexed network balances remain unclaimable. Funds remain in KEY custody until the existing owner-authorized, fee-free claim redeems confirmed relay_earned.

Keep ABI/storage/signature wire, epochs/replay protection, BP eligibility/bans, reviewed token/system identities, automatic RAM purchases and exact reserve accounting. The legacy retained_base receipt field reports execution fees; use the reviewed code version and corresponding receipt.relayer to determine destination. Historical transactions retain their historical interpretation.

Publish this exact review/source before a complete TEST1800-chain-second review and veto check, then execute the matching bytes. Production7days remain unchanged. No reset, BP pause, votes/bonds/caps/native-system changes, new server or TPS. All builds/tests/browser checks run remotely. REVIEW.json records exact hashes, actual check counts and retained parser failures.
