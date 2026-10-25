# PX7 G6e2a：九个 128 位整数 host

日期：2026-10-15。沿用 `feature/parallel-tx-execution`，HEAD `3856ab5b0723da1a1c41d60751109af789403945`；原共识/经济/治理、主分支和回退标签不变，实验默认关闭。

本批是原 43 个编译器接口中的九个整数运算，不替代其余 34 项、控制台、完整 CDT、当前真实节点与 G7 的原目标。前批已完成“预执行后旧截止误淘汰”修复的普通回归，但诊断版 ASan 在私有准备期触发原 50ms 截止，仍是未通过项；见 [阶段边界记录](PX7_PRODUCTION_PHASE_RESULTS.md)。不放宽时限，不删除失败，独立推进这批纯整数能力。

## 实现与原行为

- 新增 `parallel_pure_integer.hpp`：纯 128 位计算，无数据库、线程、host 或 VM 内存能力；以无符号位组合及 `bit_cast` 避免私有路径的有符号拼接/溢出。返回空结果表示整笔回退，不定义新的共识异常。
- `parallel_wasm_worker.cpp` 准入 `__ashlti3`、`__ashrti3`、`__lshlti3`、`__lshrti3`、`__divti3`、`__udivti3`、`__multi3`、`__modti3`、`__umodti3`，使用正确 i32/i64 ABI 和原 legacy scalar pointer 代理。整数有符号/无符号结果均保留原 16 字节对齐、未对齐写回、零地址及范围规则；检查当前既有截止，完全算完后发布结果，不另领时间预算。
- 逻辑左右移及原名为 arithmetic-left 的 FC 左移在 shift≥128 时按原规则置零；算术右移在私有路径仅接受 0…127，超界整笔回原实现，不为原未定义边界另造结果。负数右移用无符号位补符号，避免原生有符号移位依赖。
- 无符号乘法保留模 2^128；除零整笔回退；signed min / -1 返回 min，signed min % -1 返回零，保留原显式特例而不触发私有原生 UB。
- 全部九项是原 CF host，不添加普通 action-only 限制；不修改原 `webassembly/compiler_builtins.cpp`、原 FC 或数值运行库。

## 测试范围与未完成边界

`parallel_integer_host_tests.cpp` 新增私有五组、原 VM/controller 四组：

1. 无界 `cpp_int` 独立参考，包含正负极值、低高半字边界、移位 0/1/31/32/63/64/65/127/128/129/255/UINT32_MAX、除法/取模符号和 min/-1 特例。
2. 九项真实签名，16 字节对齐/未对齐、源参数与输出重叠、末尾有效指针、零/越界/溢出、失败后健康复用及取消。
3. 原模块 start 计算、apply 修改输出后再次复用，私有及原 VM 分别对照。
4. 1/2/4 worker 长驻池的成功/除零失败混排；不发布失败的私有写入。
5. 普通、CF、sync、mixed 图及独立冻结日志证明；CF 自行比较期望位，不调用原本被禁的 action return host。
6. 原 WASM 逐字节对照；先 store 再除零的原整笔撤销、私有前缀丢弃及后续成功复用。
7. 1/2/4 worker × 四执行模式中的九个接口正式生产采用、原串行/并行区块验证、完整非计时 trace 和账本对照；另测除零和超界 ASHR 的整笔回退、保守串行尾部及后续交易。超界 ASHR 只对照未改的原实现，不宣称其输出具有新跨平台定义。

新源加入普通和内存检查目标；私有 TSan 显式排除新的 controller/oracle 套件，不能把不完整插桩的 controller 叫作全链线程检查。私有普通支持集合由 35 增至 44，拒绝集合 163→154；原 262 项总数守恒断言及每项拒绝测试仍保留。

## 当前证据

证据目录：`build/local-tests/parallel-integer-hosts-20261015-ZS1Hr4/`。
挂载卷归档：`/run/media/guagua/新加卷/pureeos-integer-host-artifacts-20261015-TJY59q/`。

原 49 项源码先保存为 `before-sources.sha256`；`initial-sources.sha256` 冻结当前 51 项，另固定原哈希/指针及 compiler-builtins/FC 源码。24 项静态审计通过；访问清单 **807 源文件 / 262 原 host** 精确匹配。清单仅增加纯 helper 头，更新 worker 的源码/行号与新增结果指针 `.get()` 候选（255 行），不因词法扫描将任何路径自动批准；原 header 没有状态访问候选。

