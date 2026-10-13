# PX7 G5c3c2：同步工作池与正式采用（开发中）

## 当前状态（2026-10-14）

从 `ff31fd5` 继续，用户要求持续推进、边测边改。前一版同步私有帧及主键 journal 已保存独立检查点；普通三轮通过，完整 ASan/UBSan/LSan 三轮仅在四物理核亲和性条件下通过，默认调度的一次 500ms deadline 失败和原始日志保留，见 [c1 记录](PX7_G5C3C1_RESULTS.md)。本阶段的插桩检查同时覆盖 c1 同步私有帧和新工作池路径，条件与失败记录见下文。

前置检查点：`f74ebec` 的独立工作池和 `d00fa86` 的当前链只读证明。当前源码已接入 controller、真实同步上下文重放及整根 CPU/撤销防护；正式采用 13 组通过，并已复现/修复同步阶段计数遗漏。完整普通三轮每轮 180 项 / 14,887 条，同步专项三轮每轮 50 项 / 4,947 条；真正的 ASan/UBSan/LSan 三轮每轮 187 项 / 16,509 条；限定范围的私有 TSan 默认调度三轮每轮 136 项 / 9,636 条（51 项混合 controller 用例明确排除），全部通过。相邻状态/浮点与资源池两类插桩各三轮也通过。资源池曾因缓存切换沿用错误运行库，已保留失败证据、重新链接补测，并加入自动运行类型防护。准确产物均归档，证据目录 `build/local-tests/parallel-sync-adoption-20261014-O4kTI7/`。继续当前核心/token、原链兼容及真实节点矩阵；默认开关关闭，不是整个计划完成。

以下独立工作池的历史证据位于 `build/local-tests/parallel-sync-pool-20261013-YpfU1t/`：普通三轮和无亲和性条件下的完整 ASan/UBSan/LSan 三轮通过，相邻状态/浮点与资源池检查也各三轮通过。TSan 默认调度首轮的旧调用树重叠测试触发一次 500ms deadline，新增同步测试通过，没有数据竞争报告；同程序在四物理核亲和性下全套三轮通过。默认条件的时序风险仍保留，不以受控通过覆盖它，也不把这些旧检查点结果延伸为最新正式入口和真实节点进程矩阵已通过。

### 独立工作池检查点的代码（`f74ebec`，历史状态）

- `parallel_worker_pool.hpp/.cpp`：新增拥有根输入、完整代码目录和协议观察的同步任务及独立结果；四类模式必须且只能选一类。executor 在所属 worker 内构造，模式切换清除旧 executor；同步活跃帧仍各自独占 VM，不使用休眠缓存。任何失败清空全部结果，缺失捕获只返回拥有的请求，不暴露父写入前缀。
- 同步任务计时仅累加根和每个子帧的独占 CPU，并检查非负及总预算；根开始/结束区间包含子编译，明确只能作诊断。真实并行重叠仍只比较没有后代的叶帧执行区间。
- `parallel_ordered_batch.hpp`：显式支持独立同步结果的完整域指纹和全调用树依赖，沿用原有序消费、精确主键冲突、结构变更保守回退、身份与取消规则。协调器不提供授权、资源结算或正式写账能力。
- `parallel_sync_call_tests.cpp`：最终新增七项工作池测试，覆盖 1/2/4 worker 与三轮四模式切换、输入所有权、失败/缺失/取消后健康复用、业务负返回保留私有写入、输入预算及模式混用拒绝、同表不同行可用/同记录冲突、删除重建影响其他读取、外来票据/指纹/基底和回调重入拒绝、真实叶帧执行重叠。协调器测试回调只计数，不把它称为正式采用。

源码清单更新匹配，审计 24 项、进程工具 17 项通过。当前没有改动 `controller`、`apply_context`、`host_context` 的同步正式执行入口；生产路径仍不创建同步任务，默认实验开关继续关闭。

### 首轮普通实测

`build-normal-first.log` 编译成功，有一处测试 `case 9` 的 `for ...; break;` 缩进告警；分支含义无误，但仍将在下一版用明确大括号消除。`sync-first.log` 27 项 / 2,290 条全部通过（5.57s），其中新增工作池六项 / 896 条，原同步私有与真实原链 oracle 21 项 / 1,394 条。没有亲和性设置或并跑重负载，仍保留原 500ms 预算；真实池内叶帧 2/4 线程重叠检查通过。

