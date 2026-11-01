# 7 天治理即时功能验收（2026-11-01）

结果：**PASS**。无需等待 7 天现实时间即可完成执行期限与升级的功能验收。正式 nodeos 与正式 604800 秒规则均未修改，服务器时钟未修改，已有浏览器链未重置。测试没有新增云服务器，所有临时 nodeos 已停止。

## 测试方法与范围

在保留的 AMD EPYC 4345P 裸金属机上远程编译独立原生测试夹具。夹具建立比服务器当前时间早 8 天的独立创世链，产生真实签名区块、正常提案和执行交易。区块时间跨越完整 604800 秒审查期，全部历史区块时间均早于服务器实际时间。生产 nodeos 使用原版二进制，在隔离网络、parallel 模式下，从空状态完整重放已封存的不可逆区块；启用 `--disable-replay-opts --force-all-checks`，没有跳过签名、权限、WASM 或交易执行检查。

夹具采用测试框架的历史时间推进能力；这不等于等待了现实 7 天，也不证明 7 天连续运行稳定性。被拒绝的提前执行交易不会入块，其异常与提案保留断言由夹具日志证明；正式 nodeos 重放证明成功入块路径和最终状态。此前 93 项原生回归（111 次成功执行）及新 ABI 的 17 次复核保留原报告，本次不将重复运行或辅助读取程序冒充额外业务用例。

## 验收结果

| 项目 | 结果 |
|---|---|
| 期限前 0.5 秒执行 | 拒绝，待处理提案仍保留 |
| 满 604800 秒后正常开发者权限执行 | 成功安装指定 WASM / ABI |
| 原合约已有的真实缺岗永久黑名单 | 升级后保留证据，不再占用候选名额；第 211 名正常递补 |
| 正式 nodeos 空状态完整重放 | PASS，最终区块 1266，含创世块；重放 1265 个后续区块 |
| 完整规范状态哈希 | 与独立原生读取程序重放同一已封存区块得到的哈希完全一致 |
| 表元数据 / 付款人 / 计数 | 668 个物理表条目，全部一致；1730 条主表记录由完整状态哈希覆盖 |
| 候选二级索引 | rank、previous_rank、ticket、frozen_epoch 四个索引，各 212 行顺序完全一致 |
| 委员会与最终性 | 21 名出块者、21 名最终确认者、阈值 15 |
| 资源与发行 | CPU16、NET4MiB/块、每交易 NET512KiB、RAM 增长 1024 字节/块；初始发行 1 亿 AIC，上限 21 亿 |
| 功能开关与账号 | deferred stage1/2 继续禁用；7 天治理特性激活；212 个 BP 名称均 12 位 |
| 实际 PureHistory 模块读取新合约 | PASS：注册 212、有效 211、永久黑名单 1、在岗 21；正常、自投和被拉黑账号的票数拆分正确 |

本次新生成的集成场景有 **15521/15521 个断言通过**。已有“提案期间被永久拉黑的成员同时从有效否决分子、选民分母剔除，保留原始历史”的原生回归结果见上一轮完整报告；本次端到端历史重放场景中的永久黑名单产生于新提案之前，二者范围分别记录。治理仍是开发者发起、BP 在审查期内否决的模型。

## 精确来源与证据

- nodeos 源码：`5d95aa18fea665f4ee60eda13b01dec82ff43535`
- nodeos ELF SHA256：`e6fe4d55986d7ff3ccbccf3cc54943be0555ada07b28483c7954ebc4ab5a35b6`
- 系统合约源码：`15dea1aa8a4b158eb771c5b13947ddeaafd28910`
- 安装 WASM SHA256：`373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb`
- 安装原生序列化 ABI SHA256：`038ffbab99e3c261dfc0d1655ddd3acd75b89bfeaae350eb86e4df930564f959`
- 独立测试链 ID：`25f7b6b3f91a2961a2f33d6914b309be5e6e1458dfa8bf602c68eeb2726b49dc`
- 区块 1266 ID：`000004f2e25ae94b2670a9105d71c2045600a1e7e5d9c412497613a10884ff7b`
- 全状态哈希：`bbd17ae27b4fce0b1aaca78d763c6f6d53bc51f464823fbe403051d563f15fc8`
- 原始区块记录留在服务器：`immediate-replay-r1/export/blocks/blocks.log`；其 SHA256 与字节数见 r5/RESULT.json；不下载区块数据库、钱包、私钥、原始节点配置。

