# 首版构建标识：导出源码不能丢失版本与来源

2026-10-23 01:27UTC。元数据专项通过；候选6正在准备，不是发布完成。

01:44UTC最新：候选7的11程序构建于01:41:18UTC完成，真实三工具来源步骤
exit0，准确输出`v2.0.0-src-beb91fb95dfc0217491c5016edc5f3da49fde4f0001407354bbee87ea2e0aa55`。
新增hash/显示标签分离已经实际nodeos、keosd、cleos启动验证，不仅是模拟。
新链接资源15项/178断言各三轮通过，14项业务预检正在最后的21人故障确认
用例；完整59步尚未完成。后续attempt-8仍waiting-for-core，无提前起链或发行。

01:40UTC当前：候选6的真实nodeos在前置检查中返回Invalid hex character 's'，
configure/build成功、build-version-provenance exit1，整体failed并关闭归档。
归档82,556,506字节，SHA256
`ee2dfd753727dc24e518f282ba5606ab1ad05ad587e3b91a5ac0485dfd5d4843`。
448条构建采样无错误；该候选没有进入业务或实链验收。候选7已核验父轮
75,911源文件/11程序并独立准备完成，实际仅三个版本文件不同，配置通过、
正在重编。metrics7于01:37:20UTC从构建开始采样，绑定唯一容器ID。
最新`candidate-controls-full-attempt-20`为358项固定三轮全部通过、输入未变。
后续attempt-8仍waiting-for-core；不以专项或失败归档完整放行。

01:33UTC补充：调用方审查发现初版修复将src-前缀放入version_hash，违反
nodeos::main按hex解码数字版本的既有接口。这是本次元数据修复新引入的问题，
不归咎于原链执行逻辑。新增真实版本库回归已复现前缀错误；现把显示标签与
原始hash分开：version_hash只返回原始十六进制（未标识则空），version_full
独立显示src-或unidentified-source。实际版本库7项通过，358项控制三轮通过。
候选6仍在构建，保留其真实前置检查结果；当前有效候选7冻结20overlay/5runner，
只接受候选6明确版本前置失败的关闭缓存，不拿失败当通过。新后续attempt-8
两条任务waiting-for-core，尚未起链。metrics6仍采样当前构建；metrics7未启动。
01:27UTC以下较早“当前候选6/后续7”是当时状态，按本段新绑定继续。

## 实际发现

候选5的相同ELF在原生宿主执行版本命令时，nodeos、cleos均返回`unknown`。
根因是`libraries/version/CMakeLists.txt`仅在存在`.git`时填写项目版本；完整
导出源码有准确内容摘要，但没有Git目录，原fallback将版本清空。这不是
交易/共识失败，但不应直接作为可追溯的正式发行包。

候选5停止前25/58步全部exit0，14业务预检、双引擎专项各三轮、资源三轮、
只读输入三轮和十轮重叠均通过；完整native首轮中途被明确替代，不能说成
完整通过，也不冒称该轮业务断言失败。关闭归档为82,621,378字节，SHA256：
`f9f8088289c84dc757770fa90e11af66afe82370bcb6e9e8dba99026899f07fb`。
`superseded-while-running.json`保留替代理由；原report/monitor未被改判。
metrics5关闭，1410条逐秒样本、无采样错误。原后续attempt-6尚未起链即替代。

## 最小修正

- 沿用项目已有2.0.0主版本，移除历史dev1.3后缀。版本字符串不代表已经发布，
  当前仍无稳定标签或Release。
- 导出源码使用实际项目版本；显式`PUREEOS_SOURCE_ID`只接受64位小写SHA256，
  全版本显示`v2.0.0-src-<manifest SHA256>`，不伪装成Git提交。
- 未提供身份的导出源码明确标记`unidentified-source`，不能拿它作为本轮发行
  验收程序。Git检出保留原真实提交/dirty行为，并拒绝同时冒称导出清单身份。
- 不修改链ID、协议特征、共识、签名、经济/治理规则或CPU/NET执行预算。

实际版本库编译专项6项通过，包含真实Git提交/dirty、无Git实际库编译、错误
摘要拒绝和身份冲突拒绝。首轮一个负例仅因CMake错误消息换行而文本断言失败，
原拒绝正确；归一空白后复测，不修改生产拒绝条件。
全套`candidate-controls-full-attempt-18`为357项固定三轮通过、记录输入未变，
禁止导入私有cloud_round。专项中的a*64为接口探针，不冒称实际候选源码摘要。

## 当前候选与门槛

`local-native-profile-retest-attempt-6`独立冻结19份overlay、5份runner，从已关闭
候选4复制独立缓存；不用正在运行的构建缓存，不复用任何父轮通过结论。
完整源码逐项核验后，实际源码清单SHA256传入CMake；重新链接11个程序。
新增`build-version-provenance`步骤实际调用nodeos/keosd的full-version和cleos
version full，三个输出必须准确匹配同一份真实源码清单。完整矩阵为59步。

`local-release-workflows-attempt-7`及`live-reward-claims-attempt-7`仅接受候选6：
59步全0、11程序摘要、版本来源证明与清单一致、关闭归档通过且容器停止。
当前两者仍waiting-for-core，未起链。旧候选局部通过或archive完整不构成放行。
metrics6已绑定新容器名，等待启动后按唯一ID采样，不追随替换容器。

真实节点/恢复/40独立进程、实际86400秒领取、跨主机故障、适当环境TPS、
最终Git源码/配套SDK及联合发行仍须完成；不扩展到新协议或无关功能。
