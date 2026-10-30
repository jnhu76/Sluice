# Closure Baseline Snapshot

Frozen at: 2026-10-30 06:57:12 +0800 (CST) / 2026-10-29T22:57:12Z.

Scope: read-only live-state capture in the real environment. No push, merge,
branch deletion or file deletion was performed; `receipts/` untouched. This
file was committed to the audit branch `research/retention-wsl2-audit`
(worktree `/tmp/sluice-closeout-614`, HEAD `86277ece`) as the closure baseline
record.

## 1. Expected vs live comparison

| Item | Expected | Live (2026-10-30 06:57 +0800) | Match |
| --- | --- | --- | --- |
| Sluice `origin/master` | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | yes |
| PR 614 head | `86277ece0e3c7723f0aa2c8de7c1c892b7bd8f97` | `86277ece0e3c7723f0aa2c8de7c1c892b7bd8f97` | yes |
| PR 594 head | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | `2714aa65fe4837d30f5fa3fcdb27db38c23b7136` | yes |
| dududu `origin/main` | `12a8712eb5ff5f48e570b49b1e01ad5821837dca` | `12a8712eb5ff5f48e570b49b1e01ad5821837dca` | yes |

Result: no deviation against any expected SHA. No increments recorded.
Non-expected live facts observed and recorded below (not deviations): dududu
local `main` is `a71fe6f51011685fd9808a0a7f033c24a216c50a`, i.e. local `main`
differs from `origin/main`; PR 584 is CLOSED with `mergedAt: null`.

## 2. GitHub state

Commands and verbatim outputs:

```
$ gh pr view 614 --json state,headRefOid,mergeable
{"headRefOid":"86277ece0e3c7723f0aa2c8de7c1c892b7bd8f97","mergeable":"MERGEABLE","state":"OPEN"}

$ gh pr view 594 --json state,headRefOid
{"headRefOid":"2714aa65fe4837d30f5fa3fcdb27db38c23b7136","state":"OPEN"}

$ gh pr view 584 --json state,mergedAt
{"mergedAt":null,"state":"CLOSED"}
```

- PR 614: OPEN, head `86277ece0e3c7723f0aa2c8de7c1c892b7bd8f97`, mergeable `MERGEABLE`.
- PR 594: OPEN, head `2714aa65fe4837d30f5fa3fcdb27db38c23b7136`.
- PR 584: CLOSED, `mergedAt` null (closed without merge).

## 3. Sluice main repository (`/home/hoo/Projects/Sluice`, branch `master`)

```
$ git rev-parse origin/master
84e1b7d578c2c58a3c46033487d9e1ac7c639b8b

$ git for-each-ref --format='%(refname:short) %(objectname:short)' refs/remotes/origin
origin 84e1b7d5
origin/backup/pr-601-original-head-e4bb8833 e4bb8833
origin/backup/pr-612-original-head-70d48f5e 70d48f5e
origin/master 84e1b7d5
origin/research/575-native-e5-atomic-rmw e9baafb9
origin/research/593-key-path 2714aa65
origin/research/product-integration-records 4d621d91
origin/research/r6-ac1-vectored-semantics 999b5305
origin/research/retention-wsl2-audit 86277ece
```

`origin/research/593-key-path` (`2714aa65`) matches the PR 594 head;
`origin/research/retention-wsl2-audit` (`86277ece`) matches the PR 614 head.

Dirty state:

```
$ git status --porcelain
(no output — clean)
```

Worktrees:

```
$ git worktree list
/home/hoo/Projects/Sluice        84e1b7d5 [master]
/tmp/sluice-closeout-614         86277ece [research/retention-wsl2-audit]
/tmp/sluice-val-c1-plat          84e1b7d5 (detached HEAD)
/tmp/sluice-val-correctness      84e1b7d5 (detached HEAD)
/tmp/sluice-val-correctness-base ac995641 (detached HEAD)
/tmp/sluice-val-k594-retention   2714aa65 (detached HEAD)
/tmp/sluice-val-p2c1             84e1b7d5 (detached HEAD)
/tmp/sluice-val-r6ac1-reverify   84e1b7d5 (detached HEAD)
```

8 worktrees total: 1 main checkout plus 7 under `/tmp` (this audit worktree
plus 6 detached validation worktrees).

