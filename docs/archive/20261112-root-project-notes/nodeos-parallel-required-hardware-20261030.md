# nodeos 并行强制启动与最低硬件检查（2026-10-30）

已完成服务器编译、普通/ASan/UBSan 组件检查和 9 项真实 nodeos 进程测试。本报告只证明启动策略；不代表 P2P 自发现或三台全并行吞吐已经验证。

- 源码提交：44ab5dcc64bed7d649c6c5f2e96f659bfa1cdebd。
- Node SHA256：1de5adfa0018e44ec3e19e3bf4e9f631eae59a439f556fbe734e6864264b4bf2。
- 基于已选 group8 源码 3a3f9361；未包含已失败的 bounded-send P2P 候选。
- 运行硬件：AMD EPYC 4345P，8 物理核 /16 线程，Vultr 64 GiB 裸机。实际 MemTotal 63,360,244 KiB（约 64.88 GB /60.43 GiB）。
- 最低配置：8 物理核、16 逻辑线程、64 GB 系统可见物理总内存。此处 GB 为十进制，最低值 64,000,000,000 字节；不以当前空闲内存、MemAvailable 或链 RAM 市场为准。
- CPU 物理核按 Linux socket/die/core 拓扑计数，SMT 不重复计为物理核；必要时读取 /proc/cpuinfo，无法验证拓扑时拒绝启动。
- worker 默认 auto，按既有亲和性/配额规则计算；batch 默认 auto；生产并行默认 true；引擎默认匹配主 VM。
- 显式关闭生产并行、worker 为 0 或 1、batch 为 1，均拒绝启动。仍要求现有已审核 WASM 哈希、全量验证和引擎一致性；未放宽授权、冲突、回滚或计费规则。
- 硬件不合格在链数据库打开前退出，日志包含检测值和最低配置要求。无发行版命令行/环境变量绕过参数。
- 真实进程拒绝测试：物理核不足、线程不足、内存低于字节边界、拓扑无法识别、关闭生产并行、0 worker、1 worker、单笔 batch，全部通过。
- 合格服务器的正常启动：默认 8 worker、batch24；本地 HTTP 接口正常，优雅关闭成功。
- 所有伪硬件输入仅通过 root 管理的隔离测试挂载提供，不添加生产运行绕过入口。
- CPU16/NET16MiB 开发链资源上限、P2P64MiB 消息上限、1GiB 生产队列、RAM1024、系统合约模式和停用 deferred 的要求继续遵循原配置。
- 后续联网测试只使用全部开启并行的节点。历史串行证据和失败 P2P 候选记录保留；自发现仍独立待测。

证据目录：/opt/aic-node-parallel-required-20261030/