首版普通程序、三份工作池/协调器源和测试/CMake 已归档 `first-normal-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `db4d991dc5570b295a12204120d6574a3a44f07445939af68ef60262ef4ee5cd`，精确文件哈希见 `first-artifacts.sha256`。随后才修改告警和扩展测试。新增的定时 generation 取消不声称取消时已经执行到某个确定指令：调度延迟也可能发生在准备阶段；它验证整次失败不发布结果、健康同池任务不受影响、后续任务仍可复用，以及原始账本未变。

补强后的 `sync-three-rounds.log`：三轮各 28 项 / 2,319 条通过，分别 5.67s、5.65s、5.61s，总计 16.96s；其中工作池现有七项 / 925 条。`worker-three-rounds.log`：完整普通三轮各 158 项 / 12,259 条通过，22.43s、22.40s、22.41s，总计 67.27s。没有设置亲和性或并跑重负载，原预算及旧断言均未放宽。

准确普通程序 SHA-256 `04b5c73b76910f2c135f685d140d4334ca60afcd3fd1f003f016272a8e4cb752`；三份实现与完整同步测试文件哈希见 `normal-artifacts.sha256`。归档 `normal-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `96d7121467c2e105cb82ac06b7f8d4b38eb37eb27db8ff916051ddd966b85530`。之后才启动内存检查构建；这仍不是正式同步采用、TSan 或新节点进程验收。

### 内存检查三轮通过（2026-10-14）

`build-asan.log` 三个目标均构建成功。日志保留一处旧状态测试的有/无符号比较告警：`parallel_state_view_tests.cpp:502`，源自已有 `2d03650f` 的测试断言，与本阶段同步源码无关；未改 vendor Boost 或关闭告警。新增同步/工作池代码未出现新增编译告警。

直接运行插桩程序的 `sync-asan-first.log`：28 项 / 2,319 条通过。随后 `asan-three-rounds.log` 在默认调度、不设亲和性、不并跑构建或其他重负载测试下全部通过：

- WASM：三轮各 165 项 / 13,881 条，73.12s、73.18s、73.60s。
- 状态/浮点：三轮各 63 项 / 90,619 条，2.35s、2.33s、2.32s。
- 同步资源池：三轮各 15 项 / 1,072 条，0.28s、0.29s、0.29s。

总计 227.80s，进程退出 0，启用 `detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0`，未使用 suppressions，无 ASan/UBSan/LSan 报告。此为本版本观察，不代表 c1 那次默认调度 deadline 的时序风险已根治，也不是 TPS 提升或整节点全部插桩证明。

三个准确程序依次为 `7347cafe3b4c734cad9c1beae80145f6da63c1be2f09606a08127dc6c4e35703`、`700169c63c9c4ec989682a50c79751e2e348406adedfcd0975829c4c3a4adca8`、`4c6717876b640ca4750cafbec50dcdc6547f36690591cf0bddbe4504b2537f44`。相关实现和测试哈希见 `asan-artifacts.sha256`；归档 `asan-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `3cf0b58f25bfceca623633de8af447b1468c8bc25c2c3f9078e5ba9db2a1fe2b`。

归档后才切换 `PUREEOS_PARALLEL_SANITIZERS=thread`。为保留 ASan 对象并降低 tmpfs 占用，原 WASM 对象链接移到本证据目录的 `wasm-asan-object-link`，仍指向 b1 的 `asan-object-cache`；两个相邻 ASan 对象目录（约 80 MiB、36 MiB）可恢复地移入本目录的 `native-asan-object-cache` / `pool-asan-object-cache`。三个原构建路径现在分别链接到本目录新建的 `wasm-tsan-object-cache` / `native-tsan-object-cache` / `pool-tsan-object-cache`，没有删除对象或测试证据、没有触碰用户链。TSan 仍只检查受插桩的私有路径，沿用明确排除混合原 controller oracle 的测试入口，不能通过 suppression 掩盖数据竞争。

## 线程检查默认调度失败与诊断（2026-10-14）

`build-tsan.log` 三目标单个 `-j1` 构建成功，未出现新增同步代码告警。完整重编译还记录了 SoftFloat `s_mulAddF64.c` 的 `sig128C` 可能未初始化告警，以及前述旧状态断言的 signedness 告警；没有修改第三方浮点库或关闭告警。源码复查中 `sig128C` 只在 `expDiff > 0` 分支使用且在前面的同条件分支赋值；此观察不是穷尽编译器/浮点行为的证明。

`tsan-three-rounds.log`：默认调度、不设亲和性、无并跑重负载。相邻状态/浮点 63 项 / 90,619 条和资源池 15 项 / 1,072 条均三轮通过；WASM 首轮运行 134 项，133 项通过、1 项中止，9,452 / 9,453 条通过。失败是旧 `tree_workers_overlap_real_wasm_intervals_without_sharing_writes` 的 `threads=4 job=1 failure=deadline attempt_wall_us=500412`；until-fail 因此未运行该目标第二、三轮。新增同步私有 16 项 / 376 条、工作池 7 项 / 925 条全部通过，日志没有 TSan 报告。此轮总体退出 8，不能记为全套通过。

`tsan-deadline-diagnostic.log`：同一二进制、同一预算、默认调度单独三次运行原失败用例，各 66 条通过；首次命令误用了不被测试读取的 `PUREEOS_PARALLEL_TEST_DIAGNOSTICS` 环境名，因此只有通过结果，没有计时诊断。随后以正确的 `PUREEOS_PARALLEL_TEST_METRICS=1` 额外运行一次，`tsan-deadline-metrics.log` 66 条通过，1/2/4 worker 批次墙钟分别 1,563,725 / 807,767 / 433,672μs、编译累计 2,949 / 8,062 / 19,373μs、执行累计 1,557,038 / 1,562,291 / 1,615,953μs。累计阶段时间不是单任务时间或 TPS，不能据此断定 SMT 是唯一原因。

继续用同一程序执行 `taskset -c 0-3` 的 `tsan-physical-three-rounds.log`。只调整这次测试进程亲和性，不修改系统、生产线程策略、原 500ms 上限、忙循环次数或通过断言，也不使用 suppression。

受控完整三轮已全部通过：WASM 各 134 项 / 9,469 条（31 项混合 controller 用例显式排除），45.32s、45.16s、45.25s；状态/浮点各 63 项 / 90,619 条；资源池各 15 项 / 1,072 条。总计 143.92s，退出 0、无 TSan 报告。这不是整个原链/OC 的线程检查证明，更不是全计划完成。

三个准确程序 SHA-256 依次为 `66b2637a3746f34be175b157aa64c8756907c56331b5f9e8f56ab07126228314`、`448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`、`e7e85ce5745dd3e96fbce9dfbf27cd435b5a6322506af0fea362147a55b423a3`。`tsan-artifacts.tar.gz`（0600）保存三程序及同步/工作池/协调器相关源码、原失败测试和计时辅助器，逐文件比较通过，SHA-256 `e5c92638fe8a072700568ce81ee706c78225221426068d25b64a7ea317fdb359`。之后才继续下一阶段源码修改。审计 24 项与进程工具 17 项重新通过；原 nodeos SHA-256 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`，main/回退标签的提交仍为 `a4a40a6a1e72c531cd8267758074229ccc0655ce`，reference-contracts 未改动。

