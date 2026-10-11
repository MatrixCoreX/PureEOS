# 指定高度暂停后恢复调度修复

PX5/PX6 真实持续运行测试发现的既有问题，修复已通过重新编译后的串行/并行进程验收。未修改共识、交易格式、链上限额或旧基线二进制。

## 可复现原因

`producer_plugin_impl::start_block()` 先 abort 当前 pending block，然后在 `head == pause_at_block_num` 时提前返回；此时可能保留前一块的 `producing` 模式，但已没有 pending block。原 `resume()` 只在 `speculating` 模式重新调度；这使“已暂停、旧模式仍为 producing”的节点恢复后缺少唤醒源，收到的新交易留在 incoming 队列，HTTP 响应持续等待。

测试先完成一个六块窗口，再按序提交下一批、恢复出块。以下失败证据都保留在 `build/local-tests/parallel-overlay-20261011-q42k0b/`：

- `parallel-producer-soak-d2s0_hqm`：首次脚本同步等待暂停节点 RPC，属于工具调用顺序问题，不能单凭此结果认定节点缺陷。
- `parallel-producer-soak-rhwyglzi`：改为异步请求，并通过原 `get_unapplied_transactions` 确认每笔确实进入队列后再恢复，第二轮仍无区块、请求超时。
- `parallel-producer-soak-fjlvl3eo`：对**保留的旧二进制**重复相同流程，第二轮同样停在 `Producer resumed.` 而没有重新调度。这证明问题早于本次并行生产改造。

旧二进制 SHA-256：`e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。修复前新二进制：`ec4d12d38a21b87e8ce7ec3c944c47819e12e146707e1368353c13ef6da84737`，已单独归档在 `px5-pre-resume-ec4d-artifacts.tar.gz`，归档 SHA-256：`5aeaae8f07aed8ab5cff5a162648ea5a7a003ff66f7eaafbe9785698e37e9930`。

## 最小修复

`plugins/producer_plugin/producer_plugin.cpp` 的 `resume()`：除原 speculating 分支之外，**没有正在构建的区块**也执行原 abort/schedule 流程。已在 producing 且有正常 pending block 的路径保持不变；实际新建区块仍经过原生产资格、暂停、安全和出块截止检查。

持续运行工具保留旧程序诊断开关 `--old-producer`，不修改旧程序去让它通过。新程序 `d6cac377…` 已通过：

- 首次两轮/32 笔：`parallel-producer-soak-g3fpzyp_`。
- 并行 4 worker：`parallel-producer-soak-wfyvyyqu`，180 轮/2,880 笔，真实场景 762.18 秒，连同准备/关闭共 768.28 秒；head 1085 / LIB 1083，活动窗口无漏槽，旧新完整验证者收据/状态/LIB 与 SHiP 全文件相同。
- 关闭并行：`parallel-producer-soak-cxkc_7jd`、`parallel-producer-soak-pjfrwrj0`、`parallel-producer-soak-i4a2ijsl`，三个独立进程实验各三轮/48 笔通过，无并行生产批次。

这是本地有界持续验证，不是数月稳定性或物理断电证明。180 轮使用的脚本另存 `px6-soak-180-harness.tar.gz`，SHA-256 `b6dd506a375c780736be8092ae4c097f21a75fe61605a1e71e3465a3603d8a71`；随后脚本仅增加 `--workers 0` 串行修复验证模式。
