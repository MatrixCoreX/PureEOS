# 治理7天与黑名单排名验收

本目录冻结在保留的EPYC4345P(8物理核/16线程/64GiB)服务器实际执行的测试源码与结果，不替换常规单元测试。逻辑源码cbe8646585783e0a145ffe4d40e9771454dadac3；系统合约源码5aaec95f76fdf8a07f329d5b66e0a60de9f3c8b7，WASM373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb。

16项定向、56项资源/投票/奖励/治理/委员会回归及22项激活前原生兼容用例通过，扣除重复的2项奖励测试，共92个不同用例。两项fault_injection用例单独标识，只检验异常状态保护和快照中断排队；不能把注入状态当作真实处罚证据。另有真实三窗口处罚、正常审查/devexec、独立重放及快照恢复验证。

候选1的6项定向测试虽通过，额外快照中断测试发现FIFO回退，故未部署。最终版本保留中断快照已分配票据，防止候补插队。7天审查跨越容量自然增长210到211，测试按真实容量验收，不修改增长规则。全部原始失败及重试保存在服务器同名campaign目录和performance-results证据包。

重现使用现有远端构建树`/opt/aic-owner-preparation-combinations-20261029/build`及其compile_commands.json、已固定依赖和CDT合约产物；脚本在Ubuntu26工具链rootfs中把campaign挂到`/campaign`，只读依赖挂到`/opt`。`native_guest.py`保留实际编译/链接参数适配；`supported_regression.py`运行互补套件。需要相同目录映射和原始6ed4合约(正常升级输入)，勿直接套用到未准备环境。后续移植测试入口时应保持案例及输入SHA不变。

这不是节点TPS或30台独立机器的证据，不改变在线串并行模式；在线nodeos仍必须全部并行，达到8物理核/16线程/64GB硬件下限。deferred两阶段禁用，没有send_deferred/cancel_deferred测试。在线部署、真实7天公示和24小时观察另行记录。

# Seven-day governance and blacklist rank qualification

This directory preserves the exact campaign harness and results from the retained EPYC4345P8core/16thread/64GiB server. It complements ordinary unit tests. The logic/contract revisions and hashes above pin the inputs.16directed,56supported-function and22preactivation native cases passed:92unique case names after two overlapping reward cases. Fault-injection cases are explicitly separate from genuine finalized duty evidence. Real WASM handoffs, normal review/devexec, snapshots and independent replay are covered.

Candidate1 was rejected for FIFO changes after a ranking interruption and was never deployed. The final candidate preserves existing tickets. A7day review naturally opens capacity211; tests follow the unchanged growth rule. Original failures remain in campaign evidence. Reproduction requires the recorded build tree, toolchain/rootfs mounts, fixed dependencies and original6ed4WASM; the scripts document those actual inputs rather than installing them. This is functional evidence, not nodeTPS or30machine qualification. Live rollout and real review/observation periods are separate.
