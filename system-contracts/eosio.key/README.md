# eosio.key · Atomic RAM candidate / 自动购 RAM 候选

2026-11-05: candidate `e006128f25fcda3f2395302212247b436b550d8ffde0f7da3064b8cec624a442`
passed 237 real native checks and 26 SDK tests remotely. **Not yet deployed to the public TEST chain.**
Current public chain still uses the historical `bc3358d7` profile. Native creation qualification,
full TEST1800s review, effective BP-veto checks and canonical deployment evidence remain required.

本分支新合约自动按交易缺失记录购买真实 RAM，发送人另付 AIC 费用，收款本金完整到账。
命名→公钥采用一个交易、一次签名、三个根动作，无需另行预存手续费。key→key 和
key→命名保留本地用户证明，中继承担原生 CPU/NET。AIC/USD/CNY 都支持；接收方无需
先准备 AIC，发送其他代币的人仍须另有 AIC 手续费。已有记录不重复买 RAM。

The sender pays additional AIC fees; recipients receive full principal. Missing records trigger
an actual native RAM purchase in the same atomic transaction. Existing records buy no more RAM.
A named deposit uses one transaction/signature and three root actions, without persistent prepaid
fee credit. Public-key sends keep local owner proofs and independent-device replay protection.

- [三种转账、所有费用、实际回执及限制](docs/ATOMIC_RAM_TRANSFERS.zh.md)
- [Three transfer routes, complete fees, actual receipts and limits](docs/ATOMIC_RAM_TRANSFERS.en.md)
- [Exact token identities and reviewed code](token_allowlist.json)

| Direction / 方向 | Candidate path / 候选接口 |
|---|---|
| Named → key / 命名→公钥 | SDK `nativeDepositActions`: declaration + AIC funding + principal; same sender signs all three |
| Key → key / 公钥→公钥 | `ksend`, owner proof plus qualified BP relay |
| Key → named / 公钥→命名 | `withdraw`, or `selfout` with an explicitly authorized native resource payer |

Full token identities are `eosio.token::4,AIC`, `usd::4,USD` and `cny::4,CNY`, each code-hash pinned.
USD/CNY are planned ordinary issuer accounts initially delegated to `eosio.dev`, not system custody.
`eosio.key` stays sealed and nonprivileged. New accounts/services are created after startup through
reviewed native proposals; no direct developer deployment or ledger-administrator key is added.

Normal transfers no longer require manual `regtoken`, sponsored capacity or native recipient
opening. Bootstrap still needs the small measured quota necessary for code/init/scratch storage.
A conservative allocation budget is not exact native row metering. Fee-free full-value income
claims retain claimant-paid CPU/NET and recipient opening; see the detailed documents.

Legacy API/cleos/deployment documents and earlier performance reports describe historical profiles.
Their prepaid fee, PK1/PK2 deposit and manual USD-open instructions do not apply to this candidate.
Old native cleos deposit shortcuts, released Android APKs and explorer profiles still require
separate adaptation/QA before anyone may claim full compatibility with the replacement chain.

Builds/tests run remotely only. SDK/reference relay use Node.js >=22.12; the sole public HTTP
relay remains the founder. This is a development TEST candidate, not a stable production release.

License: repository [AIC noncommercial license](../../AIC_LICENSE.md).
