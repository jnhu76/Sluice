# Repository Reference Snapshot — Sluice and dududu

English reference snapshot of the branch, worktree, pull-request and runtime state
collected during the retention-WSL2 evidence audit. It restates in one place the
facts of the two repository snapshots taken on 2026-10-29; the Chinese Sluice
freeze report lives in `state-freeze-report.md` alongside this file.

Collection discipline: this round was read-only. No product code was modified, no
PR was merged or closed, no branch / worktree / evidence directory was deleted,
and no force push or destructive git command was run. The round's only writes were
files under this report directory (`state-freeze-report.md`, seven receipt files
under `receipts/`) and this document.

## Overview

| Item | Value |
| --- | --- |
| Snapshot captured (UTC) | `Thu Oct 29 14:26:50 UTC 2026` — output of `date -u`, run while writing this document |
| Sluice default branch | `master` = `origin/master` @ `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` |
| dududu default branch | `main` = `origin/main` @ `12a8712eb5ff5f48e570b49b1e01ad5821837dca` |
| E5 evidence directory | `/home/jnhu/sluice-593-research/evidence` — accessible = **false** |

E5 directory accessibility, re-verified while writing this document:

```
$ ls -ld /home/jnhu/sluice-593-research/evidence
ls: cannot access '/home/jnhu/sluice-593-research/evidence': No such file or directory
$ ls -ld /home/jnhu
ls: cannot access '/home/jnhu': No such file or directory
```

The parent path `/home/jnhu` does not exist on this host, so the #594 E5 evidence
directory is not reachable from this account.

UNKNOWN items: none. Every field required by both snapshots was collected with
receipts or re-verified while writing this document; nothing is missing.

## Sluice (github.com/jnhu76/Sluice)

Local clone: `/home/hoo/Projects/Sluice`; sole remote `origin` =
`git@github.com:jnhu76/Sluice.git` (receipts/04-default-branch-and-remote.txt).

### Default branch

`origin/HEAD` is a symbolic reference to `refs/remotes/origin/master`;
`origin/master` = `master` @ `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
(tip subject: "perf(direct): P2C1 read compositions run prevalidated
register-resident steps (#608) (#612)").

### Remote branches (8 refs under `refs/remotes`)

`git for-each-ref refs/remotes` returned exactly the following 8 references — a
complete enumeration, not a partial inference (receipts/01-remote-refs.txt):

| Ref | SHA | Note |
| --- | --- | --- |
| origin/HEAD | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | symbolic → origin/master (SHA row listed as resolved) |
| origin/master | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | default branch |
| origin/backup/pr-601-original-head-e4bb8833 | `e4bb8833699d0ca5dee191883f3fec6d41b221b2` | original-head backup of MERGED PR #601 |
| origin/backup/pr-612-original-head-70d48f5e | `70d48f5eb55a664544b5160cd4675d32150bb21e` | original-head backup of MERGED PR #612 |
| origin/research/575-native-e5-atomic-rmw | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` | head of CLOSED, unmerged PR #584 |
| origin/research/593-key-path | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | head of OPEN PR #594 (only active work) |
| origin/research/product-integration-records | `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` | no headRefName or SHA match among the 376 PRs (grep count 0) |
| origin/research/r6-ac1-vectored-semantics | `999b5305ae7d413a2a71acde926f8c6cfef6876a` | head of CLOSED, unmerged PR #611 |

The branch↔PR cross-validation above was performed by grepping the 376-row PR
table (receipts/pr-list-full.tsv); the product-integration-records zero-match was
re-verified against receipts/pr-list-full.json while writing this document.

### Local branches

`git branch -vv` shows a single local branch (receipts/02-branch-vv.txt):

| Branch | SHA | Upstream | Ahead/behind |
| --- | --- | --- | --- |
| master | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | origin/master | none (in sync) |

### Worktrees

| Path | Branch | HEAD | Dirty |
| --- | --- | --- | --- |
| /home/hoo/Projects/Sluice | refs/heads/master | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | No — `git status --porcelain` empty |

This is the only registered worktree; no other agent holds an execution worktree,
and at Sluice-snapshot time /tmp contained neither a `sluice-*` nor a
`dududu-audit` directory (receipts/03-worktree-list.txt,
receipts/05-running-processes.txt; the dududu clone below was created afterwards).

### Audit target pool (7 entries)

Pool = remote branches minus `origin/master` and `origin/HEAD` (6 remain) ∪ local
branches. The 6 remote SHAs are mutually distinct and no local branch shares one,
so each is listed once with an empty `alsoLocal`; local `master` carries the same
SHA as the excluded `origin/master` (the defaultSha) but, with that ref out of the
pool, has no merge counterpart and is listed as its own entry.

