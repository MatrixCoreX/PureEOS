# PX7：跨执行模式有界模块缓存

日期：2026-10-15。分支 `feature/parallel-tx-execution`，HEAD `3856ab5b0723da1a1c41d60751109af789403945`。接续 [二级边界与失败记录](PX7_SECONDARY_BOUNDARY_RESULTS.md)。本批证据：`build/local-tests/parallel-cross-mode-cache-20261015-lzNxnp/`。

## 改造前证据

独立探针 `build/local-tests/parallel-dispatch-profile-20261015-hOcKtg/` 链接冻结的上一批普通库，没有改动生产代码。程序 SHA-256 `88c6e7002d93ed71813db8e4b7664d06671453ccc95da9dd8ccf4134d5885020`，固定三次独立运行，每次 **1 / 5,696** 全通过。每次包含 1/2/4 worker、四种模式序列、480 个私有任务。源码、链接输入及程序前后核验一致，首编的三处诊断接口调用错误及修正日志保留。

| 固定输入 | 改造前编译次数（三轮） |
|---|---|
| 单 worker，同一代码连续 tree 32 次 | 1 / 1 / 1 |
| 单 worker，同一代码连续 mixed 32 次 | 1 / 1 / 1 |
| 单 worker，tree/mixed 交替 32 次 | 32 / 32 / 32 |
| 单 worker，每批 mixed+tree，32 批共 64 次 | 64 / 64 / 64 |
| 2 worker，每批 mixed+tree，64 次 | 16 / 30 / 4 |
| 4 worker，每批 mixed+tree，64 次 | 30 / 32 / 29 |

这是共享取任务、模式切换销毁缓存的实测成本，不是正式提交或 TPS 基准，也不足以将旧 ASan/core 截止失败唯一归因于编译。每次新建 batch identity；检查私有状态、返回值及 VM 内存/全局变量重置。宿主有其他负载，计时仅存档，不据此算性能倍数。

## 本批实现与不变条件

- `parallel_wasm_worker.hpp/.cpp`：引入线程归属明确的 `isolated_module_cache`；一个 worker 的全部执行模式共用至多九个活动及休眠模块。复用条件为完整代码字节及全部七项 VM 限制，不使用 receiver 或代码哈希单独作键。
- 同步祖先的活动槽不可复用或淘汰；每个活动深度独占，递归到同一代码也须另借模块。先销毁淘汰项，再编译新项，不出现临时第十个模块；失败不发布键，RAII 在异常撤销时归还借用。
- standalone 普通/通知/tree 执行器仍限四槽 LRU；standalone sync 保留原九个固定深度槽。池内模式共用九槽，不是六种模式各保留九槽。纯普通池原先至多四槽，本版至多九槽，因此其常驻上界有所增加，但整个 worker 的既有最高上界仍为九；不将其称为零内存成本。
- `parallel_worker_pool.cpp`：仍在原共享队列取任务，不静态绑定交易、不改变负载均衡策略。惰性构造放在原异常保护内；销毁旧 executor 时仅保留线程本地缓存，不保留状态覆盖层、host 能力、输入或结果。
- 所有入口保持原 VM reset/start、执行前后取消检查、50ms 生产和 500ms 尝试截止；编译仍占原截止。外部缓存为空或线程不符时明确拒绝，不让可变 VM 跨线程。
- 新增五组测试：六模式交替及写后失败复用、跨模式九槽 LRU/编译失败、七项限制键、空值/跨线程拒绝、1/2/4 worker 六模式成功/失败批次。旧四槽、固定深度、活动祖先和原语义测试继续保留。

## 当前验收边界

修正测试夹具后，普通程序 `a56add14c55c5dccbd6534bb55426b262951920506fd13d07d784aef206fcf18` 已成功构建，无 warning/error。预定六轮普通运行全部通过：

| 范围 | 固定三轮结果 | 墙钟秒数 |
|---|---|---|
| 缓存、mixed 私有、同步工作池 | 每轮 **45 / 9,961**，255 个未选用例 skipped | 0.60 / 0.60 / 0.60 |
| 完整普通 worker | 每轮 **300 / 54,659**，全部通过 | 161.87 / 161.87 / 161.67 |

新增五组共 **5,547** 条断言，包含预热后缺失账号仍请求捕获的负对照。46 份冻结源码及程序在运行前后、归档前哈希一致；审计 24 项、访问清单精确匹配和 `git diff --check` 通过。成功程序逐字节比较后归档为 `/run/media/guagua/新加卷/pureeos-cross-mode-cache-artifacts-20261015-sk4WqD/cross-mode-worker`，清单 `normal-success-archive.sha256`。上述墙钟仅记录固定测试，不用于宣称 TPS 或整体加速倍数。

