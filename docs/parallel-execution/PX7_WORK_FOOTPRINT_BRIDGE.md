# 并行容量访问集合桥接

本批新增 `parallel_work_footprint.hpp`，把已封存 overlay 的数据库观察、完整写日志和可信 owner 补充观察，转为 `parallel_work_budget` 所需的排序、去重读写键。它尚未接入普通 Node，也不改变现有收费、共识、治理、奖励或资源比例。

只有私有数据库观察时，输出为 `complete=false` 且无键的整体顺序屏障。不能把 `matches_base_reads`、私有执行成功、生产者声明或这个桥接器的返回值，当作授权与采用证明。

## 逻辑键

数据库统一使用完整 `(code, scope, table)` 三个 64 位值的整表 guard。主键点查、范围查、未命中、五类二级索引和表元数据都读这个 guard；任何主表或二级写都写它。因此点写不会绕过范围读，插入空表、删除最后一行及删除后重建也不会丢失冲突。

这里没有对 256 位二级值截断或哈希。整表 guard 明确不包含二级值，代价是同表不同主键、不同二级索引之间可能产生假冲突。后续若要细化，需同时改范围和生命周期保护；不能只换成精确行键。

其他域保留 account、code、permissions、RAM、CPU、NET、account sequences、configuration、protocol、producer policy、native global。每个账户域也读该账户的 existence guard，使账户创建/删除类写与依赖它的读写相遇。权限暂按整个 actor 保守保护，不能漏掉父权限、未命中及层级变化。全局域不压缩 feature digest，先用整个逻辑集合。

`execution_work_key` 仍是 domain 加五个 64 位字段；没有修改既有 budget 的结构。所有名称直接使用完整 `name.to_uint64_t()`。

## RAM 必须按真实日志判断

实际逻辑见：

- `host_context.cpp:335`，`db_update_i64`。
- payer 变化时，约 365、371 行分别退款旧 payer、向新 payer 收费。
- payer 相同，仅 `old_size != new_size` 时才调用 `update_db_usage`，约 378 行。
- 建表、删表分别在约 248、272 行更新 table payer 的 RAM。
- `apply_context.cpp:780` 的 `update_db_usage` 最终进入 `transaction_context::add_ram_usage`。

桥接器从 frozen base 的 find 元数据建立原有 row payer/尺寸和 table payer/count/generation，再按原写日志逐项更新。主表同 payer、同尺寸的 update 不虚构 RAM 写；二级键尺寸按索引类型固定，同 payer 更新也不虚构 RAM 写。payer 或尺寸改变，记录真正受影响的旧/新账户。table payer 仅在建表及删除最后一行时产生 RAM 写。

不能将整个日志折叠为最终状态。A→B→A 仍要保留 B 的中间 RAM 影响；表删除后重建不能把旧 generation 的行当作新行。缺少初始 find、对象/generation 不符、缺失 payer、未知操作、计数溢出、捕获不完整等，均不输出半套可并行键。

这避免一个明显的假依赖：四个余额表虽然都由同一创世账户支付建表费，A→B 与 C→D 的定长余额修改，并不因此共享这个创世账户的 RAM 写。若同时真的扩行、更换 payer 或删表，则共享 RAM 依赖仍须保留。 这个正例只证明数据库/RAM 不应因共同创建 payer 产生假依赖，不证明完整交易的接收序号、授权序号和其他提交状态都已得到并行证明；这些必须由后续真实 owner 接线与串行工作预留处理。

## owner 接线边界

`canonical_work_coverage` 是可信 C++ owner 的接口约定，不是密码学证明。默认八项均未确认：语义成功、同一 canonical 前缀、完整 action tree、代码与 native handlers、授权、交易资源、其他 native 状态、协议与提交。只有 owner 在相应真实路径完成检查并收集实际观察，才可全部确认。

建议接线顺序：

1. 在 `transaction_context` 的真实授权、资源及最终化路径收集 owner 观察，绑定当前交易及 canonical 前缀生命周期。不要从用户字段、worker 声称或本机线程数构造这些观察。
2. 只有整个私有结果经过 `validate_isolated_*_current_state`，并且实际采用/原始 host replay 成功，才能使用相应完整 journal。单 action 结果不能冒充完整多 action、通知、inline、sync 或 mixed tree。
3. 将所有动态 receiver 的 code/VM/privilege、授权访问的全部 actor、CPU/NET 账户、额外 RAM、协议配置和 native 操作访问合并。现有 `read_accounts` 只证明 `is_account` 身份，不能替代这些观察。
4. 收据、序号和其他 canonical commit 操作必须单独计入 owner 串行工作。桥接器不会自行把共享序号写当作可忽略操作；后续 phase-aware 优化需要额外证明。
5. 在交易 undo 仍有效时完成 footprint 与确定性工作核验及 `try_add`。被拒绝候选需原子撤销；失败或部分采用不得重新执行已采用前缀。
6. 发生 canonical 回退、分支变化、缺失状态或只覆盖了部分树时，输出整体屏障，直到真实执行记录补齐。底层捕获、资源、中断及内存异常传播；不能 catch 所有异常再把部分键标为完整。

目前没有真实 owner 收集器与生命周期令牌，也没有正式成本表、收据/资源供给及实际批次调度包络接线，所以该组件不授予生产并行容量信用。

## 验证范围

`parallel_work_footprint_test.cpp` 使用与真实 const accessor 对应的 owned 结构模型，验证访问映射及日志算法，不伪造 Chainbase、授权或实际采用。覆盖：

- 默认及任意部分 owner 覆盖均为空键屏障；未授权状态不读取 frozen base。
- 未命中和全部主表范围查询与插入冲突。
- 全部二级查询族、256 位完整输入、跨索引共享表生命周期。
- A→B、C→D 同创建 payer、同尺寸更新可独立；payer/尺寸变化确实依赖 RAM。
- A→B→A、删最后一行/重建/继续更新、二级 payer 变动。
- 不完整日志、未知域/操作、长度与键数边界、排序去重。
- 1000 个固定种子逻辑表/RAM 冲突独立 oracle。
- 每个实际分配点的失败不改变已发布 footprint 或源状态模型。

另有 `parallel_work_footprint_real_types.cpp` 编译检查，真实实例化 `primary_overlay_result` 及其 frozen base 接口。本地 FC/Boost 依赖头来自已有 PureEOS sibling checkout，命令保存在证据中。该检查不执行真实数据库，也不等同整节点编译/运行验收。

所有测试费率和调度数都是组件测试输入；本批没有 TPS 测量。
