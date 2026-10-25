# PX7 c3c/m2b：混合调用树正式接入（实施中）

日期：2026-10-14。分支 `feature/parallel-tx-execution`；证据目录 `build/local-tests/parallel-mixed-adoption-20261014-ECzQQD/`。此前 m2a 普通/插桩记录及完整回归失败继续保留，不当作本版测试结果。

**当前状态（2026-10-15）**：混合真实生产五模式、完整 55 阶段恢复各首轮通过，实验节点 `dfd1fe8b…`、导出器 `74a9a378…`；该 worker 普通完整固定三轮各 **269 / 26,757**，核心三轮各 **63 / 12,378**、实际 token 三轮各 **3 / 1,471**、原兼容 **21 组**均通过，七类相邻入口各三轮通过。实际生产/恢复导出分别 **1 / 845**、**1 / 1,123**，harness **22 项**通过。旧 ASan 首轮 **272 / 276** 的四项失败保留。混合专用有界闲置模块复用版 `6468eb51…` 新增七组 **7 / 474** 首轮通过，完整普通固定三轮各 **276 / 27,231** 全通过；新 ASan `997122aa…` 首轮 **278 / 283**，五项／十条采用计数断言失败，后两轮未执行，未报告内存错误，但完整检查不是通过。重复进程、本版线程检查、计时稳定性及 G6/G7 尚未完成，不验收全部 G5 或整个计划。以下保留各时点记录，早期状态不代表最新进展。

## 正式边界扩充：专项通过，完整回归保留时序失败

本批只修改混合测试，生产实现与正式首版一致。构建退出 0；程序 `9dd3779d50228600a145e5fa42e9204f08ab394900de5ee8691aba3074a1a8cd` 保存为 `worker-formal-boundary-initial`，源码快照为 `formal-boundary-tests-initial.cpp`，14 项清单 `formal-boundary-initial-sources.sha256` 在构建后及测试前核对一致。`formal-boundary-first.log` 记录首轮 **14 / 4,536**，29.99 秒；不将旧版完整结果套用到本版。

新增覆盖：

- 五层同步帧与通知、两类 inline、多根混合；原生产、原串行验证、并行验证在 0/1/2/4 worker 下比较稳定轨迹和全状态。每个后续 receiver 的防御回退继续保留前缀；只累计一次各帧独占 CPU，不重复计入递归的包含耗时。
- 只在外层设置只读，深层 flags=0 及回调根账号仍继承只读；无写批次两笔必须全部采用。
- 子同步调用删除并重建行，同组根/通知旧句柄失效，新查询依次取得新句柄；inline 单独从新缓存开始。另对使用旧句柄、写后 trap、深层只读写入拒绝分别核对原失败轨迹、账本/RAM/去重状态撤销，同代码同链同工作池随后健康交易成功。
- 队列提前停止、回调抛错、abort/start、过期截止、重入拒绝、替换 metadata 身份和原 interrupt 路径；重启 pending block 后新混合结果可采用，旧能力不能残留。
- 同一未签名字节产生相同交易 ID，错误签名即使私有执行成功也必须被原授权拒绝；重复交易仍由原入口拒绝，abort 清除原交易状态并允许健康重试。

本批没有扩大 50ms/500ms 预算，没有改变原同步 RAM 语义或 `adopted` 的既有“执行采用次数”含义。混合节点进程夹具、恢复和本版插桩仍是后续工作。

完整普通 `formal-boundary-normal-three.log` 首轮耗时 107.85 秒，退出码 8：唯一失败是旧 `sync_leaf_wasm_intervals_overlap_on_two_and_four_threads` 未观察到叶帧区间重叠。混合 14 项全部通过；没有状态/轨迹/内存错误报告，但不据此宣称完整通过。`until-fail:3` 在首轮终止，后两轮未执行。同一准确程序固定十次独立诊断全部 **1 / 42** 通过，见 `formal-boundary-leaf-diagnostic-ten.log`；这不能抵消完整失败，也尚不能证明根因。

该旧测试 barrier 位于首次编译之前，冷编译及调度错峰可能影响短叶帧观测。只增加失败时的线程数、root/leaf 微秒区间及独占耗时诊断，**不修改原循环次数、重叠断言或预算**；源快照 `sync-tests-before-leaf-diagnostics.cpp` 保留。此问题与此前插桩下的时序敏感性一起归 G7 继续定位，不伪装为已修复。

