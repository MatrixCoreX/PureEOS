# 已验证的 group8 基线

代码来源：3a3f9361bdfb49a3be355dff2f5b3fb249bfd191。本提交保存该完整代码树，仅新增本目录的运行配置和来源说明，不修改已测试源码。

使用所附 nodeos-options.ini 显式启用 group8、Native、输入指纹和 trace。开发链参数保持 CPU16（3,200,000微秒／块）、NET16 MiB／块、P2P64 MiB／消息和1 GiB队列，逐笔限制与计费保持。CPU32压力测试不改变开发默认。

系统合约保留现有投票、生产／终局／轮换、10%／90%资源、奖励减半和治理。RAM初始64 GiB，以现有 setramrate(1024) 初始化增长；需核对部署WASM和实时参数。

已完成单节点纯转账、复杂合约、安全及独立全量串行核验。多机器测试另行进行，本提交不声称多节点或发布资格已完成。原始运行证据保留服务器，本机不包含私钥、运行状态档案和二进制。

2026-10-29 开发默认更新为 CPU16／NET16MiB。development-chain-parameters.json 为完整链参数预设，用于新链 genesis 的 initial_configuration 或按现有治理权限调用 setparams；现有链不会因文件变化自动修改。上游内置通用 genesis 常量及已测试二进制未改动，逐笔CPU150000µs、NET524288字节、最低CPU100µs保持。历史 benchmark_scope 仍为 CPU32／NET32MiB；4–5万TPS是目标，并非本配置已经通过持续或多节点资格核验。
