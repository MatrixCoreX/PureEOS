from pathlib import Path
import json,shutil
D=Path(__file__).resolve().parent;R=D.parents[1];W=R/'PureEOS-Testnet-Ops-20261111';A=W/'operations/20261112-git-upload-audit'
for f in ['HEAD_COVERAGE_VERIFICATION.json']:shutil.copy2(D/f,A/f)
for f in ['verify_head_coverage.py','prepare_publication.py','write_reports.py']:
 if (D/f).exists():shutil.copy2(D/f,A/'audit-tools'/f)
s=json.loads((A/'RESULT.json').read_text());s['status']='PASS_SOURCE_AND_TAG_UPLOADS_VERIFIED';s['document_publication']='These records are included in the containing Ops Git commit. Final publication/ref verification is retained in the original local audit FINALIZATION_RESULT.json.';(A/'RESULT.json').write_text(json.dumps(s,ensure_ascii=False,indent=2)+'\n')
(A/'REPORT.zh-CN.md').write_text('''# Git 上传核对与补齐记录（2026-11-12）

已直接核对 GitHub，补齐此前遗漏的运维分支、历史源码、暂存区版本和旧标签。最新钱包 test39、KEY、Signup、系统账号提案、浏览器、History 及抗量子方案均已有对应远端提交。

| 范围 | 结果 |
|---|---|
| 工作树与仓库 | 49 个原登记工作树、8 个旧依赖/改动子模块工作树、2 个额外历史仓库，共 59 个工作树、19 个 Git 公共仓库 |
| 本地分支 | 129 条分支记录、121 个不同仓库内的唯一分支头全部可从实际 GitHub 分支到达 |
| 新增备份分支 | 37 个：5 个依赖原提交、3 个依赖工作版本、10 个主工作版本、3 个原始暂存区版本、16 个未发布历史提交 |
| 标签 | 33 条本地标签记录全部核对；补充 19 个归档标签 |
| 内容复核 | 13 份工作版本中 742 条已审阅路径逐项复核，3 份暂存区树与原索引完全一致 |
| 文档补充 | 28 份原根目录方案/实验文档；命名账号资源代付研究原始 JSON 与中英说明 |

## 实际遗漏与修复

`PureEOS-Test` 的 `origin` 指向本机 `PureEOS-v1-hardening`，之前对这个远端的推送只到了本机仓库。`ops/relay2-eligibility-20261111` 的 `55dfdcd1d0f8b12f4d87a653f1cbfbe9b280cda2` 当时没有出现在 GitHub，本次已经通过 `publish` 远端补齐。此报告和附带文档继续通过实际 GitHub 的 `publish` 远端提交。

以后核对这组运维分支，应使用 `git ls-remote publish` 或显式 MatrixCoreX GitHub URL。只看本机 `origin`、本机跟踪分支或“推送成功”输出不足以证明已上传 GitHub。

## 归档与恢复

完整链接见 [ARCHIVE_INDEX.md](ARCHIVE_INDEX.md)。源码分支使用 `backup/git-upload-20261112/` 前缀，分别存入 MatrixCoreX 的 PureEOS、PureEOS-CDT、PureEOS-Explorer 和 PureHistory 仓库；标签也放入同日前缀的归档命名空间。

历史工作版本沿用原提交作为父提交。暂存区与工作目录内容不同时分别归档，原文件、分支、HEAD 和索引保持原样。原来尚未提交的历史目录仍可显示有改动，这是保留开发现场的预期结果。本次只有原本干净的运维工作树新增核对报告及文档提交。没有把历史实验合并到当前发布分支。

部分旧分支名在远端已经不存在，或远端同名分支已前进；其提交内容已经在远端历史或新备份中，原分支名与提交号保存在 `HEAD_COVERAGE_VERIFICATION.json`。本次没有回退或强推现有远端分支。

旧 PureEOSIO 依赖的备份也已上传。仅在工作版本归档树中，把不可用的旧依赖 URL 改为对应 MatrixCoreX 备份，并绑定确切备份提交。原源码中的 `.gitmodules` 与原始暂存区快照保持原样。CDT 的 LLVM、Clang、LLD 均无本地修改，提交与官方 GitHub main 一致。

## 核验边界

上传及内容核对只读取源码/Git 元数据，没有构建、部署、链上操作、服务或手机操作。私钥、凭证、钱包、运行配置、区块数据、构建产物与缓存保留本机；两个排除的运行配置路径记录在 `RESULT.json`。根目录文档和资源代付研究按原阶段归档，资源代付仍是研究方案。

稀疏签出中未展开的源码按原索引保留；一份旧 History bundle 缺少的历史对象通过现有完整本地仓库只读取得。最终已独立核对远端引用、文件内容和暂存区树。过程中的失败证据保留本机，没有重跑旧部署、手机安装或金融操作控制器。
''')
(A/'REPORT.en.md').write_text('''# Git upload audit and completion — 2026-11-12

Direct GitHub checks confirmed the current wallet test39, KEY, Signup, system-account proposals, explorer, History and bilingual post-quantum plan. Missing historical source and operations work has been archived.

| Scope | Result |
|---|---|
| Inventory | 49 registered worktrees, 8 legacy/modified dependency worktrees and 2 additional historical repositories: 59 worktrees across 19 common Git repositories |
| Local branches | All 129 local branch records, representing 121 unique repository/head pairs, are reachable from actual GitHub branch heads |
| New backup branches | 37: 5 dependency heads, 3 dependency working snapshots, 10 main working snapshots, 3 original staged trees and 16 unpublished historical heads |
| Tags | All 33 local tag records verified; 19 archive tags added |
| Content | 742 reviewed paths verified across 13 working snapshots; 3 staged snapshots exactly match their original indexes |
| Documents | 28 original root-level project notes plus the named-account resource sponsorship research and bilingual context |

## Corrected publication gap

The `PureEOS-Test` remote named `origin` resolves to the local `PureEOS-v1-hardening` repository. Earlier successful pushes to that remote did not publish to GitHub. The operations branch `ops/relay2-eligibility-20261111` at `55dfdcd1d0f8b12f4d87a653f1cbfbe9b280cda2` was absent on GitHub and has now been pushed through the actual GitHub remote named `publish`.

Future operations publication checks must use `git ls-remote publish` or the explicit MatrixCoreX GitHub URL. Local tracking refs and successful local pushes alone do not establish GitHub publication. This report and its accompanying documents are published through `publish`.

## Archives and recovery

[ARCHIVE_INDEX.md](ARCHIVE_INDEX.md) links every added source branch and lists the archived tags. References use the `backup/git-upload-20261112/` namespace in the user's MatrixCoreX repositories. Historical source remains separate from deployed release branches.

Working snapshots retain the original commit as a parent. Distinct staged and working versions have separate backups. Original historical source files, branch pointers, HEADs and index bytes are preserved; previously dirty worktrees intentionally remain dirty. Only the initially clean Ops worktree receives the final audit/research/documentation commit.

Some historical branch names are absent remotely or the remote branch has advanced. Their commits are already reachable through remote history or the new archives; the original local names and SHAs are recorded in `HEAD_COVERAGE_VERIFICATION.json`. Existing remote branches were not forced backward.

Legacy PureEOSIO dependency commits are also archived. Working archive trees alone point unavailable dependency URLs to their exact MatrixCoreX backup commits. Original `.gitmodules` files and exact staged snapshots remain unchanged. Clean LLVM, Clang and LLD dependencies match their official GitHub main heads.

## Verification and scope

Remote refs, reviewed file content and staged trees were independently verified. Sparse source files absent from the working directory retain their original indexed content. An incomplete historical History bundle used the existing complete local History object database without editing its source, index, config or alternates.

This was a Git/source/documentation task. No builds, deployments, chain writes, financial actions, server/service changes, phone actions or subagents were used. Runtime configuration, credentials, private keys, wallets, chain data, build products and caches remain local. The historical project notes retain their original context; native resource sponsorship remains a design study. Failure evidence remains local, and completed deployment/phone/financial controllers were not replayed.
''')
entry='- COMPLETE 2026-11-12 Git upload audit: 59 source worktrees/19 common repos,129 localbranch records/121 uniqueheads reachable on ACTUAL GitHub;37 immutable backup branches+19 archived tags,33 localtags checked;742 reviewedpaths/13 working snapshots+3 exact original stagedtrees PASS. Fixed ops/relay2-eligibility-20261111 missingGitHub55df: TEST origin is LOCAL PureEOS-v1-hardening; publish is GitHub. Future Ops uploads MUST use publish/explicitMatrixCoreXURL and direct ls-remote, never inferGitHub from localorigin. Historical source/HEAD/index preserved, onlycleanOps receives documentation commit;28 rootnotes+native sponsorship research archived, sponsorship stilldesignonly. No forcepush/merge/build/server/chain/finance/phone/subagent actions. Root release-work-20261112/git-upload-audit-r1 authoritative; original failure/evidence retained. Completed one-shot archival tools MUSTNOT blindlyrerun; old deployment/install/financial controllers MUSTNOT rerun. Final document publication receipt in FINALIZATION_RESULT.json.\n\n'
for path in [R/'AGENTS.md',W/'AGENTS.md']:
 text=path.read_text()
 if not text.startswith('- COMPLETE 2026-11-12 Git upload audit:'):path.write_text(entry+text)
print('PUBLIC_REPORTS_AND_COMPLETION_NOTES_PREPARED')
