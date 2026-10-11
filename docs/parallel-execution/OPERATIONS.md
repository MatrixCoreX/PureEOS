# 受限并行执行：实验、关闭与回退

本功能默认关闭，只适用于独立测试节点；不是主网上线建议。15 个 host 是 PX0–PX6 首版边界，详见 [历史访问边界](PX0_RESTRICTED_BOUNDARY_REVIEW.md)。PX7 G5c2 当前有 27 个受限入口、7 个只允许链接且调用必整笔回退的入口、228 个拒绝链接的生产导入；未声明内存最大值的模块也必须通过逐次增长保护。支持主键创建/修改/删除及表生命周期、长度/RAM payer 变化；多根动作和受审查的有代码通知共享本笔私有状态，跨交易可并行试算。正式采用按原顺序重放影响共享游标的查询、变更和通知，原资源/授权/回执及失败撤销保留。系统/原生特权处理、一般二级索引、inline、sync-call 仍走串行；不能只加代码哈希就宣称所有操作都已并行。进度和本版验收以 [PX7 计划](PX7_GENERAL_EXECUTION_PLAN.md) 及 [通知接入记录](PX7_G5C2B_RESULTS.md) 为准。

## 参数与模式

仅在新建的测试配置中追加，下列 `<...>` 为需要替换的占位符，不是实际值：

```ini
wasm-runtime = eos-vm
validation-mode = full
experimental-parallel-validation-threads = 2
experimental-parallel-validation-batch-size = 8
experimental-parallel-code-hash = <已审查合约 WASM 的 64 位十六进制 SHA-256>
```

这只开启区块验证试算。生产者另需显式添加：

```ini
experimental-parallel-production = true
```

不改变原链 CPU/NET/RAM、交易格式、出块间隔、21 人轮岗、finalizer、治理或奖励。哈希是本地准入门槛，不是链上许可，也不免除原签名/权限/资源检查。代码升级后新哈希默认串行。

带代码通知需要把**所有实际执行的已审查接收者代码哈希**分别加入本地名单，而不只是顶层 token/业务合约。任意子代码缺失、换成未审查代码、特权/原生能力、未支持 host 或局部预算不足，都只影响试算采用率，必须回到原执行；不要因此扩大未审查代码名单。当前最多 16 个顶层动作、整笔 256 个接收者步骤和 4,096 个游标/通知事件；通知之间共用原组游标，不是多个线程同时写一笔交易。局部限制不取代原协议限制。

线程数允许 0–16（默认 0），批次 1–64（默认 8）。不得与轻验证、trusted-producer 跳过验证、OC、JIT、Deep Mind 或原 profiling 混用；不满足入口条件时拒绝启动或保守串行，不降级共识验证。需要验证接管时使用完整检查而非跳过 undo 的快速重放。

试验先选 2 个 worker、批次 8，再与 4 个 worker 比较。1 个 worker 是排错对照，不是提速承诺；不要把机器全部核心分给交易池，owner、P2P、BLS、RPC 和存储仍需要 CPU。热点高、系统交易多或排队不足时保持关闭，生产端不会人为等待凑批。

## 当前工作区如何复测

本次构建目录是 `/dev/shm/pureeos-release-ESOoZv`，重启后 tmpfs 会消失；永久产物归档与哈希记录在阶段验收文档中。原 `programs/nodeos/nodeos` 没有被替换，新程序为 `programs/nodeos/nodeos-parallel`。在已配置的构建目录可单独编译：

```sh
cmake --build /dev/shm/pureeos-release-ESOoZv --target nodeos-parallel parallel_validation_test parallel_wasm_worker_test -j1
```

完整编译配置保存在产物归档的 `CMakeCache.txt`，其中路径属于本机，不能直接当作其他机器的配置。原生业务回归还依赖已经构建的配套合约夹具。

在仓库根目录运行真实生产者对照，会创建私有临时子目录和本地端口，不启动或重用已有链：

```sh
python3 -B tests/parallel_producer_process_test.py \
  --old-nodeos /dev/shm/pureeos-release-ESOoZv/programs/nodeos/nodeos \
  --new-nodeos /dev/shm/pureeos-release-ESOoZv/programs/nodeos/nodeos-parallel \
  --fixture-exporter /dev/shm/pureeos-release-ESOoZv/unittests/parallel_validation_test \
  --old-sha256 e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c \
  --output-parent build/local-tests/parallel-overlay-20261011-q42k0b
```

