# PX7：准备阶段截止与诊断修复

日期：2026-10-15。证据目录 `build/local-tests/parallel-preparation-budget-20261015-9bwh42/`。这是 G7 的一个子项，不是完整性能或稳定性验收；G6b 的状态层结果保持独立记录。

## 代码复查证据与修改

原 `controller::prepare_isolated_attempts` 有两个缺口：

1. checkpoint 先以 `cancel.stop_requested()` 抛 `interrupt_exception`。生产取消对象同时携带本地 50ms 截止，因此到时可能先走“中断”分支，没有增加 preparation deadline / budget 计数。`997122aa…` 的原 ASan 失败样本中，一项多根同步出现 attempts=4、success=0、所有截止计数=0；此样本和源码共同说明诊断存在盲区，但不证明所有旧采用失败都是同一个原因。
2. 验证端 500ms 截止在完成最初的 invocation / code / CF 数据准备之后才初始化，该段循环也没有 checkpoint。生产端虽更早设定 50ms 截止，但同样要到初次准备之后才检查。

本批将私有截止和 checkpoint 建立移到准备入口，新增入口、每笔输入、每个根及每个新接收者的协作检查。原 helper 中单次有界操作/内存分配依旧不能被强行抢占，这不是硬实时保证。仍采用同一个绝对截止，不给重试或不同根重新领预算。

checkpoint 调用原 `isolated_cancel::check()`，以观察时的原因区分外部 generation 取消与本地截止：外部取消继续抛原 interrupt；本地截止增加 preparation deadline，走现有 budget fallback 计数和原序串行路径。原取消优先级测试已在 worker 套件中，当前须重建后复测，不能引用旧 ELF 充数。

**数值不变**：生产最多 50ms、仍受真实出块窗口四分之一及原 CPU 保留时间约束；验证准备最多 500ms。原授权、RAM、交易 CPU 超限、undo、队列处理和默认关闭开关不改。验证计时起点提前属于范围修正，不是把本地预算放大。

## 测试与当前进度

在原 `slow_private_queue_head_falls_back_and_does_not_starve_following_transaction` 增加 39 条断言，保留原慢队首 CPU 拒绝、后续交易成功、去重状态与余额检查。1/2/4 workers 分别覆盖 100ms 短窗口不进入试算、500ms 窗口进入并触及原 50ms 私有截止；先核验实际 CPU 保留预算确实位于两个窗口之间，再检查截止/预算准确记一次、不误报其他准备失败、无并行采用。没有降低原断言或改变忙循环。

本批核心已以单个 `-j1` 构建成功，日志 `build-core.log`。程序 SHA-256 为 `1c8397a832b948a7b2ef79f4bfae2db8b5e3de0d5e8c615b10340deab6d88123`，编译参数及链接命令分别保存为 `core-flags.make`、`core-link.txt`。编译日志中 `parallel_validation_tests.cpp:1475` 的有符号比较警告也存在于旧版构建日志，不是这 39 条断言新增的警告。

超时边界用例固定三轮全部通过，每轮 **1 个用例 / 134 条断言**（原 95 条加本批 39 条），见 `boundary-{1,2,3}.log` 和对应 `.time`。准确入口为 `parallel_production_tests/slow_private_queue_head_falls_back_and_does_not_starve_following_transaction`，不是 validation suite。36 份源码及程序在测试后哈希复核一致。100ms 窗口不试算、500ms 窗口本地截止准确记一次、慢交易原 CPU 拒绝、后续交易及余额/去重均通过；这仍不证明旧混合树的所有采用率失败已修复。

完整核心固定三轮全部通过：每轮 **63 个用例 / 12,417 条断言**，耗时 **93.29 / 90.70 / 90.09 秒**，日志 `core-{1,2,3}.log`。排除的一个 benchmark 导出入口不属于本次正确性验证，不以这些耗时估算 TPS。外部原 token 产物 `ea1bd149e28f21420450b6c7b2c5be600ef9922e44c383fa4489b025aac0eb46` 的三个原场景也固定三轮通过，每轮 **3 / 1,471**，耗时 **7.41 / 7.71 / 7.51 秒**，见 `token-{1,2,3}.log`。未与编译、节点或压缩并发，原超时与断言不变；结束后 36 份源码、核心程序及 token 产物哈希一致。

本版兼容程序 `7a94096ffe4fc55a13fbc064e9711d2721fcc4603076336e6280d8967670e3ed` 已重新链接，**21 组原行为检查全部通过**，78.36 秒，日志 `build-compat.log`、`compat.log`；覆盖只读交易、C++/WASM 同步调用、资源、数据库、延迟、RAM、checktime、主观计费、队列、原数据库/multi-index/权限/action ordinal/结果 API。源码及程序在结束后复核一致。可恢复归档 `compat-initial.gz` 的 SHA-256 为 `41180400439d3f116bcc2c59e22f0349f659f0e9c92171fe6838da9e739f4dec`，完整性及恢复程序哈希已核验。

独立 worker、插桩及实验节点回归尚未完成，不能沿用旧程序的通过结果。这些正确性通过不等于 G7 全部稳定性/性能门槛验收。

以上测试全部结束、源码/程序身份核验后，才开始 G6c 类型化写日志及二级覆盖层改造。该后续源码在另一个证据目录 `parallel-secondary-overlay-20261015-1ek2VQ/` 构建中，尚未验证；本记录的通过不能当作它的测试证据。

`current-sources.sha256` 记录 36 份源码；旧核心 `d2091b91…` 与 `parallel-mixed-adoption-20261014-ECzQQD/validation-formal-initial` 的身份在重建前一致。当前核心完整归档为 `core-initial.gz`，归档 SHA-256 `a71578c42cdd8304e3c829c3d2004cab24ae44b85757e1a96932a3f2d00f7540`；gzip 完整性及解压流的程序哈希均已核验。

访问清单仅 controller 哈希和行位置变化，没有新的数据库访问/host 注册，`inventory-after.log` 精确匹配。两份改造前文件保留于本证据目录的原相对路径。

## 可恢复空间整理

为本次链接释放 tmpfs UID 配额，仅删除已核验的旧未压缩 `B/unittests/parallel_wasm_sanitizer_test`（B=`/dev/shm/pureeos-release-ESOoZv`，416,536,872 bytes，程序 SHA-256 `997122aa34a16766c50ddc7ffbea5f32531207d456a21d8743cf81447709422c`）。完整归档仍在 `parallel-mixed-lru-20261015-TlEO9d/wasm-asan-lru-initial.gz`，归档 SHA-256 `815e34c1cf34b26e42988155a69a65b03e66c9015dd6d42eeba6ec646abdfbe3`；删除前通过 gzip 完整性与解压流哈希检查，确认原文件为普通文件而非符号链接。操作后 UID 配额余量 540,818,432 bytes。源码、失败日志、原节点及链数据未删除。

G6b 的新 secondary / native 插桩程序也已分别归档为其证据目录中的 `secondary-asan-initial.gz` / `native-asan-initial.gz`，归档 SHA-256 分别 `7a0225c209357955ef0d710c65ef18c0cf25dfb0558d2396bedfba6f812ede9e` / `312e14bc66256bfd585cbf0b43e23e349f4be8fd0ec35c99e5567e632c1b8fbb`，完整性及恢复程序哈希均已复核。它们不是完整 WASM/节点测试证据。
