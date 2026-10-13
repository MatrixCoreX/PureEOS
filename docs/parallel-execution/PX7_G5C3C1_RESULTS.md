# PX7 G5c3c1：嵌套 VM 防护与同步私有帧（开发中）

## 当前状态：已恢复推进（2026-10-13）

用户已要求持续推进本实验分支，边测边改。最新状态：同步私有帧已覆盖主键读写，普通专项及完整工作池三轮通过；ASan/UBSan/LSan 同步专项通过，完整三轮在四物理核亲和性条件下通过；默认调度下的一次 500ms deadline 失败保留，不能称无条件通过。工作池任务与正式采用尚未接入同步结果，TSan 和新节点回归尚待完成，不能据此称为全面并行已完成。恢复时核对 `43767cd`、依赖 `16f9c4c` 与干净的已跟踪工作树，根盘约 14 GiB、tmpfs 用户配额余量 633,359,360 字节。以下逐时点记录和暂停记录保留为历史，以本节最新实测为准，不代表当前仍暂停。

### 恢复后的原链验证

`build-resumed-compatibility-20261013.log` 构建成功，无该次日志中的 warning/error。未与构建或其他重负载测试并跑，`resumed-sync-timeout-reuse-three-rounds.log` 三轮分别 1.59s、1.73s、1.89s，每轮 30 条断言通过，总计 5.35s；同一 tester 每轮分别触发三次超时并随后健康复用缓存父/子模块。此前准确旧版的两次 30s 挂住负对照仍保留。`resumed-full-compatibility.log` 21/21 组通过，总计 85.60s。原 30s CTest 上限、交易预算和功能断言未放宽。

当前兼容程序 SHA-256 `85f95bcdf796394e3dc3ff216141b795253f3ecb60d75b8b7ee96b79b95289af`，精确测试源及 VM 头文件哈希见 `resumed-runtime-artifacts.sha256`。该程序归档 `resumed-compatibility-artifact.tar.gz`（0600）逐文件比较通过，SHA-256 `c4fda7c42d88249da4bc38fc4acab0b9c812b06227e417cc389637edd51c27d2`。原 nodeos 哈希再次核对未变。这里只关闭嵌套防护的原链接口回归缺口；完整并行工作池、当前新节点与真实进程还需在后续源码版本重建复测。

### 私有同步第一版：已写入，尚待编译/运行

新证据目录 `build/local-tests/parallel-sync-frames-20261013-1Hd871/`。审阅暂停草稿后新增 `parallel_sync_call.hpp` 和 `parallel_sync_call_tests.cpp`，修改 `parallel_wasm_worker.hpp/.cpp` 及测试注册。每个活跃帧分别构造模块/上下文/allocator，不使用会淘汰父模块的休眠缓存；只提供一个普通根动作和纯计算/主键只读同步子调用。所有写入、通知、inline、打印和 eosio_exit 仍需整笔回退，无正式 adopter hook。

新 host 仅在专用栈内 capability 存在时可执行；旧入口的四种同步接口保留“可链接但调用即回退”，增加正确类型、start 调用及错误签名测试。元数据证明、账户不存在与 flags 检查次序、归一化状态/返回长度、返回数据隔离、每层独立游标、只读继承、sender 协议位、平面 call ordinals/console markers、整笔预算和不重复计 CPU 均单独建模；未支持操作不创造新共识拒绝规则。上限为 64 次调用、8 层子调用、9 个活跃模块、256 KiB 代码/输入预算和整笔 500ms，不因递归刷新。原链 oracle 显式比较同步记录，不依赖缺少这些字段的 SHiP 摘要。

源码清单更新匹配，审计工具 24 项通过；`build-private-first.log` 正在以单个 `-j1` 构建。新增源码和测试尚无运行成功证据，不把“已写入”记作“同步并行已验收”。

### 只读第一版普通回归通过，继续主键写入

