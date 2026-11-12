# PX7 G5c3c3b：多根同步正式接入（实施中）

## 当前状态（2026-10-14）

从已验证的私有基础检查点 `3856ab5` 继续。证据目录 `build/local-tests/parallel-sync-multi-adoption-20261014-hTf9RI/`。普通完整回归默认 100 档、核心/当前 token、七类相邻入口各三轮通过，21 组兼容通过。新多根真实出块和 55 阶段恢复各三轮通过，五类旧业务五模式回归通过。用户授权的 50 档私有 TSan 三轮通过；同档 ASan/UBSan/LSan 默认及物理核两组三轮各前两轮通过、第三轮同步采用计数因 worker 截止失败，专项也保留一次截止失败。无内存错误报告不等于整套三轮全通过，c3b 的插桩计时稳定性仍未验收。默认实验开关关闭，未修改经济/治理、原 nodeos、main 或基线。OC 仍关闭，通知/inline/CF 与同步混合执行属于后续阶段；不宣称整个分支完成。

## 本批改造

- `parallel_sync_call.hpp` / `parallel_worker_pool.hpp/.cpp`：增加拥有多根输入的第五种互斥任务和交易级输出，成功耗时为所有根及子帧独占时间之和。整笔 16 根、64 次调用、256 KiB 输入/代码等边界不变；失败不发布成功前缀。旧单根私有 API 保留用于回归。
- `parallel_ordered_batch.hpp`：接受新的交易结果类型，仍绑定批次身份、完整指纹、冻结副本和原顺序，不把协调器回调当授权。
- `controller.cpp`：实际普通根触及同步 host 后，一次有界升级为交易级同步任务；正式单根也使用同一交易消费路径。owner 捕获目录、名单和全交易原始依赖，仍共享原整批绝对截止，不给发现重试重新领预算。
- `parallel_canonical_adoption.hpp/.cpp`：第一根前核验所有根、目录、协议、完整冻结 journal 与当前原始读取；后根不得把已合法修改的前缀状态再次与初始快照比较。每根建立短生命周期的 replay 能力，局部调用序号归零、写日志区间保留绝对位置；经过原同步上下文、主键 host、授权、RAM 和撤销路径。
- `apply_context.cpp` / `transaction_context.hpp/.cpp`：原普通根逐个消费，最后一根成功才标记交易采用。CPU 绑定实际根序号并防重复。当前根重放前失败仅可回到当前及后续原执行，不能重做前缀；开始当前根重放后的错误继续原整笔 undo。reset 清除整个能力。
- `parallel_sync_call_tests.cpp`：新增五模式切换、跨根依赖/生命周期、限额、晚失败/取消及健康复用、身份/乱序/重入、真正叶 WASM 重叠、当前链证明变异、不同合约根、普通读根穿插、负业务状态、后根 RAM 授权失败、错误签名/重复交易/abort 后复用等。旧“所有多根均回退”测试只对符合能力的分支改为要求真实采用，未审查子代码仍必须回退。
- 生产/恢复导出器保留原单根入口，增加独立多根入口和语料类型。多根生产夹具每笔两个根，同一行、不同根 nonce，明确核对每根自己的输入与局部调用。恢复使用真实递增值，分叉切换须撤销两次写入。
- `parallel_producer_process_test.py` / `parallel_execution_harness_test.py`：独立互斥的 `--sync-multi-root-fixture`，逐根复用原同步状态/输入检查并另验真实序号和回执顺序；增加 29 类双根 trace 变异和 5 组非法参数组合。`harness-first.log` 20 项通过（5.187s），仅工具自检，不运行节点。

## 验收顺序与边界

源码清单保持原紧凑格式，只更新实际变动的 11 个文件；仍为 798 文件、262 个 host 注册、114 个含候选访问的文件，无新增 host。词法计数只多一个 owner 当前状态证明的 `db()` 访问（state_accessor 210 → 211），不是工作线程获得账本权限。`access-inventory-check.log` 精确匹配通过；格式化扫描输出与紧凑清单直接文本比较曾产生纯格式的大差异，未把该差异套用到源码清单，而按 JSON 结构核对变动后再更新。`build-normal-first.log` 中新私有工作池库已构建成功，控制器及完整测试目标仍在构建中。

同一次构建随后完成控制器、正式采用、交易/动作/同步上下文及其依赖，成功链接新的 `libeosio_chain.a`，没有截至此处的编译错误；开始编译测试目标。此时节点和 worker 的旧可执行文件尚未被新链接替换，不能运行它们冒充本批验证。

