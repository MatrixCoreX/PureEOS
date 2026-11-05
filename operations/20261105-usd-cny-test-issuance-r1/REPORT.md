# USD/CNY 发行上限与测试资金

状态：已完成。最新“上限10亿”覆盖此前“上限1亿”的指示；另为每种币新增发行1亿，保留原有1万。全部在一笔六根动作交易中原子完成，先发行到对应发行账号，再全额转给既有测试资金账号 `gptgenesisbp`。

| 币种 | 发行上限 | 本次新增发行 | 当前已发行总量 | gptgenesisbp 当前余额 |
|---|---:|---:|---:|---:|
| USD | 1,000,000,000.0000 | 100,000,000.0000 | 100,010,000.0000 | 100,009,796.0000 |
| CNY | 1,000,000,000.0000 | 100,000,000.0000 | 100,010,000.0000 | 100,009,800.0000 |

Chain ID：`09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`。交易：`c899dcd45d5694f601c602e2e26d4403e8acb47f30224279a4e02dab81e1fef2`，不可逆区块 42769，区块 ID `0000a711e6868b30df906e72338490e25bd08669ffb08f6759ecb88a8f1cf36a`。原生 CPU 228 微秒、NET 672 字节，由 `eosio.dev` 已有资源承担；沿用既已激活的 first-authorizer 计费，不改资源参数。

通过 `usd/cny@active → eosio.dev@active → civicgovlead@active` 原有授权执行普通 `setmaxsupply`、`issue`、`transfer`。无治理提案、合约/ABI/权限变动；AIC发行量与上限、KEY账本/准备金/RAM、原25个节点PID均核对不变。这里只为测试资金池备款，没有启动TPS或额外转账活动。

原生交易早已不可逆成功。第一次历史校验假定接口存在 receiver 字段而报错，保留 INITIAL_PROGRESS_FAILURE.json；只读复核使用实际 notified 字段和规范动作序号，并独立校验 transfer 新增的 amount/symbol 展示别名。六个动作与原意图逐字段一致；没有重新签名、提交或重复发行。原始签名包和钱包凭据仅留远端私有目录，未导出。临时签名钱包已关闭。

This completed issuer operation raised BOTH caps to 1 billion and issued an ADDITIONAL 100 million of EACH token to the existing test funder. Previous issued tokens remain: total supply is 100,010,000 each. One atomic six-action transaction is irreversible. AIC supply/cap, KEY ledgers/reserves/RAM, authorities/code/ABI and all original25node PIDs remain unchanged. No TPS, node reset/pause or repeated issuance. The initial history parser failure was reconciled read-only against the original native ID.
