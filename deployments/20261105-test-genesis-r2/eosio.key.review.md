# 公开 TEST 创建/部署审核 / Public TEST creation/deployment review

此文件和附带的精确WASM、原始ABI及初始化是审核对象。公示完整1800链秒，BP按冻结有效选民的原生三分之二门槛否决；每项独立执行，不自动升级。
This review binds exact WASM, raw ABI and initialization. Each proposal waits1800chain seconds and independently checks the native two-thirds effective-BP veto threshold. No automatic execution.

旧链记录单独保留；本文件仅针对新的 Chain ID。创始人使用已发行余额赞助这次创建所需代码/ABI/少量运行RAM，不增发AIC，不预置大型收款池。预算为市场反算加20%价格余量及0.0100AIC；整个已审付款用于实际原生买RAM。随后KEY缺失收款行在每次转账内自动购RAM，并由发送人承担实际费用，收款本金全额。
Old history remains separate. Founder sponsors code/ABI/small runtime RAM from existing issued funds. The reviewed budget is the market quote plus20% price headroom and0.0100AIC; all of it buys actual native RAM. Future missing KEY records are funded by sender fees in each atomic transfer, preserving full principal.

所有权 / Ownership: Sealed system service: unsatisfiable owner and own-code active only; no external management key or BP/dev direct takeover.

权限边界：BP治理只否决；不新增强改普通用户权限或资产动作。dev不获通用特权。KEY为封印托管例外。USD/CNY是模拟第三方资产，无现实法币储备承诺，普通合法发行人保留其自身账号管理能力，不控制AIC或KEY。可见代码与BP否决是治理保障，不是任意代码良性的自动证明。
BP governance is veto-only. No forced ordinary-user authority or asset mutation action is added, and dev receives no universal privilege. KEY remains sealed custody. USD/CNY simulate third-party assets without a fiat reserve claim; their authorized holder manages only those issuer accounts. Visible code review and veto are governance safeguards, not a proof that arbitrary code is benign.

验证依据：修复后native58（真实手续费/注资REX、深度4）；KEY候选238原生+最终包26SDK。前轮1–4已取消；新5–8重新完整公示。公开部署后仍须实测三方向/三币种、费用/RAM/回滚/历史/浏览器，不能用私有结果冒充公开结果。
Qualification: repaired native58 with actual fees/fundedREX at depth4; KEY238native and final-package26SDK. Previous IDs1–4 were cancelled; new IDs5–8 require fresh full reviews. Actual public route/token/fee/RAM/rollback/history/browser checks remain required after deployment.

## 精确字段 / Exact fields

```json
{
  "target": "eosio.key",
  "chain_id": "09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532",
  "expected_proposal_id": 5,
  "supersedes_cancelled_proposal_id": 1,
  "authority_version": 2,
  "RAM_payment_atoms": 49628,
  "RAM_minimum_bytes_estimate": 1821370,
  "RAM_quote_atoms": 41273,
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
  "code_SHA256": "7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788",
  "JSON_ABI_SHA256": "f4d64cd7bd84796fab77886fc8308def72b19076a76c2e2bae58857f7bf3b0c5",
  "raw_ABI_SHA256": "b6db52cd47fdf811bee83030e524de35d0b6f55aece386392b56d1a085df2f83",
  "initializer": {
    "chain_id": "09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532",
    "deployment": "f34e7c1e9b017054ce9be91b8dfeb276dbee58baecbdf7af1910d7012d543f02",
    "fee_contract": "eosio.token",
    "fee_symbol": "4,AIC",
    "system_hash": "b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170",
    "policy": {
      "minimum_fee": 100,
      "unit_fee": 10,
      "new_row_fee": 500,
      "maximum_multiplier_bps": 100000,
      "epoch_seconds": 1,
      "max_wallets": 1000,
      "max_balances": 4000,
      "max_assets": 32,
      "max_native_payers": 1000
    }
  },
  "initializer_SHA256": "52971f394da4cd66ae05f3b790462484ab2efcbe7977f3c431e7e0cdea4afc41",
  "review_seconds": 1800,
  "sponsor": "gptgenesisbp",
  "developer": "eosio.dev",
  "native_SHA256": "946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397"
}
```
