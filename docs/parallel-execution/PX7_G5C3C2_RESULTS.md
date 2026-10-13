# PX7 G5c3c2：同步工作池与正式采用（开发中）

## 当前状态（2026-10-14）

从 `ff31fd5` 继续，用户要求持续推进、边测边改。前一版同步私有帧及主键 journal 已保存独立检查点；普通三轮通过，完整 ASan/UBSan/LSan 三轮仅在四物理核亲和性条件下通过，默认调度的一次 500ms deadline 失败和原始日志保留，见 [c1 记录](PX7_G5C3C1_RESULTS.md)。本阶段的插桩检查同时覆盖 c1 同步私有帧和新工作池路径，条件与失败记录见下文。

本阶段证据目录：`build/local-tests/parallel-sync-pool-20261013-YpfU1t/`。独立工作池与协调器已通过普通三轮和无亲和性条件下的完整 ASan/UBSan/LSan 三轮；相邻状态/浮点与资源池检查也各三轮通过。TSan 默认调度首轮的旧调用树重叠测试触发一次 500ms deadline，新增同步测试通过，没有数据竞争报告；同程序在四物理核亲和性下全套三轮通过。默认条件的时序风险仍保留，不以受控通过覆盖它。正式同步采用和最新真实节点进程矩阵仍未接入/验收，不是整个计划完成。

### 已写入的代码

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

### 当前链证明子步骤（从 `f74ebec` 继续，普通三轮通过）

新证据目录 `build/local-tests/parallel-sync-owner-20261014-TPVmH2/`。`prepare_isolated_sync_program` 复用现有普通代码捕获并读取实际 `code_object.sync_call_supported`；空代码仅代表现存普通账号，系统/特权/未知 VM/不存在账号不伪造成空代码。`current_sync_context` 捕获实际深度、数据预算、sender 协议位和控制台设置。`validate_isolated_sync_current_state` 是 owner 只读辅助函数：只接一个普通根，检查全目录去重和预算，对包含未执行 receiver 的全部代码、真实导出位及全部 limits 做比较，再以当前根输入和协议验证完整 journal、当前查询/账户依赖。checkpoint 异常向上传播，函数不写库，也不是授权或正式采用能力。

新增七组 `parallel_sync_owner_tests`，覆盖真实位与代码所有权、目录重排/重复/未执行代码及 limits 篡改、当前代码/VM/特权变化、协议/返回上限变化、精确行变化与无关行更新、交易输入及单普通根边界、取消异常不被吞掉。测试夹具直接改元数据只在临时 tester 的独立 undo 内进行，没有拓宽生产可写数据库 API。普通同步专项将包含这些用例；TSan 显式排除新的混合 controller suite，ASan 则保留，不能将未插桩的原 controller 误称为线程检查覆盖。源码清单更新匹配，`build-normal-first.log` 正在单个 `-j1` 构建；新增七组尚未跑过，不列为通过。

首构建已完成链库，但测试编译退出 2：新夹具两处把 `shared_blob` 当作可下标的数组。修正为该类型的原有 `assign(data, size)` 接口，没有修改底层类型或数据库 API；随后在 `build-normal-second.log` 重编。代码复查另外发现只读辅助函数把可为空的默认 checkpoint 原样传入了要求可调用对象的 `matches_base_reads`；待可运行测试确认后用非空包装修复，不能吞掉实际取消异常。此时新增用例仍未运行。