`build-private-first.log` 已完成，无 warning/error；`private-first.log` 首轮 13 项 / 578 条通过，其中私有 11 项 / 226 条，原链显式 trace 对照 2 项 / 352 条。`private-first-three-rounds.log` 随后三轮全部通过，总计 7.16s。`worker-first-three-rounds.log` 完整工作池三轮各 143 项 / 10,518 条通过；未设亲和性，也未并跑构建或其他重负载测试。

准确普通程序 SHA-256 `1bc56169fdb0829dea26a76da7b544203ef3e471afbdeb4bdb5b6f266e04e92c`，源码哈希见 `first-artifacts.sha256`。这不是 sanitizer 或正式同步采用验收。下一步在同一隔离入口扩展主键写入，按帧记录查询/写入/即时子调用的混合事件，重放时每个活跃帧保有独立游标、全帧共享一个写日志；父写区间包含子写，绝不能重复重放。明确验证业务负返回保留子写、只读继承拒绝写、父旧游标在子删除/重建后失效、真实原链 RAM 行为和失败整次丢弃。新增叶帧执行区间用于验证 2/4 线程的真实重叠，不把包含子模块编译的父区间当作纯 WASM 重叠证据。

只读普通三轮总计 59.21s。准确程序与测试/执行器源码归档 `readonly-first-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `459c7bb0cf2d72d32994d9345bbece40f5752a2e400662a6ccc976801e85464b`；归档完成后才继续改源码。

### 主键写入与预检查：已写入，构建中

新增 `parallel_sync_call.cpp`：独立指纹和冻结状态事件重放；根/子帧分别保存查询、写入、即时子调用序列，整个尝试最多 4,096 个事件。每个帧重新创建游标，全帧共享写日志，严格检查全局写入顺序、调用 ordinal、sender、只读继承、代码/协议/输入指纹、写对象代际和读取依赖覆盖；owner 的取消异常必须逃出预检查，不能被错误转为可继续采用。仅检查冻结副本结构，不声称已经完成正式授权、RAM 或当前链状态验证。

新测试覆盖三层即时写入、业务负返回保留写入但隐藏已设置的返回值、只读孙调用拒绝、父旧游标 get/next/previous 在子删除/重建后失效、父重新 find 成功、付款人变更/记录变长/表销毁重建与原链空 RAM 回调一致、事件与写入预算、写入后 trap/超时及健康重试。篡改检查包括调用关系、顺序、重复写入、指纹、只读、计时、console marker 和禁止通知。叶帧 1/2/4 线程重叠用例只比较没有子调用的执行区间，未把编译区间纳入其中。`build-primary-first.log` 正在构建，尚无本版本运行结论；源码清单匹配，审计 24 项、工具 17 项通过。正式 adopter/controller/生产开关均未接入同步能力，所有打印/inline/通知/eosio_exit 仍整次回退。

主键首轮保留两类夹具失败：`build-primary-first.log` 因测试直接调用 controller 私有 `mutable_db()` 编译失败，执行器与预检查已编译；仅把初始数据构造改为现有 oracle 使用的测试内数据库引用，不扩大生产 API。`build-primary-second.log` 编译成功，`primary-first.log` 21 项中 15 通过、6 中止，710/716 条通过；6 项均抛 `primary_state_missing`。复核确认 `capture_queries(keys)` 只捕获表元数据，夹具误把它当成整表复制，未提供实际点查询。改为显式捕获 7/9/11 主键与用到的范围，并给游标/只读预期失败增加 `not primary_state_missing` 检查，避免捕获缺失被算成正确失效。未放宽未知数据回退规则、执行预算或旧成功断言。叶帧重叠 42 条及创建/变长/付款人/表退役原链对照 40 条首轮通过，但此时仍不能宣称主键版本整体通过；`build-primary-third.log` 正在重建。

修正夹具后 `build-primary-third.log` 成功，无新增编译告警。`primary-second.log` 全部 21 项 / 1,394 条通过：私有 16 项 / 376 条，原链 oracle 5 项 / 1,018 条。`primary-three-rounds.log` 三轮全部通过，17.63s；`primary-worker-three-rounds.log` 完整普通工作池三轮各 151 项 / 11,334 条通过，分别 25.90s、23.57s、22.58s，总计 72.08s。无亲和性或并跑重负载条件；包括真实叶帧 2/4 线程执行重叠、原链业务错误写入/祖先游标失效/只读孙调用/原 RAM 行为的重复对照。

普通程序 SHA-256 `c20dffd0d8ce28fa079c739d50e3479da79ab247e57127267e7dc357925d7ae3`，准确源哈希见 `primary-normal-artifacts.sha256`；归档 `primary-normal-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `12b51f0fd398a1b7851321c6d36479027f4d2b2e926c03ea9593af86a37cb79d`。随后开始 `build-primary-asan.log` 的单个 `-j1` 内存检查构建。普通三轮不是 sanitizer 或正式采用验收，后续两类 sanitizer、原链核心和最新真实节点仍待完成。

