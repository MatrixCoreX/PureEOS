# PureEOS 主线程优化实验台账

状态：原16组矩阵及group8独立复杂资格完成；新增SHA＋group8匹配双组已完成且未采用，当前推进f629dc37路由计数安全回归。已选group8基线保持，5万TPS目标与后续研究未完成。

更新时间（UTC）：2026-10-29T18:37:03.363980+00:00

## 范围与采用规则

覆盖本轮 EPYC 4345P（8 物理核/16 线程/64 GiB）服务器现存机器记录和主线程代码版本。旧机器的历史报告保留在原位置，不将其参数混入本轮结果。完整记录包括准备检查、纯转账筛查、复杂合约稳定性、组件与诊断；没有节点 TPS 的项目明确标记。

成功且稳定的优化才作为最新开发基线；没有明显收益的候选独立保留。当前开发基线为3a3f9361配合完整Native/指纹/trace/提前交接group8；fc7作为历史匹配参考保留。门槛沿用事前规则：固定窗口和整批 TPS 均至少提升 2%，主线程 CPU 时间/笔不升，再完成复杂稳定性和独立串行检查。新夹具参考复核已另记：窗口36,494.75 TPS，整批37,213.30 TPS；不改写历史结果。

默认开发 CPU16=3,200,000 µs/块；本轮实验 CPU32=6,400,000 µs/块。NET32 MiB/块、P2P64 MiB/消息、队列1 GiB、逐笔限制、计费和系统合约模式保持。RAM64 GiB，测量前 setramrate1024。纯转账为 SYS/TKN 混合；复杂轮 90% 转账+10% 支持的复杂操作；禁用 deferred。

单块折算峰值、固定窗口和整批 TPS 分别保留；稳定性轮/诊断/组件数据不得写成纯转账稳定容量。80k offered 的饱和筛查没有达到 offered 门槛，不自动等于节点崩溃或账本错误。

## 测试方法纠正

首轮 ab9 与首轮干净 3f 漏掉 harness/sender 的整进程树 CPU 隔离。原 TPS 保留，历史性能淘汰比较暂缓；相应 affinity-fixed 记录已补全：主线程 CPU0，CPU8 不使用，其余节点/发送/采集线程仅 CPU1–7,9–15；MemoryMax56 GiB、swap0；窗口前后独立验证全部进程线程。

主线程已拆出的工作、空代码通知现有路径及已拒绝的权限/资源预取组件，见 [owner-work-parallelization-and-next-paths-20261029.md](owner-work-parallelization-and-next-paths-20261029.md)。第13组启动前脚本路径错误已保留并修正；此前12组结果没有重跑，详见 [20261029-epyc4345p-combination-prelaunch-path-method-fix.json](20261029-epyc4345p-combination-prelaunch-path-method-fix.json)。

## 按版本索引

| 代码版本 | 改动 | 详情 |
|---|---|---|
| c0cb2cb9f6268271f0ca04b7be0af432953e048c | Server baseline matching verified R14 dce2bb38;identity ac1645f6 | [c0cb2cb9f6268271f0ca04b7be0af432953e048c.md](main-thread-experiments/versions/c0cb2cb9f6268271f0ca04b7be0af432953e048c.md) |
| 1258c1f8fe1f6cf74f4c6de6bc410155ed75b393 | Bind immutable preflight to complete current action bytes;reclaim one bounded immutable batch on worker and drain before next capture | [1258c1f8fe1f6cf74f4c6de6bc410155ed75b393.md](main-thread-experiments/versions/1258c1f8fe1f6cf74f4c6de6bc410155ed75b393.md) |
| 0e05bfcc99570b7b92d8904838a6bcb0fe1b393d | Use current object identity for exact positive read validation | [0e05bfcc99570b7b92d8904838a6bcb0fe1b393d.md](main-thread-experiments/versions/0e05bfcc99570b7b92d8904838a6bcb0fe1b393d.md) |
| b8407259d544a2442e0808b30e96f9b5a5d4481d | Remove bounded hint FIFO shifts and redundant key lookups | [b8407259d544a2442e0808b30e96f9b5a5d4481d.md](main-thread-experiments/versions/b8407259d544a2442e0808b30e96f9b5a5d4481d.md) |
| 4e1852d173e2d1d030b7b0bd547fec3fa6ca77f0 | Reuse bounded raw storage for original per-index Undo session wrappers | [4e1852d173e2d1d030b7b0bd547fec3fa6ca77f0.md](main-thread-experiments/versions/4e1852d173e2d1d030b7b0bd547fec3fa6ca77f0.md) |
| 75a55efddaa671b7f2a104dd94126104e8e82ca8 | Serialize mapped database lifetimes while retaining concurrent session regression | [75a55efddaa671b7f2a104dd94126104e8e82ca8.md](main-thread-experiments/versions/75a55efddaa671b7f2a104dd94126104e8e82ca8.md) |
| 448eb0536875215291615d30b1516d99d8b73570 | perf: reduce owner input copies and recycle empty batch storage | [448eb0536875215291615d30b1516d99d8b73570.md](main-thread-experiments/versions/448eb0536875215291615d30b1516d99d8b73570.md) |
| 3dcc4677afe98138d6c4510adaedab726eab3532 | perf: bound transaction action digest reuse and compact immutable snapshot directories | [3dcc4677afe98138d6c4510adaedab726eab3532.md](main-thread-experiments/versions/3dcc4677afe98138d6c4510adaedab726eab3532.md) |
| ccac1f1b54f1a906a4af89b45889bb6086dacd20 | test: keep allocation failure injection out of compiler inlining and verify failure was exercised | [ccac1f1b54f1a906a4af89b45889bb6086dacd20.md](main-thread-experiments/versions/ccac1f1b54f1a906a4af89b45889bb6086dacd20.md) |
| fc7b5908922cb0eb276209c7f7d8b5599eb8a85b | perf: defer unused host iterator cache allocations until first use | [fc7b5908922cb0eb276209c7f7d8b5599eb8a85b.md](main-thread-experiments/versions/fc7b5908922cb0eb276209c7f7d8b5599eb8a85b.md) |
| 2e032ce7a8b23ae7ef220a39018d21f0052e6710 | perf: preserve exact SHA256 byte ordering with fixed-width chunk comparisons | [2e032ce7a8b23ae7ef220a39018d21f0052e6710.md](main-thread-experiments/versions/2e032ce7a8b23ae7ef220a39018d21f0052e6710.md) |
| 022785e70b0524a93d57c8d1960cc3710f0ede61 | perf: coallocate owned inputs and reduce small trace growth and join contention | [022785e70b0524a93d57c8d1960cc3710f0ede61.md](main-thread-experiments/versions/022785e70b0524a93d57c8d1960cc3710f0ede61.md) |
| 23a0a4d3aec9fbcde1b4074f912d7f21091c9b9d | perf: batch and inline owner snapshots without changing canonical validation | [23a0a4d3aec9fbcde1b4074f912d7f21091c9b9d.md](main-thread-experiments/versions/23a0a4d3aec9fbcde1b4074f912d7f21091c9b9d.md) |
| 3b0c6aa85a09d33a283576007d47091a2aac45da | perf: keep short block-tail deadlines from penalizing healthy admission | [3b0c6aa85a09d33a283576007d47091a2aac45da.md](main-thread-experiments/versions/3b0c6aa85a09d33a283576007d47091a2aac45da.md) |
| 679a733e9f27a9c4e739ec94d049a4acbdc586c8 | perf: defer unaffordable block-tail preparation without penalizing healthy workloads | [679a733e9f27a9c4e739ec94d049a4acbdc586c8.md](main-thread-experiments/versions/679a733e9f27a9c4e739ec94d049a4acbdc586c8.md) |
| cabd1aa23db0f875bfc02063f57998276731f517 | Retain bounded unknown-cost probes for learned mixed-batch admission | [cabd1aa23db0f875bfc02063f57998276731f517.md](main-thread-experiments/versions/cabd1aa23db0f875bfc02063f57998276731f517.md) |
| 882ed34363b6984eb869c2d37e0e5f6718e6472f | perf: reuse four owned root programs within one unchanged preparation prefix | [882ed34363b6984eb869c2d37e0e5f6718e6472f.md](main-thread-experiments/versions/882ed34363b6984eb869c2d37e0e5f6718e6472f.md) |
| ee3818eeff5c9aa1cc60a6a240566953790572b9 | perf: inline immutable single-row snapshot directories with bounded spill | [ee3818eeff5c9aa1cc60a6a240566953790572b9.md](main-thread-experiments/versions/ee3818eeff5c9aa1cc60a6a240566953790572b9.md) |
| 24c9cadbc67c6543e693ee37a4a6c86564a5c5fb | perf: batch owned snapshot directory storage with bounded inline bookkeeping | [24c9cadbc67c6543e693ee37a4a6c86564a5c5fb.md](main-thread-experiments/versions/24c9cadbc67c6543e693ee37a4a6c86564a5c5fb.md) |
| 2bb4d7489ed4b5cac638763f4ed9f1bb7c3bab55 | perf: preserve SHA256 byte ordering with direct native-word comparisons | [2bb4d7489ed4b5cac638763f4ed9f1bb7c3bab55.md](main-thread-experiments/versions/2bb4d7489ed4b5cac638763f4ed9f1bb7c3bab55.md) |
| 4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0 | perf: fuse owner primary snapshot observations and reduce owned context allocations | [4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0.md](main-thread-experiments/versions/4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0.md) |
| 42d6e8affa04747f125a55dac84c2424ef770651 | Reduce owner validation lookups with fresh exact identity membership | [42d6e8affa04747f125a55dac84c2424ef770651.md](main-thread-experiments/versions/42d6e8affa04747f125a55dac84c2424ef770651.md) |
| f153c330a66e093188c923d30de3c8d4f1bdc1df | Add opt-in bounded owner admission routing diagnostics | [f153c330a66e093188c923d30de3c8d4f1bdc1df.md](main-thread-experiments/versions/f153c330a66e093188c923d30de3c8d4f1bdc1df.md) |
| 5cfdd6ac795145b177acfb782d1f86f3a40175e6 | Reduce bounded owner authorization and notification allocations | [5cfdd6ac795145b177acfb782d1f86f3a40175e6.md](main-thread-experiments/versions/5cfdd6ac795145b177acfb782d1f86f3a40175e6.md) |
| 57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2 | Reduce task queue overhead and retain callback ownership on failure | [57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2.md](main-thread-experiments/versions/57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2.md) |
| 9aee6083cf95065b033a378ceae51b3465645fde | Fix pending callback ownership on queue clear and failed insertion | [9aee6083cf95065b033a378ceae51b3465645fde.md](main-thread-experiments/versions/9aee6083cf95065b033a378ceae51b3465645fde.md) |
| d38e96ec5162c2922b5df61f99210c83fa781da8 | Preserve functional admission-tail and callback ownership fixes on accepted baseline | [d38e96ec5162c2922b5df61f99210c83fa781da8.md](main-thread-experiments/versions/d38e96ec5162c2922b5df61f99210c83fa781da8.md) |
| 61b753bc8e609e17d8c42177da12b9510cee4bf6 | Fix named thread pool startup ownership and failure recovery | [61b753bc8e609e17d8c42177da12b9510cee4bf6.md](main-thread-experiments/versions/61b753bc8e609e17d8c42177da12b9510cee4bf6.md) |
| 3dc673c01d1f9fd164264b93cf9c39162550e15c | WIP: add ordered ready processing for owned worker results; producer integration pending | [3dc673c01d1f9fd164264b93cf9c39162550e15c.md](main-thread-experiments/versions/3dc673c01d1f9fd164264b93cf9c39162550e15c.md) |
| ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | WIP: opt-in ordered adoption of completed owned results with original safety paths | [ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b.md](main-thread-experiments/versions/ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b.md) |
| 3f622b5d54f133b2b650dd3b02c4c7da39d2e685 | perf: preserve SHA256 byte ordering with direct native-word comparisons | [3f622b5d54f133b2b650dd3b02c4c7da39d2e685.md](main-thread-experiments/versions/3f622b5d54f133b2b650dd3b02c4c7da39d2e685.md) |
| a9016e05cd3e66acea4823aee02c1811ac5e0b00 | WIP: group already-ready owned results without waiting for group fill | [a9016e05cd3e66acea4823aee02c1811ac5e0b00.md](main-thread-experiments/versions/a9016e05cd3e66acea4823aee02c1811ac5e0b00.md) |
| 43bd2af81daa264361cc5141447b98b8b83cd82a | WIP: wire opt-in ready group 1/4/8 and controller safety matrix | [43bd2af81daa264361cc5141447b98b8b83cd82a.md](main-thread-experiments/versions/43bd2af81daa264361cc5141447b98b8b83cd82a.md) |
| 7b6db8ddc3e2ad93db7807d96fb531ad4d2b443d | WIP: bounded native read window with joined lifetime and original snapshot limits | [7b6db8ddc3e2ad93db7807d96fb531ad4d2b443d.md](main-thread-experiments/versions/7b6db8ddc3e2ad93db7807d96fb531ad4d2b443d.md) |
| eaad84daeb86bf65583e379fd32a53cf58c1d4f1 | WIP: independent owned fingerprint and trace-reserve switches with combination oracles | [eaad84daeb86bf65583e379fd32a53cf58c1d4f1.md](main-thread-experiments/versions/eaad84daeb86bf65583e379fd32a53cf58c1d4f1.md) |
| be33a591e44849e094f9de9894480c1040b049dd | Avoid redundant owner hash-vector allocations when owned fingerprint workers are enabled | [be33a591e44849e094f9de9894480c1040b049dd.md](main-thread-experiments/versions/be33a591e44849e094f9de9894480c1040b049dd.md) |
| 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | WIP: retain staged publication overlap with a two-participant initial native capture | [3a3f9361bdfb49a3be355dff2f5b3fb249bfd191.md](main-thread-experiments/versions/3a3f9361bdfb49a3be355dff2f5b3fb249bfd191.md) |
| 48d4183271f65d658f506e6a2e685747b67d1a7e | perf: preserve SHA256 byte ordering with direct native-word comparisons | [48d4183271f65d658f506e6a2e685747b67d1a7e.md](main-thread-experiments/versions/48d4183271f65d658f506e6a2e685747b67d1a7e.md) |
| f629dc378fb44e1f0c6407eee64750cf307373c2 | diagnostic: count owner routing on group8 without enabling performance timers | [f629dc378fb44e1f0c6407eee64750cf307373c2.md](main-thread-experiments/versions/f629dc378fb44e1f0c6407eee64750cf307373c2.md) |

## 全部现存机器记录与 TPS

