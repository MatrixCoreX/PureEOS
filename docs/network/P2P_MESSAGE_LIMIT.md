# P2P message limit / P2P 消息大小

## 结论

当前开发预设为 **NET 4 MiB/块、P2P 64 MiB/消息**，普通转账双轮短窗已完成，参见[实测记录](NET4_SELF_TPS_20261031.md)；大数据边界验证尚未完成。保持接收上限，不因 NET 调整而扩大消息或队列预算。4 万 TPS、144 字节计费 NET 的普通转账约消耗 2.75 MiB/块；4 MiB 在字节预算上足够，实际吞吐仍须测量。较大交易会更早触及 NET 上限。

不能只按计费 NET 的两倍推导消息上限。链参数中 `context_free_discount_net_usage_num/den = 20/100`；交易计费近似为：

```text
计费 NET = 基础费用 + 不可裁剪部分 + ceil(可裁剪部分 × 20%)
可裁剪部分 = 序列化签名 + packed_context_free_data
```

P2P 仍传输完整的这些字节。因此，仅从计费上限推导，折扣数据占主导时可能接近 `4 / 0.2 = 20 MiB`，另有区块和消息封装。它低于当前 64 MiB 接收上限，但这只是源码边界推导，尚未构造并验证一个满足全部检查、接近该边界的实链区块，不能据此宣称所有消息形态已通过验证。

普通转账消息通常小得多，但它们不能代表全部支持的数据形态。zlib 路径的 1 MiB 解压保护也不能直接作为未压缩数据的共同上限。

## 实测范围

保留的 NET16MiB 三机功能链证据包含 30,127 个规范区块，不能作为 NET4MiB 新验证结果。最大序列化区块为 **524,730 字节**，加当前区块消息类型标记后为 **524,731 字节，约 0.5004 MiB**，占 64 MiB 上限的 **0.7819%**。该最大区块（高度 26271）的原始 SHA256：`32838511dfd24cfda40ac696f912042b8cadc0e07fb98df6404590abea192218`；完整区块日志 SHA256：`65bc908951c718e1d5a2b880ab44a52307d820f93d114c7074230ca0f7fa95eb`。

这是从规范区块重建的区块消息大小，未进行全部 P2P 消息抓包，也不是 16 MiB 满额区块的资格验证。它只能说明本轮工作负载实际用了多少空间。

## 发送端约束与采用条件

当前发送队列有约 **40 MiB** 的检查阈值和 **80 MiB** 的入队硬阈值。它们是累计队列字节，与单消息接收上限不同。单纯将接收上限改为 128 MiB，仍可能在合法大消息发送时触发队列断连。

新预设至少须验证：

- 普通转账、未压缩 context-free data、签名数据、压缩数据及区块封装的实际字节与计费关系。
- 新 NET 上限下普通和大交易的吞吐、传播、验证、最终确认及重启同步。
- 64 MiB 接收边界、超限/零长度/截断消息拒绝，解压保护继续生效。
- 单个合法大消息可以入队，慢节点通过有界背压处理；多连接和并发收发的内存占用受控。
- Ubuntu 24/26 互联以及现有并行执行、授权、计费和共识检查保持。

接收上限不是预先为每条连接分配的内存，也不是每秒带宽配额；较大上限仍会扩大处理恶意或慢连接时的资源边界。

源码依据：[NET 计费](../../libraries/chain/transaction_context.cpp)、[可裁剪大小和解压](../../libraries/chain/transaction.cpp)、[消息封装](../../plugins/net_plugin/include/eosio/net_plugin/buffer_factory.hpp)、[接收和队列阈值](../../plugins/net_plugin/net_plugin.cpp)、[当前链参数](../../config/chain-parameters.json)。

## English

The current development preset is **4 MiB/block billed NET and 64 MiB/message P2P**, with two standard-transfer windows completed (see [measurements](NET4_SELF_TPS_20261031.md)); large-data boundary qualification remains incomplete. Keep message and queue budgets unchanged. At 144 billed bytes per transfer, 40,000 TPS consumes about 2.75 MiB/block; this fits the byte budget, while actual throughput still requires measurement. Larger transactions reach the NET ceiling sooner.

Prunable data, including serialized signatures and packed context-free data, is billed at 20% while P2P carries its full size. A discount-heavy 4 MiB billed block can theoretically approach 20 MiB before block/message overhead. This is a source-based inference, not a reproduced boundary block or qualification of every message shape. The compressed-input decompression guard does not establish the same bound for uncompressed input.

The retained NET16MiB functional-chain log contains 30,127 canonical blocks. Its largest reconstructed block-message payload is **524,731 bytes / 0.5004 MiB / 0.7819% of 64 MiB**. This historical reconstruction is not new NET4MiB evidence, a capture of every message, or full-NET boundary qualification.

The sender also has approximately 40/80 MiB queue thresholds. Raising only the receiver limit does not make large-message propagation reliable. Qualification must cover legal large blocks, billing versus actual wire bytes, receive boundaries and malformed input, sender admission, slow peers, bounded concurrent memory, cross-Ubuntu networking, validation, finality and restart synchronization.
