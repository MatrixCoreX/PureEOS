# Effective-maintenance BP shutdown check

用户要求维护中的BP停止运行以节省资源。本次链上仅 montrealmila 处于已生效维护（state=2），已不在当前/待生效/提议的出块或确认职责中。检查前该服务已是 inactive、PID0、开机自启 disabled；没有重复执行停机操作，也没有声称本次新释放内存。

实际原生进程只有25个正常BP和1个历史节点，维护身份也不在自动内存保护的25节点名单里，不会被该保护任务拉起。配置、密钥、数据和finalizer安全记录保留；没有改变维护、投票、保证金或权限，没有签名或付款。最新用户指令覆盖原先维护演示中“保持进程在线”的安排。

The only effective-maintenance identity is montrealmila. Its service was already stopped and disabled before this request. The check confirms no hidden extra native process and exclusion from automatic recycling, without service changes, data deletion, new resource-savings claims or chain/financial actions. RESULT.json records this timestamped read-only verification.