| 记录/用例 | 代码 | 范围 | 窗口 TPS | 整批 TPS | 详情 |
|---|---|---|---:|---:|---|
| 20261027-epyc4345p-cpu32-owner-hint-fifo-round4-single-round / screen-cpu32-owner-batch4-round1 | b8407259d544a2442e0808b30e96f9b5a5d4481d | 单节点纯转账筛查 | 32,901.92 | 33,923.49 | [20261027-epyc4345p-cpu32-owner-hint-fifo-round4-single-round.md](main-thread-experiments/records/20261027-epyc4345p-cpu32-owner-hint-fifo-round4-single-round.md) |
| 20261027-epyc4345p-cpu32-owner-main-thread-round2-single-round / screen-cpu32-owner-batch2-round1 | 1258c1f8fe1f6cf74f4c6de6bc410155ed75b393 | 单节点纯转账筛查 | 32,454.33 | 33,703.73 | [20261027-epyc4345p-cpu32-owner-main-thread-round2-single-round.md](main-thread-experiments/records/20261027-epyc4345p-cpu32-owner-main-thread-round2-single-round.md) |
| 20261027-epyc4345p-cpu32-undo-session-cache-single-round / screen-cpu32-undo-slots-round1 | 75a55efddaa671b7f2a104dd94126104e8e82ca8 | 单节点纯转账筛查 | 35,540.42 | 35,923.20 | [20261027-epyc4345p-cpu32-undo-session-cache-single-round.md](main-thread-experiments/records/20261027-epyc4345p-cpu32-undo-session-cache-single-round.md) |
| 20261028-epyc4345p-cpu32-SHA-order-main-diagnostic / 无节点TPS | 2bb4d7489ed4b5cac638763f4ed9f1bb7c3bab55 | 诊断采样（不作容量结果） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-cpu32-SHA-order-main-diagnostic.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-SHA-order-main-diagnostic.md) |
| 20261028-epyc4345p-cpu32-admission-mixed-probe-single-round / screen-cpu32-admission-mixed-probe-round1 | cabd1aa23db0f875bfc02063f57998276731f517 | 单节点纯转账筛查 | 34,693.92 | 35,789.64 | [20261028-epyc4345p-cpu32-admission-mixed-probe-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-admission-mixed-probe-single-round.md) |
| 20261028-epyc4345p-cpu32-current4b8-expanded-complex-security-single-round / stability-cpu32-expanded-contracts-round1 | 4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,990.42 | [20261028-epyc4345p-cpu32-current4b8-expanded-complex-security-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-current4b8-expanded-complex-security-single-round.md) |
| 20261028-epyc4345p-cpu32-expanded-complex-security-single-round / stability-cpu32-expanded-contracts-round1 | cabd1aa23db0f875bfc02063f57998276731f517 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,984.11 | [20261028-epyc4345p-cpu32-expanded-complex-security-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-expanded-complex-security-single-round.md) |
| 20261028-epyc4345p-cpu32-fresh-next2-baseline-single-round / screen-cpu32-bulk-baseline-next2-round1 | fc7b5908922cb0eb276209c7f7d8b5599eb8a85b | 单节点纯转账筛查 | 37,555.00 | 36,429.95 | [20261028-epyc4345p-cpu32-fresh-next2-baseline-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-fresh-next2-baseline-single-round.md) |
| 20261028-epyc4345p-cpu32-lazy-iterator-reserve-single-round / screen-cpu32-lazy-iterator-round1 | fc7b5908922cb0eb276209c7f7d8b5599eb8a85b | 单节点纯转账筛查 | 36,912.83 | 37,083.98 | [20261028-epyc4345p-cpu32-lazy-iterator-reserve-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-lazy-iterator-reserve-single-round.md) |
| 20261028-epyc4345p-cpu32-owned-digest-snapshot-single-round / screen-cpu32-owned-digest-round1 | ccac1f1b54f1a906a4af89b45889bb6086dacd20 | 单节点纯转账筛查 | 36,600.75 | 37,084.03 | [20261028-epyc4345p-cpu32-owned-digest-snapshot-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owned-digest-snapshot-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-admission-routing-diagnostic / 无节点TPS | f153c330a66e093188c923d30de3c8d4f1bdc1df | 诊断采样（不作容量结果） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-cpu32-owner-admission-routing-diagnostic.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-admission-routing-diagnostic.md) |
| 20261028-epyc4345p-cpu32-owner-alloc-notify-reserve-single-round / screen-cpu32-owner-alloc-notify-round1 | 022785e70b0524a93d57c8d1960cc3710f0ede61 | 单节点纯转账筛查 | 36,818.67 | 36,558.50 | [20261028-epyc4345p-cpu32-owner-alloc-notify-reserve-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-alloc-notify-reserve-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-batch48-single-round / screen-cpu32-owner-batch48-round1 | 4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0 | 单节点纯转账筛查 | 35,372.83 | 36,422.24 | [20261028-epyc4345p-cpu32-owner-batch48-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-batch48-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-bounded-allocations-single-round / screen-cpu32-owner-bounded-allocations-round1 | 5cfdd6ac795145b177acfb782d1f86f3a40175e6 | 单节点纯转账筛查 | 35,297.58 | 36,407.69 | [20261028-epyc4345p-cpu32-owner-bounded-allocations-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-bounded-allocations-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-bulk-inline-snapshot-reserve-single-round / screen-cpu32-owner-bulk-inline-snapshot-round1 | 23a0a4d3aec9fbcde1b4074f912d7f21091c9b9d | 单节点纯转账筛查 | 37,050.08 | 37,619.26 | [20261028-epyc4345p-cpu32-owner-bulk-inline-snapshot-reserve-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-bulk-inline-snapshot-reserve-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-current-root-views-single-round / screen-cpu32-owner-views-round1 | 448eb0536875215291615d30b1516d99d8b73570 | 单节点纯转账筛查 | 36,261.75 | 36,554.85 | [20261028-epyc4345p-cpu32-owner-current-root-views-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-current-root-views-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-fused-primary-capture-single-round / screen-cpu32-owner-fused-primary-capture-round1 | 4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0 | 单节点纯转账筛查 | 35,210.58 | 37,348.69 | [20261028-epyc4345p-cpu32-owner-fused-primary-capture-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-fused-primary-capture-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-identity-lookup-single-round / screen-cpu32-owner-identity-lookup-round1 | 42d6e8affa04747f125a55dac84c2424ef770651 | 单节点纯转账筛查 | 35,267.75 | 36,412.71 | [20261028-epyc4345p-cpu32-owner-identity-lookup-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-identity-lookup-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-root-program-cache-single-round / screen-cpu32-owner-root-program-cache-round1 | 882ed34363b6984eb869c2d37e0e5f6718e6472f | 单节点纯转账筛查 | 34,856.92 | 36,820.15 | [20261028-epyc4345p-cpu32-owner-root-program-cache-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-root-program-cache-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-sha-order-single-round / screen-cpu32-owner-sha-order-round1 | 2bb4d7489ed4b5cac638763f4ed9f1bb7c3bab55 | 单节点纯转账筛查 | 36,551.17 | 36,303.36 | [20261028-epyc4345p-cpu32-owner-sha-order-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-sha-order-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-snapshot-batch-single-round / screen-cpu32-owner-snapshot-batch-round1 | 24c9cadbc67c6543e693ee37a4a6c86564a5c5fb | 单节点纯转账筛查 | 35,168.17 | 36,406.68 | [20261028-epyc4345p-cpu32-owner-snapshot-batch-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-snapshot-batch-single-round.md) |
| 20261028-epyc4345p-cpu32-owner-task-queue-single-round / screen-cpu32-owner-task-queue-round1 | 57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2 | 单节点纯转账筛查 | 34,895.50 | 36,037.78 | [20261028-epyc4345p-cpu32-owner-task-queue-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-owner-task-queue-single-round.md) |
| 20261028-epyc4345p-cpu32-sha256-order-single-round / screen-cpu32-sha256-order-round1 | 2e032ce7a8b23ae7ef220a39018d21f0052e6710 | 单节点纯转账筛查 | 36,761.33 | 36,298.77 | [20261028-epyc4345p-cpu32-sha256-order-single-round.md](main-thread-experiments/records/20261028-epyc4345p-cpu32-sha256-order-single-round.md) |
| 20261028-epyc4345p-current57-main-thread-profile / 无节点TPS | 57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2 | 诊断采样（不作容量结果） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-current57-main-thread-profile.md](main-thread-experiments/records/20261028-epyc4345p-current57-main-thread-profile.md) |
| 20261028-epyc4345p-native-ledger-workflow-equivalence-v1 / 无节点TPS | 4b8ace5fcec7d7a5512350ec1d1c60f0fe7364c0 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-native-ledger-workflow-equivalence-v1.md](main-thread-experiments/records/20261028-epyc4345p-native-ledger-workflow-equivalence-v1.md) |
| 20261028-epyc4345p-offline-owned-parallel-capture-oracle / 无节点TPS | 组件/未绑定 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-offline-owned-parallel-capture-oracle.md](main-thread-experiments/records/20261028-epyc4345p-offline-owned-parallel-capture-oracle.md) |
| 20261028-epyc4345p-single-key-authorization-component-oracle / 无节点TPS | 组件/未绑定 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-single-key-authorization-component-oracle.md](main-thread-experiments/records/20261028-epyc4345p-single-key-authorization-component-oracle.md) |
| 20261028-epyc4345p-task-queue-component-oracle / 无节点TPS | 组件/未绑定 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-task-queue-component-oracle.md](main-thread-experiments/records/20261028-epyc4345p-task-queue-component-oracle.md) |
| 20261028-epyc4345p-task-queue-ownership-only-fix / 无节点TPS | 9aee6083cf95065b033a378ceae51b3465645fde | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261028-epyc4345p-task-queue-ownership-only-fix.md](main-thread-experiments/records/20261028-epyc4345p-task-queue-ownership-only-fix.md) |
| 20261029-epyc4345p-SHA-order-3f622b5d-affinity-fixed-single-screen / screen-cpu32-owner-sha-order-round1 | 3f622b5d54f133b2b650dd3b02c4c7da39d2e685 | 组件/安全/工具验证（无节点TPS） | 38,594.83 | 36,821.64 | [20261029-epyc4345p-SHA-order-3f622b5d-affinity-fixed-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-SHA-order-3f622b5d-affinity-fixed-single-screen.md) |
| 20261029-epyc4345p-SHA-order-3f622b5d-single-screen / screen-cpu32-owner-sha-order-round1 | 3f622b5d54f133b2b650dd3b02c4c7da39d2e685 | 单节点纯转账筛查 | 34,615.75 | 35,799.23 | [20261029-epyc4345p-SHA-order-3f622b5d-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-SHA-order-3f622b5d-single-screen.md) |
| 20261029-epyc4345p-SHA-order-group8-combination-equivalence-checkpoint / 无节点TPS | 48d4183271f65d658f506e6a2e685747b67d1a7e | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-SHA-order-group8-combination-equivalence-checkpoint.md](main-thread-experiments/records/20261029-epyc4345p-SHA-order-group8-combination-equivalence-checkpoint.md) |
| 20261029-epyc4345p-action-trace-reserve-component / 无节点TPS | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-action-trace-reserve-component.md](main-thread-experiments/records/20261029-epyc4345p-action-trace-reserve-component.md) |
| 20261029-epyc4345p-baseline-group8-repeat-pure-r1-3a3f9361-single-screen / screen-cpu32-baseline-group8-repeat-pure-r1-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 38,071.42 | 37,887.18 | [20261029-epyc4345p-baseline-group8-repeat-pure-r1-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-baseline-group8-repeat-pure-r1-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-combination-prelaunch-path-method-fix / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-combination-prelaunch-path-method-fix.md](main-thread-experiments/records/20261029-epyc4345p-combination-prelaunch-path-method-fix.md) |
| 20261029-epyc4345p-complex-renewal-RAM-profile-method-fix / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 诊断采样（不作容量结果） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-complex-renewal-RAM-profile-method-fix.md](main-thread-experiments/records/20261029-epyc4345p-complex-renewal-RAM-profile-method-fix.md) |
| 20261029-epyc4345p-cpu32-group8-network-r1-expanded-complex-security / stability-cpu32-expanded-contracts-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,973.76 | [20261029-epyc4345p-cpu32-group8-network-r1-expanded-complex-security.md](main-thread-experiments/records/20261029-epyc4345p-cpu32-group8-network-r1-expanded-complex-security.md) |
| 20261029-epyc4345p-cpu32-group8-network-r2-a5-expanded-complex-security / stability-cpu32-expanded-contracts-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,952.22 | [20261029-epyc4345p-cpu32-group8-network-r2-a5-expanded-complex-security.md](main-thread-experiments/records/20261029-epyc4345p-cpu32-group8-network-r2-a5-expanded-complex-security.md) |
| 20261029-epyc4345p-cpu32-group8-network-r3-expanded-complex-security / stability-cpu32-expanded-contracts-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,982.48 | [20261029-epyc4345p-cpu32-group8-network-r3-expanded-complex-security.md](main-thread-experiments/records/20261029-epyc4345p-cpu32-group8-network-r3-expanded-complex-security.md) |
| 20261029-epyc4345p-cpu32-group8-repeat-single-r1-expanded-complex-security / stability-cpu32-expanded-contracts-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,939.23 | [20261029-epyc4345p-cpu32-group8-repeat-single-r1-expanded-complex-security.md](main-thread-experiments/records/20261029-epyc4345p-cpu32-group8-repeat-single-r1-expanded-complex-security.md) |
| 20261029-epyc4345p-cpu32-owner-combo-expanded-complex-security-single-round / stability-cpu32-expanded-contracts-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,995.43 | [20261029-epyc4345p-cpu32-owner-combo-expanded-complex-security-single-round.md](main-thread-experiments/records/20261029-epyc4345p-cpu32-owner-combo-expanded-complex-security-single-round.md) |
| 20261029-epyc4345p-cpu32-owner-group8-expanded-complex-security-single-round / stability-cpu32-expanded-contracts-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 90%转账+10%复杂合约稳定性 | 8,000.00 | 7,970.64 | [20261029-epyc4345p-cpu32-owner-group8-expanded-complex-security-single-round.md](main-thread-experiments/records/20261029-epyc4345p-cpu32-owner-group8-expanded-complex-security-single-round.md) |
| 20261029-epyc4345p-fingerprint-inflight-and-512-allocation-faults / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-fingerprint-inflight-and-512-allocation-faults.md](main-thread-experiments/records/20261029-epyc4345p-fingerprint-inflight-and-512-allocation-faults.md) |
| 20261029-epyc4345p-fingerprints-plus-trace-3a3f9361-single-screen / screen-cpu32-fingerprints-plus-trace-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,263.83 | 36,428.88 | [20261029-epyc4345p-fingerprints-plus-trace-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-fingerprints-plus-trace-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-full-context-fingerprint-staging-component / 无节点TPS | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-full-context-fingerprint-staging-component.md](main-thread-experiments/records/20261029-epyc4345p-full-context-fingerprint-staging-component.md) |
| 20261029-epyc4345p-g8net-r1-pure-a4-3a3f9361-single-screen / screen-cpu32-g8net-r1-pure-a4-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 37,131.17 | 37,197.25 | [20261029-epyc4345p-g8net-r1-pure-a4-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-g8net-r1-pure-a4-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-g8net-r2-pure-a4-3a3f9361-single-screen / screen-cpu32-g8net-r2-pure-a4-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,403.67 | 38,014.68 | [20261029-epyc4345p-g8net-r2-pure-a4-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-g8net-r2-pure-a4-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-g8net-r3-pure-a4-3a3f9361-single-screen / screen-cpu32-g8net-r3-pure-a4-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,772.75 | 37,876.03 | [20261029-epyc4345p-g8net-r3-pure-a4-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-g8net-r3-pure-a4-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-group8-owner-routing-counter-prototype / 无节点TPS | f629dc378fb44e1f0c6407eee64750cf307373c2 | 诊断采样（不作容量结果） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-group8-owner-routing-counter-prototype.md](main-thread-experiments/records/20261029-epyc4345p-group8-owner-routing-counter-prototype.md) |
| 20261029-epyc4345p-grouped-ready-controller-safety-matrix / 无节点TPS | 43bd2af81daa264361cc5141447b98b8b83cd82a | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-grouped-ready-controller-safety-matrix.md](main-thread-experiments/records/20261029-epyc4345p-grouped-ready-controller-safety-matrix.md) |
| 20261029-epyc4345p-hardened-read-capture-component / 无节点TPS | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-hardened-read-capture-component.md](main-thread-experiments/records/20261029-epyc4345p-hardened-read-capture-component.md) |
| 20261029-epyc4345p-immutable-static-input-staging-component / 无节点TPS | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-immutable-static-input-staging-component.md](main-thread-experiments/records/20261029-epyc4345p-immutable-static-input-staging-component.md) |
| 20261029-epyc4345p-input-fingerprints-3a3f9361-single-screen / screen-cpu32-input-fingerprints-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 35,909.75 | 36,817.34 | [20261029-epyc4345p-input-fingerprints-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-input-fingerprints-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-mixed-prelaunch-template-method-fix / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-mixed-prelaunch-template-method-fix.md](main-thread-experiments/records/20261029-epyc4345p-mixed-prelaunch-template-method-fix.md) |
| 20261029-epyc4345p-native-capture-7b6db8dd-single-screen / screen-cpu32-native-capture-round1 | 7b6db8ddc3e2ad93db7807d96fb531ad4d2b443d | 单节点纯转账筛查 | 37,049.50 | 36,555.20 | [20261029-epyc4345p-native-capture-7b6db8dd-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-capture-7b6db8dd-single-screen.md) |
| 20261029-epyc4345p-native-capture-inflight-and-512-allocation-faults / 无节点TPS | 7b6db8ddc3e2ad93db7807d96fb531ad4d2b443d | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-native-capture-inflight-and-512-allocation-faults.md](main-thread-experiments/records/20261029-epyc4345p-native-capture-inflight-and-512-allocation-faults.md) |
| 20261029-epyc4345p-native-fingerprints-trace-3a3f9361-single-screen / screen-cpu32-native-fingerprints-trace-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,701.25 | 36,555.95 | [20261029-epyc4345p-native-fingerprints-trace-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-fingerprints-trace-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-fingerprints-trace-group4-3a3f9361-single-screen / screen-cpu32-native-fingerprints-trace-group4-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 38,325.75 | 38,458.35 | [20261029-epyc4345p-native-fingerprints-trace-group4-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-fingerprints-trace-group4-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-fingerprints-trace-group8-3a3f9361-single-screen / screen-cpu32-native-fingerprints-trace-group8-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 39,215.67 | 38,168.73 | [20261029-epyc4345p-native-fingerprints-trace-group8-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-fingerprints-trace-group8-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-full-same-binary-3a3f9361-single-screen / screen-cpu32-native-full-same-binary-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,776.67 | 36,555.11 | [20261029-epyc4345p-native-full-same-binary-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-full-same-binary-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-plus-fingerprints-3a3f9361-single-screen / screen-cpu32-native-plus-fingerprints-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,705.92 | 37,217.91 | [20261029-epyc4345p-native-plus-fingerprints-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-plus-fingerprints-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-plus-trace-3a3f9361-single-screen / screen-cpu32-native-plus-trace-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 37,010.33 | 36,681.66 | [20261029-epyc4345p-native-plus-trace-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-plus-trace-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-read-window-integration / 无节点TPS | 7b6db8ddc3e2ad93db7807d96fb531ad4d2b443d | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-native-read-window-integration.md](main-thread-experiments/records/20261029-epyc4345p-native-read-window-integration.md) |
| 20261029-epyc4345p-native-staged2-3a3f9361-single-screen / screen-cpu32-native-staged2-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,715.33 | 36,428.12 | [20261029-epyc4345p-native-staged2-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-staged2-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-staged2-fingerprints-3a3f9361-single-screen / screen-cpu32-native-staged2-fingerprints-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,185.17 | 36,300.13 | [20261029-epyc4345p-native-staged2-fingerprints-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-staged2-fingerprints-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-staged2-fingerprints-trace-3a3f9361-single-screen / screen-cpu32-native-staged2-fingerprints-trace-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,412.92 | 36,425.92 | [20261029-epyc4345p-native-staged2-fingerprints-trace-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-staged2-fingerprints-trace-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-staged2-fingerprints-trace-group4-3a3f9361-single-screen / screen-cpu32-native-staged2-fingerprints-trace-group4-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 37,129.25 | 37,208.43 | [20261029-epyc4345p-native-staged2-fingerprints-trace-group4-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-staged2-fingerprints-trace-group4-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-staged2-fingerprints-trace-group8-3a3f9361-single-screen / screen-cpu32-native-staged2-fingerprints-trace-group8-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,806.00 | 36,949.40 | [20261029-epyc4345p-native-staged2-fingerprints-trace-group8-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-staged2-fingerprints-trace-group8-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-native-staged2-trace-3a3f9361-single-screen / screen-cpu32-native-staged2-trace-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,501.08 | 36,812.16 | [20261029-epyc4345p-native-staged2-trace-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-native-staged2-trace-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-ordered-ready-adoption-ab9fa2d7-affinity-fixed-single-screen / screen-cpu32-owner-ready-adoption-round1 | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 组件/安全/工具验证（无节点TPS） | 37,527.42 | 37,758.85 | [20261029-epyc4345p-ordered-ready-adoption-ab9fa2d7-affinity-fixed-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-ordered-ready-adoption-ab9fa2d7-affinity-fixed-single-screen.md) |
| 20261029-epyc4345p-ordered-ready-adoption-single-screen / screen-cpu32-owner-ready-adoption-round1 | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 单节点纯转账筛查 | 35,622.25 | 36,553.24 | [20261029-epyc4345p-ordered-ready-adoption-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-ordered-ready-adoption-single-screen.md) |
| 20261029-epyc4345p-owned-ready-group-1-4-8-component / 无节点TPS | a9016e05cd3e66acea4823aee02c1811ac5e0b00 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-owned-ready-group-1-4-8-component.md](main-thread-experiments/records/20261029-epyc4345p-owned-ready-group-1-4-8-component.md) |
| 20261029-epyc4345p-owner-preparation-combination-integration / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-owner-preparation-combination-integration.md](main-thread-experiments/records/20261029-epyc4345p-owner-preparation-combination-integration.md) |
| 20261029-epyc4345p-owner-preparation-unbuilt-be33a59-review / 无节点TPS | be33a591e44849e094f9de9894480c1040b049dd | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-owner-preparation-unbuilt-be33a59-review.md](main-thread-experiments/records/20261029-epyc4345p-owner-preparation-unbuilt-be33a59-review.md) |
| 20261029-epyc4345p-owner-preparation-unbuilt-eaad84d-review / 无节点TPS | eaad84daeb86bf65583e379fd32a53cf58c1d4f1 | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-owner-preparation-unbuilt-eaad84d-review.md](main-thread-experiments/records/20261029-epyc4345p-owner-preparation-unbuilt-eaad84d-review.md) |
| 20261029-epyc4345p-owner-work-parallelization-source-review-prototype / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-owner-work-parallelization-source-review-prototype.md](main-thread-experiments/records/20261029-epyc4345p-owner-work-parallelization-source-review-prototype.md) |
| 20261029-epyc4345p-p2p-capacity-release-gate / 无节点TPS | ab9fa2d7d | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-p2p-capacity-release-gate.md](main-thread-experiments/records/20261029-epyc4345p-p2p-capacity-release-gate.md) |
| 20261029-epyc4345p-preparation-features-off-control-3a3f9361-single-screen / screen-cpu32-preparation-features-off-control-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,809.92 | 36,296.22 | [20261029-epyc4345p-preparation-features-off-control-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-preparation-features-off-control-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-readonly-permission-resource-prefetch-component-prototype / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-readonly-permission-resource-prefetch-component-prototype.md](main-thread-experiments/records/20261029-epyc4345p-readonly-permission-resource-prefetch-component-prototype.md) |
| 20261029-epyc4345p-ready-adoption-main-thread-profile / 无节点TPS | ab9fa2d7d7cf2fb29b8148905f4dc65399c8e81b | 诊断采样（不作容量结果） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-ready-adoption-main-thread-profile.md](main-thread-experiments/records/20261029-epyc4345p-ready-adoption-main-thread-profile.md) |
| 20261029-epyc4345p-ready-coordinator-safety-prototype / 无节点TPS | 57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-ready-coordinator-safety-prototype.md](main-thread-experiments/records/20261029-epyc4345p-ready-coordinator-safety-prototype.md) |
| 20261029-epyc4345p-ready-group4-43bd2af8-single-screen / screen-cpu32-ready-group4-round1 | 43bd2af81daa264361cc5141447b98b8b83cd82a | 单节点纯转账筛查 | 37,620.67 | 37,756.56 | [20261029-epyc4345p-ready-group4-43bd2af8-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-ready-group4-43bd2af8-single-screen.md) |
| 20261029-epyc4345p-ready-group8-43bd2af8-single-screen / screen-cpu32-ready-group8-round1 | 43bd2af81daa264361cc5141447b98b8b83cd82a | 单节点纯转账筛查 | 37,543.42 | 36,427.83 | [20261029-epyc4345p-ready-group8-43bd2af8-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-ready-group8-43bd2af8-single-screen.md) |
| 20261029-epyc4345p-sha-group8-fresh-reference-3a3f9361-single-screen / screen-cpu32-sha-group8-fresh-reference-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 39,601.33 | 38,169.62 | [20261029-epyc4345p-sha-group8-fresh-reference-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-sha-group8-fresh-reference-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-sha-order-native-fingerprints-trace-group8-48d41832-single-screen / screen-cpu32-sha-order-native-fingerprints-trace-group8-round1 | 48d4183271f65d658f506e6a2e685747b67d1a7e | 单节点纯转账筛查 | 38,386.50 | 37,749.91 | [20261029-epyc4345p-sha-order-native-fingerprints-trace-group8-48d41832-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-sha-order-native-fingerprints-trace-group8-48d41832-single-screen.md) |
| 20261029-epyc4345p-stable-fc7-current-fixture-reference-single-screen / screen-cpu32-stable-fc7-round1 | fc7b5908922cb0eb276209c7f7d8b5599eb8a85b | 组件/安全/工具验证（无节点TPS） | 36,494.75 | 37,213.30 | [20261029-epyc4345p-stable-fc7-current-fixture-reference-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-stable-fc7-current-fixture-reference-single-screen.md) |
| 20261029-epyc4345p-staged-partition-component-rejected / 无节点TPS | 57fcb6d215ec7ba5a1fe8198763a0fba6defa7e2 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-staged-partition-component-rejected.md](main-thread-experiments/records/20261029-epyc4345p-staged-partition-component-rejected.md) |
| 20261029-epyc4345p-startup-ownership-functional-fix / 无节点TPS | 61b753bc8e609e17d8c42177da12b9510cee4bf6 | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-startup-ownership-functional-fix.md](main-thread-experiments/records/20261029-epyc4345p-startup-ownership-functional-fix.md) |
| 20261029-epyc4345p-trace-reserve-3a3f9361-single-screen / screen-cpu32-trace-reserve-round1 | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 36,821.75 | 36,429.80 | [20261029-epyc4345p-trace-reserve-3a3f9361-single-screen.md](main-thread-experiments/records/20261029-epyc4345p-trace-reserve-3a3f9361-single-screen.md) |
| 20261029-epyc4345p-transaction-ID-owned-checked-hint-component / 无节点TPS | 43bd2af81daa264361cc5141447b98b8b83cd82a | 组件/安全/工具验证（无节点TPS） | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-epyc4345p-transaction-ID-owned-checked-hint-component.md](main-thread-experiments/records/20261029-epyc4345p-transaction-ID-owned-checked-hint-component.md) |
| 20261029-group8-three-machine-r1-pure-TPS / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-group8-three-machine-r1-pure-TPS.md](main-thread-experiments/records/20261029-group8-three-machine-r1-pure-TPS.md) |
| 20261029-group8-three-machine-r2-pure-TPS / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-group8-three-machine-r2-pure-TPS.md](main-thread-experiments/records/20261029-group8-three-machine-r2-pure-TPS.md) |
| 20261029-group8-three-machine-r3-pure-TPS / 无节点TPS | 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191 | 单节点纯转账筛查 | 未测/该记录未给出 | 未测/该记录未给出 | [20261029-group8-three-machine-r3-pure-TPS.md](main-thread-experiments/records/20261029-group8-three-machine-r3-pure-TPS.md) |

