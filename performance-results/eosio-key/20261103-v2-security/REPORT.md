# eosio.key 协议 2：远端验证与发布阻断

当前结论：资金和并发回归通过，但**资源隔离未通过，禁止据此推广到公开链或稳定版**。任意自定义代币合约可借提币原生授权消耗共享托管账户的 RAM，必须先确定代码准入或独立托管方案。

源码代码提交：`622eb43c8eb214bd430e9377e37a0a1654d8897a`；开发分支 `wip/eosio-key-v2-security-20261103`。后续报告提交只更新文档/证据，不代表稳定版发布。原生 nodeos、eosio.system 与公开演示链未修改。

## 实际验证

| 项目 | 结果及范围 |
|---|---|
| SDK / 参考中继 | 13 项测试通过：完整交易签名、精确整数、并发、同包重试、落盘失败、重试上限、队列及 HTTP 边界 |
| 正常系统构建 | 58 项观察符合测试预期，其中一项明确是**已知 RAM 漏洞的成功复现**；不能把全部 PASS 理解为安全发布 |
| 资金、签名和并发 | 两个独立请求同代次均不可逆到账；原交易重放、过期清理后重放、压缩变更、篡改、重复证明、inline 调用均未造成重复扣款 |
| 客户端 | C++ cleos 和 SDK 签名一致；公钥到文本账户全额到账；RPC 凭据未发给中继；`-d` / `-d -s` 仅导出不广播 |
| 费用与储备 | AIC 费用额外扣除、上限生效；真实 PowerUp 租用提高费率；四种资产负债与实际储备一致 |
| 权限 | 原生 BP 热密钥撤销/轮换、真实维护、自助退出、代次撤销与超过 2^53 的代次均验证 |
| 替身补充 | 56 项观察通过；黑名单、异常押金、有效状态、极端市场及系统哈希变化为钱包逻辑证据，**不代表原生多节点黑名单共识验证** |
| RAM 探针 | bios 私有链复现 4,330 字节额外计费；收款通知计费被原生规则拒绝 |
| 实际钱包 RAM | 正常系统私有链的 eosio.key 同样被恶意代币额外计费 **4,330 字节**，内部行数计数不变，余额检查未发现该资源消耗 |

## 保留的限制

需要用户确定：[第一版已审核代码准入，或任意自定义代币使用独立托管](../../../system-contracts/eosio.key/docs/TOKEN_CUSTODY_REVIEW.md)。在此之前保留 `release_qualified=false`。没有公开链部署、重置、主分支合并、稳定标签、TPS 测试或第三方独立审计。

本轮在独立 Vultr AMD 测试机运行，实际识别为 AMD EPYC-Rome，4 个允许逻辑 CPU、2 个虚拟机报告核心、4 线程，8 GB 级内存。宿主机真实物理核心数量未知，不是 EPYC 4345P 性能测试。使用独立 TEST CPU4/NET1MiB 预设；正式 CPU16/NET16MiB 默认未变。所有临时测试节点已退出；公开演示链另行核验仍为 26 个进程。

C++ 客户端在 Vultr 上以 GCC 13.3 编译修改的 main/http 源码，复用已留存且逐项哈希记录的原生库；不是完整干净 CMake 的官方客户端发布。CDT 1.0.0 构建合约，Node 22.23.4 运行 SDK；依赖、命令、ABI/WASM/客户端哈希和失败尝试分别保留。用户提出远端专用规则之后，本机仅编辑源码、编排、接收和校验报告。

## 证据

- [资金/并发与实际钱包 RAM 结果](FUNCTIONAL_R4.json)
- [替身结果](FIXTURE_R3F.json)
- [原生 RAM 探针](RAM_AUTHORITY_PROBE.json)
- [机器信息](HOST_FACTS.json)、[构建哈希](BUILD_RESULT.json)、[源码匹配](SOURCE_MATCH.json)
- [原生命令](NATIVE_COMPILE_COMMANDS.json)、[测试参数](CHAIN_PARAMETERS.json)
- [失败与淘汰尝试](EXPERIMENTS.md)、[历史索引](PREVIOUS_ATTEMPTS.json)
- [进程清理和防火墙](HOST_CLEANUP.json)、[原演示链核验](OLD_DEMO_UNCHANGED.json)

No TPS was measured. Functional regression passed, but shared-custody RAM isolation failed and release remains blocked. The independent native probe and actual wallet both reproduced the extra RAM charge. Choose reviewed-code admission or separate custody before implementing the fix and requalifying. No private keys, wallets, node configurations, state databases or raw blocks are included.