首次完整构建在新测试的 `BOOST_REQUIRE_MESSAGE` 处退出 2：消息参数的三元表达式缺少括号，与 Boost lazy stream 宏展开的 `<<` 优先级冲突。保留 `build-normal-first.log`、`compile-failure-source.diff` 和准确测试源码 `compile-failure-sync-tests.cpp`（SHA-256 `6ef26516543b4c664964d2f4ee93d57b0cbf19832e0eebf054c4666692492048`）；等待构建完全退出后仅给消息表达式加括号，未改测试条件。修复构建使用独立的 `build-normal-macro-fix.log`，不覆盖失败日志。

修复构建成功，首版 worker SHA-256 为 `148e5ae9040212410774c533c27d0c6a9c97882bdf2551344fa55c259eb92312`。同步完整专项 `sync-first.log` 首轮通过：77 / 207 入口被选择，130 跳过，8,005 / 8,005 断言，54.96s；其中四个导出入口在未设置相应环境变量时直接返回，不把这些计为真实生产/恢复语料测试。新增正式多根顺序、普通读根穿插、跨合约 sender/返回值、业务负状态保留写入、后根 RAM 失败整笔撤销及健康复用、错误签名/重复/abort 防护均实际运行通过。开始完整 worker 与同步专项各三轮回归，插桩与真实节点尚待本版验证。

`normal-three-rounds.log` 随后全部通过（总计 379.59s）：worker 三轮各 207 入口 / 17,945 断言，72.01 / 71.79 / 72.05s；同步专项三轮各 77 入口 / 8,005 断言，54.27 / 54.92 / 54.52s。无亲和性限制，不与构建/压缩或其他重负载测试并行，未更改原尝试预算、断言或压力循环。四个导出入口仍须单独设环境变量实际生成语料；本项不是生产 TPS 测量。

本轮正常版归档 `normal-first-artifacts.tar.gz`：权限 0600，12,901,567 bytes，SHA-256 `1a5818a731549f724634990af7384bbafb191cb607d171065e5042f74687db02`；包含准确 worker、编译/链接参数、改动源码、基线和源码差异、宏失败原件及修复后测试日志。`tar --compare` 和归档 SHA 复核通过。随后仍以单个 `-j1` 构建 ASan/UBSan 检查版本，构建期间冻结源码，不使用尚未重链接的旧插桩程序冒充本版验证。

先检查源码清单、单个 `-j1` 构建，新增用例和原同步正式路径定向初验；失败必须保留准确程序/日志，再修复重测。随后完整 worker/同步、核心/token、兼容和相邻重复回归，核验实际 ASan/UBSan/LSan 与私有 TSan 程序身份后运行插桩。最后使用新节点和新多根语料跑真实生产及恢复，并回归原单根和其他调用树入口。

私有协调器回调只是排序探针；正式接入由真实 controller 差分验证。叶帧时间区间只证明执行重叠，不证明 TPS；HTTP submission trace 与 SHiP 不能替代完整 call trace 的链级对照。后根防御回退的返回边界已用下面的诊断注入补证，实际分配器失败本身未注入，不以晚 RAM 失败或私有失败冒充该边界。c3a 保留的默认 TSan 旧时序敏感性仍在，不通过放宽预算、删断言或隐藏失败来验收。

## 后根 false 返回边界：独立调试器故障注入

不向生产代码添加测试回调、不暴露私有 replay 指针。使用本版正常 worker（`148e5ae9…`）及原本已通过的 `multiple_contract_roots_share_writes_but_not_call_ordinals_sender_or_returns`：三个根通过同步子调用将同一余额 10 → 13 → 17 → 22；核对每根返回值、sender/ordinal、完整非计时 trace、回执及原串行/并行验证的完整性哈希。

`late-root-false.gdb` 只启动新的隔离测试程序，不 attach 已有节点。在公开采用包装函数**尚未执行当前根**时，指定一次返回 false，随后禁用断点；前根的正常采用已完成。未修改二进制文件、授权、截止时间或任何原断言。无注入基准先在调试器下通过 166 条，确认 18 次入口顺序（1/2/4 worker 各 3 个生产根、3 个验证根）；据此覆盖每种线程数、生产/验证、根 2/根 3，共 12 个位置。

