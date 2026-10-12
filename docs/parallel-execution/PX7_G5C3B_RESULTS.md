# PX7 G5c3b：context-free 内联执行（开发与验证中）

从 `11d05e7` 继续，证据目录 `build/local-tests/parallel-context-free-20261012-wbEaQl/`。本阶段仍在实验分支，默认开关关闭，不宣称所有合约已全面并行。

## 当前结论

b1 的 CF 内联正式采用、原空数据 memcpy 边界修复及有界回退诊断已经实现；最新普通工作池三轮各 128 项 / 9,296 条通过。较早 b1 的真实生产/恢复结果在下文保留，但新增修复/诊断后的节点程序还需重新构建和复测，不能混用程序版本声称已全盘验收。

最新 ASan/UBSan/LSan 全套仍未通过：135 入口中 130 通过、5 失败，10,426 条中 26 条失败；没有新的内存诊断。明确捕捉到树工作池约 500ms 的 deadline 回退，以及正式生产批次的 deadline/采用不足。后者原有整批上限是 50ms，不应误称全部都在 500ms 触发。旧 TSan 首次偶发失败仍不能仅凭本次现象追认同一根因。继续测量/优化准备、编译和执行路径，不放宽上限、不删除实际采用断言，不提前勾选 b1 完成。

## b0 私有执行与结构预检

正在实现/验证：

- 新 `isolated_context_free_data` 在分配前检查最多 1,024 条、总计 256 KiB 的本地尝试预算，然后独立拥有数据并一次计算摘要；各个步骤共享 const 所有权，只绑定摘要。不持有 packed_transaction 或 controller 指针。超限只是本地回退，不是共识新限制。
- 调用树发送事件、子组明确区分 context-free；根与步骤的 v2 本地指纹绑定数据摘要及调用类型。发送顺序分配 ordinal，执行先通知，再 context-free 子队列，再普通子队列。
- context-free 中允许固定输入、内存、断言和 `get_context_free_data`；已有所有 context-aware 接口增加入口检查，包括授权、账户/主索引读取、写入、通知、两类发送、get_sender 和 set_action_return_value。并非只禁止写入。其他未支持接口仍整笔回退。
- 数据读取保留原来的缺项 -1、零缓冲区查询长度、截断拷贝和内存范围检查。子动作必须无授权。原始发送字节和严格长度检查沿用先前实现。
- 整树重建检查类型、两个子队列和禁止效果；默认 context 不提供新能力，正式 owner/adopter 暂未开放，原三类私有入口仍不能发送 context-free inline。

新增测试包括不可变输入/边界、交错队列、19 类禁止接口、篡改预检、空代码子动作、1/2/4 线程混合成功失败，以及原串行 trace/receipt 和接口失败码对照。构建/执行结果待补充，不能用用例已写代替通过。

初次构建通过；`private-first.log` 的 7 项 / 397 条通过（6 项私有 / 297 条、1 项原链队列与 receipt 对照 / 100 条）。随后补充固定输入读取、模块 start 的上下文限制、同交易 ID 不同附属数据拒绝复用，以及一页内存尾部/零长度读取。尚待扩展后的完整验证。

审计清单本身匹配；第一次审计单测 24 项中有 1 项失败：新增两项树接口未同步旧测试的精确名单。已补 2 个正确签名的真实调用夹具、树接口集合及仍禁止链接的 225 项计数；保留 `audit-tests.log`，复测另存 `audit-tests-final.log`。不是删除检查或把禁止调用改成成功。进程工具 16 项通过。还在运行原链接口对照前修正测试部署 helper：原 setcode 拒绝同代码重复部署，helper 应跳过已安装的相同父代码，不修改原 setcode 规则。

扩展后首次全工作池 116 项中 115 项通过，8,387 条中 8,385 条通过；两条错误码断言失败。`oracle-detailed.log` 确认 db_next_i64 / db_previous_i64 的 output=0 会先在原 `webassembly/common.hpp::from_wasm` 被拒绝（3070002，不能为空指针建立引用），未到 CF 权限检查（3050007）。CF 禁止接口测试改用合法非零输出指针来单测上下文限制，另留独立空指针/失败撤销对照，不把两种失败宽泛地当一种。