1/2/4 worker 新用例每个池执行 12 批 × 6 模式，共 72 个同代码任务，含 24 个写后 trap；每轮累计编译数均为 1 到 worker 数之间，单 worker 明确为一次。结果/私有写入/原账本和失败后复用均检查；这不是配置 worker 数就证明并发，也不是实际 TPS。原叶帧重叠、递归九槽和旧四槽/固定深度回归仍保留。

所有预算、默认 100 档、断言及停止规则不变。本版 ASan/UBSan/LSan 已构建并运行：缓存专项三轮全通过，二级协调器首轮失败，随后按规则停止；准确结果见下节。消费者重新构建后的原 host、核心、实际 token 各三轮及 22 项兼容全部通过。随后本版 TSan 三类专项各三轮通过，但完整首轮有一项截止失败，详情及准确 ELF 见线程结果节；不是线程检查完整验收，节点也仍未验收。旧 ASan 二级专项失败、核心第三轮失败及所有未执行项目继续保留，不能沿用旧库的通过结果给新代码验收。

完整 CDT、一般 builtins/hash/console、真实节点及 G7 推广仍是原目标，未因本优化而取消。实验开关保持默认关闭，经济/治理/共识规则、原基线和用户配置不变。

## 当前 ASan / UBSan / LSan 结果

单个 `-j1` 构建退出 0，53:50.42、峰值 RSS 4,554,328 KiB，无编译 warning/error。程序 SHA-256 为 `fbf478231fdaf01091c97772efaec4c28801584c00c3577fa380c6b0c7524a7c`，实际 ELF 同时引用 ASan/UBSan；运行启用泄漏检测、首次检测器错误即停。插桩边界仍是私有执行器、状态/重放和指定 action/transaction 上下文及 SoftFloat，不是完整 controller、Chainbase、FC 或 nodeos。

| 范围 | 实际结果 | 墙钟秒数 |
|---|---|---|
| 缓存、mixed 私有、同步工作池 | 三轮各 **45 / 9,961** 全通过；每轮 262 项未选 | 2.71 / 2.90 / 2.90 |
| 二级协调器／原 WASM 对照／私有工作池 | 首轮 **25 / 27 项、20,958 / 20,970 条**通过，退出 201；280 项未选 | 98.11 |
| 二级第二、第三轮及完整三轮 | **未运行**，按首次失败停止规则结束 | — |

二级首轮的 12 条失败只涉及两个既有 controller 用例的生产采用／回退计数：

- `five_families_are_actually_adopted_in_production_validation_and_mixed_call_trees`：mixed 模式中，族 0／2 worker 和族 1–4／4 worker 的五组 `adopted` 为 0 而非 1；每组 8–10 次尝试、0 次私有成功，并记录 worker 和准备截止。
- `secondary_prefix_undo_on_late_inline_authority_and_sync_final_ram_failure_keeps_tail_and_reuse_safe`：七组晚授权失败的 `serial_fallbacks` 为 0 而非 2，均有准备截止；准备整体返回空时外层仍走原串行 callback，但不计入内部该计数。原授权错误、整笔撤销、尾交易、健康复用及其状态/轨迹断言没有失败。该计数口径不能用来宣称原交易未执行，也不能删断言把本轮改判通过。

没有 ASan/UBSan/LSan 错误报告，也没有原账本/trace 对照差异，但这不是完整插桩通过。不能仅以失败条数比旧版少就推导性能提升，宿主条件和随机调度并非受控配对。原 50ms/500ms、默认 100 档、无亲和性均保持；源码 46 项和程序在运行前后复核一致，原始日志和状态完整保留。

失败程序逐字节及 SHA-256 校验后移存到 `/run/media/guagua/新加卷/pureeos-cross-mode-cache-artifacts-20261015-sk4WqD/cross-mode-asan`，另保留相同的 `cross-mode-asan-failure`；清单 `asan-failure-archive.sha256`。原 tmpfs 构建名现已空出，后续线程检查必须重新链接，不能恢复旧程序后当成 TSan。未删除链数据或用户文件。

