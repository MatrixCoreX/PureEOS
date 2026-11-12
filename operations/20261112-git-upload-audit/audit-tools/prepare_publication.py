from pathlib import Path
import json,shutil,datetime,collections,re,hashlib
D=Path(__file__).resolve().parent;R=D.parents[1];W=R/'PureEOS-Testnet-Ops-20261111';A=W/'operations/20261112-git-upload-audit';A.mkdir(parents=True,exist_ok=True)
P=json.loads((D/'PLAN.json').read_text());B=json.loads((D/'BACKUP_RESULT.json').read_text());T=json.loads((D/'TAG_RESULT.json').read_text());C=json.loads((D/'CONTENT_VERIFICATION.json').read_text());doc=json.loads((D/'ROOT_DOCUMENT_INVENTORY.json').read_text());assert B['status']=='PASS_ALL_SOURCE_BACKUPS_PUBLISHED'and T['status']=='PASS_ALL_LOCAL_TAGS_VERIFIED'and not doc['findings']
for f in ['PLAN.json','BACKUP_RESULT.json','CONTENT_VERIFICATION.json','TAG_RESULT.json','ROOT_DOCUMENT_INVENTORY.json','ADDITIONAL_REPOSITORIES.json','ADDITIONAL_REMOTE_CHECK.json','LOCAL_AUDIT_FINAL_BEFORE_DOCS.json','GITHUB_REMOTE_HEADS_FINAL_BEFORE_DOCS.json']:
 shutil.copy2(D/f,A/f)
sc=A/'audit-tools';sc.mkdir(exist_ok=True)
for n in ['audit_local.py','audit_remote.py','prepare_plan.py','prepare_supplement.py','upload_backups.py','upload_tags.py','verify_backup_content.py']:shutil.copy2(D/n,sc/n)
(sc/'README.md').write_text('These are completed audit/one-shot archival tools, retained for review. Do not rerun the upload/preparation tools against changing worktrees. Original local evidence is under release-work-20261112/git-upload-audit-r1. Read-only tools use local Git object/index metadata; they do not build, deploy, sign transactions or update phones. Plans and previous immutable commit IDs are inputs, not a live automatic backup service.\n')
archives=W/'docs/archive/20261112-root-project-notes';archives.mkdir(parents=True,exist_ok=True)
for f in doc['files']:
 src=R/f['path'];assert hashlib.sha256(src.read_bytes()).hexdigest()==f['SHA256'];shutil.copy2(src,archives/f['path'])
(archives/'README.md').write_text('# Historical project notes / 历史项目文档\n\nThese 28 original root-level Markdown/text notes are preserved verbatim as of 2026-11-12. They record historical designs, experiments and test results, and do not describe one unified current deployed version. No code was built or deployed by this upload. The manifest records exact original filenames and SHA-256 values.\n\n这 28 份项目根目录的方案、实验及测试文档按原文归档。各文档形成于不同阶段，不能合并理解为当前链上实现；本次上传没有执行其中的操作。原文件名和 SHA-256 见 manifest.json。\n')
(archives/'manifest.json').write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n')
research=W/'operations/20261111-named-resource-sponsorship-research';research.mkdir(parents=True,exist_ok=True)
for f in ['RESEARCH.json','CHAIN_CAPABILITIES.json']:
 shutil.copy2(R/'release-work-20261111/named-resource-sponsorship-research-r1'/f,research/f)