## 已选开发基线

采用说明见 [selected-development-baseline-20261029.md](selected-development-baseline-20261029.md)；原始测量记录保持当时状态，晋升决定单独记录。

## 本轮16组组合汇总

事前登记的16组同二进制筛查已完成；采用结论和逐组证据见 [main-thread-optimization-combination-results-20261029.md](main-thread-optimization-combination-results-20261029.md)。这不是穷尽所有候选或完成发布资格。

## 组件与研究补充

| 方案 | 已验证范围 | 当前处理 |
|---|---|
| 离线并行只读 capture | 每个变体 2,677,344 次查询结果比较；旧版17、加固版26生命周期检查；normal/ASan/UBSan/TSan | 离线通过；在线写隔离、Undo/分叉生命周期与接线待完成；没有节点TPS |
| staged capture 分区 | 23,266 oracle；8项正常/sanitizer 检查通过；组件 CPU 新/旧约1.137 | 组件成本上升，未接节点 |
| ready coordinator / canonical bridge | 规范顺序、停止前缀、异常、取消、中断、重入；8个真实 controller oracle 双VM及 scoped sanitizer | 已接入 ab9 的实验开关；尚未性能/稳定性采用 |
| named pool startup | reserve 分配失败和首个 pthread 创建失败可复现；每个变体80检查、11项正常/ASan/UBSan/TSan | 单独功能修复61b保留 |
| task queue 所有权 | 原 clear 漏3个callback、插入失败漏1个已复现；11项正常/sanitizer检查 | 单独功能修复9aee保留；不依赖队列替换性能 |
| Native 只读账本验证工具 | 同一历史全量用例与原RPC断言等价；工具检查加速 | 仅压测工作流加速，不能算链TPS改进 |
| TSan/glibc signal 恢复兼容调查 | 独立最小复现，限定 runtime 正/负对照保留；真实race/不安全handler仍报告 | 仅隔离研究测试，不改生产运行时，不关闭安全报告 |

## 本轮增量优化实验状态

| 方案 | 状态 | 完成条件 |
|---|---|
| 在线只读状态并行采集 | 7b6db8d 37,049.50窗口/36,555.20整批；owner24.354µs下降而TPS无明显提升；3a3f936 staged2单项及组合矩阵已完成，结果见16组汇总 | 严格写入隔离、完整拥有复制、4 MiB界限、Undo/分叉/取消、normal与sanitizer，再看节点收益 |
| 提前整理不可变交易输入 | 静态复制分发组件已测且成本上升而拒绝；完整上下文指纹组件已通过 normal/sanitizer，4参与者 owner约0.695→0.388µs；3a3f936完整normal、26TU scoped sanitizer及512故障矩阵通过；单项/组合16组节点矩阵完成，结果见汇总 | 完整输入绑定；owner仍核对当前状态、权限、资源、重复交易与提交 |
| 每4/8笔就绪结果分组交接 | a901组件矩阵、43bd控制器正常/sanitizer矩阵通过；group4已完成37,620.67窗口/37,756.56整批TPS，未达采用门槛；group8已完成37,543.42窗口/36,427.83整批，未达采用门槛 | 保持规范顺序、停止前缀、异常回收、不可变所有权；证明交接成本下降 |
| trace/receipt/input 临时存储预留复用 | trace独立组件通过，3步骤约省0.022µs；3a3f936已有独立开关且完整正确性矩阵通过；16组节点矩阵完成，结果见汇总 | 不跨有效期复用、不影响异步持有者、回滚与内存上界 |
| 交易ID查找/插入提示复用 | 正常/ASan/UBSan/TSan通过；空闲组件核对约慢9.36%，本变体拒绝，不接节点 | 唯一性检查保留，提示有明确失效规则，Undo/分叉/写入后回原路径 |

真正并行提交数据库需要 MVCC/分片等更大架构研究，未当成已完成的增量补丁。当前按用户要求仍只做单节点；实际多节点风险另见 [single-node-network-followups-20261029.md](single-node-network-followups-20261029.md)。

## 完整性

本次覆盖 93 个现存机器 JSON、39 个代码版本；生成 93 份记录 MD 与 39 份版本 MD。所有条目链接到原始证据，失败记录和方法纠正均保留。


## 新增组合与路由研究

SHA字节排序＋group8已完成同新夹具匹配双组、完整正常／scoped sanitizer与独立审计，未达到采用门槛；结果与当前后续路由计数研究见 [sha-order-group8-combination-progress-20261029.md](sha-order-group8-combination-progress-20261029.md)。原group8基线保持。f629dc37计数诊断正在安全回归，没有新容量结果。


## 三机联动编排与失败留存 2026-10-29

用户要求基线再复测并加入多机器/复杂合约；最终为三台 EPYC4345P 裸机（各8物理核/16线程/64GiB）：Manchester 8-worker出块节点、Manchester 8-worker完整验证节点、Amsterdam 0-worker串行完整验证参考节点。两台新机仅在全部测试及归档校验通过后删除，原开发机保留。一个出块者和现有finalizer/治理名单保持，不以三台验证进程冒称三个出块者或法定人数故障测试。

基线源3a3f9361bdfb49a3be355dff2f5b3fb249bfd191，Node SHA256 051989ea0dec7049c253c782bc5adfa9c83772862592173b26936042c4eb98ea，group8/batch24。测试CPU32；开发默认CPU16/NET32MiB保持。P2P64MiB/消息、1GiB队列、单笔/计费限制、Node100ms和私有准备50ms等执行预算不变，系统WASM保持fc639b60009090cb66424a58b9112fdb87309391a286905a0669ae3397289186，RAM初始64GiB、setramrate1024，其余投票/资源10%-90%/减半/出块治理模式不变，deferred禁用。

每次纯SYS/TKN转账104万笔，1秒预热+12秒固定发压+90秒排空；纯TPS轮不安排停机。每次混合984000笔（90%转账、10%复杂合约20类），另2280项原生功能/安全断言（1000有效、1280预期拒绝），并独立完整串行重放。第二、第三混合轮分别停止串行/并行验证节点20秒并从本机数据库恢复。部署、RAM、质押/PowerUp和签名均在计时外。使用出块机同一单调时钟观察三台head/LIB，不跨主机相减单调时钟；逐规范块计入真实交易。分别记录出块节点、各验证节点、三台全部执行/确认的窗口与全量TPS；CPU、样本间隔、落后块数和计划恢复单列。80笔连通检查不作为容量成绩。

已保留的工具/部署失败与修复（均未改变基线C++、预算或校验断言）：

