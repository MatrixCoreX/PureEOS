# PX5 生产端接入与验收记录

日期：2026-10-11。默认关闭，没有替换运行节点、部署系统合约或修改 `main`。

## 实现边界

- `producer_plugin` 在原 incoming/unapplied 队列顺序收集有界批次；不增加凑批等待，不把新到的单笔交易改为异步成功，也不在 speculative/只读窗口启用生产者试算。
- controller 的同步批次作用域先收齐所有私有结果，再逐笔调用原队列处理逻辑。队列删除、保留、失败上限、主观计费和 RPC 回调仍由原处理器决定。
- 采用能力绑定 metadata 身份、交易 ID、代码/输入指纹、原位置、待出块代数与外部中断代数。回调未调用原 push、错配 metadata、异常、abort 后重新 start、截止或中断都会停用旧结果。
- 正式 push 仍完成原授权、交易去重、资源结算、receipt、undo 和输出。只有已审查的受限 WASM 成功结果可以替代一次 WASM 重跑；其他操作完整串行。
- 成功试算的执行耗时在首次正式写入前一次性补记。排队、等其他线程、未采用的失败/冲突试算不重复计入交易 CPU。事务和 action 的本地 elapsed 包含所采用的私有耗时；该字段不是新的共识数据。
- 整批私有阶段共用绝对截止，包括尚未开始的任务。本地阶段上限不能成为共识拒绝；如果真实出块截止还没到，仍可回到原串行处理。原 CPU 总额没有乘以核心数。

CLI 新增 `--experimental-parallel-production`，必须显式开启已有受限 worker 池，且满足原解释器、完整验证、审查代码哈希与互斥条件。只开验证线程不自动开启生产端。下述受限进程验收已通过；仍不建议用于生产网络，性能和扩大准入另行判断。

## 已取得的核心证据

证据位于 `build/local-tests/parallel-overlay-20261011-q42k0b/`。`px5-first-core-tests.log` 首轮通过，`px5-core-repeat3.log` 为初始版本重复。新增慢队首预算修复后，`px5-starvation-core-repeat3.log` 中验证端和生产端各在三个独立进程重新通过。

| 组别 | 功能用例 / 断言 | 覆盖 |
|---|---:|---|
| 当前验证端 | 8 / 851 | 原 6 项差分/恢复/切叉；新增指标开关一致性与白名单内不支持创建的完整回退 |
| 生产端 | 6 / 542 | 0/1/2/4 线程的按序非显式计费；串行核心验证生产块；冲突、旧状态失败、权限拒绝、重复、队列拒绝、回调异常、重入、metadata 错配、abort/start、截止、中断；100/500ms 窗口慢队首被原 CPU 规则拒绝后，后续正常交易仍能收录 |
| 单独的实时语料导出 | 1 / 14 | 当前三次进程测试的每份 `export.log`；新私有目录、当前时间创世、真实签名交易及仅供本测试的确定性凭据；初始烟测为 13 断言，后加创世 JSON 精度检查 |

CPU 测试在固定时间点验证精确加量和单次补记，覆盖最小 CPU、交易头上限、块 CPU 余额、账户主观剩余额度、已中断、无效时长、显式验块计费不变。另验证“私有 CPU 为 30ms、真实墙钟窗口为 20ms”不会重复扣减墙钟；测试无需实际睡眠 30ms。

导出用例在常规 CTest 未设置环境变量时不执行导出，因此常规生产端报告显示的第 7 个用例不计作第 7 项功能验证。真实导出另有上述独立结果；持续测试指定交易数量时再多一个数量输入检查。验证/生产各自选择 suite，报告中其他 suite 的 skipped 是显式测试筛选，不是悄悄忽略失败。

首轮构建停在新增测试的两处编译问题：Boost 消息中的条件表达式需加括号；资源额度夹具不能访问 controller 的私有 `mutable_db`。后者改用原 transaction_context 自带的 undo session 覆盖夹具资源变化，没有扩大生产 API 的数据库权限。失败构建日志保留。

## 兼容与线程检查

当前核心与工具复测 `px5-current-core-tools-repeat3.log` 五组各三次通过；进程检查工具后续增加精确 LIB 等待的两项单测，当前共 12 项。`px5-compatibility-repeat3.log` 中 21 组、183 功能用例 / 3,104 断言的原有只读、sync-call、索引、资源和 API 兼容测试各三次通过（不把重复次数累计成不同用例）。

兼容首轮 `checktime_observation` 出现测试计时断言失败：原夹具把日志 deadline 减去含签名 CPU 调整的 start 与请求墙钟上限直接比较，同时忽略日志毫秒截断。修复后直接捕获传给 controller 的实际绝对 deadline，并按相同日志精度做等值比较；没有增加 CPU 限额或放宽原计费规则。首次失败日志保留在 `px5-compatibility-regression.log`。

普通 worker 和 ASan/UBSan/LSan 均为 33 用例 / 816 断言，各三次通过；TSan 为 31 用例 / 783 断言，各三次通过。TSan 仅插桩私有 worker/状态/批次模块，排除依赖未插桩原 controller 的两项对照，不能称整个 nodeos/FC 已通过 TSan。相应日志为 `px5-worker-repeat3.log`、`px5-worker-asan-ubsan-lsan-repeat3.log`、`px5-worker-tsan-repeat3.log`。

