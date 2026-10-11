# PX7 G5c3a3：普通 inline 正式接入

日期：2026-10-12；分支 `feature/parallel-tx-execution`，起点 `ef0c457`。证据目录 `build/local-tests/parallel-inline-adoption-20261012-deGm6P/`。本文件随实际验证更新，不将未运行的项目记为完成。

本阶段已按以下受限普通 inline 范围验收，继续推进其余通用并行阶段，不代表全面合约并行完成。

## 实现

- 工作池新增拥有输入、代码和协议配置的 tree 任务及独立结果，与旧顶层/通知模式互斥；切换模式销毁旧 executor，每线程只保留一份模块缓存。缺失状态/代码、预算、取消及深层失败不返回成功前缀。
- 协调器绑定树域指纹；controller 的验证、生产、接收者发现和统计接入新类型。仍遵守发现最多 64 轮、整批 500 ms、生产 50 ms 预算及原回退规则，未开放默认开关。
- `parallel_canonical_adoption` 首步检查全部真实 `(receiver, account, action)`、当前代码/元数据/协议配置、整树 journal 和当前读取依赖。按原事件顺序调用原 DB/通知 host，并调用原 `send_inline` 包装，保留原始字节长度检查、FC 解包、发送当时的权限检查及排队。
- `apply_context` 只读暴露组首 ordinal/inline 深度，核验 sender/creator/ancestor；原调度会扩容 trace 容器，新 adopter 不跨发送持有 trace/act 引用。后续防御失败只回退当前及尾部，保留原游标和已排队的 inline；重放异常继续整笔原 undo。
- `transaction_context` 新增 tree 指针，reset 清除能力，CPU 按实际接收者执行步骤、ordinal 和耗时防重复计费。授权、RAM、最终计费、回执与失败撤销仍走原路径。

当前只增加普通 inline；context-free inline、sync-call、一般二级索引和未覆盖的其他 host 仍回退，不是全面合约并行完成。ENABLE_OC=OFF，不宣称 OC 验收或 TPS 提升比例。

## 已完成的普通测试

- 新工作池 4 项 / 757 断言：三种任务混合、1/2/4 线程、失败后复用、缺失请求、互斥/输入预算、取消、整树冲突及真实 WASM 执行区间重叠。重叠项 66 条，确认 1 线程不重叠、2/4 线程重叠；不是只看排队或编译时间。
- 新正式 inline 5 类功能 / 1,622 断言：两根动作、子动作写自身表、兄弟动作删表重建、通知与 inline 交错、原 sender/回执顺序/声明授权、原 `eosio.code` 授权与缺失 permission、RAM 拒绝、孙动作 trap、未审核子代码及原生 `eosio::linkauth` 串行边界。
- 直接原 host 边界检查父动作采用后子动作回退，不重写父状态、不重复排队、inline 新建游标；另检查深层当前代码/特权元数据、过期数据与篡改发送字节在正式事件前拒绝。直接边界用例不冒充完整 controller 执行；完整回执和原计费由另外的生产/验证用例覆盖。
- 完整普通 worker 三轮，每轮 106 项 / 7,915 条。完整核心三轮，每轮 43 入口 / 11,236 条（40 功能 + 3 个未请求的导出入口；容量基准排除）。当前 PureEOS token 三轮，每轮 3 项 / 1,471 条。
- 审计 24 项及清单匹配通过；进程工具 16 项通过，包含 inline RPC 树的缺失/重复/循环、目标、授权声明、payload、ancestor 和非 ordinal 执行顺序检查。
- 状态层 57 项 / 90,530 条、串行参考 2 项 / 5,165 条通过；重新链接的兼容矩阵 21 组全部通过（40.28 秒）。

本批初次新增测试源码编译发现 tester 需要可变交易左值，以及 Boost 消息宏需要给条件表达式加括号；均在测试代码中修复，`build-tests-first.log` 保留。初次 pool 运行仅包含先编译的 3 项 / 691 条；追加重叠用例时与该次编译发生时间重叠，因此不将它算作该用例通过证据。冻结测试源码并重建后，`pool-overlap.log` 的 4 项 / 757 条及上述三轮完整测试才是当前普通 worker 的证据。

