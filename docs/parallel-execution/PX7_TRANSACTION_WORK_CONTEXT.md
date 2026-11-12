# 每笔交易工作量生命周期组件与接线位置

本文件对应新增 `transaction_work_context.hpp`。组件已经实现本地预算预留、
一次语义账单、跨重试签名 seed、代次和一次采用管理；尚未接入实际
`transaction_context`，没有激活新收费或区块容量规则。

## 代码接口

`transaction_work_policy` 只存共同策略版本、单笔语义工作上限和内存价格。
它必须来自经过认证的链配置，不能来自生产者声称的账单、本地线程数或计时。

`transaction_work_local_limits` 单独存累计试算工作量、试算次数和可回退的
owner 尝试次数。不同本地限额可改变是否试算、是否回退，但不能改变最终
语义费用或区块有效性。

`transaction_work_seed` 只允许两类已经执行一次、可复用的工作：

- 根据真实交易签名类型和数量计算的逻辑签名工作量；不能使用当前
  `signature_cpu_usage()` 的实测微秒，也不能因签名恢复缓存命中变成零。
- 不依赖链状态、确实只执行一次的交易固定前置操作。若解析或其他操作会
  在重试内实际重跑，必须由 attempt 计量，而不能假设它一直在 seed 内。

权限树、代码、资源、payer 等依赖 canonical prefix 的操作都不是持久 seed。
新的 epoch 必须重新读取、验证并计算这些工作。

典型流程：

```cpp
transaction_work_context work(common_policy, local_limits, immutable_seed);
auto reservation = work.reserve_trial(per_attempt_allowance);

// reservation 可移动到 worker；执行对象和它的 meter 不能移动。
auto trial = reservation.start();
{
   auto binding = trial.bind_vm(worker_vm.get_context());
   // 所有 action、notification、sync frame 使用同一个 trial。
   // 执行插桩后的 VM 与原生成本 hook。
}
auto receipt = trial.seal_success();

// owner 先独立验证真实代码、环境、授权、依赖、完整 effects 与当前 epoch。
auto semantic = work.begin_retryable_attempt();
semantic.adopt(std::move(receipt)); // whole-TX 计数摘要，只导入一次
semantic.consume(owner_work);     // 与已经导入的逻辑 host 工作分清边界
auto final = semantic.finish();
```

若本地额度不足以预留可回退 owner 尝试的最坏工作量，直接选择
`begin_final_attempt()`。最终 canonical 尝试不需要本地试算额度，但不能再
调用 `discard_for_retry()`。它仍受共同单笔语义上限约束。

## 失败、回退及生命周期规则

- 私有尝试先预留其最大额度，防止多个 worker 合计超过同一 TX 的本地上限。
  健康结束只结算成功计量的工作，退回未用额度；已关闭 ticket 不会重开，
  尝试次数也不会退回。即使零工作取消，也不能无限重新试算。
- 失败或未正常封口的试算保守地用完整笔预留。`usage()` 仍只包含成功预扣；
  这不是给用户补收失败费用，而是防止原子预扣失败留下余额后不断免费重试。
  本地试算失败允许最终 canonical 执行；语义预算耗尽则永久终止这笔 TX。
- 可回退 owner 尝试在开始前预留 `semantic_limit - seed.total()`。放弃时只把
  owner 自己的工作计入本地累计额度，减去已复用 seed 与已结算的 private
  import；不能再把相同 private 工作收一次。成功时这些 owner 工作属于最终
  路径，释放其重试预留。
- `advance_epoch()` 只能在没有活动 owner 尝试时执行。旧排队 ticket 不准
  开始；已经运行的旧 trial 可以在既有上限内收尾，但旧 receipt 不准采用。
  共同策略版本改变时，controller 必须丢弃旧 context，不能只推进 epoch。
  此时仍需保留外围总本地配额，不能通过重建 context 反复刷新全批次预算。
- receipt 是不可复制的进程内来源凭据，不是生产者提交的密码学证明。它
  只能在同 context、同 epoch、成功封口状态下采用一次；它不认证数据库
  effects、权限、代码或状态依赖。
- `bind_vm()` 检查模块的显式插桩标记。未插桩模块不能产生空的合法工作量。
  当前标记还不是正式计量版本核验；编译缓存版本和共同价格版本必须在
  实际 runtime 接线中关联。
- sync 子调用用 `attempt.bind_vm(child_context)` 共享相同 meter，不创建新
  semantic attempt。重入同一 VM context 也由组合 guard 恢复原绑定。