运行器均已检查 shell 语法：`build-consumers.sh` / `run-consumers.sh` 的实测结果如下；`build-tsan.sh` / `run-tsan.sh` 保留匹配对象缓存并核对实际线程运行库，预定冷构造、缓存、二级、私有完整各三轮，完整首轮失败后停止。私有排除名单与 CMake 精确匹配，无 suppression。本版 TSan 的实际通过、失败和未运行范围见后节，不以脚本存在或旧版通过记录替代。

## 当前消费者、实际 token 和兼容回归

三个消费者构建退出 0，6:59.10、峰值 RSS 2,851,488 KiB。保留 `parallel_validation_tests.cpp:1475` 引出的 Boost 有符号/无符号比较警告：实际统计为 uint64，旧夹具的非负期望为 int；本轮没有为清理警告改动冻结输入，没有生产代码编译错误。

| 程序 | SHA-256 |
|---|---|
| `parallel_validation_test` | `0995200deece6613c7522baddb60bfb5a647e5c4ebdaffade8f12830f39a8762` |
| `parallel_serial_reference_test` | `42cce24e001f3e8f40b25a720172ac6fa02ad913c365084bcf15354daf4ab4c9` |
| `parallel_compatibility_test` | `42074d11acb44d6712753f77e2c9e306d6df4910d5017e75c57fb5a41536e8b1` |

`run-consumers.sh` 固定序列退出 0：原 host 对照三轮各 **3 / 21,094**，核心三轮各 **63 / 12,417**（90.70 / 89.69 / 91.90 秒），未修改 PureEOS token 三轮各 **3 / 1,471**，兼容 **22 / 22**（83.34 秒）全通过。核心明确排除基准导出用例（1 项未选），没有导出真实进程语料，不能把这些普通程序的通过当作 nodeos 生产/恢复通过，也不抵消上面的 ASan 截止失败。

46 项源码、三个程序及 token 夹具前后哈希一致，记录了运行期间外部 Rust 编译负载，但不据此推导唯一性能原因或 TPS。三个程序均逐字节核验归档为挂载卷本批目录中的 `cross-mode-parallel_*_test`，清单 `consumer-archive.sha256`。此前失败的旧消费者程序、日志和当前 ASan 失败仍保留。随后无并发构建/测试地完成一次独立原 C helper 边界诊断（见主计划 G6e0），再保持源码冻结切换线程检查；该诊断未改动生产源码或普通库。

## 本版私有 TSan 结果（2026-10-15）

单个 `-j1` 构建退出 0，23:48.61、峰值 RSS 3,369,924 KiB，无编译 warning/error。实际 ELF 已核验为 TSan，SHA-256 `aef773a919a1bad2da2369635898bee1ffb85f2d59f4b44ecba3415d3461a782`。默认 100 档、无亲和性、无 suppression、原 50ms/500ms 截止不变。

| 范围 | 实际结果 | 墙钟秒数 |
|---|---|---|
| 独立进程冷构造／注册 | 三轮各 **1 / 420** 全通过 | 0.31 / 0.30 / 0.30 |
| 缓存、mixed 私有、同步工作池 | 三轮各 **45 / 9,961** 全通过 | 3.41 / 3.41 / 3.40 |
| 二级协调器／私有 WASM／重放 | 三轮各 **23 / 2,945** 全通过 | 1.60 / 1.60 / 1.60 |
| 完整私有路径 | 首轮 **204 / 205 项、23,036 / 23,038 条**通过，退出 201；102 项原 controller/oracle 排除 | 51.57 |
| 完整私有第二、第三轮 | **未运行**，首次失败即停 | — |

首轮唯一失败为既有 `three_exclusive_modes_keep_owned_results_and_one_thread_local_cache`：4 worker、第 0 轮、第 2 个 job 报 `deadline`，`attempt_wall_us=500502`，两条断言失败并中止该用例；不是检测器报告的 race。其余选中用例通过，无 ThreadSanitizer 错误报告。该现象与此前同一原强度忙循环的截止敏感性相符，但本次没有测量具体调度/执行成本，不能据此断言唯一根因，也不能用专项通过抵消完整失败。

46 项源码和程序前后身份一致。没有重跑到通过、降低压力、放宽时限或删除断言。检查范围仍不是全量插桩 controller、FC/OpenSSL、Chainbase 或 nodeos；相邻 native/pool 独立 sanitizer 程序本批未重建/运行，不能沿用旧版通过。失败 ELF 逐字节及 SHA-256 复核后归档为本批挂载卷目录的 `cross-mode-tsan` 与 `cross-mode-tsan-failure`，清单 `tsan-failure-archive.sha256`；tmpfs 的 sanitizer 程序名已空出，未删除链数据。当前源码阶段有普通及消费者通过、ASan/TSan 完整截止风险，真实节点和 G7 继续待验收。随后 G6e1 纯哈希接口另开源码快照和构建，不能把本节结果当作新接口已验证。

