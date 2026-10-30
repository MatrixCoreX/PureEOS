from pathlib import Path
import fcntl,json,time,sys,hashlib,collections
B=Path('/opt/aic-bp-discovery-live-20261030');C=Path('/opt/aic-bp-discovery-30nodes-20261030');E=Path('/opt/aic-bp-maintenance-30nodes-20261030');F=Path('/opt/aic-bp-maintenance-feature-20261030')
R=Path('/opt/aic-performance-results/machines/amd-epyc-4345p-8-core-processor--baremetal--8physical-16threads--65536MiB')
sys.path.insert(0,str(B/'input'));import functional_common as f
with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
 fcntl.flock(lock,fcntl.LOCK_EX)
 catalog=json.loads((B/'input/FUNCTIONAL_CASES.json').read_text());catalog['original_rounds']=13;catalog['rounds']=14;catalog['original_planned_cases']=134;catalog['additional_maintenance_cases']=20;catalog['total_current_cases']=154;f.write(B/'input/FUNCTIONAL_CASES.json',catalog)
 counts=collections.Counter(x['status']for x in catalog['cases']);now=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
 sections=[
  '## 2026-10-30 三机功能验证续记（'+now+'）',
  '这是进度记录，不是全部验收完成或发布批准。原13轮134项，加维护第14轮20项，共154项。当前状态计数：'+json.dumps(dict(counts),ensure_ascii=False)+'。TODO和PARTIAL不计通过；各次原始FAIL保留。当前所有工作均为功能/安全验证，节点TPS未测。',
  '硬件仍为原三台EPYC4345P（各8物理核/16线程、64GiB），未新建云服务器。原应用链65cc0acd...保留运行；维护验证用独立链c3e3a6e8...，各30个独立并行nodeos、独立数据和safety目录，曾在同三台主机并存。不能把共享硬件模拟当30/60台实机性能结果。CPU16=3200000us，NET16MiB/块，P2P64MiB/消息，生产队列1GiB；8worker/batch24/group8，单笔和计费规则不变。RPC读取到额外max_action_return_value_size=256，原值保留；第一次严格字典相等检查因preset未列该扩展而失败，属于测试断言形状错误，无参数变更。',
  'Node生产源码ce13ad23934dda44d418d8c6dd9882ba73496c97，含完整测试源码4ede04b47318fdff90389a0450098bc94dd9d3c0；Node SHA256 6e9c668586dfae11f82a461028a734704e853b32f7acc1232559ffbf4de68cf7。候补断种子修复：组件和变更代码ASan/UBSan通过；30实例保留数据/safety滚动升级、两种子+第三进程停止时其余27在原60秒内确认转账，恢复后全30区块一致。证据/opt/aic-bp-standby-seed-failover-20261030/live-qualification/QUALIFICATION.json。',
  '功能模拟启动器过早绑定主线程，导致后建worker继承单核亲和性；已删除这一步并将当时30实例全部线程恢复允许16逻辑CPU，随后全30转账确认通过。没有改Node字节或链额度，也没有据此宣称历史TPS提升。证据/opt/aic-bp-discovery-30nodes-20261030/worker-affinity-fix/QUALIFICATION.json；后续维护实例从启动起全部CPU可用。',
  'R05 NET-01..09及BAN-17通过：断连/重复交易一次记账、单节点P2P隔离、满足/不足门槛分区、恢复同链、错误链/坏签名/64MiB+1长度头拒绝、真实自有P2P50ms延迟和队列检查。临时nft/tc故障规则已撤回。证据/opt/aic-bp-discovery-30nodes-20261030/NETWORK_FUNCTIONAL_PASSED.json。',
  '真实固定版本AtomicAssets、AtomicMarket、Bancor Token/Converter/Network五份WASM已部署，30节点链上code hash和ABI一致。NFT-01/02/04/05完成；NFT-03仅权限、重复ID、错误scope等已执行子项，伪造通知未完成。DEF-01/02/03/05/09完成；DEF-06/07仅部分数量和充值提现子项。单跳正反兑换按固定公式和最小单位核对；原inline深度4拒绝直接双跳，失败整笔回滚，未扩大限制；同一交易内两个顶层动作组成两池兑换通过。跨合约兑换后第二动作失败，全部钱包/储备回滚。其余DeFi/NFT/VM/系统ABI支持动作仍需按计划覆盖，不能称所有第三方合约安全。证据C目录third-party-live及B/rounds/R09、R10。',
  '第三方准备脚本误用治理封存后的eosio@active，和账户未建立/权限列表未排序，均被原权限检查正确拒绝。改用正常资金developer建12字母账户、账户自质押投票、建立全部账户后排序加入明确eosio.code权限。NFT schema遗漏必需name:string、模板同时不可转让和不可销毁，均为测试输入错误；查固定源码后修正，原失败保留。没有绕过治理、改合约安全检查或放宽链上限。',
  '维护当前候选源码63255178dd824948afc723859cebe9321691a0e9，WASM SHA256 6ac9dafc4c74fb47f9ce1897be65d98cfc30ddf2c66cfe68b51ca623c8db435d，ABI SHA256 6809814cb91cdfb55e9cf9bfb926343b9a8be02f82b8996796e11173eec40a9c。原ABI条目全保留，仅新增setmaint/bpmaint；单选、轮岗/双权威、10/90、奖励减半/治理、RAM64GiB初始+setramrate1024、deferred关闭不变。新版本Native39971/39971断言（13个检查组）通过，包含每次交接中当前proposer/finalizer发现身份检查。数字是断言数，不是TPS或39971种独立场景。',
  '6325517额外显式加入当前Native finalizer身份并集，属于保守权威覆盖。旧79b36c4 WASM对同一属性Native对照也通过41627断言；没有复现旧版本在当前profile缺失finalizer的反例，不得称已证明旧版本存在此缺陷。对照评估/opt/aic-bp-maintenance-feature-20261030/finalizer-identity-negative-control/CONTROL_ASSESSMENT.json；原错误的“必须失败”控制器断言保留。',
  '历史权益成熟路径通过独立新创世Native虚拟25小时验证，原24h常量不变：维护时结算先前24个出块积分，钱包8000.0000→8062.8918 SYS，维护状态仍2，重复领取拒绝；整组64454/64454断言通过。真实30节点链尚未满注册24小时，原冷却拒绝且历史积分/钱包不变，是正常行为；最初用例错误期待立即可领的FAIL保留。未来时钟实验链绝不部署为live。证据/opt/aic-bp-maintenance-feature-20261030/reward-maturity-native-lab/fixture/REWARD_MATURITY_RESULT.json。',
  '原应用链bpaac已在较早滚动故障期间形成真实三次个人零窗永久ban：evidence_slots=[1693278720,1693278972,1693279224]，同policy23，ban_final_block11771。该角色保留，不清洗。M30-06第一次查询误用voters而非votersv2；下一次210秒期望所有30个进程均持BP发现记录也不正确：29个合格BP已恢复，唯一缺失是已ban普通观察节点，其普通P2P/账本同步正常但BP发现权限没有恢复。已修正按实际角色检查，业务最终确认仍含全部30实例；原FAIL保留。此事不能宣称为新的Node发现bug或自动解封。',
  '仍有全网378秒暂停后20BP永久ban的隔离旧WASM失败（BAN-12），/opt/aic-bp-blacklist-pause-lab-20261030/fixture/BAN12_RESULT.json。全网停摆个人计罚口径已向用户提出选择，未收到明确答复前不擅改这项业务规则。该问题及所有未完成项继续阻止整体基线提升/发布。',
  '最新Node和系统合约源码已保存到本机Git review refs，实验分支已有Git提交，稳定基线未被未经验证的候选覆盖。本机仅接收源码、明确要求的Markdown和小型hash回执，没有拉回原始运行归档或本机编译/性能分析。',
  '清理约束再次确认：完成必要扩展多节点测试、保存详细项目/结果并核对hash后，DELETE新增db8ee7ed-2357-436c-8614-d0f485f52ed6（66.245.194.132）及e93d8e9e-43df-4a7b-b203-38e94b612f2e（136.244.110.182）。只保留原机3b0155c9-16ef-48e9-afb3-4c5e838e5184（66.245.195.50）。目前未删除，不用关机代替删除，也不删除其他服务/机器。'
 ]
 p=R/'main-thread-optimization-experiments-20261029.md';p.write_text(p.read_text()+'\n\n'+'\n\n'.join(sections)+'\n')
 progress=R/'p2p-maintenance-functional-progress-20261030.md';progress.write_text('# 多节点发现与维护功能测试进度\n\n'+'\n\n'.join(sections)+'\n')
 plan=f.PLAN;plan.write_text(plan.read_text()+'\n\n阶段进度 '+now+'：14轮154项，状态计数'+json.dumps(dict(counts),ensure_ascii=False)+'。详细说明与原失败保留在'+str(progress)+'；尚未全部验收或发布，新增两台测试机未删除。\n')
 p=R/'rotation-and-planned-maintenance-mechanism-20261030.md';p.write_text(p.read_text()+'\n\n实现续记 '+now+'：当前候选63255178dd824948afc723859cebe9321691a0e9、WASM6ac9dafc4c74fb47f9ce1897be65d98cfc30ddf2c66cfe68b51ca623c8db435d；Native39971断言通过，额外历史权益虚拟时钟整组64454断言通过。原24h冷却保持。三机30实例维护基础项目已有实际结果，未执行/部分分支继续明确标记，不能按全部20项完成理解。最终以测试计划和进度报告为准。\n')
 f.write(C/'LATEST_PROGRESS_RECORD.json',dict(UTC=now,case_counts=dict(counts),total=154,source_node='4ede04b47318fdff90389a0450098bc94dd9d3c0',source_system='63255178dd824948afc723859cebe9321691a0e9',report=str(progress),baseline_promoted=False,cloud_extra_two_deleted=False,primary_kept=True))
 print(json.dumps(dict(report=str(progress),counts=dict(counts),baseline_promoted=False,extra_two_deleted=False)))