- NFT规则模板换行语法错误，尚未开始负载即停止。
- 用户实际主组GID与假定值不同，systemd 216/GROUP；改用实际GID。
- force-all-checks和disable-replay-opts是CLI选项，不能写config.ini；移入启动参数并保持启用。
- UFW默认INPUT DROP覆盖了NFT的外部来源拒绝规则。a2检查出块head96而两个验证节点仍head64。仅增加三台测试机彼此9876/TCP的UFW放行，不作为有效TPS。
- 旧失败状态造成一次新控制器READY前启动竞态；改为先等新控制器READY。
- a3的80笔远端P2P转发、三机最终状态和两个归档各67文件校验通过，blocks.log/index与出块节点逐字节一致；但正式104万笔轮在发送前因完整状态哈希RPC默认5秒传输等待超时停止。a4仅将状态核对HTTP等待改为180秒；执行预算不改，正式输入全新生成。
- 旧失败用例的已答复IPC请求在控制器重启后被重复处理；加入答复去重并保存reconciled请求。一次重启命令误指向JSON请求文件，没有启动节点或负载，随后改为明确的控制器脚本路径。
- 旧MD生成器会重建总表，因此本段与逐轮MD另存于当前campaign目录，每轮结束及最终汇总时自动合并，保留全部失败与结果。

a4第一轮正式104万笔已全部最终确认、未解决0，独立原始数据审计通过。出块窗口37131.17 TPS、全量37197.25；三台全部执行/确认全量31236.32 TPS。远端并行验证全量31236.32，串行参考35004.19；本轮较慢的是并行验证节点，CPU约2.977逻辑核、主线程91.84%，相对出块节点块观测延迟p95约4779ms、最大5422ms。出块节点约5.196逻辑核、主线程95.84%。原因尚未定位，需分别检查验证准备、签名、合并提交和P2P接收/追块队列。该结论是后续优化项，尚未证明是数据库或某一个算法所致。短固定窗口不代表可长期稳定维持峰值。

远端安全用例涉及逐笔大量只读RPC。为避免跨地区逐次读取耗尽原3600秒交易有效期，后续将同一次before/after捕获所需的只读请求在验证机本地顺序执行，经原capture函数重放所有结果与断言。只允许get_table_rows/get_account/get_currency_balance；写方法在发RPC前拒绝，返回截断会失败。真实双机只读传输、原函数捕获等价、写拒绝和截断拒绝验证见READ_BATCH_TRANSPORT_PROOF.json。只在两个已关闭case之间升级控制器；现有a4第一混合轮输入不变，不改变交易寿命、执行预算或安全断言。

待覆盖：多出块者轮换、finalizer法定人数故障、网络分区/双签/分叉与长期持续吞吐。当前一出块者拓扑和短轮次不作为首发所有功能已合格的声明。后续优化应保留force-all-checks、disable-replay-opts、全部签名/授权/回滚验证及原资源协议。

本机完整源码已保存至/home/guagua/AntelopeIO/PureEOS-group8-baseline-20261029，分支baseline/group8-qualified-20261029，提交5ffee9305b4e1168937c7c57192041f1ee2dfa54（父提交为选定基线，仅新增基线说明与参数文件），原PureEOS工作区已有改动保留。主证据目录/opt/aic-group8-repeat-multimachine-20261029；最终报告group8-three-machine-network-report-20261029.md位于本机型结果目录。失败与成功节点归档都要在原开发机逐文件校验后才可删除新加机器。


### NET 上限与 P2P 队列核对

对当前基线源码核对：NET额度本身不使空闲区块填充到上限；P2P按实际序列化消息发送。链上计费NET与线上的消息字节不同。第1/2轮最大实测P2P载荷分别3783124字节（约3.61MiB）和3333684字节（约3.18MiB），未接近64MiB单消息上限。

另有独立的每连接发送等待队列：默认40MiB，在开始读消息时若排队超过该值会关闭连接；入队超过80MiB也会放弃连接。其计数与正在发送的out_queue分开。1GiB是producer的待执行交易队列，不是P2P发送队列。提高NET链上上限不会自动提高这些阈值。

更正核验结果：两个验证节点自身日志没有发送队列满；出块节点第1/2轮各有一次真实发送队列满，目标均为Amsterdam串行验证节点136.244.110.182，累计等待发送分别67697330和79255485字节（约64.56与75.58MiB），触发40MiB等待阈值而关闭连接，发生在固定发压窗口后的排空/追块阶段。不是压测客户端连接，也不是某个消息超过64MiB。实际最大单消息约3.61/3.18MiB，因此不能将积压直接归因为NET32MiB单项配置；需联合核验事务传播/区块广播的累计流量、WAN发送窗口、接收处理及重连追块。三节点最终全部交易确认与状态一致仍通过，但压力链路存在已观察到的非计划断连，不能声称全程连接平稳。half heartbeat刷新和timeout=0汇总与此分开。证据见CONFIRMED_P2P_BACKPRESSURE_EVENTS.json及NET_CAP_P2P_QUEUE_EVENT_DETAILS.json。现有纯转账未填满NET32MiB，不能据此推断满NET、大消息或慢网络时仍有同样传播/追块表现。后续应联合核验真实序列化块大小、发送排队、接收/校验耗时与恢复能力；本次仅记录，不修改40/80/64MiB限制、1GiB队列或正在测试的链上参数。


## 三机第二轮混合编排中断与恢复 2026-10-29

正式第二轮混合 attempt4 在安全预检查中因编排 SSH ControlPath 路径过长而中断；这是测试工具配置错误，未判为产品或合约缺陷，未复用该轮局部结果。随后为远程逐项只读检查配置连接复用，路径缩短到 /run/aicg8mux-1029/%C，配置仅作用于两台临时验证机。专用 -N RPC 隧道显式设置 ControlMaster=no、ControlPath=none，避免复用客户端提前退出及转发残留。原日志、状态、块文件及失败轨迹全部保留。

独立 attempt5 重新准备完整984000笔交易及2280个安全用例，未更改交易3600秒有效期、资源预算、WASM、安全断言和SDK参数。新输出目录曾遇到所有者权限和独占创建文件已存在的问题，已归档旧输入和日志并创建空输出目录，以新鲜原生语料和逐笔签名预检查重新执行；2026-10-29 15:30:25 UTC 两项预检通过，随后启动全新的三机混合轮。失败 attempt4 和预检恢复过程不计入合格轮次。

只读 RPC 批处理保持原 capture 函数及断言：先记录确切读取调用，在验证节点本地依次执行白名单 get_table_rows/get_account/get_currency_balance，再由原函数消费响应。批次上限64项/262144编码字节，变更方法在RPC前拒绝；已检查原顺序结果等价、截断响应失败及两台真实节点只读查询。有效变更交易逐项等待规范链确认。

用户追加 BP 自发现测试后，两台临时验证机的自动删除增加待完成条件；原三机报告的 safe_to_delete_peers 在该项完成前必须为 false。现有发布系统合约和已完成轮次仍保持原字节与口径。


## 三机第三轮混合检查通道恢复与后续候选 2026-10-30

原代码3a3f9361的三轮纯转账窗口TPS为37131.17、36403.67、36772.75，每轮1040000笔全部确认且未解决为0。前两轮混合各984000笔、2280个功能/安全用例及独立全链串行核对已完成。第三轮原attempt4的压力阶段之后，后置合法安全样例未确认，结果为未通过；其预签名窗口在2026-10-29 17:22:38 UTC到期。该轮原始结果、日志和三机归档保留，不复用为合格混合结果，详细时序与日志审查见THIRD_MIXED_ATTEMPT4_EXPIRY_REVIEW.json。

新的attempt5保持原3600秒交易有效期、全部原断言、CPU32/NET32MiB测试参数、Node051989ea和原系统WASMfc639不变，重新执行完整原生语料、签名预检、前后2280用例、984000笔压力与全量串行复核。只读查询通过SSH转发中的localhost HTTP长连接批处理，固定三种只读方法、最多64项/262144字节，按原顺序执行，禁止写方法；组件测试及两台实机的接口来源哈希、写入拒绝已验证。源代码是input/peer_read_api.py，SHA256 ea07099e8de451ce39a41694878eb36dd9a8281b08903f61eafde6cf5d43d77e。此改动不放宽链预算或减少测试。当前这轮尚未合格。

P2P发送候选代码位于/opt/aic-p2p-backpressure-fix-20261030/source，分支development/p2p-backpressure-fix-20261030，提交01bdd60e1（包含d88a6b3c7）。每个异步发送批次最多1MiB/256条完整消息，单条合法大消息完整发送，批次之间重新选择同步/普通消息优先级；40/80MiB发送队列、P2P64MiB消息、生产者1GiB队列及计费不变。仅在积压断连时记录分类，正常交易无逐笔新计时。当前处于待编译/待验证，Node TPS未测，不是新基线。后续验证控制器等待原三机完整结束后串行运行：/opt/aic-p2p-backpressure-network-20261030。

BP peer-key合约及节点改动仅为独立候选：/opt/aic-bp-discovery-feature-20261029。用户已授权最小接入并要求测试成功才纳基线；当前未部署。合约参数名和delpeerkey的双参数已依据Vaulta实链ABI核对；删除时需当前key匹配，保留投票、轮换、治理、10/90资源、奖励和RAM1024规则。节点候选还补充空名单撤销及gossip签名/内容/当前key复核。其安全与三机自发现测试尚待执行；有现存问题先修，不提前推广。开发默认仍为CPU16/NET16MiB，本段CPU32/NET32MiB只是原授权测试参数。


### 三机纯转账第1轮已核对

源 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191，group8，CPU32/NET32MiB，实际三台 EPYC4345P；一台8-worker出块、一台8-worker全验证、一台0-worker串行参考。104万笔SYS/TKN纯转账全部最终确认，未解决0笔；三节点最终head、LIB、完整状态哈希和系统参数相同，验证节点归档逐文件通过，blocks.log/index与出块节点逐字节相同。

|口径|窗口 TPS|全量 TPS|
|---|---:|---:|
|出块节点|37131.17|37197.25|
|三节点全部执行|37131.17|31236.32|
|三节点全部确认|34836.58|31236.32|

串行参考节点也参与三节点共同完成指标，因此该指标包含串行验证能力限制。每个节点独立追块吞吐、CPU、观测间隔和延迟详见 /opt/aic-group8-repeat-multimachine-20261029/ROUND1_NETWORK_TPS.json。本轮为短窗口并行基线联动结果，不代表多出块者/分区容错或长期稳定峰值。




### 三机纯转账第2轮已核对

源 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191，group8，CPU32/NET32MiB，实际三台 EPYC4345P；一台8-worker出块、一台8-worker全验证、一台0-worker串行参考。104万笔SYS/TKN纯转账全部最终确认，未解决0笔；三节点最终head、LIB、完整状态哈希和系统参数相同，验证节点归档逐文件通过，blocks.log/index与出块节点逐字节相同。

|口径|窗口 TPS|全量 TPS|
|---|---:|---:|
|出块节点|36403.67|38014.68|
|三节点全部执行|36535.42|32395.33|
|三节点全部确认|33787.08|32395.33|

串行参考节点也参与三节点共同完成指标，因此该指标包含串行验证能力限制。每个节点独立追块吞吐、CPU、观测间隔和延迟详见 /opt/aic-group8-repeat-multimachine-20261029/ROUND2_NETWORK_TPS.json。本轮为短窗口并行基线联动结果，不代表多出块者/分区容错或长期稳定峰值。




### 三机纯转账第3轮已核对

源 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191，group8，CPU32/NET32MiB，实际三台 EPYC4345P；一台8-worker出块、一台8-worker全验证、一台0-worker串行参考。104万笔SYS/TKN纯转账全部最终确认，未解决0笔；三节点最终head、LIB、完整状态哈希和系统参数相同，验证节点归档逐文件通过，blocks.log/index与出块节点逐字节相同。

|口径|窗口 TPS|全量 TPS|
|---|---:|---:|
|出块节点|36772.75|37876.03|
|三节点全部执行|36681.92|36267.79|
|三节点全部确认|33625.00|34555.88|

串行参考节点也参与三节点共同完成指标，因此该指标包含串行验证能力限制。每个节点独立追块吞吐、CPU、观测间隔和延迟详见 /opt/aic-group8-repeat-multimachine-20261029/ROUND3_NETWORK_TPS.json。本轮为短窗口并行基线联动结果，不代表多出块者/分区容错或长期稳定峰值。


## Group8 三机联动复核 2026-10-29

代码 3a3f9361bdfb49a3be355dff2f5b3fb249bfd191；Node SHA256 051989ea0dec7049c253c782bc5adfa9c83772862592173b26936042c4eb98ea。

三台实际 EPYC 4345P 裸机，各 8 物理核/16 线程/64 GiB；Manchester 出块节点、Manchester 8-worker 完整验证节点、Amsterdam 串行完整验证节点。CPU32 为授权测试参数，开发默认仍为 CPU16；NET32 MiB/块，P2P64 MiB/消息，1 GiB 队列，计费与单笔上限保持。

每轮纯转账 1,040,000 笔，SYS/TKN 各半且互不冲突；三个纯转账轮没有计划停机。混合轮 984,000 笔，90% 转账、10% 复杂动作；每轮另有 2,280 个功能/安全断言，并独立串行重放。测试计时不含部署、买 RAM、质押、PowerUp 或签名。

|轮次|出块节点窗口 TPS|出块节点全量 TPS|三节点全部执行窗口 TPS|三节点全部执行全量 TPS|三节点全部确认全量 TPS|
|---|---:|---:|---:|---:|---:|
|g8net-r1-pure-a4|37131.17|37197.25|37131.17|31236.32|31236.32|
|g8net-r2-pure-a4|36403.67|38014.68|36535.42|32395.33|32395.33|
|g8net-r3-pure-a4|36772.75|37876.03|36681.92|36267.79|34555.88|

|纯转账轮次|出块节点逻辑核占用|并行验证逻辑核占用|串行验证逻辑核占用|
|---|---:|---:|---:|
|g8net-r1-pure-a4|5.196|2.977|2.601|
|g8net-r2-pure-a4|5.189|3.167|2.446|
|g8net-r3-pure-a4|5.227|3.265|2.541|

|混合稳定性轮次|全部确认交易数|三台全部执行全量吞吐|安全用例|计划恢复验证|
|---|---:|---:|---:|---|
|g8net-r1-mixed-a4|984000|7967.51|2280|无计划停机|
|g8net-r2-mixed-a5|984000|7943.70|2280|serial-validator 停20秒并从原数据库恢复|
|g8net-r3-mixed-a5|984000|7973.04|2280|parallel-validator 停20秒并从原数据库恢复|

串行参考节点参与了三台全部执行/确认指标，该口径包含串行验证能力的限制。以下单列远端并行与串行验证节点吞吐。

|纯转账轮次|并行验证窗口 TPS|并行验证全量 TPS|串行验证窗口 TPS|串行验证全量 TPS|
|---|---:|---:|---:|---:|
|g8net-r1-pure-a4|37131.17|31236.32|37131.17|35004.19|
|g8net-r2-pure-a4|36535.42|32395.33|36535.42|35978.71|
|g8net-r3-pure-a4|36681.92|36267.79|36681.92|36560.63|

|轮次|出块节点发送等待队列断连次数|记录中的积压 MiB|
|---|---:|---|
|g8net-r1-pure-a4|1|64.56|
|g8net-r2-pure-a4|1|75.58|
|g8net-r3-pure-a4|1|65.19|

全部节点执行/确认 TPS 以同一台主机的单调时钟采样实际规范链 head/LIB 为准，逐块计入真实交易；跨机器单调时钟不相减。完整轮持续时间及最大采样间隔、节点 CPU、落后块数、恢复事件见各 NETWORK_TPS_AUDIT.json。

已覆盖：真实跨机器 P2P 转发与块传播、并行/串行完整验证、三轮压力、相同最终 head/LIB/全状态哈希、相同系统 WASM/资源参数/RAM1024、P2P 消息声明长度边界、复杂合约安全断言、两种验证节点分别停止20秒后从本机数据库恢复追块、归档逐文件校验及 blocks.log/index 与出块节点逐字节一致。

本轮保持一个出块者及原 finalizer/治理模式。尚未证明多出块者轮换、finalizer 法定人数故障容错、网络分区/双签/分叉下的安全性或长期持续峰值。未将本报告当作首发全部功能已通过。

部署失败保留：NFT 规则语法、实际 GID、CLI 专属校验选项的位置、UFW 默认 DROP 拦截 P2P、旧失败状态导致首次重启竞态。均发生在正式压力前，未修改基线 C++、预算、WASM 或安全断言。失败目录与状态保留，不计为有效 TPS。

前两轮纯转账均在排空阶段观察到出块节点至Amsterdam验证节点的发送等待队列超过40MiB并关闭连接（约64.56/75.58MiB）。交易最终均确认且状态一致，吞吐指标包含实际传播与恢复过程；不声称全程无断连或网络容量已完全合格。详见CONFIRMED_P2P_BACKPRESSURE_EVENTS.json。

新增机器将在完整证据归档校验后删除；原开发机保留。云端删除回执另附。