## 4. dududu repository (`/home/hoo/Source/dududu`)

```
$ git rev-parse origin/main main
12a8712eb5ff5f48e570b49b1e01ad5821837dca
a71fe6f51011685fd9808a0a7f033c24a216c50a

$ git for-each-ref --format='%(refname:short) %(objectname:short)' refs/remotes/origin
origin 12a8712
origin/main 12a8712
origin/r1-request-geometry 47cd197
origin/r5-async-host-probe 4eafbc0
origin/sluice-r0-closure cc053cf
origin/sluice-r0-e5-verdict cc321a4

$ git status --porcelain
(no output — clean)

$ git worktree list
/home/hoo/Source/dududu b07307e [r1-parquet-sluice-bench]
```

Facts: `origin/main` is `12a8712` (matches expectation); local `main` is
`a71fe6f5`, so local `main` differs from `origin/main`. The single worktree
is checked out on branch `r1-parquet-sluice-bench` at `b07307e`; working tree
clean.

## 5. Running processes

```
$ pgrep -af 'xmake|sluice|zcode' || true
941 /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/node /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/server-cli.js serve --supervisor --server-root /home/hoo/.zcode/server
961 /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/node /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/server-core.js 1
1317 /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/node /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/server-cli.js server proxy-stdio --server-root /home/hoo/.zcode/server
1407 zcode-cli
1554 zcode-cli
1563 zcode-cli
1569 zcode-cli
1579 zcode-cli
78923 zcode-cli
79026 zcode-cli
79034 zcode-cli
79048 zcode-cli
79052 zcode-cli
89870 /home/hoo/.zcode/server/releases/3.16.3-linux-x64-5062d871bd2a/runtime/node --max-old-space-size=256 /home/hoo/Projects/Sluice/.zcode/workflow-runs/dwfrun-9c3dadc1-44c8-4eb5-9716-0e62680c0c62.mjs
90320 /usr/bin/zsh -c ... (the shell running this pgrep probe itself)
```

Interpretation: no `xmake` build/test processes and no Sluice binaries are
running. Only the zcode host tooling (server, cli sessions) and the workflow
runner performing this audit (`dwfrun-9c3dadc1-…`) matched; the last line is
the probe's own shell.

## 6. E5 reachability probe (lightweight, no full manifest)

Command (credential value redacted from this record; the connection used
sshpass-supplied authentication over SSH):

```
$ sshpass -p <redacted> ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 \
    jnhu@192.168.31.75 'hostname; test -d /home/jnhu/sluice-593-research/evidence && echo EXISTS || echo MISSING; find /home/jnhu/sluice-593-research/evidence -type f | wc -l; du -sb /home/jnhu/sluice-593-research/evidence | cut -f1'
E5
EXISTS
6913
1423204159
```

- Host: `E5`, IP `192.168.31.75`, reachable (SSH login succeeded).
- `/home/jnhu/sluice-593-research/evidence` exists.
- File count: 6913 (`find -type f | wc -l`).
- Size: 1,423,204,159 bytes (`du -sb`, apparent size).
- Probe depth: lightweight only, per plan — no full file manifest was taken.

## 7. Counting rules (统计口径)

- The governance matrix covers 11 branches and does NOT include dududu's
  default `main` nor this audit branch (`research/retention-wsl2-audit`).
- The live ref snapshot counts in this document are a different, separately
  labeled scope: Sluice `refs/remotes/origin` shows 9 entries (8 named
  remote branches — including this audit branch — plus the bare `origin`
  remote-HEAD entry); dududu `refs/remotes/origin` shows 6 entries (5 named
  remote branches plus the bare `origin` entry).
- Worktree counts: Sluice 8 worktrees (1 main + 7 under `/tmp`); dududu 1.
- E5 byte figure is `du -sb` apparent size, not disk-block usage.
- All timestamps are local CST (+0800) with UTC given for the freeze moment.

## 8. Dirty-state summary

| Location | State |
| --- | --- |
| `/home/hoo/Projects/Sluice` (master) | clean |
| `/home/hoo/Source/dududu` | clean |
| `/tmp/sluice-closeout-614` (research/retention-wsl2-audit) | clean before this snapshot file was added |
