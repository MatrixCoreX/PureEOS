# BP 自投、普通用户随机投票、随机业务与押金页面

状态：**准备与部署通过，五节点故障测试另行记录**。快照时间：2026-11-02T01:40:43.163802+00:00。

本阶段接续 [40 BP＋历史节点完整验收](../REPORT.md)。链 ID：`9924d9d6405276c9e4cfee86a5b497a19b5c3fa4d576508689884b8be47a0bac`。沿用保留的 AMD EPYC 4345P、8物理核/16线程、标称64GiB（可见64,880,889,856字节），Vultr `vbm-8c-64gb-amd`，Ubuntu24.04.5LTS＋发布运行时。没有额外服务器或TPS测量。

## 真实投票

全部45个BP均保留本人给本人投票；补齐了创始BP原先缺失的自投。90个新普通账户以独立密钥、本人的真实资源质押和PowerUp随机单选BP。第一组随机排列覆盖45个BP，第二组45次均匀随机选择；随机质押100–1000AIC，准备阶段重采样消除总票数碰撞。公开随机种子不用于生成私钥。

核对通过：每个BP自投110.0000AIC对应1,100,000最小票单位，每个BP均有普通用户支持，45个总票数两两不同。自投按账户身份计算，与资金来源无关；既有普通用户对创始BP的320AIC支持保留。全部资金来自已发行AIC，未增发。映射、真实整数票重和规范不可逆交易均在附件中。

自投/他人票数接口只新增已验证的系统WASM `b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170` 与 packed ABI `f19e4c55e11f5029f4a161865ac0b0ea29f730d4f19fbdc6eb30c29c10ef5512` 精确组合，保留已有版本及未知版本拒绝。11项独立模块测试和公网45个BP的total=self+other、与链上计划一致性检查通过。

## 普通账户持续随机业务

隔离应用账户 `civiccounter` 提供本人授权的tick和note动作，固定大小用户状态以单调nonce防重放。远程发布CDT1.0.0构建、真实WASM的16条断言通过，包括正常调用、错误授权、重放、越界、超长消息和失败合约导致前序转账回滚；断言数不是独立安全场景数。

| 文件 | SHA256 |
|---|---|
| 源码 | `38a8d0184b824bfba264624200fc2e34a6228e60786765d518afb24df870fac4` |
| WASM | `8a0c9b48dd7044199fa38a184a6287e0f79350462f909aa9d8afb2437de84108` |
| ABI JSON | `89524b0163849cda60aa20fc73a7a5559186e8c3d4a0fdab58fe6bd73bf84a04` |

独立本地钱包仅保留90个普通账户密钥，BP及部署者密钥已移出。每批随机1–20笔，目标90%小额转账/10%tick或note调用，实际比例留档；转账每笔0.0001–0.0100AIC。单批全部不可逆并排空后，随机等待区块且实际核对空块。网络不确定时只重用同一已签名包与交易ID，不生成重复支付。每20批核对90账户完整余额。

快照：220批、2434笔，其中转账2216、tick 108、note 110，真实空块730个，均有规范区块ID。

空块是已经生成但无交易的区块，不能当作BP未出块；缺勤仍按原生已分配个人窗口判断。1–20是模拟提交批量，其他用户和管理交易不受该模拟范围限制。正式TPS计时之前必须停止后台并排空；本阶段不宣称额外吞吐或新应用哈希的并行资格。CPU16/NET4MiB/状态16GiB、RAM64GiB+1024/块、原有并行白名单及deferred禁用保持。

## 浏览器部署

[浏览器](https://aic.matrixai.one) 已部署私有库提交 `50f07cf0aad18495403eef56f93e014274dd3b24`。节点表显示实际押金和五种状态；现有状态区显示锁定/罚没合计，账户信息显示冷却、退出、罚没决定及结算。金额精确到4位AIC，当前押金与已罚没金额分开；读取失败为未知，成功无记录才显示未缴纳。

206项测试、生产构建、本地及公网真实Chrome的中英文/深浅主题/1440与390宽度、账户详情和45个节点实际票数检查通过。前端仍为静态文件并直读HTTPS API，MatrixAI其他站点文件哈希保持，v1.0.0标签不变，浏览器库保持私有。

## 保留记录

投票脚本初次变量名冲突发生在签名前，未产生资金交易。首次普通用户PowerUp份额1e6低于最低费用，资金/质押/投票组合交易整体回滚；保留已创建账户凭证后，改为验证过的1e9份额成功，没有重复创建或充值。API测试曾重复发现引用类，原18次执行记录另存，清理后按11项独立测试计数。

附件及逐文件SHA256位于本目录；私钥、钱包、私有配置、未广播签名包未导出。五节点停机还必须等待实际入选和出块，本报告不宣称黑名单/罚没实测完成。

## English

Preparation/deployment PASS; the five-node fault campaign remains separate. All45 BPs self-vote. Ninety independent ordinary accounts self-stake and randomly select BPs using a reproducible public plan; all45 totals are distinct. The exact native self/other API profile and all live rows were verified.

The remotely built ordinary contract passed16 real-WASM assertions. Continuous1–20-transaction batches mix small transfers and tick/note calls, finalize/drain before idle blocks, and retain canonical evidence. No new TPS campaign, issuance, cap or execution-whitelist change was made. The signing wallet contains only ordinary-account keys.

The private explorer now displays exact on-chain bonds/states in both languages/themes. All206 tests, build and real local/public Chrome checks passed. Historical releases/tags and other website files remain preserved.