`px5-current-native-regression.log` 的 14 组、264 用例 / 197,714 断言通过（本轮一次完整运行，1655.76 秒）：状态视图/串行参考/SoftFloat 基础，当前核心的原生 Savanna、原生与合约升级治理、一票一投、历史区块格式、10%/90% 资源、21/22 人确认边界、单席位轮岗、841 个真实注册候选人的容量边界、离线黑名单、出块奖励与减半、最低正票激活。委员会/轮岗部分是实际合约加独立 controller 验证，不冒充 21 台真实服务器。

## 真实进程重复验收

当前程序 SHA-256 为 `d6cac3777ab1e458d5dbcf59b719c40226defef34178a04d100e0bb7762683df`。以下全为真实独立 nodeos，不是 controller 模拟：

| 场景 | 三次报告目录 / 日志 | 结果 |
|---|---|---|
| 验证、恢复、切叉、关闭并行 | `parallel-process-o6ss7mu6`、`parallel-process-rwkvqrv3`、`parallel-process-q4g2_1ft`；`px5-validation-exact-lib-repeat3.log` | 每次 55 阶段通过；旧/新 0/1/2/4、真实 P2P、强杀、完整重放、损坏日志拒绝、快照和 SHiP 差分 |
| 生产与旧程序完整重放 | `parallel-producer-oh76qj70`、`parallel-producer-gyming6l`、`parallel-producer-f5ubs3a9`；`px5-live-round-aligned-repeat3.log` | 每次旧生产者及新 0/1/2/4 五模式通过，每模式 16 笔；启用模式有实际采用，关闭模式无生产试算 |
| 长驻进程持续运行 | `parallel-producer-soak-wfyvyyqu`；`px6-soak-180-rounds.log` | 180 轮、2,880 笔全部最终确认；详见 PX6 结果与暂停恢复修复记录 |

每次生产模式均验证原顺序且唯一收录、最低 CPU、旧验证者完整块 JSON、精确状态/LIB、活动槽间隔 500ms、三节点 SHiP 文件相同，停机后再由旧程序带 `force-all-checks` / `disable-replay-opts` 完整重放。新 1/2/4 每模式 16 次私有试算、8 次采用、8 次冲突后回退；四个 scope 的重复访问使全部采用本就不符合依赖规则。

固定 14 秒有限输入窗口统计唯一已最终确认 ID、P50/P95/P99、重复/错误和积压。16/14 TPS 是人为输入速率，不是吞吐容量。新旧独立生产夹具的实测 CPU 不要求彼此相同，但每个已产块在所有验证者和重放结果中必须一致。

### 保留的失败证据与处理

- `parallel-producer-ep78mrd7`：创世时间微秒经 JSON 往返后链 ID 改变；夹具对齐协议块时间并断言 JSON 往返链 ID 一致。
- `parallel-producer-tw1xrhoc`、`parallel-producer-it7xr26d`：交易全部正确收录，但末段局部窗口合法走串行，未覆盖实际采用。原出块提前量按 12 个槽累积，脚本现对齐整轮起点、延长观察窗包含等待，不改节点时限，也没有删掉采用断言。
- `parallel-process-swp2327k`：旧节点 get_info 块头缓存先更新、LIB 后更新（`get_info_db.cpp` 的 accepted/irreversible 信号分别发布缓存）。等待条件从仅块 ID 改为块 ID 与精确 LIB 同时满足；保持 30 秒上限，增加仍错误时的实际/期望诊断和两项单测。此后完整矩阵三次通过。
- 旧生产者的高度暂停恢复缺陷及修复，见 [独立记录](PAUSE_RESUME_FIX.md)。

## 诊断与性能边界

在既有默认关闭的 `performance-metrics` 中增加固定标签：`parallel_capture`、`parallel_batch_wait`、`parallel_compile`、`parallel_execute`、`parallel_adopt`、`parallel_fallback`、`parallel_conflict`。最后一项为零时长事件计数，只计首次实际读写依赖冲突，不把已经禁用的串行尾部重复计作冲突。阶段时间可能重叠，不应简单相加当成整块耗时。Prometheus 沿用原固定大小数组/直方图导出。

PX6 负载对照与最终适用范围另有测量记录。当前成功结果仍是受限既存行更新及私有计算，不是通用转账、多索引、跨合约或并行 OC。操作见 [实验与回退说明](OPERATIONS.md)。

## 永久产物归档

上述证据根目录中的 `px5-current-d6cac-artifacts.tar.gz`（SHA-256 `03cfac8b7079f285f76618468fbca3d5af0747dbb18ee76274ce770d6512f1e5`）保留当前 nodeos、验证/生产夹具、普通 worker、私有 TSan、兼容、原生业务测试程序及 CMake 配置；不包含任何运行签名密钥。TSan 程序 SHA-256 `33850949f91328959dd80b674e7b65f0f0340fddcdc6cd70cdbdbcc09115dab3`。

ASan/UBSan/LSan 程序单独保存在 `px5-worker-asan-artifacts.tar.gz`，SHA-256 `11a284a99940ec96e6b99af965ed3d3a7ba804782d929ea9d5b2c37238fc63a1`。不要将后来切为 TSan 的构建目录程序与此前 ASan 日志混配。