每个注入位置的原 Boost 程序退出 201，恰好是**一个预期的采用计数断言失败**（整笔不再算并行采用），其余 165 条全部通过。不能把这些原程序日志写成“Boost 全通过”。独立 `verify-late-false.py` 严格验证 XML：只有对应生产或验证的 `[0 != 1]` 计数失败，线程上下文正确，没有额外错误/异常/告警，指定入口只注入一次，`late-false-summary.json` 为 `expected-fault-effects-verified`。这证明 late-false 后继续当前根及后缀、不会重跑前缀；不声称实际分配器 OOM、线程检查或真实节点进程故障。

首次调试器基准在平台校时的 `SIG34` 停下，退出 255、目标命中 0，XML 为空，不计测试成功。核对 `platform_timer_posix.cpp` 的 `SIGRTMIN` 定时器后，让调试器正常传递此信号，保留 `late-root-false-before-timer-fix.gdb` 与首次原始日志；`baseline-v2` 随后通过。没有禁用链本来的计时器/超时机制。注入全部完成后，程序 SHA 再核对不变，脱离调试器将同一测试重跑三次，各 166 条全部通过。

ASan/UBSan 三目标构建已成功，实际动态运行时身份检查通过；新 WASM 检查程序 SHA-256 `5f02da36fbc6d13a0268bb2ba102a508f706bbee33ea24e3d3813642b3bff63e`，相邻 native/pool 程序内容未变，仍为 c3a 已记录的哈希。运行时身份不代表整个 controller/Chainbase/FC 全量插桩。

## 默认调度 ASan 初验：保留失败，未验收三轮

`asan-three-rounds.log`：启用泄漏检查及 ASan/UBSan halt，无亲和性设置、无同时构建/压缩。native 三轮各 63 / 90,619（2.40 / 2.32 / 2.35s），pool 三轮各 15 / 1,072（0.30 / 0.29 / 0.29s），三个程序运行时身份前置检查各三轮通过。

WASM 第一轮 214 / 19,567 全通过（152.38s）；第二轮 213 通过、1 用例 abort，19,550 / 19,551 断言（152.46s）。唯一报告为未改动的 `parallel_action_tree_pool_tests/tree_workers_overlap_real_wasm_intervals_without_sharing_writes`：4 线程、job 1、`failure=deadline`、`attempt_wall_us=500560`，触及原 500ms 预算。无 ASan/UBSan/LSan 错误报告；新增多根功能用例均通过。CTest 退出 8，总计 320.39s，until-fail 导致 WASM 第三轮未运行，不能记为全套三轮通过。

保存 CTest 原始临时结果为 `asan-default-first-ctest-last.log`、`asan-default-first-ctest-failed.log`，继续保留准确程序/源码/运行时身份。核对拓扑是 4 物理核、8 逻辑核（0/4、1/5、2/6、3/7 同核），后续使用原程序分段诊断和受控物理核对照；未改压力循环、500ms 截止或采用断言，不把定向/条件通过写成默认调度风险已修复。

默认失败归档 `asan-default-first-artifacts.tar.gz`：0600，71,960,029 bytes，SHA-256 `56531a853ea7fe0a61879d59be2350244ff8b5915e6ba58dc78557f530227f53`，包含三个准确程序、解析真实位置后的编译/链接参数、当前源码和基线、原失败与 CTest 临时日志；tar 对比和 SHA 复核通过。

同一程序、无亲和性限制、开启原有诊断计数，将失败用例单独重跑三次，各 66 条通过。四线程阶段批次 wall 分别 395,088 / 371,696 / 358,407μs；编译 8 次共 16,262 / 17,741 / 14,839μs，24 次执行累计 1,374,116 / 1,335,851 / 1,342,537μs。执行累计包含并行重叠，不能拿来推算 TPS；这只是诊断运行，不能覆盖原全套失败。保留原 50,000 次压力循环、原 500ms 上限。

随后仅给本次 CTest 及子进程设置 `taskset -c 0-3`，重新运行三组插桩各三轮，输出 `asan-physical-three-rounds.log`。核对实际 WASM 子进程亲和性确为 0-3，见 `asan-physical-child-affinity.txt`；未修改宿主全局调度策略、功耗、温控或已有节点设置。该受控对照全部通过，CTest 退出 0，总计 478.86s；WASM 三轮各 214 入口 / 19,567 断言，153.95 / 154.72 / 154.52s，native/pool 及三类运行时前置检查也各三轮通过。未报告内存、未定义行为或泄漏错误。此结论限定本次插桩范围及物理核亲和性条件，不能替代默认调度失败，不证明全链已插桩或默认时序敏感性已修复。

