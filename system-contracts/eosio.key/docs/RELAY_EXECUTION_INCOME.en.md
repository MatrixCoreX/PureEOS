# Execution fees earned by the actual relay

Status: candidate development/qualification; not active until native proposal and irreversible execution.

For commercial ksend, withdraw and cancel, exclude actual native RAM spending first. Credit execution fees plus relay fees to the submitting BP's relay_earned. The existing fee-free claim redeems confirmed earnings in full. Funds remain in contract custody until authorized claim rather than being transferred to the BP on every payment.

Preserve total pricing, 10–30% relay rates, upward rounding, signed rate/max_fee, full principal, automatic RAM purchases, market multiplier and eligibility checks. Relay fee=base×selected rate; base=execution fee+actual RAM spending, including any minimum supplement. Relay income=total−actual RAM spending, not guaranteed profit or measured per-transaction CPU/NET reimbursement.

Named deposits, selfout and selfcancel have no commercial relay and retain their current network-reserve destination. Do not migrate or retroactively distribute historical network reserves, including rows indexed by BP names. Claim cannot redeem network reserves.

Keep ABI/table/wire layouts. The legacy feereceipt.retained_base field reports execution fees excluding RAM. Interpret its destination using the associated receipt.relayer and reviewed code version; historical receipts keep their historical meaning.

Example: total0.0294AIC, RAM0.0037, execution0.0189, relay fee0.0068. The relay can earn0.0257 under the new rule; sender cost and full principal remain unchanged.