## 后续验收

### 正式重放与 owner 调度（从 `d00fa86` 继续，按发生顺序记录）

证据目录 `build/local-tests/parallel-sync-adoption-20261014-O4kTI7/`。以下先保留首次构建前的实现说明，随后按时间记录失败、修复和复测；当前结论以本文件开头及最新实测段落为准，不把中间版本的“待测”误读为最新状态。

- 独立同步输入类型移至同步头文件，原工作池接口类型不变。普通根的真实 `call/get_call_return_value` 才返回 `sync_required` 本地提示，不通过导出位推断调用行为；legacy、通知/inline 子帧和 CF 不因此获得能力。
- controller 保留原 500ms/生产 50ms 绝对截止、64 轮上限和代码许可检查，每次增补目录重新计算完整指纹、建立新 batch；返回的 attempts 持有最终拥有目录，工作线程从自己的 job 副本读取。验证入口和生产 slot 分别消费 sync/tree 结果。
- 整根 adopter 还要求 SYNC_CALL 已激活、一个普通顶层 action、原 WASM 位置、互斥 controller 指针及全代码/协议/状态证明。私有实现类只能在完整预检查及返回缓冲准备后设置交易内栈能力；原 host 只能消费，不能注入能力。
- 原同步函数继续负责 trace/marker、检查顺序、子 context、状态归一化及异常；重放调用该函数，在 VM 点进入真正的 sync_call_context。只读检查显式保留；不改子写入的原空 RAM 回调，删除仍走原祖先游标失效广播。开始重放后的错误只走原 undo，绝不 catch 后重跑父 VM。
- 非显式出块把所有独占 CPU 汇总一次通过普通根防重复入口计费，零执行的缺失/空代码/不支持导出帧不会造成计费序号跳号。call trace 仅加经过证明的子树诊断时间，不再次收费；显式验证沿用已给定的区块 CPU 和原 replay 墙钟，和现有 action/transaction 的诊断处理保持一致。reset/undo 清空新增指针。
- 新增一组本地模式提示测试，以及四组真实链对照：不同子记录/同记录冲突，递归和负业务返回保留写入，三个未执行帧后才进入真实子 VM，以及子写后原 RAM 授权失败不重跑父动作。正例逐项比较所有非计时 call trace 字段、原回执和账本完整性哈希，不依赖省略 call_traces 的 SHiP 格式。临时链通过原 bios 动作激活 SYNC_CALL/GET_SENDER_FIX，不改 Savanna 创世 manifest。

