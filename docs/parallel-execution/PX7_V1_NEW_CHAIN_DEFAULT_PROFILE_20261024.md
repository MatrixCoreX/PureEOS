# 新链默认合约配置收尾

2026-10-24。原构建曾默认将四项PureEOS功能关闭；测试入口显式ON，所以已测WASM
正确，但普通开发者直接CMake构建会生成不符合本轮新链要求的配置。

现将reference-contracts-v1-hardening的根CMakeLists.txt及contracts/CMakeLists.txt
统一为四项默认ON且拒绝OFF：原生开发者升级、一票一投/取消代理、10/90资源权重、
顺序单席轮岗。根入口补传SYSTEM_ENABLE_CDT_VERSION_CHECK，使内层也执行版本检查。
旧配置缓存不会被静默当作新链发布物。README改为本项目的真实规则和AIC CDT入口。

使用同一个已核验AIC CDT1.0.0 SDK（d58f3968…）分别从根目录和contracts目录
全新配置并完整重建。两次各23份业务WASM/ABI都逐字节匹配此前云端最新SDK
重建和40节点实际使用的产物。没有修改生产C++、合约C++或正式常量。
两个入口的四种OFF配置，共8项全部被拒绝。输入摘要运行前后一致。

首轮已重建根入口23项相同，但检查拒绝信息时未处理CMake自动换行，报告失败；
原日志和报告保留。修复仅规范化诊断中的空白后，在第二个全新目录重新完整验证
两个入口，全部通过。没有修改旧结果或业务逻辑。

证据：new-chain-default-contract-build-attempt-1（原失败）；
new-chain-default-contract-build-attempt-2/report.json（两个重建、8反例）。
这是新链构建入口修复，不表示其余尚未完成的原生/插桩/真实24h/许可和联合发布
门槛已通过。TPS按用户最新指示在首版发布后以单台节点测试。
