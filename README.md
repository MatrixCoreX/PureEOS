# PureEOS

PureEOS 是独立维护的 C++ 新链项目，使用原生 Savanna 最终确认，支持不同交易的
并行合约执行和按区块顺序提交状态。配套工具链为 **AIC CDT 1.0.0**。

本标签为 **PureEOS 2.0.0，并行首版**，与 **AIC CDT 1.0.0** 配套发行。
[联合发行页](https://github.com/MatrixCoreX/PureEOS/releases/tag/v2.0.0)提供运行包、
CDT SDK及源码、匹配的系统合约源码、验收摘要和 SHA256SUMS。
当前验收范围见[验收状态](docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt)和
[发布清单](docs/parallel-execution/PX7_V1_RELEASE_CHECKLIST.txt)；历史候选包保留原身份。

## 并行执行

- 首版按4–16个软件worker配置；实际CPU核心数、交易复杂度及冲突会影响收益。
- 同一交易内部保持顺序和原子性，正式数据库按区块中的交易顺序提交。
- 提交前重新检查状态依赖；冲突、资源不足或不支持的能力走安全串行路径。
- 使用明确的JIT运行配置和经过验证的合约代码摘要，注册多个worker本身不是TPS证据。

首版发布后再以单台节点做串行及4/8/12/16 worker的TPS压力对比，覆盖独立转账、
独立合约、复杂合约和混合冲突交易。尚未实测的提升倍数不作承诺。

## 运行环境

现代构建目标为 **Ubuntu 26.04 LTS / Ubuntu 26.10，x86_64，JIT**。

| 环境 | 已验证范围 |
|---|---|
| Ubuntu 26.04 | 当前源码四工具 Release 构建和四个原生组件检查通过。 |
| Ubuntu 26.10 | 当前源码四工具构建、全部启用的原生功能、显式双引擎并行矩阵、SDK/合约和真实进程恢复通过。 |

容器检查覆盖相应用户空间；各二进制包按其实际构建和运行环境标注，不宣称通用
Linux可移植性。其他系统暂不属于首版验收范围。详细记录见
[现代Ubuntu结果](docs/ubuntu/UBUNTU_MODERN_RESULTS.txt)。

## 从源码构建

安装构建依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git openssh-client \
  pkg-config libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
```

检出要验证的分支或发行标签，再初始化固定依赖：

```bash
git clone https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
git checkout v2.0.0
git submodule update --init --recursive --jobs=4
python3 -I -B tools/prepare_vm_vendor.py --apply
python3 -I -B tools/prepare_vm_vendor.py --check
```

VM固定基线由独立公开源码镜像MatrixCoreX/AIC-VM提供，提交和继承许可均保持不变。
当前 CI 已在不提供私有部署钥的情况下从空目录递归检出全部188个固定模块，
并验证四个 VM 头文件的补丁。GitHub 自动生成的源码压缩包不包含子模块；
从源码编译请使用上述 Git 检出和 vendor 准备步骤。运行包的编译身份是来源清单
摘要，发行 Git 提交另行记录，两者的文档/交付脚本差异见来源对应报告。

现代构建入口使用独立目录，不替换已有节点或安装CDT：

```bash
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
bash tools/ubuntu-modern-build.sh --test --jobs=2
```

输出位于`build/ubuntu-<版本>-jit/`：节点为`programs/nodeos/pureeos-ubuntu-nodeos`，
同时构建cleos、keosd和helios-util。`--test`执行明确的队列、计时器及生产者组件，
不是完整整链或TPS验收。可用`--build-dir=/your/isolated-build`指定独立目录。

此配置明确`ENABLE_OC=OFF`，无需另装LLVM开发包；编译合约需要匹配的AIC CDT SDK。
首次安装、签名钥、资源初始化及矿工加入请按
[使用说明](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt)执行。
默认测试私钥和测试资金不用于正式创世；构建脚本不会自动创建或升级现用链。

## 使用发行包

运行包面向 Ubuntu 26.10 x86_64 用户空间，不捆绑主机动态库。
下载运行包和 SHA256SUMS 后，在新目录中核验和解压：

```bash
sha256sum -c SHA256SUMS --ignore-missing
mkdir pureeos-v2.0.0
tar -xzf pureeos-v2.0.0-ubuntu26.10-x86_64.tar.gz -C pureeos-v2.0.0
cd pureeos-v2.0.0
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

运行包包含 nodeos、cleos、keosd、helios-util 和匹配的系统合约。
CDT SDK 解压后以 `aic-cdt-1.0.0/bin` 加入 PATH，并把 SDK 根目录传给
`CMAKE_PREFIX_PATH`；安装目录可以含空格。SDK 内也有独立 SHA256SUMS。
本版修复了参数封装读写、零额领奖、线程CPU计费和VM函数入口下标检查。
确定性执行工作计量/容量协议在独立分支开发，不包含在本版。

## 系统规则与恢复

本轮系统合约使用一票一投、取消代理、候选线性扩容、顺序单席轮岗、实际出块奖励，
以及开发者升级和固定14天公示。节点与系统合约须匹配；合约构建现已默认开启并
要求全部新链功能，旧配置不能静默生成旧规则合约。

非正常停机后若提示数据库dirty，先保留区块、finalizer安全数据和配置，按实际
日志选择原生重放流程；不要手工清除finalizer安全目录。当前版本已完成串行和
4-worker各三条新链的异常停止、重启与重放检查。历史40进程轮岗、真实主机停启、
限定私有worker范围的ASan/UBSan和真实24h领奖证明保留各自的旧程序/合约摘要；
它们不冒充本版新程序重新运行的同名整机或24h任务。

## 许可

自有原创部分的许可见[AIC开源许可1.0](AIC_LICENSE.md)。继承代码和第三方声明按
实际许可保留，范围见[LICENSING.md](LICENSING.md)与[LICENSE](LICENSE)。当前继承
基线中的BSL生产用途适用范围仍在核清，不以新许可文件宣称全仓库已重新授权。