ASan 目标同时加入新修改的 `host_context.cpp`；TSan 继续明确排除混合 controller 的 oracle、owner 和新 adoption suite，不能伪称整节点插桩。源码清单匹配、`git diff --check` 通过，首个 `-j1` 普通构建已启动；新逻辑及新断言尚未有运行结论，失败/恢复、原链兼容和真实节点矩阵仍待补齐。

首构建期间，访问审计 24 项和进程工具 17 项通过，分别保存在本目录 `access-audit-tests-first.log`、`process-harness-tests-first.log`。主控制器、采用器、事务上下文及同步 host 已完成编译，尚未生成可运行的新测试程序；记录了一处 `fc::microseconds` 的显式默认构造告警，待本次构建结束后修正，不在构建过程中更换源码。

首构建最终完成链库链接，但新增测试文件在 `BOOST_REQUIRE_MESSAGE` 的消息表达式处编译失败（退出 2）。原因是未括起三元表达式，Boost 宏的 `<<` 优先级导致尝试打印 `optional<fc::exception>`。失败源码先归档 `first-compile-source.tar.gz`（0600），逐文件比较通过，SHA-256 `7a4d68507423634728361047eda9489f225907b38fdf1ce8b64f8f7a36cddcf8`；此为源码归档，没有声称生成了本版测试程序。随后给消息表达式加括号，并将时间返回值明确写成 `fc::microseconds{}` 消除新增告警；不修改 Boost、不关闭告警、不改断言条件。继续第二次增量构建，补测草稿暂未应用。

第二次构建成功且无新增告警。`adoption-first.log` 定向运行五组：正式同步采用四组 / 457 条全部通过（0/1/2/4 worker，同区块串行/并行验证、完整非计时调用记录、原回执和完整性哈希一致）；模式提示测试在第三条处中止，整体退出 201，459 / 460 条通过。失败原因是新增测试给 `ordered_attempt_batch` 传入空执行指纹，触发原有构造防护，尚未进入工作池；直接 executor 的模式提示和 legacy 拒绝已通过。修正夹具为真实根指纹加原 tree 协议指纹，不放松生产批次检查。

首运行程序 SHA-256 `21d70e516f23a8336668d35ace8beb1c0416f96414d0ccc58a9a700b2d66904c`；改源码前归档 `first-run-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `459264c0340c56ef134d8c3de5c9e54b42e15dfd60701a06f18bb12675a21931`。这轮不是全套通过。日志启用了诊断开关；源码复查发现现有 `parallel_compile/parallel_execute` 阶段计数尚未覆盖新同步帧，故其中的阶段累计不能当作同步完整编译/执行成本，也不用于推算 TPS。同步路径的诊断补齐仍待处理；实际采用、状态和时间预算断言不依赖这些计数。

随后应用补测：调用方无导出/仅读返回、递归和父返回保存、状态归一化、关闭控制台、删除重建，共五组；另加四组故障与超时后三轮健康复用、七类队列边界、未审核子代码/多个同步根回退、原签名授权与重复拒绝。正式采用 suite 现有 13 组，新增九组尚未运行；没有把源码数量当成覆盖通过数量。继续第三次增量构建和复测。

第三次构建通过。`adoption-expanded-first.log` 14 组 / 2,294 条全部通过，其中模式提示 1 组 / 21 条，正式采用 13 组 / 2,273 条。每类子失败（trap、动作专用 host、只读写、超时）在 1/2/4 worker 的同链/同池内分别三次失败后健康复用；失败不写账，后续健康交易确实被采用，并以相同 CPU 回执让串行链重新执行对照。错误签名即使获得成功私有结果仍被原授权拒绝；正确签名、相同交易 ID 的后续重试成功，重复成功交易仍拒绝。

`normal-three-rounds-pre-metrics.log` 的完整工作线程三轮已通过，各 179 项 / 14,741 条，59.23s、59.16s、59.06s；同步专项三轮随后开始，尚未写为通过。默认调度、不设亲和性、不并跑编译或其他重负载，不放宽 50ms/500ms 尝试预算。阶段性能计数缺口仍待下一小步：先用计数/区间不重叠测试复现，再分别记录同步模块构造与每帧独占执行（包括失败），不得把父子包含时间当作两份 CPU 或反向输入资源计费。

同步专项随后三轮各 49 项 / 4,801 条通过，42.47s、42.46s、42.64s；两类三轮总计 305.06s，退出 0。准确程序 SHA-256 `9c5ca2e6f0619262a6e08d08b78452cfa76780dd5367fce9a0d932f76587903f`；`pre-metrics-normal-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `67c29045440e12044e8b4756d3d49d8a98970c831b2651fc55e9779a2a48d52c`。归档后才新增阶段计数负对照用例；实际计数源码尚未修改，待重现原版遗漏。