`build-primary-asan.log` 已完成，未发现 warning/error。准确插桩程序 SHA-256 `59243557ae495ba6d9fac1f0a6fa0e9ee46434423774649b29b13049afd0b27d`。直接运行该程序（不是指向普通程序的 sync CTest）得到 `primary-sync-asan-first.log`：21 项 / 1,394 条全部通过，包括 16 项私有测试和 5 项真实原链 oracle，进程退出 0，无 ASan/UBSan/LSan 报告。启用 `detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0`，没有 suppressions、亲和性或并行重负载。继续 `primary-asan-three-rounds.log` 完整插桩回归；此时尚无三轮或 TSan 结论。

完整插桩首次请求三轮：第一轮 158 项 / 12,956 条全部通过（72.53s）；第二轮 157/158 项通过，12,823/12,825 条通过（71.77s），旧 `parallel_action_tree_pool_tests/three_exclusive_modes_keep_owned_results_and_one_thread_local_cache` 在 threads=4、round=1、job=2 达到 `deadline attempt_wall_us=500464`，故按 until-fail 停止，第三轮未执行。没有 sanitizer 报告，同步专项两轮均通过，但不得称完整三轮成功。

保留原日志并使用同一程序、不改预算/循环/断言，单独开启诊断复测该失败用例，`primary-asan-deadline-diagnostic.log` 666 条通过。九批的编译累计 6.4–21.1ms，WASM 执行累计约 1.54–1.63s（各 worker 耗时之和，不是单个任务或整批墙钟）；4 线程三批墙钟约 515.8–561.1ms。单次诊断成功只能说明失败对时序敏感，不能证明唯一原因或消除失败。下一项为同程序 `taskset -c 0-3` 的完整三轮受控检查，限制仅作用于测试子进程，不改变产品设置或原 500ms 尝试上限。

受控结果 `primary-asan-physical-three-rounds.log`：三轮各 158 项 / 12,956 条全部通过，73.20s、72.95s、73.29s，总计 219.47s；进程退出 0，无 sanitizer 报告。同一准确程序及源码归档 `primary-asan-artifacts.tar.gz`（0600），逐文件比较日志为空，SHA-256 `883ffb4df02c2f241bf3065e95ebcf885ea915ceb5a15e406dd60c13517d16fd`。下一步在保留本版检查点后接入独立同步工作池模式；TSan 将继续覆盖未改动的同步帧以及新增工作池路径，不将其写成已通过。正式同步采用、一般接口/二级索引、完整 OC 和本次新节点进程矩阵仍未完成。

## 暂停交接记录（2026-10-12）

用户要求“先把这个工作暂时暂停，阶段性收尾”。已停止本轮唯一构建进程组，构建会话退出 130；这是主动中断，不算编译失败，也不算构建或回归通过。不再启动测试或推进新功能。