## 2026-10-30 P2P bounded-send correction qualification
- Machine: unchanged actual AMD EPYC 4345P bare metal, 8 physical cores / 16 threads / 64 GiB.
- Original campaign: three pure-transfer and three 90/10 mixed three-machine cases completed and final audits passed. Pure producer-window TPS: 37131.1667 / 36403.6667 / 36772.7500; 1,040,000 finalized per pure case, 984,000 finalized and 2,280 security cases per mixed case; zero unresolved. Full independent serial replay and three-node state comparisons retained.
- Preserved original failures: mixed r3/a4 exceeded its pre-signed expiry window before the post-security finality assertion. Fresh complete r3/a5, including every security case, passed after correcting read-only SSH transport; no failed-case assertions, WASM or resource budgets were relaxed.
- Transport candidate source: 01bdd60e186d837b37ee209a997f4f419d6aaf10; Node SHA256: 0620b0828d0da01174399feab64490ac2230620bb68e322729eb70b19ef5c515.
- Change: bound each asynchronous send to 1 MiB / 256 complete messages, permit a single larger legal frame intact, reconsider sync/block/control priority between batches, and record queue class composition only on overflow. P2P payload limit 64 MiB, soft/hard pending-send thresholds 40/80 MiB, producer queue 1 GiB, all billing and consensus rules remain unchanged.
- New component tests: normal and ASan/UBSan passed, including priority, FIFO, callbacks, message boundaries, buffer lifetime and 100,000-message randomized FIFO.
- Orchestration failures retained separately: completion-marker/phase-lock ordering race, then linker dependency receipt targeting the read-only parent build. Both orchestration paths corrected; fresh candidate binary built successfully. These are not claimed as chain runtime failures.
- At this entry: full core regression still running, no candidate three-machine TPS result yet; node TPS unmeasured for this candidate. Not promoted to baseline.
- Evidence: /opt/aic-p2p-backpressure-fix-20261030 and /opt/aic-p2p-backpressure-network-20261030. Original six-case evidence is immutable and retains source 3a3f9361.
- BP discovery remains a separately versioned, unqualified branch. It has not been activated on the retained chain or represented as tested.

## 2026-10-30 rejected bounded-send candidate and mandatory parallel-node policy
- Candidate 01bdd60e186d837b37ee209a997f4f419d6aaf10 / Node 0620b0828d0da01174399feab64490ac2230620bb68e322729eb70b19ef5c515 did not qualify. Fresh case g8net-r1-pure-a2: producer fixed-window 37191.6667 TPS; all-three whole-corpus applied/finalized rate 21503.3328 TPS. All 1,040,000 finalized, zero unresolved, three state hashes and raw archives verified, but a send-queue overflow at 80 MiB on the serial-validator link remains. Repeated log entries are not counted as independent disconnects. No baseline promotion.
- Historical topology was deliberately one producer plus one parallel validator and one serial validator on three identical 8C/16T/64GiB EPYC4345P hosts. These results are not an all-parallel network throughput ceiling.
- User now requires only parallel live-node scenarios, explicitly prohibits serial-only nodeos, and requests startup rejection below 8 physical cores, 16 logical threads, 64 GB RAM, with detected and minimum configuration in logs. Further serial-node performance diagnostics stopped. New all-parallel scenarios must be separately versioned and preserve historical evidence.
- New mandatory-parallel/hardware source starts from qualified 3a3f9361; the rejected bounded-send optimization is excluded. CPU16/NET16MiB development caps, P2P64MiB messages, producer queue1GiB and all billing/consensus/system-contract policy remain as previously selected.

## 2026-10-30 mandatory nodeos parallel/hardware startup gates passed
Source 44ab5dcc64bed7d649c6c5f2e96f659bfa1cdebd; Node 1de5adfa0018e44ec3e19e3bf4e9f631eae59a439f556fbe734e6864264b4bf2. Normal and ASan/UBSan hardware component gates plus 9 real-process startup cases passed (8 expected rejections before chain DB opens, one healthy 8-worker/batch24 API startup and clean stop). Minimum 8 physical cores /16 logical threads /64 decimal GB MemTotal. Serial-only nodeos disabled. No new TPS measurement or discovery qualification claimed. Report: /opt/aic-performance-results/machines/amd-epyc-4345p-8-core-processor--baremetal--8physical-16threads--65536MiB/nodeos-parallel-required-hardware-20261030.md


### 2026-10-30 全并行三机 CPU16 / NET16MiB 复测

源码 44ab5dcc64bed7d649c6c5f2e96f659bfa1cdebd；nodeos SHA256 1de5adfa0018e44ec3e19e3bf4e9f631eae59a439f556fbe734e6864264b4bf2。三台 EPYC 4345P（8 物理核 /16 线程 /标称 64GiB），均并行8 worker、batch24、group8，CPU16、NET16MiB/块、P2P64MiB、生产队列1GiB。单轮12秒饱和窗口生产端37259.4167 TPS，三机整批应用及最终确认22400.2096 TPS。1040000 笔全部最终确认、未确认0；完整验证、三个状态哈希和原始区块审计一致。生产端记录9380条队列溢出日志，不能当作9380次独立断连。两台验证节点均为并行模式，故积压不能归因于串行节点。新硬件门槛及并行启动代码有效，但传输过载尚未解决；不提升吞吐基线，不声称持续容量或BP自发现已通过。证据：/opt/aic-all-parallel-network-20261030/QUALIFICATION.json 和 NETWORK_CORRECTNESS_PROOFS.json。


### 2026-10-30 全并行 P2P 未确认交易路径隔离

状态：运行中，TPS 待测；仅诊断，不选为基线。源码44ab5dcc/Node1de5adfa保持不变，三机8 worker，CPU16/NET16MiB保持不变。仅纯转账压力轮中两个验证节点设置p2p-accept-transactions=false；链上完整区块验证继续并行，80笔远端入口冒烟保持正常交易转发。此隔离同时减少未确认交易的预执行/转发和额外网络字节，不能单独作为纯CPU因果证明；也不能把关闭接收作为正式问题修复。方法与审计在/opt/aic-parallel-relay-isolation-20261030/。


### 2026-10-30 BP 自发现分支接入强制并行硬件前端

独立候选852c488d450401d47ab31597a3338c2cf2f6303a，从44ab5dcc重新接入peer-key缓存、签名重放绑定、撤销/轮换与连接生命周期修复；明确排除未通过的bounded-send候选。最低硬件和强制并行三个前端文件逐字节保持已测版本。当前仅准备源码，组件回归/编译排在上述实机测试后，尚未测试链上原生合约或三台真实BP发现；节点TPS未测，自发现未激活，不提升基线。源码证明/opt/aic-bp-discovery-feature-20261029/PARALLEL_ONLY_SOURCE_PREPARED.json，后续隔离构建/opt/aic-bp-discovery-parallel-qualification-20261030/。


#### 全并行未确认路径隔离：最终结果（未采用）

生产端固定12秒窗口37655.5000 TPS；整批入块37735.6806 TPS；三机整批应用/最终确认21459.9567 TPS，耗时48.462353035秒。1040000笔全部最终确认，未确认0，三个状态哈希、完整校验和原始区块逐字节审计一致。与正常接收的全并行22400.2096 TPS相比没有改善，不选作基线，不把禁用未确认交易接收当正式修复。生产端固定窗口约5.16194逻辑核、主线程93.616%；阿姆斯特丹验证机约2.33351逻辑核、主线程83.817%，均为短窗采样。

生产端有2条明确发送队列关闭日志，分别对应两个验证节点（70818098与79625048字节），发生于start_read_message的40MiB检查；不是暂停读取。queue_write另有80MiB关闭检查，两种检测均保留。此隔离同时减少预执行和网络流量，仍然发生关闭，因此不能仅把瓶颈归因于HEAD逐笔预执行；需继续定位完整区块、异步写与链路/追块背压。

报告恢复：测试及原始审计完成后，汇总脚本漏带NETWORK_REPORT_CLOCK.json。失败保留REPORT_PHASE_FAILURE_1.json；复用同一boot_id时钟偏移并重新验证相差30ns，只重建汇总，无重新压力负载、无复用其他用例成绩。独立组件构建首次因Ninja生成的module mapper未生成而停止，另存构建失败，修正为在本次证据目录复制/生成空映射后重试，生产源码未改。

证据：/opt/aic-parallel-relay-isolation-20261030/QUALIFICATION.json、NETWORK_CORRECTNESS_PROOFS.json、network-cases/g8net-r1-pure-a1/NETWORK_TPS_AUDIT.json。全部大文件仍保留主服务器，三台机器保留供后续BP发现资格测试；自发现未激活。


#### BP 独立组件回归迭代（节点TPS未测）

852c488d初版：先因缺少Ninja module mapper停止，再发现测试时钟模拟类型不匹配生产fc::time_point，普通安全测试未能编译；保留构建日志。411c281a修正测试时钟类型，安全断言未变；14项普通组件中12项通过、两个历史active_schedule清理断言失败。0cfbc1a245ea7af027348f42e93d851abce1e851统一名单切换的连接调整，避免旧账户差集清理和新端点计划重复处理；空pending名单也立即更新连接。测试辅助函数同时模拟真实active_schedule与reachable子集，不改原断言。修复后14项/312条全部通过。

旧3a3f9361头文件负对照，保留真实签名与当前测试，准确复现6条应拒绝却接受的断言（改端点、伪造initial、轮换旧key、撤销key、过期、9个端点），新版同断言通过。报告脚本曾因查找英文failure而非Boost实际failed停止，保留ORACLE_REPORT_FORMAT_FAILURE_1.json，改为逐项核对六条精确失败后重新完整执行，未放宽单元断言或安全检查。ASan/UBSan、合约构建、原生链和三机自发现资格仍待后续通过，尚未部署、未提升基线，节点TPS未测。证据/opt/aic-bp-discovery-parallel-qualification-20261030/。


#### 并行路径的实际执行证据

未确认路径隔离轮的两个验证节点退出统计均为43363 batches、1040000 attempts/succeeded/adopted、serial_fallbacks=0；preparation_deadlines、worker_deadlines、其他失败和prefix_refresh均为0。故该轮完整区块交易确实走了并行验证，而非仅配置开关开启；这些是本地累计尝试统计，可能包含之后回滚的块，最终104万唯一交易确认另由独立账本审计证明。


#### BP 组件及构建检查最终状态（尚未激活）

候选node源码0cfbc1a245ea7af027348f42e93d851abce1e851：23项编译/运行任务按各自预期全部通过，包括14普通组件312断言、原始安全/空名单负对照、改变的缓存/真实签名验证代码ASan/UBSan；并非全nodeos或生产链整体插桩。系统合约候选a240fc708a0088cafd985f19a619163598d1f7ab编译成功，WASM eea11729b51d4c3eb72834345eaf7492373bc94bac664874c7186ebc696e471d，ABI bb99f6415d43531de47d3dbe0974a9c6a97151916c7d37d67a20a24d95edf851。原有87动作、37表、145结构及其他ABI既有条目逐项完全一致，仅新增regpeerkey/delpeerkey/getpeerkeys、peerkeys表及必要类型/返回值；原发布WASM fc639b...未改。合约构建在缓存类型校验处曾停止，实际version-check为顶层传入的UNINITIALIZED=ON并在contracts/CMakeLists中使用；修正报告校验类型后复核通过，未关闭版本检查、未修改合约源/功能开关。

nodeos四个改变/前端TU重新编译、三个静态库更新、最终链接共8项全通过；候选二进制SHA256 fed399d5b57b1b897f4ccabe886ade152967b312cebd7d0524b819272616c129。最低硬件和强制并行前端保持已验证版本。尚未部署候选系统合约，原生链动作/内存/权限与三台真实BP发现、密钥撤销/轮换资格待测；P2P传输积压仍待解决。节点TPS未测，不提升开发基线，不宣称自发现已启动。所有失败和恢复日志均保留在/opt/aic-bp-discovery-parallel-qualification-20261030/。


### 2026-10-30 功能专测：BP自发现冷启动重试

用户追加：三机激活后在同三机模拟约30个独立并行节点，仅低频功能测试；包括永久黑名单、恢复、系统动作及真实第三方DeFi/NFT。详细计划p2p-discovery-multinode-functional-test-plan-20261030.md共13轮134项（18项掉线/黑名单），另有90个系统ABI动作盘点。后续本轮不跑压力、不评TPS。

R01原生准备完整245断言通过，部署真实系统扩展、3个实际生产/最终确认者、正常质押/投票/PowerUp账户，验证peer-key权限、K1/R1、RAM与只读。R02尝试1为部署工具错误（不存在的OC选项及校验文件路径错误），原配置与日志保留。R02尝试2真实三机普通出块/LIB均正常且达到1300，但每机gossip仅本机1条；首条握手发生于本地公钥缓存就绪前，初始消息未发送，后续未重试，判FAIL并保留原链和safety。

1340fbfd为本地initial待发重试中间源码，未单独构建/测TPS；7041fb612454179c475a71ce278c7d57035833dd进一步保持未验签连接普通计数，只改net_plugin.cpp，未放宽认证、预算、链资源或硬件检查。新版本正在独立构建及全新chain ID三机a3功能回归，旧失败不覆盖。不宣称已激活或提升基线；节点TPS未测。证据/opt/aic-bp-gossip-startup-retry-20261030/及/opt/aic-bp-discovery-live-20261030/rounds/R02/GSP-02/attempt-2/result.json。

### 原生fixture编译头文件一致性修正（2026-10-30）

30节点准备第一次失败：terminate-at-block=1异常。复查发现测试helper继承的compile_commands指向/work/source，容器该目录仍是缺少六个group8配置字段的旧头文件，与链接的已固定group8库不一致。原三节点原生helper证明撤回资格，正在用明确固定源码和build路径重建并重测；独立三机RPC转账、链上hash和网络证据保留，不代替原生检查。证据：/opt/aic-bp-discovery-live-20261030/NATIVE_HEADER_AUDIT.json。此项不测TPS，也不提升基线。

### 复核与SEC-04真实失败（2026-10-30）

已查原三节点fixture-v5 unit：显式绑定固定源码到/work/source和固定build到/work/build，原准备的头文件原本正确；此前撤回原生证明的预防性状态已纠正。新30节点helper第一次缺绑定的问题已修复，明确路径复核：三节点245/245、30节点563/563断言通过。

SEC-04 attempt-2在原210秒时限内失败：peer-key重新登记且最终确认后，一台节点仍缺该BP签名发现记录。保留失败及全部缓存观察；正在以0b2d4a1ba1943e8e9b1a3054c74aabcfc52e1bfe修复远端密钥缓存更新未触发重新获取签名记录。不放宽时限、签名或资源限制，不提升基线。TPS未测。证据：/opt/aic-bp-gossip-roster-resync-20261030/REPRODUCER.json。

## 2026-10-30 候补断种子修复与计划维护开发

本段是功能/安全修复，节点TPS未测量；CPU16、NET16MiB/块、P2P64MiB/消息、8worker/batch24/group8不变。实际硬件仍为三台EPYC4345P，8物理核/16线程、64GiB，30进程仅作功能模拟。

候补在无出块/最终确认职责时，原实现只依赖静态种子，种子同时离线后6个候补停止追块；原M30-08失败保留。修复生产源码ce13ad23934dda44d418d8c6dd9882ba73496c97；完整测试源码4ede04b47318fdff90389a0450098bc94dd9d3c0；Node SHA256 6e9c668586dfae11f82a461028a734704e853b32f7acc1232559ffbf4de68cf7。候补从已签名且当前/待生效权威中选择最多3条发现连接，不可达10秒换组，现有身份校验、撤销、静态连接保护保留。组件全套、精确旧版本反例及变更代码ASan/UBSan通过。30实例保留原数据/safety逐个升级；两种子+第三进程停止时其余27节点在原60秒确认预算内确认低频转账；恢复后30节点区块一致。证据：/opt/aic-bp-standby-seed-failover-20261030/live-qualification/QUALIFICATION.json。基线尚未整体提升。

组件测试先因模拟条目签名相同导致去重而失败，修正独立签名后又发现测试chain_id默认构造不可用；两次测试代码错误日志及源码分别保留。生产Node字节未因此重建或改变。

维护扩展79b36c44eba984a9c44124cb7a16e20b13f3a657已编译；原ABI条目全部保留，新增setmaint/bpmaint。WASM SHA256 8758312bc222ba3ec5170383be60ea1daf42e5d423de90dafd7f9e95cb4e6c16。初次编译缺少rotation.hpp依赖已修复，错误保留。Native首次新增奖励检查错用上游producers表名（本链为producersv2），属于测试脚本错误，已纠正并重跑；尚不能宣称维护交接合格。实现与原始发布WASM独立保存，未部署入当前30节点链，未提升基线。

