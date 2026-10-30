# Ubuntu 24.04 / 26.04 LTS / 26.10 编译与运行验证

状态：PASS。验证日期：2026-10-30 至 31。本轮为兼容性检查，没有 TPS 测量。

| 项目 | Ubuntu 24.04.5 LTS | Ubuntu 26.04.1 LTS | Ubuntu 26.10 |
| --- | --- | --- | --- |
| 环境 | 保留的 EPYC 裸金属宿主 | 独立启动的临时虚拟机 | 24.04 宿主上的真实 26.10 根文件系统 |
| 内核 | 6.8.0-146-generic | 7.0.0-38-generic | 共享 6.8.0-146-generic |
| GCC / CMake / glibc | 13.3 / 3.28.3 / 2.39 | 15.2 / 4.2.3 / 2.43 | 15.3 / 4.3.4 / 2.44 |
| nodeos / cleos / keosd / helios-util 源码构建 | PASS | PASS | PASS |
| 构建入口检查 / 原生组件程序 | 14 / 4 通过 | 14 / 4 通过 | 14 / 4 通过 |
| CDT 编译、链接、ABI 语料 | 32 项 × 3 轮通过 | 32 项 × 3 轮通过 | 32 项 × 3 轮通过 |
| 原生 SDK 编译与运行 | 76 作业通过 | 76 作业通过 | 76 作业通过 |
| 系统合约重编译 WASM / ABI 一致 | PASS | PASS | PASS |
| 完整验证、并行节点实际运行及 P2P 同步 | PASS | PASS | PASS |

24.04/26.10 使用 AMD EPYC 4345P 裸金属，8 物理核、16 线程、64 GiB。26.04 使用 Intel Xeon Processor (Cascadelake) 虚拟机，8 个来宾核心、16 vCPU、64 GiB；这是功能兼容性证据，不是物理独占或性能资格。26.04 使用自身内核和 glibc，不以 26.10 用户空间代替 LTS 验证。

当前 SDK 已验证实际合约编译、ABI 及原生运行；完整 LLVM 后端源码重建未包含在本轮资格中。精确源码、ELF、系统合约摘要及范围见 [JSON 清单](COMPATIBILITY_20261030.json)。

## 节点和 P2P

每次采用两个完整验证、并行 8 worker / batch 24 / group 8 的观察节点。规范重放与实际 P2P 接收均从快照 40264 开始，接收后续 1714 个区块至 41978；共同最终确认高度 41976 的 ID 为 `0000a3f82996b9ad679f606198b9ca4ac0d16a44546a405f209cf7ea333fd993`。两个进程的链头、共同已确认区块、21 人 / 15 票政策及系统状态一致；未配置生产或确认私钥，结束后正常退出。重放端 LIB 41978 与 P2P 端 41976 的差别按共同高度比较。

保持 CPU 3,200,000 μs/块、NET 16 MiB/块、P2P 64 MiB/消息、RAM1024、禁用 deferred 和全部硬件/并行检查。没有重复不足 21 人故障场景或运行压力负载。

## 构建入口及可复现命令

节点安装、编译和运行命令见[项目 README](../../README.md)。构建入口与 CI 矩阵支持 24.04、26.04、26.10；配置示例已移除只接受命令行的两个 replay 选项。CI 配置更新不代表 GitHub 远程 CI 作业已经通过。

系统合约顶层 CMake 默认开启并向子项目传递维护开关；CDT helper 显式开启 peering/maintenance，实际生成 23 份合约产物。当前系统源码/WASM 包见[系统合约目录](../../system-contracts/README.md)，全部选定模式必须保留。直接构建 contracts 子目录时必须指定 WASM toolchain：

```bash
export PUREEOS_CDT_ROOT=/absolute/path/to/pureeos-cdt-1.0.0
export PATH="$PUREEOS_CDT_ROOT/bin:$PATH"
cmake -S /absolute/path/to/system-source/contracts -B /absolute/path/to/contracts-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PUREEOS_CDT_ROOT" \
  -DCMAKE_TOOLCHAIN_FILE="$PUREEOS_CDT_ROOT/lib/cmake/cdt/CDTWasmToolchain.cmake" \
  -DPUREEOS_BP_PEERING=ON -DPUREEOS_BP_MAINTENANCE=ON \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON -DPUREEOS_SINGLE_CHOICE_VOTING=ON \
  -DPUREEOS_RESOURCE_SPLIT=ON -DPUREEOS_SEQUENTIAL_ROTATION=ON
cmake --build /absolute/path/to/contracts-build --target eosio.system --parallel 2
sha256sum /absolute/path/to/contracts-build/eosio.system/eosio.system.wasm
```

OS campaign 在奖励修正前重编译的 WASM 为 `5f25a25816fc6db145dbd811d9c24dd99c9b9b1fd260b8e16a045476d0c3c11a`，ABI 为 `6809814cb91cdfb55e9cf9bfb926343b9a8be02f82b8996796e11173eec40a9c`。后续精确 1% 合约及真实动作测试单独记录在[奖励复核报告](../rewards/EXACT_1PCT_20261031.md)，不声称已删除的 VM 再次编译过新合约。节点业务/共识 C++ 无改动。

## 原始尝试和临时机清理

保留 /proc 缺失、测试发现路径、CLI-only 配置、不同 LIB 高度比较、维护开关遗漏、直接合约构建遗漏 toolchain 等原始失败和定向重试；错误产物未部署。24/26.10 原始项目保留在 `/opt/pureeos-ubuntu24-26-compat-20261030`。

26.04 临时机 UUID `ee5f39eb-6b89-4ceb-8736-7454f66e2d5b` 的 86,715 个文件逐一校验后归档至保留的主机；随后删除临时机和临时云 SSH key，连续两次确认列表不存在且直接查询 404。原始主机保留。归档位于 `/opt/pureeos-private-evidence-20261030/ubuntu2604-vm-ee5f39eb-6b89-4ceb-8736-7454f66e2d5b/pureeos-ubuntu2604-compat`。

## English

**PASS, 2026-10-30/31.** Native Ubuntu 24.04.5 LTS, a separately booted Ubuntu 26.04.1 LTS VM, and a real Ubuntu 26.10 root filesystem all built the four node tools, passed 14 build-profile checks and four component programs, completed 32 SDK compilation/link/ABI cases for three rounds and 76 native SDK jobs, and rebuilt matching system WASM/ABI. The table records exact compilers, CMake, glibc and kernels. The 26.10 rootfs shares the 24.04 host kernel; the 26.04 VM uses its own kernel. VM evidence does not qualify dedicated hardware throughput. No complete LLVM backend rebuild or TPS measurement is claimed.

Parallel full-validation observer nodes replayed or received 1714 actual P2P blocks from snapshot 40264 to head 41978. Both match at finalized height 41976 under the existing 21-member/15-vote policy and shut down normally. Chain caps, hardware minimums, disabled deferred transactions and RAM1024 remain intact. Node runtime/consensus C++ did not change.

Use the project README for node build/run instructions. The direct contract-build commands above explicitly select the WASM toolchain and every required mode. CI configuration is not remote CI execution evidence. The OS campaign predates the separately qualified exact1pct contract; see its own reward report rather than attributing a new build to the deleted VM.

Original failures and targeted retries remain recorded. After all 86,715 VM files were archived and hash-verified on the retained primary, the temporary VM and cloud SSH key were deleted. Two subsequent checks confirmed absence and HTTP 404. The original primary remains retained.
