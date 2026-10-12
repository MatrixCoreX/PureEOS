# PX7 G5c3c1：嵌套 VM 防护与同步私有帧（开发中）

## 当前状态：按用户要求暂停（2026-10-12）

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
