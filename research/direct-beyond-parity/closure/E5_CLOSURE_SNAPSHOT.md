# E5_CLOSURE_SNAPSHOT.md — R0 Environment & Git Snapshot (live-probed)

- Probe time (UTC): 2026-10-28T23:25:30Z
- Prober: E5 R0 environment/git probe (dynamic-workflow subagent). All values
  below are from commands executed in this session; no historical report was
  substituted. Read-only session: no PR merged, no ref rewritten, no file
  modified except this snapshot. `git fetch origin master` was run once in the
  Sluice repo (origin/master already equal, no ref change); in dududu only
  non-mutating `git ls-remote` was used because local tracking refs are stale
  by design of the probe boundary.

## 1. E5_HOST_IDENTITY

| Item | Value | Command |
|---|---|---|
| hostname | `E5` | `hostname` |
| kernel | `Linux E5 7.2.11-200.fc44.x86_64 #1 SMP PREEMPT_DYNAMIC Thu Oct 15 15:58:58 UTC 2026 x86_64 GNU/Linux` | `uname -a` |
| distro | Fedora Linux 44 (KDE Plasma Desktop Edition), VERSION_ID=44, support ends 2027-05-19 | `cat /etc/os-release` |
| CPU model | Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz | `lscpu` |
| topology | 1 socket, 10 cores, 2 threads/core (SMT on), 20 logical CPUs; NUMA: 1 node (0-19) | `lscpu` |
| cache | L1d 320 KiB (10 inst), L2 2.5 MiB (10 inst), L3 25 MiB (1 inst) | `lscpu` |
| notable flags | avx2, bmi1/2, rdrand, arch_perfmon, pebs, cqm | `lscpu` |
| nproc / memory | 20; MemTotal 65676312 kB (~62.6 GiB); swap: zram 8G | `nproc`, `/proc/meminfo`, `swapon --show` |
| CPU governor | `schedutil` (adaptive; not pinned performance) | `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` |

## 2. Toolchain & environment

| Item | Version | Command |
|---|---|---|
| glibc | GNU libc 2.43 | `ldd --version` |
| gcc | 16.2.1 20260819 (Red Hat 16.2.1-2) | `gcc --version` |
| clang | 22.1.8 (Fedora 22.1.8-4.fc44) | `clang --version` |
| xmake | v3.1.2+HEAD.5d21e9d | `xmake --version` |
| binutils | GNU ld/as/ar 2.46.1-1.fc44; objdump/nm/readelf present at /usr/sbin | `ld --version` 等 |
| liburing | 2.13 (liburing-2.13-2.fc44.x86_64, liburing-devel installed; `/usr/include/liburing.h` present; `pkg-config --modversion liburing` = 2.13) | `rpm -q`, `pkg-config` |
| io_uring sysctl | `/proc/sys/kernel/io_uring_disabled` = `0` (enabled) | `cat` |

## 3. PMU / perf

- `/proc/sys/kernel/perf_event_paranoid` = `2` — user-space measurement allowed;
  kernel-space PMU events restricted. (BLOCKER-ADJACENT: any kernel-mode
  profiling beyond user space is not possible without privileges; sudo forbidden.)
- `perf --version` → perf version 7.2.11-200.fc44.x86_64.
- Functional check: `perf stat -e cycles,instructions true` succeeded (user
  events counted, e.g. 145,835 instructions:u). perf is USABLE for user-space
  events on this host.
- `/proc/sys/kernel/perf_cpu_time_max_percent` = 25.

## 4. fio / DuckDB availability

- **fio: NOT INSTALLED (confirmed).** `command -v fio` → not found;
  `rpm -q fio` → `package fio is not installed`. This blocks the #578 fio
  native baseline until fio is installed (Owner/authorization required — sudo
  is forbidden in this workflow).
- `command -v duckdb` → not found (DuckDB CLI absent; R0 must build from
  pinned upstream source per plan).
- `command -v strace` → /usr/sbin/strace (present).

## 5. Filesystem & storage

| Mount | Source | FSTYPE | Options | Space |
|---|---|---|---|---|
| `/home` (repo root) | /dev/sda3[/home] | btrfs | rw,relatime,seclabel,**compress=zstd:1**,ssd,discard=async,space_cache=v2 | 445G total, 56G used, **387G avail** (13%) |
| `/` , `/var/tmp` | /dev/sda3[/root] | btrfs | same family as above (subvol=/root) | 387G avail |
| `/tmp` | tmpfs | tmpfs | rw,nosuid,nodev,seclabel,nr_inodes=1048576,inode64,usrquota | **32G avail** (RAM-backed) |
| `/boot` | /dev/sda2 | ext4 | rw,relatime,seclabel | — |

Scratch space available: `/tmp` (tmpfs, 32 GiB, RAM-backed — page-cache-free
identity), `/var/tmp` and `/home/jnhu` (btrfs, 387 GiB). FS identity note:
the repository lives on **btrfs with zstd:1 compression**; benchmark file
system identity must record this.

Commands: `df -T`, `findmnt -T`, `df -h`, `findmnt -t …`.