此对照同时发现旧私有 `adjacent()` 缺少原 legacy_ptr 的非零检查：普通调用也可能计算成功，并在正式直接重放 cursor 时绕过原参数转换失败。这是实际并行语义风险，不仅是测试预期问题。新增先写行再调用 next/previous 的四种私有入口与原链对照，先验证未修复版的差异，再修复空指针回退并复测；该修复尚在进行中。原 VM 的错误类型和检查先后顺序不改动。

未修复负对照 `adjacent-before-fix.log`：4 项 / 220 条中仅新私有回归的 8 条预期失败（两个接口 × 四个入口意外未拒绝），3 项原链 oracle / 200 条通过。原链对照含普通父动作先建表写行，再传空输出指针时整笔撤销，以及非零输出正常成功。对应程序和源码归档 `adjacent-before-fix-artifact.tar.gz` 已逐文件比较，SHA-256 `35578255c43bfcc0032eb44873da2bfe469c6b478225f663c37829e73c8b6f8f`；该程序是故意保留的未修复对照，不用于正式节点。现在为私有 adjacent 加非零检查，正在重建/复测。

随后修复版完整普通工作池三轮全部通过：每轮 118 项 / 8,455 条，见 `worker-fixed-{1,2,3}.log`。固定测试程序 `e9679768e1b34a5144e4850c4c0bee327cace586771ff57e84afb909a9c7cee5`。新增 CF 私有 9 项 / 324 条、原链 oracle 3 项 / 200 条；原有功能及两项新增链接回退夹具一并通过。测试中复制未使用的空签名容器曾触发 GCC 告警，改为只构造相同主体后重建无该告警；两版日志均保留。审计清单再次匹配。此处仅普通 b0 验证完成，新的 sanitizer、正式 CF 采用和进程矩阵仍待 b1 一起验收。

## b1 正式接入与边界验证中

正式首次采用必须核对实际 packed 数据；数据核验成功后逐步骤只复用不可变已验证摘要，不能反复复制整个事务输入。原 send_context_free_inline 仍在发送位置运行账户和无授权检查；保留原 undo 和 CPU。skip_trx_checks 下原路径会跳过 CF 执行，须在首次采用前拒绝不兼容的树，不错位消费后续步骤。

`transaction::id()` 只对交易主体计算，不能用它代替 context-free 数据绑定；两份主体相同而附属数据不同的尝试必须有不同的本地执行指纹。生产 owner 当前的 skip_auth_check 门槛与 skip_trx_checks 同属 light_validation_allowed，但 adopter 仍需独立防御，不能只依赖调用者碰巧走到该门槛。

b1 预定落点：结果持有同一个 const 数据快照，executor 返回、journal 对照和 controller 准备一起绑定；adopter 第 0 步把它与实际 packed 数据核对，后续步骤复用结果内不可变快照，不重复复制/散列整段。检查真实 trace.context_free 等于组类型，按发送事件选择原 send_inline / send_context_free_inline；首次采用前拒绝 skip_trx_checks。保留默认不带快照的旧私有调用方式，但不能据此支持 CF 子组。新增正式测试要让普通父动作先写行、CF 子动作再校验数据，验证错误撤销整笔、成功原 receipt/状态一致、1/2/4 工作线程确实采用，以及之后的恢复和混合版本。

上述 b0 普通初验已保存为 `a393b84`。修复后程序归档 `private-fixed-artifact.tar.gz` 比较通过，SHA-256 `7b471e11e2dade9712a49a6eae787040cd3087b58d168679a21757c7cb233bc2`。

b1 开发中：已按上述落点接入结果快照/实际输入核对、两类原发送接口、真实 CF 标志及 skip_trx_checks 防御；undo 补清通知/树指针与 receiver 索引。测试增加结果快照篡改和同内容不同快照对照，并加入父动作先建表写行、四个 CF 子动作、交错通知/普通 inline 的 0/1/2/4 工作者生产/验证比对，以及 CF 数据错误后的撤销、串行尾部和下一批重新采用。正在 `build-adoption-first.log` 构建，尚未验收；本轮 sanitizer 和真实进程/恢复仍未完成。

