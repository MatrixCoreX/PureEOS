# PX7 G7：缩小私有模块编译锁

日期：2026-10-15。分支 `feature/parallel-tx-execution`。证据目录 `build/local-tests/parallel-resolver-lock-20261015-bdYGf6/`。这是接续实施记录，不是 G7 或全面并行已完成。

## 当前状态

普通构建和固定重复回归、当前消费者回归已通过；当前内存插桩完整首轮仍有两项截止相关失败，私有线程检查默认首轮也有一项压力超时，真实节点仍待验证，实验默认仍关闭。当前 50ms/500ms 时限、测试默认 100 档、旧忙循环与断言不变；单独标注的 50 档诊断不替代默认验收。

## 当前消费者回归与存储整理

三个消费者程序已重新链接当前代码，`run-consumers.sh` 固定回归退出 0：原 host 对照三轮各 **3 / 21,094**，核心三轮各 **63 / 12,417**（84.39 / 84.19 / 84.90 秒），未修改的 PureEOS token 专项三轮各 **3 / 1,471**，兼容入口 **22 / 22** 通过（74.27 秒）。核心明确排除基准导出套件，没有请求任何进程夹具导出；几个导出用例的无请求返回不代表跑过真实节点。

- validation：`8d236eed37ae862c69ef64f3d0732427fd1e105d46f2d41dc8ede52939d0747d`。
- serial reference：`fd6d1df94e2abe2e78899ed25d62e20f5f76450ff05ef6857e6e8cc184338801`。
- compatibility：`cb903703ce96bfbae4207092be70847695300ed3557abe18ed072e24db256cd7`。

43 份源码、三个程序和两个原 token 夹具前后哈希一致。旧消费者程序在重新链接前保存为 `before-consumers.tar.gz`，归档比较及哈希校验通过，SHA-256 `d9262a11ce92e81fc69e81fb93d115dffd3909f8b2281947f1382d3fa51c8046`。

测试全部结束后，仅将两份生成的 WASM ASan/TSan 对象缓存迁至 `/run/media/guagua/新加卷/pureeos-resolver-caches-20261015-3x3Z50/`，原路径保留软链接，146 个文件逐个哈希复核一致。根分区可用空间恢复到 1,595,224,064 bytes（约 1.49 GiB）；未调整分区、删除链数据或降低节点空间保护线。后续使用缓存需要该卷保持挂载，恢复说明见本批 `STORAGE_RECOVERY.md`。真实节点尚未重建/运行，空间余量并不等于节点验收通过。

## 当前私有 TSan 结果及补充诊断边界

程序 `bb37080499de765d9225f1f6665f2a45cd76d6a73a966d701de4137f2b56245d` 已构建，运行库核验只有 TSan。新进程并发冷构造三轮各 **1 / 420** 全通过。默认 100 档私有首轮执行 198 项：**197 通过、1 失败并中止该用例，99 项 controller/oracle 明确排除**；**16,785 / 16,787** 条断言通过，49.95 秒，退出 201。原 `three_exclusive_modes_keep_owned_results_and_one_thread_local_cache` 在 threads=4 / round=1 / job=2 出现 `deadline attempt_wall_us=501847`，两条断言失败；没有 TSan 竞争报告。按方案不运行默认第二、第三轮，源码/程序后置校验一致。

按照用户此前允许适当降低本机测试负载的要求，额外预先固定 `run-tsan-profile50.sh` 三轮，使用已存在的测试专用 `PUREEOS_PARALLEL_TREE_STRESS_PERCENT=50`（忙循环 25,000 次，默认仍为 50,000）。生产代码、50ms/500ms 时限、断言及排除名单不变，无亲和性或 suppression。三轮随后全部通过，每轮 **198 / 16,918**，99 项 controller/oracle 排除，用时 **40.25 / 40.55 / 40.55 秒**，没有竞争报告，源码/程序前后核验一致。其作用只是补齐当前插桩环境下的路径检查，不能写成默认 100 档稳定性通过、性能验收完成或全链 TSan 通过。

## 当前 ASan / UBSan / LSan 结果

实际程序 SHA-256 为 `79fd220fbfee2298c0a6e15c8e03b1c5b48020afb816db6750d36ee737f53cbf`，构建退出 0，运行库检查确认为 ASan/UBSan，开启泄漏检查，无 suppression 或继承环境覆盖。

新增并发冷构造在三个独立进程各 **1 / 420** 全通过；完整首轮 **295 / 297 项、44,005 / 44,008 条断言** 通过，用时 **230.36 秒**，退出 **201**。按预先固定的方案停止，完整第二、第三轮未运行。两项失败均为 4 worker：

- `production_and_validation_replay_shared_cursors_both_queues_and_conflicting_tail`：adopted `0 != 1`、serial_fallbacks `0 != 2`；attempts=30、successes=0、worker deadlines=1、preparation deadlines=1。
- `recursive_mixed_calls_keep_return_scope_and_charge_only_exclusive_cpu`：adopted `0 != 1`；attempts=11、successes=0、worker deadlines=1、preparation deadlines=1。

没有报告 ASan/UBSan/LSan 错误，也没有报告原账本/trace 对照差异；但这不构成完整回归通过，更不证明没有线程竞争。43 份源码及程序后置校验一致。`run-asan.sh`、`asan-runtime.json`、`asan-cold-*`、`asan-full-1.*` 和 `asan-run-*` 保留所有原始结果。缩小锁并未解决全部截止问题；下步先验证私有 TSan，再继续分析有界发现重跑及准备成本，不修改旧断言或用重跑覆盖该失败。