最终内存检查归档 `asan-artifacts.tar.gz`：0600，71,985,865 bytes，SHA-256 `f5bfc7616f9522095d0dd5d075a0e0c1dff1ae1893fdcad4802b64cf6f509b63`，含默认失败、定向诊断和物理核对照原始日志、准确三个程序/源码/参数。tar 对比与 SHA 校验通过后才切换线程检查缓存；旧内存检查程序及缓存链接均保存在本证据目录，没有覆盖原 nodeos。

## 重新配置暴露的 CTest 注册问题

切换 TSan 的第一次 CMake 配置退出 1，尚未开始编译或线程检查。原因是 Boost 允许在同一文件多次打开 `parallel_sync_pool_tests`，而已有 CMake 源码扫描为每次出现都注册同名 CTest；新增测试把同一套件打开了三次。原失败日志 `configure-tsan.log` 和原构建文件 `configure-failure-unittests-CMakeLists.txt` 已保留。修复只在每个文件扫描出的套件名列表上去重，保留所有 Boost 测试及每种运行时的独立入口，不更改任何断言或链执行逻辑。随后重新配置两次检查重复配置，再继续单个 `-j1` 构建。

修复后的两次配置均成功；CTest JSON 和生成的命令核对确认恰好保留 eos-vm / eos-vm-jit 两个套件入口，均选择完整 `parallel_sync_pool_tests`。完整 `unit_test` 尚未构建，因此该项只是注册检查，不是运行上述两个运行时测试；首次临时检查尝试读取 CTest JSON 中不存在的 `command` 字段，随后改查生成的 `CTestTestfile.cmake` 命令，不借此声称执行成功。源码审计工具另跑 24 项通过（4.313s），插桩身份检查工具 9 项通过（0.011s），均为轻量工具自检，不与正在构建的程序混作线程检查结论。

本次重新编译 SoftFloat 的 `s_mulAddF64.c` 出现两条 `sig128C` 可能未初始化告警，原文留在 `build-tsan.log`。沿控制流复查：该值仅在 `expDiff > 0` 分支被读取，同一条件此前已赋值；两段之间未修改 `expDiff`。这是源码分析，不把它写作新增运行时故障，也不通过改依赖算法或关闭告警消除输出。

## TSan 默认调度：首次失败及后续诊断

修复配置后，三个线程检查程序构建成功，ELF 运行时身份检查通过。WASM 程序 SHA-256 `4324ea9761f441c5288dc33fd1c5780d3929451283bda36c50cc39f72fbbce34`；相邻 native / pool 分别为 `448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`、`e7e85ce5745dd3e96fbce9dfbf27cd435b5a6322506af0fea362147a55b423a3`。仍排除混合未完全插桩 controller 的 oracle/正式采用测试，不声称全链 TSan。

`tsan-default-three-rounds.log`：无亲和性设置，启用 `halt_on_error=1`，无同时构建或压缩。运行时身份检查三类各三轮通过；native 三轮各 63 / 90,619（2.40 / 2.27 / 2.22s），pool 三轮各 15 / 1,072（0.69 / 0.69 / 0.67s）。私有 WASM 首轮选择 150 / 214 入口，64 跳过；148 通过、2 abort，10,769 / 10,772 断言通过，45.24s。旧调用树三模式用例在线程 1、round 1、job 5 报 `deadline` / 502,442μs（普通与 fatal 两条失败）；旧实际重叠用例在线程 4、job 1 报 `deadline` / 500,430μs（一条 fatal）。无 TSan 竞争报告。CTest 退出 8，总计 58.21s，私有 WASM 后两轮未运行。

保存准确失败程序/源码/构建参数、配置失败及修复、CTest 原始临时日志后归档 `tsan-default-first-artifacts.tar.gz`：0600，30,347,247 bytes，SHA-256 `844a836c2727a161c522c18988bddda56e782290b78f9e5a26159dde0cda63a5`；tar 对比与 SHA 复核通过。随后开始同一程序的两个旧压力用例三轮分段诊断，保留各轮退出码，不将定向或物理核条件下的通过冒充默认失败已修复。