计数负对照首次构建（`build-metrics-negative.log`）发现测试把 `locate_primary` 的元数据行当作带 value 的完整行，退出 2，尚未运行该测试。失败测试源码保存在 `metrics-negative-compile-source.tar.gz`，逐文件比较通过；改为原有 `read_primary(query, 8)` 有界读取，不新增/放宽数据库接口。随后重新构建测试，计数实现仍保持未修复版。

修正后的负对照构建成功。`metrics-negative.log` 复现 146 条中的 90 条失败，均为本应非零的同步编译/执行次数及累计时间仍为零；其余 56 条通过，包括实际返回、原异常、原状态和区间上界。准确程序 SHA-256 `a813e196f85160fd385eaacb5f2de4ecdb15d99cd809a895434b672a036a8954`；先归档 `metrics-negative-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `76ebaa558a776c4ca3c7b34f654a3b85b4467dfbc6bc77716e74a7167fb87c69`，随后才改计数实现。

计数修复源码：同步模块构造单独记录 `parallel_compile`（含失败）；同步 `execute_step` 用 RAII 记录每帧独占执行尝试，减去直接子调用的整个区间，不把父子包含时间重复累加。原子开关仍是可选的本地诊断，不读回计数作预算/计费/采用判断，也未增加阶段标签或改变共识数据。子区间现在在正常返回和异常展开时都结算，并在父返回缓冲处理之前结束；正常成功计费沿用原有独占时间，失败尝试仍不产生可采用结果。编译度量使用保证复制消除的局部构造，不增加帧 VM 分配或共享活跃 VM。新测试用同一线程验证计数和编译/独占执行区间之和不超过整次墙钟，覆盖缺失账户不进 VM、递归、写入/负业务返回、子执行异常和根/子编译失败，各三次；修复版尚待编译实测。

`build-metrics-fixed.log` 构建成功，`metrics-fixed-first.log` 同一负对照测试修复后 146 / 146 条通过。期间未改该测试的预期计数、区间上界、输入或异常断言；源码审计匹配。继续 `normal-three-rounds.log` 的完整普通和同步专项各三轮，最新程序尚未归档或做插桩，不把上一个程序的通过当作本程序已验收。

最新普通三轮完成：工作线程各 180 项 / 14,887 条（59.54s、59.35s、59.19s），同步专项各 50 项 / 4,947 条（42.65s、42.45s、42.52s），总计 305.73s，退出 0。默认调度、无亲和性、无并跑重负载，保留原尝试预算。准确程序 SHA-256 `4e3033ccd153fd2078eca154d7f592efd1b79ec3f6997f0a7f6df88be8a6459c`；`normal-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `f8253962ebcd05c4ea195b1c3d7ca869a47163518e36704cd035794e0151f81f`。源码审计匹配，审计 24 项、进程工具 17 项再次通过，`git diff --check` 通过。

随后核对原 nodeos 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`，main 和回退标签所指提交仍为 `a4a40a6a1e72c531cd8267758074229ccc0655ce`，reference-contracts 和 eos-vm 子模块均无工作区改动。三个旧 TSan 程序哈希仍与前文独立工作池归档一致。为开始 ASan，将构建目录内三个 TSan 对象链接可恢复地移至本证据目录 `wasm-tsan-object-link` / `native-tsan-object-link` / `pool-tsan-object-link`，再链接回已保留的 ASan 对象目录；没有删除缓存或测试证据。配置改为 `address,undefined`，`build-asan.log` 单个 `-j1` 构建三个明确的 sanitizer 目标，尚无这批新代码的插桩通过结论。

`build-asan.log` 三目标单个 `-j1` 构建完成，没有新增编译告警或错误；新增同步 host 源也实际编译进入插桩目标。`sync-asan-first.log` 直接运行新 `parallel_wasm_sanitizer_test`，50 项 / 4,947 条通过（其余 137 项本次明确未运行），退出 0。启用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0`，未设置亲和性或 suppression，没有并跑构建/重负载；无 sanitizer 报告。随后开始 `asan-three-rounds.log` 的完整 WASM、状态/浮点、资源池各三轮，尚不把正在进行的轮次写为通过。

`asan-three-rounds.log` 三类各三轮完成，退出 0，总计 400.19s。WASM 每轮 187 项 / 16,509 条，130.38s、130.80s、130.31s；状态/浮点每轮 63 项 / 90,619 条，2.30s、2.28s、2.31s。全程默认调度、不设亲和性、不并跑构建或其他重负载，原预算与断言不变；没有 ASan/UBSan/LSan 报告。资源池虽然每轮 15 项 / 1,072 条通过（0.59s、0.58s、0.59s），但后续归档核验发现实际运行的是旧 TSan 文件，**这三轮不计为资源池 ASan 通过**。

