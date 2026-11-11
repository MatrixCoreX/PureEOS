# eosio.key · 命名账号自付资源候选

2026-11-11 候选 `cdfe0f44`：命名转公钥采用发送方 CPU/NET/RAM，两动作同笔授权、本金全额到账，应用手续费为零，RAM不足失败且不自动购买。原生隔离191项、SDK30项通过，ABI与7db完全一致。当前尚待完整1800秒开发者提案公示及不可逆执行，不能把候选构建当成链上生效。

[中文流程与边界](docs/NATIVE_SENDER_RAM.zh.md) / [English review](docs/NATIVE_SENDER_RAM.en.md)。公钥付款路线的既有费用机制保持。

---

# eosio.key · 全网公钥账户与按需 RAM

2026-11-07 当前代码7db，raw ABI83；提案4已在267777不可逆执行，三币种三路线、原子注册与本轮新增收入领取已实测。固定钱包/余额/资产总量门槛已移除，新增记录仍由本笔费用购买真实RAM；现有布局和完整本金保持。

- [中文实际结果与三种费用流程](docs/GROWTH_TRANSFERS.zh.md)
- [English activation and fee flows](docs/GROWTH_TRANSFERS.en.md)
- [中文增长/兼容设计](docs/UNBOUNDED_GROWTH.zh.md)
- [English growth/compatibility design](docs/UNBOUNDED_GROWTH.en.md)

以下记录按原历史时点保留，不表示当前部署状态。

## 历史候选记录

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

## Atomic public-key registration / 公钥余额注册桥接

The candidate pairs `withdraw` or `selfout` of native AIC to `eosio.signup` with one exact second root: `eosio.signup::createkey(request_id)` with no authorization. The existing v2 holder proof signs the complete two-root transaction. All other owner operations keep one root. Extra roots, a changed request ID, a non-native token, a changed Signup WASM or any extra continuation authorization are rejected before KEY debits. The SDK prepares and verifies this pair automatically for the Signup destination.

Signup stages actual received funds against the complete transaction hash, request ID, name and parsed Owner/Active keys, then creates at top level. It removes its pending row in the same transaction. Any failure rolls back the principal, KEY fee, account, RAM and stakes; no persistent prepayment, BP custody or extra relay permission is introduced. Keep current native depth4, commercial execution-income allocation, rates/caps, full principal and free claim. Signup still uses0.1000 AIC NET plus0.1000 CPU with ownership transferred to the new account; the remainder buys at least4096 RAM bytes. Native-payer transfer registration remains supported.

This is a coordinated candidate upgrade: activate the exact reviewed Signup WASM first, then this KEY WASM, with a complete native TEST1800-second review for each. Production7days remain unchanged. The existing native cleos generic transfer flow remains unchanged; registration needs a client that constructs this complete owner-signed pair. Remote build/qualification and actual activation evidence determine availability; this source section alone is not deployment proof.