另有真实未解决缺陷：隔离新链Native全局停摆378秒（原504秒保护阈值内）后20个BP被永久ban，证据/opt/aic-bp-blacklist-pause-lab-20261030/fixture/BAN12_RESULT.json。该失败阻止发布/整体基线提升；不得用正常转账或网络测试覆盖。全网停摆暂停个人计罚的业务口径仍需确认。

Native维护复测4391/4391断言通过（13个明确列出的检查组，不能冒充新增20项全部验收）。证据：/opt/aic-bp-maintenance-feature-20261030/native-tests/native-build/attempt-1793327621050395735/prepare-real-contract-fixture.log 及 fixture/MAINTENANCE_NATIVE_RESULT.json。检查涵盖本人权限、非BP拒绝、候补维护、RAM payer、重复申请/恢复拒绝、发现名单排除、跨动作原子回滚、在岗draining与历史积分保留、双政策释放且检查点最终确认后有效、两轮不新增奖励或职责处罚、全部9候补挂起时仍保持21+21、取消恢复。真实多节点维护和历史违规/永久ban等新增边界尚待验收；候选仍未整体提升基线。

R05网络功能轮NET-01..09通过（NET-03引用本轮30节点原目录滚动升级证据），BAN-17证明P2P拒绝不会写链上永久ban。原始证据：/opt/aic-bp-discovery-live-20261030/rounds/R05 与 /opt/aic-bp-discovery-30nodes-20261030/NETWORK_FUNCTIONAL_PASSED.json。确认少数隔离仍有门槛、两侧不足门槛不产生新最终确认、恢复一致；注入50ms仅命中自有P2P流，临时nft/tc规则均清理。无压力/TPS测量。

第三方部署首两次错误分别为脚本误用治理封存后的eosio@active（系统正确拒绝），以及跨合约权限引用尚未创建账户且列表未排序（系统正确拒绝）。均属准备脚本错误，原失败保留。已改为正常资金充足developer建12字母账号、账户自质押投票、所有账户创建后再按排序添加明确eosio.code授权；不绕过治理、不扩大链参数。


## 2026-10-30 三机功能验证续记（2026-10-30T03:53:36Z）

这是进度记录，不是全部验收完成或发布批准。原13轮134项，加维护第14轮20项，共154项。当前状态计数：{"PASS": 62, "TODO": 84, "SKIPPED_USER": 1, "FAIL": 1, "PARTIAL": 6}。TODO和PARTIAL不计通过；各次原始FAIL保留。当前所有工作均为功能/安全验证，节点TPS未测。

硬件仍为原三台EPYC4345P（各8物理核/16线程、64GiB），未新建云服务器。原应用链65cc0acd...保留运行；维护验证用独立链c3e3a6e8...，各30个独立并行nodeos、独立数据和safety目录，曾在同三台主机并存。不能把共享硬件模拟当30/60台实机性能结果。CPU16=3200000us，NET16MiB/块，P2P64MiB/消息，生产队列1GiB；8worker/batch24/group8，单笔和计费规则不变。RPC读取到额外max_action_return_value_size=256，原值保留；第一次严格字典相等检查因preset未列该扩展而失败，属于测试断言形状错误，无参数变更。

Node生产源码ce13ad23934dda44d418d8c6dd9882ba73496c97，含完整测试源码4ede04b47318fdff90389a0450098bc94dd9d3c0；Node SHA256 6e9c668586dfae11f82a461028a734704e853b32f7acc1232559ffbf4de68cf7。候补断种子修复：组件和变更代码ASan/UBSan通过；30实例保留数据/safety滚动升级、两种子+第三进程停止时其余27在原60秒内确认转账，恢复后全30区块一致。证据/opt/aic-bp-standby-seed-failover-20261030/live-qualification/QUALIFICATION.json。

功能模拟启动器过早绑定主线程，导致后建worker继承单核亲和性；已删除这一步并将当时30实例全部线程恢复允许16逻辑CPU，随后全30转账确认通过。没有改Node字节或链额度，也没有据此宣称历史TPS提升。证据/opt/aic-bp-discovery-30nodes-20261030/worker-affinity-fix/QUALIFICATION.json；后续维护实例从启动起全部CPU可用。

R05 NET-01..09及BAN-17通过：断连/重复交易一次记账、单节点P2P隔离、满足/不足门槛分区、恢复同链、错误链/坏签名/64MiB+1长度头拒绝、真实自有P2P50ms延迟和队列检查。临时nft/tc故障规则已撤回。证据/opt/aic-bp-discovery-30nodes-20261030/NETWORK_FUNCTIONAL_PASSED.json。

真实固定版本AtomicAssets、AtomicMarket、Bancor Token/Converter/Network五份WASM已部署，30节点链上code hash和ABI一致。NFT-01/02/04/05完成；NFT-03仅权限、重复ID、错误scope等已执行子项，伪造通知未完成。DEF-01/02/03/05/09完成；DEF-06/07仅部分数量和充值提现子项。单跳正反兑换按固定公式和最小单位核对；原inline深度4拒绝直接双跳，失败整笔回滚，未扩大限制；同一交易内两个顶层动作组成两池兑换通过。跨合约兑换后第二动作失败，全部钱包/储备回滚。其余DeFi/NFT/VM/系统ABI支持动作仍需按计划覆盖，不能称所有第三方合约安全。证据C目录third-party-live及B/rounds/R09、R10。

第三方准备脚本误用治理封存后的eosio@active，和账户未建立/权限列表未排序，均被原权限检查正确拒绝。改用正常资金developer建12字母账户、账户自质押投票、建立全部账户后排序加入明确eosio.code权限。NFT schema遗漏必需name:string、模板同时不可转让和不可销毁，均为测试输入错误；查固定源码后修正，原失败保留。没有绕过治理、改合约安全检查或放宽链上限。

维护当前候选源码63255178dd824948afc723859cebe9321691a0e9，WASM SHA256 6ac9dafc4c74fb47f9ce1897be65d98cfc30ddf2c66cfe68b51ca623c8db435d，ABI SHA256 6809814cb91cdfb55e9cf9bfb926343b9a8be02f82b8996796e11173eec40a9c。原ABI条目全保留，仅新增setmaint/bpmaint；单选、轮岗/双权威、10/90、奖励减半/治理、RAM64GiB初始+setramrate1024、deferred关闭不变。新版本Native39971/39971断言（13个检查组）通过，包含每次交接中当前proposer/finalizer发现身份检查。数字是断言数，不是TPS或39971种独立场景。

6325517额外显式加入当前Native finalizer身份并集，属于保守权威覆盖。旧79b36c4 WASM对同一属性Native对照也通过41627断言；没有复现旧版本在当前profile缺失finalizer的反例，不得称已证明旧版本存在此缺陷。对照评估/opt/aic-bp-maintenance-feature-20261030/finalizer-identity-negative-control/CONTROL_ASSESSMENT.json；原错误的“必须失败”控制器断言保留。

历史权益成熟路径通过独立新创世Native虚拟25小时验证，原24h常量不变：维护时结算先前24个出块积分，钱包8000.0000→8062.8918 SYS，维护状态仍2，重复领取拒绝；整组64454/64454断言通过。真实30节点链尚未满注册24小时，原冷却拒绝且历史积分/钱包不变，是正常行为；最初用例错误期待立即可领的FAIL保留。未来时钟实验链绝不部署为live。证据/opt/aic-bp-maintenance-feature-20261030/reward-maturity-native-lab/fixture/REWARD_MATURITY_RESULT.json。

原应用链bpaac已在较早滚动故障期间形成真实三次个人零窗永久ban：evidence_slots=[1693278720,1693278972,1693279224]，同policy23，ban_final_block11771。该角色保留，不清洗。M30-06第一次查询误用voters而非votersv2；下一次210秒期望所有30个进程均持BP发现记录也不正确：29个合格BP已恢复，唯一缺失是已ban普通观察节点，其普通P2P/账本同步正常但BP发现权限没有恢复。已修正按实际角色检查，业务最终确认仍含全部30实例；原FAIL保留。此事不能宣称为新的Node发现bug或自动解封。

仍有全网378秒暂停后20BP永久ban的隔离旧WASM失败（BAN-12），/opt/aic-bp-blacklist-pause-lab-20261030/fixture/BAN12_RESULT.json。全网停摆个人计罚口径已向用户提出选择，未收到明确答复前不擅改这项业务规则。该问题及所有未完成项继续阻止整体基线提升/发布。

最新Node和系统合约源码已保存到本机Git review refs，实验分支已有Git提交，稳定基线未被未经验证的候选覆盖。本机仅接收源码、明确要求的Markdown和小型hash回执，没有拉回原始运行归档或本机编译/性能分析。

清理约束再次确认：完成必要扩展多节点测试、保存详细项目/结果并核对hash后，DELETE新增db8ee7ed-2357-436c-8614-d0f485f52ed6（66.245.194.132）及e93d8e9e-43df-4a7b-b203-38e94b612f2e（136.244.110.182）。只保留原机3b0155c9-16ef-48e9-afb3-4c5e838e5184（66.245.195.50）。目前未删除，不用关机代替删除，也不删除其他服务/机器。


## 2026-10-30 全网停摆误罚修复：用户已明确授权

用户本次明确要求：全网停摆不能把仍运行的正常BP误加入永久黑名单；修复后再测试。此前等待业务口径确认的状态已结束。该保护不降低最终确认门槛，不允许候补在不足当前法定票数时自行接管，也不清除此前有效永久封禁。

独立系统合约候选6f3160ed23b725e8cd97790f7a0e9042afbfdc9a，父版本63255178dd824948afc723859cebe9321691a0e9。只改rotation_growth.hpp和rotation_state.cpp：将完整21人排班一轮252slot（126秒）作为链/最终确认观察中断保护边界，包含等于边界；进入保护时丢弃未结算/不完整观察，不再递增penalty_epoch从而清洗此前已结算的一/二次个人违规。恢复后从新的完整个人窗口重新观察，原三次已最终确认个人零窗口永久ban规则保留。这个126秒是保护判定边界，不是一个精确识别任意短暂“全网停摆”根因的探针；短于边界的间断仍按原窗口观察，并已加入125秒无误封回归。

新WASM5f25a25816fc6db145dbd811d9c24dd99c9b9b1fd260b8e16a045476d0c3c11a；ABI6809814cb91cdfb55e9cf9bfb926343b9a8be02f82b8996796e11173eec40a9c，与父维护候选完全相同。原发布WASM与原失败均保留。当前已完成原378秒用例匹配负对照：父维护版本仍误封20人（原测试FAIL，573/574），新候选0人误封（574/574）。125、126、127、378、504、3600秒边界均无新永久ban；个人历史证据和最终确认暂停检查、原维护完整回归及独立三机30实例验证仍在执行，尚不能宣称整体资格通过或发布。

实际机器仍为三台EPYC4345P、各8物理核/16线程/标称64GiB；所有live nodeos并行8worker/batch24/group8，CPU16=3200000us、NET16MiB/块、P2P64MiB/消息、1GiB生产队列、RAM1024初始化和deferred关闭保持。节点TPS未测。独立时钟跳跃Native实验根目录禁止部署为live；后续live只能使用正常维护fixture在真实时钟追平后的快照。已有应用链65cc0acd...保持原版本运行；该候选尚未整体提升。证据/opt/aic-bp-global-pause-guard-20261030/。


## 全网停摆误拉黑修复及定向验收（2026-10-30T07:29:43.005409+00:00）

用户已明确授权修复和测试，原“全网停摆计罚口径待确认”不再是待决事项。独立候选6f3160ed23b725e8cd97790f7a0e9042afbfdc9a，父63255178。WASM 5f25a25816fc6db145dbd811d9c24dd99c9b9b1fd260b8e16a045476d0c3c11a；ABI 6809814cb91cdfb55e9cf9bfb926343b9a8be02f82b8996796e11173eec40a9c 与父版本逐字节相同。仅改两份轮岗源文件，原发布WASM fc639b...本次重新核对哈希且保留。

保护边界为完整21人排班一轮252slot=126秒，含等于边界。链/确认观察中断达到边界时暂停新增个人缺席计罚，不补造停摆窗口，不因暂停本身清洗此前已结算1/2次违规或永久ban。恢复后从新完整窗口观察；第3次真实、已最终确认个人零窗口仍可永久封禁。短于126秒仍按原窗口规则判断，该保护不识别任意短暂故障根因或操作系统开关机状态，不降低确认门槛，也不允许缺票时由候补自行接管。

父版本同一378秒Native用例误封20BP，573/574断言、原FAIL保留；新候选574/574、误封0。扩展Native一项用例2926/2926断言、11组检查通过：125/126/127/378/504/3600秒、真实缺确认密钥的最终确认暂停/恢复、保留此前1/2次有效违规、第3次真实违规封禁、既有ban及证据跨重启/停摆保留、维护不能清除ban。原维护回归44387/44387断言通过。断言数不是独立场景数或TPS。

三机30实例先完成130.906秒边界实测，再保留原数据/safety恢复同一候选链，暂停出块378.893秒；链上两个相邻且已最终确认区块的实际时间间隔为381.000秒，仍低于原504秒保护阈值。30进程前后PID相同并保持运行，没有新增永久ban或已结算个人缺席次数；恢复后在原60秒预算内全体最终确认，低频业务余额和共同已确认区块一致。

所有30进程8worker/batch24/group8，全部线程允许16逻辑CPU；三台均EPYC4345P，8物理核/16线程/标称64GiB。CPU16/NET16MiB、P2P64MiB/消息、1GiB队列、RAM1024和禁用deferred保持。仅功能验证，不测TPS，不作为30台独立机器的性能资格。

所有测试工具失败和源码均保留：空密钥参数不会动态删除已加载密钥；时间测试开关不阻止recent块投票，改为隔离控制器重启并验证全部相关密钥未配置及LIB稳定。私有getter和宏模板逗号两次编译错误保留并修正。两次整行比较错误包含可变队列ticket；完整原始行与远端独立复核证明只有ticket变化，所有处罚/ban字段不变，失败维护调用的bpmaint字节不变。未放宽生产共识、授权、签名或黑名单不变式。

证据：/opt/aic-bp-global-pause-guard-20261030/QUALIFICATION.json；实机：/opt/aic-bp-global-pause-guard-20261030/live30/MATCHED378_QUALIFICATION.json。总体154项当前状态{"PASS": 68, "TODO": 77, "SKIPPED_USER": 1, "PARTIAL": 8}，其余TODO/PARTIAL不计通过。该候选尚未整体晋级开发基线，原应用链65cc0acd...继续使用原系统WASM并正常最终确认，历史有效ban保留。节点开发基线仍本机eec21c3f/服务器44ab5dcc。新增候选测试链已停机，原数据/safety保留；三台机器仍保留，完整功能项目和报告验收后才删除新增两台。


准备与中断记录补充：隔离源首次Git提交因身份配置以128退出，未开始构建，修正仅限本次提交身份；Native等待阶段停止过一次尚未启动节点的launcher以校正暂停时长元数据和自有网络清理。既定130秒实机按原方案完成后，再以同一数据/safety做378秒匹配验证，未中途延长已开始的用例。全部相关回执、七次扩展Native helper源码/日志（六次工具失败及最终通过）保留在/opt/aic-bp-global-pause-guard-20261030/。生产源始终为6f3160ed，Node TPS未测。


## 20261030-final-functional-00443330-sub21 / 2026-10-30T14:16:30Z

全部功能项及原始区块、归档和逐笔成功业务审计已通过；关闭步骤FIN-07/FIN-08按各自回执登记。原先排除的BAN-07、BAN-10已按用户新要求完成三机实测：21人15票政策正常移除无候补的永久ban席位后成为20人14票；14名有效确认者在线持续确认，13票时停止最终确认，短断和长断后恢复，旧违规及ban保留；候补恢复后回到21人15票。此次不足21实测范围是20人政策，不覆盖1至20所有人数/所有组合。

节点源码00443330a0d29673700a4eebee78de3b0234c281，二进制e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32；系统源码6f3160ed23b725e8cd97790f7a0e9042afbfdc9a，WASM 5f25a25816fc6db145dbd811d9c24dd99c9b9b1fd260b8e16a045476d0c3c11a，ABI 6809814cb91cdfb55e9cf9bfb926343b9a8be02f82b8996796e11173eec40a9c。PureEOS CDT代码d5816c442dd03db13413645f99c61a01c82fdda6保持，展示名/新目录PureEOS CDT，重编译WASM/ABI字节一致。原发布WASM fc639b60009090cb66424a58b9112fdb87309391a286905a0669ae3397289186保留。

