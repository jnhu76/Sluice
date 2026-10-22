# MW-HW-1 environment and baseline freeze (WSL2)

Research-only round. No product change on master; candidates live in isolated
worktrees and one OPEN research PR.

## Host and kernel

| Item | Value |
|---|---|
| Host | WSL2 on AMD Ryzen 7 5800H (Zen 3), 8 logical CPUs visible to WSL2 |
| Kernel | 6.18.54.1-microsoft-standard-WSL2 (SMP PREEMPT_DYNAMIC, built Oct 6 2026) |
| Memory | 15 GiB |
| Governor | not exposed under WSL2 (`scaling_governor` absent) — no host tuning attempted |
| Affinity | all measurements pinned `taskset -c 2`; no cset isolation; SMT sibling not isolated |
| Filesystems | ext4 (repo), tmpfs (`/tmp`: fixture + worktrees + outputs) |
| Load | interactive WSL2 session; loadavg sampled 0.3–1.2 during runs |

## Toolchain and tools

| Tool | Version / status |
|---|---|
| gcc | 15.2.0 (Ubuntu) — harness build; xmake release libs |
| clang++ | 21.1.8 (unused) |
| xmake | 3.1.2 |
| liburing | system headers + `/usr/lib/x86_64-linux-gnu/liburing.so` (capability probe OK) |
| perf | 6.18.54.1.g26d1ceb55087; `perf_event_paranoid=2`, `kptr_restrict=1` — user-space events only, no kernel symbols/tracepoints |
| strace | 6.19 (census only; traced wall time never used as a result) |
| bpftrace | 0.25.0 present; kernel attribution blocked by `perf_event_paranoid=2` — not used |
| pidstat | MISSING (sysstat not installed) — `/usr/bin/time -v` + `/proc/<pid>/status` used instead |

`KERNEL_ATTRIBUTION_UNAVAILABLE`: WSL2 + paranoid=2 hides kernel symbols and
tracepoints; system-CPU time is reported from process accounting only. Kernel
side shares are never labelled with function names.

## Baseline identity

| Item | Value |
|---|---|
| BASE SHA | `8e2aa5ab94670a318288c77da796825cfa9f3cfe` (master, merge of PR #565 H2b) |
| Working tree | clean at measurement time |
| Build | `xmake f -m release --liburing=y` ; `xmake build sluice_core sluice_async` |
| Harness | `bench/native-linux-m2/pipeline_ab.cpp` (unmodified M2 harness), built `g++ -std=c++20 -O3 -DNDEBUG` |
| `pipeline_ab_base` SHA-256 | `c570cae83eda14d44cebeeef6977a02e534482bbab44ad7a3918c743b4058895` |
| `libsluice_async.a` SHA-256 | `4cf33a45ab5b8ad75565bb49ada80fbd6a706e5f42442f09c3a328dbf49f860e` |
| `libsluice_core.a` SHA-256 | `9e745439b8d437b2231443c937eb935d964007e4cb4dcb63884042fae84117d8` |
| Fixture | 64 MiB tmpfs pattern file `/tmp/mw-hw-1-out/fixture/data.bin` (harness `--create-fixture`) |

Experiment arms (single-mechanism patches on isolated worktrees, each
fast-forwarded from BASE):

| Arm | Branch | Commit | Mechanism |
|---|---|---|---|
| e1 | `research/mw-hw-1-e1-pending-control-skip` | `9af4d46ac753f7a4d5ac7a84b5438501789f9396` | derived `pending_control_count_`, skip empty pending-control sweep |
| e2 | `research/mw-hw-1-e2-direct-dispatch` | `e545664676c48ab592d195bf7e90f51505bf2f7e` | direct dispatch when dispatch queue empty (falsifier) |
| e3 | `research/mw-hw-1-e3-publication-snapshot` | `0176e10a36154c157985049ef132061868f610f4` | single-lock publication snapshot (falsifier) |

Arm binary SHA-256 values: `ab/arm-identity.txt` and `SHA256SUMS.txt` in the
raw output directory (see README).

## Scope limits

- WSL2 is exploration/screening only. No native-Linux product performance gate
  is claimed or passed by anything in this round.
- No global kernel configuration, cache dropping, governor change or module
  install was performed.
- The harness does not accept `--block=0`, so the zero-op (K>0 owed) workload
  has no A/B row in this round; its correctness side is covered by the owed
  suites and the E1 parity oracle (E1 never touches the owed-delivery path).
- `strace` ptrace overhead (~2x wall) means census counts are the only quantity
  taken from traced runs.