默认定向三轮均退出 201：首轮三模式用例通过 666 条，重叠用例在线程 1 / job 2 截止 500,646μs；第二轮三模式在线程 1 / round 0 / job 8 截止 502,372μs，重叠在线程 4 / job 1 截止 500,457μs；第三轮三模式在线程 4 / round 0 / job 2 截止 500,710μs，另有一个 notification 结果未产生的断言失败，重叠在线程 4 / job 2 截止 500,336μs。三轮通过断言分别 676 / 677、112 / 115、512 / 516；不称定向三轮通过。

同程序 `taskset -c 0-3` 完整私有对照首轮 150 / 11,339 通过（51.50s），第二轮旧三模式用例在线程 4 / round 1 / job 2 截止 502,182μs：149 通过、1 abort，11,206 / 11,208（49.47s），第三轮未运行；CTest 退出 8，总计 113.22s。无竞争报告，但物理核条件也未验收三轮。原强度全部线程检查证据归档 `tsan-artifacts.tar.gz`：0600，30,369,063 bytes，SHA-256 `f634195247527aedb630aa149aff395cfa13bd96b2ebd8fcded83824a9b9930b`，tar 对比与 SHA 复核通过；原压力测试源码另存 `tree-pressure-original-tests.cpp`（`b14a6dcf3ba34d3e7a971979b7a193f92fb2ec5df28f2b4ba7938873b8d613df`）。

## 用户授权的本机压力档位调整（2026-10-14）

用户明确允许因本机性能适当放宽测试参数。只在 `parallel_action_tree_pool_tests.cpp` 增加测试专用环境变量 `PUREEOS_PARALLEL_TREE_STRESS_PERCENT`，允许 25 / 50 / 100 三档，不设置仍为原 50,000 次；50 档为 25,000 次，25 档为 12,500 次。只减少夹具 WASM 无状态忙循环，不改交易数、线程数、数据依赖、调用树结构、余额/权限/回滚/实际重叠断言；原执行器 500ms 和真实生产批次截止都不变，也不改变 nodeos 的 `max-transaction-time`。非法、空白或零值拒绝，不静默降级；新增 15 条档位解析断言，每次运行输出明确档位标记。

普通回归保留默认 100 档；本机 TSan/ASan 拟显式使用 50 档并标记结果，不将其冒充原 100 档压力通过或性能提升。原强度失败完整保留；正在重编译两个测试程序，尚未获得新档位的测试结论。

两个测试程序随后构建成功：普通 worker SHA-256 `4c3679cccda87e4f92ed787d303571635d7fbb6d7a0bd6ab0db37ffd856fe05b`，TSan worker `538732b1179b6de387537fadd8351a7b5b722144581724dae6f435b8c0ecc26c`，运行时身份核验通过。普通程序的 100 / 50 / 25 档完整树工作池专项各 5 项 / 837 条通过，输出与实际忙循环次数标记一致。另用 75 档启动真实夹具，原程序按预期退出 201 并报告档位错误；这是配置拒绝的负测试，不是正常运行成功。继续默认调度（不设亲和性）的 50 档 TSan 三轮，以及普通默认 100 档完整回归。

`tsan-profile-50-three-rounds.log` 全部通过：私有 WASM 三轮各 151 / 215 入口、64 个混合未完全插桩 controller 用例跳过，11,354 / 11,354 断言（42.98 / 41.20 / 41.22s）；native 三轮各 63 / 90,619（2.27 / 2.42 / 2.39s），pool 三轮各 15 / 1,072（0.66 / 0.64 / 0.63s），三个运行时身份前置各三轮通过。CTest 退出 0，总计 138.30s；无 TSan 报告，无亲和性限制，也无同时构建/压缩。结论限定为用户授权的 50 档压力设置及本次插桩范围，不替代上面的原强度失败。随后启动显式移除档位环境变量的普通完整三轮，须在日志中确认仍为 100 / 50,000。

`normal-profile-default-round-1/2/3.log` 随后各 208 入口 / 17,960 断言全部通过，三份日志均明确为 `tree_percent=100 busy_iterations=50000`，整组退出 0。新增档位解析的 15 条计入其中，原普通回归强度未减；导出包装入口仍需后续设环境变量实际生成语料，不以本项替代真实节点测试。

