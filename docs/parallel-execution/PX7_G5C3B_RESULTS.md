# PX7 G5c3b：context-free 内联执行（开发与验证中）

从 `11d05e7` 继续，证据目录 `build/local-tests/parallel-context-free-20261012-wbEaQl/`。本阶段仍在实验分支，默认开关关闭，不宣称所有合约已全面并行。

## b0 私有执行与结构预检

正在实现/验证：

- 新 `isolated_context_free_data` 在分配前检查最多 1,024 条、总计 256 KiB 的本地尝试预算，然后独立拥有数据并一次计算摘要；各个步骤共享 const 所有权，只绑定摘要。不持有 packed_transaction 或 controller 指针。超限只是本地回退，不是共识新限制。
- 调用树发送事件、子组明确区分 context-free；根与步骤的 v2 本地指纹绑定数据摘要及调用类型。发送顺序分配 ordinal，执行先通知，再 context-free 子队列，再普通子队列。
- context-free 中允许固定输入、内存、断言和 `get_context_free_data`；已有所有 context-aware 接口增加入口检查，包括授权、账户/主索引读取、写入、通知、两类发送、get_sender 和 set_action_return_value。并非只禁止写入。其他未支持接口仍整笔回退。
- 数据读取保留原来的缺项 -1、零缓冲区查询长度、截断拷贝和内存范围检查。子动作必须无授权。原始发送字节和严格长度检查沿用先前实现。
- 整树重建检查类型、两个子队列和禁止效果；默认 context 不提供新能力，正式 owner/adopter 暂未开放，原三类私有入口仍不能发送 context-free inline。

新增测试包括不可变输入/边界、交错队列、19 类禁止接口、篡改预检、空代码子动作、1/2/4 线程混合成功失败，以及原串行 trace/receipt 和接口失败码对照。构建/执行结果待补充，不能用用例已写代替通过。

初次构建通过；`private-first.log` 的 7 项 / 397 条通过（6 项私有 / 297 条、1 项原链队列与 receipt 对照 / 100 条）。随后补充固定输入读取、模块 start 的上下文限制、同交易 ID 不同附属数据拒绝复用，以及一页内存尾部/零长度读取。尚待扩展后的完整验证。

审计清单本身匹配；第一次审计单测 24 项中有 1 项失败：新增两项树接口未同步旧测试的精确名单。已补 2 个正确签名的真实调用夹具、树接口集合及仍禁止链接的 225 项计数；保留 `audit-tests.log`，复测另存 `audit-tests-final.log`。不是删除检查或把禁止调用改成成功。进程工具 16 项通过。还在运行原链接口对照前修正测试部署 helper：原 setcode 拒绝同代码重复部署，helper 应跳过已安装的相同父代码，不修改原 setcode 规则。

扩展后首次全工作池 116 项中 115 项通过，8,387 条中 8,385 条通过；两条错误码断言失败。`oracle-detailed.log` 确认 db_next_i64 / db_previous_i64 的 output=0 会先在原 `webassembly/common.hpp::from_wasm` 被拒绝（3070002，不能为空指针建立引用），未到 CF 权限检查（3050007）。CF 禁止接口测试改用合法非零输出指针来单测上下文限制，另留独立空指针/失败撤销对照，不把两种失败宽泛地当一种。

此对照同时发现旧私有 `adjacent()` 缺少原 legacy_ptr 的非零检查：普通调用也可能计算成功，并在正式直接重放 cursor 时绕过原参数转换失败。这是实际并行语义风险，不仅是测试预期问题。新增先写行再调用 next/previous 的四种私有入口与原链对照，先验证未修复版的差异，再修复空指针回退并复测；该修复尚在进行中。原 VM 的错误类型和检查先后顺序不改动。

未修复负对照 `adjacent-before-fix.log`：4 项 / 220 条中仅新私有回归的 8 条预期失败（两个接口 × 四个入口意外未拒绝），3 项原链 oracle / 200 条通过。原链对照含普通父动作先建表写行，再传空输出指针时整笔撤销，以及非零输出正常成功。对应程序和源码归档 `adjacent-before-fix-artifact.tar.gz` 已逐文件比较，SHA-256 `35578255c43bfcc0032eb44873da2bfe469c6b478225f663c37829e73c8b6f8f`；该程序是故意保留的未修复对照，不用于正式节点。现在为私有 adjacent 加非零检查，正在重建/复测。

随后修复版完整普通工作池三轮全部通过：每轮 118 项 / 8,455 条，见 `worker-fixed-{1,2,3}.log`。固定测试程序 `e9679768e1b34a5144e4850c4c0bee327cace586771ff57e84afb909a9c7cee5`。新增 CF 私有 9 项 / 324 条、原链 oracle 3 项 / 200 条；原有功能及两项新增链接回退夹具一并通过。测试中复制未使用的空签名容器曾触发 GCC 告警，改为只构造相同主体后重建无该告警；两版日志均保留。审计清单再次匹配。此处仅普通 b0 验证完成，新的 sanitizer、正式 CF 采用和进程矩阵仍待 b1 一起验收。

## b1 尚待正式接入

正式首次采用必须核对实际 packed 数据；数据核验成功后逐步骤只复用不可变已验证摘要，不能反复复制整个事务输入。原 send_context_free_inline 仍在发送位置运行账户和无授权检查；保留原 undo 和 CPU。skip_trx_checks 下原路径会跳过 CF 执行，须在首次采用前拒绝不兼容的树，不错位消费后续步骤。

`transaction::id()` 只对交易主体计算，不能用它代替 context-free 数据绑定；两份主体相同而附属数据不同的尝试必须有不同的本地执行指纹。生产 owner 当前的 skip_auth_check 门槛与 skip_trx_checks 同属 light_validation_allowed，但 adopter 仍需独立防御，不能只依赖调用者碰巧走到该门槛。

b1 预定落点：结果持有同一个 const 数据快照，executor 返回、journal 对照和 controller 准备一起绑定；adopter 第 0 步把它与实际 packed 数据核对，后续步骤复用结果内不可变快照，不重复复制/散列整段。检查真实 trace.context_free 等于组类型，按发送事件选择原 send_inline / send_context_free_inline；首次采用前拒绝 skip_trx_checks。保留默认不带快照的旧私有调用方式，但不能据此支持 CF 子组。新增正式测试要让普通父动作先写行、CF 子动作再校验数据，验证错误撤销整笔、成功原 receipt/状态一致、1/2/4 工作线程确实采用，以及之后的恢复和混合版本。

复查另发现 `transaction_context::undo()` 没有像 reset 一样清除通知/树能力和 receiver 索引；后续补齐并测试撤销/复用。顶层 context-free actions、同步调用、二级索引及其他一般 host 不在当前 b0 已实现范围。