实际硬件为 AMD EPYC 4345P、8 物理核心 / 16 硬件线程、标称 64 GiB，系统 MemTotal 63360244 kB，Ubuntu 24.04.5。临时节点满足原硬件门槛并强制 parallel；没有串行比较，没有 TPS 或饱和压力工作负载。纯重放约 2 秒；r5 从空状态重放并复核耗时 4.356 秒，此数字不包含此前夹具编译、签名历史生成和整个排查过程。不能将这两个数字当 TPS 或完整工程耗时。

## 失败、修正与复测

1. r1：重放达到精确目标，但测试管理脚本使用 `systemd-run --wait` 的启动作业在正常停止时被取消，导致脚本失败。保留 FAILURE 和 REPLAY_TARGET_REACHED。
2. r2：重放完成；测试脚本把内部二级表的物理名称传给只接受主表 ABI 名称的 get_table_rows，查询失败。改用完整状态哈希、get_table_by_scope 元数据和四个正式二级索引复核。
3. integrity 与 r3：独立读取同一已封存历史，计算全状态哈希；正式 nodeos 比较全状态与表 / 索引均通过。
4. r4：组合脚本的 state 字典遮蔽同名检查函数。保留失败记录，修正名称。
5. r5：修正后从空状态重新执行整个重放与复核，PASS。
6. PureHistory 首次补测：在 systemd 完成网络命名空间初始化前进入其命名空间，连接等待失败。节点日志表明启动正常；改为每次重新进入目标命名空间探测 API，确认就绪后再导入实际部署模块，r2 PASS。未修改 PureHistory 业务代码。本项验证实际 Python 服务模块读取，不宣称另行完成 SHIP / Elasticsearch 全量重新索引。

## 现有浏览器链的独立状态

快照 UTC：2026-11-01T06:07:07.661883+00:00。现有链 ID 仍为 `03c98ab62ea764e74d2d80ac862b10f4d9c4dc1f0b7a3370692a44427f3f7aea`，31 个 API 均响应，31 个实例已报告新 nodeos 完整版本；待维护 0、维护中 0。此时正式链系统 WASM 仍为原 6ed4，治理特性尚待既有自动滚动流程的后续激活与正常提案；快照审查期仍为 14 天。该状态与隔离测试链的成功升级严格区分。后续正式提案会按已选择的 7 天规则运行，不用等待该现实期限来完成本次功能验收。

详细快照见 CURRENT_CHAIN_AND_CLEANUP.json。上一轮完整报告为 `../20261101-blacklist-votes-governance-r1/REPORT.md`。所有私钥与测试区块数据库继续只留服务器；此证据包仅包含白名单源代码、公开结果、日志、公开创世信息及文件摘要。

# English

PASS: immediate functional acceptance uses an isolated historical signed chain and the unchanged production nodeos binary. The 604800-second production review rule and host clock remain unchanged. Early execution is rejected in the native fixture; normal authorized execution after the full chain-time deadline installs the expected WASM/ABI. A fresh full-check nodeos replay agrees with an independent replay's complete canonical state hash, 668 physical table metadata entries, and all four candidate secondary-index orders. Existing permanent-ban evidence survives and the next eligible candidate fills the vacancy. Actual deployed PureHistory Python modules correctly read the new contract state. This is functional replay evidence, not seven days of wall-clock stability, TPS qualification, a public-chain reset, or a claim that the existing public chain already runs the new contract. Failed harness attempts and targeted corrections are retained.
