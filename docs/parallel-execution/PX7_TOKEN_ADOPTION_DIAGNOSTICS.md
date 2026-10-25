# PX7：真实 token 多动作采用失败诊断

日期：2026-10-16。分支 `feature/parallel-tx-execution`。

证据：`build/local-tests/parallel-token-adoption-20261016-mDSMO0/`；准确程序归档位置：`/run/media/guagua/新加卷/pureeos-token-adoption-artifacts-20261016-HwQ5wf/`。

## 起因与边界

[联合发现批次](PX7_CALLEE_DISCOVERY_RESULTS.md) 的完整普通 worker 三轮、原串行和核心各三轮通过；真实 PureEOS token 第二轮却出现一条生产采用数 **0 != 2**。其余 1,470 条断言通过，没有账本/回执/RAM 差异报告。保留首轮通过及第二轮失败，未执行第三轮和兼容/插桩，失败程序与源文件已归档，不重跑覆盖。

原日志缺少 worker 数和阶段计数，尚不能确定是准备截止、worker 截止、能力拒绝或其他路径。现场其他项目的构建进程占用较高 CPU，只是相关线索，不能直接认定根因；不停止其他项目进程，不改变亲和性、断言或原预算。

## 本批改动

仅修改 `unittests/parallel_validation_tests.cpp`：复用 `parallel_test_diagnostics.hpp`，在原多动作 token 用例的生产批次结束后、并行验证结束后，分别记录 worker 数、采用/回退数、实际尝试/成功数、准备 deadline/budget/other 和 worker deadline/VM timeout/other。原交易、授权、节点配置、账本/回执/RAM 及采用数期望完全不变。

70 项输入单独快照；除这个测试文件外的 69 项逐字节与上一批比较一致，31 项原实现不变。单个 `-j1` 诊断版构建成功，**4:51.65**，峰值 RSS **2,962,208 KiB**；保留一个旧 Boost 断言的有符号/无符号比较 warning（原用例、当前行 1476），没有编译错误。程序为 `55a6eb80ec53998b555d731e1bd04751b2f6dbc4718f3ee49d1c71d196d183e0`，不把增加日志称为修复。

## 预先固定的诊断

`run-diagnostic.sh` 使用与失败时相同的三个 token 用例、同一实际 token WASM，运行恰好三个独立进程。普通断言失败仍收集后续预定样本，最终只要任一轮失败就返回失败；不是跑到通过才停止。异常退出、信号或外部看门狗终止则立即停止；完整源码、原实现、程序与 token 在前后核验。

三轮均通过只能说明此诊断窗口未复现；若复现，须据阶段计数再定位。没有新的根因证据前，不改生产实现、不宣布问题已修复，也不把三轮诊断当作完整消费者回归通过。

## 实际诊断结果

三轮各 **3 / 3、1,471 / 1,471** 通过，分别 23.18 / 21.41 / 20.45 秒。每轮 0 worker 的生产/验证均零尝试和零采用；1/2/4 worker 的生产/验证均 **20 次尝试、4 次成功结果、2 笔采用、0 回退计数**，所有准备/worker 失败分类为零。注意成功结果数包含发现过程中的成功任务，并不是四笔交易提交。

本窗口未复现原失败，未确定原根因。高负载同时存在也不足以证明高负载必然导致失败；不据此改原生产预算。70 项相关输入、31 项原实现、准确程序及 token 前后校验匹配；程序逐字节归档为本批挂载卷 `token-adoption-diagnostic`，清单 `diagnostic-archive.sha256`。前批失败和未运行的兼容/插桩序列不被本结果覆盖。

另用现有 `wasm-objdump -x` 检查同一 token 产物，结果保存为 `token-wasm-structure.log`：23 个导入、46 个本地函数，没有同步 `call` 导入。结合新增联合提示仅位于同步执行器的调用目标读取，可确认这个 token 不直接进入新增同步提示路径；这不能排除程序布局、调度或截止等间接影响，也不能代替对原失败的阶段诊断。

## 独立线程检查

使用本批冻结输入检查联合发现的 worker/pool 新代码，不把它当作 token 采用失败的修复验收。单个 `-j1` 构建 **8:59.67**，峰值 RSS **3,396,248 KiB**，退出 0，无 warning/error；实际程序 `973ad5990da5cf0498af60ef111522aaec3e3eda4bc7627a1d388606ca1f33e4` 的 TSan 运行库、八个私有数值 C helper 前缀及插桩引用均核验通过。

联合发现 **1 / 693**、冷构造 **1 / 420**、单任务 **1 / 3,210**、缓存 **47 / 13,864**、二级 **23 / 2,945**，五类专项各三个独立进程全通过。