## 截止后的下一步研究边界

2026-10-16 接续：浮点版本的完整 TSan 第二轮和 ASan 首轮仍有截止失败，准确记录见 [浮点检测结果](PX7_FLOAT_HOST_RESULTS.md)。独立证据批次为 `build/local-tests/parallel-single-task-cache-20261016-LsnSW3/`，先保存前版 68 项源码与 31 项原实现，再**仅新增测试，不改原调度**。新测试覆盖 1/2/4 worker、六种执行模式、48 次连续单任务的成功/写后 trap/预先取消/缺失查询，再交替运行 12 次单/多任务批次；每轮 owner 改变原始值并重新捕获快照和 ticket，核验返回值、私有写与原账本隔离。真实模块构造数要求连续单任务仅一次，后续多任务阶段不超过 worker 数；不是时间或 TPS 断言。

负对照构建 1:24.72、峰值 RSS 2,169,060 KiB，程序 `749ef8c0bffa372f139a4e90cf7e7f7f55ec6c468312e9b25d0015431d902312`。首次启动命令因工作目录错误在源码哈希预检退出、没有运行测试，记录于 `probe-preflight-note.txt`；正确目录运行退出 201，**3,116 / 3,210 条**。94 条失败全部为模块编译计数（实际 2 / 3 / 4 而非 1），状态隔离/返回值/错误原因/取消/缺失请求及单多任务交替检查均通过。两线程从第 1 轮就编译第二份，四线程随后累计到四份；准确负对照程序逐字节归档为 `/run/media/guagua/新加卷/pureeos-single-task-cache-artifacts-20261016-gXjtYZ/single-task-cache-negative-control`，源码清单 `probe-sources.sha256`、归档清单 `probe-archive.sha256`。

之后才修改 `parallel_worker_pool.cpp`：线程获得不可变 ordinal，等待谓词仅让 worker 0 领取**单任务批次**；多任务仍使用原共享队列。原 `notify_all`、owner join、取消/截止、输入和结果所有权、九槽缓存上限不变；不绑 OS CPU，不跨线程移动 VM，不复用旧结果或快照。无新增数据库/host 访问，人工复核后访问清单仅更新该文件 SHA；24 项静态审计和完整 **822 源文件 / 262 host** 清单精确比对通过。

修正版构建 36.10 秒、峰值 RSS 1,423,692 KiB，无 warning/error，程序 `8e66d6d4abe2bf5be89e943d4d910004062e6c2088be517ca59fa0d60acbc65e`。相同定向测试固定三轮各 **1 / 3,210** 全通过，没有改断言、负载或任何预算；缓存/混合私有/同步池/旧调用树专项随后固定三轮各 **51 / 14,008** 全通过。完整普通固定三轮随后各 **332 / 120,055** 全通过，209.32 / 208.92 / 207.12 秒；`fixed-run.status=0`，68 项源码、31 项原实现和程序的整批前后核验一致。这些测试秒数不换算 TPS，也不据其相对前版差值做性能归因。

成功程序逐字节核验归档为本批挂载卷 `single-task-normal-worker`，清单 `normal-success-archive.sha256`，原负对照仍保留。消费者重建退出 0，17.65 秒、峰值 RSS 210,436 KiB，无 warning/error。原串行 oracle 三轮各 **3 / 21,094**，核心三轮各 **63 / 12,417**（仅基准导出不选），实际 token 专项三轮各 **3 / 1,471**，兼容 **22 / 22**（80.50 秒）全通过；`consumer-run.status=0`，源码、原实现、程序与 token 整批前后核验一致。

三个消费者分别为核心 `11b0096e347523410f6554610337221d8804b43f919a8741748ff61117030fff`、原串行 `8f2858eda0e0d4b7ed9a7893b3c10bfbf3c393a65ebeaa8ada2d84162a15795c`、兼容 `8f56db78d829c75dc43efff749512b05af13184eb34c91fb819f9bf00ab25d1d`；逐字节归档为本批挂载卷 `single-task-parallel_*_test`，清单 `consumer-archive.sha256`。

本版 ASan/UBSan 构建退出 0，3:38.41、峰值 RSS 2,148,340 KiB，无 warning/error；实际运行库已核验，程序 SHA-256 `737679a1a0a651f4584496b03466c0492a8d445130752207182d923f0c54a855`。启用泄漏检测、首次检测器错误即停，原 50ms/500ms、默认 100 档、无亲和性不变。按预先固定顺序实际执行：