下一批补测：CF 空代码接收者、未入本地名单与原生/系统接收者屏障、数据捕获超限仅回退、顶层 CF 仍串行、LIGHT/跳过检查不消费 CF 尝试；随后导出新的 CF 业务夹具做真实多模式生产/恢复。二级索引、同步私有执行等后续阶段继续保留为未完成。

编译阶段保留两个失败记录：`build-adoption-first.log` 是 BOOST_REQUIRE_MESSAGE 条件表达式缺括号；`build-adoption-expanded.log` 是新恢复导出器把临时交易传给要求左值引用的 tester 接口。均只修正测试写法，未修改共识限制或删除断言；`build-adoption-expanded-2.log` 正在重建。另将 CF 数量比较的类型统一，消除有符号/无符号告警。

已编写正式边界：空代码 CF 接收者、未批准代码、超过本地 CF 数据预算但协议合法、顶层 CF 继续串行、LIGHT 不启动尝试、原生 eosio::linkauth 的 CF 失败和父写入撤销，以及同交易 ID/不同 CF 数据在正式第 0 步无副作用拒绝。全状态、原始 trace/receipt/console/RAM、确实采用和回退分别检查；尚待程序执行结果。

新鲜进程导出器为两根动作、18 个步骤、8 个 CF 子动作，父动作创建独立业务行，CF 子动作校验只读附属数据；恢复导出器另外混入顶层 CF 的串行交易和三块竞争分叉。新 `--context-free-fixture` 与其他业务类型互斥，逐项检查 ordinal、祖先、授权、CF 类型、payload、RAM 和实际执行顺序。进程工具单测已 17 项通过（含错误参数及 10 类篡改/循环树拒绝）；审计 24 项、清单匹配。真实进程与恢复仍未跑，不能用导出器已编写代替验收。

`transaction_context::undo()` 清理已补入，本轮正在验证撤销/复用。顶层 context-free actions 的并行、同步调用、二级索引及其他一般 host 仍未开放。

普通 b1 初验：修正测试编译写法后构建成功，无该文件告警。`adoption-expanded-2.log` 的 20 个入口 / 1,142 条全部通过，其中 18 个实际功能用例，2 个导出器因未提供环境变量未生成数据；正式采用 6 个功能用例 / 615 条，私有 9 项 / 327 条，原链 oracle 3 项 / 200 条。随后完整工作池三轮各 126 个入口 / 9,073 条通过，见 `worker-adoption-{1,2,3}.log`。同内容快照可采用、同 ID 异内容在任何写入前拒绝，空代码可采用；超预算/顶层 CF 无尝试但原串行成功，LIGHT 无尝试且原结果一致；原生 CF 错误保持 missing_auth_exception 并撤销父行。正式生产/验证 0/1/2/4 线程对照及失败后恢复通过。继续构建核心测试和 nodeos-parallel，新的 sanitizer、导出器实际生成及真实进程矩阵仍待执行。

正式核心三轮均通过：每轮 63 个入口 / 12,378 条，跳过 1 个 benchmark；导出器空入口不计作实际进程验证。当前 reference-contracts 的原 token（`ea1bd149e28f21420450b6c7b2c5be600ef9922e44c383fa4489b025aac0eb46`）三轮各 3 项 / 1,471 条通过；原同步/只读 102 项 / 1,070 条、状态层 57 项 / 90,530 条、串行参考 2 项 / 5,165 条通过；21 组兼容检查全部通过（56.27 秒）。新增 CTest 入口 `pureeos_parallel_context_free`，待下一次配置刷新后执行。

本轮固定普通程序：nodeos-parallel `cffe60f96cf11efe6ddb226dc9a94f6263804c1594490a3025c18106492aa890`，核心/导出器 `b100c8dd7389e7f2648e850d608ddddc9a1c75a8f553dc8a9534adf2f74ce215`，工作池 `51d4f35869fb9a44f280bdb1541e7cea30989d209276269eaa97bd5ac9271404`；其余哈希见 `adoption-runtime-artifacts.sha256`，原 nodeos 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。新进程父目录 `/dev/shm/pureeos-g5c3b-process-7sEW3Q`，正在运行 CF 五模式生产，结果待补。

