# 长期分支入口 / Long-term branches

## 简体中文

PureEOS 的两个长期入口固定为：

| 分支 | 用途 | 内容 |
| --- | --- | --- |
| [`main`](https://github.com/MatrixCoreX/PureEOS/tree/main) | 正式主线、GitHub 默认入口 | 正式发行基线；保持正式构建及治理参数。 |
| [`testnet`](https://github.com/MatrixCoreX/PureEOS/tree/testnet) | 当前完整测试网主线 | 低配 TEST 配置、受编译开关隔离的测试治理、已验证的测试功能、运维源码与记录。 |

正式主线保留发行版的七天升级审查规则。测试网的三十分钟审查只属于独立 TEST 构建及其显式测试开关。主版与测试版的参数、构建和部署记录各按所在分支核验。

日常正式版修复以 `main` 为基线；测试网更新以 `testnet` 为基线。通用修复按提交审查、验证后在两条主线之间同步。测试专用参数、缩短的审查周期、运行脚本和一次性实验记录留在 `testnet`，不以整条测试线反向合并来替代正式版发布审查。

`feature/*`、`fix/*`、`docs/*`、`ops/*` 和 `wip/*` 可以承载临时工作或历史成果；`testing/*` 中原低配入口属于历史基线。`release/*`、发布标签与 `backup/*` 保留其发布或存档含义。它们不再作为“最新完整测试网”的长期入口，也不因确立本约定而删除、改写或重放其中已完成的控制器。

2026-11-19 建立 `testnet` 时沿用完整测试线 `b91f59ac2586eda2b2049e888f5e4f00cf795037`；正式主线沿用 `a0bacba27e59bd215fd94c978f9b217ec9a09ee3`。本次入口整理只添加文档，不发布新二进制，也不触发服务器、链上资金或手机操作。历史运行结果以各自日期、Chain ID、代码摘要和回执为准。

## English

The permanent entry points are [`main`](https://github.com/MatrixCoreX/PureEOS/tree/main) for the production release line and [`testnet`](https://github.com/MatrixCoreX/PureEOS/tree/testnet) for the complete current TEST line. GitHub's default remains `main`.

Production retains its release governance rules. The thirty-minute review belongs only to the separate TEST build and its explicit compile-time option. Review and qualify shared fixes before carrying them between the two lines; keep TEST-only parameters, operational tools and experiment records on `testnet`.

Topic, operations, release and backup refs retain their existing history and purpose. Older dated branches are not the canonical latest TEST entry. Completed one-shot controllers must not be replayed. Establishing these entries changes documentation and Git refs only, not binaries, running services, chain state or phones.
