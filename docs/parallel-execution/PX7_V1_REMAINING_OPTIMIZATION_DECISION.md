# 首版剩余优化：代码复核与实施次序

日期：2026-10-22。这是代码级判断和后续验收安排，不是 TPS 实测报告。
每个候选的源码和输入单独冻结；新改动进入下一候选，不改写正在验收的源码。

## 已确认的成本来源

1. `controller.cpp::prepare_isolated_attempts` 在每个未完成交易的发现波次中，
   由 `input.roots/input.programs` 构造新的 worker job。当前 vector 按值复制；
   mixed 和 sync 输入也有相应复制。初始根准备函数只取 code hash 和 action，
   不复制 WASM，不能把这一步也错误算成一次 WASM 深复制。
2. `parallel_canonical_adoption.cpp::prepare_isolated_tree_program` 由 owner
   复制实际代码并核验 SHA-256。不能直接让 worker 借用 Chainbase 的 code_object。
3. `parallel_wasm_worker.cpp::leased_modules` 最多九个模块槽；命中比较完整
   代码和 limits，未命中时还要保有编译输入。递归祖先正在租用的槽不能重置。
4. `parallel_ordered_batch.cpp::inspect_candidate` 对结构改变保持保守冲突；
   普通行更新已有精细的主键/查询检查，并不是“同一个合约就全部冲突”。
5. 正式 adoption 仍需要原宿主授权、RAM、CPU/NET、迭代器、通知/inline/
   同步调用及撤销处理。这部分是串行提交，不等于智能合约重新全部串行执行。

因此，N1 减少发现波次、N4 减少无任务唤醒具有明确落点，但不能据此保证总 TPS
或宣称几十倍提升。轻量交易可能仍由 owner、代码复制、校验和提交成本主导。

## N2 与 N5：联合推进，不只替换一个 hash 比较

建议同一后续候选采用不可变代码对象，先做正确性原型，再决定是否进入发布候选。

- owner 从真实代码复制、校验后构造不可变对象；worker 只接收拥有生命周期的
  `shared_ptr<const ...>`，不共享 VM、overlay 或 controller/database 指针。
- 每个 job 的 roots/catalog 自身不可变；发现新 receiver 或环境时生成下一版本，
  不能在其他 worker 读取期间原地追加 vector。取消后仍必须 join 全部 worker。
- 原有逻辑代码字节、输入、发现次数和绝对时间预算继续逐项计入；物理去重不能
  偷偷提高允许的合约规模，也不能让旧对象无限驻留。
- 模块命中可使用可信对象身份做快路径，但身份必须来自受控构造/有界驻留表；
  外部声明的 hash、地址或 code_object ID 不能独立充当等价证明。不同对象同字节、
  代码更新、分叉撤销、limits/引擎变化、负缓存、递归活跃祖先分别验证。
- 统计实际 job 构造复制字节和模块比较字节，分别报告冷/暖、1/4/16 worker、
  1/8/64 笔、根/inline/通知/sync/mixed、单代码和多代码的结果。

复杂度估算：若 B 笔交易、每笔代码目录 P 字节、D 次发现波次，当前仅 job
代码复制项可达约 B×P×D；只读共享可以消除此处的重复 payload 复制，但不能
消除 state capture、VM 执行、指纹、正式提交或编译。这个式子不是 TPS 倍数。

### 已实施的 N2 子集：同交易、同目录版本、跨发现波次只读复用

第六候选新增 `parallel_immutable_input.hpp`，tree/sync/mixed job 持有拥有生命周期的
只读输入。owner 在 joined preparation 内首次复制每笔交易目录；仅新状态查询
不重复制，新增 receiver、捕获环境或模式升级前使目录失效，下一轮产生新版本。
已完成的交易立即释放缓存，不在其他交易的稀疏尾部持续保留。缓存不跨 batch、
前缀刷新或正式提交；不共享 VM、overlay、数据库，也不跳过原指纹和逻辑预算。

实际 helper 的独立程序：普通、ASan/UBSan、TSan 各三轮通过，含拷贝失败恢复、
错误线程拒绝和 16 个只读线程。64 笔×64 波次、目录不变的复制计数由原构造方式
需要的 4096 次降为实际 64 次；这是局部复制次数，不是整链吞吐倍数。
证据 `immutable-input-native-attempt-1/report.json`。新增真实 WASM 用例显式覆盖
解释器/JIT 与 1/4/16 worker、旧新版本共存和原模块字节上限；整合结果仍待第六
候选验证。不能仅凭 helper 通过就标记 N2 整体发布验收完成。

N5 模块身份快路径、跨交易代码驻留/去重尚未实施，保留原字节比较。N2 本轮仅
消除发现波次 job 构造复制，没有宣称消除初始目录、环境、编译或提交成本。

## N3：先采样，再移除重复工作

已有 `parallel_capture/batch_wait/compile/execute/adopt/fallback/conflict` 分阶段
计时。下一轮应补充复制字节、指纹字节和核验次数，以区分“等待执行”和“owner
忙于构造/提交”；`batch_wait` 包含 worker 工作，不能与 execute 相加当独立 CPU。

可复用的证明只能属于同一交易、同一实际前缀、同一输入与环境。前序写入、
code/permission/配置改变、undo/fork、重新捕获都会使相应证明失效。不能仅凭
同一个 tx id 就跳过正式检查，也不能把访问路径提示变成授权或有效性证明。

当前决定：不删检查、不缩短回执、不改资源结算；N3 微优化待真实分阶段证据。

## N6：保持结构冲突保守路径

进一步放开同表结构变化，需要证明不存在/存在、first/last、lower/upper bound、
next/previous、重复二级键、表删除/重建和对象 ID 分配全部等价。仅比较写入主键
不同不足以证明范围查询、空表和游标仍有效。

当前决定：首批新候选保持现有安全路径，不为提高采用率去掉结构冲突检查。
后续原型须与串行 oracle 比对逐步轨迹和最终账本，并包含失败撤销、快照重放。

## 本次部署门槛

`v1_candidate_deployment_test.py` 只接受同候选构建、controller、worker、
访问提示/唤醒/只读输入组件、黑名单退出及历史收益组合场景三轮全部通过，
并额外要求叶子执行重叠十轮稳定检查通过的归档。
再对程序、支持源码和全部 WASM/ABI 做摘要校验，才允许在现有 A 私有目录起链。
新链配置 JIT/4 worker，执行 1→4 接入、排名、实际不可逆出块、资源 10/90、
REX CPU/NET 租赁、少数/多数故障及恢复。此轮不是 40 节点、不是性能验收。

原费用截止北京时间 22:45、低配 TPS 取消、14 天/1461 天/86400 秒正式常量均
保持不变。若核心未通过或剩余时间不足，任务必须拒绝部署，不自动购买或延期。
