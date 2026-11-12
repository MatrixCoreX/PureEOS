# PX7 G6d：同步二级索引与提交后段失败边界

日期：2026-10-15。分支 `feature/parallel-tx-execution`，HEAD `3856ab5b0723da1a1c41d60751109af789403945`。接续 [二级索引正式提交接线](PX7_SECONDARY_ADOPTION_RESULTS.md)。证据目录：`build/local-tests/parallel-secondary-boundaries-20261015-QuhV04/`。

## 本批范围

本批修改 `unittests/parallel_secondary_wasm_tests.cpp`，不修改生产规则或预算。

- 五族二级索引的生产／串行验证／并行验证对照增加纯同步模式。真实执行只触及普通根和同步 call，不执行通知或 inline；controller 只在实际遇到 host 时升级模式。检查两个普通根、三个同步 trace、负业务返回保留写入及新根游标重置，并要求真正采用。
- 新增五族 × 两种晚失败 × 1/2/4 worker，共 30 个 controller 组合。无权限 inline 在二级索引和两个同步调用已执行后被原授权检查拒绝；另一组合通过真实签名的 `setalimits` 设置 RAM 上限，完整纯同步执行后由原 finalize 拒绝。
- 每次失败均与原串行异常、非计时 action/call trace 对照，核对整库完整性哈希、四账户 RAM、共同表不存在、失败交易去重记录不残留、成功尾交易唯一入块。解除测试 RAM 上限后，同一代码／工作池的新健康交易必须真正采用，串行与并行验证状态和轨迹一致。
- `adopted` 是执行采用计数，不等于获准入块：完整重放后在 finalize 因 RAM 失败的交易计为一次执行采用，但没有 receipt，也不进入区块。权限发送中途失败不计完整采用。
- 非计时 trace 对照补交易、action、call 的异常码／错误码。显式构造程序容器，移除上一批临时聚合表达式的 GCC 警告源。

这里是 tester 内实际 controller 的生产／验证，不是新版本 nodeos 进程测试，也不是 TPS 测量。原 50ms 生产预算、500ms 尝试预算、默认 100 档、无亲和性均不变。实验默认关闭，原节点／回退基线／经济治理规则不变。

## 验证状态

修正后普通编译成功且无 warning/error。普通专项预定三轮再完整三轮，六轮全部退出 0；尚未将本批标为完整验收。ASan/UBSan/LSan 首轮专项失败并按预定规则停止，详见下节。私有 TSan、消费者兼容及当前真实节点验证继续。

插桩构建期间观察到宿主存在其他 Rust 编译任务，已保存仅含进程名／资源用量的 `asan-build-host-load.log`，未干预其他任务。此记录解释为什么不能直接把构建墙钟当作本项目代码成本，不是测试失败的归因。`run-consumers.sh` 后续执行结果如下；限定私有范围的 `run-tsan.sh` 尚未执行，不能按已有脚本算验收通过。

本版插桩构建退出 0，墙钟 35:55.25，峰值 RSS 4,180,968 KiB；程序 528,145,504 bytes，SHA-256 `4f9def85a8e1ded980e5cd5388fb0df3fdf0e2c3f1817b29cf8d3df07613c7bd`。实际 ELF 依赖检查为 `asan` 和 `ubsan`，43 份冻结源码构建前后校验一致。这里不是全节点插桩：worker／覆盖层／采用器及选定 action/host/transaction context、SoftFloat 已插桩，完整 controller／Chainbase／FC 未全量插桩。构建结束时其他 Cargo 任务仍存活，先重建当前核心／原接口 oracle／兼容性三个消费者目标，未并发启动重负载测试。

### 插桩首轮失败：保存而不验收

`run-asan.sh` 在无本任务构建、检查未发现编译进程后开始默认 100 档、无亲和性的固定序列。首轮专项退出 **201**：选中 27 组中 **25 通过、2 失败**，**20,941 / 20,970** 条断言通过，275 个未选用例 skipped；98.47 秒、峰值 RSS 824,832 KiB。后续专项第 2/3 轮及全部完整轮次均未执行。

