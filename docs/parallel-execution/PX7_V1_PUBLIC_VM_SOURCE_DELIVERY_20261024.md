# VM 固定源码的匿名交付

2026-10-24。节点首版尚未正式发布；本次发布的是固定依赖源码镜像。

原 `MatrixCoreX/PureEOS-VM` 保持私有且不修改。独立公开镜像为
[MatrixCoreX/AIC-VM](https://github.com/MatrixCoreX/AIC-VM)，仅推送已审查基线
`16f9c4c3ea7a172716d0bf68c60806e499f5988a` 到 `fixed-baseline` 分支，未复制其他分支或标签。
主项目 `.gitmodules` 改用该公开 URL，gitlink 提交不变，三份并行 VM 补丁及生产字节不变。

公开前检查该提交可达的10774个对象，其中1278个提交、5638个blob；实际当前 API/GitHub
凭据、SSH私钥和GitHub令牌特征均未命中。这是明确范围的检查，不声称能识别一切任意
历史字符串。原工作树HEAD和本地未提交改动保留。

禁用Git凭据助手、全局Git配置、交互提示及相关凭据环境后，独立目录从公开地址获取
该提交和两个嵌套依赖。提交和tree逐项匹配；显式应用三份VM补丁，再做只读复验，
得到与真实c91节点构建源相同的三个头文件摘要。节点整体188模块的无私有凭据检出
由随后专门源码CI验证；本次两模块成功不能代替整个节点的证明。

原VM许可实际为 **EOS VM License V1.0**，含来源声明及公开对应源码条款，不能写成
MIT，也未将其替换为AIC许可。镜像保留准确原文，正式发行继续携带完整依赖声明。
主项目的BSL及独立新链生产用途问题仍须分开核对。

推送后第一次立即读取默认分支时，GitHub元数据尚未传播，外层断言失败；提交已正确
推送。原失败保留，随后按精确新仓库ID、公开状态、默认分支、提交、tree及匿名检出
独立核验。没有因该失败改动提交内容、私有原仓库或本地Git引用。

临时私有CI只读部署钥和CI secret已提前撤销；最新原生CI已完成私有检出阶段，
后续匿名交付CI不再依赖该钥。本次镜像关闭Actions，不触发继承的历史工作流。

证据：`public-VM-source-delivery-attempt-2/review.json`、`publication.json`、
`anonymous-proof.json`；`node-private-ci-attempt-1/credential-lifecycle.json`。