三台EPYC4345P 8物理核/16线程/64GiB，Ubuntu24.04.5宿主搭配匹配glibc2.44运行环境；30独立并行实例共享三机及额外非出块快照观察节点，8worker/batch24/group8，完整验证。CPU16/NET16MiB、P2P64MiB单消息、1GiB队列、单笔CPU150000us/NET524288bytes/minCPU100us、RAM初始64GiB/setramrate1024、单选无代理、10%/90%、减半、14天治理和deferred禁用保持。仅功能资格，没有新增TPS测量；历史吞吐/失败/串行记录保留原版本和范围。

逐笔审计：声明成功的549个唯一交易ID均在最终确认区块中，重复canonical ID为0，声明成功但未确认0；预期拒绝、原失败中较晚确认的包、原失败未入链的包分开。旧失败不因后续恢复而追认原60秒预算通过。九ABI共231入口映射为正例/原生/只读/实际拒绝，废弃deferred入口排除，入口覆盖不冒充全部业务配置正例。

明细：/opt/aic-bp-full-functional-20261030-r1/FINAL_REPORT_PRE_CLEANUP.md；用例目录：/opt/aic-bp-full-functional-20261030-r1/input/FUNCTIONAL_CASES.json；三机逐文件SHA及全部30实例共同区块原始字节：/opt/aic-bp-full-functional-20261030-r1/FINAL_ARCHIVE_QUALIFICATION.json；私有大归档只留主服务器。晋级回执：/opt/aic-bp-full-functional-20261030-r1/DEVELOPMENT_BASELINE_PROMOTED.json。发布及额外两机清理须见最终关闭回执。


## 最终发布与关闭 / 2026-10-30T14:37:49Z

154/154项目闭环通过（含BAN-07/BAN-10实机补测），没有跳过用例。原生/组件/实机与历史证据按各自范围保留；弃用deferred仍不执行。节点交付0b78fb08a5ca808b7caf4c0d4ecadda9ea4921d9、编译来源00443330a0d29673700a4eebee78de3b0234c281，CDT交付b83d664ad5c05029bd509065e699436138fb98dd、编译代码d5816c442dd03db13413645f99c61a01c82fdda6。PureEOS与PureEOS CDT日期发行均已公开发布（CDT仓库保留原私有权限），14个发行资产下载检查全部SHA256一致。

PureEOS: https://github.com/MatrixCoreX/PureEOS/releases/tag/baseline-2026.10.30 
PureEOS CDT: https://github.com/MatrixCoreX/PureEOS-CDT/releases/tag/baseline-2026.10.30 

私有完整归档及全部原始失败、数据、安全文件留原主服务器；额外两台及其临时云SSH公钥条目已删除，列表和资源API两次确认不存在，原主服务器保留。原发布WASM、旧tags/assets、历史TPS范围不改写。本轮没有新增TPS测量。发布核验回执：/opt/aic-bp-full-functional-20261030-r1/PUBLISHED_BASELINE_DOWNLOAD_VERIFIED.json。


## 2026-10-31 exact1pct current-node pressure / 20261031-single-node-exact1pct

独立链单轮104万笔全部不可逆；固定入块36436.33TPS、不可逆36591.00TPS、整批不可逆36919.53TPS；163854余额账户及同高度状态哈希独立审计通过。原线程CPU监测错误值已撤回，主线程未测。 本次无新的主线程优化候选或基线变更；实际EPYC4345P 8物理核/16线程/64GiB，源码00443330/nodeos e7776703/系统6ed4cccc，CPU16/NET16MiB/P2P64MiB，准备均在计时前完成，全部原始失败/重试及范围见 /opt/aic-performance-results/machines/amd-epyc-4345p-8-core-processor--baremetal--8physical-16threads--65536MiB/20261031-single-node-exact1pct/REPORT.md。


## 2026-10-31 exact1pct current-node pressure / 20261031-browser-visible-stress

既有浏览器链保持运行，93万笔纯转账与65万笔90/10混合全部不可逆；纯转账固定入块29913.03、固定不可逆29425.90TPS。混合结果与纯转账分开；全部跨合约对账通过。 本次无新的主线程优化候选或基线变更；实际EPYC4345P 8物理核/16线程/64GiB，源码00443330/nodeos e7776703/系统6ed4cccc，CPU16/NET16MiB/P2P64MiB，准备均在计时前完成，全部原始失败/重试及范围见 /opt/aic-performance-results/machines/amd-epyc-4345p-8-core-processor--baremetal--8physical-16threads--65536MiB/20261031-browser-visible-stress/REPORT.md。


## 2026-10-31 NET16 original overload archived with failures

# NET16旧轮归档 / 2026-10-31

用户要求重置新链，旧轮以已知失败状态归档，未宣称通过。旧链e7ea3de60c86af008d29cf92461e0b1ae4b5153a167325908272ad64aa0299db、首BP bpnodeaaaaaa，EPYC4345P8物理核16线程64GiB、Ubuntu24.04.5、CPU16/NET16MiB/P2P64MiB、全部并行。

60000目标纯转账实际固定发送60000TPS，1860000写出、885365不可逆、974635未确认。固定入块23998.33、固定不可逆23878.97TPS，未达40000对照；主线程94.90%，历史栈同机4.3796核当量。已确认账务通过，P2P每连接100MiB在途保护有入口丢弃记录。

混合60000目标在15.944秒因客户端1024未完成写入保护中止：954986写出、381389确认、573597已写未确认、2945014未写。60秒窗口未完成，不提供其固定窗口TPS。只读恢复检查实际已确认交易和33项复杂状态通过。第一次供应量检查遗漏bootstrap两个testct账号各1000AIC，历史证明均在压力前且期间不变，修正后1亿供应量严格守恒；原失败保留。

首个30000目标轮902018确认/27982未确认。PureHistory逐笔复核发现该轮2048条文档缺失，分布2082和2098块各1024；原链canonical/SHIP完整，索引完整性未通过。缺口与原始失败、Redis AOF内存修复、发送器上限修复等全部保留于/opt/pureeos-browser-visible-stress-12char-20261031-r1。旧链目录/opt/pureeos-explorer-devnet-12char-20261031-r1，旧历史物理归档/opt/pureeos-hyperion-20261031/archive/chain-e7ea3de60c86af00-self-net4-20261031-r1/data。

后续按NET4和自己质押/PowerUp新规则建立新chain ID，结果另计，不改写本轮数据。



## 2026-10-31 NET4 self-owned resources / two80000 offered / history paused

# NET4 / 自付资源 / 两轮80000投送TPS · 2026-10-31

两轮实际执行及已确认账务/历史核验通过；未确认投送按用户要求只记录数量，不补发，不当作成功TPS。40000是本轮对照基准，是否达到以实际固定窗口数据为准。

| 指标 | 第一轮 | 第二轮 |
|---|---:|---:|
| 实际发送TPS | 80000.08 | 80000.00 |
| 固定入块TPS | 31707.50 | 32105.50 |
| 固定不可逆TPS | 31551.33 | 31447.00 |
| 整批已确认交易不可逆TPS | 30018.02 | 30677.46 |
| 节点CPU逻辑核当量 | 3.40 | 3.32 |
| 主线程CPU% | 96.84 | 96.93 |
| 已确认笔数 | 562181.00 | 569731.00 |
| 未确认笔数 | 477819.00 | 470269.00 |
| 最大计费块CPU us | 2071762.00 | 2232833.00 |
| 最大计费块NET bytes | 2983248.00 | 3215232.00 |

每轮纯AIC转账1040000笔，80000TPS投送、1秒预热、12秒固定窗口、90秒后续排空，固定窗口事先锁定。发送/入块/不可逆分开统计，整批口径分子为实际已确认交易。约100ms本地head/LIB、0.5秒线程CPU采样；两个窗口独立且语料ID不重叠。此为短窗/单节点资格，不是长期或多节点持续容量。

原保留Vultr EPYC4345P裸机，8物理核16线程、标称64GiB、可见64880889856bytes，Ubuntu24.04.5及匹配打包运行时。没有新服务器。按用户授权，两轮计时期间PureHistory/ES/Mongo/RabbitMQ/Redis/API全停；nodeos继续保存SHIP trace/state日志。测完从新链创世重写回填后恢复实时历史，现全部依赖健康，公网HTTPS/CORS和首末交易查询通过。

当前链 `3da0d4adaf64140424652f95f0b65b2f113b5b142afaa3794fb041294c08711f`，首BP bpnodeaaaaaa，开发者govdevaaaaaa；16,000参与账号和8个初始普通账号全部12位，协议系统账号及拍卖保留，短号租赁/aic更名未实施。1600账号自己质押、14400自己付费PowerUp30天，自己签名；资金正常转账发放，初始化首笔可以由已有账号合并资助执行资源，但质押from/receiver与PowerUp payer/receiver均为受益账号，扣其资产并归其持有。已枚举全部1608条委托记录均from==to；BP自己质押10AIC NET/100AIC CPU。生成器对所有正向资源动作强制同名与本人授权。

CPU16=3200000us/块，NET4MiB/块，P2P64MiB/消息，生产者队列1GiB；单笔CPU150000us、最低100us、NET512KiB。8workers/batch24/group8、全并行、完整权限/签名/计费/逐笔冲突回退；RAM64GiB+显式1024bytes/块、deferred两阶段禁用、单选无代理、10%/90%、生产/确认/轮换/维护/处罚、精确1%计提及减半、14天治理保留。AIC初始1亿/上限21亿、额外4%不发行，其他发行方式未改。

nodeos源码00443330a0d29673700a4eebee78de3b0234c281，二进制e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32；系统源码432360edd6763be000626898435bffcfe1aaf866，WASM6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62。运行参数、配置SHA、PID、系统WASM测量前后相同；没有为计时重启/暂停节点。预生成签名、账号、RAM和资源准备均在计时外；发送端单连接、8192有界待写，仅测试客户端变化，节点每连接100MiB在途保护不变。各版本/语料/收据/时间序列精确SHA见EVIDENCE_MANIFEST.json。

两轮合计1131912笔已确认ID唯一、executed、canonical父哈希连续、CPU/NET逐笔逐块上限有效，全部跟踪余额精确核对并保持1亿AIC供应量。PureHistory逐笔核对这些ID与区块位置，缺失0、重复0；连续区块头/账户历史/详情通过。用户允许的是未入链投送丢弃，这与已入链交易的历史完整性分开验证。没有可用CUA浏览器，未把API验证称为GUI目视验收。