- `finish`、`seal_success`、`discard_for_retry` 必须等 VM/native borrow 退出。
  提前封口会使该尝试失败，不能 catch 后继续。组合 guard 先解除 VM 绑定，
  再释放 meter 存储；预算 slot 因此不会因错误的销毁顺序立即悬空。
- 这只保护预算指针，不延长 controller、DB session 或 host interface 的寿命。
  执行对象按线程使用，controller 仍必须取消并 join 所有 worker 后再销毁。
  编译/装载、队列元数据、外围重试次数与物理截止保护仍需独立有界配额。

## 实际代码接线

1. `controller.cpp::push_transaction` 当前在约 4021 行取得实测签名 CPU，
   4039 行构造 `transaction_context`。新增逻辑签名 seed 应从已解码且类型
   已验证的交易输入和共同价格计算，不能把现有本地 `sig_cpu_bill_pct` 当作
   共识工作量规则。对显式收据也必须重算并核验。
2. 私有 batch 在 TC 创建前就开始执行。context 应由该 TX 的 batch/owner
   持有，先发出试算 reservation，再把同一个 context 交给 TC；不能在 worker
   与 TC 分别创建两套预算。若加入 metadata，须同步估算其队列内存占用。
3. `parallel_wasm_worker.cpp::isolated_wasm_module::execute_step` 是单 frame 的
   当前边界，约 1142 行开始。不要在那里每个 action 创建新 trial。应由
   transaction/tree/notification/sync/mixed 的整个 TX executor 持有一个
   trial，并将其 scope 传入所有 start/execute/native hook。
4. 当前 worker 使用 `isolated_vm_options`，普通路径未启用新工作量检查点。
   接线时必须使用插桩编译选项并绑定预算，然后才初始化 globals/memory 和
   start。模块缓存必须包含共同计量版本；不支持同等计量的 OC 路径要拒绝。
5. `transaction_context.cpp::exec` 的 OC 中断会在约 423 行调用 `reset()`。
   新工作 context 不能跟着现有 `initialize()` 重建。健康的优化回退先
   `discard_for_retry()`、撤销 DB、推进 epoch，再开新 attempt；实际语义
   资源异常不可走此路径。没预留重试额度时直接执行最终 canonical 引擎。
6. 当前 `charge_isolated_cpu`、notification/tree/mixed 入口按 frame 收实际 CPU。
   新语义摘要按完整 TX 导入一次；所有采用模式共用同一个入口，不能复制
   当前逐 frame CPU 收费而再次导入整 TX。导入前先做原有 code/context/
   dependency/effects 验证，验证失败不得调用 `adopt()`。
7. 原生 replay 中已经在 imported host 费用计入的逻辑操作不能再次收费；
   另外发生的授权、验证、提交开销需要自己明确的成本 hook。缺失这些边界，
   本组件不保证整个项目已经完整计费。
8. `transaction_context::finalize` 成功后调用 `finish()`，将原始工作摘要、
   共同策略版本与账户账单分别写入新收据。账单 floor 和签名折扣只由共同
   定价规则计算；block 总量、串行/关键路径、账户虚拟供给与 onblock 预留
   由其他组件接线，本文件不直接放宽原来的 200000us 限制。

## 验证范围

纯组件测试 `transaction_work_context_test.cpp` 的 22 组用例包含状态机、并发预留、线程
来源、内存分配失败、UINT64 边界、1000 个固定种子的试算历史、失效代次、
重复采用、seed 和 owner retry 去重、借用指针生命周期等场景。

`transaction_work_context_vm_test.cpp` 的 6 组用例另外使用实际解释器与 JIT，核验内存
初始化、相同程序从试算超限回退到 canonical、一次采用、原生回调中的
子 VM 共用预算、提前 finish 被捕获后仍不能成功，以及未插桩模块拒绝。
其中所有价格都是测试输入；这些测试不代表 Node、数据库采用、正式成本
表、区块收据或 TPS 已完成。


本地验收：22 组组件用例在普通、ASan/UBSan 各三轮通过；6 组真实 VM 用例在
普通、ASan/UBSan、SoftFloat 各三轮通过，合计 186 次顶层用例执行。
每次组件运行还包含 1000 个试算历史的独立支出 oracle。消毒器覆盖 C++
预算/RAII/异常桥接，不覆盖生成的 JIT 机器码和缓存 SoftFloat 归档。
控制性错误对照及最终源码快照在独立证据目录
`CPU-M3-transaction-work-20261025-attempt-1`，不属于生产启用证明。