## m2c 混合进程语料（源码已写，尚未运行节点）

新增独立 mixed 生产/恢复导出器：原签名动作创建状态，两个根动作各含三层同步、两条通知、两类 inline；CF 接收者实际读取捕获的 CF 数据。每笔十条普通 action trace、八条同步 call trace，真实累加同一行而非覆盖常数。生产导出器用原串行路径依次生成十六笔交易的期望轨迹；Python 新 `--mixed-fixture` 逐接收者/子调用核对输入、返回、只读、身份、队列顺序、无错误及无额外 RAM 账。HTTP 提交轨迹仍明确不等于实际生产采用轨迹，后者由 controller 差分证明；SHiP 未序列化同步 call trace 的限制不隐瞒。

固定两批各八笔同一行冲突的语料要求实际采用 **2**、原路径回退 **14**；仍单独检查十六笔全部入块/最终确认、正常 slot、旧程序全验证、全状态/SHiP 一致和强检查重放。恢复导出器准备连续区块及三块竞争分叉，用两根/一根造成 +8/+4 的不同真实余额，检查切换后撤销，再继续 +8；不能用两条分叉写同一常数冒充回滚。

Python harness 新增错误参数组合、两批精确计数、十接收者/八子调用各字段篡改及嵌套 JSON 检查，**22 项单元检查通过**（7.089 秒，无 nodeos）。C++ 导出器和区间诊断正在单任务编译，`formal-process-initial-sources.sha256` 固定 17 个源码文件；没有把普通默认跳过的导出入口算作实际导出成功，更没有据此声称真实节点/恢复验收。

导出器首版构建成功，程序 `bfd4cbda12bc0e62e28a1020170e8d82398394d67d3663e7ad927275dca25a20` 保存为 `worker-formal-process-initial`。实际生产导出首测 **13 / 14** 后中止：串行 oracle 沿用链级测试的 `contracts_console=true`，却要求与真实节点的 false 配置一样没有 console markers。原 `apply_context::store_console_marker` / `sync_call_context::store_console_marker` 在 true 时即使无文本也记录位置；原日志 `mixed-producer-export-initial.log` 和失败目录保留。仅将生产导出 oracle 改为真实节点相同的 false，保留原断言，开始重编译；链级 true 条件仍由既有正式差分测试覆盖。

同一 `bfd4cbda…` 实际恢复导出 **1 / 1,123** 通过，语料 `mixed-recovery-corpus-initial/`、日志 `mixed-recovery-export-initial.log`：覆盖 0/1/2/4 worker 对全部区块的原串行轨迹/全状态对照及 +8 → +4 → +12 的竞争分叉回滚。它是导出器内部 controller 验证，**尚非 55 阶段真实 nodeos 恢复验收**。

控制台配置修正后构建退出 0，程序 `72df1e17f0a73762d5babca893adf99fc6b577d5c50ebb1843524d67e39a0718` 保存为 `worker-formal-process-console-fixed`。新目录实际生产导出 **1 / 797**、恢复导出 **1 / 1,123** 全通过；harness **22 项**再次通过，6.980 秒，日志 `mixed-harness-tests.log`。`formal-process-console-fixed-sources.sha256` 17 项在测试前一致，编译/链接参数另存；开始同程序固定三次 269 项完整普通回归，逐轮保留结果，不用循环直至通过代替固定样本。先前冷叶帧重叠失败和初次导出配置失败均保留，真实节点及插桩仍待本版验证。

上述固定三轮随后全部通过，日志 `formal-process-normal-{1,2,3}.log`：每轮 **269 / 26,757**，用例耗时 **108.60 / 109.95 / 107.78 秒**。默认 100 档、50,000 次既有压力循环，无亲和性、无编译/压缩并发；第二轮只另做轻量只读访问清单核验，精确匹配（`formal-process-inventory-check.log`）。17 项源码及当前程序测试后再次核对一致，原 nodeos 仍为 `e28d8313…`。两个新导出包装入口默认不生成语料，真实导出计数由上面的独立实际运行记录支撑，不能以 269 中的包装入口通过冒充真实进程。下一步重建本版实验节点与相邻核心/兼容目标，继续五模式混合生产、55 阶段恢复及插桩验证。