- 29 条失败均为生产采用／回退计数：成功对照中的 mixed 模式 2/4 worker 共 10 条采用数不足；晚权限失败组合的回退计数 13 条不足；故障后健康 mixed 交易 6 条采用数不足。报告上下文含 `preparation_deadlines`，多数亦含 `worker_deadlines`。纯同步、RAM 晚失败等其他断言未报告失败。
- 没有报告账本／非计时 trace 差异或 AddressSanitizer／UBSan／LSan 错误。但发生回退的场景不能据此宣称其正式并行重放已通过插桩，也不能把未执行的完整套件视为通过。
- 运行前 `vmstat` 两个瞬时样本的 CPU idle 仅 29% / 32%，不是安静宿主；后续又观察到其他桌面测试占用多核并伴随 Rust 编译。运行前无编译进程不等于无竞争。证据为 `asan-before-run-host-load.log`、`asan-before-run-vmstat.log` 和后续负载记录；没有将宿主或某个代码成本认定为唯一根因。
- 源码／程序运行前后哈希一致。失败 ELF 已逐字节比较、全量 SHA-256 校验后归档到上述挂载卷的 `secondary-boundary-asan-failure`，清单 `asan-failure-archive.sha256`。不覆盖失败日志或删采用断言，也不放宽生产 50ms／尝试 500ms。

### 消费者构建与原始运行结果

三个当前消费者构建成功，2:42.80，无 warning/error，源码前后校验一致。SHA-256：核心 `15f7373b255aca804035dd26e71413f4da05b75da7d37b8f107d078c8e265bbe`；原接口 oracle `d793c316cc308feb243c6a8e19596bdc5e2f3d529e78779e90a5a8869bd88a39`；兼容 `c5506033584297a56660dbd527e764068b755a17b5d136a0864bfc914460a967`。旧三程序已归档并逐字节核对，两版 token 哈希亦匹配。

首次准备运行消费者时再次检测到外部 Cargo 任务，包装命令在进入 `run-consumers.sh` **之前**退出 3，当时没有进入测试，这不是三个测试程序本身失败。没有停止其他项目的任务；已向用户询问是否便于暂停其他重负载任务，或先推进代码稍后集中测试。

上述为首次延期记录。外部任务消失后，重新记录宿主负载并实际开始原预定序列：

- 原接口 oracle 三轮，每轮 **3 / 21,094** 全通过。
- 核心第 1/2 轮，每轮 **63 / 12,417** 全通过，另一个基准导出入口按原脚本明确 skipped；分别 104.47 / 94.10 秒。
- 核心第 3 轮退出 **201**：选中 63 组中 62 通过、1 失败，**12,416 / 12,417** 条断言通过，115.39 秒。唯一失败为 `parallel_context_free_adoption_tests/failed_cf_child_undoes_parent_and_next_batch_does_not_reuse_old_capability`，`workers=4, failure=1` 的 `serial_fallbacks` 差值 0 而非预期 2。账本／原撤销／其他功能断言未报告差异。该旧用例未打印准备失败细分，不能仅凭计数就确认具体原因。
- 运行前无编译进程，但第 3 轮结束时又有外部 Cargo／多路 Rust 编译；保留 `consumer-actual-before-run-*`、`consumer-failure-host-load.log`。这是时序背景，不是唯一根因证明。
- 按预定停止规则，**当前 token 专项和 22 组兼容未执行**；不把较早版本的结果移用到本版。三个当前程序均逐字节核对后归档为挂载卷的 `current-parallel_*_test`，清单 `consumer-current-archive.sha256`。源码／三程序／两版 token 运行前后校验一致，`consumer-run.status=201` 保留。

已预先写好**仅一次**的 `run-asan-controller-diagnostic.sh`，对同一插桩程序的两个失败场景做后续负载对照，不放宽预算、负载或断言；目前尚未执行，不能替代失败的专项／完整 gate。消费者剩余 token／兼容可独立补测，但不得覆盖核心第三轮失败。先取得稳定运行窗口及编译／发现成本证据，再决定代码改造与对应复测；不重复整套直到碰巧通过。

### 下一步源码线索（尚未测量、未实现）

后续更新：上述一次插桩诊断包装命令在检测到外部 Cargo/rustc 后、实际测试之前退出 3，记录为 `asan-controller-diagnostic-deferred.status`，并未消费一次实际测试或得到测试结果。随后独立计数探针固定三轮确认跨模式重复编译成本，开始新一批有界缓存改造；见 [跨模式缓存记录](PX7_CROSS_MODE_CACHE_RESULTS.md)。本节以下文字保留为当时的线索，不能视作最新实现状态。

