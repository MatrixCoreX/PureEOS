# 真实进程测试发现的 HTTP 区块缓存缺陷

这不是并行算法引入的问题：保留的旧 nodeos（SHA-256 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`）也能复现。修复只进入实验分支，不改 main 或旧二进制。

## 现象与原因

固定业务语料通过 `/v1/chain/push_block` 输入后，旧程序逐块执行所得区块 ID、LIB 和完整性摘要全部正确，正常退出也是 0。但是重新启动失败，报 `block_log.cpp` 的 `Read outside of file`。

`signed_block` 的 JSON 反射只填公开字段，不填私有 `packed_block` 缓存。原 HTTP 入口直接将该对象放入 shared_ptr；而区块日志、恢复记录和网络发送读取的是 `packed_signed_block()`。Release 模式的 assert 被关闭，所以空缓存可能写入持久化路径，并非仅仅少一个性能缓存。

同一高度的错误 `blocks.log` 只有 1,585 字节，正确串行导出的日志为 12,383 字节。失败现场保留在：

`build/local-tests/parallel-overlay-20261011-q42k0b/parallel-process-ivbiy58p/old-serial/`

## 修复边界

`plugins/chain_plugin/chain_plugin.cpp` 的 `read_write::push_block` 使用既有 `signed_block::create_signed_block` 工厂，在发布为只读共享块之前完成打包。交易、签名、区块 ID、收据及文件格式没有改变；不修改持久化先后或 finalizer 规则。

同时检查了另外三个生产读取路径：P2P、区块日志和恢复记录均使用已有 `fc::raw::unpack` 特化，该特化会填缓存；原生生产者也已经使用该工厂。没有把测试用的合成对象误当成生产入口批量修改。

## 验证

- 新增 JSON 往返、逐块序列化字节一致、并行采用和关闭重开的原生用例。当前验证套件 6 用例 / 780 断言，独立重复三次通过。
- 修复后的新 nodeos，0/1/2/4 worker 的真实 HTTP 输入、正常重启、SIGKILL、脏状态拒绝及检查式重放已经通过首轮。
- 旧 nodeos 保持不变，通过既有 P2P 解码路径读取相同串行签名区块，继续作为独立状态/恢复参考。没有通过改写旧二进制或跳过它的状态比较来掩盖缺陷。
- `/v1/chain/push_block` 原有的“内部失败可能仍返回 HTTP 成功”语义本次没有顺带改变；测试绝不以 HTTP 状态码当作区块被接受，而是核对实际 head 和状态。

进程脚本另外修正了两个测试配置/断言问题：单机多观察者需要在**新建测试配置**提高每 IP 连接数；恢复损坏的诊断文本包含 `Truncated or oversized`。这两项不改变链实现或放松损坏拒绝要求。相关失败日志也保留，不计为通过。