初版普通构建通过（8:11.70，峰值 RSS 3,132,036 KiB），程序 `53f2d08af221caa28d3e6bb2a6d2e8c3ee077745692bf2b2c1406a6293f27ab7`。首轮 integer 为 **8 / 9、8,852 / 8,853**，退出 201：原 VM 的全部数值向量被测试夹具装入一个区块，tester 每笔固定计费 2,000μs，剩余区块 CPU 只有 1,900μs 时触发 `block_cpu_usage_exceeded`。原始失败保留在 `normal-integer-1.log`，程序已归档为 `initial-block-budget-failure` 并逐字节核验；其余后续轮次按预定首次失败即停未运行。不能把这次中断当作所有数值向量已验证。

五组私有测试 **5 / 6,418**、另三组原 VM/controller **3 / 1,930** 均通过，其中 1/2/4 worker 的全部 14 种正式生产/验证场景 **1 / 1,797** 通过。仅修正测试夹具：每 32 个向量完成一个区块，保留每个输入、原 CPU 计费和区块资源限额，不修改任何生产源码。`fixture-fixed-sources.sha256` 仍为 51 项且只改变这一个测试文件。

## 分块夹具修正后的普通验证

增量构建退出 0，2:01.92、峰值 RSS 2,772,316 KiB；初版及增量构建均未报告编译 warning/error。新程序 SHA-256 `79b0d4e9cef0ff10f0c792803cc4f584ab90bd7d41827aadd0b03317e89dd52d`。

| 固定顺序 | 三轮结果（每轮） | 说明 |
|---|---|---|
| integer | **9 组 / 12,036 条，全通过** | 包括原 VM 全部向量，原先中断的用例现为 3,688 条全部通过 |
| hash | **11 组 / 4,252 条，全通过** | 包括生产阶段边界和安全检查 |
| 完整 worker | **320 组 / 70,931 条，全通过** | 默认 100 强度，无亲和性，原 50ms/500ms 不变 |

固定序列 `fixed-run.status=0`，每阶段独立进程各三次，首次失败即停规则未改变。51 项源码、14 项原语义源码和程序前后哈希一致；24 项静态审计及 807 文件/262 host 清单再次通过，diff 空白检查通过。成功程序逐字节核验归档为 `integer-worker`，清单 `normal-success-archive.sha256`；初版夹具失败仍保留。

这些结果是普通构建的私有路径、原 VM 和真实 controller 验证，不是 nodeos 进程、全量线程检查或 TPS 验收。

## 本版消费者与后续插桩

三个消费者重建退出 0，22.71 秒、峰值 RSS 209,716 KiB，无 warning/error。固定原串行 oracle 三轮各 **3 / 21,094**，核心三轮各 **63 / 12,417**（仅基准导出入口未选），未改的当前 PureEOS token 三轮各 **3 / 1,471**，兼容 **22 / 22** 全通过；兼容耗时 101.95 秒。`consumer-run.status=0`，源码、原语义源码、三个程序及 token 的前后哈希一致。

- `parallel_validation_test`：`95ac4bcea215f0e5a1c66b936f71ec5f57f90600800e11de96a110baa6557137`。
- `parallel_serial_reference_test`：`e02c1f7637cc9dcadebb87d7eaa309d351bf34a668769b970dde0b60bc5a89a1`。
- `parallel_compatibility_test`：`169e90c1b7344dc361ca00024c72f981c25e3addf0ff867c8ea89f5c71d79131`。

均已逐字节核验归档为 `integer-parallel_*_test`，清单 `consumer-archive.sha256`。私有 TSan 运行器的排除列表与当前 CMake 精确一致。ASan 完整失败归档后已分别保存当前 ASan 缓存、切换旧 TSan 缓存；重新配置退出 0，当前单个 `-j1` 构建中，尚无本版 TSan 运行结果。

## ASan/UBSan 当前结果

单个 `-j1` 插桩构建退出 0，10:32.84、峰值 RSS 4,593,560 KiB，无编译 warning/error。实际 ELF 运行库为 ASan+UBSan，程序 SHA-256 `590f61cec712208353011668066ec06205f21be67b2c291f2d8ebab3d7c88af8`。固定私有整数、原 VM/controller 整数、哈希、完整各三轮，首次失败即停；默认 100 强度、无亲和性、原 50ms/500ms、泄漏检查及错误即停均不变。