档位改造后的普通和线程检查准确程序、源码、编译参数及原始日志已归档 `profile-normal-tsan-artifacts.tar.gz`：0600，42,894,551 bytes，SHA-256 `60bb879c3a97d63b59eef27b41a66c7e4123d5873537b6f23f95cae110d2aa9d`；tar 对比和 SHA 复核通过。保留原强度归档及旧源码副本后，才切换回 ASan/UBSan 缓存并重新链接，未拿另一种运行时或修改前程序冒充新档位验证。

## 50 档内存检查：默认第三轮同步截止失败

新三程序构建与实际运行库核验通过；WASM SHA-256 `6c22bd68d6992b88c16a6b8c7b58f9e2eb20297dcaf098985aa0243298f0ae96`，native / pool 仍分别为 `700169c63c9c4ec989682a50c79751e2e348406adedfcd0975829c4c3a4adca8` / `4c6717876b640ca4750cafbec50dcdc6547f36690591cf0bddbe4504b2537f44`。

`asan-profile-50-three-rounds.log` 默认调度、无亲和性、无并跑重负载，开启泄漏检查及 ASan/UBSan halt。WASM 前两轮各 215 项 / 19,582 条通过（150.26 / 148.35s）；第三轮 214 项通过、1 项失败，19,581 / 19,582 条（156.28s）。唯一失败为既有 `same_code_recursion_and_preserved_parent_returns_match_original_contexts` 的一线程生产采用计数 `[0 != 1]`：4 次尝试、0 成功、1 次 worker deadline，无准备截止/预算/其他错误、无 VM 超时。其余状态、完整 trace、原链对照断言通过，无 ASan/UBSan/LSan 错误报告。该正式生产尝试受原整批 50ms 上限约束，不与旧私有树 500ms 压力失败混为同一入口；未放宽任一上限。

native 三轮各 63 / 90,619（2.39 / 2.36 / 2.41s），pool 三轮各 15 / 1,072（0.31 / 0.30 / 0.30s），运行库前置检查各三轮通过。CTest 退出 8，总计 470.67s；三轮都已实际运行，但不能记为全通过。原 CTest 临时结果另存 `asan-profile-default-first-ctest-last.log` / `asan-profile-default-first-ctest-failed.log`，继续同一程序的三次定向计时诊断，不覆盖失败。

`asan-profile-sync-diagnostic-{1,2,3}.log` 随后三次各 454 条通过（每次只选择 1 / 215 入口）；三个子进程均退出 0。只启用现有阶段计时，不改程序、预算或断言。诊断中生产步骤 wall 包括原串行执行及最终采用等工作，不等于纯 private attempt，也不是 TPS。通过只说明定向未复现；原完整第三轮失败不能由此抵消。随后用同一程序和 50 档、仅测试进程 `taskset -c 0-3` 做三轮完整物理核对照，输出独立日志 `asan-profile-50-physical-three-rounds.log`。

该物理核对照也未全通过：前两轮各 215 / 19,582（160.54 / 148.54s）；第三轮 214 项通过、1 项失败，19,580 / 19,582 条（154.11s）。失败是 `recursive_parent_visibility_negative_business_writes_and_full_call_traces_match_serial` 的 4 worker：12 次尝试、0 成功、2 个 worker deadline，采用和回退计数各一条不符；其余完整 trace、状态、原链对照通过。无 ASan/UBSan/LSan 报告。native / pool 及运行库前置各三轮通过，整体退出 8、478.98s。实际子进程亲和性留在 `asan-profile-physical-child-affinity.txt`；原 CTest 临时日志另存 `asan-profile-physical-ctest-last.log` / `asan-profile-physical-ctest-failed.log`。仅固定物理核不能解决该时序敏感性，不能把“无内存报告”写成三轮全通过。

不继续用整套反复重跑寻找通过结果。改为同一程序、默认调度下的完整同步正式采用套件固定三轮诊断（`asan-profile-adoption-diagnostic-{1,2,3}.log`），只开启现有阶段计时，原 50ms 生产上限和全部采用断言保留；专项通过也不替代上述完整套件失败。随后继续普通程序及真实节点的独立验证，默认插桩下的截止敏感性作为待处理风险记录。