`build-normal-second.log` 构建成功；`owner-first.log` 七组全部在 fixture 的默认 checkpoint 校验处复现 `std::bad_function_call`，退出 201，84 / 91 条通过。该失败版程序 SHA-256 `219b1a9025fdc30faa52d4708b820a92428ac028b1dc624334de65cfa990aa26`；源码和程序先归档 `first-failure-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `203b2c661c9c2ad02a767319df01cda487f088f40fd6ab3bb674941c673ecaa5`。随后才将 `matches_base_reads(live, checkpoint)` 改为传入已有的非空 `check` 包装；外部实际取消回调仍直接调用并传播异常，没有 catch 或放宽证明。正在 `build-normal-fixed.log` 重建，尚未把修复写为通过。

修复后构建成功，`owner-fixed-first.log` 新增七组 / 188 条全部通过（含默认省略 checkpoint，以及第 1/3/20 次检查抛异常的传播）。`sync-three-rounds.log` 完整同步专项三轮各 35 项 / 2,507 条通过，6.80s、6.85s、6.93s，总计 20.61s；没有亲和性或并跑重负载。源码审计匹配，当前继续完整普通工作池三轮。这批 owner 辅助代码尚未重新做 ASan，TSan 的混合 controller 排除边界也未改变；前一检查点的插桩通过不能自动延伸到这批新代码。

`worker-three-rounds.log` 完整普通三轮各 165 项 / 12,447 条通过，23.55s、23.50s、23.71s，总计 70.79s，默认调度且不并跑重负载。准确普通程序 SHA-256 `13572261d2312fce05efa4829ce3df8615539d4096c860db6eab4754d1c649d2`；`normal-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `a03bc80f4e6a179a866a81a86cc5411a29531cf7cfcb5b45e3f3e7a09206e5a9`。审计 24 项重新通过。保存此只读校验子步骤后继续正式重放入口；新 owner 代码的 ASan 将与正式接入一起运行，不能把这个普通测试检查点称为整个 c2 验收。

先编译/修复、运行同步专项和完整普通三轮；然后使用明确插桩程序重新测试同步帧及工作池、完整 ASan 与私有 TSan，不用普通 CTest 冒充 sanitizer。之后按 [总计划 c2 拆分](PX7_GENERAL_EXECUTION_PLAN.md) 接入真实同步上下文、整根预检查、原 host 重放和逐帧 CPU/undo 清理。正式接入后仍需当前 core/token、原链兼容及旧/新节点的生产、验证、恢复矩阵；不能将本文件的私有结果替代这些验证。

### 编译期间继续核对的接入风险（尚未改源码）

`controller::prepare_isolated_attempts` 目前固定构造 tree job；`validation_attempts`、验证入口和生产 adoption slot 都只消费 tree result。增加工作池结果字段本身不会让真实节点采用同步结果，三个入口必须一同改造并留有各自的差分证据。同步指纹绑定完整目录，而当前 tree 的协调指纹只绑定根与协议；每次增补同步目录后必须重新计算 requests、重新建 batch identity，并让采用侧持有最终那一份拥有目录，不能引用已经移动给 worker 的 job 或用不同的目录重算。

模式选择不能把 `code_object.sync_call_supported`（被调用方导出能力）误当“根动作调用了同步接口”的证明。拟先增加私有树执行遇到同步能力时的精确、有界模式请求，仅允许一个普通根的尝试升级为独立同步模式；不能对每一种不支持接口无限重试，也不改变原整批 500ms/生产 50ms 上限。根带通知/inline 或其他未覆盖行为时，独立同步入口仍整次回退。此请求只是本地调度提示，不授权代码、跳过原检查或产生共识拒绝。

`transaction_context` 的采用指针目前是 private，仅 controller/apply_context 友元可以设置。后续同步重放能力也应保持封闭、栈内 RAII 和明确 reset/undo 清理，不为方便测试公开任意注入回调的入口。首写后的内部不匹配必须使原交易撤销；协调器不能 catch 后重新执行已经写过的父动作。原 `execute_sync_call` 的异常、负状态、返回长度及 call trace 创建顺序仍由原函数负责。

继续核对 CPU 身份发现：缺失账户/空代码/不支持导出的同步调用同样消耗 call ordinal，却不进入子 VM，因此不能用“上一个计费 ordinal + 1”简单判断下一个真正执行的子帧。单普通根的首版可评估在整根预检查后，沿用原根动作防重复入口，一次收取“根及所有子帧独占耗时之和”；各 call trace 再按经过验证的子树时间补诊断 elapsed，而不是再次收费。这样计费与显示分离，避免缺失帧跳号和父子重复收费；仍需验证原资源失败、timer pause/resume、显式验证与非显式出块。若改用逐帧收费，则必须显式消费零执行帧并保持原 trace/异常检查次序，不能只在子 VM hook 内递增一个连续计数器。此处仅记录设计取舍，尚未接入任何计费源码。
