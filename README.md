# PureEOS

PureEOS 是独立维护的 C++ 新链项目，使用原生 Savanna 最终确认，支持不同交易的
并行合约执行和按区块顺序提交状态。配套工具链为 **AIC CDT 1.0.0**。

当前首个并行版本仍在验收，尚未联合正式发布。默认开发分支、历史程序和候选包
不代表稳定版。实际进展见[验收状态](docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt)
和[发布清单](docs/parallel-execution/PX7_V1_RELEASE_CHECKLIST.txt)。

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
| Ubuntu 26.04 | 原生Release构建、生产者相关组件及恢复检查已有记录；最新远程原生构建仍在执行。 |
| Ubuntu 26.10 | 当前源码节点、工具、核心回归、AIC CDT和系统合约集成，以及两VM上的30→40候选轮岗与故障拉黑已验证。整机恢复、完整插桩及真实24h领奖继续补测。 |

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
# 选择对应发行说明给出的准确ref；当前没有已宣布的稳定标签。
git submodule update --init --recursive --jobs=4
python3 -I -B tools/prepare_vm_vendor.py --apply
python3 -I -B tools/prepare_vm_vendor.py --check
```

VM固定基线由独立公开源码镜像MatrixCoreX/AIC-VM提供，提交和继承许可均保持不变。
已在禁用凭据的独立目录检出该提交及两个嵌套依赖，并重现三份已测VM补丁；
整个节点的匿名递归检出仍须单独核验。完整源码归档可只读检查已准备的vendor，
不把一个源码清单摘要伪装成Git提交。

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
[候选使用说明](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt)执行。
默认测试私钥和测试资金不用于正式创世；构建脚本不会自动创建或升级现用链。

## 系统规则与恢复

本轮系统合约使用一票一投、取消代理、候选线性扩容、顺序单席轮岗、实际出块奖励，
以及开发者升级和固定14天公示。节点与系统合约须匹配；合约构建现已默认开启并
要求全部新链功能，旧配置不能静默生成旧规则合约。

非正常停机后若提示数据库dirty，先保留区块、finalizer安全数据和配置，按实际
日志选择原生重放流程；不要手工清除finalizer安全目录。整机补测结果以验证记录为准。

## 许可

自有原创部分的许可见[AIC开源许可1.0](AIC_LICENSE.md)。继承代码和第三方声明按
实际许可保留，范围见[LICENSING.md](LICENSING.md)与[LICENSE](LICENSE)。当前继承
基线中的BSL生产用途适用范围仍在核清，不以新许可文件宣称全仓库已重新授权。
