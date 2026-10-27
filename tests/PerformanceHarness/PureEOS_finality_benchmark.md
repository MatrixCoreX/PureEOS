# PureEOS 最终确认 TPS 工具

此入口独立于旧 `performance_test_basic.py`，不改变旧测试报告的含义。包含预签名 HTTP 入口，以及第二批新增的 C++ 发送、隔离单进程部署、环境取证和阶段计时入口。**工具存在不等于已经测出最大 TPS；真实链验收状态以计划和证据索引为准。**

## 使用边界

- 使用已完成创世、合约部署、正常账号资源配置、正票激活和治理封口的**独立测试链**。本工具不替代这些步骤，不修改 CPU/NET 参数、资源比例、验签或持久化策略。
- 明确指定测试链 ID；初始化核对身份成功后才发送交易。不要指向生产链或已有手动测试链。
- 输入为预签名的普通业务交易，一行一个 packed transaction JSON（`signatures`、`compression`、`packed_context_free_data`、`packed_trx`），可由现有 cleos/签名工具离线准备。输入必须包含足够多的唯一交易，TAPOS 和过期时间应覆盖全部预热、测量及排空时间；不能重复使用相同转账生成相同 ID。
- 交易 ID 从未压缩的 `packed_trx` 重新计算，拒绝重复、压缩格式错误和声明 ID 不一致。可附 `action_count`，但 action/s 只是输入声明的辅助指标，不是已从 ABI 解码核实的数值；交易数始终按唯一 ID 计算。
- 输入文件包含交易签名，应放在测试目录中保护好。证据仅保留 ID、receipt、观察时间及错误类别，不写签名、交易原文或密钥。
- 不关闭磁盘余量保护，不删除旧证据。测真实安全写盘成本须使用真实磁盘；RAM 盘数据不能当作 SSD 性能结论。

## 运行

从 PureEOS 仓库根目录执行（下面的 URL、链 ID 和输入文件必须换成自己的**独立测试链**）：

```bash
python3 tests/PerformanceHarness/finality_benchmark.py \
  --endpoint http://127.0.0.1:独立测试端口 \
  --chain-id 测试链的完整chain_id \
  --transactions /绝对路径/unique-signed-transactions.jsonl \
  --evidence /绝对路径/新文件-evidence.jsonl \
  --rate 10 --warmup 120 --seconds 600 --drain 30
```

此例至少需要 7200 笔不同交易。先以低负载验证链身份、余额、资源和证据，再逐级增加速率；正式对照每档至少重复三次。同档配置和交易类型应保持一致。`--inflight` 限制同时进行的 HTTP 请求；不会使用无界任务队列补偿发送器超载。

独立重算（不访问链、不签名、不发送交易）：

```bash
python3 tests/PerformanceHarness/finality_tps.py /绝对路径/新文件-evidence.jsonl \
  --output /绝对路径/新文件-report.json
python3 tests/PerformanceHarness/test_finality_tps.py -v
```

输出使用独占创建，不覆盖旧文件。中断、缺少末尾 `end`、观察没有覆盖完整窗口、LIB 回退/跨分叉、缺失父块或相同块 ID 内容矛盾都会报错，不能按成功完成解释。

## 统计口径

| 字段 | 含义 |
|---|---|
| `sent_unique_tps` | 测量窗口内第一次实际发起请求的业务 ID 数 / 固定窗口秒数 |
| `rpc_accepted_unique_tps_diagnostic` | RPC 接受 / 预执行成功的观察速率；不是最终 TPS |
| `canonical_included_unique_tps` | 证据结束时主链上的成功业务交易，首次观察上链发生在窗口内 |
| `finalized_success_tps` | 首次观察到最终确认发生在 `[start,end)` 的成功业务 ID 数 / 固定窗口秒数 |
| `finalized_during_drain` | 窗口结束后才观察到最终确认，单列，不补入前一窗口的分子 |
| `cohort_*` | 窗口内发送的这一批交易，到窗口结束/证据结束时的完成及未决数量 |
| `sender_missed_schedule` | 发送线程、日志/CPU 或并发额度跟不上目标速率；该档不能声称测到链容量 |
| `observer_error_events` | 观察 RPC 异常；此时完整测量标志为假 |
| `max_observation_gap_ms` / `max_sample_collection_ms` | 轮询间隔和样本采集开销，必须与延迟结果一起解释 |
| `max_head_lib_gap_blocks` | 观察到的 head 与 LIB 最大高度差；不是固定的秒数 |