| 当前单任务缓存版内存检查 | 实测结果 |
|---|---|
| 单任务六模式/新鲜快照 | 三轮各 **1 / 3,210** 全通过 |
| 缓存、私有混合、同步池 | 三轮各 **46 / 13,171** 全通过 |
| 混合原链、owner、正式采用 | 三轮各 **29 / 6,155** 全通过 |
| 二级私有、轨迹、原链 | 首轮 **14 / 15、20,322 / 20,336**，退出 201 |
| 二级第二/三轮、完整三轮 | 首次失败后未运行 |

二级首轮 101.91 秒、峰值 RSS 888,484 KiB。14 条失败全部在旧 `secondary_prefix_undo_on_late_inline_authority_and_sync_final_ram_failure_keeps_tail_and_reuse_safe`：10 条回退计数、1 条正式采用计数及3条随后健康交易采用计数。前 11 条均带 `preparation_deadlines=1`，其中部分同时有 worker deadline；后三条健康复用检查未采集完整阶段计数，不能替它们推定具体超时位置。原账本、撤销和非计时 trace 检查没有报告差异，也没有 ASan/UBSan/LSan 错误，但整批不是通过。`asan-run.status=201`，68 项源码、31 项原实现及程序前后核验一致。失败程序逐字节比较后存入本批挂载卷 `single-task-asan` 与 `single-task-asan-original`，清单 `asan-archive.sha256`；不是丢弃失败后重跑到绿。

同源码私有 TSan 接续：启动前审查发现尚未运行的 `build-tsan.sh` 含错误引号和重复尾部，先保留 `build-tsan-before-review.sh`，仅修正构建脚本，重新检查语法再运行。生产和测试 68 项冻结输入未改。ASan 与 TSan 对象、数值库各自保存/恢复，实际配置和运行库分别核验，单个 `-j1` 构建已开始；预定冷构造、单任务、缓存、二级、完整私有各三轮，首次失败即停。117 项未全量插桩的 controller/oracle 继续明确排除，不是全链线程检查。

本版 TSan 构建随后退出 0，1:43.02、峰值 RSS 1,908,624 KiB，无 warning/error，程序 `82f0c7e82ab58eda2f8ef34bff08365fe9e842ef8f755a44ff759d53dcee8c40`，实际运行库仅 TSan。冷构造 **1 / 420**、单任务 **1 / 3,210**、缓存/私有混合/同步 **46 / 13,171**、二级/协调器 **23 / 2,945**，均各三轮通过。默认 100 档完整私有首轮 **220 / 222、55,265 / 55,268**，两项旧用例各自中止，117 项 controller/oracle 排除；退出 201，60.20 秒、峰值 RSS 108,936 KiB，第二/三轮未运行。没有 TSan 竞争报告，68 项源码、31 项原实现与程序前后核验一致。

- `persistent_one_two_four_threads_run_multiple_bounded_batches` 的结果存在性断言失败，原断言未输出 worker failure/阶段，不能直接认定是超时。
- `three_exclusive_modes_keep_owned_results_and_one_thread_local_cache` 在 threads=4 / round=2 / job=5 明确为 `deadline attempt_wall_us=500533`，两条断言失败。

两份准确 TSan 程序由 `archive-tsan.sh` 在确认进程结束后逐字节比较保存，清单 `tsan-archive.sha256`。下一源码批次先补失败诊断，再建立实际同步目标账号/代码两步发现的可复核轮数基线；不降低旧压力、不删除采用或结果存在性断言。

真实节点、三笔交易混合截止和其他未完成范围继续保留。这个反例和修复仅证明连续单任务的冗余编译减少，不证明历史全部截止根因或 TPS 倍数。下面“下一步”是取得该负对照前的研究历史，不代表单任务修正尚未实现。

接续只读复查：`controller::prepare_isolated_attempts` 在 tree→sync/mixed 模式升级时**已经**将所有根 receiver 加入账号捕获集，不能把重复增加根账号预取当新收益。真正同步目标的顺序是 `state.read_account(receiver)`、原 flags 校验、程序查找；未知目标可能分别触发账号与程序发现，但普通 `is_account` 也会产生账号请求。若后续研究合并捕获，必须明确区分实际调用目标和普通账号观察、保留原拒绝次序及所有目录/字节/时间上限，先取得发现轮数证据；不能见到任意账号查询便无界预取其代码。当前没有实施此改动。