CF 真实生产五模式全部通过：`parallel-producer-b69uv2xy/report.json`。新 0/1/2/4 工作者和旧生产者各自新鲜 16 笔 / 32 根动作 / 288 个执行步骤；每个并行模式均为 2 批、96 次含发现的尝试、16 次成功并采用、0 次已排队尝试回退。每组新旧观察者的区块/完整状态/SHiP 字节摘要一致，旧版强制检查重放一致；所有交易最终确认，固定观测区间内正常 500ms 出块。这里是兼容性/最终确认的有限负载，不是饱和 TPS 基准。

恢复导出器已实际执行并通过 387 条断言，产出 18 个连续检查点与 3 个竞争分叉，含普通根的 CF 子树和明确串行的顶层 CF。`cf-recovery-corpus-v1/manifest.json` SHA-256 `728eec38fef6e7d84cc106ac50a7ba441b3ec2b5a51ada41c8a760b9b5ea645b`；0/1/2/4 验证器逐块 oracle 及并行实际采用检查通过。正在运行真实恢复矩阵，尚未宣称恢复或 sanitizer 完成。

真实 CF 恢复 55 阶段全部通过，目录 `parallel-process-1omboa0m`：非法启动参数拒绝、逐块对照、正常重启/SIGKILL/脏库拒绝、强制检查恢复、恢复记录截断/缺父 fail-closed、关闭并行不转换状态、不可逆快照恢复及后续块、混合版本 P2P/完整 SHiP 比较、竞争分叉。各并行模式常规 18 检查点实际采用 48 笔、分叉序列实际采用 18 笔；顶层 CF 交易沿原串行。不是断电硬件测试。

CF 两组进程证据已归档并逐文件比较：`cf-process-evidence.tar.gz`（0600），SHA-256 `a743b83f82295a2ea6e3b3158fa8638c82f21e8241fb885bb44c714e1d0f1d96`。确认两个报告 passed、无节点进程、再次核验哈希/tar 内容后，`cleanup-cf-generated-state.py` 仅移除精确 58 个生成的 `data/state/shared_memory.bin`，释放 256,253,952 字节；路径逐条保留在 `cf-generated-state-cleanup.log`，可从归档恢复。日志/配置/区块/快照和用户链未清理。继续旧 ordinary-inline 与当前 token 五模式进程回归，然后执行本轮插桩检查。

旧业务真实回归均通过：ordinary-inline `parallel-producer-k0_vegvq`，当前 token 多动作 `parallel-producer-gb9chng3`，各 5 模式。每个并行模式 inline 采用 4 / 回退 12，token 采用 6 / 回退 10，符合固定热点/队尾策略；新旧区块、状态、SHiP 和旧版强制检查重放一致，全部交易最终确认。固定程序哈希复核未变。程序加两组进程证据归档 `adoption-runtime-and-regression.tar.gz`（0600）逐文件比较通过，SHA-256 `e1439030356be86c0b24b24a4e421bb7a9ba8df9493b001a37253ffcdf942c90`。再次核对报告、无运行节点、哈希/tar 后，仅移除两组的 30 个生成状态文件，释放 132,833,280 字节；脚本/逐条路径在 `cleanup-regression-generated-state.py` 和对应日志，可恢复。

刷新配置后新增 CTest `pureeos_parallel_context_free` 已通过（4.49 秒）。正在构建本轮 TSan：只执行私有 worker/数据层，明确排除未完整插桩的 controller/oracle/正式采用测试；后续 ASan/UBSan/LSan 包含正式采用，仍不冒充整节点全库插桩。线程和内存检查尚未完成。

TSan 构建保留 SoftFloat `s_mulAddF64.c` 的两条 `sig128C` maybe-uninitialized 告警，与 a3 `build-tsan.log` 相同。源码复核发现两处读取均只在 expDiff>0 的分支，前段在同一条件赋值且中间不修改 expDiff；本轮没有为消除告警而改第三方算法或关闭告警。后续插桩结果不能被表述成 MSan 的未初始化内存检查。

