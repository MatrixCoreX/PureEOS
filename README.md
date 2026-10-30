# PureEOS System Contracts

[简体中文](#简体中文) · [English](#english)

## 简体中文

当前新链配置保持单选直接投票、原生 Savanna 出块/确认与轮岗、签名 BP 发现、计划维护、全链停摆缺席保护、10% 质押 / 90% PowerUp 和 14 天升级治理。RAM 初始 64 GiB，初始化时调用 `setramrate(1024)`；延迟交易两个禁用阶段保持激活。

出块奖励按当前已发行量，以每 364 天 **1%** 的系数计提，每 1461 天减半。金额使用整数最小代币单位计算，跨减半段合并余数后，在填池区间末尾向下取整。新发行全部用于出块奖励，不发行额外 4%，不新增投票或候补奖励。

首次正权重直接投票激活计时。实际出块累计 `unpaid_blocks`；成功 `claimrewards` 时才实际发行和填池，再按未领奖出块比例支付。首次领取距注册、后续领取距上次成功领取均须超过 24 小时。失败领取原子回滚，不清除出块份额及计时。

当前浏览器测试链初始发行 `100000000.0000 AIC`，上限 `2100000000.0000 AIC`。上限限制发行总量，不保证奖励公式最终发行到上限。这里只定义奖励规则，代币符号、初始发行与上限通过正常初始化设置。

### 编译

安装 PureEOS CDT 1.0.0 SDK，并填入其实际绝对路径。Ubuntu 24.04 LTS / 26.04 LTS 的节点和 SDK 使用范围见 PureEOS 兼容性报告。

```bash
export PUREEOS_CDT_ROOT=/absolute/path/to/pureeos-cdt-1.0.0
export PATH="$PUREEOS_CDT_ROOT/bin:$PATH"
cmake -S . -B build/current -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PUREEOS_CDT_ROOT" \
  -DPUREEOS_BP_PEERING=ON -DPUREEOS_BP_MAINTENANCE=ON \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON -DPUREEOS_SINGLE_CHOICE_VOTING=ON \
  -DPUREEOS_RESOURCE_SPLIT=ON -DPUREEOS_SEQUENTIAL_ROTATION=ON
cmake --build build/current --target contracts_project --parallel 2
sha256sum build/current/contracts/eosio.system/eosio.system.wasm
python3 tests/run_reward_accrual_exact.py --output /path/to/new-math-results --sanitize
```

当前包面向新创世链。已有链的奖励政策状态迁移和实际升级需单独验证，并遵守链上治理；本包不执行自动升级。产物及测试范围以配套清单和报告为准。许可见 `AIC_LICENSE.md`、`LICENSING.md`、`LICENSE` 和各组件声明。

## English

The current new-chain profile retains direct single-choice voting, native Savanna proposer/finalizer rotation, signed BP discovery, planned maintenance, global-interruption absence protection, 10% staking / 90% PowerUp and the 14-day upgrade review. RAM starts at 64 GiB; explicitly initialize growth with `setramrate(1024)`. Both deferred-disable stages remain active.

Block rewards accrue at **1% of currently issued supply per 364-day contract year**, halved every 1461 days. Integer token-unit arithmetic combines remainders across halving segments and floors once at the end of each funding interval. All new reward issuance funds block pay. The additional 4% is never issued; voting and idle standby status create no new reward entitlement.

A positive-weight direct vote starts the reward clock. Produced blocks accumulate unpaid credits. A successful `claimrewards` issues/funds the pool and pays the claimant's proportional unpaid-block share. Claims require more than 24 hours since registration or the last successful claim. Failed claims roll back without consuming credits or accrual clocks.

The explorer devnet initializes `100000000.0000 AIC` with a `2100000000.0000 AIC` maximum. The maximum is a ceiling, not a guaranteed issuance target. Symbols and supply parameters are initialized through ordinary actions.

### Build

Install PureEOS CDT 1.0.0 and run the complete commands above with your SDK's absolute path. The root CMake build forwards all selected modes and produces the system contract under `build/current/contracts/eosio.system/`. The independent arithmetic test uses Python Fraction and can run with UBSan through the final command.

This package targets a fresh genesis. Existing-chain reward-policy migration and deployment need separate qualification under on-chain governance; there is no automatic upgrade. Consult the matching manifests and reports for exact artifacts and scope. See `AIC_LICENSE.md`, `LICENSING.md`, `LICENSE` and component notices for licenses.
