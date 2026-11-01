# 开发者系统合约升级

系统升级采用“指定开发者提案、BP否决”。普通用户和普通BP不能提交、执行或撤销受保护系统合约的升级；不恢复eosio.msig/eosio.wrap管理权限。新治理协议启用后的新提案固定审查7天(604800秒)，开发者不能提前执行。

## 协议启用

所有验证节点、出块节点及历史节点须先更新到支持`PUREEOS_SEVEN_DAY_GOVERNANCE`的软件，核对相同定义和不可逆链身份，再通过本机producer API计划协议激活。该功能依赖已启用的`PUREEOS_ROTATION_CONTEXT`，无需合约预激活。不得让旧二进制参与新协议；激活后不可回退为不认识此功能的旧节点。

激活前按原规则重放；激活前已经发布的提案保留原`execute_after`，不会被追溯缩短。激活后的`review_seconds`是下一次提案的7天政策，已有提案应直接读取自身截止时间。创世profile2新链须在单节点EOSIO引导阶段完成系统初始化及协议激活，然后以604800秒调用一次性`govinit`。RAM仍通过初始化`setramrate(1024)`启用，不改变既定CPU/NET、投票、奖励或资源模式。

## 审查名单与计数

开发者必须在同一交易内先`refreshgov`，紧接`devpropose`。名单来自合格注册、正整数票及有效出块/BLS密钥的投票排名，最多210个，排除开发者与永久封禁BP。系统合约的排名容量也先排除永久封禁，候补可递补；维护及其他既定政策保持。

提案保存冻结的`proposal_voters`与原始`vetoes`。提案待执行期间，永久封禁者从有效分母N剔除；其已经签署的否决也从有效分子V剔除，历史记录不删除。新BP不会中途加入冻结名单；普通票数变化、注销或维护不重新定义已有提案的冻结权利。否决门槛为`ceil(2*N/3)`；达到门槛阻止执行。N=0时拒绝执行，不能当作自动通过。该门槛不同于最终确认的`floor(2*N/3)+1`。

## 哈希与权限

提案绑定目标账户、旧/新WASM哈希、旧/新序列化ABI哈希及审查文档哈希。ABI哈希是打包后的ABI字节SHA256，不能用人类可读JSON文件SHA256替代；可通过`get_raw_abi`核对。开发者使用独立`upgrade`权限，BP用自身权限否决。受保护系统owner/active管理权限在封存后不可绕过。`devexec`只能安装提案绑定的内容，采用一次性原生能力；失败整体回滚，不能通过特权转发伪造。

截止前可否决；截止时起才能执行。取消并重新提案会重新计算完整7天。任何外部定时执行器仍须在实际链时间达到截止后提交新签名交易，并重新验证待处理ID、哈希、有效名单和否决状态；不得使用已废弃的deferred交易或修改系统时间/数据库来跳过审查。

## 查询与验证

`get_upgrade_state`保留原字段，并提供`governance_v2_activated`、`effective_proposal_voters`、`effective_vetoes`、`excluded_banned_voters`、`effective_voter_count`、`effective_veto_count`、`required_vetoes`及`execution_blocked_no_electorate`。这些有效数按查询head状态计算，不是已完成提案执行时的历史快照；完整历史以链上交易和索引为准。

[固定测试项目与结果](../tests/qualification/governance7-20261101/README.md)覆盖7天精确边界、旧提案截止不变、黑名单分子/分母、权限拒绝、零名单/异常行、维护、排名递补、正常升级和重放。在线原始合约须走实际7天审查才能替换，离线测试通过不等于已经在线生效。

# Developer system-contract upgrades

Only the designated developer may propose, cancel or execute protected system upgrades; BPs exercise a veto. Ordinary users/BPs gain no proposal authority, and legacy multisig/wrap management stays disabled. After `PUREEOS_SEVEN_DAY_GOVERNANCE` activation, new proposals require exactly604800seconds. All producers, validators and history nodes must first support the feature. It depends on rotation context and requires no WASM preactivation. Previously pending deadlines stay unchanged; replay before activation preserves prior semantics. A fresh profile2 chain activates the feature and seals with604800seconds during EOSIO bootstrap.

`refreshgov` must immediately precede `devpropose` in the same transaction. The frozen electorate contains at most210qualified ranked BPs, excluding the developer and permanent bans. Permanently banned accounts never consume ranked capacity; successors backfill. Pending proposals retain their original electorate/veto records, while bans remove accounts from both effectiveN and already-castV. New entrants, ordinary vote changes, deregistration and maintenance do not rewrite an existing frozen electorate. A veto requires `ceil(2*N/3)`; an empty effective electorate blocks execution. This differs from the native finality threshold.

Proposals bind target, old/new code and packed ABI hashes, plus a review-document hash. Packed ABI SHA256 is not the JSON artifact SHA256. Dedicated developer upgrade permission, protected account sealing, top-level authorization, hash binding, single-use execution and atomic rollback remain enforced. Vetoes end at the deadline; execution cannot start earlier. Cancellation/resubmission starts a new full review. External timers must submit fresh signed transactions after actual chain time reaches the deadline and recheck ID, hashes and effective vetoes. Deferred transactions, DB edits and wall-clock manipulation are not upgrade mechanisms.

The read API retains raw records and exposes effective counts and exclusions as of its head. These are not historical execution-time snapshots. See the pinned qualification project linked above. Native unit simulation is separate from live rollout and the real7day contract review.