首次 TSan 三轮请求未全部通过：资源池 15 项 / 1,072 条、原生状态/浮点 63 项 / 90,619 条均三轮通过；私有工作池第一轮 107 项 / 7,301 条通过，第二轮旧 `tree_workers_overlap_real_wasm_intervals_without_sharing_writes` 在一个 outcome 缺少结果处失败（其余 106 项通过），第三轮因 until-fail 未执行。没有 ThreadSanitizer race 诊断，但不能据此忽略断言失败。该 fixture 每个 WASM 人工循环 50,000 次、两根/多步骤，整笔仍受原 500ms 本地上限；目前只是假设可能触及预算，尚未确定失败原因。

同一固定 TSan 程序隔离该用例连续五轮均通过（各 66 条），失败日志仍保留。接下来用仅诊断的 `trace-private-throws.cpp/.so` 在异常已抛出后记录类型/栈，定位 worker catch-all 原因；不修改生产源码、时限或通过标准，不把带诊断辅助的运行冒充正式验收。未完成本轮线程验收，ASan 仍待后续。

诊断辅助下，整个 tree pool 4 项 / 757 条通过，完整私有工作池三轮各 107 项 / 7,301 条通过；该重叠用例没有产生异常记录。辅助程序已能记录其他有意触发的 primary budget/invalid-attempt 等路径，但未复现首次失败，不能反推它一定是超时、散热或系统负载。原二进制三个 SHA 均未变。正在无 LD_PRELOAD 的原 CTest 入口再跑三轮，保留首次失败作为尚未唯一定位的偶发测试回退；没有降低 50,000 次循环、放宽 500ms 上限或删除结果/实际重叠/隔离状态断言。

随后无诊断辅助的原 CTest 私有 TSan 三轮全部通过：每轮 107 项 / 7,301 条，合计 74.02 秒，见 `adoption-tsan-clean-repeat.log`。固定工作池程序 `8cbbc6f88f0a71ed266a35990935f6b6f226f708b768e4e09b9bde4760164f3a`；原生程序 `448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`；资源池 `e7e85ce5745dd3e96fbce9dfbf27cd435b5a6322506af0fea362147a55b423a3`。本批最终重复通过不抹去上面的首次失败，既不宣称全程零失败，也不宣称其根因已修复。保留有界回退诊断待办，继续 ASan/UBSan/LSan 正式采用检查。

固定 TSan 三个程序再次核对哈希后归档 `adoption-tsan-artifacts.tar.gz`（0600），逐文件比较通过，SHA-256 `1b8b1040e793aaaa6286187e36880e043e34920585d3e53bbabb90fc5ff39003`；随后才切换至 address,undefined 构建，未覆盖丢失 TSan 的通过/失败版本。ASan 构建进行中，尚无本轮内存检查结果。

ASan 最后资源池链接因 `/dev/shm` 用户配额不足失败（不是源码编译错误），保留 `build-adoption-asan.log`。构建完全结束后，将本轮 `parallel_wasm_sanitizer_test.dir` 生成对象缓存移至证据目录 `asan-object-cache`，原位置仅保留指向该目录的符号链接；没有删除对象、覆盖节点或清理用户链。配额可用量从 27,806,720 增至 433,097,728 字节；原参数重试链接成功，见 `build-adoption-asan-link-retry.log`。

首次 ASan 全套发现两个问题，尚未通过验收：旧三模式切换测试出现一次缺少树结果的回退断言失败；原串行 `apply_context::get_context_free_data` 对空数据、非零目标缓冲区调用 `memcpy(..., nullptr, 0)`，在 `apply_context.cpp:785` 触发 UBSan 的 nonnull 检查。后者有具体源码与运行证据，不是将未定位的工作池回退推测为相同原因。接下来保留修复前程序/失败日志，修正零长度复制，并增加有界的本地回退分类诊断，再重新验证；不得调大原 500ms 预算、降低忙循环或删除成功/隔离/重叠断言以规避失败。

首次 ASan 全套在 UBSan 报错后最终达到 CTest 180 秒上限退出，不能算完成剩余用例。相同程序单独运行 `original_data_host_and_forbidden_apis_match_private_decisions`，关闭报错栈展开但保留 halt_on_error，0.77 秒以退出码 1 重现同一 nonnull 错误，见 `asan-empty-cf-before-fix.log`。三个修复前程序已核对哈希、归档并逐文件比较：`adoption-asan-before-fix.tar.gz` SHA-256 `d32ac66af5142a8deb297350041ddf7ce48933a3b707a58725dc0467c06f4c14`。生产源码只为零长度复制增加分支，原返回值不变；测试补查空输入不改写原缓冲区和零缓冲区长度查询，正在重建。

