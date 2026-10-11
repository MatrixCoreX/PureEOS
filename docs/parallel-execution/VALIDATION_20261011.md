# PX0 工具与分支回退验证

日期：2026-10-11。功能提交：`c00779e9dd81c5eb0a7cf5fb2e27a3b7716d3966`。

## 本次结果

| 检查 | 实际结果 |
|---|---|
| 原生源码基线 | `main` 与标签 `baseline/pre-parallel-20261011` 均指向 `a4a40a6a1e72c531cd8267758074229ccc0655ce` |
| 合约源码基线 | `main` 与同名标签均指向 `c454378bf2efc4e15fa2c55169ba4749795f3534`，工作区干净 |
| 工具单元测试 | 最终 16 个不同用例全部通过 |
| 实际源码清单检查 | 273 个源码文件指纹、扫描器指纹与快照匹配；新增/修改/删除清单为空 |
| CMake 配置 | 现有 Release 构建目录重新配置成功，退出 0；没有执行 C++ 编译或合约编译 |
| CTest 注册与重复 | `pureeos_parallel_access_audit_unit`、`pureeos_parallel_access_audit_snapshot` 两项各连续运行 3 遍，全部通过 |
| 计划快照 | Git 内两份计划与工作区原件字节完全一致 |
| 文档检查 | 本批 Markdown 链接有效，无行尾空白；`git diff --check` 通过 |
| 实际回退演练 | 从开发分支切至 `main`，确认 HEAD 等于基线、跟踪文件无差异、开发工具/文档消失且原配置保留，再切回功能提交成功 |
| 原生运行逻辑 | 本批相对基线没有修改 `libraries/`、`plugins/`、`programs/` 中的执行代码 |
| nodeos 产物 | 仍为 SHA-256 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`，未部署新程序 |

重复测试不累计为更多不同单元用例。两项 CTest 其中一项运行 16 个 Python 用例，另一项核对真实源码清单。

## 命令

```bash
python3 -B tests/parallel_access_audit_test.py -v
python3 -B scripts/parallel_access_audit.py --check docs/parallel-execution/access-inventory.json
cmake -S /home/guagua/AntelopeIO/PureEOS -B /dev/shm/pureeos-release-ESOoZv
ctest --test-dir /dev/shm/pureeos-release-ESOoZv -L parallel-px0 --repeat until-fail:3 --output-on-failure
```

上面的 `/dev/shm` 是本次已有构建目录，不是未来回退的持久化依赖。测试本身只读取生产代码并在系统临时目录创建/清理测试夹具，不需要启动 nodeos、创建 socket 或改已有链数据。

16 项用例覆盖注释/字符串/原始字符串过滤、行号、续行、数据库别名候选、范围与 session、注册宏定义排除、未知 intrinsic 保持待审查、数字分隔符、源码增删改、零命中文件变更、扫描器/范围变更、配置与构建输出排除、外部符号链接拒绝、缺失源文件、错误/重复快照、CLI 返回码、只读检查及合法 `build_block.cpp` 不被误排除。

## 边界

没有运行本项目全量 C++/WASM/真实链回归或 TPS 测量。没有实现、验证或启用并行写交易执行；PX0 完整人工审查和区块语料仍待推进，PX1–PX6 尚未完成。

本次 CMake 只更新测试注册与构建元数据，没有覆盖此前归档。没有更改系统合约、手动链、钱包、磁盘分区或温度/功耗策略；未推送远端，未合并开发分支到 `main`。