(research/'README.md').write_text('''# Named-account resource sponsorship / 命名账号资源代付研究

Research recorded on 2026-11-11, published on 2026-11-12. This remains a design study: no sponsorship implementation, contract upgrade, simulation execution or live sponsored native transfer was performed.

协议层面，具备所需资源和授权的普通命名账号也可以做 CPU/NET 代付，不需要 BP 身份或系统特权。当前 eosio.key 商业中继登记的 BP 资格限制属于现有业务规则，不能等同于协议限制。原研究中的“沿用合格 BP 登记”是当时的一个建议，并非已经决定所有未来原生代付服务必须由 BP 提供。

- 当前链已在第 1 块激活 `ONLY_BILL_FIRST_AUTHORIZER`。承担 CPU/NET 的账号必须是首个普通 action 的首个授权者；仅调整签名数组顺序无效。
- RAM 仍按具体合约动作的 payer 规则分配。可在完整交易中预先按报价购买发送账号缺少的 RAM，再由发送账号承担数据行 RAM；不能假设代签会自动改变 RAM payer。
- 先组装完整交易，明确收款、币种、本金、资源购买及服务费，由用户核对签署，再由资源方对相同字节共同签名。固定 AIC RAM 支出可以限制现金成本，实际获得字节不足则整笔交易失败。
- 命名转命名可研究使用现有原生动作组合；命名转公钥当前直接充值入口严格要求两个根动作，不接受任意代付前缀，需要另行设计受治理的专用入口，保留现有直接零手续费路径。
- `compute_transaction` 可帮助发现执行、资源及 RAM 问题；它不能证明签名授权成立，也不能保证未来状态、执行耗时或 AIC 成本相同。接口存在已通过只读元数据确认，本研究没有执行该接口。
- 当前钱包与中继没有实现这套原生代付服务。是否开放普通服务商、是否在链上强制资格，应在后续方案中明确。

At protocol level an ordinary named account with sufficient resources and the required authorization may sponsor CPU/NET; it does not need BP status or privileged status. Existing KEY relay BP eligibility is a separate application rule. The earlier BP-registry recommendation is an option, not an implemented universal restriction. RAM payer rules, immutable joint signatures, explicit fees, atomic rollback and the current strict KEY two-root deposit boundary must be addressed separately. `compute_transaction` is useful preflight evidence, not proof of authorization or a cost guarantee. See the original source hashes and historical read-only capability snapshot in the JSON files.
''')
newtags=[t for t in T['tags']if t['action']=='UPLOADED_ARCHIVE_TAG'];unique_heads={(e['common'],h['commit'])for e in P['entries']for h in e['local_heads']};summary={'status':'PASS_SOURCE_AND_TAG_UPLOADS_VERIFIED_DOCUMENT_PUBLICATION_PENDING','verified_UTC':B['verified_UTC'],'scope':'Project Git source/documentation/history backup only; runtime configuration, credentials, keys, wallet files, build artifacts and chain data excluded.','planned_source_worktrees':len(P['entries']),'git_common_repositories':len({e['common']for e in P['entries']}),'original_registered_worktrees':49,'additional_historical_repositories':2,'legacy_or_dirty_submodule_worktrees':8,'dirty_source_worktrees_archived':sum(e['dirty']for e in P['entries']),'unique_local_head_commits_covered':len(unique_heads),'new_backup_branches':len(B['published']),'backup_kinds':dict(collections.Counter(x['kind']for x in B['published'])),'local_tag_records_verified':len(T['tags']),'new_archive_tags':len(newtags),'reviewed_source_paths_verified':sum(x['reviewed_paths_verified']for x in C['working_snapshots']),'staged_trees_verified':len(C['staged_snapshots']),'root_documents_archived':len(doc['files']),'ops_missing_branch_repaired':{'remote':'https://github.com/MatrixCoreX/PureEOS.git','branch':'ops/relay2-eligibility-20261111','repaired_head':'55dfdcd1d0f8b12f4d87a653f1cbfbe9b280cda2','reason':'origin resolves to a local repository; previous local pushes did not publish this branch to GitHub. Explicit publish remote used.'},'original_sources_branches_and_indexes_preserved_before_document_commit':True,'expected_followup':'Only the clean Ops worktree receives the audit/research/root-document archival commit. Historical original worktrees and staged states stay untouched.','excluded_runtime_configuration_paths':['PureEOS/config-dir/config.ini','PureEOS-v1-hardening/config-dir/config.ini'],'clean_official_CDT_dependencies':'LLVM/Clang/LLD clean detached heads exactly match official GitHub main; no upstream pushes.','no_build_chain_finance_server_phone_or_subagent_actions':True}
(A/'RESULT.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n');(D/'PUBLICATION_PLAN_RESULT.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
lines=['# Git upload audit / Git 上传核对清单','','All refs below were checked directly against GitHub. Historical work is archived, unmerged and unqualified; it does not change deployed versions.','', '| Repository | Type | Backup reference | Commit |','|---|---|---|---|']
for x in B['published']:
 url='https://github.com/MatrixCoreX/'+x['destination']+'/tree/'+x['commit'];lines.append('| '+x['destination']+' | '+x['kind']+' | `'+x['ref']+'` | ['+x['commit'][:12]+']('+url+') |')
lines+=['','## Archived tags','','| Repository | Original tag | Remote archival tag | Object |','|---|---|---|---|']
for x in newtags:lines.append('| '+x['destination']+' | `'+x['original_ref'].removeprefix('refs/tags/')+'` | `'+x['remote_ref'].removeprefix('refs/tags/')+'` | `'+x['object'][:12]+'` |')
(A/'ARCHIVE_INDEX.md').write_text('\n'.join(lines)+'\n')
print(json.dumps(summary,ensure_ascii=False,indent=2))