磁盘维护仅处理一份本任务的重复历史检查程序：`parallel-mixed-return-scope-20261014-2PI1SZ/wasm-asan-before-m2a` 与同目录 `wasm-asan-current` 逐字节及 SHA-256 均相同（`bd0c85de…`）。前者压缩为 `.gz`，69,723,619 bytes、0600、SHA-256 `278559c6432d80f12767573cc2524620cd30d0721fcf8ec51092cbfd9919dc43`，通过 gzip 完整性及解压流哈希校验后才移除其 394,487,448-byte 原件；另一份原件仍保留，可从它或压缩包恢复。净释放约 310 MiB；三份校验记录为 `archive-baseline-{originals,restored,compressed}.sha256`。未移除日志、源码、已有链或其他产物，操作不与构建/测试并发。

原实验节点 `15d5ac17…`、核心 `0f6e7d8d…`、兼容 `49ce63f9…` 在更新前分别保存为 `nodeos-before-formal`、`validation-before-formal`、`compatibility-before-formal`，清单 `pre-formal-node-core-programs.sha256`。现在单个 `-j1` 重建这三个目标，日志 `build-formal-node-core.log`；它们的新身份与实际进程结果尚待构建后核验，不引用旧程序通过记录代替。

### m2c 真实进程首测：HEAD 临时执行预期修正

三个目标已成功重建：实验 nodeos 为 `dfd1fe8bff669d1bf85d7aaa8a28ab12da29ef7f0e7e744326341f500e98c7de`，核心为 `d2091b91d3185ff943ed4a3893fe2a960a499eb3d3897c6de4f85065b7c75cc0`，兼容为 `93c09e85b1223b6092903b9d895aba7f38c61c7f0504416c23f9261ed6f38e00`。备份及清单 `formal-node-core-programs.sha256` 保留，原 nodeos 未变。构建成功不代表这些新程序已完成回归。

五模式混合生产首测在 **并行关闭模式、第二笔 HTTP 提交** 失败，尚无完成的模式：`parallel-producer-rpvt5gn3/report.json`、`mixed-process-first.log`。增加失败前立即保存单笔原始响应和精确返回差异的诊断后，再测 `parallel-producer-2o9t7rx3/report.json`、`mixed-process-return-diagnostic.log` 确认：两次提交均返回 13/17，导出器却将第二笔期望写为 21/25。

原 `controller.cpp` 在 HEAD 模式的 ephemeral pending block 中保存待出块交易后撤销临时执行；真实夹具暂停生产并使用该模式。导出器原先使用普通 pending block 连续执行，错误地将先前交易状态累积进后一次**提交**预期。修正仅针对导出器：每笔串行 oracle 执行后 abort/start，验证原始状态哈希未变、两根返回恒为 13/17；恢复生产后交易仍应按原序累积，不能修改链模式、业务执行或放松最终账本检查。两个失败目录及原程序均保留。该修正已写入，待重新编译和真实进程验证；现有 `72df1e17…` 完整三轮结果不挪作新程序结果。

修正构建退出 0，新程序 `74a9a37858051175e8647ff3aeef777f5b6c7059030ad165b7a91d15c23abf13` 保存为 `worker-formal-process-head-fixed`，17 项源码清单 `formal-process-head-fixed-sources.sha256` 一致。实际导出分别 **1 / 845**、**1 / 1,123**；Python **22 项**通过，7.026 秒，三个 `*-head-fixed.log` 日志保留。

真实生产 `mixed-process-head-fixed-1.log`、`parallel-producer-9dai9a69/report.json` 五模式全部通过：旧程序、实验关闭、1/2/4 worker；每种十六笔、三十二根动作全部入块及最终确认，九段相邻 slot 均 0.5 秒。开启模式精确两次采用、十四次冲突回退；同模式旧全验证节点的区块、全状态及 SHiP 输出一致，旧程序强检查重放通过。每次 HTTP 提交的十 action / 八 call 原始响应单独保留。各模式使用独立新创世，所以**不能跨模式比较状态哈希或 SHiP 文件字节**；比较发生在同一链的生产者/新验证者/旧验证者之间。本有限负载不是 TPS 容量测试。

