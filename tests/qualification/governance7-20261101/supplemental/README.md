# 新ABI及系统合约补充验证

运行节点源代码固定为`5d95aa18fea665f4ee60eda13b01dec82ff43535`。此提交只保存后续测试、契约源码差异和证据，没有修改运行实现。源码编译、原生测试和合约构建在保留的EPYC4345P服务器完成。

参见[完整报告](REPORT.zh.md)、[资格结果](QUALIFIED.json)及[现场快照](STATUS.json)。92个原有不同用例加17次新ABI复跑，其中16个名字重复、增加1个新用例：共93个不同用例、111次通过，不包含失败/中断尝试。新ABI仅替换四段说明，CDT源码生成文件、原生序列化及完整升级交易签名/解码分别核对。原始失败留在硬件归档。

合约源代码bundle基于已发布`d126cae28057edfcd0e2eb84654afd8efd6da257`，最终为`15dea1aa8a4b158eb771c5b13947ddeaafd28910`；patch可对该已发布源树应用。WASM373b66、ABI JSONdd02bd及序列化ABI038ffb完整哈希在CANDIDATE.json。源码重建证明WASM字节等于已资格验证的C2逻辑，不是引入上游EOS整份合约。

原生项目使用完整节点构建和既有发布合约包；驱动保留实际路径与命令。重现须将当前、旧6ed4合约包及ABI校验期望字节置于驱动中的对应路径，expected ABI字节可由原生cleos convert pack_hex --type abi_def生成并校验038ffb完整哈希。不得在现网改时钟或数据库来模拟七天。种子状态故障注入单独标记。

快照中节点滚动更新尚未完成，新协议和新WASM不能宣称已全部线上生效。全节点就绪后才可激活协议；正常提案仍须等待实际七天。该归档没有新增TPS、31台物理机性能或新版Ubuntu26.04重测结论。

# Supplemental ABI and system-contract qualification

The running node source is pinned to`5d95aa18fea665f4ee60eda13b01dec82ff43535`. This follow-up adds evidence/tests and the system-contract source delta only; runtime implementation is unchanged. Builds/native tests ran on the retained EPYC4345P host.

There are93unique native cases and111successful executions: the17new-ABI runs repeat16of the original92names and add one. See QUALIFIED.json and REPORT.zh.md. Exact source-generated JSON, native packed ABI, unchanged WASM and a complete signed/decoded but unbroadcast upgrade transaction are verified independently. Failures/interrupted attempts remain in the hardware archive.

The incremental contract bundle requires published based126cae28057edfcd0e2eb84654afd8efd6da257; the plain patch applies to that source tree. Final contract source15dea1aa8a4b158eb771c5b13947ddeaafd28910 retains all selected modes. CANDIDATE.json pins full WASM/ABI hashes. The driver requires a complete node build and the pinned released/current contract packages at its recorded paths. Generate expected packed ABI bytes with native cleos and verify the038ffbfull digest. Never manipulate a live clock/DB to bypass review.

Live rollout, feature activation and the real seven-day review are distinct pending stages in this snapshot. No new TPS,30physical-machine performance or new Ubuntu26.04 qualification is claimed.