失败版程序已保存在 `wasm-asan-failure.gz`（完整性和解压流哈希均通过），仅移除已确认无占用的 493,078,016 字节未压缩副本；恢复信息见本批 `STORAGE_RECOVERY.md`。随后保留并切换匹配的对象缓存，配置 `thread` 并启动单个 `-j1` TSan 构建，源码继续冻结。当前尚无新 TSan 运行结果；预先固定的 `run-tsan.sh` 将先跑新进程冷构造三轮，再跑与 CMake 完全相同排除名单的私有套件三轮，首次失败即停止，不设 suppression。未全量插桩的原 controller/oracle 仍明确排除。

## 普通构建实测

- `parallel_wasm_worker_test` SHA-256：`5fa30789c020030b77c716c6136eb40e18586162198655f604ce02efecd8c4bc`。单任务构建退出 0，24 项静态访问审计通过，接口清单匹配。
- 新增并发冷构造测试在三个独立进程各 **1 项 / 420 条断言** 全通过，覆盖 4/2/1 worker；因此每个进程都包含四线程首次进入私有注册器。
- 原三个实际叶帧重叠用例固定 **100 轮**，每轮 **3 项 / 336 条断言**，全部通过；不是遇错重试直到成功。原 100 轮中的一次旧版失败继续保留，不能因此宣称根因已经证明或永久消除。
- 完整普通 worker 固定三轮各 **290 项 / 42,386 条断言** 全通过，退出 0。运行脚本 `run-normal.sh`、各轮 `.log/.time/.status` 及前后源码/程序校验均保留；43 份冻结源文件和程序前后身份一致。无负载覆盖、无亲和性，无并发构建或其他重测试。
- 上述是正确性和重复性证据，不是 TPS 基准，也不替代当前 ASan/UBSan/LSan、私有 TSan 与真实节点验收。

## 促成本批的证据

G6d 基线普通 worker `d3964b3f…` 三轮各 289 / 41,966 通过；ASan/UBSan/LSan 程序 `b43d9692…` 的二级专项三轮各 14 / 14,950 通过，完整首轮 294 / 296、43,585 / 43,588，221.15 秒，失败即停止后两轮。两项旧混合采用测试的 4 worker 场景出现三个计数失败，并记录 worker/preparation deadline；未报告内存错误或原状态/trace 差异。完整失败程序、源码、日志和恢复信息均在 G6d 证据目录，不能以本批未来通过覆盖该失败。

其中 `serial_fallbacks == 0` 不能解释为交易没有执行：`for_each_parallel_production_transaction` 在准备返回空结果时仍调用原串行 callback，但这条外层路径没有按事务增加该计数；本轮原 callback 数量、回执和状态对照没有报告失败。计数口径与采用率、真实执行结果必须分开，不能通过修改预期计数隐藏 deadline。

## 修改及限制

- `parallel_wasm_worker.cpp` 用私有 `functions` 适配器继承原注册器，只在原 `resolve(module)` 周围保留共享 mutex。导入存在性、种类、签名和异常顺序仍由原 resolver 检查；`call_once` 注册仍发生在 backend 构造前。
- 移除覆盖整个 backend 构造的外层锁。两遍解析、模块 allocator、globals 和执行 context 都保持 worker 独占，不使用 `backend::share`；构造时尚无线性内存 allocator，因此不执行 start。原解析/内存/代码上限不变，编译成本仍受原尝试 deadline 检查。
- 这是基于源码的临界区缩小，不是旧重叠失败根因已被证明，更不保证所有本地截止失败消失。线程检查和冷/热测量仍是验收条件。
- 访问清单只更新该文件哈希/行号，805 份生产源文件、262 个原 host 注册及所有候选访问分类计数不变；没有新增接口或解除二级正式提交保护。

## 新测试及验证顺序

`parallel_module_cache_tests/concurrent_cold_linking_preserves_types_failures_and_private_ownership` 在 4/2/1 worker、各三轮中并发构造相同和不同代码；精确拒绝缺失接口、错误签名、截断模块和错误 magic，再检查健康执行、私有写之后 trap、同模块健康复用及原数据库不变。WAT 转换和 Boost 断言都在 owner 上；worker 只持有冻结数据，模块在创建线程使用/销毁。启动失败时释放等待中的线程，并预先预留 future 容量，避免临时 future 析构等待未打开的启动门。

该用例须单独在新进程运行，才能覆盖四线程首次进入私有注册 `call_once`；随后并入完整套件。它不以时间重叠或 TPS 作为通过断言，旧三个实际 WASM 叶帧重叠用例仍原样保留。下一步实际结果和失败证据在此追加，不能把源码存在视为已经验证。

## 截止失败后的接续源码复查（未实施）

`controller::prepare_isolated_attempts` 在每轮工作全部 join 后才捕获补充数据，随后从头重跑。同步及 mixed 的实际调用先 `state.read_account(receiver)`、再查代码目录；缺少两者时可能先请求账户、下一轮再请求同一 receiver 的程序。mixed 升级也只给 roots 补账户，而目录中可能已有先前树阶段发现的其他 receiver。这些是可检查的冗余发现路径，不是本次全部截止失败已归因。

下一步优先评估实际调用目标的账户/程序合并捕获：请求仍只携带 owned 值，原 owner 补充并生成新的不可变快照，worker 不接触 live DB；只针对真正要执行的 receiver，不猜测普通 `is_account` 参数会被执行。必须保留“账户存在性先于 flags/代码”的原可见语义、缺失账户负返回、serial-only/native/特权边界、未用目录的指纹验证、相同绝对截止及所有资源上限。若目录已知而账户未知，也必须保证捕获确实推进，不能变成重复请求死循环。需要补同轮双请求、已知/未知目录组合、缺失账户+非法 flags 的先后顺序、取消和原执行对照，再衡量发现轮数；不得简单增大时间限额或删除采用率断言。