恢复首轮 `mixed-recovery-process-1.log`、`parallel-process-dsvb19lr/report.json` 未全部通过：2 worker 的快照恢复进程在启动时记录 `exceeded file system configured threshold` 后正常退出。此时文件系统可用 1,072,435,200 bytes，低于原 1 GiB 阈值；没有调整节点磁盘保护。之前已记录的逐块验证、重启、强检查恢复及坏日志拒绝仍保留，但不作为完整 55 阶段通过。测试已停止自身节点，下一步校验并压缩三份本任务历史 ELF，保留原失败目录、全部日志和链数据，然后以同一程序/语料重新完整执行。

三份历史程序已无损压缩为各自 `.gz`（0600），先核对原 SHA-256，再做 `gzip -t` 和解压流哈希核对，最后只移除对应未压缩 ELF：`parallel-mixed-return-scope-20261014-2PI1SZ/wasm-asan-current`、`parallel-mixed-pool-20261014-KdE3jF/wasm-asan-tested` 与 `wasm-tsan-tested`。压缩哈希分别为 `278559c6432d80f12767573cc2524620cd30d0721fcf8ec51092cbfd9919dc43`、`95621a4fb82715e64ed37e4b488406f1ba6a691c8708d5f8028b8ae5b00807bb`、`35434d9d4a5ad9cd8ddbeb69bb4ec4936376140b258fd0ae561661b8b9174c4b`；原身份不变，`archive-historical-{1,2,3}.sha256` 可核验并恢复。净释放 **776,023,067 bytes**，不与测试/构建同时压缩。

随后同一程序/语料完整恢复 **55 / 55 阶段通过**，见 `mixed-recovery-process-after-archive-1.log`、`parallel-process-8vcneqdj/report.json`。包含五模式逐块/重启/SIGKILL 后脏状态拒绝及强检查恢复、两种坏日志拒绝、快照恢复与尾部接续、开启后关闭并行不转换状态、六节点混合版本 P2P、三块真实竞争分叉的撤销及继续。五份快照哈希相同；新 1/2/4 worker 的正常业务段各采用 16、回退 32，竞争分叉段各采用 4、回退 2。独立 `check-mixed-reports.py` 核对五模式生产报告与完整 55 阶段恢复报告通过，不把磁盘中断的部分结果相加冒充一轮完整成功。真实重复轮次仍待做；当前开始默认 100 档普通 worker、核心、实际 token 各固定三轮及 21 组原兼容测试。

随后普通固定三轮全部结束，`formal-head-normal-{1,2,3}.log` 每轮 **269 / 26,757**，108.34 / 106.41 / 109.21 秒。核心 `d2091b91…` 的 `formal-core-{1,2,3}.log` 每轮 **63 / 12,378**，90.59 / 91.60 / 92.90 秒；排除的一个 benchmark 入口不是本次性能验收。当前 token `ea1bd149…` 的三个原场景每轮 **3 / 1,471**，7.41 / 7.71 / 7.71 秒。兼容 `93c09e85…` 的 21 组 CTest 全通过，78.06 秒，`formal-compat.log`。上述固定轮次不是直到通过的重试，默认 100 档，无编译/节点/压缩并发。源码 17 项及五份 ELF 的测试后哈希一致；访问审计 `formal-head-inventory-check.log` 精确匹配。此证据属于当前 `74a9a378…`，不需要借用之前的完整结果；旧失败仍不因此删除或宣称根治。接着重建六个相邻目标并运行七类入口，随后重建当前内存检查程序。

六个相邻目标重建退出 0，`build-formal-adjacent.log`；七类入口固定三轮全部通过，`formal-adjacent-{1,2,3}.log`，七份程序的 `formal-adjacent-programs.sha256` 前后匹配。覆盖状态视图、串行参考、SoftFloat、同步资源池、OC allocator 失败清理（仅 allocator，不是完整 OC 并行）、原生 VM guard，以及同父子模块超时后复用。

开始当前 ASan/UBSan/LSan 目标的单任务构建，未开始新插桩测试。先以运行库检查确认三个旧 ELF 为 TSan，记录 `pre-formal-tsan-runtime.json`；将其移动为本目录 `{wasm,native,pool}-tsan-before-formal`，保留旧 TSan 对象目录链接为 `{wasm,native,pool}-tsan-object-link-pre-formal`，恢复 m2a 留存的 ASan 对象链接到构建目录并重新配置、强制重新链接。配置日志 `configure-formal-asan.log`、构建日志 `build-formal-asan.log`。构建后还须检查新 ELF 的真实运行库和哈希，不能以 CMake 选项当作插桩证据。当前没有新内存检查通过结论。

