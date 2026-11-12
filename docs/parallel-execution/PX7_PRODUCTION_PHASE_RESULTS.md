# PX7：生产预执行截止与有序提交阶段分离

日期：2026-10-15。分支 `feature/parallel-tx-execution`，HEAD `3856ab5b0723da1a1c41d60751109af789403945`；不改 `main` 或 `baseline/pre-parallel-20261011`，不改系统经济/治理，实验默认关闭。

## 起因与证据边界

前批 `parallel-pure-hash-20261015-tZE6di` 的普通哈希专项和完整 worker 各三轮全通过。ASan/UBSan 首轮哈希 **8 / 9、2,642 / 2,644**：两线程、混合 CF/sync 的一批四笔交易仅采用两笔，另外两笔安全回退；账本、非计时 trace、区块验证对照通过，未报告内存错误。打印的 worker/preparation deadline 均为 0，不能直接叫作 worker 超时。该批失败程序、失败日志与未执行轮次均保留。

源码发现阶段边界不一致：生产 `cancel.deadline` 为整个私有预执行批次设置至多 50ms；所有 worker 返回并通过最终准备检查以后，有序依赖扫描正确地只检查外部中断、pending 代际及真实出块窗口。然而传给 `production_adoption_slot` 的同一 `cancel` 还携带旧时间，随后 `push_transaction` 的 `stop_requested()` 会把已完成的结果判作过期。这可以解释“私有 deadline 统计为 0，却在后段回退”的一种路径；尚无原失败时的完整现场，不能宣称它是那次失败的唯一原因。

## 本批步骤与不变条件

1. 冻结并完成前批消费者验证，再新增确定性反例；不在编译/运行期间修改输入。
2. 在正式 controller 的生产回调里等待超过 50ms，分别等待在第一笔之前和已采用一笔之后；覆盖 1/2/4 worker、普通/CF/sync/mixed，要求预执行确实完成、账本及非计时 trace 与原验证器一致。
3. 同时验证延迟后的外部中断、pending 换代、不同交易 metadata、真实 block deadline 失效仍不能使用旧结果；错误后能够处理新批次。
4. 先在未修生产代码上保留确定性失败，再限定修改已经完成私有阶段之后的提交令牌。50ms 预执行、500ms 单次 worker、原 CPU 计费、授权/资源/undo、依赖冲突、真实出块截止及取消检查全部保留。
5. 以新源码哈希重新构建、固定轮次验证。旧批次通过不能转算为修改后的结果；没有测试的内容明确列出。

本批目录：`build/local-tests/parallel-production-phase-20261015-LQt8Ie/`。
挂载卷归档：`/run/media/guagua/新加卷/pureeos-production-phase-artifacts-20261015-nYHTAo/`。
初始快照 `before-sources.sha256` 覆盖前批 48 项及 controller 统计头文件共 49 项；`original-sources.sha256` 另固定十项原哈希/指针语义源文件。

## 未修版确定性反例

前批消费者原串行 oracle、核心、当前真实 token 各三轮及兼容 22/22 全通过，原程序均核验归档后，新增两组 oracle 测试及原哈希失败计数的完整诊断输出。生产代码没有先改。

反例构建退出 0，2:07.86、峰值 RSS 2,769,960 KiB。`diagnostic-sources.sha256` / `diagnostic-programs.sha256` 固定输入及程序；一次固定反例运行 8.31 秒，退出 201，**1 / 2、1,560 / 1,608**。24 种组合（1/2/4 worker × 四模式 × 两个延迟位置）全部复现，失败恰为 48 条采用/回退计数：第一笔前等待时采用 0/3、回退 3；采用一笔后等待时采用 1/3、回退 2。准备阶段成功检查、所有账本/非计时 trace 及区块验证对照均通过。另一组外部中断、pending 换代、metadata 身份及真实出块截止的 **192 / 192** 通过。源码/程序前后匹配，失败 ELF 逐字节核验为 `pre-fix-delayed-adoption-worker`，清单 `diagnostic-archive.sha256`。

这证明旧截止会淘汰已经完成的结果，但不据此重写前批 ASan 失败当时未采集的现场。反例和原 ASan 失败分别保留。

## 修正与待验范围

`libraries/chain/controller.cpp` 只在构造正式采用 slot 时复制外部中断 generation / observed，不把已经结束的私有阶段 deadline 继续带入。worker 输入、初始及最后准备检查、50ms/500ms 限制没有变化；真实出块窗口、原 CPU 检查、pending 代际、metadata 身份与后续撤销/冲突检查仍在原路径。

访问清单只更新已人工核对的 controller 一行：源码 SHA 及 130 个访问候选的行号顺移，200 个候选内容及原 host 注册不变；24 项静态审计及完整清单精确匹配通过。`fixed-sources.sha256` 冻结新 49 项，单个 `-j1` 修正版构建退出 0，9:09.35、峰值 RSS 5,814,140 KiB，无 warning/error。

