# 当前候选的程序与系统合约交付验证

2026-10-23。这是可复核的开发候选包，不是正式发布或 TPS 验收。

`runtime-candidate-package-attempt-1/pureeos-runtime-candidate.tar.gz`：

- 39,113,056 字节；SHA256 `f1343ea4bd9fa9c82ae052f028eb17db0adade2907d751fa0277832ab2f5c584`。
- 四个程序：`bin/nodeos`、`bin/cleos`、`bin/keosd`、`bin/helios-util`，逐项匹配候选9已通过完整59步的 ELF 摘要。
- 16 个明确选定的 bootstrap/system/token/资源辅助 WASM/ABI；不打包旧 msig/wrap、测试合约或 CMake 探测程序作为部署目标。
- 两份当时的候选说明/验收清单、项目 LICENSE、来源清单与 SHA256SUMS，共25个文件。
- 来源 `beb91fb95dfc0217491c5016edc5f3da49fde4f0001407354bbee87ea2e0aa55`；核心归档 `b2ae5e3de8832cd68c11d56c879c30ad3fea516b4eaa1108d09df0b3a850b10d`。

入口 `tests/package_v1_candidate_runtime.py` 只读核验核心完整成功、关闭归档、真实版本来源、归档字节/摘要及合约工件摘要；输出必须是新的同级目录，现有输出拒绝覆盖。它只收集明确允许的文件，不采集钱包、私钥、节点配置、状态库或测试链创世配置，也不安装系统服务、升级合约或创建云服务器。

实际命令：

```bash
python3 -I -B tests/package_v1_candidate_runtime.py \
  --candidate /你的证据目录/local-native-profile-retest-attempt-9 \
  --contracts /你的证据目录/contracts-safe-eviction-attempt-2/contracts \
  --output /你的证据目录/一个全新交付目录
```

首次隔离 CLI 调用在创建输出前因 `-I` 不包含脚本所在目录而拒绝本地支持模块导入，尚未打包或修改输入。已明确装载脚本自己的支持目录，并新增独立工作目录下 `-I --help` 无副作用检查；当前5项打包控制均通过，含原文件篡改、归档字节篡改、重复条目、额外路径、链接/路径逃逸、输出覆盖和伪造发布标志拒绝。控制用的是合成小文件，实际程序验证另记如下。

`runtime-candidate-package-smoke-attempt-1` 已在同一不可变 Ubuntu26.10 用户态镜像中实际解压到 `install with spaces`，9项检查全部通过：解压后 SHA256SUMS、三个实际完整版本、helios-util BLS 命令帮助及四个程序动态依赖。解压后的相同内容重新生成归档，SHA256 与上面完全一致。脚本/输入/依赖输出和原日志保存在独立 smoke 目录，容器已退出；没有自动启动链或钱包。

本包没有捆绑主机动态库，不声明通用 Linux 或 Ubuntu26.04 整链可移植性。CDT 的独立可迁移开发 SDK、精确编译源码和较新的测试/操作脚本分别交付，不能将后加脚本冒充生成本 ELF 的旧编译源快照。最终稳定版仍须完整节点/恢复/30→40进程、真实一天实际到账、主机故障、有效性能对照及 node/CDT 联合验收；本包各清单保留 `release_ready=false`。