归档核验发现两个测试工具问题并保留原记录：一是 `tar --compare` 拒绝跨文件系统的对象目录软链接，四份构建元数据报 `Invalid cross-device link`，不能声称整份归档比较通过；二是资源池程序 SHA 与旧 TSan 一致，`readelf -d` 确认其依赖 `libtsan.so.2`，而 WASM/状态程序确实依赖 `libasan.so.8` 和 `libubsan.so.1`。原因是缓存切回 ASan 后对象和链接配方比仍留在输出位置的 TSan 程序旧，CMake 判定不必重链接。初始归档、清单与比较日志改名保留为 `mixed-runtime-first-*`，旧资源池程序可恢复地移到 `pool-program-before-asan-relink`；只重新构建缺失的资源池可执行程序，不清理整个构建树。后续必须在运行前校验每个程序实际 sanitizer 依赖，不能只看 CMake 配置或目标名。

`build-pool-asan-relink.log` 只重新链接资源池目标，`readelf` 随后确认三个程序全部依赖 ASan/UBSan、不含 TSan。`pool-asan-three-rounds-relinked.log` 真正的资源池 ASan 三轮各 15 项 / 1,072 条通过，每轮 0.29s，总计 0.89s，无报告。WASM 和状态/浮点程序未改变，不重复算其通过次数。初始混合运行归档 SHA-256 `04aa1203e721c38021f060299645ecdcb106924ddbd8749a970cc7994b5aef9a`，比较失败日志及旧 TSan 程序完整保留。

新增 `scripts/parallel_sanitizer_check.py`：不执行被检查程序，只校验 ELF `NEEDED` 中实际 sanitizer 集合并记录文件 SHA；检查前后哈希不同也拒绝。明确只证明动态运行库身份，不冒充整程序插桩覆盖。Linux 的四类 sanitizer CTest 入口都加独立 `FIXTURES_SETUP/FIXTURES_REQUIRED` 检查，失败时依赖用例不启动，不改其原超时或断言。`parallel_sanitizer_check_test.py` 九项通过；`pool-asan-runtime-guard-three-rounds.log` 自动加入 guard，检查器及资源池各三轮通过。对真正 ASan 程序要求 TSan 的负对照被拒；独立临时 CTest 项目故意把旧 TSan 程序送给 ASan guard，`runtime-negative-ctest.log` 退出 8，依赖用例明确 `Not Run`，执行标记不存在，证明不是只打印警告后继续测。源码访问清单仍匹配。

最终 `asan-artifacts.tar.gz`（0600）保存三个正确程序、相关源码和新增检查器；常规文件由 tar 比较，跨文件系统软链接指向的四份明确构建元数据逐个按字节比较，均通过。归档 SHA-256 `5cded5a6aeb42db290d285a8e123ca1b55dc178720d6e0d139bc1222e03d7167`。WASM、状态/浮点、资源池程序 SHA-256 依次为 `01e0358e45cea26e89d70ec20b59e7eb225b88e66e93084bf7343070020b5a4d`、`700169c63c9c4ec989682a50c79751e2e348406adedfcd0975829c4c3a4adca8`、`4c6717876b640ca4750cafbec50dcdc6547f36690591cf0bddbe4504b2537f44`。

归档完成后才切回三个已保留的 TSan 对象链接；ASan 链接和三个实际程序分别可恢复地保留为 `*-asan-object-link` / `*-program-before-tsan-relink`，输出位置缺失确保重新链接，不依赖旧时间戳。`build-tsan.log` 开始单个 `-j1` 三目标构建；尚无当前 TSan 通过结论。新增进程夹具/检查器草稿仍未应用，不在构建期间更换编译源码。

`build-tsan.log` 三目标构建完成，新增同步代码无编译错误；仅保留先前已记录的 SoftFloat `sig128C` 两处可能未初始化告警。构建完成后，自动 runtime fixture 范围明确限定为 Linux/GNU，检查器只认识动态 GNU ELF 运行库名称，不用该命名规则误判 Clang 或非 ELF 工具链；默认生产构建不新增这个依赖。九项检查器测试重新通过。本机实际编译器为 GNU 15.2.0，这个范围修正不改变当前测试入口或任何 C++ 实现。访问审计 24 项、原进程工具 17 项也再次通过。

`tsan-runtime-manifest.json` 在运行前核验三个实际程序都只依赖 TSan：WASM `78e2198ba8b16104141680b86983279604d11f42d8192d89b4f25884facb94e8`、状态/浮点 `448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`、资源池 `e7e85ce5745dd3e96fbce9dfbf27cd435b5a6322506af0fea362147a55b423a3`。随后开始默认调度的 `tsan-three-rounds.log`：三个原有测试家族及其自动 runtime fixtures，不并跑构建/重负载、不设亲和性，启用 `TSAN_OPTIONS=halt_on_error=1`，无 suppression。当前轮次仍在运行，不预先宣称通过。