该专项第一轮退出 201：22 个选择入口中 21 个通过，3,112 / 3,113 条；新增 `late_root_ram_authorization_failure_undoes_prior_roots_and_reuses_the_chain` 的私有成功计数 `[0 <= 0]`，上下文明确为 5 次尝试、0 成功、1 个 worker deadline，其余准备/VM/其他原因均为 0。原失败撤销、完整 trace 和健康交易复用断言通过，但本次没有命中要求的“私有成功后由原 RAM 拒绝”分支，不能把串行撤销成功冒充这个分支已覆盖。第二、三轮各 22 / 3,113 全通过（193 个其他入口未选择；四个导出包装仍需显式环境变量）。总脚本退出 1，原逐轮退出码和日志完整保留；新用例所需分支已在此前普通回归、完整内存检查通过轮及这两轮中命中，不声称本机所有重复均稳定。

50 档准确三个 ASan/UBSan 程序、源码、运行库身份、默认/物理核完整失败及两组定向诊断已归档 `asan-profile-artifacts.tar.gz`：0600，72,125,503 bytes，SHA-256 `eca0109fa5085b7e01c7d3553bdb3a723747b13b90ebc299b55dedf0f16a82ea`。tar 对比及 SHA 复核通过，原失败未覆盖。随后开始 `build-adjacent.log` 单个 `-j1` 构建实验节点、核心/兼容及七类相邻目标，源码冻结；原 nodeos 校验仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。此处只是进入后续独立检查，未把未稳定的插桩计时验收打勾。

## 本版普通节点与相邻验证

`build-adjacent.log` 随后全部目标成功，退出 0。有一条来自本批未修改的 `parallel_validation_tests.cpp:1475` 经 Boost.Test 展开的有符号/无符号比较告警，与 c3a 已记录的来源相同，不称零告警。新实验节点 SHA-256 `2fe808f6cdd09caff382dcbcab12c939cc93cb1d4059185a646f867bdbbd6969`，核心 `e6e8940c77ab0d174e36bff9515c7c4595bca864abf7d18436c63c2f91423748`，兼容 `1bb65e042742b3a235c17e36955434fc7a17227d971afc9a5ec63db10b273089`；worker 仍为 `4c3679cc…`，原 nodeos 仍为 `e28d8313…`。此时才开始运行本版相邻及真实进程检查，不使用 c3a 的旧节点产物替代。

`tooling-final.log` 六组轻量工具自检全部通过（15.97s），与构建并行运行，但没有同时运行节点或计时压力测试。开始三轮核心及当前 token、21 组兼容和七类相邻入口，输出独立本版日志，结果另记。

`core-round-{1,2,3}.log` 三轮各 63 / 12,378 通过，明确不运行另外一个 benchmark 入口；`current-token-round-{1,2,3}.log` 三轮各 3 / 1,471，通过未修改的当前参考 token（SHA-256 `ea1bd149…`）。`compatibility.log` 21 组全通过（79.48s）；`adjacent-three-rounds.log` 七类入口各三轮通过（共 11.29s），包括状态视图、串行参考、软件浮点、资源池、OC 分配器失败注入、原生 VM 防护及原同步超时后复用。构建仍关闭 OC，分配器检查不是完整 OC 运行验收。整组回归脚本退出 0，未与构建或压缩并跑。

准确实验节点、worker、核心/兼容及七类相邻程序、对应编译参数、源码和日志归档为 `current-normal-artifacts.tar.gz`：0600，69,061,301 bytes，SHA-256 `171762af307764bb3d475d80437fd224567d788e199fe1440a64ceb1f39abf1c`；tar 对比及 SHA 复核通过后，才开始真实节点测试。

## 本版多根真实节点测试

`multi-sync-producer-first.log` / `parallel-producer-4owdwn_o/report.json` 为 passed：旧节点、新串行、1/2/4 worker 五模式各 16 笔、32 个普通根，固定 14s 窗口均获得 16 笔最终确认，初次追赶后的连续槽间隔均为 500ms。三个开启模式各 2 批、80 次实际发现/执行尝试、16 个私有成功结果、4 笔正式采用、12 笔冲突回退；关闭模式无生产批次。逐根提交 trace、原回执顺序、三节点完整性/SHiP 及旧节点强校验磁盘重放一致。HTTP trace 不替代链级完整 call trace 对照，固定有限输入也不是饱和 TPS 测量。随后新导出多根递增语料并运行恢复矩阵，不能把旧单根语料算作这次多根恢复。

