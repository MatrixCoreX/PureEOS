# CDT运行库参数读取诊断

2026-10-24。已证明旧SDK封装路径出错，重编试验恢复正确值；正式修复/回归未完成。

只读实际链查询：globalv2.max_block_cpu_usage=1601009006、min_transaction_cpu_usage=
1601795177，节点实际值分别200000与100；NET镜像也不一致。节点仍按正确的
原生共识参数执行；不能用合约镜像推算CPU/TPS上限。

在原生控制器加载最新d58 SDK编译的诊断WASM，直接host调用回传68字节，解码
CPU200000/NET1048576；SDKget_blockchain_parameters返回的结构保持0。派生结构
和复制同样保持0。诊断通过带数值的显式断言停止，测试suite本身故意失败，
不是将其记为功能通过。首个仅编译失败的探针也保留。

使用相同SDK前端/后端，重编当前源码eosiolib.cpp，替换libeosio.a中唯一同名
eosiolib.cpp.obj，再编同一探针。SDK/直接/派生/序列化解码/返回复制均正确。
旧库摘要9a76c3952072032e5aef0b20b97b22312aec7e80b9c91092aefc91ecef21dfd3，
试验库481cbf3cebbeddefb00d725d68df287a0fc6cd3d12a4f0e565864a99466e6774。
这证明已带入的静态运行库与当前正确序列化路径不符；具体旧库生成过程还需
核对，不能仅凭现象说成节点v0字段ABI改变或整个LLVM后端错误。

接下来：按当前源码和配套头文件正式重建WASM运行库，加入真实get/set和全部
字段往返检查、失败回滚控制；以新的SDK身份重编23份合约、比较ABI并复测
治理/投票/资源/奖励/轮岗和实际部署。运行库改动与Node实际限额分开；不缩短
14天/四年常量，不修改真实24h任务。候选TPS可以先测，但必须记录其SDK和
WASM准确身份，不能称为已修复发布版。

证据：cloud-real-day-20261023-attempt-2/readonly-global-vs-native-1104.json；
global-parameter-probe-attempt-1/attempt-2/report.json；
global-parameter-probe-attempt-1/attempt-3-runtime-rebuild/report.json。
