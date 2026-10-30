# Ubuntu 24.04 宿主验证 / Ubuntu 24.04 host verification

日期 / Date: 2026-10-30

## 中文

三台实机宿主均报告 Ubuntu 24.04.5 LTS、x86_64、内核 `6.8.0-146-generic`、系统 glibc 2.39。节点在匹配的 Ubuntu 26.10 / glibc 2.44 用户空间运行：A 使用隔离 rootfs，B/C 使用同一运行时来源的动态 loader 和共享库。每台为 AMD EPYC 4345P、8 物理核心 / 16 逻辑线程，达到 64 GB 系统可见物理内存门槛。

本轮部署 30 个并行 BP 进程，并增加无 BP 私钥的快照 / 只读 API 观察实例。实测覆盖已包括发现和密钥撤销、网络恢复、滚动重启、维护交接、21 人委员会的 15/14 门槛，以及全体 BP 在线停摆恢复。全体停摆产生 380.5 秒的已最终确认相邻区块间隔，恢复后完整观察窗口内新增永久 ban 为 0。

完整扩展功能套件仍在进行；该记录确认宿主和上述匹配运行方式，未宣布当前候选的全套功能资格或基线提升。TPS 未测量。Ubuntu 26.10 发行包仍保持其用户空间要求；24.04 宿主测试没有使用系统 glibc 2.39 直接执行该二进制。

## English

All three physical hosts report Ubuntu 24.04.5 LTS, x86_64, kernel `6.8.0-146-generic`, and host glibc 2.39. Nodes run with matching Ubuntu 26.10 / glibc 2.44 userlands: host A uses an isolated rootfs; hosts B/C use a matching dynamic loader and shared libraries from that runtime. Each host has an AMD EPYC 4345P with 8 physical cores / 16 logical threads and meets the 64 GB system-visible physical-memory floor.

The campaign deploys 30 parallel BP processes and an additional snapshot / read-only API observer without BP private keys. Completed checks include discovery / key revocation, network recovery, rolling restarts, maintenance handoff, the 15/14 threshold of a 21-member committee, and recovery after every BP remained online while production was paused. The resulting gap between adjacent finalized blocks was 380.5 seconds, with zero new permanent bans after recovery and a complete observation window.

The expanded functional suite remains in progress. This record verifies the host and matching runtime arrangement described above; it does not promote the candidate baseline or claim complete feature qualification. TPS is unmeasured. Ubuntu 26.10 release packages retain their userland requirements. The 24.04 host test did not execute this binary directly against host glibc 2.39.

## Evidence / 证据

See the sanitized [machine-readable record](UBUNTU_24_HOST_VERIFICATION_20261030.json) for exact OS/runtime fields and candidate hashes. Host labels omit addresses, server IDs, credentials, and private keys.