`tsan-three-rounds.log` 随后默认调度三轮全部完成，退出 0，总计 148.03s。私有 WASM 每轮 136 项 / 9,636 条，45.74s、45.28s、45.35s；每轮明确排除 51 项混合 controller 用例。状态/浮点每轮 63 项 / 90,619 条，2.08s、2.08s、2.05s；资源池每轮 15 项 / 1,072 条，0.58s、0.59s、0.60s。三个 runtime fixtures 也各三次核验正确程序。全程没有 TSan 报告、不设置亲和性、不放宽预算或断言；这次默认通过不抹掉历史 500ms 时序失败，也不宣称该风险已根治。

`tsan-artifacts.tar.gz`（0600）保存上述三个精确程序、相关源码、当前 GNU 范围的检查器及构建元数据。常规文件逐文件比较和跨设备链接元数据字节比较均通过，SHA-256 `0e67e9d2d709e147eb6a4406de88b8d7361bfcf7c3641d280c927b76dd2a2b0d`。保存这个受限同步正式采用检查点后，继续新同步真实进程夹具、最新 core/token、兼容与恢复矩阵，不把当前单元/插桩通过替代进程验收。

正式接入验收清单（当前普通、ASan 和限定范围 TSan 已通过，进程验收另计）：

- 父合约没有同步导出、仅读取最近返回值、递归相同代码和调用 trace 扩容；调用方行为与被调用方导出能力分别验证。
- 缺失/空代码调用穿插真实调用、非法业务状态归一化、只读属性继承；完整非计时 call trace 对照。
- 子调用删除/重建父持有的行：旧句柄拒绝与新查询成功；子 store/resize/payer 的原 RAM 空回调语义。
- 子写后 trap、只读写、错误 host 与超时：原交易完整撤销，然后同一节点/工作池执行健康交易，检查状态和能力不残留。
- 队列拒绝、回调异常、错误 metadata 身份、重入、截止、abort 和取消后不能再次消费旧结果。
- 未审核的子代码、原授权拒绝、多根/通知/inline 等当前未支持组合必须按原路径处理，不能因本次接入默默放开。
- 上述已支持路径的普通三轮通过，具体数量和版本见最新归档；未支持的混合通知/inline/多根同步仍是回退边界，不是全面并行已实现。
- 仍待：当前核心/token、原链兼容及真实节点生产/验证/恢复矩阵。

真实进程验收仍使用独立实验节点程序，不覆盖原 nodeos。HTTP 入队响应与 SHiP 历史对照各有边界：前者可能只是 speculative 执行，后者不包含完整 call_traces；不得把任一接口单独称为正式同步重放的完整 trace 验证，链内采用计数和逐字段对照仍是必要证据。

### 当前链证明子步骤（历史：从 `f74ebec` 到 `d00fa86`，普通三轮通过）

新证据目录 `build/local-tests/parallel-sync-owner-20261014-TPVmH2/`。`prepare_isolated_sync_program` 复用现有普通代码捕获并读取实际 `code_object.sync_call_supported`；空代码仅代表现存普通账号，系统/特权/未知 VM/不存在账号不伪造成空代码。`current_sync_context` 捕获实际深度、数据预算、sender 协议位和控制台设置。`validate_isolated_sync_current_state` 是 owner 只读辅助函数：只接一个普通根，检查全目录去重和预算，对包含未执行 receiver 的全部代码、真实导出位及全部 limits 做比较，再以当前根输入和协议验证完整 journal、当前查询/账户依赖。checkpoint 异常向上传播，函数不写库，也不是授权或正式采用能力。

新增七组 `parallel_sync_owner_tests`，覆盖真实位与代码所有权、目录重排/重复/未执行代码及 limits 篡改、当前代码/VM/特权变化、协议/返回上限变化、精确行变化与无关行更新、交易输入及单普通根边界、取消异常不被吞掉。测试夹具直接改元数据只在临时 tester 的独立 undo 内进行，没有拓宽生产可写数据库 API。普通同步专项将包含这些用例；TSan 显式排除新的混合 controller suite，ASan 则保留，不能将未插桩的原 controller 误称为线程检查覆盖。源码清单更新匹配，`build-normal-first.log` 正在单个 `-j1` 构建；新增七组尚未跑过，不列为通过。

首构建已完成链库，但测试编译退出 2：新夹具两处把 `shared_blob` 当作可下标的数组。修正为该类型的原有 `assign(data, size)` 接口，没有修改底层类型或数据库 API；随后在 `build-normal-second.log` 重编。代码复查另外发现只读辅助函数把可为空的默认 checkpoint 原样传入了要求可调用对象的 `matches_base_reads`；待可运行测试确认后用非空包装修复，不能吞掉实际取消异常。此时新增用例仍未运行。

