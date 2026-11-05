# 测试网快速重置标准流程

此流程把“准备、验证、切换、启动、治理部署、验收”固定为可恢复阶段。
本轮采用2026-11-05用户明确选择的整网从创世重建；以后的公共测试网重置仍须
属于当次明确选择的任务，隔离功能测试优先使用独立目录和端口。

## 固定输入与缓存

每轮生成不可变run_id、TEST配置、目标主机指纹、精确旧ChainID、角色与节点
名单、源码/工具链/依赖/协议特性摘要、合格节点和合约工件摘要、阶段脚本摘要。
私钥、钱包口令、签名包和原始测试状态只保留远端私有目录，不写进配置模板或Git。

复用缓存必须同时匹配实际源码输入、编译标志、依赖、工件哈希和所需测试资格。
文档修改不必重编整个节点；协议或相关源码变化不能借旧PASS记录跳过验证。
只增量重编受影响翻译单元并核验其余对象。合约字节未改则复用已审核WASM/ABI。

## 标准阶段

| 阶段 | 完成条件 |
|---|---|
| prepare | 配置/命名/预算、运行目录和允许操作已生成，计划和脚本哈希固定。 |
| qualify | 构建及对应原生、真实WASM、报文检查通过；失败尝试保留。 |
| archive_old | 停新提交、排空并核对旧交易/余额，停止旧服务，旧数据/封禁/提案/索引及配置可恢复保留。 |
| bootstrap_core | 新ChainID/密钥/目录，核心系统/代币/RAM/资源市场/保证金初始化；不预装待治理服务。 |
| developer_role | eosio.dev账号型权限，普通签名账号承接；多签不可由更弱上级权限绕过。 |
| founder_handover | gptgenesisbp真实缴保证金、自投、注册三类密钥并正常原生交接，移除创世管理/生产签名者。 |
| history_and_admission | 新索引命名空间，逐个BP真实缴押金/自投/准入，90普通账号随机投票且最终总票不同；确认实际共识策略。 |
| key_propose_and_execute | 精确审核eosio.key创建/部署/初始化，完整TEST1800秒，核对有效否决后执行且不可逆。 |
| signup_propose_and_execute | 单一创建通道空闲后，另提eosio.signup，独立完整1800秒后执行且不可逆。 |
| verify_and_publish | 实际注册、资源、签名、费用、唯一relay、历史/浏览器、业务和代码pin核对；源码与双语证据入Git。 |
| cleanup | 公有证据异地哈希核验后，仅清理本轮已停隔离临时数据；不删除旧链归档或签名材料。 |

public-testnet使用明确的旧ChainID和该轮重置授权；isolated使用独立目录/端口，
不得覆盖公共链。默认无新服务器、无TPS、无额外封禁/币量/投票规则调整。

## 执行与恢复

在保留远端主机上使用已准备的具体计划：

```sh
python3 tools/testing/testnet_reset.py prepare --plan /opt/pureeos-RUN/PLAN.json --run-dir /opt/pureeos-RUN
python3 tools/testing/testnet_reset.py run --plan /opt/pureeos-RUN/PLAN.json --run-dir /opt/pureeos-RUN
python3 tools/testing/testnet_reset.py status --plan /opt/pureeos-RUN/PLAN.json --run-dir /opt/pureeos-RUN
python3 tools/testing/testnet_reset.py resume --plan /opt/pureeos-RUN/PLAN.json --run-dir /opt/pureeos-RUN
```

RUN为每轮实际生成的唯一目录，模板不是可直接执行的部署计划。执行器核对主机、
TEST限制、计划/脚本哈希和阶段依赖，使用互斥锁与原子状态文件。成功阶段只校验
既有凭证，不重复执行；中断/失败阶段必须走reconcile，不能盲目重签或重发支付。
交易阶段的恢复要核对签名包摘要、精确交易ID、过期时间及完整不可逆规范区块
收据覆盖，确认执行过则保留，确认未发或已过期且完整缺席才可生成新的尝试。

控制器日志默认私有；公有阶段凭证仅记录脱敏结果和真实链上证据。切换前与
切换后ChainID/代码/索引必须分别核对，旧缓存或历史不可混入新链。

## 提速方式与不可缩短项

提前生成配置、复用合格构建、批量但有界的创世账号准备、准备阶段独立工作并行、
只恢复未完成步骤、仅等待必要服务健康。协议公示、原生准入、最终确认、保证金
冷却及封禁证据窗口按真实规则执行；隔离控制器时钟推进不算真实线上等待。

每轮报告实际准备/构建/停机/启动/公示/验收耗时及失败项，不预设“重置只需几分钟”。
本轮须把完整实际运行和恢复能力验证记录补入报告后，才能称流程已验收。
