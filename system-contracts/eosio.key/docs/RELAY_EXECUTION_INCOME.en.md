# Execution fees earned by the actual relay

Status: active on the current TEST chain since 2026-11-05. Native proposal #1 completed its 1,800-second review and executed irreversibly in block 35304. Code SHA-256: `1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d`. The production review period remains seven days.

For commercial ksend, withdraw and cancel, exclude actual native RAM spending first. Credit execution fees plus relay fees to the submitting BP's relay_earned. The existing fee-free claim redeems confirmed earnings in full. Funds remain in contract custody until authorized claim rather than being transferred to the BP on every payment.

Preserve total pricing, 10–30% relay rates, upward rounding, signed rate/max_fee, full principal, automatic RAM purchases, market multiplier and eligibility checks. Relay fee=base×selected rate; base=execution fee+actual RAM spending, including any minimum supplement. Relay income=total−actual RAM spending, not guaranteed profit or measured per-transaction CPU/NET reimbursement.

Named deposits, selfout and selfcancel have no commercial relay and retain their current network-reserve destination. Do not migrate or retroactively distribute historical network reserves, including rows indexed by BP names. Claim cannot redeem network reserves.

Keep ABI/table/wire layouts. The legacy feereceipt.retained_base field reports execution fees excluding RAM. Interpret its destination using the associated receipt.relayer and reviewed code version; historical receipts keep their historical meaning.

Actual post-activation example: a new key received the full 3.0000 AIC. The sender paid an additional 0.0299 AIC: actual RAM spending 0.0037, execution fee 0.0193, relay fee 0.0069. Relay income was 0.0262 AIC. The observed resource multiplier was 21335/10000 and selected relay rate was 30%; these values are not a fixed quote.

Twelve real transfers covered all three directions and AIC/USD/CNY. Earnings from eight commercial relay transfers totaled 0.2261 AIC and were claimed once in full with a 0.0000 AIC application fee. The claimant still pays native CPU/NET. Claim transaction: `1eee7ffeb5e7414841fbb732c0e910d01fd6043714fc28981093164bbe9a2881`, irreversible block 37478.