## 专用进程夹具

新增 `--inline-fixture`，单笔两根动作及 16 个执行步骤，使用原授权和真实主键生命周期 host；与 token/通知等夹具标志互斥。导出器使用隔离的新目录与确定性的测试密钥，不访问用户钱包/链。

`export-live.log` 1 项 / 16 条通过；`export-recovery.log` 1 项 / 398 条通过，已导出 `inline-recovery-corpus-v1/`：20 个检查点、包括 inline 写入的可逆尾块和 3 个竞争分叉块。导出时原串行与 0/1/2/4 线程验证逐块比较回执与账本。其 manifest SHA-256 为 `0f3b51c5f22a2aa250fd4691591288e7cf370cb4a38508073ab5f03361867887`。

真实进程目录 `/dev/shm/pureeos-g5c3a3-process-YxQNNa/`，以下报告均为 `passed`：

- `parallel-producer-epd8b58w`：普通 inline 五模式（旧节点、新节点关闭及 1/2/4 线程），各 16 笔、32 根动作、每笔 16 接收者执行步骤；启用模式各 2 批 / 112 尝试 / 16 成功 / 4 采用 / 12 回退。RPC 调用树、payload、授权声明、实际执行顺序、最终确认、旧节点强校验重放和 SHiP 一致。
- `parallel-process-3w98_a3g`：上述普通 inline 语料的真实恢复矩阵 55 阶段通过，包括逐块校验、重启、SIGKILL 后恢复、损坏/截断拒绝、快照、混合新旧节点和竞争分叉。
- `parallel-producer-75h03a3c`：带代码通知五模式通过，启用模式各 2 / 128 / 16 / 4 / 12。
- `parallel-producer-s4v9hvsf`：当前 PureEOS token 多动作五模式通过，启用模式各 2 / 128 / 16 / 6 / 10，各 16 笔、33 根动作。
- `parallel-process-uuu5ywbg`：旧带代码通知语料再跑 55 阶段全部通过；manifest `76165ddb81dd0930b2e8bfb76a9e6e62943529962de7f67cd9382b9a022adb9b`，不是早期单动作语料。

这些固定小负载证明正确性，不是容量/TPS 实验。进程运行期间另有单任务 sanitizer 编译，测试用普通 nodeos 和导出器始终固定、未重新链接；不能从这些运行时间推导性能提升比例。

## 环境及失败记录

ASan 工作池已链接，但原生 sanitizer 首次链接报告 `No space left on device`。核验成功进程归档、报告及进程均退出后，只删除归档中生成的 `data/state/shared_memory.bin`，随后原生程序链接成功。没有修改配额、保护阈值或用户链。

首轮 ASan 与另一组原生/CTest 检查重叠：工作池一项生产计数预期失败（实际串行、没有取得并行批次）；注册入口另一次混合工作池用例未得到预期 tree 结果。未出现 sanitizer 越界/泄漏诊断；日志保留为 `asan-worker-1.log`、`asan-ctest.log`。同一二进制单独运行三轮全部通过，每轮 113 入口 / 9,537 条（包括两个未请求的导出入口）；原生三轮各 63 项 / 90,619 条通过，单独 CTest 两个注册入口通过（33.52 秒）。该差异与并发负载下的时间预算退化相符，不是已经通过故障注入证明的唯一原因；没有放宽正式 50 ms 生产或 500 ms 私有预算，也未删除原来的采用计数断言。失败不能算作通过，后续这类严格时序验收必须避免同时运行重负载测试。

最终 Python 复核第一次以包名组合调用时，进程工具的同目录 import 无法解析；`final-harness-audit.log` 保留调用失败。改用各脚本原直接入口，审计 24 项、进程工具 16 项分别通过，源码清单及普通程序哈希再次匹配。

已逐文件比较并校验 SHA-256 的进程归档（权限 600）：