零长度修复重建后，原失败 oracle 的 1 项 / 52 条通过，见 `asan-empty-cf-fixed.log`。全套 `adoption-asan-empty-fixed-full.log` 在 65.53 秒完成，但仍是失败：133 入口中 128 通过、5 失败（其中 1 中止），10,662 条中 31 条失败。没有新的 sanitizer 内存报错；失败为树工作池缺结果，以及 ordinary-inline/CF 正式采用数量断言，不能将状态/trace 一致等价成采用要求达成。保留该版程序的 SHA 与 `adoption-asan-empty-fixed.tar.gz` 逐文件比较归档，不用反复重跑的成功掩盖当前失败。

现在开发有界诊断而不改变预算或调度：检查点抛出保留原 invalid-attempt 基类的细分类异常，在触发时区分取消/截止；工作池仅保存固定 enum 与尝试墙钟，分别识别缺状态、缺程序、预算、未支持能力、无效 ticket、VM 超时/错误、分配失败、未知异常，失败仍不发布任何前缀且 elapsed 保持零。增加 owner 的本地准备阶段截止/预算/其他回退计数和 worker 类别摘要，不进入链状态、receipt、SHiP 或 CPU 计费。测试失败消息打印这些计数，不放宽原采用断言；新用例覆盖取消优先级、旧异常基类兼容、十类实际坏任务/随后成功重试，以及原通知/树发现路径。分配失败和未知异常的 catch 分类不等于已实际注入整机 OOM。正在 `build-fallback-diagnostics.log` 重建普通和插桩工作池；新诊断尚未运行验证，也不把它当作偶发失败根因已修复。

上述空数据修复版、诊断改动前的插桩程序归档 SHA-256 为 `c42ef2d1941224e98b7cedd3d9938361c2c150e9f58c52712883216d9a8681ff`。诊断代码对应词法审计重新匹配，审计测试 24 项、进程工具 17 项通过（`audit-diagnostics-*`、`harness-diagnostics-tests.log`）；这不是 C++ 编译/运行验证的替代。

诊断版构建成功。新增检查点 8 条、十个故障场景及随后恢复共 110 条通过；工作池定向 22 项 / 2,764 条通过。完整普通工作池三轮各 128 项 / 9,296 条通过，见 `diagnostics-worker-{1,2,3}.log`。普通程序 SHA-256 `3299586503d51417a787c15278906cc16490a5469700ba97915233abea322806`，插桩程序 `c342c83c034a8b13149c9d212337ca5c29b8218d5b24e9e1fcf338923cde681a`。构建保留 state_history 复制 signature/std::variant 路径的 GCC maybe-uninitialized 告警，未改签名表示或关闭告警。

`diagnostics-asan-full.log` 在 79.63 秒结束：135 入口中 130 通过、5 失败（2 中止），10,400 / 10,426 条通过。新故障分类用例本身通过；树三模式切换抓到 threads=2、wall=500476us，树重叠抓到 threads=1、wall=500531us，均明确为 deadline。正式采用失败的 worker_deadlines 非零；部分 CF 批次私有成功 4 笔、仅采用部分并串行回退，不能将其与“私有完全没有结果”混为一类。生产原有 cancel.deadline 最多 50ms，owner 的外部取消/截止检查先抛 interrupt_exception，所以 preparation_deadlines/budget 可以为零；并不是一定到达 500ms 准备上限。继续保留所有采用断言并定位编译/执行耗时，尚未验收。

上述两份诊断程序测试后哈希不变，归档 `fallback-diagnostics-artifacts.tar.gz` 已逐文件比较，SHA-256 `2b7bf97e2dc9247941bb8f32631819fd48752b3f5304d09ad4ad30094191b390`。此处保存开发检查点，不标记插桩/全部计划完成；后续继续有证据的性能定位和修复。