### 当前 LRU 内存检查结果（2026-10-15）

`parallel-mixed-lru-20261015-TlEO9d/build-lru-asan.log` 三目标构建结束，无编译错误；WASM 程序为 `997122aa34a16766c50ddc7ffbea5f32531207d456a21d8743cf81447709422c`，native / pool 与上轮相同。`lru-asan-runtime.json` 确认实际链接 ASan 和 UBSan，不将动态链接确认当作全链插桩证明。新 WASM 归档 `wasm-asan-lru-initial.gz`，压缩包 SHA-256 `815e34c1cf34b26e42988155a69a65b03e66c9015dd6d42eeba6ec646abdfbe3`，完整性与解压流程序哈希均已复核；旧失败程序仍保留。

固定要求三轮的 `lru-asan-three.log` 退出 8，总耗时 222.88 秒。50 档、无亲和性、无重型任务并发，原 50ms/500ms 及断言不变：native 三轮各 **63 / 90,619**、pool 三轮各 **15 / 1,072** 通过；WASM 第一轮 **278 / 283、28,843 / 28,853**，后两轮依 until-fail 未执行。失败为一项原多根同步业务返回用例，以及混合共享游标/冲突尾、返回作用域、独立批次、递归四项用例；共十条采用/回退数量断言，未报告原状态/轨迹差异或 ASan/UBSan/LSan 错误。七项新增 LRU 用例通过，不表示时序问题全部解决。源码 17 项与三个 ELF 在运行后复核一致，随后才应用独立 G6a 源码。

源码复查另发现准备阶段 `checkpoint` 先用 `cancel.stop_requested()` 抛中断；生产取消对象也含本地 50ms 截止，因此可能在递增 preparation deadline 诊断之前退出。该观察解释了诊断存在盲区，但还不是上述每次失败根因的实测证明；后续应单独补计数与取消/预算边界测试，不更改原截止或将失败改判通过。当前停止无差别重跑同一完整套件，继续独立 G6a 基础实现与验证。

### 当前 ASan 失败基线及混合复用实施

单任务构建成功，WASM 插桩程序 `0997b49d88289f32dfa4757fff76f46e402021ba7017e31ce79351544c79b092`；native / pool 分别为 `700169c6…` / `4c671787…`。`formal-asan-runtime.json` 确认三个程序的 ASan/UBSan 动态运行库，实际编译/链接参数另存 `formal-asan-{wasm,native,pool}-{flags.make,link.txt}`。三个 ELF 已无损压缩为 `{wasm,native,pool}-asan-formal-initial.gz`（0600），逐个通过完整性及解压流 SHA-256 校验；压缩哈希分别 `c6f44b43fee774d667a7c8ce3748b172e24185c061fa1add2a7224d567622232`、`0f8d0bf36350286e8159b7e930142f2f44c85e1f6786a31c3767b53a8f1de854`、`9cee404e900fbd8e8993ae8d6e226eca4b07ffd2d2a3cb30d75c48a043075e8e`。

`formal-asan-three.log` 使用预先声明的 50 档、无亲和性，ASan leak detection/UBSan halt 开启，不与编译、压缩或节点并发。native **63 / 90,619** 与 pool **15 / 1,072** 各三轮通过；WASM 首轮 **272 / 276、28,367 / 28,379**，CTest 退出 8，until-fail 的后两轮没有运行。失败为混合冲突尾部、独立全采用、原权限/RAM 失败后复用、递归 CPU 计费四个入口的采用/私有成功数量，共十二条断言。多个失败上下文明确为私有成功 0、worker deadline 1～4；原状态/完整轨迹/撤销对照未报告差异，也没有 ASan/UBSan/LSan 错误报告，仍不是完整通过。源码 17 项和三个程序前后核对一致，原失败不删除、不降低断言或放宽 50ms/500ms 预算。

后续证据目录 `build/local-tests/parallel-mixed-lru-20261015-TlEO9d/` 保存三份改造前源码。开始落实下面的混合专用复用设计：只替换 mixed executor，旧 pure-sync 深度槽不变；九个 live+dormant 的合计上限、独占活动深度、空闲精确 key 优先、无空位才淘汰最久未使用且未租出的模块。新增七组冷/热精确计数、九项 LRU、全部 VM 限制与当前元数据、九个活动递归实例、失败构建、1/2/4 worker、start/内存/全局变量及异常/超时后复用测试。源码已写，尚未编译；不先声称它已修复全部时序问题。修复后再构建线程检查和重复真实进程，避免将即将被替换的旧 ELF 检查冒用为新实现验收。

