"""Format verified remote results; publish no private frontend sources to core."""
from pathlib import Path
import hashlib,json,shutil,tarfile
D=Path(__file__).resolve().parent;W=D.parents[1];E=D/'evidence';R=E/'reports'
read=lambda n:json.loads((R/n).read_text())
p=read('PROPOSAL.json');x=read('EXECUTION.json');canonical=read('EXECUTION_CANONICAL.json');smoke=read('LIVE_SMOKE.json');history=read('API_after_PUBLIC_RESULT.json')
before=read('API_before_PUBLIC_RESULT.json');audit=read('LIVE_AUDIT_after.json');cleanup=read('CLEANUP_RESULT.json');fee=read('LIVE_FEE_INTEGRATION.json');source=read('SOURCE_PROVENANCE.json')
assert x['status']=='UPGRADED_IRREVERSIBLE'
for report in [canonical,smoke,history,audit,cleanup,fee,source]:assert report['status']=='PASS'
for name in ['APPS_SWITCHED.json','LEDGER_after-upgrade.json']:assert read(name)['status']=='PASS'
assert json.loads((E/'browser-public-after/RESULT.json').read_text())['status']=='PASS'
assert json.loads((E/'navigation-public/RESULT.json').read_text())['status']=='PASS'
assert audit['original_node_PIDs_unchanged']and audit['votes_bonds_unchanged']and audit['consensus_caps_unchanged']
money=lambda n:f'{int(n)//10000}.{int(n)%10000:04d}'
link=lambda tx:'https://aic-test.matrixai.one/#/transaction/'+tx
rows='\n'.join('| '+str(c['ram_budget_bytes'])+' | '+c['principal']+' | '+money(c['quote']['execution'])+' | '+money(c['quote']['storage'])+' | '+money(c['base_atoms'])+' | '+money(c['service_atoms'])+' | '+money(c['total_atoms'])+' | [交易]('+link(c['transaction_id'])+') |'for c in smoke['cases'])
text=f'''# 开发者提案页面、导航与 RAM 计费升级（2026-11-04）

状态：开发、发布、完整1800秒原生公示、不可逆升级、真实交易/历史/浏览器验证与临时磁盘清理全部完成。

## 页面与发布

[开发者提案页面](https://aic-test.matrixai.one/#/upgrades)与治理页读取原生get_upgrade_state，显示最新提案、开发者、目标、公示时刻、有效选民/否决门槛及默认折叠的哈希/选民。按实际链上区块时间计算剩余时间；公示结束仍待执行。RPC只保留最新提案，pending=false不区分执行或撤回，因此显示“已结束”，单独报告当前代码匹配，未伪造完整提案历史。

导航栏取消可见滚动条，矮屏使用紧凑间距，保留滚轮/触摸/键盘访问全部入口；正文与数据表滚动不变。桌面误显示的手机菜单按钮同时修正。中英文、深浅色、1366×768/1440×900/390×844/844×390实际Chrome导航验证均通过。

最终静态发布：{audit['public_frontend_release']}。六个公网文件逐字节SHA256匹配；JavaScript内容摘要在仅CSS迭代中保持相同。前端源码提交{source['frontend_commit']}，{source['compiled_inputs_match_git']}个构建输入匹配Git，最终源码归档SHA256为{source['qualified_source_sha256']}。前端源码归档仅保留在私有浏览器仓库及本地证据；公共合约仓库不包含其源码或完整私有归档。

## 实际原生提案与升级

链ID：{x['chain_id']}。

- 提案#{x['proposal_id']}：[提交交易]({link(p['transaction_id'])})，不可逆块{p['canonical_receipt']['block_num']}。
- 原生提交时间{p['governance']['proposed_at']}UTC，最早执行{p['governance']['execute_after']}UTC；原生公示1800秒，24有效选民/16票否决门槛。
- [实际devexec交易]({link(x['transaction_id'])})，不可逆块{canonical['block_num']}，时间{canonical['timestamp']}UTC。实际间隔{canonical['review_elapsed_seconds']}秒，执行回执executed，CPU用量{canonical['CPU_usage_us']}微秒。
- 新WASM：{x['new_code_hash']}；原始ABI摘要仍{x['new_abi_hash']}，无数据迁移。

复用此前已远端通过114正常原生/115明确标注夹具/21SDK的相同bc3358d7候选，没有重编译或替换已审核字节。来源提交d5c176e7d9f14ad98129c5631f3bbdece8a3ba3e，资格源码归档6d1e987f8d5dbb3f65c763d348954ab7b8c002d302d76ef11f56754f8a7250a6。审核文档及摘要见RAM_REVIEW.json与PROPOSAL.json。正式版本七天不变，无直接setcode、提前执行或重启链。

## 实际费用与本金

新增行按0/512/1536字节核定预算，读取当前已审核RAM/AIC储备：net=ceil(Q×B/(R−B))，RAM=ceil(net×200/199)，含0.5%买入手续费。CPU/NET PowerUp市场倍率仅乘执行项，基础费=max(100原子单位,执行项+RAM)，再加10%–30%中继服务费并向上取整。保留0.0100 AIC基础保底、签名总费上限、赞助预算、币种白名单、USD命名账户预先open及领取免应用费。此为预算市场报价，不是原生RAM精确计量，也不执行自动RAM购买。

三笔真实小额转账仅使用原创世HTTP中继30%；第三笔由现有原生cleos旧请求签名。每笔已核对规范不可逆回执、相同包重试不重复、发送人额外付费、完整本金到账、预算新增字节及RAM购买储备未被转账改变。

真实环境出现原创世RPC读表500/截断响应，原始失败及已冻结包全部保留。SDK/中继增加可选只读RPC，24项远端测试通过；读数据走现有历史节点8890，广播仍走创世8888，显式核对两端链ID与写端代码pin。默认单端点兼容，未增加自动写重试、HTTP入口、密钥或节点。只重启原中继应用；原日志保留，前两笔已完成交易复核后继续第三笔，没有重新准备或重复支付已广播任务。此配套运行时源码归档为4feeba4c2bd74469660ea0010df0c9df47737575f1b893404c0e9222417b5ee9，合约/ABI字节保持不变。

| 预算字节 | 本金 | 执行项AIC | RAM项AIC | 基础AIC | 中继AIC | 总费AIC | 回执 |
|---:|---|---:|---:|---:|---:|---:|---|
{rows}

新收款测试公钥：{smoke['public_key']}。私钥只保留在远端受限文件，不进入证据或Git。AIC/USD原生储备、合约reserve和完整负债等式逐一相等。配套SDK/中继切换到相同资格源码，原热钥、权限、HTTP入口和已完成日志均保留；历史投影只更新代码pin。

## 验证与运行边界

251项完整前端测试通过；后续CSS调整重复TypeScript/build及实际Chrome检查，功能JavaScript摘要不变。最终提案预览、升级前公网、公示中公网、升级后公网各四组；最终导航预览/公网各四组，全部通过。升级后首页四种浏览器组合共60次实际费率/存储场景检查，公式保持默认折叠；另有{fee['SDK_UI_comparisons']}组真实已部署市场版本SDK/前端整数比较。旧迭代截图及日志另行保留。

原{before['wallets']}钱包/{before['views']}历史视图/{before['unique_native_transactions']}笔交易逐条保留；新增三笔后为{history['wallets']}钱包/{history['views']}视图/{history['unique_native_transactions']}笔原生交易。实际receipt金额、费用承担方、旧新公钥格式、筛选与错误请求均验证；不声称完整终身历史覆盖。

实际主机仍是原共享Intel Cascadelake4vCPU/2个来宾核心/8125768KiB内存，物理核心独占不可见。链参数保持max_block_cpu_usage=800000、max_block_net_usage=1048576、max_transaction_cpu_usage=150000，WASM参数也完全一致。原{audit['node_count']}节点PID与unit配置、投票、保证金、系统/代币/节点字节均保持，25注册/24合格，原germanycrest禁令保留，native21成员/15票。后台普通业务与主历史服务持续运行，仅重启匹配升级所需的中继和公钥历史投影应用。未停链、重置、租服务器或运行TPS；不据功能通过宣称主机容量或延迟问题解决。

首次静态发布的Python urllib公网校验收到403，改用原有Node校验及实际Chrome，同源请求返回200且六文件摘要完全匹配；原失败保留。成熟后的首次账户只读预检查500时，流程在签名/广播升级前停止；改用历史只读RPC和有界读取重试后完成，不更改节点参数。读写分离测试的首次启动脚本有引号错误，未启动测试或改服务，随后独立脚本修正。原生cleos验证前重新解锁原签名钱包；所有失败及恢复记录保留。公示期间导航的后续CSS发布不修改提案代码/ABI/审核文档或截止时间。

## 磁盘清理与保存

先校验完整证据与最终源码/制品，再仅删除本轮已停用frontend工作树及重复styles.css，释放分配空间{cleanup['allocated_removed_MiB']}MiB；清理后可用{cleanup['available_GiB']}GiB。净文件系统差异包含持续链活动，见原始结果。当前静态版本、活动SDK/中继、共享依赖、私钥、当前链/历史数据库、原26节点与先前不可变档案均保留。失败日志、所有源码版本摘要、原生提案/执行、真实交易、截图及清理清单完整保留。不要重跑已完成治理或付款控制器。

English: the read-only native proposal page and scrollbar-free navigation are deployed. The same qualified RAM candidate completed its full1800-second TEST review and irreversible execution. Three real founder-relayed AIC/USD payments, exact balances/fees/budgets/reserves, preserved history and post-upgrade browser checks passed. Production review and all protected chain/node settings remain unchanged. Private explorer source is retained only in private storage.
'''
(D/'REPORT.md').write_text(text)
relative=Path('performance-results/machines/intel-xeon-cascadelake--shared-vm--4vCPU-2guestcores--8125768KiB/20261104-dev-proposals-ram-activation-r1')
for repository,private in [('PureEOS-Key',False),('PureEOS-explorer-key-history-20261103',True)]:
 out=W/repository/relative;out.mkdir(parents=True,exist_ok=True);shutil.copyfile(D/'REPORT.md',out/'REPORT.md');shutil.copyfile(D/'PLAN.md',out/'PLAN.md')
 evidence=out/'evidence';evidence.mkdir(exist_ok=True)
 for pth in E.rglob('*'):
  if not pth.is_file():continue
  rel=pth.relative_to(E)
  if not private and rel.parts[0]in ['source','static']:continue
  dest=evidence/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(pth,dest)
 operations=out/'operations';operations.mkdir(exist_ok=True)
 for pth in D.iterdir():
  if pth.is_file()and pth.suffix in ['.py','.mjs','.cjs']:shutil.copyfile(pth,operations/pth.name)
 digests={str(pth.relative_to(out)):hashlib.sha256(pth.read_bytes()).hexdigest()for pth in out.rglob('*')if pth.is_file()and pth.name!='EVIDENCE_MANIFEST.json'}
 (out/'EVIDENCE_MANIFEST.json').write_text(json.dumps({'frontend_source_included':private,'files':digests},indent=2)+'\n')
 if private:shutil.copyfile(D/'VERIFIED_EVIDENCE_MANIFEST.json',out/'FULL_ARCHIVE_MANIFEST.json')
print('Verified completion report and visibility-scoped evidence copied to both repositories')
