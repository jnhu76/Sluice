# E1 Native-Linux Validation — Environment (Gate R0)

Round: E1 native-Linux A/B validation on host E5, 2026-10-22.
Captured verbatim record: `~/sluice-e1-e5/results/environment.txt`.

| Item | Value |
|---|---|
| Host | `E5` (jnhu@192.168.31.75, worked on directly, no SSH hop needed this round) |
| OS | Fedora Linux 44 (KDE Plasma Desktop Edition) |
| Kernel | 7.2.9-201.fc44.x86_64 |
| CPU | Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz, 10C/20T, 1 NUMA node |
| Pinned CPU | 2 (SMT sibling 12 — idle during campaigns; governor `schedutil`, driver `intel_cpufreq`) |
| Compiler | g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2) |
| xmake | v3.1.2+HEAD.5d21e9d |
| liburing | 2.13 (dev + runtime) |
| perf | 7.2.9-201.fc44.x86_64, `perf_event_paranoid=2`, `kptr_restrict=0` |
| strace | 7.2 |
| tracefs | NOT readable by normal user; tracepoint attribution = NOT_RUN (protocol forbids sudo / global permission changes) |
| Memory | 62 GiB total, ~59 available; per-run 1-min loadavg during W-02 campaigns 0.78–1.55 (median 1.35, `frequency.jsonl`, 330 samples) — the box was not idle during the round |
| perf quality | All `perf stat` runs 100.00 time-enabled (`:u` events, no multiplexing) |

Experiment tree (absolute paths on E5; left in place after the round):

```
/home/jnhu/sluice-e1-e5/base         BASE worktree  (master 38175637)
/home/jnhu/sluice-e1-e5/candidate    E1 worktree    (2caead59 = BASE + e1 patch)
/home/jnhu/sluice-e1-e5/scripts      frozen runner/reducer/harness sources
/home/jnhu/sluice-e1-e5/results      W-02 campaign + R1 semantic + attribution evidence
/home/jnhu/sluice-e1-e5/results-w03  W-03 campaign
/home/jnhu/sluice-e1-e5/results-p5   P>0 campaign + seam probe
/home/jnhu/sluice-e1-e5/mech-base    instrumented BASE copy (scan counters; evidence-only)
/home/jnhu/sluice-e1-e5/mech-e1      instrumented E1 copy  (scan counters; evidence-only)
/home/jnhu/sluice-e1-e5/smoke*       pre-formal smoke campaigns
```

Pre-existing directories `~/Sluice-e5*` and `~/h2b-e5-results` (H2b round) were
not touched. The BASE arm's `libsluice_async.a` reproduces the H2b E5 receipt
byte-for-byte (`76e61047…`), confirming the frozen-master build is reproducible
and that the new BASE fully inherits the merged H2b productization.

Fixture: `/tmp/sluice-e1-fixture/data.bin` (64 MiB tmpfs, pattern-filled,
sha256 `1cdf83d6…`, verified by every campaign run).

Permission boundary honored: all experiments ran as normal user `jnhu`; no
sudo, no sysctl, no governor change, no CPU isolation, no kernel module or
service adjustment.
