# 停用节点日志删除与磁盘清理（2026-11-11）

结果：PASS_STOPPED_NODE_LOGS_DELETED_AND_DISK_CLEANED，时间 2026-11-11T02:54:59.777909+00:00。用户最新指令“已停用旧节点 日志就全部删除”覆盖了本轮最初的无损归档安排。压缩任务仅完成40份后已停止；没有继续消耗资源压缩剩余日志，所有选中的原始日志、已有/新压缩副本和残留partial文件随后直接删除。

共删除113项停用旧节点运行日志及其副本，本阶段实际释放文件分配块 15491039232 字节；另删除498个可重新下载/生成的APT安装包与缓存文件，共 256724992 字节。较本轮归档开始时，可用空间由 24.41GiB 增至 42.55GiB，整机净增 18.14GiB。包含用户改指令前已发生的40份归档节省；没有重复把原始日志和压缩副本当成两份原始释放量。并发区块和系统日志写入会影响整机差值。

删除范围由确切路径清单限定：旧链节点目录的nodeos/stdout/stderr运行日志、当前已退役身份的运行日志归档、维护停机montrealmila的运行日志（若存在），以及APT包缓存。所有目标在删除前核对文件类型、设备/inode、大小、mtime、链接数和占用引用。旧链blocks.log、状态库、SHIP链历史、创世与旧处罚数据，配置/密钥/钱包/安全文件、源代码、签名包、APK、Android SDK/系统镜像和原AVD/Keystore没有被清理。当前仍在线身份的15份历史日志归档和共享系统日志保留原轮转政策。审计JSON、源码与故障结论保留；被明确授权删除的停用节点日志不再保留压缩副本。

删除后9861个受保护非日志文件元数据逐项一致；原生ELF SHA256仍为946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397，25台BP和历史节点PID不变，没有停止这些服务、签名、转账或修改链参数。montrealmila此前已经停机且禁用自启，本次没有重复停机，也不声称新释放了它的内存。

最后读取95个已停用原生服务的实际unit/drop-in日志配置，没有发现非空残留日志文件。当前链头 #689910、不可逆块 #689907，原生21/15；维护名单仍只有montrealmila。最初仅使用systemctl show的输出模式属性无法取得文件路径，随后改为解析实际unit/drop-in配置完成覆盖核验；该核验修正未修改服务。

这些脚本属于已完成的一次性测试网操作，不得盲目重跑。prepare_cleanup.py/apply_cleanup.py是已按用户指令取消的初始归档阶段；prepare_delete_r2.py/delete_logs_r2.py是最终删除阶段。verify_cleanup_r2.py是实际unit文件路径核验。正式依据见evidence/DELETE_R2_RESULT.json、evidence/FINAL_LIVE_R2.json及PUBLIC_EVIDENCE_MANIFEST.json。原始运行日志已按要求不可逆删除，后续只保留审计记录。

The latest explicit instruction authorizes deleting all stopped old-node runtime logs, superseding this cleanup's initial archival plan. The archive worker was stopped after40files;113selected raw/archive/partial runtime-log files and498re-downloadable APT cache files were then removed. Free disk rose from24.41GiB to42.55GiB, a net18.14GiB increase across the whole operation. All9861protected non-log metadata records and the native ELF hash remained exact;25BP and history PIDs remained. Ninety-five disabled native unit configurations were checked for remaining nonempty runtime logs. Blockchain/state/SHIP history, existing keys/configs/wallets/safety, code, signed packages, APKs and protected Android/AVD data remain. Fifteen archives belonging to currently active identities and the shared system journal retain their existing policies. No financial action or active-node stop occurred. Maintenance montrealmila was already stopped/disabled before this request and remains excluded from recycling. These completed/cancelled dated one-shot controllers must not be replayed.