换成 `tests/parallel_producer_soak_test.py` 并加 `--workers 4 --rounds 180` 可复测长驻进程反复出块。仅供测试的确定性签名凭据留在该次私有目录，不得移作真实链密钥。

真实 token 生产互验：给上面的 `parallel_producer_process_test.py` 加 `--token-fixture`，默认使用仓库固定原版 token。测试本机当前 PureEOS token 时，再设置 `PUREEOS_PARALLEL_TOKEN_WASM=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-resource-split/eosio.token/eosio.token.wasm`。导出器严格核对该已审查产物的固定哈希，不接受任意外部合约；五种模式均重新创世，报告保存实际代码哈希和负载类型，仍不代表饱和 TPS 测量。

在 token 模式另加 `--lifecycle-fixture` 检查首笔首次收款创建余额行/表及 RAM 费用；另加 `--ram-fixture` 检查既有余额行换付款人的正负 RAM 转移，两者不能同时使用。固定 16 笔语料要求各启用模式确实采用目标操作，不能仅以回退后余额正确冒充通过。G4b 证据中的部分已结束测试数据库已压缩归档，恢复位置与精确清单见 [验收记录](PX7_G4B_RESULTS.md)。

token 模式加 `--multi-action-fixture` 检查一笔内创建、花费并关闭余额行，共 16 笔/33 个根动作，不能与 RAM/lifecycle 模式合用。单独加 `--coded-notification-fixture`（不能同时指定 token 等夹具开关）检查两个根动作和带代码通知：16 笔/32 个根动作，每笔 8 个接收者步骤。脚本检查 RPC 的嵌套通知树、原回执顺序、三节点历史摘要和旧程序强校验重放，并核对每阶段与结束时导出器二进制没有变化；测试运行时不要重链接导出器或节点程序。

固定语料性能工具是 `tests/PerformanceHarness/parallel_replay_benchmark.py`，参数为上述 old/new/hash/output-parent，以及 `--corpora build/local-tests/parallel-overlay-20261011-q42k0b/px6-corpora-v1 --repetitions 3`。它测完整重放，不测生产容量。每份报告保留二进制/语料/脚本哈希、完整性、全部样本和主机负载。

## 监控与关闭

`performance-metrics = true` 加原 Prometheus 插件可以观察固定标签的 capture、batch_wait、compile、execute、adopt、fallback、conflict。诊断默认关闭，标签有界。阶段计时有嵌套和线程重叠，不能相加推算 TPS；退出日志分别列出验证和生产的批次、试算、成功、采用、回退总数。检查实际采用量而非只看线程数。

PX7 G2 起，`attempts/succeeded` 计入按需取数的发现/重跑轮次，可能多于唯一交易数；成功试算也可能因本轮其他任务需要补数据而丢弃。`serial_fallbacks` 只统计已进入顺序协调器的回退，不代表全链所有串行执行。不能把这些计数直接相除当作链的整体并行率或 TPS。

首版不提供热切换。正常停机、等待 worker 退出并备份配置后，在新程序配置中删除生产开关（或设为 false），把验证线程数设为 0，再重启。没有并行持久状态需要迁移，也不需要重新创世或迁移资产。

回退旧二进制时使用原配置副本，删除所有 `experimental-parallel-*` 选项，旧版本不认识它们。先在独立恢复目录用可信快照/完整区块日志进行完整重放，核对链 ID、最终确认高度与状态。不要让两个进程同时打开同一个 data-dir，不要复制正在写入的 mmap 状态冒充一致备份。

若发现状态不一致，先隔离该测试节点并保存证据，不能仅关闭开关后继续使用疑似污染的状态。物理断电、磁盘损坏和跨机恶劣网络属于另外的恢复/部署验收；进程 SIGKILL 测试不能替代它们。

源码回退点是 `baseline/pre-parallel-20261011` / `main` 的 `a4a40a6a1e72c531cd8267758074229ccc0655ce`。分支成果没有自动合并、推送或覆盖该基线；有未提交个人文件时不要执行破坏性重置，优先在另一个 worktree 查看基线。