- [第一轮样例](https://aic.matrixai.one/#/transaction/4e8a859d9bd461e764b99b4e74a81c0ed2a20cbf3c75b3fef25b40e748bcd4bb?block=1101)
- [第二轮样例](https://aic.matrixai.one/#/transaction/5c69924e99f10f6c671382d8559d34c53c1dc435fa073813d40db3898e972acd?block=1476)

旧NET16轮及其客户端中止、Redis重写OOM和2048条旧索引缺口完整归档，没有改写为通过。新链为用户再次明确要求重置后新建，与旧结果分开。不声称本次是仅改变一个变量的历史开销对照或新主线程优化提升。全部原始数据保留于/opt/pureeos-browser-visible-stress-net4-self-20261031-r1；测试钱包已停止，节点、PureHistory及后端继续运行。



## 2026-10-31 Independent-state transfer and complex rounds, history paused

# 独立业务状态两轮测试 · CPU16/NET4 · 2026-10-31

两轮计时、已确认账务与历史核验完成。纯转账为本轮低冲突实测TPS；混合轮为业务稳定性吞吐。80,000TPS是目标投送，未确认只记录、不补发。

| 指标 | 独立转账 | 独立复杂业务混合轮 |
|---|---:|---:|
| 实际发送TPS | 80000.00 | 80000.00 |
| 固定窗口入块TPS | 29611.58 | 22666.58 |
| 固定窗口不可逆TPS | 29763.58 | 21877.75 |
| 整批已确认不可逆TPS | 28529.12 | 21788.14 |
| 已确认笔数 | 534651.00 | 424560.00 |
| 排空后未确认笔数 | 505349.00 | 615440.00 |
| 节点CPU逻辑核当量 | 3.22 | 2.68 |
| 主线程CPU% | 96.92 | 89.46 |
| 最大计费块CPU us | 2121029.00 | 1596840.00 |
| 最大计费块NET bytes | 3054240.00 | 2862304.00 |

每轮1,040,000笔，1秒预热、12秒事前固定窗口、90秒排空；约100ms head/LIB与0.5秒线程CPU采样。整批分子只用已确认交易。短窗单节点用于理想业务工况参考，未宣称理论最大值、长期持续或多节点容量。

81,920对、163,840个新增12位账户；不同账户对使用独立余额、NFT记录与RAM付款人。语料轮转时复用账户对，实际同块复用分别为0/0，连续24项分组复用分别为0/0。详见INDEPENDENCE_AUDIT.json。该分组审计不代表内部调度器冲突计数；共享只读配置/合约地址与业务写冲突不同，链级共享开销仍在。

混合语料90% AIC转账、5% AtomicAssets NFT往返加TKN支付/退款、5%跨两个代币合约的多动作原子结算；每个复杂事务5个顶层动作、2个签名，NFT另有inline日志。4096个NFT预先分别铸给独立付款人，集合/模式/铸造均在计时外。计时内不修改共享AMM池、铸造计数器、RAM市场、REX或PowerUp池。AtomicAssets未加入并行code-hash白名单，原安全回退保留；业务状态独立不承诺全部WASM走并行快路径。实际接受比例见kind_counts。

准备/部署/RAM/签名在计时外。新增16384个账户自己质押、147456个自己付款并授权PowerUp30天；全链17994条委托from==to，新增租赁账户都有本人订单。50000AIC初始化资金来自已有testusaaaaab转给testusaaaaaa，没有额外发行，没有改动封存的eosio权限。按实际不可逆ID精确核对全账户AIC净支付并保持1亿供应；相关TKN余额/供应与4096个NFT所有权/数据/RAM付款人、NFT全局配置/计数器保持不变。超额支出失败时AIC与NFT原子回滚通过，攻击拒绝不计成功业务。

原Vultr EPYC4345P裸机，8物理核16线程、标称64GiB、Ubuntu24.04.5。同机投送；两轮计时PureHistory及全部六个后端容器停止，节点SHIP记录保留。链未重置；准备前只将本地状态库容量1024扩到8192MiB并重启一次容纳新增账户，测量中PID/配置/二进制一致。工作集扩大也改变状态规模与缓存，本轮和上一轮的差异不能全部归因于冲突；上一轮同块账户重复不直接证明同一并行批次冲突。

CPU16=3200000us/块、NET4MiB/块、单笔CPU150000us/最低100us/NET512KiB，P2P64MiB消息/每连接100MiB在途保护/1GiB生产队列、8workers/batch24/group8、完整授权/验签/计费/冲突回退均保留。RAM64GiB初始+1024bytes/块、deferred两阶段禁用，全部既有投票/治理/奖励/生产/确认/轮换/维护/处罚模式未变。没有节点主线程优化或基线晋级。

node源码00443330a0d29673700a4eebee78de3b0234c281，实际libexec/nodeos SHA256 e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32；系统源码432360edd6763be000626898435bffcfe1aaf866，WASM6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62。应用WASM/ABI复用来源、生成器构建与全部证据SHA见provenance/manifest。当前链3da0d4adaf64140424652f95f0b65b2f113b5b142afaa3794fb041294c08711f。

原始失败保留于attempts：生成器fc::variant索引编译错误；钱包空HTTP参数；重试目录已存在；封存eosio权限无法外部转账。这些准备尝试没有计时TPS。修正后使用现有普通测试账户资金，链安全规则不变。

两轮合计959211笔不可逆交易已在PureHistory按ID及原始区块位置逐笔核对，缺失0、重复0；区块头连续性、账户历史、NFT inline详情和公网HTTPS/CORS通过。六个后端、历史资源保护定时器恢复，测试钱包停止；API核验不冒充GUI目视验收。

- [独立转账样例](https://aic.matrixai.one/#/transaction/1e58f9a231915ccbabe17ae1aba57c7e6a75906dd3be49f9bf8b858a0fb68e1f?block=50424)
- [混合轮样例](https://aic.matrixai.one/#/transaction/dcbd0dac113226078681cab9d6f6a711ebb22df47b50cf3da73f48d269da9929?block=50893)

原始语料/收据/账务/采样/失败证据保留于/opt/pureeos-independent-net4-20261031-r1；私钥、钱包、完整配置不复制到公开结果目录。



## 2026-10-31 Selected NET4 DB16 eight-connection SHIP off/on pair

# SHIP关闭/开启：纯转账配对对照 · 2026-10-31

固定其余条件后，开启SHIP的入块TPS下降23.20%，不可逆TPS下降23.80%。本轮显示SHIP是明显吞吐开销来源；一对短窗口不等于长期容量或统计重复均值。

| 指标 | SHIP关闭 | SHIP开启 |
|---|---:|---:|
| 固定窗口入块TPS | 36567.83 | 28084.50 |
| 固定窗口不可逆TPS | 35197.17 | 26819.50 |
| 整批不可逆TPS | 37310.57 | 29370.76 |
| 实际发送笔数 | 1040000.00 | 1040000.00 |
| 已确认笔数 | 1040000.00 | 1040000.00 |
| 排空后未确认 | 0.00 | 0.00 |
| 节点CPU逻辑核当量 | 4.66 | 4.53 |
| 主线程CPU% | 96.56 | 97.34 |
| 最大计费块CPU us | 2239748.00 | 2119744.00 |
| 最大计费块NET bytes | 3045992.00 | 2882792.00 |
| 窗口写入bytes/s | 23685614.57 | 43126576.27 |
| 峰值节点RSS bytes | 1810526208.00 | 1991577600.00 |

CPU16=3200000us/块，NET4MiB/块，状态库16384MiB，8连接，8workers/batch24/group8、chain线程6。每轮1040000笔AIC普通转账，80000TPS目标投送、1秒预热、12秒事前固定计时、90秒排空。约100ms本地head/LIB观测，0.5秒CPU/IO采样。入块/不可逆和整批分开，未确认不计成功TPS。

两轮复用同一个快照、同一链ID/初始块和逐字节相同的已签名交易语料；只有被测节点的SHIP trace/state日志开关不同。所有部署、RAM、自己质押/自己PowerUp、签名、预加载、构建、独立审计均在计时外；PureHistory/API/ES/Mongo/RabbitMQ/Redis及自动拉起/回填任务在两轮窗口全停。SHIP开启组不启动历史后台消费者。

两轮各104万笔都已不可逆，未确认0。逐笔检查原始native blocklog、规范链父哈希、execute收据、逐笔/逐块CPU和NET上限，并用逐字节验证过的停止后数据库副本独立核对163854余额账户、1亿AIC供应量、21亿上限、自有质押/PowerUp资源及最终状态哈希。原生断言数量不冒充独立场景数；签名/授权完整校验由原live node保留，审计另核对packed IDs/动作及真实收据。

SHIP关闭日志字节：{'trace_history.log': 0, 'chain_state_history.log': 0}；开启日志字节：{'trace_history.log': 696788148, 'chain_state_history.log': 309646201}。开启后生成约1GB轨迹/状态历史。实际编译源码已使用zlib::no_compression；插件仍有历史序列化、零压缩封装、写入工作；本对照没有函数级profiling，不将全部差异具体归到某个内部函数或磁盘带宽。

关闭SHIP时，NET4MiB也恢复到约3.66万入块TPS，因此这套纯转账的旧3.6万水平不要求NET16MiB。本轮没有单独量化1/8连接、8/16GiB状态库或完整浏览器链状态的作用；这些组按用户最新加速要求取消，不能将本轮结果解释为所有历史差额的完整分解。

复用历史参考fixture作为固定受控输入（其中普通账号为历史8位名称，闭环测试专用，不对外发布/新建用户账号）；当前浏览器链及正式新部署脚本保持12位账号策略。此配对测试在独立、仅loopback的测试链上运行，未重置或改动浏览器链NET4基线。私钥/钱包/完整配置不复制到公开结果目录。

实际机器为原AMD EPYC4345P裸机8物理核/16线程、标称64GiB、Ubuntu24.04.5及匹配打包运行时。node源00443330a0d29673700a4eebee78de3b0234c281 / SHA256e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32；系统源432360edd6763be000626898435bffcfe1aaf866 / WASM6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62。CPU/NET单笔规则、P2P64MiB/每连接100MiB在途保护、1GiB队列、完整验签/权限/计费/冲突回退、RAM64GiB+1024增长、deferred禁用和全部现有系统模式保留；没有基线优化晋级。

前期历史方法第一尝试与约1.895秒编译重叠，且计时后快照RPC超时，已排除性能结论并保留原始失败。后续被用户缩小范围的矩阵全部取消；正式结果只取本报告两组。收集改为最终integrity+停止后数据库独立审计，不在暂停出块时等待快照的最终性条件。

测后已恢复PureHistory与六个后端，修复回填时Rabbit1GB内存水位报警导致的发布停滞（容器6GiB/水位4GB，调整预取与积压阈值），不删除原区块、SHIP日志或索引数据。此前浏览器链两轮959211笔已确认ID已逐笔验证历史缺失0/重复0，公网HTTPS/CORS及NFT inline详情通过。此恢复不影响已结束的TPS窗口。

全部原始证据保留在/opt/pureeos-tps-cause-isolation-20261031-r1。输入、二进制、脚本、规范链、CPU/IO与收据SHA256见EVIDENCE_MANIFEST.json。


## 2026-10-31 Live public producer SHIP disabled, 12-character accounts

# 当前出块节点关闭 SHIP 确认测试 · CPU16/NET4 · 2026-10-31

当前公开测试链首个 BP `bpnodeaaaaaa` 关闭 SHIP 后完成一轮普通 AIC 转账测试。全部 1,040,000 笔已不可逆、原生账务与 blocklog 审计通过，并已逐笔核对历史索引位置；缺失和重复均为 0。

| 指标 | 实测 |
|---|---:|
| 目标投送 TPS | 80,000 |
| 固定窗口入块 TPS | 36892.67 |
| 固定窗口不可逆 TPS | 35810.17 |
| 整批不可逆 TPS | 37804.93 |
| 发送 / 最终确认 | 1040000 / 1040000 |
| 排空后未确认 | 0 |
| 主线程 CPU | 96.13% |
| 节点 CPU 逻辑核当量 | 5.01 |
| 最大块计费 CPU，μs | 2270427 |
| 最大块计费 NET，字节 | 3269376 |

1 秒预热、事前固定 12 秒计时、90 秒排空，8 个投送连接。准备账号、RAM、自己质押/自己 PowerUp、部署、初始化、签名均在计时外。81,920 对 12 位账号轮转使用；同一区块和按顺序划分的 24 项分组内账号对重复均为 0，不把该分组等同于调度器批次。新增自有质押账号 16,384，自付并授权 PowerUp 账号 147,456。公网链未重置，测量中出块节点保持同一 PID、配置和二进制。

实际机器为原 AMD EPYC 4345P 裸机，8 物理核、16 线程、标称 64GiB，Ubuntu 24.04.5，匹配打包运行时。CPU16=3,200,000μs/块，NET4MiB=4,194,304 字节/块，本地状态库 16GiB。8 workers/batch24/group8、chain threads6、1GiB 生产队列、P2P64MiB/每连接100MiB在途保护、单笔CPU150000μs/最低100μs/NET512KiB、授权/验签/计费/冲突回退均保留。允许每主机16个P2P连接以容纳8个loopback投送连接，未放宽消息/在途/生产队列保护。

计时期间非出块历史节点、PureHistory 及六个后端容器全停，出块节点不加载 SHIP。原出块节点历史日志大小和修改时间保持不变，详见 PRODUCER_SHIP_DISABLED_PROOF.json。没有 SHIP 代码优化或新节点版本晋级。原生审计核对全部1,040,000笔packed ID/动作/执行收据、规范链及179865个余额账户、1亿AIC供应量和21亿上限、自有资源、最终状态完整性；签名授权仍由实际live node完整校验。

计时后短暂停止出块进程复制并逐字节校验干净状态/区块数据，用独立原生程序审计，随后恢复原出块节点，保持SHIP关闭。非出块并行历史节点使用独立数据目录、RPC8890/P2P9877/SHIP8080，不配置出块身份或生产密钥，已核对规范不可逆块ID并追平。PureHistory从该节点消费SHIP，六个后端和资源保护定时器已恢复，HTTPS/CORS、区块头连续性、交易详情和账户历史通过。此前两轮1,131,912笔、独立转账/复杂轮959,211笔、此次1,040,000笔，共3,131,123笔已确认公开测试交易均已核对历史，缺失0、重复0。API验收不冒充浏览器GUI目视验收。

历史节点和服务仍在同一台物理机，正常运行时共享CPU、内存、磁盘；本轮峰值是在历史服务全停条件下测得，不能当作同时运行历史时的持续容量、多节点容量或40,000TPS保证。状态库16GiB是本地当前状态容量，与链上64GiB RAM市场不同；RAM每块1024增长、deferred两阶段禁用及既有系统投票/治理/奖励/生产/确认/轮换/维护/处罚模式均保留。

节点编译源码00443330a0d29673700a4eebee78de3b0234c281；nodeos SHA256 e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32。系统源码432360edd6763be000626898435bffcfe1aaf866；系统WASM 6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62。链ID 3da0d4adaf64140424652f95f0b65b2f113b5b142afaa3794fb041294c08711f。

- [交易样例](https://aic.matrixai.one/#/transaction/f27c31d232d2b619e199b810ffb92a9216b8c0a06846ddd19070b6f2dd2f9f37?block=58352)
- [最后一笔样例](https://aic.matrixai.one/#/transaction/a69fd149a8f1442f180ccc848766fbf422b3b38fbcbf0dd6263896c15661c6cd?block=58405)

完整原始收据、语料、CPU/head采样、原生审计数据库副本和失败恢复记录保留于/opt/pureeos-live-producer-noship-confirmation-20261031-r1及其前序campaign；公开结果仅同步脱敏报告、摘要、审计和脚本，不包含私钥、钱包、完整运行配置或数据库。


## 2026-10-31 Founder pressure and scheduled 30 BP functional demonstration

# Founder pressure and automatic 30 BP demonstration · 2026-10-31

Checkpoint phase: **JOINING**. The 24-hour campaign runs automatically; this checkpoint does not claim that observation has already finished.

- Browser: https://aic.matrixai.one/#/governance
- Observer: https://purehistory-api.matrixai.one/rotation-demo/state
- New chain: `03c98ab62ea764e74d2d80ac862b10f4d9c4dc1f0b7a3370692a44427f3f7aea`; first BP: `founderbpgpt`.
- Final topology: 30 independent parallel BP processes plus one non-producing parallel SHIP history node on the same retained server. Native policies eventually contain 21 active seats with nine eligible standbys.
- Joining starts `2026-10-31T10:22:17.477934+00:00` UTC, one-hour deadline `2026-10-31T11:22:17.477934+00:00`; observation ends `2026-11-01T10:22:17.477934+00:00`.
- Current snapshot: 11 registered, 11 online, 8 effective proposers / 8 effective finalizers, 24 irreversible state-change events.

The original 3da0d4adaf64140424652f95f0b65b2f113b5b142afaa3794fb041294c08711f chain, producer/history directories and index data are retained at ARCHIVE_RECEIPT paths. Fresh genesis and keys use isolated storage. System names and the qualified auction contract remain unchanged.

## Founder-only pressure round

1,040,000 sent, 1,040,000 finalized, zero unconfirmed. Pure AIC transfers: 80,000 offered TPS, one warmup second, fixed 12-second window, 90-second drain, eight sender connections. Fixed inclusion **20575.33 TPS**, fixed finality **21095.17 TPS**. Producer SHIP was disabled while the separate history node and all six history backends ran on the shared host. This is not sustained or multi-node performance qualification.

Accounts, RAM, self staking, self-paid PowerUp and signing were prepared before timing. Canonical receipts, full integer AIC ledger and caps passed. Every accepted ID and original canonical position matched history: missing zero, duplicates zero; HTTPS/CORS/detail queries passed. Immutable report digest `ec295c3b5b83ffa717c1db119ac1d50d62069e8fc3380e8066483ca2981b3ae3` is committed by transaction `df7d247efe1d3931036119ce03add0c04e4b6bd3c439a88bae368bd435928388`. See pressure/REPORT.md and REPORT_PRECOMMIT.json.

## Machine, versions and fixed modes

Original Vultr AMD EPYC 4345P: eight physical cores, 16 threads, advertised 64 GiB, Ubuntu 24.04.5 and matching packaged runtime. CPU16=3,200,000us/block; NET4MiB=4,194,304bytes/block; each local state capacity 16,384MiB; eight workers / batch24 / group8, six chain threads. P2P64MiB/message, per-peer100MiB in-flight guard, producer queue1GiB, transaction caps/billing, full validation/auth/conflict fallbacks, RAM64GiB plus explicit1024bytes/block, disabled deferred stages1/2 and all selected system modes remain unchanged. Loopback peer/client allowance128 supports this shared-host topology.

Node source `00443330a0d29673700a4eebee78de3b0234c281`, SHA256 `e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32`; system source `432360edd6763be000626898435bffcfe1aaf866`, WASM `6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62`. No node optimization or baseline promotion.

## Automatic joining and evidence

Each BP has independent keys, ports and mutable data. The founder is the sole ordinary seed; signed BP gossip uses registered peer keys. Static seed configuration is not presented as gossip qualification. Once registered and self-voted, candidates are selected and handed off by the on-chain contract. The controller sends no manual schedule-setting transactions. Registration/online counts remain distinct from effective native policies; their handoff and observation timing is preserved.

Persistent controllers: `pureeos-rotation-demo-join-20261031.service` and `pureeos-rotation-demo-observer-20261031.service`. The finalized SHIP journal and canonical headers remain in `/opt/pureeos-rotation-demo-20261031-r1/rotation-observer.sqlite`. All31 distinct state files/configs are prepared; a full runtime/PID/synchronization audit automatically follows the last actual join. Final24-hour qualification requires actual within-one-hour joining, runtime proof, native21-member policies, all30BPs producing irreversible blocks, online state and no permanent bans. Counts freeze at the inclusive24-hour boundary; FINAL_DEMO_REPORT.json/md and this archive update after the historical cursor catches up. Nodes/history remain running.

Frontend source `b1994590a451e0d51fa30089163fc4c5021ef704`: 140 tests and production build passed. Real headless Chrome verified both languages/themes, all30 planned identities, actual finalized handoffs, on-chain pressure summary and mobile layout; local screenshots retain evidence. The UI is read-only and pages historical events by stable sequence.

Setup attempts retained: an HTTP-only observer route was corrected for HTTPS; an isolated deadline-test namespace omitted json before a passing retry; the layout helper's conditional-expression whitespace syntax error was corrected before execution; the initial archive-generation heredoc had nested quote delimiters. These are setup/component attempts, not additional pressure windows. Private keys, wallets, full configs, raw corpora and databases remain on the server and are excluded from public artifacts.


## 2026-10-31 Real-chain explorer and server-owned lifetime peak

Current frontend 10bdf9a9e301fe52dbd4267c27db1ad9ddd0bd72, release 20261031T114838Z.149frontend tests,9server tests and real Chrome bilingual/mobile verification PASS. Native active membership is displayed as a compact wrapping roster; latest effective arrivals are green and the latest actual block producer has a prominent label. All-time canonical irreversible receipt peak is server-owned, not stored in browsers; current single-block peak 32322.0TPS at block1140. This is not fixed-window/sustained throughput or a node optimization.30BPs joined within the hour and the31-process runtime audit passed;24-hour observation remains pending.


## 2026-10-31 Version metadata correction and existing BP maintenance qualification

Metadata-only source7372b1de, node SHA2560b69754391844339416ced7626fcab18afa9ce00dc4835796dda8e888cdee155. Parent e777 reproduced byte-exact; core objects unchanged.11metadata and11live maintenance checks PASS; contract WASM unchanged. Node TPS/CPU performance unmeasured. First rollout cancellation submission timed out; finalized cleanup and targeted r2 retry are retained.31-node rollout and24-hour observation must be checked separately. See20261031-version-identity-maintenance-review/REPORT.md.


## 2026-11-01 Five planned maintainers, five real absence faults and natural20-member policy

User-requested functional campaign on retainedEPYC4345P8physical/16threads/64GiB. Node7372b1de / SHA0b697543...; unchanged systemWASM6ed4..., CPU16/NET4MiB/RAM1024 and deferred disabled. No TPS/CPU performance measurement. Expected real permanent bans and natural20-member policy are separate from maintained-node mispenalties. Original24-hour no-ban criterion is retained and annotated as intentionally affected. See20261101-five-maintenance-five-shutdown/REPORT.md for current status, original failures and recovery.


## 2026-11-01 Maintenance retention, real blacklist and AI BP replacement targeted retry

Functional only, node7372b1de/SHA0b697543..., unchanged WASM6ed4..., live CPU16/NET4MiB/RAM1024; retained EPYC4345P8physical/16threads/64GiB nominal. TPS and performance CPU utilization unmeasured. Preserve maintenanceR1 RPC500 failure and AI-R1 unconfirmed expired creation; R2 retains five maintainers, real five bans, natural20-member qualification before five new AI test identities. See20261101-five-maintenance-five-shutdown-r2/REPORT.md for current status and evidence. No optimization or baseline promotion.