有界复用首版普通构建成功，`parallel-mixed-lru-20261015-TlEO9d/build-lru-normal.log`；当前 worker `6468eb518545ca1230984162a50f35a74479e4b49888bf0326af7f2228051a54` 保存为 `worker-lru-initial`，17 项 `lru-current-sources.sha256` 与构建后源码一致，编译/链接参数另存。`lru-seven-first.log` 的七组 **7 / 474** 首轮全部通过：固定双根/通知/两类 inline/同步组合冷态总共编译 **5** 个模块，后三次完整执行总计仍为 5；九槽 LRU 淘汰、全部七项限制、元数据不借缓存越权、九个同时存活递归模块、失败构建后复用、1/2/4 worker 独立缓存以及 start/全局变量/内存重置均通过。这是固定夹具的准确编译次数，不是 TPS 或真实节点性能结论。

访问审计初次只读检查识别出两份预期变化文件，日志 `lru-inventory-check.log` 保留。人工核对仅源码哈希与既有调用行号移动后，以最小补丁更新两项清单；`lru-inventory-check-updated.log` 完全匹配，没有新增 host 或数据库访问准入。原 nodeos 哈希仍为 `e28d8313…`。开始同一程序的 276 项普通固定三轮，默认 100 档、无亲和性，不与编译或压缩并发；逐轮保存状态，不用反复重试至通过来代替样本。

有界复用版完整普通固定三轮随后全部通过，`lru-normal-{1,2,3}.log` 每轮 **276 / 27,231**，耗时 **111.30 / 110.63 / 108.39 秒**，`lru-normal-three-status.txt` 三轮退出码均 0。程序和 17 项源码在测试后复核一致；此前四个混合正式失败入口在本轮普通构建全部通过，仍不能代替插桩验证，也不据此撤销旧冷叶帧区间失败。

继续构建当前 ASan/UBSan：先复核旧三份 ELF 哈希，验证旧 WASM 压缩包完整性及解压流仍为 `0997b49d…`，才仅移除构建目录的该旧未压缩 ELF（414,678,184 bytes）；归档、源码、日志及链数据不动。它可从 `parallel-mixed-adoption-20261014-ECzQQD/wasm-asan-formal-initial.gz` 完整恢复，归档清单另存 `before-relink-asan-archive.sha256`。操作后 tmpfs UID 配额余量为 691,260,416 bytes，为新链接留出空间。当前同一个 `-j1` 构建的日志为 `lru` 目录下 `build-lru-asan.log`，CMake 保持 `address,undefined`；新 ELF 运行库/身份和测试结果须在构建后确认，当前尚无本版内存检查通过结论。

### 混合模块复用优化的依据（已实现，完整验收待继续）

首轮真实生产记录的 `parallel_compile` 差值：1/2/4 worker 分别 **240 / 256 / 288 次**，计入数据发现阶段；其包含耗时分别 0.021437616 / 0.036698389 / 0.111067761 秒。它们不是壁钟吞吐、不是全链时间占比，也不证明编译是唯一瓶颈。源码 `leased_sync_modules::acquire` 按深度固定槽，混合普通接收者均位于 depth 0，依次切换代码会相互逐出，其他空闲深度槽不能复用。

下一步拟**仅混合执行器**使用至多九个模块的闲置复用池：活动深度单独登记；精确代码与所有 VM 限制匹配，不能复用任何仍被父帧占用的实例。优先闲置匹配，再空槽，最后淘汰最久未用的非活动槽；先释放被替换模块再编译，失败不发布新键。析构归还不分配、不抛异常，LRU 使用固定容量内的有界序号。原单根/多根纯同步深度缓存暂不改；全部内存/深度/截止预算不扩张。缓存不能替代当前协议、权限、输入和代码依赖证明。

实施前后需有精确冷/热编译次数、递归同时存活的独占 VM、九槽淘汰、全部键变化、编译失败及 trap 后复用、跨线程拒绝、原 trace/全状态和真实进程对照。旧冷叶帧区间不重叠及插桩 deadline 失败仍独立保留，不能以此优化宣称它们已经解决。G6 仍需五类二级索引及一般 host：本次只读复查还确认 legacy pointer 的**对齐直接访问/未对齐临时拷贝**会改变重叠输出的实际写回顺序，必须用原 WASM 覆盖该区别，不能统一 memcpy 输出而改变原成功语义。