完整私有首轮退出 201，**221 / 223、55,364 / 55,367**，52.03 秒，峰值 RSS 107,972 KiB。两个旧调用树压力用例中止：

- `three_exclusive_modes_keep_owned_results_and_one_thread_local_cache`：1 worker、round 0、job 5，`failure=deadline`，**500,636μs**，两条失败。
- `tree_workers_overlap_real_wasm_intervals_without_sharing_writes`：1 worker、job 1，`failure=deadline`，**500,376μs**，一条失败。

后两轮未运行。日志未报告 TSan 数据竞争；但 118 项未全量插桩 controller/oracle 被明确排除，不能称完整链或完整私有压力检查通过。上一批无结果但缺原因的旧八任务用例本次通过，不能倒推此前的失败原因。本次始终默认压力 100、原 50ms/500ms、不绑核、不抑制错误。

70 项相关源码、31 项原实现及准确程序前后匹配；失败 ELF 两份逐字节归档为本批挂载卷 `token-adoption-tsan` / `token-adoption-tsan-original`，清单 `tsan-archive.sha256`。切换时保存两组对象缓存，不删除原失败程序或链数据。随后独立执行下述同源码内存检查；token 风险与当前压力截止仍未关闭。

## 独立内存与未定义行为检查

ASan/UBSan 构建 **18:14.63**，峰值 RSS **4,554,748 KiB**，退出 0，无 warning/error。准确程序 `83f413d387a327a2c449434bcaf23f3dd04aab8fd36b0122a6f2af0295ddaa68` 的实际运行库与私有 helper 插桩均核验；运行启用泄漏检测，检测器报错立即停止。

联合发现 **2 / 1,203**、单任务 **1 / 3,210**、缓存 **47 / 13,864** 三类各三轮全通过。联合发现每轮 24 个实际 controller 组合均与负对照配对，仍实测同步 **4 → 3**、混合 **5 → 4** 轮。

混合首轮退出 201，**27 / 29、6,148 / 6,155**，65.29 秒，峰值 RSS 831,052 KiB：

- `production_and_validation_replay_shared_cursors_both_queues_and_conflicting_tail` 的 1/2/4 worker 各两条采用/回退计数失败。对应 attempts/succeeded/worker-deadline/preparation-deadline 为 **27/0/1/1、30/2/1/1、30/0/3/1**，均有准备截止。这里 `serial_fallbacks=0` 不表示没有走原串行路径：此统计只计已准备成功的批次内部回退，准备阶段整体放弃另计。
- `original_inline_authority_and_ram_checks_undo_then_reuse_the_same_workers` 的 failure 0 / 4 worker，一条 `succeeded > 0` 失败。该断言缺少完整阶段计数，不能未经证据把它也归为截止；后续需给该旧用例补诊断。

混合第 2/3 轮、二级全部三轮、完整全部三轮未运行。日志没有 AddressSanitizer、UBSan 或 LeakSanitizer 错误报告，也没有账本/非计时 trace 差异报告；但测试失败，不得称完整内存检查通过。插桩仍非全量 controller/FC/Chainbase/nodeos。

70 项输入、31 项原实现和程序前后匹配；失败程序两份逐字节归档为 `token-adoption-asan` / `token-adoption-asan-original`，见 `asan-archive.sha256`。没有重跑到通过、减小压力或放宽原时限。

## 独立兼容性补测与剩余工作

`run-compat.sh` 独立补齐原失败序列未执行的 **22 / 22** 项兼容检查，退出 0；CTest 耗时 91.57 秒，外层计时 91.80 秒，峰值 RSS 129,772 KiB。包括原同步调用超时后的复用。准确程序仍为联合发现版 `1ddf71fb66ef0683b1545afe455822191f2e841873552a76ae93ac80a1c587b4`，源码/原实现/程序前后匹配，程序已在前批消费者归档中保存。这是独立补测通过，不修改原 token 失败序列的退出状态。

仍未关闭：token 偶发未采用的具体原因；默认压力下的私有截止；混合原授权/RAM 旧用例缺少阶段诊断的单条失败；联合发现的剩余目标/目录边界；控制台、完整 CDT、最新真实节点及真实性能验收。下一步先给 `parallel_mixed_tree_tests.cpp` 的上述旧失败断言补足已有完整统计，再依据反例决定优化或修复，不能把所有计数失败直接归为宿主性能。

本批没有改生产实现、默认开关、主分支/回退标签或系统经济治理。普通工作池程序仍是前批已三轮验证的准确 ELF；新增代码仅为消费者测试的诊断。所有测试/构建进程已结束，原失败、诊断、插桩和兼容结果分别保留；整个并行计划尚未完成。