`controller::prepare_isolated_attempts` 每一发现轮重新提交全部交易；`isolated_worker_pool` 使用共享递增 `next`，没有将一笔重试绑定到同一 worker。另一个更直接的成本是每个 worker 只保留一种 executor：同批 mixed 交易与 tree/noop 尾交易可能反复切换模式、销毁彼此的模块缓存，即使只有一个 worker 也存在该路径。此策略原本用于限制资源，**不能只删除 reset 后无上限保留全部缓存**。

先用固定输入的编译次数、发现轮数及分段耗时验证上述成本，再评估稳定分配或跨模式共享的有界独占模块缓存。保持活动模块不可复用／淘汰、每次 VM reset、全部 key／限制、全 worker 资源总上限、取消和同一截止；比较多任务均衡性，不能用静态分配换取单交易通过却未经验证地牺牲一般负载。宿主负载、冷编译、模式切换及执行成本必须区分。这不是已确定根因或已取得 TPS 增益；完整 CDT／一般 host／真实节点等原范围不缩减。

| 检查 | 固定三轮结果 | 耗时（秒） |
|---|---|---|
| 二级与协调器专项 | 每轮 27 组 / 20,970 条全通过，268 个未选用例明确 skipped | 56.76 / 55.86 / 55.46 |
| 完整普通 worker | 每轮 295 组 / 49,112 条全通过 | 161.82 / 164.68 / 178.12 |

五族更新／混合／纯同步 × 1/2/4 worker，共 45 个成功 controller 组合，每轮 1,850 条断言；新增 30 个晚失败／复用组合每轮 2,340 条断言。全部正常生产和验证均核对采用数量，不能靠安全回退碰巧通过状态对照。没有报告状态或非计时 trace 差异。整套外层 480 秒 watchdog 不是交易预算。

普通程序 SHA-256：`2d9620e2ecfa0d688fa48026480feb78e883bb80273e604aaa6b099285d3d884`。43 份源码及程序运行前后、归档前均复核一致；审计 24 项、访问清单精确匹配和 `git diff --check` 通过。准确普通程序已逐字节比较保存到 `/run/media/guagua/新加卷/pureeos-secondary-boundaries-artifacts-20261015-1eePRE/secondary-boundary-worker`，哈希清单为 `normal-and-old-tsan-archive.sha256`。

源码：`before/` 保存修改前 43 份，`initial/` 保存首版，`fix/` 保存修正后 43 份；各有对应 SHA-256 清单。运行脚本核对源码／二进制前后哈希，固定轮数，遇到失败立即停止并保存日志。

## 保留的失败

首次普通构建退出 2：新增 noop 夹具将两个 C 字符串直接相加；另有 RAM 有符号比较和两种异常枚举的条件表达式警告。修正为 `std::string`、与原 RAM API 一致的 `int64_t` 和明确异常整数码。生产源码未改。原 `build-normal.log/.time/.status` 和首版源码保留；修正日志为 `build-normal-fixed.*`。

## 构建缓存与恢复

旧 TSan ELF `bb370804…` 在无人使用且哈希复核后，从 tmpfs 构建路径移至上述挂载卷的 `before-tsan`，移动后全量哈希仍一致；不是删除失败证据或链数据。新 ASan 因目标不存在必须重新链接，并由 `parallel_sanitizer_check.py` 检查实际 ELF 依赖，不凭 CMake 选项推断。

本目录保留 `wasm/native/pool-tsan-object-link` 三个原链接；tmpfs 中的二级与 SoftFloat TSan 目录保留为 `.tsan-boundary-cache`，库为 `libpureeos_softfloat_sanitized.tsan-boundary.a`。恢复 ASan 使用上一批保存的匹配缓存。`configure-build-asan.sh`、两版 CMakeCache、flags/link 文件记录准确过程。切回 TSan 前必须停止构建／测试、保留最新 ASan 缓存和 ELF、恢复匹配缓存并强制重新链接。保持挂载卷在线；没有分区操作，也未修改 Windows 原有文件。

## 未完成范围

本批边界通过不能替代完整 CDT multi-index、builtins/hash/console、当前 nodeos 生产／恢复、长驻与混合版本及 G7 性能稳定性验收。旧默认压力／插桩 deadline 失败继续保留，不以新功能通过覆盖。
