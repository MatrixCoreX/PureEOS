# 自定义代币托管边界 / Custom-token custody boundary

2026-11-03 最新范围：测试白名单为系统 AIC 与标准 `keyusdtoken1::4,USD`，绑定完整币种和审核代码哈希；后续新增通过受保护 eosio.key 代码的原生七天开发者提案/BP 否决流程。第三方文本收款账号必须先付 RAM open。RAM 登记与白名单相互独立；独立远端正常系统74项、替代表夹具74项与SDK/中继14项通过，现已部署公开测试链；旧风险复现保留，稳定版本未更新。详见 [白名单提案流程](TOKEN_ALLOWLIST.md)。

2026-11-03 最终选择并完成方案一：系统 AIC 与标准 USD 的完整身份/代码哈希白名单。远端正常系统74项、替代夹具74项和SDK/中继14项通过，已部署公开 TEST 链。两币在新链封存前引导配置，未声称经过七天成熟提案；后续新增必须走既有开发者提案。方案二仅保留为未选研究。

Option 1 is selected and qualified for the reviewed AIC/standard USD profile. It is deployed on the fresh public TEST chain, without changing the stable release. Bootstrap admission is distinct from executing a mature seven-day proposal. Option 2 remains unimplemented research.

## 已复现的问题 / Reproduced behavior

Vultr 原生运行时探针表明：收到托管账户转账授权的恶意代币第一接收合约，能给该托管账户额外计费 **4,330 字节 RAM**。同一探针里的收款通知计费被已激活的 `RAM_RESTRICTIONS` 拒绝。前者是授权后的原生计费语义，不会被代币余额、通知、储备复核或钱包自己的行数计数器覆盖。

The native Vultr probe accepted an additional **4,330 bytes** billed by an authorized malicious token first receiver to its custody sender. A recipient-notification attempt was rejected by activated `RAM_RESTRICTIONS`. The first-receiver behavior is permitted native authority semantics; balance, notification, reserve and wallet-local row checks do not cover that external RAM allocation.

A malicious issuer can already freeze or devalue its own token. Shared-custody RAM abuse is more serious because it can affect availability for unrelated assets. A passing transfer/replay suite does not remove this issue.

## 方案一：第一版接入已审核代码 / Option 1: reviewed implementations

- 保留同一个 `eosio.key` 托管入口、多发行方、多符号和多精度账本，AIC 与使用已审核标准代币代码的其他币均可接入。
- 按 **WASM 代码哈希** 准入，不能只登记账号。每次存入和每次授予提币原生权限前重新核验当前代码，升级成未知代码后立即拒绝授权。
- 第一版以已核验的标准代币实现为基线；其他实现经审核、测试后通过既有七天治理升级增加。BP 不能自行绕过准入，避免让中继变成资产安全管理员。
- 保留原生 RAM 通知限制和 inline 授权限制，继续对标准代币新收款余额行计费并设置预算。
- 仍然依赖代币发行规则和链治理。代码准入解决共享托管权限暴露，不承诺代币价格、偿付或发行方行为。

Keep one custody entry point and the multi-issuer/symbol/precision ledger. Admit reviewed WASM hashes, and check current issuer code before accepting a deposit or granting an outbound native authorization. Account-name lists alone are insufficient because code can change. Add other implementations only after review/testing through existing seven-day contract governance. BPs must not override the global admission rule. This retains issuer-value and governance trust boundaries.

This is the smaller first-release change. It supports many tokens using reviewed implementations, but does not accept arbitrary custom issuer programs.

## 方案二：任意自定义合约采用独立托管 / Option 2: separate custody

- 主账本及 AIC 费用托管不向未知代币合约授予原生账户权限。每个发行方使用独立的托管账号、固定且有预算的 RAM 和受控的托管代码。
- 原生存入直接发送到对应托管账号；cleos/SDK 查询路由并隐藏内部账号选择。不能先把未知币收进主托管账户再转出隔离，否则第一次转出仍暴露主账户授权。
- 主账本只接受已登记托管代码的可验证通知，核验发行方、精度、金额、储备和费用。未知发行方只能影响它自己的托管预算，不能控制主账本或其他币的托管权限。
- 所有托管账号不得保留部署者 owner/active 私钥；权限只授予受治理控制的托管代码。新托管创建、RAM 资金来源、代码升级、路由冲突与恢复均须单独设计和测试。
- 发行方仍可冻结或伪造自身代币。隔离的目标是把影响限制到该发行方，不能让不可信代币变成可信资产。

The core/AIC account must never grant authority to unknown issuers. Each issuer needs a separate custody account with its own bounded RAM and controlled vault code. Deposits must go directly to that vault; forwarding from the shared account would reintroduce the same exposure. cleos/SDK routing can preserve a simple user command. The central ledger must authenticate registered vault code and verify accounting. Vault creation, fee/RAM funding, key removal, governance, upgrades, routing and recovery require additional implementation and qualification.

This preserves support for arbitrary custom issuer programs while isolating availability impact, but changes custody routing and needs a separate development cycle. No nodeos or system-contract change is assumed authorized by this document.

## 当前发布状态 / Current release state

当前 AIC/USD 资格及制品见 [20261103-token-whitelist](../../../performance-results/eosio-key/20261103-token-whitelist/REPORT.md)。拒绝未列币种及批准 USD 被替换成恶意代码的路径；USD 文本收款人必须预先 open。稳定 v1.0.0 标签/制品不变，未进行 TPS 或任意自定义代币资格测试。

The qualification is limited to the reviewed whitelist and public TEST deployment. It does not implement native exact RAM metering or arbitrary-code custody isolation.

历史任意代币版本实际复现：正常系统 WASM 的私有链上，`eosio.key` 提取测试恶意币时额外被计费 **4,330 字节 RAM**，`balance_count` 未变化，四种资产的储备等式仍成立。此项是已确认的资源隔离失败，见 [FUNCTIONAL_R4.json](../../../performance-results/eosio-key/20261103-v2-security/FUNCTIONAL_R4.json) 中 `unreviewed_issuer_RAM_exposure` 和 `release_qualified=false`。

The normal-system wallet itself reproduced the extra **4,330-byte** charge without changing its row counter, while all four reserve equations remained valid. This is a confirmed resource-isolation failure. Functional execution PASS does not mean release approval.