`build-normal-second.log` 构建成功；`owner-first.log` 七组全部在 fixture 的默认 checkpoint 校验处复现 `std::bad_function_call`，退出 201，84 / 91 条通过。该失败版程序 SHA-256 `219b1a9025fdc30faa52d4708b820a92428ac028b1dc624334de65cfa990aa26`；源码和程序先归档 `first-failure-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `203b2c661c9c2ad02a767319df01cda487f088f40fd6ab3bb674941c673ecaa5`。随后才将 `matches_base_reads(live, checkpoint)` 改为传入已有的非空 `check` 包装；外部实际取消回调仍直接调用并传播异常，没有 catch 或放宽证明。正在 `build-normal-fixed.log` 重建，尚未把修复写为通过。

修复后构建成功，`owner-fixed-first.log` 新增七组 / 188 条全部通过（含默认省略 checkpoint，以及第 1/3/20 次检查抛异常的传播）。`sync-three-rounds.log` 完整同步专项三轮各 35 项 / 2,507 条通过，6.80s、6.85s、6.93s，总计 20.61s；没有亲和性或并跑重负载。源码审计匹配，当前继续完整普通工作池三轮。这批 owner 辅助代码尚未重新做 ASan，TSan 的混合 controller 排除边界也未改变；前一检查点的插桩通过不能自动延伸到这批新代码。

`worker-three-rounds.log` 完整普通三轮各 165 项 / 12,447 条通过，23.55s、23.50s、23.71s，总计 70.79s，默认调度且不并跑重负载。准确普通程序 SHA-256 `13572261d2312fce05efa4829ce3df8615539d4096c860db6eab4754d1c649d2`；`normal-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `a03bc80f4e6a179a866a81a86cc5411a29531cf7cfcb5b45e3f3e7a09206e5a9`。审计 24 项重新通过。保存此只读校验子步骤后继续正式重放入口；新 owner 代码的 ASan 将与正式接入一起运行，不能把这个普通测试检查点称为整个 c2 验收。

先编译/修复、运行同步专项和完整普通三轮；然后使用明确插桩程序重新测试同步帧及工作池、完整 ASan 与私有 TSan，不用普通 CTest 冒充 sanitizer。之后按 [总计划 c2 拆分](PX7_GENERAL_EXECUTION_PLAN.md) 接入真实同步上下文、整根预检查、原 host 重放和逐帧 CPU/undo 清理。正式接入后仍需当前 core/token、原链兼容及旧/新节点的生产、验证、恢复矩阵；不能将本文件的私有结果替代这些验证。

### 首次接入前的风险分析（历史设计记录，最新实现及实测见上文）

`controller::prepare_isolated_attempts` 目前固定构造 tree job；`validation_attempts`、验证入口和生产 adoption slot 都只消费 tree result。增加工作池结果字段本身不会让真实节点采用同步结果，三个入口必须一同改造并留有各自的差分证据。同步指纹绑定完整目录，而当前 tree 的协调指纹只绑定根与协议；每次增补同步目录后必须重新计算 requests、重新建 batch identity，并让采用侧持有最终那一份拥有目录，不能引用已经移动给 worker 的 job 或用不同的目录重算。

模式选择不能把 `code_object.sync_call_supported`（被调用方导出能力）误当“根动作调用了同步接口”的证明。拟先增加私有树执行遇到同步能力时的精确、有界模式请求，仅允许一个普通根的尝试升级为独立同步模式；不能对每一种不支持接口无限重试，也不改变原整批 500ms/生产 50ms 上限。根带通知/inline 或其他未覆盖行为时，独立同步入口仍整次回退。此请求只是本地调度提示，不授权代码、跳过原检查或产生共识拒绝。

`transaction_context` 的采用指针目前是 private，仅 controller/apply_context 友元可以设置。后续同步重放能力也应保持封闭、栈内 RAII 和明确 reset/undo 清理，不为方便测试公开任意注入回调的入口。首写后的内部不匹配必须使原交易撤销；协调器不能 catch 后重新执行已经写过的父动作。原 `execute_sync_call` 的异常、负状态、返回长度及 call trace 创建顺序仍由原函数负责。

继续核对 CPU 身份发现：缺失账户/空代码/不支持导出的同步调用同样消耗 call ordinal，却不进入子 VM，因此不能用“上一个计费 ordinal + 1”简单判断下一个真正执行的子帧。单普通根的首版可评估在整根预检查后，沿用原根动作防重复入口，一次收取“根及所有子帧独占耗时之和”；各 call trace 再按经过验证的子树时间补诊断 elapsed，而不是再次收费。这样计费与显示分离，避免缺失帧跳号和父子重复收费；仍需验证原资源失败、timer pause/resume、显式验证与非显式出块。若改用逐帧收费，则必须显式消费零执行帧并保持原 trace/异常检查次序，不能只在子 VM hook 内递增一个连续计数器。此处仅记录设计取舍，尚未接入任何计费源码。