本轮五个 mixed 采用失败组合实际都只有一笔顶层交易；`controller::prepare_isolated_attempts` 每轮 join 后重新提交它，`parallel_worker_pool::loop` 仍让任意空闲线程取得共享队列的下一项。线程本地缓存只能保住同线程的模块，不能消除该单任务跨线程迁移的冷编译成本。此前冻结探针已证明多线程模式切换的编译数会受取任务分布影响，但本轮尚未测量每次 controller 发现的具体归属，因此不能把它认定为唯一根因。

下一步可先做固定输入计数诊断，再评估**仅单任务批次保持同一 worker**：该批次本来没有多交易并发收益，多任务批次仍保留共享队列；不跨线程共享模块，不改变 ticket/base、输入快照、原顺序或截止。必须补 1/2/4 worker 的连续单任务、单/多任务交替、取消、失败复用及真实编译数检查。实际目标账户和程序的合并捕获仍按原计划独立评估。不得直接复用旧轮结果到新快照、放宽生产预算或删掉采用率断言。

## 构建与证据保留

首编在新增测试处因遗漏 `parallel_mixed_tree_journal.hpp` 声明头而退出 2，28:27.19、峰值 RSS 5,814,828 KiB；没有编译器 warning。保留 `build-normal.*` 及首版源码，补上 include 后增量重建，不修改生产代码、预算或断言。

补 include 后增量构建退出 0，14:04.57、峰值 RSS 3,374,724 KiB，无 warning/error。但首次普通专项失败：45 个选中用例中 **41 通过、4 中止，4,600 / 4,604**，0.60 秒，退出 201；后续专项及全部完整轮次未执行。四项均为新跨模式夹具沿用了旧仅主键快照，缺少 sync/mixed 根入口必需的账号观察，触发 `primary_state_missing`；不是超时或账本差异。旧四槽、原冷构造、同步工作池及 mixed 私有测试全通过，空缓存/线程归属新测试也通过，但不构成整批验收。

失败程序 `ba47c176c4dc15f765bedf04a1e327cb8544f7b240601e3b0009860742261d52` 已逐字节核对归档为 `/run/media/guagua/新加卷/pureeos-cross-mode-cache-artifacts-20261015-sk4WqD/missing-account-fixture-worker`。仅新增独立的跨模式测试夹具，在 owner 上创建并捕获账号，旧主键夹具不变；补充缓存已预热后去掉账号观察仍产生准确捕获请求、恢复快照再健康复用的负对照。生产代码、限额和原断言不改。修正后以 `normal-account-*` 独立日志执行相同预定三轮专项及三轮完整，不覆盖 `normal-cache-1.*` 或 `normal-run.status=201`。

账号修正版成功构建，但其首轮专项仍有 **41 / 45 通过、4 中止，4,430 / 4,434**，退出 201；后续轮次未跑。原因是将原全表捕获改为稀疏捕获时只传入表 metadata 和账号，未传入 WAT 实际调用的 `find(primary=1)` 查询，故连第一个普通模式也正确请求缺失主键。程序 `5d5f951da187f2982d1ce51634e878a244c3d10d836c32423e1c917c99be8aa0` 同样逐字节核对归档为 `missing-primary-query-fixture-worker`。随后仅补夹具的精确查询及账号数/行数/实际读取的前置断言；`normal-query-*` 独立重跑同一固定序列。源码中的稀疏捕获语义和缺失状态回退不改，不能把夹具缺项当成允许 worker 直接读数据库的理由。

修改前、首版及各次夹具修正分别保存 46 份源码与 SHA-256；当前冻结清单为 `query-fix-sources.sha256`。初始审计 24 项通过，访问清单 805 份源码／262 个 host 精确匹配，注册接口未增加。各次均单个 `-j1` 构建，期间冻结源码及测试输入。

为给 tmpfs 链接腾出空间，将已逐字节及 SHA-256 核验的上一批失败 ASan 程序移存至 `/run/media/guagua/新加卷/pureeos-secondary-boundaries-artifacts-20261015-1eePRE/before-cross-mode-asan`，哈希 `4f9def85a8e1ded980e5cd5388fb0df3fdf0e2c3f1817b29cf8d3df07613c7bd`；原归档 `secondary-boundary-asan-failure` 仍保留。未删除链数据、原基线或用户文件。需要回看旧失败可直接使用挂载卷归档；不要将它恢复到新目标名后当作新代码的插桩结果。