## 第一批：只读当前状态证明

新增 `validate_isolated_mixed_tree_current_state`：在 owner 上核对 sync-call 协议激活、全部普通根、完整有序代码目录（包括未执行账户和真实 sync-supported 位）、全部 VM 限制和协议参数、每个普通接收者的动态 native-handler 组合、实际 packed CF 数据、冻结混合日志，最后检查当前账本的精确读取依赖。即使没有 CF 子动作，空 CF 输入也必须有显式捕获能力；交易 ID 相同不能替代实际 CF 内容。CF 捕获超出本地预算返回不适用，但调用方检查点在捕获异常处理之外，不吞掉取消异常。

将既有混合步骤指纹编码提取为共享函数，worker、冻结证明和后续采用器使用同一编码；新增旧编码逐步骤对照，避免三份几何信息编码漂移。没有修改编码版本或原链业务行为。

新增七组 owner 测试：全部目录/限制及未用代码；当前代码、VM、特权和协议变化；全部根输入/同 ID 的 CF 替换与原 unsigned 拒绝；无 CF 子动作时的缺失/显式空能力；精确行值、payer、长度、对象身份与表元数据；结构篡改；每个检查点的普通异常及预算异常传播。这里直接种入的行仅用于只读 owner 边界测试，不能当作真实生产/验证语料；正式语料仍须通过原签名动作创建状态。

七组 owner 进入独立 CTest 和默认完整 worker；TSan 继续明确排除这些未全量插桩的 controller 用例，保留私有新指纹对照。编辑时纠正了过滤串中的路径笔误，并与 m2a CMake 快照对照，确认仅增加新 owner 套件排除，没有放开旧排除项。编译前源码检查修正了测试中重复终结 SHA encoder、调用不存在的 shared-string resize，以及把 const 交易传给可变 tester 入口的问题；没有声称这些测试已运行。

访问清单仍为 801 文件、262 个 host；增加一处 owner 的 `control.db()` 只读视图，state_accessor 候选 211 → 212，其他候选种类/数量不变。只更新五个相关生产文件条目和该计数，精确审计匹配。**上述源码/测试已写，尚未编译验证。**

首轮普通构建的链核心完成编译，在新 owner 测试处报一条类型错误：限制字段为 `std::chrono::milliseconds`，篡改测试却尝试直接加 `microseconds(1)`。原构建日志 `build-owner-normal.log`（退出码 2）及 `source-owner-initial/` 保留；仅将这项测试篡改改为 `milliseconds(1)`，继续单个 `-j1` 增量构建。没有修改真实限制值、生产预算或期望断言，尚无新测试通过结论。

## 只读阶段实测与下一批源码

修正测试类型后普通构建退出 0。精确程序 `b7f0270e0d4853acaaa4b827062fbfeedeae6ef9abbe0a7e12c61e974e7ad141` 保存为 `worker-owner-initial`；当前状态校验三轮各 **7 / 776**，混合私有／冻结／工作池／原串行专项三轮各 **33 / 3,101**，均通过。`owner-mixed-three.log` 保留详细结果，`owner-compile-fixed-sources.sha256` 前后核对一致。这里只运行上述专项，不将 m2a 完整或插桩结果移作本版证据。

随后开始正式接入源码：独立 mixed replay 能力、原同步 host 消费钩子、逐 receiver 独占 CPU 加全部子调用独占 CPU 一次计费、undo/reset 清理、apply 的空代码和普通接收者路由，以及 controller 的 tree/sync → mixed 模式升级、拥有最终输入与生产/验证全部入口。原同步 root-only adopter 不放宽；各重放能力互斥；原始授权、RAM、receipt 和 post-event 异常仍走原路径。组内最近同步返回不清空，后段 false 保留已正式执行的前缀和队列。

新增真实签名动作建表的原生 Savanna 测试语料，覆盖 0/1/2/4 worker 的生产及串行/并行验证、完整非计时 action/call trace、两种 inline 队列、共享游标、冲突尾部、通知/inline 首次发现同步、负状态与空代码返回生命周期；另通过仅测试 TU 定义的窄 friend 安装 controller 同类指针（不安装 replay guard），在原通知事件后故意破坏未来步骤指纹，逐个检查前缀采用后回退。该边界测试不代替签名/授权证明。**本批正式接入源码和四个测试入口已写，尚未编译运行。**

