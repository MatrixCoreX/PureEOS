# 固定来源与运行包声明补验

2026-10-23 16:51 UTC / 北京时间 2026-10-24 00:51。
本项完成源码对应关系和已知声明文件的技术核对，没有改换整个项目的许可。

## 固定提交核对

通过上游提交记录确认，Spring 在
[`e6a99f68b67abc4d89fe716755b2e1394a4991f7`](https://github.com/AntelopeIO/spring/commit/e6a99f68b67abc4d89fe716755b2e1394a4991f7)
明确把根 LICENSE 改为 MIT。已把该提交及此前查询的 2.0.0-dev1.3 固定提交
`88e95780314df5d326e3c440d9b1ec239e5a6f31` 拉到独立只读审计仓库，未合并进项目。

以 PureEOS 最初提交 `aa9cf97f34f95028c41696114787574ab2a4efc6` 为比较对象，
按完整 Git 树的对象与文件模式逐项核对：

| 比较对象 | 相同普通文件对象 | 不同普通文件对象 | 仅 PureEOS 初始树有 | 子模块对象 |
|---|---:|---:|---:|---|
| Spring 2.0.0-dev1.3 | 2314 | 84 | 0 | 12 个相同 |
| Spring 明确改 MIT 的快照 | 2107 | 253 | 38 | 10 个相同、2 个不同 |

上游两个固定提交不是简单的前后包含关系。GitHub 比较返回 diverged，MIT端
154个提交、dev1.3端561个提交位于分叉两侧，公共基点为
`71f16457975a1a1a2cd60424ff3fc495f762d52f`。另外，实际查询的
[`release/2.0` 固定提交 LICENSE](https://github.com/AntelopeIO/spring/blob/8a7415a70ea8f764f240a7b192fba406007ebdff/LICENSE)
仍含 BSL。这里仅核对明确列出的分支/提交，没有宣称检查了全部上游分支。

这是内容来源证据，不能把字节相同直接等同于完整法律判断；253份不同文件、
38份初始树独有文件、各依赖及后来的项目修改仍需分别说明适用来源与授权。
其中不同文件有93个路径位于 libraries/chain、plugins 或 programs，独有文件有
10个位于这些目录，不能把差异全部视为改名或文档。当前工作树也未被此项
“初始基线比较”完整覆盖。原 LICENSE 保持原样，不用主分支 MIT 覆盖旧版本，
不在本次核对中宣称 AIC 已获得全仓重新授权。

## 实际修复的运行包漏项

旧运行候选包2只有根 LICENSE、AIC_LICENSE.md 和 LICENSING.md，没有带入构建
依赖的独立许可声明。旧包27文件的完整性结果仍按原范围保留；它不满足现在
补充的声明文件交付要求，不能拿作完整发行包。

`tests/package_v1_candidate_runtime.py` 现在强制包含14份固定声明：SoftFloat、
WAVM、secp256k1、webauthn JSON、EOS VM、Prometheus、BLS、BoringSSL、Appbase、
Boost、RapidJSON、bn256，以及继承构建清单和实际固定依赖各自的两份 CLI11 声明。
两份 CLI11 文件摘要确实不同，因此分别保留，没有用其中一份覆盖另一份。

每份文件都必须来自候选9的真实编译源，匹配75,911文件的源码身份清单；
不从当前上游临时取一份替代，不接受缺失、改动或符号链接。来源路径和摘要
写入 BUILD_PROVENANCE，文件本身进入 SHA256SUMS；归档验证也拒绝漏声明。
这里是固定声明目录的完整性要求，没有由此判定所有第三方权利问题已经解决。

新候选包3：41文件，39,135,213字节，SHA256：
`e5cc12f8c207318777298f823dd75d3b1601eb561787969e68a8d49659ff4126`。
独立第二次打包摘要相同。四个运行程序及16份系统/资源合约与包2逐SHA相同。
云端已在带空格目录实际解压：14份声明与预期清单相同，四工具的动态依赖及
正确CLI参数共8项通过，2项错误旧参数反例被拒绝；原有 nodeos PID集合未变。

本批7项打包控制通过；包含这次修复的457项工具检查固定三轮全部通过，
每轮失败0、错误0、跳过0，输入不变。没有重新计作整链、性能或正式发布通过。

另对既有 AIC CDT SDK 归档核实了 CMake 所列的6份声明：llvm、boost、meta_refl、
wabt、jsoncons、cdt，均存在且匹配 SDK 原来源清单。没有修改或重新生成这个 SDK；
该核对不等同全部 SDK 代码的授权审计。

## 实链与证据

16:50:34 UTC 原验收7/PID33508仍实际active：head21862/LIB21860，21出块成员、
21 finalizer、threshold15；满席后15次单人替换，15/30名完成满席再入席。
完整30→40及故障尚未结束。本批没有变更生产C++、WASM、共识/经济常量、
正在运行的节点或原费用截止；真实24h仍排队，VM级关停恢复尚未执行。

证据根为 `PureEOS-v1-hardening-20261022` 下的：

- `license-source-audit-attempt-1`：三棵完整Git树、逐路径差异、固定LICENSE、
  上游比较元数据和已有SDK六声明核对。
- `runtime-notice-delivery-audit-attempt-1/report.json`：新旧20份产物不变、14声明、
  确定性归档、真实云端检查及457项三轮的关联核验。
- `runtime-candidate-package-attempt-3`、`runtime-candidate-package-attempt-3-repro`：新包及原始报告。
- `candidate-controls-attempt-41/report.json`：本批三轮工具检查。

完整多节点、VM恢复、真实领奖、合适硬件上的性能、实际来源授权范围和联合
发布仍保持独立门槛，本报告没有将其中任何未通过项豁免。


18:00UTC补查同一MIT提交的主来源：提交说明为“Move to MIT License”，
[关联PR接口](https://api.github.com/repos/AntelopeIO/spring/commits/e6a99f68b67abc4d89fe716755b2e1394a4991f7/pulls)
实际返回空列表；按license搜索的PR未提供本次不同开发基线的新增授权证据。
这不证明其他地方不存在授权，只说明本次查询没有解决旧基线范围；不改变尚未核清
的结论，也不替换原LICENSE。原查询摘要保存在license-source-audit-attempt-2。

2026-10-24补充：固定VM提交16f9c4c的LICENSE实际标题为EOS VM License V1.0，
不是MIT。逐字许可随准确源码及运行包保留；单独公开固定源镜像也保留该许可。
新的公开镜像不改变主项目BSL或全仓授权范围。详见同目录
PX7_V1_PUBLIC_VM_SOURCE_DELIVERY_20261024.md。