延迟使用同一进程的单调时钟，从发送到观察上链/最终确认，报告 nearest-rank P50/P95/P99。**不**用生产者区块时间戳减本机时间，不把区块数乘固定时隙当作实际测量时长。轮询、下载区块及 RPC 延迟会使观察延迟偏大，且可能把边界附近的确认推入排空阶段；不能当成精确共识内部延迟。

空块、漏块和长时间没有新确认均保留在固定时间分母中。失败 receipt、未知/部署交易、onblock、重复 ID 及孤块不计成功 TPS。预热发送、测量窗口内才确认的业务交易计入确认吞吐量，但不属于测量窗口发送 cohort。

证据为单运行、单时钟域格式 v1，包含 `window / anchor / sent / ack / block / sample / error / end`。不允许把不同机器或重启前后的时间线混合分析；目前未实现跨机时钟域合并。`sample` 的 head/LIB 来自同一次 get_info，按完整 ID 获取块，沿 previous 校验祖先关系。

## 轮岗缓存观测

启用既有 Prometheus 插件后，新增四个无账号/区块/交易标签的指标：

- `nodeos_rotation_context_cache_hits_total`
- `nodeos_rotation_context_builds_total`
- `nodeos_rotation_context_build_seconds_total`（成功构建耗时，不含等锁时间）
- `nodeos_rotation_context_cache_bytes`（当前缓存大小）

缓存只保留当前一项，已返回的共享只读结果可在替换后继续安全使用；进程重启不恢复缓存。完整父块 ID 决定 v1 数据，大小查询和读取使用相同数据，不读取本地可变化的 QC/LIB，不缓存权限判断。原 C++ 复制返回接口与 WASM wire ABI 均保留。

## 第二批：隔离部署与 C++ 发送

构建使用同一配置的 Release 程序，增加目标 `trx_generator`。以下命令中的目录需替换为实际构建、合约和有足够余量的 Linux 本地磁盘目录：

```bash
python3 tests/PerformanceHarness/pureeos_benchmark.py \
  --build /绝对路径/Release构建目录 \
  --contracts /绝对路径/reference-contracts/build/pureeos-rotation-v2 \
  --generator /绝对路径/Release构建目录/tests/trx_generator/trx_generator \
  --output-parent /绝对路径/独立测试父目录 \
  --rate 10 --warmup 120 --seconds 600 --drain 30 --repeat 3
```

入口使用新的随机隔离目录、端口和身份；正常部署当前系统合约、激活原生轮岗特性、配置 10/90 资源、登记 `nodea`、取得正票、先投票再买 REX，最后固定 14 天治理封口。普通 `alice/bob` 正常付费购买 PowerUp，各购买 20% CPU/NET；不使用特权无限资源。此资源档可能先达到账户配额，因此要检查账户 CPU/NET，不能把资源拒绝等同机器处理上限。创世发行量为测试用的 10 亿 SYS、最大供应量 20 亿，留出奖励增发空间，不修改奖励规则。

`eosio` 只作引导，`nodea` 成为当值者；两把引导/接班签名配置仍只算 **一个物理 nodeos 进程**，绝不是两节点或 21 节点容量验证。发送器用 `SYS` 转账，保留旧生成器默认 `CUR` 兼容性；一笔普通转账加去重 context-free nonce，action/s 只数普通业务 action。

