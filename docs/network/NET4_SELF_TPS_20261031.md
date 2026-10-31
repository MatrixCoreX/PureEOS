# NET4 与自付资源实测 / 2026-10-31

本次为两轮单节点纯 AIC 转账短窗测试。机器为 AMD EPYC 4345P 裸机，8 物理核、16 线程、标称 64 GiB，Ubuntu 24.04.5 及匹配打包运行时。CPU16、NET4MiB/块、P2P64MiB/消息、1GiB 生产队列、并行 8 workers / batch24 / group8；完整验证、权限、计费和冲突回退保持开启。

每轮目标投送 80,000 TPS，1 秒预热、12 秒固定窗口、90 秒排空，预生成 1,040,000 笔交易。账号、RAM、资源和签名准备不计时。16,000 个参与账号使用 12 位原生名字；1,600 个账号自己质押，14,400 个账号自己付费 PowerUp，并由受益账号授权。实际枚举的全部 1,608 条质押记录均为 `from == receiver`。

| 指标 | 第一轮 | 第二轮 |
|---|---:|---:|
| 实际发送 TPS | 80,000.08 | 80,000.00 |
| 固定窗口入块 TPS | 31,707.50 | 32,105.50 |
| 固定窗口不可逆 TPS | 31,551.33 | 31,447.00 |
| 已不可逆交易 | 562,181 | 569,731 |
| 已发送但未确认 | 477,819 | 470,269 |
| 最大块计费 NET，bytes | 2,983,248 | 3,215,232 |
| 最大块计费 CPU，us | 2,071,762 | 2,232,833 |
| 主线程 CPU | 96.84% | 96.93% |

本轮未达到 40,000 TPS 对照值。未入链投送按用户要求仅记录数量，没有补发；它们不计成功 TPS。两轮均未用满 4MiB 块 NET 上限。结果不代表长期、多节点或所有交易大小下的容量。

测量期间按授权暂停完整 PureHistory 后端，节点继续写 SHIP 日志；测完恢复并从新链创世回填。1,131,912 笔已确认交易与历史索引逐笔匹配，缺失和重复均为 0，公网 HTTPS/CORS 及交易详情查询通过。GUI 未自动化目视验收。

节点编译源码 `00443330a0d29673700a4eebee78de3b0234c281`，nodeos SHA256 `e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32`；系统源码 `432360edd6763be000626898435bffcfe1aaf866`，WASM SHA256 `6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62`。没有新的主线程优化或源码性能提升声明。

## English

Two short pure-AIC-transfer windows ran on an AMD EPYC 4345P bare-metal host with 8 physical cores, 16 threads and advertised 64 GiB RAM, using Ubuntu 24.04.5 and its matching bundled runtime. Caps were CPU16, 4 MiB NET/block, 64 MiB P2P/message and a 1 GiB producer queue, with 8 parallel workers, batch24 and group8. Validation, authorization, billing and conflict fallbacks remained enabled.

Each round offered 80,000 TPS: one warmup second, a fixed 12-second window and 90 seconds of drain, using 1,040,000 presigned transactions. Account, RAM, resource and signing preparation were excluded. All 16,000 participating native accounts had 12-character names; 1,600 self-staked and 14,400 paid for their own PowerUp, with beneficiary authorization. All 1,608 live stake rows were self-owned.

| Metric | Round 1 | Round 2 |
|---|---:|---:|
| Actual sender TPS | 80,000.08 | 80,000.00 |
| Fixed-window inclusion TPS | 31,707.50 | 32,105.50 |
| Fixed-window finality TPS | 31,551.33 | 31,447.00 |
| Finalized transactions | 562,181 | 569,731 |
| Sent but unconfirmed | 477,819 | 470,269 |
| Maximum billed block NET, bytes | 2,983,248 | 3,215,232 |
| Maximum billed block CPU, us | 2,071,762 | 2,232,833 |
| Main-thread CPU | 96.84% | 96.93% |

The 40,000 TPS comparison target was not reached. Unconfirmed submissions were counted without resubmission and excluded from successful throughput. Neither round filled the 4 MiB block NET budget. These results do not qualify sustained or multi-node capacity, or every transaction shape.

The complete PureHistory stack was paused during measurement while node SHIP logging continued. History was restored and backfilled from the fresh genesis afterward. All 1,131,912 finalized IDs and canonical block positions matched the index, with zero missing or duplicate records. Public HTTPS, CORS and transaction queries passed; no automated GUI inspection was available. Exact node and system source/binary hashes are listed above. No new main-thread optimization is claimed.
