# 开发者提案页面、导航与 RAM 计费升级（2026-11-04）

状态：开发、发布、完整1800秒原生公示、不可逆升级、真实交易/历史/浏览器验证与临时磁盘清理全部完成。

## 页面与发布

[开发者提案页面](https://aic-test.matrixai.one/#/upgrades)与治理页读取原生get_upgrade_state，显示最新提案、开发者、目标、公示时刻、有效选民/否决门槛及默认折叠的哈希/选民。按实际链上区块时间计算剩余时间；公示结束仍待执行。RPC只保留最新提案，pending=false不区分执行或撤回，因此显示“已结束”，单独报告当前代码匹配，未伪造完整提案历史。

导航栏取消可见滚动条，矮屏使用紧凑间距，保留滚轮/触摸/键盘访问全部入口；正文与数据表滚动不变。桌面误显示的手机菜单按钮同时修正。中英文、深浅色、1366×768/1440×900/390×844/844×390实际Chrome导航验证均通过。

最终静态发布：/var/www/aic-test/releases/20261104T074327Z-dev-proposals-ram-fees。六个公网文件逐字节SHA256匹配；JavaScript内容摘要在仅CSS迭代中保持相同。前端源码提交e2b1eb6fb6aa77c32a88193694efc98263bdfe9e，98个构建输入匹配Git，最终源码归档SHA256为80ad955b35218680568deb4627785b452bd14212118cc1cf84cfab521f76a99a。前端源码归档仅保留在私有浏览器仓库及本地证据；公共合约仓库不包含其源码或完整私有归档。

## 实际原生提案与升级

链ID：116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0。

- 提案#2：[提交交易](https://aic-test.matrixai.one/#/transaction/f5cabd10e12c3e55ac0b8fcaeeff9662617a4807a954d1f3edfda079c532985b)，不可逆块160808。
- 原生提交时间2026-11-04T07:31:10.500UTC，最早执行2026-11-04T08:01:10.500UTC；原生公示1800秒，24有效选民/16票否决门槛。
- [实际devexec交易](https://aic-test.matrixai.one/#/transaction/b029b9947a608a679baf6445a0e2d106fe2bc8601e59db58a98f5d963f45ff33)，不可逆块164544，时间2026-11-04T08:03:18.000UTC。实际间隔1927.5秒，执行回执executed，CPU用量7132微秒。
- 新WASM：bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1；原始ABI摘要仍3470a5e243680c4c93e7ef4b481bcd54cb4f626ed3d92331f414386bb5bb0043，无数据迁移。

复用此前已远端通过114正常原生/115明确标注夹具/21SDK的相同bc3358d7候选，没有重编译或替换已审核字节。来源提交d5c176e7d9f14ad98129c5631f3bbdece8a3ba3e，资格源码归档6d1e987f8d5dbb3f65c763d348954ab7b8c002d302d76ef11f56754f8a7250a6。审核文档及摘要见RAM_REVIEW.json与PROPOSAL.json。正式版本七天不变，无直接setcode、提前执行或重启链。

## 实际费用与本金

新增行按0/512/1536字节核定预算，读取当前已审核RAM/AIC储备：net=ceil(Q×B/(R−B))，RAM=ceil(net×200/199)，含0.5%买入手续费。CPU/NET PowerUp市场倍率仅乘执行项，基础费=max(100原子单位,执行项+RAM)，再加10%–30%中继服务费并向上取整。保留0.0100 AIC基础保底、签名总费上限、赞助预算、币种白名单、USD命名账户预先open及领取免应用费。此为预算市场报价，不是原生RAM精确计量，也不执行自动RAM购买。

三笔真实小额转账仅使用原创世HTTP中继30%；第三笔由现有原生cleos旧请求签名。每笔已核对规范不可逆回执、相同包重试不重复、发送人额外付费、完整本金到账、预算新增字节及RAM购买储备未被转账改变。

真实环境出现原创世RPC读表500/截断响应，原始失败及已冻结包全部保留。SDK/中继增加可选只读RPC，24项远端测试通过；读数据走现有历史节点8890，广播仍走创世8888，显式核对两端链ID与写端代码pin。默认单端点兼容，未增加自动写重试、HTTP入口、密钥或节点。只重启原中继应用；原日志保留，前两笔已完成交易复核后继续第三笔，没有重新准备或重复支付已广播任务。此配套运行时源码归档为4feeba4c2bd74469660ea0010df0c9df47737575f1b893404c0e9222417b5ee9，合约/ABI字节保持不变。

| 预算字节 | 本金 | 执行项AIC | RAM项AIC | 基础AIC | 中继AIC | 总费AIC | 回执 |
|---:|---|---:|---:|---:|---:|---:|---|
| 1536 | 0.0001 AIC | 0.0083 | 0.0037 | 0.0120 | 0.0036 | 0.0156 | [交易](https://aic-test.matrixai.one/#/transaction/33db3c92dbb38803d477e5c317827516d1af1ee30d0bda997740939e1adb52b1) |
| 512 | 0.0001 USD | 0.0083 | 0.0013 | 0.0100 | 0.0030 | 0.0130 | [交易](https://aic-test.matrixai.one/#/transaction/6863aa95f72d8807d79011e24778e4e56343409a0448ec34bb93a9c7258d4bb3) |
| 0 | 0.0001 AIC | 0.0083 | 0.0000 | 0.0100 | 0.0030 | 0.0130 | [交易](https://aic-test.matrixai.one/#/transaction/e02036832fbc852f3968a983df249201e362b4b4478666e34b64228affddc9a5) |

新收款测试公钥：PUB_K1_5vgQggb1u83oiAVWgzkojGwhty9QBXiBQyTBTHmzsDZCBZDrA8。私钥只保留在远端受限文件，不进入证据或Git。AIC/USD原生储备、合约reserve和完整负债等式逐一相等。配套SDK/中继切换到相同资格源码，原热钥、权限、HTTP入口和已完成日志均保留；历史投影只更新代码pin。

## 验证与运行边界

251项完整前端测试通过；后续CSS调整重复TypeScript/build及实际Chrome检查，功能JavaScript摘要不变。最终提案预览、升级前公网、公示中公网、升级后公网各四组；最终导航预览/公网各四组，全部通过。升级后首页四种浏览器组合共60次实际费率/存储场景检查，公式保持默认折叠；另有15组真实已部署市场版本SDK/前端整数比较。旧迭代截图及日志另行保留。

原8钱包/25历史视图/20笔交易逐条保留；新增三笔后为9钱包/31视图/23笔原生交易。实际receipt金额、费用承担方、旧新公钥格式、筛选与错误请求均验证；不声称完整终身历史覆盖。

实际主机仍是原共享Intel Cascadelake4vCPU/2个来宾核心/8125768KiB内存，物理核心独占不可见。链参数保持max_block_cpu_usage=800000、max_block_net_usage=1048576、max_transaction_cpu_usage=150000，WASM参数也完全一致。原26节点PID与unit配置、投票、保证金、系统/代币/节点字节均保持，25注册/24合格，原germanycrest禁令保留，native21成员/15票。后台普通业务与主历史服务持续运行，仅重启匹配升级所需的中继和公钥历史投影应用。未停链、重置、租服务器或运行TPS；不据功能通过宣称主机容量或延迟问题解决。

首次静态发布的Python urllib公网校验收到403，改用原有Node校验及实际Chrome，同源请求返回200且六文件摘要完全匹配；原失败保留。成熟后的首次账户只读预检查500时，流程在签名/广播升级前停止；改用历史只读RPC和有界读取重试后完成，不更改节点参数。读写分离测试的首次启动脚本有引号错误，未启动测试或改服务，随后独立脚本修正。原生cleos验证前重新解锁原签名钱包；所有失败及恢复记录保留。公示期间导航的后续CSS发布不修改提案代码/ABI/审核文档或截止时间。

## 磁盘清理与保存

先校验完整证据与最终源码/制品，再仅删除本轮已停用frontend工作树及重复styles.css，释放分配空间1.992MiB；清理后可用105.755GiB。净文件系统差异包含持续链活动，见原始结果。当前静态版本、活动SDK/中继、共享依赖、私钥、当前链/历史数据库、原26节点与先前不可变档案均保留。失败日志、所有源码版本摘要、原生提案/执行、真实交易、截图及清理清单完整保留。不要重跑已完成治理或付款控制器。

English: the read-only native proposal page and scrollbar-free navigation are deployed. The same qualified RAM candidate completed its full1800-second TEST review and irreversible execution. Three real founder-relayed AIC/USD payments, exact balances/fees/budgets/reserves, preserved history and post-upgrade browser checks passed. Production review and all protected chain/node settings remain unchanged. Private explorer source is retained only in private storage.