每轮保留 `observer.jsonl`、`sends.jsonl`、合并的 `evidence.jsonl`、发送器日志、每秒主机采样、`report.json` 和前后 Prometheus 抓取。顶层 `manifest.json` 包括实际程序/合约哈希、源码及子模块指纹、Release/OC 标志、编译器、CPU、资源/治理配置。源码指纹描述采样时的工作树；使用外部预编译程序时还必须核对相应编译依赖证据，不能凭当前源码推定旧二进制来源。

密钥仅由私有文件传递，不写入命令参数或性能报告。C++ 发送队列默认最多 256 笔；排队溢出、网络失败、写盘失败、少发交易、非零退出或缺失结束标记使测量无效。采用 Linux `CLOCK_MONOTONIC` 并核对 boot ID，拒绝不同机器/重启后的时间线拼接。P2P socket 写入不是节点接收确认，故 RPC 接受速率为 `null`；最终吞吐仍只依赖 canonical executed receipts 和 LIB。

自动部署入口在创建钱包之前检查本地 ext4/xfs/btrfs 块文件系统，要求至少 **8 GiB 安全余量 + 1 GiB 工作空间**。不降低 nodeos 既有磁盘保护；长测/多节点实际应预留更多。启动温度须低于 90°C，测量过程中达到 95°C 或可用内存不足 1 GiB 则作废并停止本轮。温度传感器缺失保留为空，不能据此声称已验证散热稳定。每轮都再次检查磁盘余量。不改内核、功耗或安全设置，不自动清除旧链。所有由本次启动的子进程在正常完成、异常或中断后收尾；保留链数据供核查。

已引导的独立 loopback 链可用 `finality_cpp_benchmark.py --help` 指定链 ID、P2P/RPC 端口、账号和私有 key JSON 文件；它不承担部署或多节点拓扑管理。`--output` 必须为不存在的新目录。

## 可选阶段计时

nodeos 增加 `--performance-metrics`，默认关闭。自动部署入口用 `--timing` 开启，须另跑相同负载的关闭/开启对照量化采样开销。

新增 `nodeos_performance_metrics_enabled` 和固定 15 种 `stage` 的 `nodeos_stage_duration_seconds` 直方图，覆盖：签名排队/恢复、主执行队列、普通交易执行、onblock、确认票排队/BLS、恢复文件等锁/打包/文件同步/目录同步/总保存/清理，以及 finalizer 安全记录文件同步/总保存。没有按交易、账号或区块 ID 增加标签。原有出块阶段和网络指标继续复用；尚非完整 WASM/host/数据库逐操作跟踪。

这些是**包含失败尝试的进程墙钟时间**，可重叠或嵌套，不应相加当作 CPU 使用量，也不进入资源计费、验证、共识或持久化决策。持久化格式和 fsync 顺序不变。

```bash
python3 tests/PerformanceHarness/stage_report.py before-metrics.txt after-metrics.txt
python3 tests/PerformanceHarness/test_benchmark_harness.py -v
```

阶段报告给出两次抓取间的增量、平均时间及 P50/P95/P99 **桶上界**，不是逐样本精确分位数；落入 `+Inf` 或无样本时为 `null`。计数重置、桶不一致会拒绝比较。

## 尚未完成的压测工作

- 新自动部署入口在真实 SSD 上的端到端验收、独立 4/21/22 进程拓扑接入和交易负载组合。
- 每线程/更细粒度执行分析、观察器/日志/计时自身开销和独立发送器容量验证。
- 当前版本与优化版本在同一 Release 配置、真实 SSD 上的 1/4/21/22 节点对照，以及 2 分钟预热 + 10 分钟稳态 × 3 的容量验收。

Python HTTP 发送、逐事件日志和 get_block 观察本身都有容量上限。`measurement_complete_without_sampling_or_sender_errors=true` 仅表示没有记录到采集/发送调度错误，不代表已达到链容量，也不代表没有业务交易被拒绝。高 TPS 档需接入高速生成器并证明观察器和发送器都有余量。不得将本目录的模拟 HTTP 单元测试结果当成链 TPS。