修正版 worker SHA-256 `db13e4cf3429fd96912bd1312b811f25aefe4d7c6f7449f49fbb31ed6190b666`；消费者程序完整哈希在 `fixed-programs.sha256`。原取消/队列安全/私有 CPU/慢队头保护专项三轮各 **3 / 279** 全通过；哈希和新增延迟两组固定三轮各 **11 / 4,252** 全通过。相同源码夹具在旧生产实现上失败 48 条，在修正版上全部通过，没有改断言、强度或生产/worker 时限。

本版完整普通固定序列随后全部通过：核心三轮各 **63 / 12,417**（仅导出基准入口不选），原串行 oracle 三轮各 **3 / 21,094**，未修改的当前 PureEOS token 三轮各 **3 / 1,471**，完整 worker 三轮各 **311 / 58,904**（194.93 / 193.64 / 192.85 秒），兼容 **22 / 22**（78.45 秒）。默认强度 100、无亲和性、无并发自有编译或重测试，原预算不变；`fixed-run.status=0`，49 项源码、十项原 hash/指针语义源码、四个程序及 token 的前后校验均一致。该序列没有失败重试，修复前反例与前批 ASan 失败单独保留。这是本版普通构建验证，不是本版插桩、nodeos 进程或 TPS 验收。

一般 host、当前真实节点、其他插桩时序风险及 G7 仍未完成。本次只证明这条已定位的阶段截止问题获修，不能声称历史的每次 deadline/回退都由同一原因造成。

普通四个成功程序已逐字节比较后归档为挂载卷 `fixed-parallel_*_test`，清单 `fixed-archive.sha256`。完成独立无符号 helper 诊断之后，本版 ASan/UBSan 构建退出 0，2:53.91、峰值 RSS 3,603,616 KiB，无 warning/error；实际运行库已核验，程序 SHA-256 `b3d564fb9e5f9b20e55f1933bff53d0c8af9707bc21d5528576a227f94a399a6`。

## 内存检查首轮及新诊断边界

首轮完整哈希专项 **10 / 11、4,235 / 4,236**，19.47 秒，退出 201。原哈希生产/验证 **648 / 648**、新 24 种完成后延迟组合 **1,416 / 1,416** 全过。新增安全组在 workers=4、mode=3（真实 block deadline）时，进入回调后、尚未注入等待之前，`ready.succeeded - before.succeeded` 为 **0 而非至少 2**，以 fatal 前提断言中止该组；安全组已执行 **175 / 176**，末尾 16 条未执行。没有 ASan/UBSan/LSan 错误报告，但这不是专项通过。hash 2/3、cache/secondary/full 全部未执行。

该前提失败发生在待测“完成后延迟”事件之前，不是旧截止淘汰已完成结果的复现。现有日志未包含当时完整的 preparation/worker 计数，不能直接指定是预执行截止、VM timeout、其他失败或准备未开始；也不能删掉“确实完成过预执行”的断言让安全测试空转。49 项源码、十项原语义源码及程序前后校验一致；失败 ELF 逐字节核验保存为 `asan-phase-failure` / `asan-phase-failure-original`，清单 `asan-failure-archive.sha256`。

下一小步只给两个新 ready 断言增加完整失败上下文：attempts、succeeded、准备 deadline/budget/other、worker deadline/VM timeout/other。没有修改生产代码、任何断言、测试顺序/负载或时限。`asan-detail-sources.sha256` 冻结同 49 项的新测试诊断版，原 `fixed-sources.sha256` 及完整普通通过记录保留；已启动单任务诊断版重编译。预先约定完整 11 项哈希固定三轮、首次失败即停；即使三轮通过，也不倒推原缺失计数或销毁原失败。不是另一次无记录重跑直到绿。

诊断版构建退出 0，4:30.97、峰值 RSS 3,291,932 KiB，无 warning/error；实际 ASan/UBSan 库已验证，SHA-256 `4ecfebcc1be6e217e6a5725af3ce174a9eddd8e90e4918d0e5de0e1c3445d5d1`。固定序列首轮再次在 ready 前提处中止，**10 / 11、4,139 / 4,140**，19.34 秒；这次为 workers=2、mode=1，计数明确为 **attempts=6、succeeded=0、preparation_deadlines=1、preparation_budget=1、worker_deadlines=2、preparation_other=0、vm_timeouts=0、worker_other=0**。因此本次诊断失败是私有准备期的原 50ms 截止，不是已完成结果在 owner 阶段过期；它不能替原首次失败填补未记录的计数。原哈希生产及 24 种完成后延迟组合再次全过，无检测器内存错误。hash 2/3 未执行，固定失败程序、源码和运行前后哈希保留；程序已逐字节核验归档为 `asan-ready-detail-failure` / `asan-ready-detail-failure-original`，清单 `asan-detail-archive.sha256`。

不降低 ready 断言，不改变 50ms/500ms，也不将本次失败算成功。保留给 G7 的冷准备/调度稳定性验收；独立继续 G6e2 的整数接口实现和实际 ABI 验证。九个整数接口是原 43 项的一部分，其余 34 项及完整 CDT/控制台/真实节点/性能门槛仍在原目标内。
