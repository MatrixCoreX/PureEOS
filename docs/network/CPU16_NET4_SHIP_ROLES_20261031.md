# CPU16 / NET4MiB、SHIP 分工与实测 · 2026-10-31

## 简体中文

当前开发链参数为 `max_block_cpu_usage=3200000`、`max_block_net_usage=4194304`。本地状态库容量为 `chain-state-db-size-mb=16384`；该容量与链上初始 64GiB RAM 市场分别配置。并行配置保持 8 workers / batch24 / group8、chain threads6。P2P 单消息 64MiB、每连接 100MiB 在途交易保护、生产交易队列 1GiB、单笔 CPU/NET 限额与计费规则保持不变。

出块节点关闭 SHIP。非出块历史节点启用 SHIP，供 PureHistory 持续获取交易轨迹和状态变化；两类节点均保持完整验证和并行运行。当前实际部署在同一台保留的物理服务器，没有新增服务器，CPU、内存、磁盘仍共享。

### 当前公开测试链上的确认测试

机器为 AMD EPYC 4345P 裸机，8 物理核、16 硬件线程、标称 64GiB，Ubuntu 24.04.5，使用配套打包运行时。链 ID 为 `3da0d4adaf64140424652f95f0b65b2f113b5b142afaa3794fb041294c08711f`，首个 BP 为 `bpnodeaaaaaa`。普通参与账号均为 12 位名字，81,920 对转账账号，16,384 个账号自己质押，147,456 个账号自己付费 PowerUp，并由受益账号授权。账户、RAM、资源和签名准备在计时前完成。

投送目标 80,000 TPS，8 个连接，1 秒预热、12 秒固定窗口、90 秒排空，共 1,040,000 笔预签名普通 AIC 转账。计时期间出块节点持续运行、SHIP 关闭；非出块历史节点、PureHistory 及全部六个后端容器均停止。

| 指标 | 实测 |
|---|---:|
| 固定窗口入块 TPS | 36,892.67 |
| 固定窗口不可逆 TPS | 35,810.17 |
| 整批不可逆 TPS | 37,804.93 |
| 已发送 / 最终确认 | 1,040,000 / 1,040,000 |
| 排空后未确认 | 0 |
| 主线程 CPU | 96.13% |
| 节点 CPU 逻辑核当量 | 5.01 |
| 最大块计费 CPU，μs | 2,270,427 |
| 最大块计费 NET，字节 | 3,269,376 |

原生 blocklog 审计逐笔核对 1,040,000 笔实际执行交易，并独立核对 179,865 个余额账户、1 亿 AIC 供应量、21 亿上限、自有资源及最终状态完整性；结果通过。转账对在同一区块内及按顺序划分的 24 笔分组内均无重复，这不等同于调度器内部批次测量。计时期间原出块节点的 SHIP 日志大小、修改时间未变化。测量后启动的历史节点已核对同步高度和规范不可逆块 ID；历史回填及 API 验证作为单独的服务恢复流程记录，不计入 TPS。

### SHIP 开关配对对照

固定 CPU16/NET4MiB、16GiB 状态库、8 个连接及同一快照、初始块、逐字节相同的签名语料，仅切换 SHIP trace/state 日志。每组同为 80,000 TPS 投送、1 秒预热、12 秒窗口、90 秒排空，外部历史服务全停。

| 指标 | SHIP 关闭 | SHIP 开启 |
|---|---:|---:|
| 固定窗口入块 TPS | 36,567.83 | 28,084.50 |
| 固定窗口不可逆 TPS | 35,197.17 | 26,819.50 |
| 最终确认交易 | 1,040,000 | 1,040,000 |
| 排空后未确认 | 0 | 0 |