- 主仓库分支 `feature/parallel-tx-execution`，生产改动检查点 `3b09ecc`；EOS VM 依赖为本地提交 `16f9c4c3ea7a172716d0bf68c60806e499f5988a`，依赖工作树干净。未推送、合并或改动 main/基线。
- 已完成的本阶段验证：原生 VM 普通防护三轮，每轮 8 项 / 2,632 条；ASan/UBSan/LSan、TSan 各三轮，每轮 6 项 / 2,279 条。负对照、修复版程序和日志均保留。
- 未完成：修复后原链兼容程序重建在 `webassembly/privileged.cpp.o` 暂停，尚未链接新程序。原链“超时后复用”此前两次负对照均超时，修复后的实际原链测试尚未运行；完整兼容、并行工作池、新节点与进程矩阵也尚未对本次依赖修复重新验收。不能把原生独立通过当作它们通过。
- 私有同步执行器仍未落源码。已将会话内四份补丁与测试草稿存到本轮证据目录的 `sync-private-draft-{header,host,executor,integration}.patch` 和 `sync-private-draft-tests.cpp.txt`，仅供恢复工作时审阅，未经编译/测试，不能直接视作可用实现。现有正式路径未开放同步并行，默认实验开关保持关闭。
- 原始 nodeos SHA-256 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`；main 与 `baseline/pre-parallel-20261011` 都是 `a4a40a6a1e72c531cd8267758074229ccc0655ce`。用户链、钱包和配置目录未修改。

恢复顺序：先检查磁盘/配额与工作树，继续单个 `-j1` 兼容目标构建；完成后独立跑超时复用三轮和完整兼容，归档准确版本。再审阅/实施私有同步草稿，补齐编译问题、只读/递归/错误码/计时/原链 trace 对照，之后重新完成普通及两类 sanitizer、核心与真实节点回归。不得在构建期间改源码，也不直接把未验收草稿接入正式 adopter。完整同步写能力、正式采用、二级索引和完整 OC 仍是后续工作。

从 `012809a` 继续。当前仍未开放私有同步调用或正式采用，默认并行开关不变。本文件记录独立证据，不以资源池、普通 inline 或 CF 的通过替代同步执行验收。

## 第一小步：先复现原运行时的清理风险

计划使用实际 EOS VM 头文件、模块和分配器，而不是另写一份被测算法。初始子模块为干净的 `3b3cde5951537700c6993d50c88885526431609e`。若需要修改依赖，将保留基线并单独提交，主仓库显式记录 gitlink；不遗留无法复现的依赖脏修改。

1. 信号范围：内外 VM 使用不同代码及内存范围；检查正常返回、C++ 异常、VM longjmp 异常和信号跳转后，父范围、跳转目标、进入前信号掩码是否恢复。补多线程 TLS 隔离。真实越界访问仅在限时子进程测试，禁用其 core dump，并防止测试框架的信号处理器掩盖崩溃。
2. 超时清理：可控 watchdog 按原方式保存多层回调，分别触发父层、子层、两层及重复回调，不依赖宿主调度来决定是否超时。记录实际代码页保护、清理后的标记和同模块正常调用。负对照发现代码页未恢复时，先记录失败再由夹具恢复页保护，避免把主测试程序挂住；这种补救不能算生产修复。后续再验证隔离进程中的真实原链超时与复用。
3. 原生分配失败：仅专用测试程序对 mmap/mprotect/munmap 使用链接包装，按线程和准确映射尺寸定位原生 wasm_allocator 的首次保护设置失败。检查完整映射回收、构造失败后的重试、正常析构和多线程隔离；此前 c0 的 OC 分配测试不覆盖此分配器。

先保存未修复版的预期失败与二进制，再逐项修复/重建，随后普通、ASan/UBSan/LSan、TSan 重复测试和原同步/只读回归。未复现的源码推断继续标为风险，不称为已确认故障。测试专用故障注入不进入节点产品。

## 测试代码已写入，负对照待编译

证据目录：`build/local-tests/parallel-native-guards-20261012-gwZzWz/`。新增 `native_vm_guard_tests.cpp` 与专用 CMake 入口，覆盖四类信号退出组合、线程隔离、父/子/两层超时、重复与异线程回调、真实解释器模块恢复与复用，以及构造失败映射回收。普通版本另有 JIT 可执行页恢复检查及限时子进程中的真实内存故障；sanitizer 不运行这两个入口，不把生成的 JIT 代码当作已插桩。解析在原生并发 fixture 中仍由 owner 顺序完成，本阶段不擅自宣称任意并发编译安全。

原 `sync_call_cpp_tests.cpp` 新增同一 tester 中三次“同步超时→新块→正常同步返回”的用例，另设 30 秒 CTest 上限，防止旧版恢复问题挂死整个测试流程。此用例尚未编译执行；之前 21 组兼容通过不包含它。VM 依赖源码目前未改动。初次 CMake 补丁因分块先后顺序错误未应用，调整补丁顺序后落盘，未改测试意图或阈值。

## 原版负对照已复现问题

首次构建成功，无新增告警。`negative-control.log`：独立防护 8 项全部失败，2,627 条中 2,196 通过、431 失败；失败分别落在父/初始信号范围恢复、超时后的父模块保护及祖先状态、构造失败映射回收，以及三个真实父内存故障子进程。原链超时后复用入口达到 30 秒上限（30.06s），整个 CTest 请求退出 8。没有改动生产依赖来制造失败。

程序和测试源码哈希见 `negative-artifacts.sha256`：普通防护程序 `16e6ff3e0798269c2483ec8242faa384cf1ba4f750149a050aa0f95cd70bf6bc`，含新原链用例的兼容程序 `e27c3bdb8b1e894c5add7e8b2a6004aa3290c5c735c01180dd95476b04bcc923`。二者、测试源、CMake 及三个原 VM 头文件归档 `negative-before-dumpable-hardening.tar.gz`（0600）逐文件比较通过，SHA-256 `9d13f0d11a5f7c8b8859e768b38ee370bbadebef6fe6e0c4a88d4cb28bec7ad1`。

首次真实故障子进程虽然设置 RLIMIT_CORE=0，wait 状态仍为 139（带 core 标志）。检查发现本机 core_pattern 使用 Apport 管道；Linux 对管道收集器不按 RLIMIT_CORE 限制转储，不能把此前设置等同为已彻底禁止。任务目录未发现 core 文件，检查时也无该收集器进程；这不证明系统收集器从未生成外部报告。现仅在测试子进程补 `PR_SET_DUMPABLE=0` 并读回确认，父进程检查无 core 标志及自身 dumpable 未变，再重新编译原 VM 负对照。系统 core_pattern/Apport 和用户进程均不改动。依据：[Linux core 手册](https://www.man7.org/linux/man-pages/man5/core.5.html)、[PR_SET_DUMPABLE 手册](https://man7.org/linux/man-pages/man2/pr_set_dumpable.2const.html)。此时仍未修改 VM 依赖，旧失败记录保留。

补强隔离后的 `negative-control-no-dump.log` 再次确认同一 431 条功能断言失败：8 项、2,632 条中 2,201 通过；新增 5 条隔离检查全部通过，三个子进程 wait 状态改为 11（SIGSEGV、不带 core 标志），父进程 dumpable 不变。原链入口再次在 30.04s 超时。固定普通防护程序为 `a027e9e9fcaa10d122ff91a6a60f73663b61627f1a6682c6a1ff98221ed6e4e5`，其他哈希见 `negative-no-dump-artifacts.sha256`。完整负对照归档 `negative-artifacts.tar.gz`（0600）逐文件比较通过，SHA-256 `0480b15a9ae3c9ff668239c5ff11b226ec57c34b37008f8dbe13b253b437b21a`。

## 三处运行时修复已写入，验证中

EOS VM 在原基线之上新建本地分支 `pureeos/native-nested-vm-guards-20261012`；尚未提交或推送，测试通过后记录依赖提交及主仓库 gitlink。改动仅在三个头文件：

- `signals.hpp`：屏蔽受管信号后发布/恢复范围和跳转目标；所有恢复快照在 sigsetjmp 前保存且之后不修改。正常、C++ 异常、长跳转出口均恢复父范围及进入前的完整掩码，不用会被跳过析构的非平凡 RAII。
- `backend.hpp` / `signals.hpp`：每活跃层拥有独立原子过期标志，线程级原子计数保留尚未退出的过期祖先；重复回调只计一次。移除并同步回调后，每层仅恢复自身触发关闭的代码页，再减少计数。使用无锁原子保证信号路径适用性，原 deadline 和异常转换规则不放宽。
- `allocator.hpp`：初次 mprotect 失败时，在构造函数抛错前释放整个已成功映射的区间；保持原异常类型/消息，不向产品添加故障注入。

`build-fixed-normal.log` 正在编译修复后的独立普通目标。新增信号屏蔽切换有运行开销，后续需在原预算下回归；不能预先宣称零开销或 TPS 增益。完整节点/原链超时复用与两类 sanitizer 尚待验证；mprotect/munmap 本身失败的全部组合、完整 OC、一般同步私有帧仍不是本次修复已完成的内容。

普通修复版构建成功，无新增告警。`fixed-normal-three-rounds.log` 原 CTest 入口三轮全部通过，每轮 8 项 / 2,632 条，总计 0.26s；未修改负对照的功能断言，431 条旧失败全部消失，包括解释器/JIT 清理、三种真实父内存故障恢复和构造失败释放。普通程序 SHA-256 `549b3aa77777582104ebb2d66852495edfa668a2637993d59cccbff5c280c742`。清单更新只涉及上述三个头文件：检查匹配、审计 24 项、进程工具 17 项通过（`audit-fixed-*`、`harness-fixed-tests.log`）。现在构建专用 TSan 目标；原链超时复用、两类插桩和新节点仍不能据此记为已验证。

TSan 构建无新增告警，`fixed-tsan-three-rounds.log` 三轮全部通过，每轮 6 项 / 2,279 条，总计 1.32s；不设 CPU 亲和性，使用 `TSAN_OPTIONS=halt_on_error=1`，没有 suppressions。普通版与 TSan 的测试源哈希同为 `9acf2fd2f0c88de6a0bb3d2c158a46a8f6ebfdda3b7e5fe9f201287086b8a1de`；两项真实 OS 故障/JIT 检查仅按预定编译宏从 sanitizer 排除，不宣称生成的 JIT 或完整节点已插桩。TSan 程序为 `81be4951e01a0384ac908065d347c3c61a068f1ca9ef0e8ccb49af7d8f63b003`，逐文件哈希见 `fixed-normal-tsan-artifacts.sha256`。两份程序、测试源和三个修复头文件归档 `fixed-normal-tsan-artifacts.tar.gz`（0600）比较通过，SHA-256 `711ceca7c32e8b7c1dd19d2341ae0f25258e722a2bfef3ad6d11cb357af716a5`，随后才切换 address,undefined 构建。

ASan/UBSan/LSan 构建成功，`fixed-asan-three-rounds.log` 三轮各 6 项 / 2,279 条通过，总计 0.68s；不设亲和性，使用 `detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0`，未屏蔽报告。程序 SHA-256 `d29fb9ef104734fee70d677c403b620bff734c6f20229edd00a911c1884a6031`；归档 `fixed-asan-artifact.tar.gz`（0600）逐文件比较通过，SHA-256 `ad0a300ba63526d01c242e53291165b60564b11c54c0edacd88f6e58ab0ca029`。

构建结束后将本轮约 115 MiB 的 `native_vm_guard_sanitizer_test.dir` 生成对象缓存移至同证据目录的 `native-asan-object-cache/`，原构建路径改为符号链接；未删除对象。tmpfs 用户配额余量从 113,134,592 增至 232,954,880 字节。此前 b1 的测试节点及证据目录仍保留，不清理用户链。

依赖提交已保存为 `16f9c4c3ea7a172716d0bf68c60806e499f5988a`，依赖工作树干净。初次提交因子模块未配置作者失败；随后只对该条 git 命令使用主仓库现有作者设置完成提交，没有修改全局或子模块的持久配置。主仓库将记录 gitlink，并保留 [独立源码补丁与发布说明](dependencies/README.md)；未推送远端。当前只是原生独立目标完成三类三轮检查，正在进入原链核心重建；此前两次超时的原链复用用例、兼容/并行工作池/最新节点仍待本修复版本验证。

## 后续边界

继续构建期间确认 tmpfs 余量降至约 183 MiB；本轮已经结束、无进程打开的 `/dev/shm/pureeos-inline-runtime-E10rJq` 再次与 b1 归档逐文件比较通过后，整体移至 `build/local-tests/parallel-context-free-20261012-wbEaQl/inline-runtime-process-files/`，原路径保留符号链接。约 412 MiB 测试文件均保留，没有删除；用户链、钱包和配置未改动。迁移后 tmpfs 配额余量 623,123,456 字节，根盘约 1.7 GiB，后续大归档仍需检查空间。

EOS VM bundle 已通过 `git bundle verify`，SHA-256 `6df8e9455b989b0941cee27323e2401109a7cc4568732edc673a612fa4bbea53`；文件位于本轮证据目录 `eos-vm-native-guards.bundle`，依赖准确的原始 `3b3cde5` 基线。不以本地 bundle 等同于已发布依赖远端。

独立补丁另以临时 `GIT_INDEX_FILE` 在准确旧基线执行正向 `--cached --check --unidiff-zero` 并应用到临时索引；生成树与修复提交的树均为 `13e7bd25796ecdfc34351eec4ba41a9633e65769`，内容复现一致。真实子模块索引和工作树未更改。

迁移后直接对旧路径执行 tar 比较时，tar 拒绝穿过跨设备符号链接；改用 strip-components 又对归档顶层空名称给出警告，两者不作成功证据。最终按归档顶层名称映射到新目录执行比较，`process-relocation-transform-compare.log` 为空、命令退出 0，所有内容及归档目录元数据核验通过。没有为让比较通过改写归档或测试数据。

私有同步帧第一版已在会话中准备草稿，**尚未落源码或编译**，等待当前原链构建完成后实施：新增独立 `parallel_sync_call.hpp` / 执行入口与专用测试，不给现有正式 adopter 添加 hook。一个普通根动作，最多 64 次同步调用、8 层子调用、9 个同时存活且各自独占 VM/context/allocator 的帧；整笔仍为 500ms，嵌套不刷新截止。先支持纯计算/主键只读，任何写入、通知、inline、打印或 eosio_exit 整笔回退。同步导出能力来自 owner 捕获位，不从私有解析结果自行授权；父自身 CPU 排除直接子调用整段时间。测试草稿覆盖同代码递归内存/全局变量隔离、立即返回与短缓冲区、错误码/返回数据清空、账户与 flags 检查顺序、只读继承、sender 协议差异、深度/取消/超时/重试、多线程独占及原链 call trace 显式对照。不能把草稿记成同步已实现。

运行时修复不等于同步合约已经并行。之后仍需独占活跃帧、原返回码/返回字节、只读与能力限制、跨帧游标失效、完整 call trace 对照、嵌套 CPU 不重复计费及整笔回退设计，见 [总计划的 G5c3c1](PX7_GENERAL_EXECUTION_PLAN.md)。现有四项休眠缓存不能淘汰仍在执行的父 VM。完整 OC 执行与一般二级索引仍需独立实现和验收。