| # | Branch | SHA | alsoLocal |
| --- | --- | --- | --- |
| 1 | master (local) | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | (none) |
| 2 | origin/backup/pr-601-original-head-e4bb8833 | `e4bb8833699d0ca5dee191883f3fec6d41b221b2` | (none) |
| 3 | origin/backup/pr-612-original-head-70d48f5e | `70d48f5eb55a664544b5160cd4675d32150bb21e` | (none) |
| 4 | origin/research/575-native-e5-atomic-rmw | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` | (none) |
| 5 | origin/research/593-key-path | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | (none) |
| 6 | origin/research/product-integration-records | `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35` | (none) |
| 7 | origin/research/r6-ac1-vectored-semantics | `999b5305ae7d413a2a71acde926f8c6cfef6876a` | (none) |

### Pull requests — full table (376 total: 361 MERGED / 14 CLOSED / 1 OPEN)

Collection: `gh pr list --state all --limit 250` returned exactly the 250 cap, so
the query was re-run with `--limit 1000`, returning 376 PRs (below the limit; no
truncation). Distinct numbers: 376, spanning #1–#613 (missing numbers are issues;
min #1, max #613). The only OPEN PR is #594 `research/593-key-path` @
`2714aa65fe4837d30f5fa3fcdb27db38c23b7136` → master. The 14 CLOSED (unmerged)
PRs are #58, #77, #317, #318, #319, #320, #322, #323, #354, #445, #535, #570, #584,
and #611. Receipts: receipts/pr-list-full.json (raw gh JSON) and
receipts/pr-list-full.tsv (full tab-separated table). The table below is generated
from receipts/pr-list-full.json; its row count, state distribution and six spot
entries (#1, #594, #601, #611, #612, #613) were verified against the audit
snapshot data while writing this document.

| # | State | Head → Base | Head SHA | Title | URL |
| --- | --- | --- | --- | --- | --- |
| 1 | MERGED | sync-runtime → master | `14bf131a0dafb74e240e2c671f6ca7f84c481a9b` | feat: sync runtime — blocking I/O pool, positional ops, TLA+ proof, benchmarks | https://github.com/jnhu76/Sluice/pull/1 |
| 2 | MERGED | feat/async-runtime → master | `422036cd53deb1cf8f26b9cae3c2fc524d45fd73` | docs(async): resolve frontier phase identity collision | https://github.com/jnhu76/Sluice/pull/2 |
| 3 | MERGED | fix/e7-t2-contract-corrective → master | `1ccbfe0c56e99c3f1445ac775b49de853d34cab1` | test(async): realign E7-T2 with E8 worker migration contract | https://github.com/jnhu76/Sluice/pull/3 |
| 4 | MERGED | feat/cpp-static-1-tsa → master | `8fe0738833494815fb9ffa727e27be4fb86ceb78` | async: CPP-STATIC-1 TSA pilot + statically exposed global lock authority corrective | https://github.com/jnhu76/Sluice/pull/4 |
| 5 | MERGED | feat/e11-arch-recon → master | `1d4b4c9c615bdc1c7a3a69498c59d13aa66dbc2b` | feat(async): E11 architecture recon — deadline-aware wait timers | https://github.com/jnhu76/Sluice/pull/5 |
| 6 | MERGED | e12-async-synchronization-primitives → master | `5b3017893bc657dd5057411f92db6f4689b9d864` | feat(async): E12-A Event synchronization primitive (CLOSED) | https://github.com/jnhu76/Sluice/pull/6 |
| 7 | MERGED | e12-b-audit → master | `0592e5cbffbf5ed0b2104c35eb4915e132a4b56c` | E12-B Semaphore preparation corrective — policy register + corrected permit model + Conclusion A seam proof + safety-only formal gate | https://github.com/jnhu76/Sluice/pull/7 |
| 8 | MERGED | feat/e12-C-async-mutex → master | `5593b186b54980f1dadeb22df560971ac046aa3a` | feat(async): add Fiber-aware AsyncMutex with direct FIFO handoff | https://github.com/jnhu76/Sluice/pull/8 |
| 9 | MERGED | feat/e12-D-condition-formal → master | `85a508dfcb3d538b07eb3a90c37a58df329c1751` | feat(async): E12-D AsyncCondition -- formal verification, implementation, and test suite | https://github.com/jnhu76/Sluice/pull/9 |
| 10 | MERGED | e12-e → master | `9c7573b9ddd20718f38594fa046306c0b7de8435` | docs(async): E12-E Corrective-2 integration architecture | https://github.com/jnhu76/Sluice/pull/10 |
| 11 | MERGED | async-mutex-nothrow → master | `e79f088fbc0449b7da374f797049885533e610a0` | docs(async): async mutex nothrow authority and TLC runner | https://github.com/jnhu76/Sluice/pull/11 |
| 12 | MERGED | e12-e-queue-production-impl → master | `b8269fdc8464b12038b2e8e2e434c98e31de2d6c` | feat(async): E12-E Queue production implementation (AsyncQueue<T>) — PASS | https://github.com/jnhu76/Sluice/pull/12 |
| 13 | MERGED | audit/e10-e12-api-semantic-closure → master | `71a4f35cea124f8e48bbab48a61b7dddbdcc7cf9` | E10-E12 Async/Sync API & Semantic Closure + Corrective-2 (review findings addressed) | https://github.com/jnhu76/Sluice/pull/13 |
| 14 | MERGED | docs/e12-independent-review-closure → master | `d2811c17222a60384a8b1c002ac2288bcbdbc028` | docs(async): close E12-B/C/D and authorize E12-G after independent reviews | https://github.com/jnhu76/Sluice/pull/14 |
| 15 | MERGED | build/xmake-sanitizer-policy-fix → master | `f4b1dfa20381a9f1d8dd9df5671c7b81f2387edf` | build(async): replace deprecated mode.asan/tsan/ubsan with set_policy | https://github.com/jnhu76/Sluice/pull/15 |
| 16 | MERGED | feature/E13-preparation → master | `38fa0cd372bfb0232ea62332b2ccd9120326aa0f` | E13: Select multi-wait preparation design (Event + Timer first scope) | https://github.com/jnhu76/Sluice/pull/16 |
| 17 | MERGED | feat/e13-select-formal-model-core → master | `845a9bdbeccc57ec193d29e3c25d9be2f4a6b721` | E13: formal specs, verification tools, and design docs for select-formal-model-core | https://github.com/jnhu76/Sluice/pull/17 |
| 18 | MERGED | feat/e13-select-formal-model-safety → master | `ce0d66005af61f7e9e93ce7e0f021c18ef14358a` | formal(e13): layered safety suite — invariants, negative models, non-vacuity, verifier (#18) | https://github.com/jnhu76/Sluice/pull/18 |
| 19 | MERGED | feat/e13-select-production-preparation → master | `ca07f78b09b41134df62a46be93a5260cf0f32f2` | E13: Select production preparation — architecture, adapters, locking, test plan (#19) | https://github.com/jnhu76/Sluice/pull/19 |
| 20 | MERGED | e13-select-p1 → master | `bca17db2f80f88fea1024cedf4b45b8a0f462c6b` | E13 P1-P6: Select types, admission, suspension, and publication | https://github.com/jnhu76/Sluice/pull/20 |
| 21 | MERGED | fix/e13-select-review-followup → master | `8cfa6dfaf63197311ba74818604ca1f218ceca5e` | fix(e13): address PR #20 review — drop redundant remove_cvref in select() requires | https://github.com/jnhu76/Sluice/pull/21 |
| 22 | MERGED | e13-select-p7-registration-rollback → master | `46e98ffc9633d18c63ecf00d2a3c79495eb397d4` | async(e13): P7 Select registration rollback + destruction closure | https://github.com/jnhu76/Sluice/pull/22 |
| 23 | MERGED | chore/semantic-test-names → master | `6b343ab37a9c0b3e0d046d324dc4309d1bd2a33b` | chore: rename tests/identifiers by behavior (drop e<digits>_ prefix) | https://github.com/jnhu76/Sluice/pull/23 |
| 24 | MERGED | docs/add-agents-md → master | `1f07e2842f2f144c5fc1c025b03d9c5f7c2c1d44` | docs: add AGENTS.md repository operating contract | https://github.com/jnhu76/Sluice/pull/24 |
| 25 | MERGED | fix/security-findings → master | `5548a83388f8aa541d9fecc3be2cd2db985269a7` | fix(security): address security review findings (WAL, I/O validation, io_uring, fiber, hardening) | https://github.com/jnhu76/Sluice/pull/25 |
| 26 | MERGED | fix/security-findings → master | `47c9643df5b1a8960235c02b6bd5da50167b9c41` | chore: update .gitignore | https://github.com/jnhu76/Sluice/pull/26 |
| 27 | MERGED | e12f-async-rwlock → master | `eacd33ae95412a317103e7973d31bb2fcb6e3dc3` | feat(async): add E12-F AsyncRwLock | https://github.com/jnhu76/Sluice/pull/27 |
| 28 | MERGED | design/e12-g-terminal-audit → master | `698543d943b33b2091647400cc79ce427432313a` | docs(async): E12-G cross-primitive terminal-resolution audit | https://github.com/jnhu76/Sluice/pull/28 |
| 29 | MERGED | impl/e14-threaded-evented-parity → master | `5661f195f87b5c106a9d38cf1ee98465161f7575` | E14: Threaded/Evented semantic parity production implementation | https://github.com/jnhu76/Sluice/pull/29 |
| 30 | MERGED | fix/e14-docs-and-script-refs → master | `783fb447460464a63bf1beadb17439ffed1ff66a` | docs: fix stale script ref, deduplicate closeout, mark preparation archived | https://github.com/jnhu76/Sluice/pull/30 |
| 31 | MERGED | refactor/split-xmake-lua → master | `9009ef0a7544dc9c0f802328a7df22181adcf135` | refactor: split xmake.lua into focused sub-files under xmake/ | https://github.com/jnhu76/Sluice/pull/31 |
| 32 | MERGED | fix/e15-runtime-foundation-blockers → master | `a02faaf7b0a436b479c9523ab48eba0579fd07ff` | fix: E15 runtime foundation corrective — Result/Context/Batch/Group lifecycle and semantics | https://github.com/jnhu76/Sluice/pull/32 |
| 33 | MERGED | docs/documentation-architecture-refactor → master | `cee0c4907506b5cab16f5c7f7c75c16b37d9cb83` | docs: reorganize documentation into structured subdirectories | https://github.com/jnhu76/Sluice/pull/33 |
| 34 | MERGED | docs/documentation-corrective-v2 → master | `261793d76a67018fc3f398f454120ad663631bf4` | docs: documentation corrective v2 — classification, architecture, and truth repair | https://github.com/jnhu76/Sluice/pull/34 |
| 35 | MERGED | design/e16-application-runtime → master | `94544b19b4b8fb550966f44fe686aed45128c5cf` | fix(e16): redesign application runtime architecture per independent review | https://github.com/jnhu76/Sluice/pull/35 |
| 36 | MERGED | test/fuzz-foundation → master | `648ecea5142193cea4bb251f4fc1fd047a743731` | test: fuzz foundation with deterministic mutation proof (SLUICE-FUZZ-001) | https://github.com/jnhu76/Sluice/pull/36 |
| 37 | MERGED | chore/rename-domain-identifiers → master | `140b0716fc62850154978b5f5a929026681a1381` | chore: rename domain identifiers (e+/p+/n+ → descriptive names) | https://github.com/jnhu76/Sluice/pull/37 |
| 38 | MERGED | fix/copy-fuzz-crash → master | `cd14a585467b22f7017f4d093f08bfbadbdca10b` | fix: correct copy_all_fault_fuzz oracle guards for two pre-existing false-positive crashes | https://github.com/jnhu76/Sluice/pull/38 |
| 39 | MERGED | docs/add-sluice-roadmap → master | `88efe1f10df967700a1c970153fe203bb186eb6f` | docs: add Sluice roadmap | https://github.com/jnhu76/Sluice/pull/39 |
| 40 | MERGED | docs/runtime-truth-sync-and-acceptance → master | `0a78f8c215de29bd00fc777292d9e579d9c4227b` | docs: correct E16 Application Runtime status — design only, not implemented | https://github.com/jnhu76/Sluice/pull/40 |
| 41 | MERGED | fix/group-transactional-evented-admission → master | `0edb80f6013d60a93196a5507a3ef6c4c95d0717` | fix(group): make evented admission transactionally exception-safe | https://github.com/jnhu76/Sluice/pull/41 |
| 42 | MERGED | chore/formal-verification-system → master | `db631865998e94e1bd166581fc5718db1d5f689d` | chore(formal): unify TLA+ models, tooling, and verification gates | https://github.com/jnhu76/Sluice/pull/42 |
| 43 | MERGED | fix/e12-queue-model-b → master | `727f91bfc2efdbb3bfe5609f6f9491f2927aad4b` | fix(formal): correct E12 Queue close-model sentinel + add reachability gates | https://github.com/jnhu76/Sluice/pull/43 |
| 44 | MERGED | feat/e16-application-runtime-lifecycle-model → master | `729d0b58fe7dbb35bfab442cc585d65f8c313879` | feat(async): implement E16 ApplicationRuntime lifecycle layer | https://github.com/jnhu76/Sluice/pull/44 |
| 46 | MERGED | fix/e16-post-merge-corrective-1 → master | `cfac7edfbbfc9588a1167f889a8d05be4c900de6` | fix(async): close E16 terminal resource and identity authority gaps | https://github.com/jnhu76/Sluice/pull/46 |
| 48 | MERGED | diag/issue-47-multi-worker-crash → master | `48505e519744a286051434b54383683f039b5c24` | fix(async): close multi-worker suspend-before-switch context race | https://github.com/jnhu76/Sluice/pull/48 |
| 49 | MERGED | feat/m1-sluice-copy-version-a → master | `c620746606ec76314fcb736635b28d974a19a912` | feat(apps): add sluice-copy and Runtime task Completion wait | https://github.com/jnhu76/Sluice/pull/49 |
| 51 | MERGED | fix/issue-50-workers-topology-race → master | `81a1134a45ee506cfb2a6f3c5d9994229474a877` | fix(async): synchronize Scheduler worker topology growth | https://github.com/jnhu76/Sluice/pull/51 |
| 53 | MERGED | feat/m1-sluice-copy-version-a → master | `0acd0edfd399300ffc380c27fb0b2178de2345f7` | fix(m1): Completion publication and sluice-copy correctness | https://github.com/jnhu76/Sluice/pull/53 |
| 54 | MERGED | test/overnight-local-gate → master | `b9a4a871abcb79b409fb745841e6f8ee7014be05` | fix(overnight): resolve timeout monotonic clock bug, fix self-test assertions, add unit tests | https://github.com/jnhu76/Sluice/pull/54 |
| 55 | MERGED | fix/hardening-flaky-watchdog-fallthrough → master | `e5a1848ef60343ad844e5926e8b0c4778c3ef426` | fix(hardening): root-cause + repair two debug-soak flaky failures (T32, wqtopo) | https://github.com/jnhu76/Sluice/pull/55 |
| 57 | MERGED | feat/hardening-heartbeat → master | `b0f35157c39a35e827e734c859955b1c70b89db4` | feat(hardening): add heartbeat observability for long-running commands | https://github.com/jnhu76/Sluice/pull/57 |
| 58 | CLOSED | feat/version-b-contract-tests → master | `fde712ce71e72cbf4072c3863d5e5605cfa137d7` | test(copy): ScriptedAsyncBackend + Version B pipeline contract tests | https://github.com/jnhu76/Sluice/pull/58 |
| 59 | MERGED | feat/version-b-pipeline → master | `19657ac36f9d640ad8f290407949e57b04a8da0d` | feat(copy): bounded reusable-buffer pipeline (Version B) | https://github.com/jnhu76/Sluice/pull/59 |
| 60 | MERGED | docs/architecture-compliance-gate → master | `bf22c4d62ad7af8412842ff53d87190ab242f38f` | docs(architecture): audit, compliance gate, and governance framework | https://github.com/jnhu76/Sluice/pull/60 |
| 61 | MERGED | fix/explicit-io-completion-authority → master | `5f75a5c243c86202dca75299f461f78bf7567e29` | fix(async): enforce explicit Completion publication authority (P0-03, P1-03) | https://github.com/jnhu76/Sluice/pull/61 |
| 62 | MERGED | agent/explicit-io-request-contract → master | `88f2d0311eb94b95b591d46530d42de8d336b7d9` | docs(adr): define unified Explicit I/O Request Contract | https://github.com/jnhu76/Sluice/pull/62 |
| 63 | MERGED | feat/bounded-request-slot-reference → master | `5c9d47346b501b822310031f1005154eddc3ca46` | feat(async): add bounded RequestKey / RequestSlot reference lifecycle (Phase B) | https://github.com/jnhu76/Sluice/pull/63 |
| 64 | MERGED | feat/phase-e-bounded-threadpool-explicit-io → master | `f71f9903711aea2bbc7286fafde50c3558bd87ad` | feat(async): migrate ThreadPoolBackend to bounded explicit-I/O (Phase E) | https://github.com/jnhu76/Sluice/pull/64 |
| 65 | MERGED | docs/phase-e-post-merge-ledger-corrective → master | `4d38eb9afd8f94ce49a3a9937d72d5110d9badcb` | docs(architecture): post-merge Phase E ledger corrective on master head a8178d8 | https://github.com/jnhu76/Sluice/pull/65 |
| 66 | MERGED | test/phase-c-explicit-io-conformance-gate → master | `43b2944957b5d9cecd28153b0a332447ca2284b8` | fix(async): explicit-I/O backend conformance gate (Phase C1) + issue #67 wait/drain/stats corrective | https://github.com/jnhu76/Sluice/pull/66 |
| 69 | MERGED | test/phase-c2-capacity-admission-rejection → master | `b193f85b52e990afed1505862ed74bf3b1a22eb0` | test(async): Phase C2a capacity/admission/rejection/accounting conformance (issue #68) | https://github.com/jnhu76/Sluice/pull/69 |
| 70 | MERGED | test/phase-c2b-generation-stale-cancel-matrix → master | `667975b1eb5d3178a8ef8d097004c9aba800eb11` | test(async): Phase C2b generation/stale-key/cancel-winner/identity-reap conformance (Issue #68 rows 3,4a,5-8) | https://github.com/jnhu76/Sluice/pull/70 |
| 71 | MERGED | test/phase-c2c-borrow-waiter-delivery-lease → master | `2b303899ee6ddae13b27286e873ebe72ada148da` | test(async): Phase C2c waiter/borrow/delivery-lease conformance (Issue #68 rows 11,12a,13,14a) | https://github.com/jnhu76/Sluice/pull/71 |
| 72 | MERGED | test/phase-c2d-failure-injection-accepted-terminal → master | `86ed4fd75fad165970266fd96488f8ac79f53d2a` | test(async): complete C2d deterministic failure-injection gate | https://github.com/jnhu76/Sluice/pull/72 |
| 73 | MERGED | test/phase-c2e-close-drain-destruction → master | `25893ce3561f59586c7143a4539dd60c7571321a` | test(async): complete C2e close-drain lifecycle gate | https://github.com/jnhu76/Sluice/pull/73 |
| 76 | MERGED | docs/phase-d-uring-migration-plan → master | `825c47b28d3680eda887b237f2fc6901ed897787` | docs(async): Phase D0 Uring migration audit & PR decomposition (plan) | https://github.com/jnhu76/Sluice/pull/76 |
| 77 | CLOSED | feat/phase-d1-uring-private-ring-requestarena → docs/phase-d-uring-migration-plan | `646dce8305d79a814c1a2ac08870f990e4ce06f9` | feat(async): D1 — UringAsyncBackend private-ring RequestArena migration | https://github.com/jnhu76/Sluice/pull/77 |
| 78 | MERGED | feat/phase-d1-uring-private-ring-requestarena → master | `235162f823e016bbd06358acb9ec59faea20340f` | feat(async): D1 — UringAsyncBackend private-ring RequestArena migration | https://github.com/jnhu76/Sluice/pull/78 |
| 79 | MERGED | fix/uring-internal-testing-guard → master | `d9b27fc9e3264204dd57bb3431ddd4c36244591e` | fix(async): unify uring internal test guard | https://github.com/jnhu76/Sluice/pull/79 |
| 80 | MERGED | test/phase-d2-uring-failure-noalloc → master | `a5406b45698c733101d61d9b17079e49e33f083c` | test(async): close Uring Phase D2 failure/no-allocation conformance gap | https://github.com/jnhu76/Sluice/pull/80 |
| 82 | MERGED | chore/add-local-pre-push-gate → master | `d6281d8b20706540f03c16d23cdc38e2e61061e8` | chore(dev): add deterministic local pre-push gate | https://github.com/jnhu76/Sluice/pull/82 |
| 83 | MERGED | test/phase-d3-uring-identity-waiter-conformance → master | `91f72245faa5aa844b274c9ac0086c2e3b4cda7c` | D3: Uring C2b/C2c identity, cancel, borrow and waiter integration evidence | https://github.com/jnhu76/Sluice/pull/83 |
| 84 | MERGED | feat/phase-d4-uring-wait-close-drain → master | `66b2a98ba6168743e356550de0e27fed386cbf49` | Phase D4: Uring wait source, close/drain proof, KernelIo gate lift | https://github.com/jnhu76/Sluice/pull/84 |
| 87 | MERGED | test/include-unistd-async-stats-wait-race → master | `9bfa22707a79277ab637424a6bb3df89c1b9b73a` | tests: include unistd.h explicitly in async stats wait race test | https://github.com/jnhu76/Sluice/pull/87 |
| 88 | MERGED | fix/queue-callguard-lifecycle-sync → master | `500b688e3d7c36526ce729cc73fcf72838902793` | fix(async): resolve Queue lifecycle race and ThreadPool gate flake (#86-A/B) | https://github.com/jnhu76/Sluice/pull/88 |
| 91 | MERGED | fix/queue-snapshot-lifecycle-gate → master | `8a5ac5cddf2a1c29b794bf155ecd7f9309cdcbb9` | fix(queue): snapshot lifecycle compliance + const-correct state_mtx_ (#90) | https://github.com/jnhu76/Sluice/pull/91 |
| 93 | MERGED | fix/threadpool-resume-bidirectional-92 → master | `7d2a4b42541f281477b3ee076e43056763b1fbc5` | fix(threadpool): complete bidirectional pause-gate resume protocol (#92) | https://github.com/jnhu76/Sluice/pull/93 |
| 102 | MERGED | docs/zig-io-rebaseline-post-phase-d → master | `fea5d26c4878cba801a239780e4d54e68549cd8d` | docs(architecture): re-baseline Zig std.Io map + reconcile post-Phase-D state + cancel rearm fix (corrective pass) | https://github.com/jnhu76/Sluice/pull/102 |
| 104 | MERGED | audit/cpp20-legacy-hygiene → master | `e3276065480f6ba4dd7cc9c99af5a257704a82b8` | chore(cpp): audit C++20 baseline and legacy compatibility debt | https://github.com/jnhu76/Sluice/pull/104 |
| 105 | MERGED | feat/phase-f1-scheduler-ready-sink → master | `cba1565d82a5c87f101247245bdcf546d43f5f24` | Phase F1 (issue #98): Scheduler consumes identity-bearing reap via Scheduler-owned ReadySink | https://github.com/jnhu76/Sluice/pull/105 |
| 106 | MERGED | feat/phase-f-remaining → master | `537e6a0907cb4d29952289b962d1f3654e5a8a5c` | Phase F (issue #98): F2 BatchResult origin + F3 public RequestHandle + F4 closeout | https://github.com/jnhu76/Sluice/pull/106 |
| 107 | MERGED | fix/audit-94-101-closeout → master | `e9305ae874ab9723e503e97a8401cdf3af33bdbd` | Audit #94–#101 final closeout: #99 design decision, #100 formal gap, #101 watchdog model fix | https://github.com/jnhu76/Sluice/pull/107 |
| 108 | MERGED | feat/phase-g-backend-progress-wake → master | `5bfb2601b29ccd3ad5857702ff47396dd9a64104` | Phase G: backend-ready progress wake — G1 BLOCKED (park-window invariant violation); forensic instrumentation + regression | https://github.com/jnhu76/Sluice/pull/108 |
| 109 | MERGED | closeout/phase-g-foundation-freeze → master | `7fcb0b9f415dad94ffd570812e6be22e258dd753` | Phase G closeout: causal wake proof, real-uring validation, formal reconciliation, foundation freeze | https://github.com/jnhu76/Sluice/pull/109 |
| 114 | MERGED | post-freeze/r0-r1-structural-hygiene → master | `4c056c694793b6942f3dfbd46857b4acdc465792` | Post-freeze R0/R1 structural hygiene: scheduler concept-TU split, Zig std.Io alignment, first workload design | https://github.com/jnhu76/Sluice/pull/114 |
| 117 | MERGED | ci/watchdog-survivor-forensics → master | `cbcce9f45690b642f301454f8c0df2949c86219b` | ci(watchdog): survivor forensics — process tree, ppid/state/elapsed, self-ancestry guard | https://github.com/jnhu76/Sluice/pull/117 |
| 118 | MERGED | fix/issue-116-runtime-reentry-liveness → master | `22de0ee94b29395430b772b82a32a7f4e7d29771` | fix(issue-116): runtime/scheduler lost re-entry liveness — ROOT_CAUSE_PROVEN_AND_FIXED | https://github.com/jnhu76/Sluice/pull/118 |
| 119 | MERGED | fix/issue-110-test-gate-generation → master | `310e5c56eb0a342cf54a3a4a4bba6dd8258896ad` | fix(issue-110): generation-scoped dequeue-gate handshake for the scheme-B race loop (test-only; production byte-identical) | https://github.com/jnhu76/Sluice/pull/119 |
| 120 | MERGED | fix/issue-115-runnable-publication-wake → master | `71f347e03dee0236c70e6a18a9e0f7c81f9a44e7` | fix(issue-115): spawn/spawn_on publish the Scheduler wake obligation | https://github.com/jnhu76/Sluice/pull/120 |
| 121 | MERGED | agent/ci-debug-release → master | `e15179c1a1b063c9fb53dfc931c3c515e38a2288` | ci: simplify Debug gate and add fast Linux Release | https://github.com/jnhu76/Sluice/pull/121 |
| 122 | MERGED | apps/file-tools-track → master | `259179c38b9417c857dfd1a00874a3ebd76639a7` | apps: file-tools track — sluice-copy Version C, sluice-hash, sluice-grep, sluice-tail | https://github.com/jnhu76/Sluice/pull/122 |
| 124 | MERGED | docs/reorientation → master | `180556279d3c4f294a5e2a6c109d188cc5b39f4d` | docs: documentation & README information-architecture reorientation | https://github.com/jnhu76/Sluice/pull/124 |
| 125 | MERGED | audit/formal-cpp-tla-realignment → master | `6ed6659ad38fd96929e21297c21f45cea0da62b9` | formal: C++ ↔ TLA+ audit — RequestArena debt closed, 19/19 suites green, harness hardening | https://github.com/jnhu76/Sluice/pull/125 |
| 126 | MERGED | perf/performance-attribution → master | `94ab95314d5a3b9894b6b923e201f633d4d13840` | perf: performance attribution framework + sluice-grep round-1 optimization | https://github.com/jnhu76/Sluice/pull/126 |
| 128 | MERGED | fix/issue-123-phase-g-closeout-flake → master | `2dd8c03663f7f390c0e70fc77a84542354ece924` | fix(test): phase_g_closeout parallel-Debug flake — blocking handshakes + case watchdog (#123) | https://github.com/jnhu76/Sluice/pull/128 |
| 130 | MERGED | test/issue-129-uring-closeout-handshakes → master | `894aae64566c0daf684e386ae86bc29ccca463a0` | test(phase-g-uring): migrate closeout observations to blocking handshakes + case watchdog (#129) | https://github.com/jnhu76/Sluice/pull/130 |
| 145 | MERGED | agent/internal-test-control-plane → master | `64655c2d865efe2a892b134157dbd272eaf21ce4` | refactor(internal-testing): move test control plane out of installed production headers (C4, #135) | https://github.com/jnhu76/Sluice/pull/145 |
| 146 | MERGED | agent/runtime-await-operation-helpers → master | `dfc4aa040e73941f365029c50c58a19c54042e94` | feat(async): runtime await-style op helpers + task-result bridge (C7, #135) | https://github.com/jnhu76/Sluice/pull/146 |
| 147 | MERGED | agent/failure-model-policy → master | `0a803cc4e969356ec9d78c201e89fa85fcb39396` | docs(policy): failure-response model T1-T7 + changed-lines assert-hygiene gate (#135 C8, #144) | https://github.com/jnhu76/Sluice/pull/147 |
| 148 | MERGED | agent/failure-model-high-risk-fixes → master | `01328812304ea0686abc73f0436387a9a1bcb6f6` | fix(failure-model): high-risk assert sites reclassified T5/L9/T3 (#135 Case B) | https://github.com/jnhu76/Sluice/pull/148 |
| 149 | MERGED | agent/async-lifetime-contract-failfast → master | `4f076985550051e5994cef8372688ea5c08062b1` | feat(async): primitive destruction fail-fast unified Debug+Release (ADR-async-primitive-lifetime-failfast, #135) | https://github.com/jnhu76/Sluice/pull/149 |
| 152 | MERGED | agent/issue141-fileop001-nofollow → master | `5a927f48a522beb0a47bc2cec045f385029d4b51` | fix(sluice-copy): reject final-destination symlink in --no-atomic open (FILEOP-001, #141) | https://github.com/jnhu76/Sluice/pull/152 |
| 154 | MERGED | fix/issue-142-dir-fsync-eintr → master | `6dc0dd57bc7b2baa7a6f59e18b1ad1aac9065fd7` | fix(copy): retry directory fsync on EINTR | https://github.com/jnhu76/Sluice/pull/154 |
| 155 | MERGED | docs/issue-139-request-lifecycle → master | `893fcc8fa519df88935cb98f26ee83a691cf173c` | docs(async): request lifecycle locality walkthrough (closes #139) | https://github.com/jnhu76/Sluice/pull/155 |
| 156 | MERGED | fix/issue-143-close-error-reporting → master | `fa7f525e14afb7b29e8e1c9fb9f10aae130dbd0d` | fix(file): make close(2) failure observable via explicit close() (closes #143) | https://github.com/jnhu76/Sluice/pull/156 |
| 157 | MERGED | design/issue-137-submission-transaction → master | `bbd4860a4569e20dbedef84d29e1459d30ad0687` | design(async): centralized submission transaction design for #137 (PROPOSED — implementation still blocked) | https://github.com/jnhu76/Sluice/pull/157 |
| 158 | MERGED | fix/issue-151-uring-baseline-t5 → master | `42479b7337e870b6ea961cb493c9d00fbe5122c1` | test(phase-g-uring): type the 6 remaining direct ws->snapshot() baselines (T5) | https://github.com/jnhu76/Sluice/pull/158 |
| 159 | MERGED | refactor/issue-137-submission-transaction-impl → master | `7f74a655b51063b0f53907ca953f170bd0cfbcbb` | refactor(async): centralize explicit-I/O submission transaction authority | https://github.com/jnhu76/Sluice/pull/159 |
| 160 | MERGED | fix/issue-137-review-corrections → master | `012a457ab96d010208a31e29dad05e43df82bcee` | fix(async): #159 review corrections — Stage-0 ring gate, lock docs, commit-error mapping, hook budget | https://github.com/jnhu76/Sluice/pull/160 |
| 165 | MERGED | chore/github-governance → master | `09afdee334fc31bafa38dae8d81b8c0c9a251567` | chore(github): standardize issue and PR governance | https://github.com/jnhu76/Sluice/pull/165 |
| 166 | MERGED | refactor/cpp-agent-skills → master | `9ac5bb290808dc40e34582a0171d44dcb09076cc` | refactor(skills): make C++ agent guidance procedural and adversarially reviewable | https://github.com/jnhu76/Sluice/pull/166 |
| 168 | MERGED | audit/162-cpp-tla-formal-closeout → master | `90e644195f1e18a0f92102a3a4bf4055ba504541` | formal+async: audit #162 — rwlock model repair (MODEL-001/002/003), model-doc verdicts, CPP-001/002 cleanup | https://github.com/jnhu76/Sluice/pull/168 |
| 173 | MERGED | formal/171a-suspend-switch-steal → master | `d64a5f9b5b151ade0fd60b847df3a6fd3cba92a5` | formal(e8): model suspend-switch steal exclusion (MODEL-007a) | https://github.com/jnhu76/Sluice/pull/173 |
| 175 | MERGED | formal/171b-wait-record-registry → master | `ce87005599699700fc298646d931dbda51bb4da4` | formal(wait-registry): model generation and delivery arbitration (MODEL-007b) | https://github.com/jnhu76/Sluice/pull/175 |
| 177 | MERGED | formal/171d-spawn-wake-epoch → master | `2e389b96929ffb930be05112a515fae6f7a63a32` | formal(scheduler): model spawn wake-epoch obligation (MODEL-007d) | https://github.com/jnhu76/Sluice/pull/177 |
| 179 | MERGED | formal/171e-worker-retire-rescue → master | `70e50cad20a0695e5b052aca057bb04adecffaea` | formal(scheduler): model worker-retire ticket rescue (MODEL-007e) | https://github.com/jnhu76/Sluice/pull/179 |
| 181 | MERGED | formal/171c-cancel-token-epoch → master | `659882ae9567d9719ebb583a17d0c52afbe3ea10` | formal(cancel): model CancelToken epoch isolation (MODEL-007c) | https://github.com/jnhu76/Sluice/pull/181 |
| 182 | MERGED | formal/169-async-mutex-negative-freshness → master | `d6b15bc837099507cb50cfbca0b6909cc72067ce` | formal(e12-async-mutex): enforce generated-negative freshness | https://github.com/jnhu76/Sluice/pull/182 |
| 183 | MERGED | cleanup/170-remove-dead-inbox → master | `228815eeadb92f63f693b00085fb2e859e4603f8` | cleanup(async): remove inert worker inbox notification surface | https://github.com/jnhu76/Sluice/pull/183 |
| 187 | MERGED | formal/e8-retire-inbox-vocabulary → master | `00c896cf84f5d8674e9ee4717da314643918562c` | formal(e8): retire unreachable inbox transport vocabulary | https://github.com/jnhu76/Sluice/pull/187 |
| 188 | MERGED | docs/formal-debt-register-rebaseline → master | `1efdd3b1611bf3e8ff3b7ab75ecf299edf7c70c5` | docs(formal): rebaseline formal debt register (issue #186) | https://github.com/jnhu76/Sluice/pull/188 |
| 190 | MERGED | formal/e9-retire-worker-quiescent → master | `d5dae4cb8adf12ff89261f599fc3ef48294b793d` | formal(e9): revive as-built worker retirement transition | https://github.com/jnhu76/Sluice/pull/190 |
| 193 | MERGED | formal/e9-generated-negatives → master | `fe57ab19895e4aea6ac5478550deebe92f13d338` | formal(e9): generate negative models with freshness gate | https://github.com/jnhu76/Sluice/pull/193 |
| 194 | MERGED | formal/e9-participant-no-progress-exit → master | `2fe5c924edb2ea91a010af7f1754b0fb1d89d473` | formal(e9): revive ParticipantNoProgressExit, complete FairRetire (#191) | https://github.com/jnhu76/Sluice/pull/194 |
| 195 | MERGED | formal/e9-reference-nonvacuity → master | `47b5547ba58c0f740ff6bb73bdd3525942695265` | formal(e9): repair SplitWait=FALSE reference-config vacuity (#185) | https://github.com/jnhu76/Sluice/pull/195 |
| 201 | MERGED | docs/v-layer-child-issue-pointers → master | `4684b076f0d769256b836cced4c92edbc1e11050` | docs(verification): point #163 V-layer roadmap at child issues #196-#200 | https://github.com/jnhu76/Sluice/pull/201 |
| 202 | MERGED | verify/v2-e9-trace-conformance → master | `c504645b2ffd28279b085b4847b1d80a7c85b668` | verify(async): #196 E9 real-C++ trace conformance pilot | https://github.com/jnhu76/Sluice/pull/202 |
| 203 | MERGED | verify/v3-completion-weak-memory → master | `4c13c0c6d98ed29aeafe8a28fb3425b83a2ac615` | verify(async): #197 bounded weak-memory pilot for Completion publication | https://github.com/jnhu76/Sluice/pull/203 |
| 204 | MERGED | verify/v5-failure-envelope → master | `cbe7b9c287e25d48ac708b7a5a77ceca38684d8f` | verify(gates): #198 machine-checkable failure-envelope matrix + validator | https://github.com/jnhu76/Sluice/pull/204 |
| 205 | MERGED | verify/v6-guarantee-cost → master | `2dce9b009b1bb9947a34bc6ea642782c2299cd32` | perf(verification): #199 measure correctness costs and sustained overload | https://github.com/jnhu76/Sluice/pull/205 |
| 206 | MERGED | verify/v7-claim-discipline → master | `283aa13ee81a9405796e46a4e488d96511a8ef22` | verify(docs): #200 enforce bounded verification claims | https://github.com/jnhu76/Sluice/pull/206 |
| 207 | MERGED | docs/issue-167-surface-separation → master | `8d53cebc772284ba967a07c9b5e61155e5eb6160` | docs: separate live user/maintainer surfaces and classify architecture evidence (#167) | https://github.com/jnhu76/Sluice/pull/207 |
| 208 | MERGED | docs/issue-167-step4-source-comments → master | `be46da0371b68a9d386b16fc3412c197e8f4cb0b` | docs: functionalize source comments and empty header stale registry (#167 Step 4) | https://github.com/jnhu76/Sluice/pull/208 |
| 209 | MERGED | docs/issue-167-step5-archive-moves → master | `dbfba3733f66b263d0013e4a41147e223c0e99a1` | docs: archive zero-consumer evidence/history records to docs/history (#167 Step 5) | https://github.com/jnhu76/Sluice/pull/209 |
| 211 | MERGED | docs/issue-167-step5b-classification → master | `492133754c125eb425e3fb59dec0b0a58eb44a6c` | docs(#167): Step 5b semantic classification of docs/design/phase-* and docs/investigations (no moves) | https://github.com/jnhu76/Sluice/pull/211 |
| 212 | MERGED | docs/issue-167-step5c-preflight → master | `1fcd9c98cd67fa089a2ad8784efe7d86e332a085` | docs(architecture): Step 5c preflight — normalize Blocked ontology to Step 5b pin model (#167) | https://github.com/jnhu76/Sluice/pull/212 |
| 213 | MERGED | docs/issue-167-step5c-safe-archive → master | `1ff06242612fbddca302a4b17f01fc244ec99d77` | docs(archive): Step 5c safe archive batch — issue-110, issue-123, phase-b (#167) | https://github.com/jnhu76/Sluice/pull/213 |
| 214 | MERGED | docs/issue-167-step5d-issue115 → master | `dda1cf1043843f48d22650c98df2b1ed0b812e85` | docs(archive): issue #115 disposition adjudication (superseded) + archive (#167 Step 5d) | https://github.com/jnhu76/Sluice/pull/214 |
| 215 | MERGED | docs/issue-167-step5e1-issue116 → master | `265963b98216d2b1d95deb89c1c6b149fea4b269` | docs(archive): Step 5e1 — issue-116 investigation to docs/history (#167) | https://github.com/jnhu76/Sluice/pull/215 |
| 216 | MERGED | docs/issue-167-step5e2-phase-e → master | `2fc67745e7e05c36164671670b1b958cefb69245` | docs(archive): Step 5e2 — phase-e frozen design to docs/history (#167) | https://github.com/jnhu76/Sluice/pull/216 |
| 217 | MERGED | docs/issue-167-step5e3-phase-f1 → master | `fca28942e2f4a473586125dee53bdb7de55fe988` | docs(archive): Step 5e3 — phase-f1 design to docs/history (#167) | https://github.com/jnhu76/Sluice/pull/217 |
| 218 | MERGED | docs/issue-167-step5e4-phase-g → master | `e82dfbc1ba8507db183c49f75fa167aef8f7966d` | docs(archive): Step 5e4 — phase-g design to docs/history (#167) | https://github.com/jnhu76/Sluice/pull/218 |
| 219 | MERGED | docs/issue-167-step5f-verification-keep → master | `359b0dabb956021fb60f93998c160b8cc62add36` | docs(verification): Step 5f — record KEEP for stable evidence identifiers (#167) | https://github.com/jnhu76/Sluice/pull/219 |
| 220 | MERGED | docs/issue-167-step5g-audit → master | `525fde532fdd887fa81c7df2b3ff5c6c8db5f05c` | docs(archive): Step 5g — final audit stale-pointer fix (#167) | https://github.com/jnhu76/Sluice/pull/220 |
| 222 | MERGED | fix/issue-210-e9-t4-failure-surfacing → master | `5325a3ed32ce5eda3d84ed59600c93ab38891f2f` | test(e9): fail-safe runner cleanup + pin T4 worker roles against the MW-S2 election race (#210) | https://github.com/jnhu76/Sluice/pull/222 |
| 224 | MERGED | research/issue-221-e1-abstraction-tax-baseline → master | `5a6d108d50e6d98d0252c3b4f7fc862bd3e694b9` | perf: add E1 explicit-I/O abstraction-tax baseline harness (#221) | https://github.com/jnhu76/Sluice/pull/224 |
| 228 | MERGED | chore/issue-227-phase0-mechanical-cleanup → master | `a4fb6cdc821a05980ccfd708df37dcc1c35bdad4` | chore: #227 phase 0 low-risk mechanical cleanup | https://github.com/jnhu76/Sluice/pull/228 |
| 230 | MERGED | docs/issue-227-readme-release-status → master | `d5c609ed277656ac844d584449c8f11fe9502fcf` | docs: correct release-status claims for v0.0.1 baseline | https://github.com/jnhu76/Sluice/pull/230 |
| 231 | MERGED | fix/issue-229-tsan-deadline-test-seam → master | `a146db1730521962dd8c32cc6215e6c0f7425420` | fix(async-test): synchronize timer state snapshots (#229) | https://github.com/jnhu76/Sluice/pull/231 |
| 232 | MERGED | docs/changelog-v001 → master | `35e123c410c232a5e87db6be4474961ab902bbcd` | docs: root CHANGELOG.md from v0.0.1 with release-draft automation | https://github.com/jnhu76/Sluice/pull/232 |
| 233 | MERGED | docs/context-orientation → master | `bf9bb6087ad21bc0c6a7d9c7d331dc95651e007e` | docs: add root CONTEXT.md orientation map | https://github.com/jnhu76/Sluice/pull/233 |
| 235 | MERGED | arch/ac-1a-minimal-observability → master | `45269ecc97d9225c4ffbf046af2517147e2006b2` | arch(observability): add minimal resource observations for AC-1a | https://github.com/jnhu76/Sluice/pull/235 |
| 236 | MERGED | research/rx1-attribution-falsification → master | `025bbec97ac19680142df7a25e5051b6e5d38633` | research(rx1): controlled attribution falsification gate | https://github.com/jnhu76/Sluice/pull/236 |
| 238 | MERGED | refactor/ac2b-ordinary-deadline-authority → master | `a7138ea4ed0831189c2be2639e8a158c965f26b3` | refactor(async): centralize ordinary deadline registration lifecycle | https://github.com/jnhu76/Sluice/pull/238 |
| 240 | MERGED | refactor/ac2c-cancellation-authority → master | `148bbfc75fdb623b15d0d1bd594f566380f7ec41` | refactor(async): centralize narrow wait cancellation authority | https://github.com/jnhu76/Sluice/pull/240 |
| 241 | MERGED | test/dst-pv1-next-runnable-seam → master | `c52303de27e86d85958392f5275a53a86daf5f3d` | test(async): add minimal deterministic runnable-choice seam | https://github.com/jnhu76/Sluice/pull/241 |
| 242 | MERGED | fix/r2-wait-lifecycle-closeout → master | `c462a7ee7a6a34e0f5fcd5db476aef90e4a33030` | fix(async): close R2 wait lifecycle after queue liveness repair | https://github.com/jnhu76/Sluice/pull/242 |
| 243 | MERGED | feat/frontend-semantic-reuse → master | `19c1bdeda7b103456630554694eefe9298d4b804` | FE campaign: prove multi-frontend semantic reuse with one wait core | https://github.com/jnhu76/Sluice/pull/243 |
| 244 | MERGED | docs/agent-contract-convergence → master | `7bceaa001a5001c6ca6c55d21313c47a2989034c` | docs: 收敛 Agent 协作契约与实现导航 | https://github.com/jnhu76/Sluice/pull/244 |
| 245 | MERGED | research/se1-hazard-corpus → master | `06137793d40a140ed6dd418ce2845c7113eb2201` | research(safety): build SE-1 async I/O lifecycle hazard corpus | https://github.com/jnhu76/Sluice/pull/245 |
| 246 | MERGED | research/se2-hazard-detection-matrix → master | `865b352991b70ca06f2c67f96faeecfd3fdc30e8` | research(safety): execute SE-2 hazard detection matrix | https://github.com/jnhu76/Sluice/pull/246 |
| 247 | MERGED | docs/archive-review-reports → master | `337d4041293df9663f415310e3bbdbd31c022583` | docs(history): archive FE/R2/SE-1/SE-2/THESIS-LEDGER review reports | https://github.com/jnhu76/Sluice/pull/247 |
| 248 | MERGED | docs/remove-archived-review-reports → master | `06543fea89b0a6407df81ea161a54ccf7e44f88f` | docs(history): remove review reports from master (restore untracked ruling) | https://github.com/jnhu76/Sluice/pull/248 |
| 252 | MERGED | research/tax0a-topology-freeze → master | `2b1d0f7e72b2a2cc874f48cc9273cfc5d39a197b` | research(tax0): freeze TAX-0A topology audit as TAX-0B preregistration (docs-only) | https://github.com/jnhu76/Sluice/pull/252 |
| 253 | MERGED | research/tax0b-exp0-capacity → master | `4e6bdb603eab458e637355519d479bf055c13c5c` | research(perf): measure TAX-0 capacity-dependent runtime tax | https://github.com/jnhu76/Sluice/pull/253 |
| 254 | MERGED | research/tax0-exp-u0-router-causality → master | `3ba1435cf7f2cf2dc24d0e3b9459cedc36f1374b` | research(perf): attribute Uring capacity tax to router scanning | https://github.com/jnhu76/Sluice/pull/254 |
| 256 | MERGED | research/tax0-router-fix-shootout → master | `b12b640134303f15cf3d6b73fa8d5162cffd7246` | research(perf): benchmark Uring router-resolution fix candidates | https://github.com/jnhu76/Sluice/pull/256 |
| 257 | MERGED | research/tax0-copy-ab1 → master | `0df66ad8fcaaae34b1f82536919ab48850caeebf` | research(tax0): COPY-AB-1 freeze — application-level copy A/B (R0 vs R1) | https://github.com/jnhu76/Sluice/pull/257 |
| 260 | MERGED | research/tax0-a2-control-plane → master | `f027a2501851d03d6f81c49dc20225d352a478e5` | research(tax0): control-plane semantic floor + causal tax attribution (TAX-0B/C/D/E) | https://github.com/jnhu76/Sluice/pull/260 |
| 261 | MERGED | perf/tax0-f01-stats-gate → master | `5204ba16d423bcd45e1f3f268125dd56df580ae2` | perf(async): gate max-outstanding sampling on stats presence (TAX-0D F01) | https://github.com/jnhu76/Sluice/pull/261 |
| 264 | MERGED | research/buf-e0-buffer-truth → master | `975e0754b366f2f9aaaf5839718a1f7727cde2b8` | research(buf-e0): #263 Phase 2 buffer truth — census, preregistered 4-arm lifecycle measurements, alignment isolation, amplifier verdict | https://github.com/jnhu76/Sluice/pull/264 |
| 266 | MERGED | research/align-e0 → master | `78fb7a02bd5ea38bec95bbb3e87c8c8222cbc9f1` | research(align-e0): #265 Phase 3 alignment threshold × size × depth × direction — preregistration, harness, WSL2-qualified + native sessions, verdict report | https://github.com/jnhu76/Sluice/pull/266 |
| 269 | MERGED | research/align-e1 → master | `24615737c0a78b36bb8aff23a05872ee824dd90b` | research(align-e1): #268 application-level chunk sweep and regime map | https://github.com/jnhu76/Sluice/pull/269 |
| 271 | MERGED | research/chunk-e0-h0 → master | `35d912e2381b2fafeb9b1f6a2b51e515b4af5a44` | research(chunk-e0): #270 host-0 chunk-size × depth sweet-spot map | https://github.com/jnhu76/Sluice/pull/271 |
| 273 | MERGED | research/rbuf-e0 → master | `c4cb8bd24ed3158f569d60fb2bef0e28a5a9a21a` | research(rbuf-e0): registered-buffer steady-state and amortization crossover | https://github.com/jnhu76/Sluice/pull/273 |
| 274 | MERGED | perf/tax0-router-r1-production → master | `45f568a04956259185015d79fee9f29b94714862` | perf(async/uring): land selected reverse router scan and measure residual tax | https://github.com/jnhu76/Sluice/pull/274 |
| 275 | MERGED | fix/sluice-copy-ready-drain-258 → master | `8e3989698cb3a14b843d1bfe3ba3773319b3dc67` | fix(sluice-copy): drain READY completions on error cleanup (#258) | https://github.com/jnhu76/Sluice/pull/275 |
| 276 | MERGED | fix/uring-equivalence-drain-262 → master | `389b0737832669e2b241051b0fa0340c8021e4fa` | test(async/uring): name the drain-stall teardown abort instead of failing silently (#262 S2) | https://github.com/jnhu76/Sluice/pull/276 |
| 278 | MERGED | research/re-h0-native-performance-closure → master | `cfdbc1722f6734061b1a7f8912d50a531a4caeef` | research(perf): RE-H0 semantic-floor residual and native performance envelope | https://github.com/jnhu76/Sluice/pull/278 |
| 280 | MERGED | research/g1-control-c0-fixed-file → master | `e86898821281f4bcbeeeb82a2e6e4cd290e2aaab` | research(control): G1-Control C0 fixed-file identity and specialization falsification | https://github.com/jnhu76/Sluice/pull/280 |
| 281 | MERGED | research/g1-control-copy-x0 → master | `e31da9a587925a13cc7a44ac2918c2051efa6e6d` | research(copy-x0): G1-Control Candidate 2 — Explicit Copy as a Legal Transformation Boundary (STOP, NO C1) | https://github.com/jnhu76/Sluice/pull/281 |
| 282 | MERGED | research/g1-control-batch-x0 → master | `3705c64b1067a53a93e98fda674eac7b4477df68` | research(batch-x0): BATCH-X0 — explicit Batch control-plane amortization falsification — STOP, NO C1 (research-only) | https://github.com/jnhu76/Sluice/pull/282 |
| 284 | MERGED | docs/g1-control-research-closure → master | `944e0232d08273f8fa18eae41d7b6cc4454a27db` | docs(research): close G1-Control strong thesis and freeze Semantic Authority verdict | https://github.com/jnhu76/Sluice/pull/284 |
| 285 | MERGED | ci/actions-node24-runtime → master | `dedc5665c86f636b92a4b89f0f1bc07e7d9c8eb1` | ci(workflows): bump actions to Node 24 runtime majors | https://github.com/jnhu76/Sluice/pull/285 |
| 287 | MERGED | research/semantic-diet-0 → master | `6b458ad84ea10bca48f063ee100ac56a5384f661` | research(semantic-diet): SEMANTIC-DIET-0 — remove speculative CopyStrategy deferred surface | https://github.com/jnhu76/Sluice/pull/287 |
| 291 | MERGED | docs/s0-docs-ia → master | `70c2cd945084e916d19e953eac7609a92cd7ff62` | docs: S0-DOCS — documentation IA, AGENTS/CONTEXT slimming, authority cleanup (#290) | https://github.com/jnhu76/Sluice/pull/291 |
| 293 | MERGED | research/safety-s0b-contract-reality → master | `b457c0f64a62ee586020020eaecaf58341826b6b` | research(safety): S0B contract ↔ correctness authority ↔ C++ reality freeze (#292) | https://github.com/jnhu76/Sluice/pull/293 |
| 295 | MERGED | research/safety-s1a-formal-coverage → master | `5348e111a94582b3bc7c2a2af8c6bd1097de6c5d` | research(safety): S1A C++ ↔ TLA+/GenMC protocol coverage and correspondence | https://github.com/jnhu76/Sluice/pull/295 |
| 297 | MERGED | research/safety-rf1-e9-startup-refinement → master | `ac4ee3815b42e522d88e46cba3af5606298294cc` | formal(e9): R-F1 worker startup population refinement (#223) | https://github.com/jnhu76/Sluice/pull/297 |
| 300 | MERGED | research/ftlr0-scip-pilot → master | `c1695ee95be96dd8e18344e7aee28047976f50dc` | research(formal): FTLR-0 SCIP pilot — formal impact recovery experiment (#299) | https://github.com/jnhu76/Sluice/pull/300 |
| 302 | MERGED | safety/fdg0-phase-a-build-truth → master | `95a8f3f238b7bf9bd38f640d480d5fa861bc3aee` | research(formal): FDG-0 Phase A — Xmake Build Truth (#298) | https://github.com/jnhu76/Sluice/pull/302 |
| 303 | MERGED | safety/fdg0-phase-b-formal-facets → master | `c290dea6425f8cc13224a9284b4ad9c77dd1a352` | formal: FDG-0 Phase B — Formal Facets (#298) | https://github.com/jnhu76/Sluice/pull/303 |
| 304 | MERGED | safety/fdg0-phase-c-historical-gold → master | `e31025bc726ac9a024f144313112c675c359c44d` | research(formal): FDG-0 Phase C — historical gold corpus, METHOD_RECALL_NOT_EARNED (#298) | https://github.com/jnhu76/Sluice/pull/304 |
| 306 | MERGED | research/tv1-trace-drift → master | `7b77f0a8e5054cb77a2e4a65c9b9dc2bac78c69e` | research(formal): TV-1 historical drift sensitivity via semantic trace validation (TRACE_CHANNEL_PARTIAL) | https://github.com/jnhu76/Sluice/pull/306 |
| 307 | MERGED | docs/trace-correspondence-authority-map → master | `9ad9af4e04e9f97ee8224be9c113c54db4777aa3` | docs(formal): clarify C++↔TLA+ correspondence and trace-guard authority | https://github.com/jnhu76/Sluice/pull/307 |
| 309 | MERGED | docs/docs-consistency-phase-a → master | `384c778f99e1a50e9459ba6005f3def17892a5a2` | DOCS-CONSISTENCY-0 Phase A — docs/authority consistency | https://github.com/jnhu76/Sluice/pull/309 |
| 310 | MERGED | cleanup/repository-context-purge → master | `0dd7ad0c1a357261c53508238c616a23652b9bf0` | cleanup: purge legacy repository context | https://github.com/jnhu76/Sluice/pull/310 |
| 311 | MERGED | docs/architecture-rebuild → master | `87ea3507b57ec385a727081a6eda4127203e5225` | docs: rebuild architecture.md from current code | https://github.com/jnhu76/Sluice/pull/311 |
| 312 | MERGED | format/code-blank-lines → master | `a20eb14a306842c6a876a9de304bb4f8165b5a15` | format: apply clang-format to include/ and src/ | https://github.com/jnhu76/Sluice/pull/312 |
| 314 | MERGED | docs/design-doctrine → master | `c856e51bd7cde220a2142297dabdb7d265ee7b37` | docs: freeze explicit I/O mission and design doctrine | https://github.com/jnhu76/Sluice/pull/314 |
| 315 | MERGED | chore/gitignore-serena-xmake → master | `2960f2920759d1a81b0cbeb8fbba1eb76c754080` | chore: ignore serena artifacts and consolidate xmake gitignore rules | https://github.com/jnhu76/Sluice/pull/315 |
| 317 | CLOSED | cleanup/async-dead-protocol → master | `eb557a21e0596e8f2da3822962cdc990dedf8246` | cleanup: remove proven-dead async protocol surface | https://github.com/jnhu76/Sluice/pull/317 |
| 318 | CLOSED | cleanup/remove-unused-sync-backend → master | `3d7591ac4848b8269e553b2c93b4c464100f03f9` | cleanup: remove unused synthetic SyncBackend | https://github.com/jnhu76/Sluice/pull/318 |
| 319 | CLOSED | test/rebuild-async-core-minimum → master | `3e39cae6c3d7e25d78c63d6a1a6b2722a5d1f585` | test: rebuild minimal async core lifecycle tests | https://github.com/jnhu76/Sluice/pull/319 |
| 320 | CLOSED | fix/async-contract-consistency → test/rebuild-async-core-minimum | `37b91123c95481500672f60247d47b0a9ceb8803` | fix: make async cancellation and moved-from contracts explicit | https://github.com/jnhu76/Sluice/pull/320 |
| 322 | CLOSED | audit/architecture-survival-map → master | `918a773e0b07d406a32df769d5209f9074dbbf01` | audit: map Sluice capabilities to APIs and execution paths | https://github.com/jnhu76/Sluice/pull/322 |
| 323 | CLOSED | test/specification-witnesses → audit/architecture-survival-map | `a44352ef3b5f34f503b627309ba96306c98e42f9` | test: rebuild minimal specification witnesses from current architecture | https://github.com/jnhu76/Sluice/pull/323 |
| 324 | MERGED | docs/explicit-file-api-adr → master | `03715b08fe79ed8eb886c811c32e74f050640881` | docs: freeze explicit File API and execution architecture | https://github.com/jnhu76/Sluice/pull/324 |
| 326 | MERGED | audit/io-architecture-gap-1 → master | `c518283ea71039a71dc5dc8d23aef5a8e37b16ed` | docs(audit): SLUICE-IO-ARCH-GAP-AUDIT-1 — IO architecture-gap audit report | https://github.com/jnhu76/Sluice/pull/326 |
| 328 | MERGED | ci/open-code-review-mimo → master | `9d8180f20f505de72097a027356ed1ded77293a1` | ci: add OpenCodeReview with Mimo v2.5 | https://github.com/jnhu76/Sluice/pull/328 |
| 329 | MERGED | ci/ocr-review-drafts → master | `0e0707ae2deb5bf11d52a81629df52677a4af306` | ci: allow manual OCR review for draft PRs | https://github.com/jnhu76/Sluice/pull/329 |
| 331 | MERGED | feat/file-resource-read-path → master | `be07443d154a747bcf08d29f126e631714c3556e` | feat(async): File resource with positional read entry (#330 shape validation) | https://github.com/jnhu76/Sluice/pull/331 |
| 332 | MERGED | feat/file-positional-write → master | `e61aefd9de2635925bb7f93cc775edb8b6a2f167` | feat: File positional write through the existing async seam | https://github.com/jnhu76/Sluice/pull/332 |
| 333 | MERGED | docs/adr-0002-durability-contract → master | `1aaacd44e70bf18851e945d1505ef405073bddcf` | docs: define canonical file durability semantics | https://github.com/jnhu76/Sluice/pull/333 |
| 334 | MERGED | cleanup/remove-synthetic-backends → master | `985671621bb33602ee42bd8fe62585740125ca8c` | refactor: remove synthetic async backends from production surface | https://github.com/jnhu76/Sluice/pull/334 |
| 336 | MERGED | feat/async-file-sync-data → master | `d28f675ac554c523e0ffe8843326a6cfb524cde1` | feat: add File SyncData async operation | https://github.com/jnhu76/Sluice/pull/336 |
| 337 | MERGED | ci/ocr-advisory → master | `9c3415df7db5b1633ec81a7d2d92a412e4434e3f` | ci: make OpenCodeReview failures non-blocking | https://github.com/jnhu76/Sluice/pull/337 |
| 338 | MERGED | docs/explicit-file-conformance-roadmap → master | `54f7f5501a0a1d8bd9ba092ddd22dff5e17b6cba` | docs: add ADR-derived Explicit File conformance roadmap | https://github.com/jnhu76/Sluice/pull/338 |
| 349 | MERGED | docs/adr-authority-convergence-1 → master | `804d47495a28cc0a7b9436529cdca7b85061f4e2` | docs(adr): converge Explicit File contract authority | https://github.com/jnhu76/Sluice/pull/349 |
| 350 | MERGED | feat/blocking-file-surface → master | `870d08f2d6c5feecc587c419f40b6c4f58828896` | feat: add first-class Blocking operations for canonical File | https://github.com/jnhu76/Sluice/pull/350 |
| 352 | MERGED | refactor/app-file-resource-convergence → master | `2ca290f95ea961f1cb4bbb1f60bbcf36d0b643ec` | refactor: converge app consumption onto canonical File | https://github.com/jnhu76/Sluice/pull/352 |
| 353 | MERGED | phase-a/a3-file-state → master | `8a912f1ebbbcce9eb96a6c68ed7be864925d6074` | [Roadmap A3] Canonical File state: size and resize | https://github.com/jnhu76/Sluice/pull/353 |
| 354 | CLOSED | phase-a/a4-sequential-file → master | `1e609b7718d49400d7fa16c03663eef41f4051d6` | [Roadmap A4] Canonical sequential File operations: read and write | https://github.com/jnhu76/Sluice/pull/354 |
| 356 | MERGED | corrective/file-access-legality-351 → master | `e78aacc84f86b27d6b92e5977eda36cb1c3aef85` | [Authority corrective] Freeze and enforce canonical operation access-legality (read on write_only = invalid_argument) | https://github.com/jnhu76/Sluice/pull/356 |
| 357 | MERGED | phase-a/a6-explicit-file-ref-final → master | `7bd432084adcaa5b57f51aeb087e9ce4a78ecb7d` | [Roadmap A6 final] Explicit outstanding operations reference canonical File semantics (access-provenanced NativeFileRef) | https://github.com/jnhu76/Sluice/pull/357 |
| 358 | MERGED | phase-a/a7-vectored-final → phase-a/a6-explicit-file-ref-final | `7ac6bd4ee241bab33b78fb9779455f0bb05edbfb` | [Roadmap A7 final] Vectored decision: not carried as canonical Phase A surface (#355 backfilled) | https://github.com/jnhu76/Sluice/pull/358 |
| 359 | MERGED | corrective/liburing-verification → master | `777449db0e9366a5b70aa3e28cebc5e37cf12f3d` | [liburing verification] SLUICE_HAS_LIBURING as public usage requirement + real-path smoke on A6 representation | https://github.com/jnhu76/Sluice/pull/359 |
| 360 | MERGED | phase-a/a4-sequential-file-final → master | `353a83ef9fc09a05653e8fc9e20b9e22b8c5ad98` | [Roadmap A4] Canonical sequential File operations: read and write | https://github.com/jnhu76/Sluice/pull/360 |
| 361 | MERGED | phase-a/a5-sync-all-final → master | `3cf6a618beffacecd3b82ecd1d9a415af47346cf` | [Roadmap A5] Canonical File-facing SyncAll | https://github.com/jnhu76/Sluice/pull/361 |
| 362 | MERGED | phase-a/a7-vectored-final3 → master | `f9b9286945c3b0655356439fde1df1dc88c06599` | [Roadmap A7 final] Vectored decision: not carried as canonical Phase A surface (#355 backfilled) | https://github.com/jnhu76/Sluice/pull/362 |
| 363 | MERGED | corrective/common-api-ledger-row → master | `ca3f3e48d0c0ca409d5ffba98d8edcba8689ae52` | [Docs] Reconcile Common logical API ledger row with landed SyncAll | https://github.com/jnhu76/Sluice/pull/363 |
| 364 | MERGED | corrective/backend-submit-privacy → master | `dd8b31969e088ae6a42bed711ef6bed2d326e2dc` | [Authority corrective] Backend submit_* overrides stay private end-to-end | https://github.com/jnhu76/Sluice/pull/364 |
| 365 | MERGED | corrective/file-access-precedence-witness → master | `61b7e3e5f179c73304d8d7348fba54f14c334ab6` | [Phase A corrective] Pin §5.5 File admission precedence with executable witnesses | https://github.com/jnhu76/Sluice/pull/365 |
| 367 | MERGED | docs/a8-snapshot-corrective-366 → master | `362ab6f8cc3b243759dd4d5c0c4db0d424dc0a5f` | docs: reconcile A8 snapshot and README state (#366) | https://github.com/jnhu76/Sluice/pull/367 |
| 368 | MERGED | audit/legacy-surface-355 → master | `f92acf30849da3b94a06c8aaa59c28f206a22408` | [Architecture audit] Final §14 disposition of retained legacy I/O surfaces (#355) | https://github.com/jnhu76/Sluice/pull/368 |
| 369 | MERGED | ci/xmake-build-cache → master | `9a2839719063d88b53f6073927aba31073702384` | ci: add cached Xmake build verification | https://github.com/jnhu76/Sluice/pull/369 |
| 371 | MERGED | docs/vectored-semantics-370 → master | `390f15bd5602e9be01bcf54eecaabcf1d6a20bcc` | docs: clarify vectored reopen semantics (#370) | https://github.com/jnhu76/Sluice/pull/371 |
| 372 | MERGED | audit/code-reality-architecture-1 → master | `b9290156f20ae17e8ad8143c9f877ff76ff9a1c9` | [Architecture audit] Code-first reality vs ADR + formal-method triage | https://github.com/jnhu76/Sluice/pull/372 |
| 376 | MERGED | formal/fcb1-stage-0 → master | `bcfb329223c28040cfc6c4a4c1d7b614405b8de7` | formal(BRAKE-1): Stage 0 — V2.2 base calculus: execution domains and call origin | https://github.com/jnhu76/Sluice/pull/376 |
| 377 | MERGED | formal/fcb1-stage-1 → master | `43787ad74b3d3a799e2d18dee7537e874f279e9b` | formal(FCB1-V2.2): Stage 1 — Event re-adjudicated on the execution domains | https://github.com/jnhu76/Sluice/pull/377 |
| 378 | MERGED | formal/fcb1-stage-2 → master | `3ce698d4c76ae0f01fd5094b43b294a19ddb567d` | formal(FCB1-V2.3): Stage 2 — Semaphore core replayed on the split return window | https://github.com/jnhu76/Sluice/pull/378 |
| 379 | MERGED | formal/fcb1-stage-3 → master | `8a3144535ef5b95ba5ead1c348e6f6cc47987270` | formal(FCB1-V2.3): Stage 3 — AsyncMutex THEOREM B on the V2.3 authority (RE-ADJUDICATED ON POST-#378 V2.3 AUTHORITY) | https://github.com/jnhu76/Sluice/pull/379 |
| 380 | MERGED | formal/fcb1-stage-4 → master | `db2b13fad0f978742a458fd9a5907df106437876` | formal(FCB1-V2.3): Stage 4 — AsyncCondition; calculus-brake fired, amended, replayed (RE-ADJUDICATED ON POST-#378 V2.3 AUTHORITY) | https://github.com/jnhu76/Sluice/pull/380 |
| 381 | MERGED | formal/fcb1-stage-5 → master | `36598554b1ede60b796150f7548d90142ad5c027` | formal(FCB-1): Stage 5 — AsyncRwLock core is THEOREM B | https://github.com/jnhu76/Sluice/pull/381 |
| 382 | MERGED | formal/fcb1-stage-6 → master | `5b16e3461954f515c7a7718c531935484cdd09bf` | formal(FCB1-V2.3): Stage 6 — AsyncQueue semantics re-adjudicated; capability verdict RESEARCH/DEFER | https://github.com/jnhu76/Sluice/pull/382 |
| 383 | MERGED | formal/fcb1-stage-7 → master | `705bf192c61158ddae258dc79ee4867c37f5d47a` | formal(FCB1-V2.3): Stage 7 — Select semantics re-adjudicated; capability verdict RESEARCH/DEFER | https://github.com/jnhu76/Sluice/pull/383 |
| 384 | MERGED | formal/fcb1-stage-8 → master | `8143e3685cdab5e9fe9d48d749e82add4b986ab3` | formal(FCB1-V2.3): Stage 8 — Scheduler driver re-adjudicated; capability verdict RESEARCH/DEFER | https://github.com/jnhu76/Sluice/pull/384 |
| 385 | MERGED | formal/fcb1-verdict → master | `71254ce945fba6bc4ee3838594ada66ee6e54590` | formal(FCB1-V2.3): FINAL VERDICT — campaign synthesis on the V2.3 authority; three RESEARCH/DEFER boundaries; multi-worker run deferred | https://github.com/jnhu76/Sluice/pull/385 |
| 386 | MERGED | architecture/fcb1-375-final-disposition → master | `11a31614adf9587e34409147f17149f0c474365b` | architecture(FCB1): adjudicate async primitive ownership and close #375 | https://github.com/jnhu76/Sluice/pull/386 |
| 389 | MERGED | architecture/explicit-io-v1-final-decision → master | `daad5dd29d48aa9c136e0027047a64ed70fdaed8` | docs: freeze Sluice v1 architecture and implementation authority | https://github.com/jnhu76/Sluice/pull/389 |
| 404 | MERGED | docs/v1-r3-architecture-view → master | `8377e5ca7308b0d37fd4e7ca8ba3f3c9bd39c501` | docs: add expanded v1 architecture ownership view | https://github.com/jnhu76/Sluice/pull/404 |
| 405 | MERGED | docs/v1-authority-archive-foundation-audit → master | `5345c4cdff05511693ebc89e1bbaae8cbef3bb25` | docs: archive superseded architecture records and freeze v1 documentation entrypoints | https://github.com/jnhu76/Sluice/pull/405 |
| 406 | MERGED | feat/v1-a1-shared-file-semantic-oracle → master | `a71f5cd059197a74117cd97b6d26cb72230e742e` | feat(v1-a1): establish shared File semantic oracle | https://github.com/jnhu76/Sluice/pull/406 |
| 407 | MERGED | feat/v1-a2-direct-w01-closure → master | `3f8ac065bc52fe7dc4cf11a9868ae164c11dd8f5` | feat(v1-a2): close direct File and W-01 semantics | https://github.com/jnhu76/Sluice/pull/407 |
| 408 | MERGED | formal/v1-f0-evidence-map → master | `58c0609f28c4256e50e4f87740424378301ce8f2` | formal(v1-f0): reset evidence map around v1-r3 protocols | https://github.com/jnhu76/Sluice/pull/408 |
| 409 | MERGED | chore/v1-comment-hygiene → master | `c115ec02928400f21d1d3b24fe8ded9bb91db4d9` | docs(governance): restrict C++ comments to local hazards | https://github.com/jnhu76/Sluice/pull/409 |
| 411 | MERGED | chore/410-comment-residual-cleanup → master | `54107fa28c3648b8e454e77d1cc63b57c6d959f6` | chore(governance): remove residual non-hazard C++ comments | https://github.com/jnhu76/Sluice/pull/411 |
| 412 | MERGED | ci/410-comment-authority-guard → master | `e8c2e370f407e2fbb3d03d47ae270f00012773b4` | ci(governance): add narrow guard against C++ pseudo-authority comments | https://github.com/jnhu76/Sluice/pull/412 |
| 416 | MERGED | test/415-primitive-short-write-evidence → master | `e9f6435374ba400e1ffb516b4a36fdae49361999` | test(evidence): pin the primitive short-write boundary at the seam | https://github.com/jnhu76/Sluice/pull/416 |
| 418 | MERGED | feat/394-b1-1-request-core-substrate → master | `a30523cccedbea85d1826225ffaabf1004c58ba7` | feat(async): standalone RequestCore protocol substrate (B1-1) | https://github.com/jnhu76/Sluice/pull/418 |
| 420 | MERGED | docs/419-mechanism-justification-gate → master | `ef52975f9c766e004a125e9435ce34fdf47d62da` | docs: add mechanism justification gate to agent guide | https://github.com/jnhu76/Sluice/pull/420 |
| 421 | MERGED | feat/394-b1-a-context-ownership → master | `de1168588356a5b0f5ccac5f6c709764e80f24c1` | feat(async): context-owned RequestCore and one context identity domain (B1-A) | https://github.com/jnhu76/Sluice/pull/421 |
| 422 | MERGED | feat/394-b1-b-threadpool-core-cutover → master | `3e78521151263c98e145d8687175907a07292825` | feat(async): ThreadPool atomic RequestCore authority cutover (B1-B, #394) | https://github.com/jnhu76/Sluice/pull/422 |
| 423 | MERGED | feat/394-b1-c-uring-core-cutover → master | `2d56fdf1d258322fe0a83bfbb8e5bedc831462e6` | feat(async): B1-C — io_uring atomic RequestCore authority cutover (#394) | https://github.com/jnhu76/Sluice/pull/423 |
| 424 | MERGED | chore/agents-testing-discipline → master | `e88c64adf3b48520c42aa9c2d3e97449b8492c07` | docs: add failure-first testing discipline | https://github.com/jnhu76/Sluice/pull/424 |
| 425 | MERGED | feat/394-b1-d-evidence-contraction → master | `50b0e089c6d5adf7de08884afd37006c56ecce4a` | refactor(async): close B1 RequestCore migration authority (#394) | https://github.com/jnhu76/Sluice/pull/425 |
| 426 | MERGED | feat/395-public-request → master | `b55957f43afb87fd2c59997aca13d80df8a17b34` | feat(async): add public Request<T> settlement responsibility (#395) | https://github.com/jnhu76/Sluice/pull/426 |
| 434 | MERGED | feat/396-observer-protocol → master | `95c0717bbd126a0d972f06d3fc281a73e3993bd3` | feat(async): migrate Observer protocol out of the backend (#396) | https://github.com/jnhu76/Sluice/pull/434 |
| 435 | MERGED | c1-h-observer-formal-model → master | `cbecd4e868e83c087f94aa70b6328140b3c822f7` | test(formal): add TLA+ ObserverCore model for the Observer protocol (#433) | https://github.com/jnhu76/Sluice/pull/435 |
| 436 | MERGED | docs/396-observer-formal-closure → master | `c27675099c01c52577115b7e196f5e17e887ee5f` | docs(formal): close #396 Observer proof obligation | https://github.com/jnhu76/Sluice/pull/436 |
| 437 | MERGED | feat/397-c2-a-progress-source-ownership → master | `4e47f9cc2bddce76d72f182a947d1ba632046613` | feat(async): C2-A move ProgressSource authority to IoContext (#397) | https://github.com/jnhu76/Sluice/pull/437 |
| 438 | MERGED | feat/397-c2-b-threadpool-no-lost-wake → master | `6041f562f8222faf0bacbe3f2f04d5da11d52c9f` | feat(async): C2-B close ThreadPool no-lost-wake protocol (#397) | https://github.com/jnhu76/Sluice/pull/438 |
| 439 | MERGED | feat/397-c2-c-uring-progress-convergence → master | `02115b64ce1097aceb7d2990a498351873a30d4f` | feat(async): C2-C converge io_uring readiness on ProgressSource (#397) | https://github.com/jnhu76/Sluice/pull/439 |
| 440 | MERGED | feat/397-c2-d-owner-outcomes-lifetime → master | `941493929b8ac0890e1f301913654012930227d6` | feat(async): C2-D single progress owner, owner outcomes, sticky control, external-host lifetime (#397) | https://github.com/jnhu76/Sluice/pull/440 |
| 442 | MERGED | fix/397-c2-abcd-conformance-contraction → master | `1b468fc8bb5da9fa938c6968ffe0a1a763343e4f` | fix(async): finalize C2 A-D conformance and contract progress mechanisms (#397) | https://github.com/jnhu76/Sluice/pull/442 |
| 443 | MERGED | formal/397-c2-e-progress-closure → master | `a73ea6835f96e013c57107bb360e0d2c00d01884` | formal(progress): close C2 ProgressSource proof obligations (#397) | https://github.com/jnhu76/Sluice/pull/443 |
| 444 | MERGED | fix/397-c2e-close-admission-transition-signal → master | `b3cdc6a2181a56656a85659eafe4287e30261c4b` | fix(progress): bind the close_admission wake to the admission transition (#397) | https://github.com/jnhu76/Sluice/pull/444 |
| 445 | CLOSED | docs/sluice-v1-pages → master | `d63c38ac8f881fcdfa8ad161123f5e1cda31fac1` | docs(pages): publish Sluice v1 interactive architecture walkthrough | https://github.com/jnhu76/Sluice/pull/445 |
| 447 | MERGED | feat/398-bounded-request-scope → master | `528cb509e794f20e219685da4c810199a167a898` | feat(async): add bounded RequestScope structured settlement (#398) | https://github.com/jnhu76/Sluice/pull/447 |
| 448 | MERGED | feat/visual-github-pages → master | `ff6b8058cdc58e0c6dce860bd5197c7dbbbb0083` | feat(docs): publish interactive Sluice v1 architecture walkthrough | https://github.com/jnhu76/Sluice/pull/448 |
| 449 | MERGED | feat/399-narrow-stackful-host → master | `4e71bd52df950f6f4298473b769d021cd18639b4` | D2 (#399): narrow single-owner stackful File-I/O host (StackfulIoHost) + W-04/HOST-03 evidence | https://github.com/jnhu76/Sluice/pull/449 |
| 450 | MERGED | feat/400-e1-backend-refinement → master | `6ae2e651d09a0b093848bd9d16c14cf74c76e281` | feat(async): E1 request-path metadata ops, effect currency, cancel refinement (#400) | https://github.com/jnhu76/Sluice/pull/450 |
| 453 | MERGED | fix/401-e2-shutdown-replacement → master | `2f17d1d577301bd067423fde7c664c84dc60d772` | E2: shutdown / poison / retained-result replacement (#401, #452) | https://github.com/jnhu76/Sluice/pull/453 |
| 455 | MERGED | docs/454-f0-role-profile-census → master | `7ed62f2208c19182ffa1c07f3782b079c33bc353` | docs(f0): Role/Profile Census + Policy + Consumer Migration DAG (#454) | https://github.com/jnhu76/Sluice/pull/455 |
| 456 | MERGED | feat/402-f1-clean-room-baseline → master | `6404e07b5e07f197224b957c94680bb785b81b75` | feat(f1): installed clean-room package baseline + requirement-indexed micro-contract oracles (#402) | https://github.com/jnhu76/Sluice/pull/456 |
| 457 | MERGED | m/m0-census → master | `15cca2f63ef587f0860997c57642661c21d795cd` | M0/M1: live consumer census & family readiness plan | https://github.com/jnhu76/Sluice/pull/457 |
| 464 | MERGED | m/m5-final → master | `f13f40db8ff0a1df4ccc40c08522fc819f556f37` | docs(m): M final integration report (#402) | https://github.com/jnhu76/Sluice/pull/464 |
| 465 | MERGED | fix/n02-frozen-baseline-provenance → master | `118ff6731cac72094e2fcf3cd8e8f4743d392971` | fix(f1): pin --check-frozen provenance to the frozen manifest baseline (N-02) | https://github.com/jnhu76/Sluice/pull/465 |
| 468 | MERGED | m/mr-tier1-exec → master | `7f5b113b923aea8d89aefc9eb9c4098c576a3a99` | M-R tier-1: retype reachable consumers to the adopted Request face + wire B-02 (#466) | https://github.com/jnhu76/Sluice/pull/468 |
| 469 | MERGED | m/mr-tier2-isolation-record → master | `a980f9e8eee53a200f8d6385b6de1f2aa3129cdb` | M-R tier-2: consumer isolation record with owners, exit conditions and COPY-B/TAIL obligations (#467) | https://github.com/jnhu76/Sluice/pull/469 |
| 470 | MERGED | m/m5-closeout → master | `89d0b087b4b5ee9f723b773769d987fdd55519f8` | M closeout: seven-family disposition, evidence and honest PHASE_M=NO (#458) | https://github.com/jnhu76/Sluice/pull/470 |
| 471 | MERGED | m/mr-tier1-residual → master | `dad773c203a762791f5c98e1bb8934707975564a` | M-R residual tier-1: retype the last two AsyncIoContext consumers + record corrections (#458) | https://github.com/jnhu76/Sluice/pull/471 |
| 472 | MERGED | m/mr-disposition-overlay → master | `b0b0e391f49384b22df0e6882dd06f472171fa53` | docs(m): M-R consumer disposition projection + Option A provenance re-anchoring (post-merge review) | https://github.com/jnhu76/Sluice/pull/472 |
| 473 | MERGED | decision/460-option-b-conditional-adr → master | `78d94227172438534d6530eb9d17ac61fae7e7b3` | docs(adr): condition ADR-0003 supported status for M-H Option B | https://github.com/jnhu76/Sluice/pull/473 |
| 476 | MERGED | docs/adr-0003-option-b-adoption → master | `f5380d5e0ee74820897e2f27ba1b1fb93e216d29` | docs(adr): formally adopt ADR-0003 narrow host design under Option B | https://github.com/jnhu76/Sluice/pull/476 |
| 477 | MERGED | test/e1-copy-b-contract-oracle → master | `f6651524295d81584146f278e118d5e2ab4853d9` | test(copy): e1 COPY-B contract oracle recovery (#474) | https://github.com/jnhu76/Sluice/pull/477 |
| 478 | MERGED | test/e4-tail-lifecycle-oracle → master | `a7c9e30dea5d43e69a1773669f2835610ffe4f5a` | test(tail): TAIL stop/settlement/teardown contract oracles (e4, #474) | https://github.com/jnhu76/Sluice/pull/478 |
| 479 | MERGED | docs/e2-app-readme-reality-alignment → master | `901acfca0b48a27e5d7704560e28710aaec2bef9` | docs(apps): align four app READMEs with live tests and behavior (e2, #474) | https://github.com/jnhu76/Sluice/pull/479 |
| 481 | MERGED | diag/e1-settlement-fix → master | `e24ee88b80867d987ec486206d6126fb6151875a` | fix(e1): publish a progress wake when a worker retires an unclaimable dispatch entry (e1, #474) | https://github.com/jnhu76/Sluice/pull/481 |
| 484 | MERGED | fix/e1-t4b-cancel-wake → master | `646db4181bfa49adde619610da899becc33b3b38` | fix(e1): signal when cancel_key retires a stale dispatch entry without a cancel win (T-4b, #482) | https://github.com/jnhu76/Sluice/pull/484 |
| 485 | MERGED | test/e3-shutdown-bounded-readiness → master | `333a3cb36cf40723c7141d5f82c7e48faaf652e1` | test(shutdown): replace fixed spin budgets with bounded readiness waits (#483) | https://github.com/jnhu76/Sluice/pull/485 |
| 486 | MERGED | docs/e1-settlement-ledger → master | `6000745e07332b0589f032e7f31af6dff59ee884` | docs(ledger): record the e1 settlement notification correction (#480, PROG-02) | https://github.com/jnhu76/Sluice/pull/486 |
| 487 | MERGED | docs/m-remaining-family-closeout → master | `89bf45ca7ffba65916fcf9bd0ae79c493756467d` | docs(m): Phase M wave-2 closeout — remaining-family dispositions and seven-family gate recompute (#458) | https://github.com/jnhu76/Sluice/pull/487 |
| 488 | MERGED | docs/v1-r4-negative-admission-m-exit-policy → master | `21b6e86abc0d7bae22980a7fd50530555c25e77d` | docs(root): v1-r4 minimal-scope exclusions and Phase M/F3 governance alignment | https://github.com/jnhu76/Sluice/pull/488 |
| 490 | MERGED | docs/489-f-w2-1-dual-profile-refreeze → master | `7f5eed6acab33c2a78390ab7e594b4e3f77f1107` | docs(f1): versioned dual-profile re-freeze for F-W2-1 | https://github.com/jnhu76/Sluice/pull/490 |
| 491 | MERGED | codex/m-final-adoption → master | `aabca28f9d46d9c552f1d62f45559edb28f4c8e8` | Phase M final adoption: minimal v1 decisions, consumer closure and Phase F handoff | https://github.com/jnhu76/Sluice/pull/491 |
| 492 | MERGED | fix/f3-hash-direct-default → master | `20fc313cb0c09f0e27611977dc39fa4d0f710397` | F3: migrate single-worker HASH to direct File reads | https://github.com/jnhu76/Sluice/pull/492 |
| 493 | MERGED | fix/f3-grep-direct → master | `0c5a6503041d48aa75062b2fcbd5f6046b524d5f` | F3: migrate GREP default execution to direct I/O and fix D-H1/D-H2 | https://github.com/jnhu76/Sluice/pull/493 |
| 494 | MERGED | codex/f3-hash-legacy-exit → master | `b3cf09156d13ff36febc6a54951b56991ed70289` | F3 Step 3: exit HASH legacy runtime compatibility | https://github.com/jnhu76/Sluice/pull/494 |
| 495 | MERGED | codex/f3-grep-legacy-exit → master | `5b752d202f1c241e6d38808894d2491284534ded` | F3 Step 3: exit GREP legacy runtime compatibility | https://github.com/jnhu76/Sluice/pull/495 |
| 496 | MERGED | codex/f3-copy-request-scope → master | `88eeb0a57a09687aedab6b6ba81eed1d5d7af818` | F3: migrate COPY pipeline to canonical RequestScope | https://github.com/jnhu76/Sluice/pull/496 |
| 497 | MERGED | codex/f3-tail-owned-driver → master | `c3217fa95cb983ae74e978f75fb011ddaac7fa0b` | F3: replace TAIL ApplicationRuntime with app-owned driver | https://github.com/jnhu76/Sluice/pull/497 |
| 499 | MERGED | codex/f3-runtime-retirement-audit → master | `76774f21d6bb9a8a71f5a768667919b121179c81` | F3: audit postmerge runtime consumers and define physical retirement batch | https://github.com/jnhu76/Sluice/pull/499 |
| 500 | MERGED | codex/f3-498-remaining-test-prep → master | `49936edabf6b0bbd6955c1e363846c62392bb46d` | F3: migrate COPY borrow-metric selftests to canonical RequestScope | https://github.com/jnhu76/Sluice/pull/500 |
| 501 | MERGED | codex/f3-498-canonical-test-migration → master | `d38430cf3ed770e2705c20eefa5c1fcd75bec6d2` | F3: migrate legacy File test consumers to canonical Request/File | https://github.com/jnhu76/Sluice/pull/501 |
| 502 | MERGED | codex/f3-498-retire-async-file → master | `a1cc92c9d1da2ff96e6b8e87fbce8b7df11a349d` | F3: retire legacy completed-return async File wrapper | https://github.com/jnhu76/Sluice/pull/502 |
| 503 | MERGED | codex/f3-part-c-c1-waiter → master | `0d11cde73484844b0a1c3201b182c1ab910ec37a` | F3: re-own Scheduler waiter oracles without ApplicationRuntime | https://github.com/jnhu76/Sluice/pull/503 |
| 504 | MERGED | codex/f3-part-c-c2-facade → master | `4964613aad16aefad1b9f3e70af8215fe5d8bc9a` | F3: physically retire ApplicationRuntime facade | https://github.com/jnhu76/Sluice/pull/504 |
| 507 | MERGED | docs/f3-phase-m-handoff-closeout-20261012 → master | `41be167196ccb98cbd0b410aa49803e4c65ff1dc` | docs(roadmap): pin Phase M closeout and F3 scoped retirement handoff | https://github.com/jnhu76/Sluice/pull/507 |
| 508 | MERGED | codex/f4-506-pool → master | `e642fd7aca270451d5072580eefd693b76ca00ca` | retire(#506): physically remove BlockingIoPool product | https://github.com/jnhu76/Sluice/pull/508 |
| 509 | MERGED | codex/f4-505-oracle → master | `2107a4f381f1a0cf936e01fc3ce97ab277d479a1` | test(#505-A): re-own semantic range oracle on canonical File | https://github.com/jnhu76/Sluice/pull/509 |
| 510 | MERGED | codex/f4-505-retirement → master | `23c925f9f7c03a2ab36021e370bac249d6ac88c0` | retire(#505-B): remove legacy File/Reader/Writer native authority | https://github.com/jnhu76/Sluice/pull/510 |
| 511 | MERGED | codex/f4-w02-borrow-lifetime → master | `e71fcdaa1fdc21b0d2dc4522a5e5e8b7266fd90f` | test(package): preserve W-02 borrowed buffers through scope cleanup | https://github.com/jnhu76/Sluice/pull/511 |
| 512 | MERGED | codex/f4-installed-retirement-verifier → master | `30e70cc21f02361e0edaf967d979499ea4e2e257` | test(f4): verify 17 installed surface retirements and owner risk evidence | https://github.com/jnhu76/Sluice/pull/512 |
| 513 | MERGED | codex/f3-final-app-host-20261012 → master | `350e789810f16078d528e572a0b124e71bd6da97` | test(apps): verify final app-host scope and reject inert TAIL workers | https://github.com/jnhu76/Sluice/pull/513 |
| 514 | MERGED | codex/f4-postmerge-b01 → master | `7b78ac5e90628d695907715b9c036b20d509619d` | test(evidence): adopt F4 clean-break postmerge B-01 package cut | https://github.com/jnhu76/Sluice/pull/514 |
| 515 | MERGED | codex/f3-postmerge-receipt-20261013 → master | `2a19df3f29210dc65719cd1fdcab2d6d6ce4afa8` | docs(evidence): record approved F3 merged-master closeout | https://github.com/jnhu76/Sluice/pull/515 |
| 516 | MERGED | f2-clean-authority → master | `acbd1a73c34c9cc569172d2794de558cf65aca54` | F2: contract the Request/Result/Identity authority (clean break) | https://github.com/jnhu76/Sluice/pull/516 |
| 517 | MERGED | f2-postmerge-receipt-20261013 → master | `63178b5727fe2ca4a8fd5a45d39782867cdf5f84` | docs(evidence): record approved F2 actual-master exit | https://github.com/jnhu76/Sluice/pull/517 |
| 518 | MERGED | f5-final-package → master | `5ef4f801095584044d61fd1156862becca5d9db7` | F5: final package & public surface contraction (#402) | https://github.com/jnhu76/Sluice/pull/518 |
| 522 | MERGED | codex/pre-f6-infra-520-519-521 → master | `7077c4d2dcc4257042e232cc522cbe3fe389c52f` | pre-F6 infra stabilization: same-dir ring-switch safety (#520), Clang mutation-target build (#519), RequestScope TSan seam (#521) + F-10 dependency-identity correction | https://github.com/jnhu76/Sluice/pull/522 |
| 529 | MERGED | codex/f6-final-closeout-523 → master | `845e17f5977f0132761db3b0471a6fd45bdd9764` | F6 final closeout: evidence adoption, findings disposition and phase closure | https://github.com/jnhu76/Sluice/pull/529 |
| 530 | MERGED | codex/525-mechanical-cleanup → master | `4d941f15ab03dc1c8e3431f56dc07b58d0104edf` | chore(headers): inert Scheduler friendship removal + installed-header -Wextra fix; F6-F17 deferred (#525) | https://github.com/jnhu76/Sluice/pull/530 |
| 531 | MERGED | codex/526-request-error-domain → master | `1a97212a059cb87524d7512f2dffbc31a9b0d0f6` | test(err): request-path submission-failure and terminal errno re-diff oracle (F6-F10, #526) | https://github.com/jnhu76/Sluice/pull/531 |
| 532 | MERGED | codex/527-test-oracles → master | `d65696bac414622ac8516719405de42b0c1dc909` | test(life,thread): File move-borrow oracle and concurrent-submit corner evidence (F6-F13/F6-F14, #527) | https://github.com/jnhu76/Sluice/pull/532 |
| 533 | MERGED | codex/524-requestcore-convergence → master | `ecdbb8c638eb5f75ce42abbb3e186989f3109765` | formal(request): remove the retired ReleaseBind flavor and migrate its mutation oracle (#524 / F6-F03) | https://github.com/jnhu76/Sluice/pull/533 |
| 534 | MERGED | codex/403-g0-performance-baseline → master | `20e0004f0cbbb8c8cd7e7f9def34e547f5e35d1f` | G0(#403): evidence-only measured baseline, causal attribution and G1 candidate package | https://github.com/jnhu76/Sluice/pull/534 |
| 535 | CLOSED | codex/403-flamegraph-evidence → master | `079265cc7284aa2c82709e8d4e100ee7f6fa2bf4` | perf(403): native flame-graph profiling evidence (evidence-only, do not merge) | https://github.com/jnhu76/Sluice/pull/535 |
| 536 | MERGED | codex/403-g1-native-optimization → master | `9069b4b5f0bbe55013068d1e186668a3f7114a57` | G1(#403): native Linux optimization — C9-A/C1/C2 with correctness fixes and tests | https://github.com/jnhu76/Sluice/pull/536 |
| 542 | MERGED | codex/v2-architecture-freeze → master | `a7cd6e4bfdef5d155e54d9c245a198de3f5a9400` | docs(v2): propose the v2 architecture and contract as a replacement root (PROPOSED) | https://github.com/jnhu76/Sluice/pull/542 |
| 543 | MERGED | codex/uring-owed-residue-report → master | `4db12a910951aff07bff072d3c49f0d6439eff27` | fix(async): report uring owed deliveries as immediate pass residue | https://github.com/jnhu76/Sluice/pull/543 |
| 544 | MERGED | docs/v2-authority-adoption → master | `ba27beee7db9a0e7180bcbc7110d317805f33b6b` | docs(v2): adopt sole authority and replace v1 conformance ledger | https://github.com/jnhu76/Sluice/pull/544 |
| 545 | MERGED | pr-b/v2-surface-contraction → master | `65a7632043ba11b38d99dbf934a2e3988331e59c` | PR B: v2 code & surface contraction (D1/D2/D6) — 25→11 headers, backend seam closed, host removed | https://github.com/jnhu76/Sluice/pull/545 |
| 546 | MERGED | repo-hygiene → master | `497a8ae4d46dfb985a7745e106007b1c567743db` | chore(hygiene): repository contraction — semantic names, closed-campaign retirement, verified evidence | https://github.com/jnhu76/Sluice/pull/546 |
| 547 | MERGED | v2-release-gates-convergence → master | `91f9c24fe4febc54dc274542d264dbd40f10aa50` | v2 release gates RG1-RG7: LSan fix, identity/exhaustion evidence, PROG-02 oracle+mutant, release matrix, D3 disclosure | https://github.com/jnhu76/Sluice/pull/547 |
| 548 | MERGED | docs/v2-release-528-closeout → master | `7424fd0ba8bdfe393f448bcaf282490eaa37e82a` | docs: v2 release documentation convergence (#528 closeout) | https://github.com/jnhu76/Sluice/pull/548 |
| 549 | MERGED | chore/v2-pre-release-residue-cleanup → master | `2f2cba0c10d4c2dda48af92839f3a4be889f7132` | chore: v2 pre-release residue and naming cleanup (v0.0.2 prep) | https://github.com/jnhu76/Sluice/pull/549 |
| 551 | MERGED | release/v0.0.2 → master | `8a2f59499c3c7688bbc251a98b87078d1f87238c` | Release: Sluice v0.0.2 (v2-only source release) | https://github.com/jnhu76/Sluice/pull/551 |
| 552 | MERGED | fix/550-threadpool-progress-test-synchronization → master | `6313c2ad521b065427bcdf29bfe5a0192dc3e044` | test(threadpool): establish causal worker-signal witness for three #550 race oracles | https://github.com/jnhu76/Sluice/pull/552 |
| 554 | MERGED | test/553-m5-deterministic-oracle → master | `e67eb25a3ce367896a22fdf21c126fb3c63a39ae` | test(threadpool): deterministic M5 signal-before-enqueue oracle (issue #553) | https://github.com/jnhu76/Sluice/pull/554 |
| 558 | MERGED | docs/llms-txt-v2 → master | `40a7d2ecb23e5796d4f91a854f22719495343f73` | docs: add llms.txt navigation index for coding agents | https://github.com/jnhu76/Sluice/pull/558 |
| 560 | MERGED | research/m0m1-state-complexity → master | `2e998fba760f553964a318b6f4a79c73f9dbeeae` | research(m0-m1): minimal-state / STL container & algorithm complexity study | https://github.com/jnhu76/Sluice/pull/560 |
| 562 | MERGED | research/native-linux-m2 → master | `741e7dc2794eb89ae29607b8930d08d83d3f0faa` | research(perf): validate io_uring mechanisms on native linux | https://github.com/jnhu76/Sluice/pull/562 |
| 565 | MERGED | perf/uring-owed-count-skip → master | `fe91e7fdc47e22bb8309e6d4c90950565c006239` | perf(uring): eliminate empty owed-event sweeps with derived count | https://github.com/jnhu76/Sluice/pull/565 |
| 566 | MERGED | research/mw-hw-1-path-elimination → master | `0294a6b5e6bf61a9fa5a312f66cbb4bdd85ca46b` | research(mw-hw-1): WSL2 path elimination & kernel boundary audit | https://github.com/jnhu76/Sluice/pull/566 |
| 567 | MERGED | research/native-linux-e1 → master | `d03f0b8b1a57fe7c5006c3c4367e1c6ed998d3f3` | research(mw-hw-1): E1 native-Linux validation round — evidence archive (CONDITIONAL) | https://github.com/jnhu76/Sluice/pull/567 |
| 569 | MERGED | perf/e1-pending-control-zero-scan → master | `c8c8eba4a456004b7592ec1397e99d13576095d9` | perf(uring): eliminate zero-pending control router scans | https://github.com/jnhu76/Sluice/pull/569 |
| 570 | CLOSED | research/dp0-data-movement-audit → master | `0d84b4775f1daf7bd11038b9b6d0c0d6dec224a7` | research(DP-0): compiler-first data-movement audit (RESEARCH ONLY — do not merge) | https://github.com/jnhu76/Sluice/pull/570 |
| 571 | MERGED | perf/bench-w03-stop-race-stats-wiring → master | `389ec94b5f6237e99634bcfe6a7d68c9e9ddae44` | fix(bench): close W-03 stop race and wire async stats | https://github.com/jnhu76/Sluice/pull/571 |
| 572 | MERGED | perf/dp1-publication-payload → master | `30d74a1831223b9245dded2af24ec3b869cdb3d3` | perf(async): eliminate unused publication payload materialization | https://github.com/jnhu76/Sluice/pull/572 |
| 573 | MERGED | perf/dp3a-reserve-reinit → master | `89dafc750de161c613d5b6b89ef1fbf7ca989cb6` | perf(request): eliminate redundant reserve-time slot reset | https://github.com/jnhu76/Sluice/pull/573 |
| 583 | MERGED | release/v0.0.3 → master | `88185f5d9db120c93d3bc6f8af3ea2a497adc34f` | Release v0.0.3: changelog, validation receipt, ledger record | https://github.com/jnhu76/Sluice/pull/583 |
| 584 | CLOSED | research/575-native-e5-atomic-rmw → master | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` | research(575): native E5 atomic RMW elimination evidence — NO_CHANGE_JUSTIFIED | https://github.com/jnhu76/Sluice/pull/584 |
| 585 | MERGED | test/575-uring-flush-count-oracle → master | `a691cc290c3ff46d7e1658497585f3bf6db90d6a` | test(uring): pin positive io_uring transport flush accounting | https://github.com/jnhu76/Sluice/pull/585 |
| 588 | MERGED | fix/587-tail-child-liveness → master | `d889cc10a53efe1db6f3ff7c184cfde8bd7c2965` | test(app): fail fast on dead tail child and surface captured diagnostics (#587) | https://github.com/jnhu76/Sluice/pull/588 |
| 589 | MERGED | docs/xmake-module-layout → master | `298b6ee141039829211c94b6028afc0d61486443` | docs(build): clarify xmake module layout | https://github.com/jnhu76/Sluice/pull/589 |
| 590 | MERGED | fix/586-asan-preload-order → master | `c768b72f0648c60c55d46ddddd3df6150edd98ca` | test(app): load ASan runtime before tail pread shim (#586) | https://github.com/jnhu76/Sluice/pull/590 |
| 592 | MERGED | research/uring-router-580-wsl → master | `82be0f4855e7ed142dc27805df16949f9185ffe6` | research(uring): archive #580 evidence — NO_TRIGGER_NO_CHANGE | https://github.com/jnhu76/Sluice/pull/592 |
| 594 | OPEN | research/593-key-path → master | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | research(593): E5 native key-path discovery & mechanism-elision campaign | https://github.com/jnhu76/Sluice/pull/594 |
| 601 | MERGED | sluice-600-direct-research → master | `2fcc79646ee42d1699468e0a50c0269f405b92d3` | research(direct): #600 E5 excess-cost attribution and C1 validation evidence | https://github.com/jnhu76/Sluice/pull/601 |
| 602 | MERGED | sluice-600-direct-c1 → master | `77e5399a45998dea396699fa2e063c94a665945a` | perf(direct): dispatch admission verdicts without Precheck materialization (#600 C1) | https://github.com/jnhu76/Sluice/pull/602 |
| 610 | MERGED | research/direct-beyond-parity-evidence → master | `d40a811dae783837c4749718a2a17f895d46d117` | research(direct-beyond-parity): archive #603 P0-P6 evidence with sha256 manifest | https://github.com/jnhu76/Sluice/pull/610 |
| 611 | CLOSED | research/r6-ac1-vectored-semantics → master | `999b5305ae7d413a2a71acde926f8c6cfef6876a` | research(ac1): E5 semantic closure for positional vectored I/O (#605) | https://github.com/jnhu76/Sluice/pull/611 |
| 612 | MERGED | sluice-608-p2c1-read-compose → master | `e4c9588ae8754ea44cf52194294103a7795aa1a0` | perf(direct): P2C1 read compositions run prevalidated register-resident steps (#608) | https://github.com/jnhu76/Sluice/pull/612 |
| 613 | MERGED | ci/comment-guard-research-evidence-scope → master | `1b3e87f02b44e89397f703607bcb7f906ad89fa4` | ci(guard): keep comment-authority scan off archived research evidence | https://github.com/jnhu76/Sluice/pull/613 |

## dududu (github.com/jnhu76/dududu)

**Provenance caveat:** this section is built from a fresh clone made for the
audit (`gh repo clone jnhu76/dududu` → `/tmp/dududu-audit`), not from the owner's
local working copy. `/home/hoo/Projects/dududu` does not exist (`test -e` →
MISSING) and `/tmp/dududu-audit` did not pre-exist (the first `gh repo clone`
attempt succeeded directly; no https fallback was needed). Its dirty-state and
worktree layout therefore reflect the GitHub remote plus a fresh clone only — not
the owner's on-disk dududu state. After cloning, `git fetch --prune` ran clean
(no output), and `git ls-remote origin` cross-checked all 5 branch SHAs as
identical to the clone's `for-each-ref`, so the branch table below is the remote's
real heads, not clone cache.

### Default branch

`origin/HEAD` → `refs/remotes/origin/main`; `main` @
`12a8712eb5ff5f48e570b49b1e01ad5821837dca`, tip subject "Merge pull request #7
from jnhu76/sluice-r0-e5-verdict" (2026-10-29 12:47:38 +0800). `main` itself is
excluded from the audit target pool.

### Remote branches (5)

| Ref | SHA | Relation to `main` |
| --- | --- | --- |
| main | `12a8712eb5ff5f48e570b49b1e01ad5821837dca` | default branch |
| r1-request-geometry | `47cd1976701a2c9e73fc31470f45aec236a3e0b2` | not an ancestor: 0 commits behind main, 4 ahead — real unmerged work; head of OPEN PR #8 |
| r5-async-host-probe | `4eafbc06d6a2e476055fe45fc75656e7e28a2183` | fully merged (ancestor of main; 0 unique commits, main ahead by 3) — post-merge residual remote branch |
| sluice-r0-closure | `cc053cf9258b98546b16f9d2f08c5559fa73bc77` | fully merged (ancestor of main; 0 unique commits, main ahead by 4) — no PR record, see below |
| sluice-r0-e5-verdict | `cc321a4994ee638493fecda0c437481528f5cea9` | fully merged (ancestor of main; 0 unique commits, main ahead by 1) — head of MERGED PR #7 |

Ancestry facts were collected with `git merge-base --is-ancestor` plus
`git rev-list --left-right --count` and re-verified in `/tmp/dududu-audit` while
writing this document (all 5 SHAs matched the audit snapshot exactly):

```
r1-request-geometry:  not-ancestor  main-only/branch-only = 0  4
r5-async-host-probe:  ancestor      main-only/branch-only = 3  0
sluice-r0-closure:    ancestor      main-only/branch-only = 4  0
sluice-r0-e5-verdict: ancestor      main-only/branch-only = 1  0
```

`sluice-r0-closure` @ `cc053cf9258b98546b16f9d2f08c5559fa73bc77` has no
corresponding PR anywhere in the all-state PR list below (the five PR heads are
r1-request-geometry, sluice-r0-e5-verdict, r1-parquet-sluice-bench,
r0-1-repository-policy and r0-foundation); its tip is already a main ancestor,
i.e. it was merged without a PR record. `ls-remote` additionally shows
`refs/pull/1/head`, `refs/pull/2/head`, `refs/pull/3/head`, `refs/pull/7/head`,
`refs/pull/8/head` and `refs/pull/8/merge` — these are PR refs, not branches, and
are excluded from the table above. Audit target pool: the 4 non-`main` branches
above, none of which exists as a local branch (`alsoLocal` empty for all).

### Local branches

| Branch | SHA | Upstream |
| --- | --- | --- |
| main | `12a8712eb5ff5f48e570b49b1e01ad5821837dca` | origin/main |

### Worktrees

| Path | Branch | HEAD | Dirty |
| --- | --- | --- | --- |
| /tmp/dududu-audit | refs/heads/main | `12a8712eb5ff5f48e570b49b1e01ad5821837dca` | No (dirtyWorkspace = false, fresh clone) |

The clone's own single worktree.

### Pull requests — full table (5 total: 1 OPEN / 4 MERGED / 0 CLOSED)

One `gh pr list --state all --limit 1000` call returned all 5 records (far below
the limit; no paging needed). The head branches of three closed PRs
(`r1-parquet-sluice-bench`, `r0-1-repository-policy`, `r0-foundation`) are deleted
on the remote; their head SHAs survive only under `refs/pull/*`.

| # | State | Head → Base | Head SHA | Title | URL |
| --- | --- | --- | --- | --- | --- |
| 8 | OPEN | r1-request-geometry → main | `47cd1976701a2c9e73fc31470f45aec236a3e0b2` | docs(bench): record metadata micro-read study and opportunity gate | https://github.com/jnhu76/dududu/pull/8 |
| 7 | MERGED | sluice-r0-e5-verdict → main | `cc321a4994ee638493fecda0c437481528f5cea9` | docs(bench): record e5 r0 verdict and owner-gate prereg | https://github.com/jnhu76/dududu/pull/7 |
| 3 | MERGED | r1-parquet-sluice-bench → main | `b07307ed545d6cdc815ea41df5f09c30e282a016` | feat(parquet): establish standalone sluice io benchmark | https://github.com/jnhu76/dududu/pull/3 |
| 2 | MERGED | r0-1-repository-policy → main | `c4fe75f4257d0eef58523a3d6e0894f7f429dad6` | chore(repo): establish local development gates | https://github.com/jnhu76/dududu/pull/2 |
| 1 | MERGED | r0-foundation → main | `59cffffc2221502409d795c0ea8fe63d931939fb` | R0: Sluice x DuckDB experimental foundation | https://github.com/jnhu76/dududu/pull/1 |

### Running processes

None. Three checks all came back empty: (1) `ps -eo pid,ppid,lstart,args | grep -i
dududu` (grep itself excluded) — no output; (2) readlink across every
`/proc/[0-9]*/cwd` — no dududu path; (3) a /proc fd scan — no process holds an
open fd on `/tmp/dududu-audit`. (The first fd scan aborted on a zsh
permission-denied glob for `/proc/1/fd/*`; it was redone with `ls` iteration and
completed with rc=0.)

## Provenance and verification log

- Audit-time receipts (Sluice): receipts/01-remote-refs.txt,
  receipts/02-branch-vv.txt, receipts/03-worktree-list.txt,
  receipts/04-default-branch-and-remote.txt,
  receipts/05-running-processes.txt, receipts/pr-list-full.json,
  receipts/pr-list-full.tsv — seven files under `receipts/`. The dududu findings
  were recorded in the audit result itself; no dududu receipt file exists.
- Re-verification commands run while writing this document (all read-only):
  `date -u`; `ls -ld /home/jnhu/sluice-593-research/evidence` and
  `ls -ld /home/jnhu`; a python pass over receipts/pr-list-full.json (row count,
  state distribution, min/max/distinct numbers, six spot entries, and the
  product-integration-records zero-match); `git -C /tmp/dududu-audit
  for-each-ref` over `refs/remotes/origin` and `refs/heads`,
  `git symbolic-ref refs/remotes/origin/HEAD`, `git log -1` on origin/main, and
  `git merge-base --is-ancestor` + `git rev-list --left-right --count` for the
  four non-default branches; `test -d /tmp/dududu-audit/.git` and
  `test -e /home/hoo/Projects/dududu`.
- UNKNOWN items: none.