- `g5c3a3-inline-process-evidence.tar.gz`：`593a20f2c235be443f85e01defe718ccee75d5e09ebf3fa0245da494fb1df2b0`，包含两个 inline 进程目录及准确恢复语料。归档后清理 58 个状态文件，释放 256,692,224 字节。
- `g5c3a3-notification-token-process-evidence.tar.gz`：`78bd7d4cef7d0321738edbdda15f4657e2ae30f45fce32926c4118a6f3ecc676`，包含通知/token 两个成功目录。归档后清理 30 个状态文件，释放 132,874,240 字节。
- `g5c3a3-coded-recovery-evidence.tar.gz`：`7b35db322b8ce521f2926bf34bd0f686d2bf0296ed9aa46029b4a899c19a7560`，旧通知恢复成功目录；清理 43 个状态文件，释放 189,775,872 字节，明细 `archived-coded-recovery-state-cleanup.log`。

清理明细在 `archived-inline-state-cleanup.log` 和 `archived-notification-token-state-cleanup.log`，均可从归档恢复；区块、快照、配置、日志没有删除。

另重新校验 G5c1 归档 `g5c1-process-evidence.tar.gz`（`8ec93567cb7537deff4c58f98c10d7dea9308b72ee97ca9bf8100110d19831f6`），仅对早期空间不足失败目录 `parallel-process-5jp1p2_i` 再做逐文件比较，确认无运行节点后清理其中 40 个状态文件，释放 172,077,056 字节。失败证据仍完整存在该归档中，原错误日志、空报告、区块和快照留在原目录；`prior-failed-state-cleanup.log` 记录准确目标。G5c1 报告所述“失败目录完整保留”是当时状态，本次只改变这些可恢复的生成态文件。

## 已固定的测试产物

- `g5c3a3-asan-artifacts.tar.gz`：`b6d42648cc3ce2a5bce3ee0856bf4c6104d82fab1c480c0e491c5b75d3887c0f`；worker `d6b55ded2e2e3cb564d716e5d3590a2ffb8027dcaa6dd22ef347587a4168b31c`，原生 `700169c63c9c4ec989682a50c79751e2e348406adedfcd0975829c4c3a4adca8`，包含匹配 CMake 配置和注册入口日志。
- `g5c3a3-runtime-3334ee76.tar.gz`：`3e1b1026ef0a1cc2df3e1a97b692a69594ff00b2c319c65e40ecaf8e27697980`，六个普通程序；配置以本批源码和上述配置记录为准，不把后续切换 TSan 的同名程序当作 ASan 产物。两个归档均权限 600、逐文件比较通过。
- `g5c3a3-tsan-artifacts.tar.gz`：`2418545b01eb665cd3a6ce41943978897aad6b8740f8327d41b453cf0e10fc90`；worker `d6517b196e6ccfe24f04749f74eb62c85199c75e60ee47bdb087ac7d3cf45c5d`，原生 `448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`，含匹配配置和注册日志，同样权限 600、逐文件比较通过。

## 线程检查与保留边界

正式 nodeos-parallel `3334ee76e4086b7265417375f34de12ce461c30ca7853280e33cb2bfe4aab6b9`；核心测试/导出器 `a0d0d347fd7364c8be5ae33f89c2c71eee33352e8a82b7ecdc389ddd3323fd98`；普通 worker `03cf181e147565148fea1f4ef6463a4299b7181465aefec79e983bf841553391`。原 nodeos 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。

私有工作池 TSan 三轮各 98 项 / 6,958 条、原生 TSan 三轮各 63 项 / 90,619 条全部通过；两个 CTest 注册入口另跑一次通过。新增 sanitizer 源码覆盖 adopter、apply/transaction context，目的是检查 trace 扩容等边界；原 controller/完整权限库仍未整体插桩，TSan 排除 15 个混合 controller / 导出入口（113 个注册入口中执行 98 个），不包装成全节点 TSan。

未运行 OC、完整业务长矩阵、长驻或饱和容量实验；未重跑早期单动作 55 阶段语料（本轮实际为普通 inline 和旧带代码通知各 55 阶段）。未实现 context-free inline、sync-call 私有执行及一般二级索引。原 nodeos 哈希未变；`main` 和基线标签均仍为 `a4a40a6a1e72c531cd8267758074229ccc0655ce`，外部 reference-contracts 仍为 `c454378bf2efc4e15fa2c55169ba4749795f3534` 且无修改；不触碰个人配置或用户链。