- 私有整数三轮各 **5 / 6,418** 全通过。
- 原 VM/controller 整数三轮各 **4 / 5,618** 全通过。
- 哈希与生产阶段边界三轮各 **11 / 4,252** 全通过。
- 完整首轮 **323 / 327 组、72,526 / 72,553 条**通过，退出 201；第二、第三轮按首次失败即停未执行。

完整首轮 5:58.29、峰值 RSS 1,058,940 KiB。27 条失败集中在四个旧用例，新增整数与哈希在完整轮中仍全部通过：

| 旧用例 | 失败数 | 这一次实际记录 |
|---|---:|---|
| mixed 递归正式采用 | 2 | 均为 4 worker，采用 0/1；worker deadline / preparation deadline 各 1 |
| mixed 签名、重复和 abort 后复用 | 5 | 私有成功或采用数不足；该旧用例没有相应完整截止计数，不能补写为已证实超时 |
| 五族二级索引生产采用 | 14 | 采用 0/1，全部有 preparation deadline；其中一条 worker deadline 为 0，其余为 1 |
| 二级晚失败撤销、尾部和复用 | 6 | 预期 fallback 2、实为 0，全部有 preparation deadline；worker deadline 分别有 0/1/2 |

这些是采用/回退的计数失败；原账本、非计时 trace、权限拒绝和撤销对照没有报告差异。整个固定序列未报告 ASan/UBSan/LSan 错误，但完整验收失败。51 项源码、14 项原语义源码和程序前后核验一致；失败程序逐字节核验后转存挂载卷 `integer-asan-full-failure` / `integer-asan-full-failure-original` 两份，清单 `asan-archive.sha256`，构建副本腾出 tmpfs 空间，可恢复，未删除链数据。

不把旧计数失败或此前 50ms 准备截止当作已修复，也不扩大时限、降低负载或反复整套重跑来抹掉失败。继续独立的本版私有线程检查，缓存按 ASan/TSan 分开保存；运行库切换后必须重新链接并核验 ELF。本节不构成未完整插桩 controller/FC/Chainbase 或真实 nodeos 的全量内存证明。

当前插桩、线程、真实节点和性能均未完成本版验收；此前准备阶段的 50ms 失败仍保留，不以这批普通通过抵消，不能勾选 G6 或全面并行完成。

## 私有 TSan 结果

构建退出 0，6:38.80、峰值 RSS 2,948,964 KiB，无 warning/error。实际 ELF 运行库只有 TSan，程序 `a00fba8df5dfadcd79663d12a98fa54e42c3b0a3cf852e053df83c0a1bd81be1`。默认 100 强度、无亲和性、原预算、错误即停，固定各专项三轮后完整私有三轮且首次失败即停：

| 范围 | 实际结果（每轮） |
|---|---|
| 冷并发构造 | 三轮各 **1 / 420** 通过 |
| 私有整数 | 三轮各 **5 / 6,418** 通过 |
| 私有哈希 | 三轮各 **5 / 1,360** 通过 |
| 缓存、混合私有与同步池 | 三轮各 **45 / 9,961** 通过 |
| 二级私有、日志与有序批次 | 三轮各 **23 / 2,945** 通过 |
| 完整私有首轮 | **213 / 215、30,273 / 30,275**，43.90 秒，退出 201；另 112 个未完整插桩 controller/oracle 明确排除 |
| 完整第二、第三轮 | **未运行** |

失败均为旧调用树压力夹具：`tree_workers_overlap_real_wasm_intervals_without_sharing_writes` 在 1 worker 的 job 0 明确记录 `deadline`、500,458μs；`three_exclusive_modes_keep_owned_results_and_one_thread_local_cache` 在第一个测试检查之前抛出 `isolated_execution_interrupted`。后者代码在入池前先计算三个串行参考，但缺少具体阶段/原因计数，只能记录为尚未完整定位的中断，不能直接套用旧 4 worker 失败原因。其余选中用例通过，没有 ThreadSanitizer 数据竞争报告；完整私有验收仍失败。

源码、原语义源码和程序前后核验一致。失败 ELF 逐字节核验后转存 `integer-private-tsan-failure` / `integer-private-tsan-failure-original` 两份，清单 `tsan-archive.sha256`，保留可恢复程序和全部失败日志；本版 ASan 缓存在本批 `*-asan-object-link` 及构建目录 `.asan-integer-cache`，当前构建配置为 TSan。不改断言、负载或时限来消除失败。独立继续剩余 34 个编译器 host，同时将 ASan/TSan 的采用/截止/中断问题保留在 G7，不宣称整数批次或总计划全部验收。
