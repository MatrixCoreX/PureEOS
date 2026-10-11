# PX7 首批：精确依赖与按查询取数

日期：2026-10-11。分支 `feature/parallel-tx-execution`，起点 `0068f79`。本页只验收 G1/G2，**不是全面合约并行已完成**。后续任务见 [滚动计划](PX7_GENERAL_EXECUTION_PLAN.md)。默认关闭、代码哈希名单、15 个受限 host、既存行同 payer/同长度更新的限制不变。

## 实际完成

1. **同表不同记录可以采用并行结果。** 依赖从整表值改成具体查询及其选中记录；表存在性、ID、payer、count 仍被检查。写入必须登记主键。同一记录发生前序写，即使写回原值，仍按冲突处理。
2. **只复制实际查询需要的记录。** worker 遇到未知表/查询，退出当前尝试并返回拥有值的数据请求；全部 worker 完成本轮后，owner 捕获所需元数据、选中行或负查询结果，再以新批次身份重试。未知不等于不存在，不能在稀疏副本中推算相邻记录。
3. **有界发现，原序回退。** 最多 8 轮；私有发现阶段有 500ms 协作式本地总预算，生产仍受原有更短的墙钟预算限制。64 个实际访问表、4096 条查询/复制行、4MiB 值副本等限制保持。超限不是共识拒绝理由，仍走原序完整串行。
4. **复核开销去重。** 每个查询都检查选择结果，但同一对象的完整内容只比较一次，避免大记录被反复复制。正式写入、资源结算、receipt/trace 和撤销仍走原 owner 路径。

改造入口为 `parallel_state_overlay.hpp/.cpp`、`parallel_ordered_batch.hpp/.cpp`、`parallel_worker_pool.hpp/.cpp`、`parallel_canonical_adoption.cpp` 和 `controller.cpp:prepare_isolated_attempts`。没有修改系统合约、通胀、治理、轮岗或链状态格式。

诊断口径：`attempts/succeeded` 包含数据发现阶段的实际尝试/成功试算，包括后来丢弃的试算，不是唯一交易数；`serial_fallbacks` 也不包含链上所有串行交易。本批真实生产每组 16 笔，典型计数为 32 次尝试、16 次成功、8 次采用、8 次批次内回退。

## 最终代码的验证结果

证据根目录：`build/local-tests/parallel-general-20261011-IXc4kD/`。

| 检查 | 结果 |
|---|---|
| 原生状态/依赖/稀疏副本 | 51 用例 / 36,751 断言，独立 3 次通过 |
| WASM worker/工作池/协调器 | 36 用例 / 845 断言，独立 3 次通过 |
| 原生 Savanna 并行验证 | 10 用例 / 1,545 断言，独立 3 次通过 |
| 生产端 CPU/资源/队列 | 6 个功能用例 / 542 断言，独立 3 次通过；另有未启用的导出入口，无功能断言 |
| 当前核心重新链接的原路径兼容矩阵 | 21 组 / 183 用例 / 3,104 断言，1 次通过 |
| 原生隔离组件与 SoftFloat TSan | 57 用例 / 36,840 断言，独立 3 次通过 |
| 私有 WASM TSan | 34 用例 / 812 断言，独立 3 次通过；排除 2 个未插桩 controller 对照用例，无新增告警抑制 |
| 真实新旧 nodeos 验证/恢复/P2P | 每次 55 阶段，独立 3 次通过 |
| 真实生产者互验 | 新 0/1/2/4 + 旧程序 5 模式，独立 3 次通过，包含旧程序完整重放新产块及 SHiP 对照 |
| 审计与测试工具 | 审计 24 用例、进程夹具 12 用例通过；791 文件 / 262 host 快照一致，未批量批准 pending |

新增关键场景：

- 同表 3 个独立主键在 1/2/4 worker 下全部真实采用；逐块比对状态完整性哈希、LIB、块 ID 和共识相关 trace。
- 双重扣款：两笔试算各自看见余额 10、各扣 7，试算都成功；正式执行拒绝第二笔，非法块已采用前缀完整撤销，随后原合法块仍可接受。1/2/4 worker 都通过。
- 前序入账后，旧余额不足的失败试算能原序重跑成功；同记录 ABA、缺失点/范围、上下界/倒退/末项、同 count 的插入删除、对象重建、payer/长度变化、预算和中断均有检查。
- 4,100 行表只复制选中的 2 行；真实链合约已有 80 个无关 scope 时，目标 3 行仍采用。
- 探测合约连续读取 9 个新查询，超过 8 轮上限后完整串行，合法性与输出保持一致。

最终验证报告：`parallel-process-yada6xm9/report.json`、`parallel-process-xnvmyoru/report.json`、`parallel-process-l_73dwnd/report.json`。最终生产报告：`parallel-producer-q4xoyb3_/report.json`、`parallel-producer-mx8t0zf7/report.json`、`parallel-producer-xswrtabz/report.json`。均位于证据根目录。

单测日志为 `final-state-*`、`final-worker-*`、`final-validation-production-*`、`final-native-tsan-*`、`final-worker-tsan-*`、`final-compatibility.log`。

中间 G1 的 `parallel-process-4gysl1d8` 属于另一二进制，不混作最终结果。`g1-validation-production-1.log` 保留新夹具 WAT 拼接括号错误；修正后重新编译验证，未删除失败记录或降低断言。

## 程序归档与边界

最终 `nodeos-parallel` SHA-256：`1894343d79309a5b1f11c51d8ec19ebc71b5184a9d28d5688cf6fc6d092aaafd`。

核心验证程序：`bbd4748730dcac3dc8924987c05f04ae2d1427630c12d2e4ee80f1fc595551dd`。

私有 worker TSan：`edff51d042889fc1d15f0734d5e47da64e05925c9cdc34431c7e513f41a98529`。

持久归档 `g2-artifacts-1894343d.tar.gz`（证据根目录，约 66MiB）保存新节点、状态/worker/核心/兼容测试、两个 TSan 程序及 CMakeCache；SHA-256：`96e49f1e65a166442ab20e5d97192cc7e97d92606c9cb0c0d9540f965adc53c2`。

原 nodeos 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`；`main`/基线标签仍指向 `a4a40a6`，合约仓库原 `main` 不变。没有覆盖手动链、钱包、个人配置，也没有推送或合并。

**G3–G7 未完成：**真实 CDT/资产转账所需 host、账户依赖和通知，主键生命周期及 RAM payer，多 action/inline/sync-call 调用树，二级索引与一般接口，新的性能/推广验收。一次正式回退仍丢弃本批其余旧结果。

本轮未做新版 ASan/UBSan/LSan 检查，也未重跑原业务 14 组长回归或 180 轮长驻测试；旧 PX6 结果只作历史记录。TSan 不是整个 controller/Chainbase/FC 的全量插桩证明。

14 秒限量生产窗口包含有意暂停和对齐，16 笔成功不是容量 TPS。G2 增加了有界发现成本，本轮没有新的性能倍数结论，不能沿用 PX6 倍数推算通用资产合约收益。实验继续默认关闭，不推荐部署现有生产链。