这对短窗中开启 SHIP 的入块 TPS 下降约 23.2%，不可逆 TPS 下降约 23.8%；两组原生状态及 blocklog 审计通过。配对使用仅 loopback 的独立测试链及保留的固定参考输入，其中包含原有 8 位账号；它与上述实际公开链 12 位账号测试分别记录。未单独量化状态库容量、连接数或其他历史参数的作用，也未做函数级性能归因。

以上均为单节点短窗结果，未达到 40,000 固定窗口不可逆 TPS，不代表持续、多节点或历史服务并行运行的容量。没有改动节点代码或晋级 SHIP 优化。完整证据分别保留在服务器目录 `/opt/pureeos-live-producer-noship-confirmation-20261031-r1` 和 `/opt/pureeos-tps-cause-isolation-20261031-r1`，包含原始结果、输入摘要、原生审计、CPU 采样与服务状态记录。

### 版本

- 节点编译源码：`00443330a0d29673700a4eebee78de3b0234c281`
- nodeos SHA256：`e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32`
- 系统合约源码：`432360edd6763be000626898435bffcfe1aaf866`
- 系统 WASM SHA256：`6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62`

## English

Current chain caps are `max_block_cpu_usage=3200000` and `max_block_net_usage=4194304`. Local state capacity is `chain-state-db-size-mb=16384`, separately configured from the chain's initial 64GiB RAM market. Parallel settings remain 8 workers / batch24 / group8 with 6 chain threads. The 64MiB P2P message limit, 100MiB per-peer in-flight transaction guard, 1GiB producer queue, transaction caps and billing rules remain unchanged.

The producer runs without SHIP. A non-producing, fully validating parallel history node supplies SHIP traces and state changes to PureHistory. Both roles currently share the retained physical server; no additional server was created, and CPU, RAM and disk remain shared.

The public test-chain confirmation used an EPYC 4345P with 8 physical cores, 16 threads, advertised 64GiB RAM, Ubuntu 24.04.5 and the matching packaged runtime. Chain ID, BP and exact source/binary hashes are listed above. All participating ordinary accounts have 12-character names: 81,920 transfer pairs, 16,384 self-staked accounts and 147,456 accounts paying for their own PowerUp, with beneficiary authorization. Preparation and signing were outside measurement.

Eight connections offered 80,000 TPS with one warmup second, a fixed 12-second window and 90 seconds of drain. The actual producer remained running without SHIP; the history node and all six history backends were stopped during timing. Results were **36,892.67 inclusion TPS**, **35,810.17 irreversible TPS** and **37,804.93 whole-corpus irreversible TPS**. All **1,040,000** submissions finalized, with zero unconfirmed. Main-thread CPU was 96.13%; billed block maxima were 2,270,427μs CPU and 3,269,376 bytes NET.

Native blocklog and independent state audits verified every executed transfer, 179,865 account balances, supply, maximum supply, self-funded resources and final state integrity. No transfer pair repeated within a canonical block or an ordered 24-entry group; these groups do not measure internal scheduler batches. Producer SHIP files remained unchanged during timing. The history node started afterward and matched canonical irreversible block IDs; history backfill and API verification are recorded separately from TPS.

The controlled SHIP pair held the snapshot, initial block, signed corpus, CPU16/NET4MiB, 16GiB state capacity and eight connections fixed. With external history services stopped, SHIP off/on achieved **36,567.83 / 28,084.50 inclusion TPS** and **35,197.17 / 26,819.50 irreversible TPS**. Both finalized all 1,040,000 transactions and passed native state/blocklog audits. SHIP reduced measured inclusion by about 23.2% and finality throughput by about 23.8% in this pair. This separate loopback-only chain reused fixed reference inputs containing existing 8-character accounts; the live public-chain confirmation above uses 12-character accounts.

These are single-node short windows, below 40,000 fixed-window irreversible TPS. They do not qualify sustained throughput, multi-node performance or performance with history services active. Other parameter effects and individual SHIP functions were not isolated. No node-code optimization was implemented or promoted. Complete raw evidence remains in the two remote campaign directories listed above.
