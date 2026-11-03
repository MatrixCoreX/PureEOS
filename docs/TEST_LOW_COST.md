# PureEOS TEST 1.0.0-test.1

## 简体中文

独立测试发行版，目标为 4 vCPU、8 GB 内存服务器。源码、构建、运行包、配置和数据都放在独立 TEST 目录；使用单独预发布标签，不改变正式版 v1.0.0。4 worker 只指并行执行池，主线程、网络和最终确认线程仍然存在。

| 参数 | 正式版选定配置 | TEST 预设 |
|---|---:|---:|
| 最低硬件 | 8 物理核 / 16 线程 / 64 GB | 4 可用 vCPU / 8 GB 档 |
| 新链 CPU / 块 | 3,200,000 μs（CPU16） | 800,000 μs（CPU4） |
| 新链 NET / 块 | 4 MiB | 1 MiB |
| 并行 worker / batch / group | 8 / 24 / 8 | 4 / 12 / 4 |
| chain / vote / net / HTTP 线程 | 6 / 4 / 4 / 2 | 2 / 2 / 2 / 1 |
| 本地状态库容量 | 16 GiB | 4 GiB，128 MiB 保护余量 |
| 生产交易队列 | 1024 MiB | 256 MiB |
| HTTP 在途请求数据 / 请求数 | 500 MiB / 默认不设数量上限 | 64 MiB / 128 |
| 入站连接 / 同 IP 上限 | 128 / 128（当前测试部署） | 64 / 64 |

状态库是文件容量上限，并非启动时预占等量物理内存；链状态增长后仍需扩容。8 GB 档启动门槛取系统和可见 cgroup 祖先额度中的较小值，要求至少 7,500,000,000 字节，预留虚拟机内核占用差异。CPU 按当前 affinity 和 quota 检查，4 vCPU 不要求 4 个独占物理核。共享 CPU 可能明显降速，功能验证不代表性能保证。

单笔 CPU150000 μs、NET512KiB、计费、权限、签名、冲突回退、P2P64MiB消息及100MiB在途保护保持。RAM 市场初始64GiB与 setramrate(1024)不变；它不是服务器内存预分配。押金10000AIC、7天退款、罚没锁池、单选投票、21人15票最终确认、轮岗、维护、黑名单、10%/90%资源、奖励减半及7天治理均沿用所部署的已验证合约。系统合约不因本测试节点发行而升级或替换；两个延迟交易禁用特性保持激活。生产节点不启用SHIP；完整历史索引另配资源，未纳入单节点8GB目标。

### 使用与编译

解压到独立空目录，例如 `~/pureeos-test-1.0.0-test.1`。检查 SHA256SUMS 后启动包内工具：

```sh
mkdir -p "$HOME/pureeos-test-data"
./bin/nodeos --config-dir "$PWD/config/test-low-cost" --data-dir "$HOME/pureeos-test-data" --genesis-json /path/to/your-test-genesis.json
```

模板没有密钥、签名者、生产者名称、固定peer或公共测试网连接，需按自己的测试链配置。使用钱包或私有签名服务配置自己的测试身份。现有链使用其原始 genesis/chain ID；不要将正式版的数据目录交给测试版，也不要在已有链本地强改CPU/NET。`chain-parameters.json` 是新测试链初始化/授权 setparams 的完整预设；写进 config.ini 不会生效。系统合约 RAM、押金与其他初始化依照现有启动手册，不放宽治理。

完整源码构建时使用独立构建目录：

```sh
cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DPUREEOS_TEST_BUILD=ON
cmake --build build-test --target nodeos --parallel 2
```

依赖和工具链见正式 README。开关默认 OFF；不带开关构建仍使用正式8核/16线程/64GB门槛。nodeos 版本及启动日志明确显示 TEST。配套 cleos/keosd/helios-util 沿用正式1.0.0工具字节。不要把本版的测试结果当作正式网容量或TPS数据。

## English

This independent TEST prerelease targets 4 available vCPUs and an 8 GB-class server. Keep source, build, installation, configuration and chain data in separate TEST directories. Stable v1.0.0 and its defaults remain unchanged. Four workers describe the execution pool, not the total process thread count.

Fresh-test-chain caps are CPU4 (800,000 μs/block) and NET1MiB/block, compared with stable CPU16/NET4MiB. Local settings are 4 workers, batch12, group4, chain/vote/net/HTTP threads2/2/2/1, a4GiB state capacity with128MiB guard,256MiB transaction queue,64MiB/128 HTTP inflight limits and64 inbound clients/per-host clients. The state file capacity is not preallocated physical RAM; expand it before live state outgrows it. The startup floor is4 affinity/quota-limited vCPUs and7,500,000,000 effective bytes after visible physical/cgroup ancestor limits, allowing kernel reservation on8GB-class VMs. Shared vCPU performance varies.

Per-transaction caps, billing, signatures, authorization, conflict fallbacks, P2P64MiB messages and100MiB inflight protection remain. The64GiB on-chain RAM market and setramrate(1024) do not allocate64GiB of host memory. Existing qualified contract bytes,10000AIC bonds, seven-day refunds, locked forfeitures, single-choice votes, native finality/rotation, maintenance, permanent bans,10%/90% resources, rewards/halving and seven-day governance remain independent of this node release. Both deferred-disable stages stay active. Producer SHIP and full history/indexing services are outside this single-node8GB target.

Use the command above with your own test genesis, private identity/signing service and peers. No private keys or public-chain peers are shipped. Existing chains retain their actual genesis/chain ID and consensus CPU/NET caps; an INI file cannot change them. The full chain-parameters.json is for fresh initialization or normally authorized setparams. Never reuse a production data directory. Build with PUREEOS_TEST_BUILD=ON in build-test; its default OFF preserves the stable hardware gate. nodeos identifies itself as1.0.0-test.1/TEST; companion1.0.0 CLI bytes are unchanged. This is functional qualification, not cheap-VPS performance or a TPS claim.

## Optional30-minute TEST governance development profile (2026-11-03)

The newly selected fresh testnet additionally opts into `-DPUREEOS_TEST_GOVERNANCE_30MIN=ON`
together with `-DPUREEOS_TEST_BUILD=ON`. This native consensus variant uses1800seconds
for governance-v2 review and labels itself `test.2-dev`. Production and the published
v1.0.0 / v1.0.0-test.1 assets retain their original bytes and seven-day review; the
legacy14-day constant is unchanged. The build rejects use without TEST_BUILD.
All nodes on this fresh Chain ID must use the same TEST30-minute binary. Existing
review/veto/hash-bound execution, native authorization, resource limits, mandatory
parallel processing and disabled deferred stages remain. Never swap this binary into
an existing seven-day chain. The current campaign is functional only, not TPS.
