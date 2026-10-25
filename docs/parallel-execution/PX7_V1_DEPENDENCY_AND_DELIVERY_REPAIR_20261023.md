# 依赖入口与候选交付包补齐

2026-10-23 15:05 UTC。状态：本节交付检查通过；完整轮岗、真实领奖、性能和正式联合发布仍未完成。

交付核对发现 README 仍指向旧 `PureEOSIO/helios`，若干子模块仍指向当前 GitHub 连接和匿名 Git 都返回 404 的 `PureEOSIO` 地址。它们会影响全新获取源码，不能以本机已有依赖作为干净检出已过的证据。

六个变化依赖保留原提交，仅修正获取来源：

| 依赖 | 固定提交 | 获取来源 |
|---|---|---|
| appbase | `74ac065ddacc46b28318f46965b6686976e84e3c` | AntelopeIO/appbase |
| EOS VM | `16f9c4c3ea7a172716d0bf68c60806e499f5988a` | MatrixCoreX/PureEOS-VM，当前私有开发仓库 |
| SoftFloat | `19cbea6c254cdbb5d539584e5696675de997721d` | AntelopeIO/berkeley-softfloat-3 |
| CLI11 | `0860a139578f699df286048808583fcc4846a703` | AntelopeIO/CLI11 |
| bn256 | `161ca72af4c647ad0b23ccd97d2dd84201fc2b37` | AntelopeIO/bn256 |
| BLS | `472a13f1f8bb225078f1aa3a4bc27cd5d8450402` | AntelopeIO/bls12-381 |

独立 VM 仓库只推送已验证的基线提交和其原有祖先，开发分支 `develop`。没有将未验证的新代码混入运行时，没有建立稳定标签/Release，也没有将仓库公开。原 VM 工作树和 Git HEAD 保持不变，另外三个未提交头文件修改仍由主仓库的准确补丁及摘要交付。主仓库相对 URL `../PureEOS-VM.git` 在实际父仓库 SSH 地址下解析正确。

实际验证从空目录建立临时 Git 根，只放入当前 `.gitmodules` 和六个准确 gitlink，然后运行正常 `git submodule update`，六个提交全部匹配。递归初始化共九个依赖模块通过。远端全新 VM 检出应用真实补丁后，目标头文件与当前候选逐字节一致。这个私有鉴权测试不能替代正式发布前的公开匿名完整检出检查。

README 已改为实际主仓库入口和当前开发状态，移除尚未发布却提示安装旧 Helios 二进制的说明，更新 Ubuntu 26.10 的实际验证范围，并说明私有 VM 开发依赖权限。没有更换依赖版本、修改 VM 头文件或切换运行中的链。

候选包补入 `AIC_LICENSE.md` 和 `LICENSING.md`，保留构建源中的原 `LICENSE`。它们描述既有许可边界，不表示全仓库已重新授权。当前运行包只有四个程序和 16 份明确选择的系统/资源合约，不混入旧 msig/wrap 或编译探针。

| 验证 | 实际结果 |
|---|---|
| 打包控制 | 五项通过，覆盖确定性、篡改、重复/额外/链接路径、不完整输入和错误正式发布声明拒绝 |
| 实际候选包 | 27 个文件，39,122,149 字节 |
| 重复打包 | 两次 SHA256 相同：`96c0c20e5769f2bc838af60a7552d3e747d32171d5c5de7bbd9620c2017a9be0` |
| 云端原生解压 | 新建带空格路径，所有交付文件摘要相同 |
| 四工具运行 | 动态依赖四项及各自支持的 CLI 四项全部通过 |
| 错误旧预检负对照 | `cleos --version`、`helios-util --version` 都实际被拒绝 |
| 节点影响 | 实测前后原有节点 PID 集合不变，没有重启当前轮岗链 |

由负对照发现，排队领奖任务原来给四个程序统一传 `--version`，会让两项预检错误失败。已改为 nodeos/keosd 的 `--full-version`、cleos 的 `version full`、helios-util 的 `--help`。前三个精确匹配原源码清单版本，工具帮助执行与准确 SHA 作为第四个程序的身份/运行证据。15 项接续/归属/证据控制重新通过，冻结清单纳入实际 probe 策略，接续服务已重新启用。没有创建新的 VM，真实 24h 仍未开始。

独立交付审计实际读取并验证：

- 完整原始构建源码包的 75,911 个文件及其二进制来源绑定。
- 当前稀疏工作树存在的 478 份运行源码，与原构建清单全部相同；未在工作树展开的源文件不能借此称为已比对。
- 当前 VM 的 2,506 个已跟踪普通文件，与冻结构建源码全部相同。
- AIC CDT 1.0.0 实际 SDK 归档摘要，以及归档内五个真实 ELF 与已通过前端构建的摘要相同。
- 新 SDK 产出的 23 份实际 WASM/ABI 摘要相同；其中 16 份运行合约与核心测试使用字节一致。
- 新运行包的四程序、16 合约、完整清单和重复归档摘要一致。

这个审计证明交付材料之间的完整性关联，不是重新编译整节点后获得相同 ELF 的声明，也不是运行时/性能全验收。当前 README、依赖 URL 和交付工具更新作为后来变更独立记录，不将整个工作树冒充原始编译快照。

证据根为 `/run/media/guagua/Windows/PureEOS-v1-hardening-20261022`，主要目录：`dependency-delivery-repair-attempt-1`、`runtime-candidate-package-attempt-2`、`runtime-candidate-package-attempt-2-repro`、`joint-candidate-delivery-audit-attempt-1`。云端公开报告为 `cloud-cohort-retest-20261023-attempt-2/runtime-delivery-2-native-smoke.json`。开发仓库不等同正式发行。

GitHub连接器没有创建仓库接口，因此私有开发库通过现有已登录的gh与Git完成创建/推送，并用API核对分支提交；没有发送消息、Issue或发布Release。主仓库本轮工作树尚未提交/推送为最终发布提交。

15:14UTC实链补充：主验收仍active/PID33508；委员会已观察到1..21全部规模。当前proposer21/finalizer21/threshold15，head10289/LIB10287，stage2。完整30/40再入席和故障尚未完成，不能据此发布。源码/交付操作快照5已关闭核验：310份文件、895142字节，SHA256 `64bf3b729b4b8e6107d2d4ae32fc4f620861a62308eaf8a42910def6f5706bfa`，创建15:13；本条实链状态晚于该快照，单独保存为云任务的`latest-committee-growth-observation.json`。

15:34UTC完整依赖补验通过：同一最初空目录夹具补全到12个顶层、递归188个模块，所有固定提交匹配，原六根成功不冒称重复重跑。公开匿名访问仍因私有VM开发库待正式发布前验收。详见dependency-complete-checkout-attempt-1/report.json。