## 尚未完成

扩充版三轮专项随后全部通过：private 三轮各 **33 / 3,101**、owner 三轮各 **7 / 776**、正式采用三轮各 **8 / 2,881**，总计 66.88 秒。日志 `formal-expanded-mixed-three.log`，测试后源码／程序哈希再次一致，编译 flags/link 单独保存。没有把上一版完整 257 项三轮挪作本版 261 项完整通过，也没有将普通程序当作插桩程序。

扩充版构建退出 0，精确程序 `314102e40cb5ab2d0f2940b76c56c2947bf85c70cb9b4d2a74392b184f80458f` 保存为 `worker-formal-expanded-initial`。首轮八项 **8 / 2,881** 全通过，17.60 秒，见 `formal-expanded-first.log`；包括全部新增元数据／区块归属、CPU 和失败复用断言。源码快照及 `formal-expanded-initial-sources.sha256` 保留；开始混合 private／owner／adoption 三项各重复三轮。此前 257 项完整回归属于上一份测试程序，本版完整注册已增至 261 项，不能直接记为其完整通过。

2026-10-15 本版首轮后：普通完整三轮均为 **257 / 22,727**，耗时 91.15 / 89.26 / 88.88 秒；`formal-normal-three.log`、程序及源码哈希核对完成。原 nodeos 哈希仍为 `e28d8313…`，生产访问清单精确匹配。上述只证明 `worker-formal-initial` 的普通范围，插桩及真实进程尚未验证。

随即扩大测试（生产实现暂未再变）：新增四笔无写冲突混合交易的全量采用、非显式计费下每个采用前缀的精确 CPU 和重复／跨模式计费拒绝、原 inline 授权及普通动作 RAM 付款授权失败、完整采用后 transaction-final RAM 超限与同代码／同链／同工作池健康复用。轨迹对照补齐稳定的交易／区块元数据；producer_block_id 按生产尚未知、验证须等于实际封块 ID 分别检查。本批增加四个测试入口并扩充旧四项，**源码已写，尚未编译／运行**。

仅 ASan/UBSan 完整 WASM 套件的外层 CTest watchdog 从 180 秒改为 600 秒：旧版尚未增加本轮用例时已耗时 163 秒。普通、私有 TSan 和单个采用专项时限未改；生产 50ms、私有 500ms、正确性断言和强度档位全都未改。这不是扩大真实执行预算，也不抵消旧版时序失败。

2026-10-15：正式接入首版普通构建退出 0，未出现编译错误；精确测试程序 `b0a04531e75b3157abd1e4ebf190119f9890eda25d6bb3b8283867ffddcf359c` 保存为 `worker-formal-initial`，构建前后 14 项源码哈希一致。新增四个正式入口首轮 **4 / 506** 全通过，10.91 秒，日志 `formal-first.log`。包含六类场景在 0/1/2/4 worker 下的生产及串行／并行验证，以及两类返回缓冲生命周期的逐步骤晚回退；它们是当前普通程序的实测结果，不是本版插桩或真实 nodeos 进程证据。接着运行默认强度 100、无 CPU 亲和性限制的完整普通回归三轮，尚待结果。

补充统计边界复查：`adopted` 是正式执行结果被采用的次数，并非最终接受交易数。原测试 `parallel_validation_tests.cpp` 已明确覆盖“完整采用后被 transaction-final RAM 检查拒绝”，此时采用计数可为 1，但没有 receipt/入块且账本撤销。本轮保留该既有语义；混合路径还需补同类尾端失败，G7 吞吐必须统计实际入块／最终确认交易，不能用该计数代替。

首版编译、四项正式入口和完整普通三轮已通过；扩充后的八项正式入口首轮与三轮专项均通过。深层递归、只读继承、删除导致祖先游标失效及取消等组合还需补齐正式路径覆盖。本地计时和异常栈不能要求字节相同，producer_block_id 的差异已按真实生产／验证时机检查。当前源码的真实节点/恢复和插桩测试尚未完成，不能验收 G5。G6 二级索引/一般 host、G7 性能/稳定性及已记录的回退时序失败继续处理；默认并行关闭。
