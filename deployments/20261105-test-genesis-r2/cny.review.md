# 公开 TEST 创建/部署审核 / Public TEST creation/deployment review

此文件和附带的精确WASM、原始ABI及初始化是审核对象。公示完整1800链秒，BP按冻结有效选民的原生三分之二门槛否决；每项独立执行，不自动升级。
This review binds exact WASM, raw ABI and initialization. Each proposal waits1800chain seconds and independently checks the native two-thirds effective-BP veto threshold. No automatic execution.

旧链记录单独保留；本文件仅针对新的 Chain ID。创始人使用已发行余额赞助这次创建所需代码/ABI/少量运行RAM，不增发AIC，不预置大型收款池。预算为市场反算加20%价格余量及0.0100AIC；整个已审付款用于实际原生买RAM。随后KEY缺失收款行在每次转账内自动购RAM，并由发送人承担实际费用，收款本金全额。
Old history remains separate. Founder sponsors code/ABI/small runtime RAM from existing issued funds. The reviewed budget is the market quote plus20% price headroom and0.0100AIC; all of it buys actual native RAM. Future missing KEY records are funded by sender fees in each atomic transfer, preserving full principal.

所有权 / Ownership: Ordinary issuer: owner eosio.dev@owner; active eosio.dev@active OR own eosio.code; nonprivileged. Legitimate owner may manage only this ordinary account.

权限边界：BP治理只否决；不新增强改普通用户权限或资产动作。dev不获通用特权。KEY为封印托管例外。USD/CNY是模拟第三方资产，无现实法币储备承诺，普通合法发行人保留其自身账号管理能力，不控制AIC或KEY。可见代码与BP否决是治理保障，不是任意代码良性的自动证明。
BP governance is veto-only. No forced ordinary-user authority or asset mutation action is added, and dev receives no universal privilege. KEY remains sealed custody. USD/CNY simulate third-party assets without a fiat reserve claim; their authorized holder manages only those issuer accounts. Visible code review and veto are governance safeguards, not a proof that arbitrary code is benign.

验证依据：修复后native58（真实手续费/注资REX、深度4）；KEY候选238原生+最终包26SDK。前轮1–4已取消；新5–8重新完整公示。公开部署后仍须实测三方向/三币种、费用/RAM/回滚/历史/浏览器，不能用私有结果冒充公开结果。
Qualification: repaired native58 with actual fees/fundedREX at depth4; KEY238native and final-package26SDK. Previous IDs1–4 were cancelled; new IDs5–8 require fresh full reviews. Actual public route/token/fee/RAM/rollback/history/browser checks remain required after deployment.

## 精确字段 / Exact fields

```json
{
  "target": "cny",
  "chain_id": "09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532",
  "expected_proposal_id": 7,
  "supersedes_cancelled_proposal_id": 3,
  "authority_version": 3,
  "RAM_payment_atoms": 8614,
  "RAM_minimum_bytes_estimate": 313050,
  "RAM_quote_atoms": 7095,
  "RAM_market": {
    "supply": "10000000000.0000 RAMCORE",
    "base": {
      "balance": "55212430767 RAM",
      "weight": "0.50000000000000000"
    },
    "quote": {
      "balance": "124477.0236 AIC",
      "weight": "0.50000000000000000"
    }
  },
  "code_SHA256": "ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb",
  "JSON_ABI_SHA256": "9846bb35be03e80a831872f1cbd0aff9732df4abeecfcc76e062d8f38960b4e4",
  "raw_ABI_SHA256": "a64141f32a82aaa3185dad05678ef44b42a8f7a9a8dbc266fd2ee98fd2ffc90e",
  "initializer": {
    "recipient": "gptgenesisbp",
    "maximum_supply": "1000000.0000 CNY",
    "initial_supply": "10000.0000 CNY"
  },
  "initializer_SHA256": "d9e4aae6bc9727a3bb6e4f68f578490c379d660036822e372d779a9656213b08",
  "review_seconds": 1800,
  "sponsor": "gptgenesisbp",
  "developer": "eosio.dev",
  "native_SHA256": "946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397"
}
```