## 6. GitHub live state (`gh auth status` + `gh pr/issue view` + `git ls-remote`)

- gh auth: logged in as **jnhu76** (https protocol; scopes gist, read:org,
  repo, workflow). OK.

### Sluice (jnhu76/Sluice)

| Ref | Value | Evidence |
|---|---|---|
| master (local HEAD) | `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` | `git rev-parse HEAD/master` |
| master (origin tracking) | same | `git fetch origin master && git rev-parse origin/master` |
| master (remote, live) | same | `git ls-remote origin refs/heads/master` |
| **PR #601** | **OPEN**, base `master`, head branch `sluice-600-direct-research`, head SHA `e4bb8833699d0ca5dee191883f3fec6d41b221b2`, not draft, updatedAt 2026-10-26T16:51:16Z | `gh pr view 601 --json …` |
| **PR #602** | **OPEN**, base `master`, head branch `sluice-600-direct-c1`, head SHA `77e5399a45998dea396699fa2e063c94a665945a`, not draft, updatedAt 2026-10-26T16:51:50Z | `gh pr view 602 --json …` |
| remote `refs/heads/sluice-600-direct-research` | `e4bb8833…` (= PR #601 head) | `git ls-remote` |
| remote `refs/heads/sluice-600-direct-c1` | `77e5399a…` (= PR #602 head) | `git ls-remote` |

Issues (all queried live via `gh issue view N --json state,title`): all **OPEN** —

- #563 OPEN — research(arch/perf): MW-HW-0 minimal necessary work × hardware-aware design
- #578 OPEN — research(app-io): minimal fio + DuckDB native baseline and async host admission R0
- #600 OPEN — research(perf/direct): attribute and eliminate synchronous read_at overhead beyond pread
- #603 OPEN — research(perf/direct): Direct Beyond-Parity umbrella — P0-P6 execution
- #605 OPEN — Design decision: positional vectored read/write (AC-1, M1 Case A) — Awaiting Owner
- #606 OPEN — Design decision: gathered positioned read with per-request results (AC-2, M1 Case B/C + M2) — Awaiting Owner
- #607 OPEN — Design decision: explicit file-to-file copy (AC-3, M3 cfr-explicit engine) — Awaiting Owner
- #608 OPEN — Product slice: adopt P2C1 read-composition prevalidated register-resident steps (integration candidate A alone)

### dududu (jnhu76/dududu)

- **Remote default branch is `main`, not `master`.** There is NO `master` ref
  on the remote (`git fetch origin master` → `couldn't find remote ref master`;
  `gh repo view --json defaultBranchRef` → `main`).
- **Live remote state** (`git ls-remote --heads origin`): exactly one branch,
  `refs/heads/main` = `b94041d918672d113ea0ca667ba8ccdabf329c80`. The remote
  branch `r1-parquet-sluice-bench` **no longer exists** on the remote
  (`git ls-remote --exit-code origin refs/heads/r1-parquet-sluice-bench` →
  exit 2, no match). Pull refs present: pull/1 = 59cffffc, pull/2 = c4fe75f
  (merged, local origin/main = its merge a71fe6f), pull/3 = b07307ed.
- **Delta explanation**: remote `main` (b94041d9) is NEWER than the local
  clone's stale tracking ref `origin/main` = a71fe6f51011685fd9808a0a7f033c24a216c50a
  (main advanced after local last fetch — consistent with PR #3 having been
  merged remotely and the r1 feature branch deleted post-merge). Local
  ancestry of b94041d9 was NOT verified by fetch (probe did not mutate refs);
  recorded as-is.
- Local dududu: checked out `r1-parquet-sluice-bench` @
  `ed053cbe098605a40fcd6c9686d5361aa4d10b01` — **clean** (0 porcelain lines),
  no extra worktrees, stash count 0. Local branches: `main` (= stale
  origin/main a71fe6f), `p4-strategy-arms`, `r1-parquet-sluice-bench` (=
  stale origin/r1-parquet-sluice-bench tracking ed053cbe; remote branch
  deleted). Local `r1-parquet-sluice-bench` is **23 commits ahead** of local
  `origin/main` tracking ref (merge-base a71fe6f), **0 behind** — i.e. main
  (as locally known) is fully contained.

## 7. Anchor reconciliation (逐一实测)

| Anchor | Expected | Measured (local / tracking / remote-live) | Verdict |
|---|---|---|---|
| B0 | `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` | master local = origin/master = `git ls-remote` refs/heads/master, all equal | **MATCH** |
| B1 | `77e5399a45998dea396699fa2e063c94a665945a` | branch `sluice-600-direct-c1` = origin tracking = remote ls-remote = PR #602 headRefOid | **MATCH** |
| P2C1 | `deaf03014173e13633a43f4db405c9a9e9860f23` | branch `direct-read-compose-prevalidated-steps` = worktree HEAD `/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps` (deaf0301, clean tree) | **MATCH** |

**anchorDeltas = ALL_MATCH** (无 delta；逐位比对 via `git rev-parse` full-SHA
equality + `test … = …`).

## 8. Worktree & branch inventory

### Sluice local branches (`git branch -vv`)

- `direct-compose-stack-readpreval-writeexit` @ 4ab6ef17 (worktree; P5 A+B stack — mission: 禁止误用)
- `direct-cost-ladder` @ 4455d98d (worktree)
- `direct-read-compose-prevalidated-steps` @ **deaf0301** (worktree; P2C1 research)
- `direct-write-compose-exit-staging` @ 476bd9ce (worktree; P2C2)
- `master` @ bdfd0d8d = [origin/master]
- `pr-601` @ e4bb8833 (no worktree; = origin/sluice-600-direct-research = PR #601 head)
- `research/593-key-path` @ 2714aa65 = [origin/research/593-key-path] (worktree)
- `sluice-600-direct-c1` @ 77e5399a = [origin/sluice-600-direct-c1] (worktree)
- `sluice-600-direct-research` @ e4bb8833 = [origin/sluice-600-direct-research] (no worktree)

Stash: empty (0 entries).

### Sluice worktrees (`git worktree list`) — frozen pair marked READ-ONLY per mission

| Path | HEAD | Branch | Hygiene (porcelain) |
|---|---|---|---|
| `/home/jnhu/Source/Sluice` | bdfd0d8d | master | 1 untracked: `research/direct-beyond-parity/` only (612 files, 6.6M — matches stated ~612/6.6MB) |
| `/home/jnhu/Source/Sluice-600-base` | bdfd0d8d (detached) | — | **FROZEN READ-ONLY.** 1 untracked `build-baseline/` (build artifacts; untouched) |
| `/home/jnhu/Source/Sluice-600-wt` | 77e5399a | sluice-600-direct-c1 | **FROZEN READ-ONLY.** clean (0) |
| `/home/jnhu/Source/Sluice-direct-compose-stack-readpreval-writeexit` | 4ab6ef17 | direct-compose-stack-readpreval-writeexit | clean (0) |
| `/home/jnhu/Source/Sluice-direct-cost-ladder` | 4455d98d | direct-cost-ladder | 7 untracked research artifacts: `research/direct-cost-ladder/{ladder, ladder-4455d98d, ladder-ab, ladder-c1, ladder-c2, m3-support, mkfixture}` (untouched) |
| `/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps` | deaf0301 | direct-read-compose-prevalidated-steps | clean (0) |
| `/home/jnhu/Source/Sluice-direct-write-compose-exit-staging` | 476bd9ce | direct-write-compose-exit-staging | clean (0) |
| `/home/jnhu/sluice-593-research/worktrees/pr` | 2714aa65 | research/593-key-path | clean (0) |

No tracked-file modification anywhere; all dirt is untracked research/build
artifacts. `git clean` was NOT run anywhere.

### dududu worktrees

- `/home/jnhu/Source/dududu` @ ed053cbe [r1-parquet-sluice-bench] — clean, single worktree, stash 0.

## 9. Frozen SHA summary

```text
SLUICE_MASTER_SHA  = bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
DUDUDU_MASTER_SHA  = N/A — no remote `master` exists; default branch `main`
                     @ b94041d918672d113ea0ca667ba8ccdabf329c80 (live ls-remote);
                     local stale tracking `origin/main` @ a71fe6f51011685fd9808a0a7f033c24a216c50a
PR601_HEAD_SHA     = e4bb8833699d0ca5dee191883f3fec6d41b221b2  (OPEN, base master)
PR602_HEAD_SHA     = 77e5399a45998dea396699fa2e063c94a665945a  (OPEN, base master)
P2C1_RESEARCH_SHA  = deaf03014173e13633a43f4db405c9a9e9860f23
E5_HOST_IDENTITY   = E5 / Fedora 44 / 7.2.11-200.fc44.x86_64 / Xeon E5-2666 v3 10C20T 1-NUMA
```

## 10. Blockers / environment constraints

1. **fio NOT INSTALLED** (confirmed: `rpm -q fio` → not installed). #578 fio
   native baseline cannot start until fio is installed; sudo forbidden →
   needs Owner-authorized install.
2. **perf_event_paranoid = 2** — user-space perf works; kernel-space PMU
   events unavailable without privilege change (sudo forbidden).
3. **DuckDB CLI absent** — R0 DuckDB baseline requires build from pinned
   upstream SHA (network/versions to be handled by R5 owner per mission §8.2).
4. **dududu remote topology** — default branch is `main` (no `master`);
   remote `r1-parquet-sluice-bench` deleted; local tracking refs stale.
   Downstream dududu work must base on live remote `main` = b94041d9, not on
   the stale local `origin/main` = a71fe6f (re-fetch before branching).
5. **CPU governor `schedutil`** and **btrfs compress=zstd:1** on the repo
   filesystem — benchmark-affecting environment facts to be recorded in any
   formal measurement identity; not changed in this session.
6. Frozen worktrees `Sluice-600-base` and `Sluice-600-wt` are read-only per
   mission boundary; their untracked contents (`build-baseline/`) left as-is.