`multi-recovery-export-first.log` 实际导出入口 1 项 / 460 条通过，生成 `multi-recovery-corpus/manifest.json`（SHA-256 `97b3e1b7aaf99bb4fef48504beaf2387852f67c383dfdd4a03bb9e6d358c1485`）。`parallel-process-utc20qnh/report.json` 的 55 阶段全通过：非法配置拒绝、逐块强校验、正常重启、SIGKILL 后强校验恢复、截断/缺父块拒绝、关闭并行无需状态转换、快照、新旧 P2P/SHiP 以及三块竞争分叉。1/2/4 worker 正常/恢复/P2P 各 16 批、240 尝试、48 私有成功、32 采用、16 回退；分叉各 4 / 30 / 6 / 5 / 1。两根在同一行的递增和分叉撤销均经过完整状态核对，SIGKILL 不代表物理断电。继续第 2、3 轮新隔离进程，而非沿用正在运行的旧节点。

随后重复两轮也全部通过：多根生产报告 `parallel-producer-oa7v0bad` / `parallel-producer-tnrun9op` 各五模式 passed；同一不可变多根语料的 `parallel-process-s8v3ln5a` / `parallel-process-xh320qku` 各 55 阶段 passed。各轮使用新的独立目录/进程，原首轮证据保留，重复脚本退出 0。由此本版多根真实生产及恢复各完成三轮；继续旧单根同步、CF、普通 inline、代码通知和当前 token 多动作的五模式回归，不把新多根通过替代旧能力兼容性检查。

旧单根同步 `parallel-producer-9cal25vy`、CF `parallel-producer-rrn7eorz` 各五模式通过。普通 inline 首次命令错误地选择 worker 作为导出器，而该入口在 `parallel_validation_test`：Boost 报没有匹配入口，`parallel-producer-kgg4fxou/report.json` 为 failed、已完成节点阶段 0，尚未启动业务节点，不是链执行故障。保留 `legacy-inline-producer.log` 和原失败目录；用实际 Boost 入口列表确认正确程序后，仅修正后续命令为核心导出器，使用新的 `legacy-inline-producer-v2.log`，不改源码或覆盖失败。前两个已完成家族不重复覆盖，代码通知与当前 token 多动作继续顺序执行。

修正入口后普通 inline `parallel-producer-m8bca7bp`、代码通知 `parallel-producer-3lxm0wag`、当前 token 多动作 `parallel-producer-5aqt8aam` 各五模式通过，剩余脚本退出 0。每个模式各 16 笔全部最终确认，token 多动作各 33 个普通根，其余两类各 32 个；旧节点强校验、回执顺序、三节点状态/历史、连续 500ms 槽检查均通过。当前 token 程序和系统合约仓库未改。

独立只读 `verify-process-evidence.py` 汇总检查全部 12 份报告：8 套成功生产矩阵共 40 个模式运行、3 套恢复共 165 个阶段，以及明确保留的 1 份零节点阶段入口错误。逐份绑定原/新程序 SHA，检查同一多根恢复 manifest、模式顺序、根/交易数、最终确认、槽间隔、旧节点重放及同步 4 采用 / 12 回退边界；`process-cross-check.json` 为 `cross-check-passed`。只统计实际运行，不把命令错误当业务通过。`current-code-program-check.log` 核验正常归档中全部程序、源码和构建参数仍一致，仅排除持续更新的两份 Markdown；`git diff --check` 通过。依赖与参考合约工作区干净，main 和带注释基线标签指向的提交仍为 `a4a40a6a1e72c531cd8267758074229ccc0655ce`，原 nodeos 仍为 `e28d8313…`。

最终进程证据归档 `completed-process-evidence.tar.gz`：0600，100,742,959 bytes，SHA-256 `1e7468d69fd10f7dde5fa4c45967536449b7a95fec53e42d4643b2aeabeceecd`。包含全部 12 个进程目录（含入口错误）、不可变恢复语料、原始日志、只读交叉检查器/结果、对应脚本和程序身份清单；tar 逐项对比与 SHA 校验通过。测试私钥仅存在于受限本地证据中，未输出或安装到用户链。确认本批 nodeos 子进程均已退出，没有删除用户数据或改动旧链。未进行新的 Git 提交；本版代码与测试改动仍在当前功能分支工作区。

本次测试批次结束，但不标记 c3b 或分支全面完成：尚需定位 ASan 下整批生产截止的时序敏感性，不能简单用 `max-transaction-time` 覆盖内部 50ms，也不能用“无内存报告”替代采用分支断言。后续混合同步/通知/inline 调用树、G6 通用能力和 G7 负载性能验证依旧按主计划推进；本记录不提供生产 TPS 或完整 OC/全链插桩结论。
