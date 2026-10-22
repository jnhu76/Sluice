# Review B — Measurement / Methodology (Gate R8)

Independent review agent B — measurement/methodology (arm identity, runner
fail-closed discipline, W-02/W-03/P5 A/B statistics, P5 scope, perf+strace
cost attribution). Date: 2026-10-22. Adversarial recomputation from recorded
raw data; no benchmarks re-run (protocol reuse rule), no builds (Reviewer A
owns the build trees), normal user only.

Campaign SHAs: BASE `381756370980f75e4beaf47310db4b8b9a99053d`
(`~/sluice-e1-e5/base`), E1 `2caead59f9317665510af72968439f1252bc5bb9`
(`~/sluice-e1-e5/candidate`, branch `research/native-linux-e1-pending-control-skip`,
single commit on `38175637`). Raw data: `~/sluice-e1-e5/results{,-w03,-p5}/`;
frozen scripts: `~/sluice-e1-e5/scripts/`.

## Independently recomputed

### Arm identity (sha256sum recomputed by this review)

| Artifact | My sha256 | Matches |
|---|---|---|
| BASE `libsluice_async.a` | `76e61047e18e5672bcd1e358f0199e08a006ca2678e5ecbd872d8e3898199afa` | arm-identity.txt, runner pin, **and `~/h2b-e5-results/arm-identity.txt` (H2b receipt confirmed byte-equal)** |
| E1 `libsluice_async.a` | `bb6ccd6ccc6c9ed06e231e966e5f21129e86126db9e8e6b3b5eb021b3e4fd351` | arm-identity.txt, runner pin |
| BASE `libsluice_core.a` | `489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a` | arm-identity.txt |
| E1 `libsluice_core.a` | `489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a` | identical core libs, as claimed |
| `e1-pending-control-skip.patch` | `ea217da4252a885f73889ba28b569f848bf050f8913b61fa69fa8bd1b385695f` | identity.md, arm-identity.txt |
| W-02 harness binaries | `4266b54d…` / `72e9807b…` | identity.md, arm-identity.txt, provenance |

- Worktrees: `git rev-parse HEAD` matches both pins; `git status --porcelain`
  empty on both (re-verified now). `master` = `origin/master` = `38175637`.
- `git show --stat 2caead59`: exactly the 5 claimed files (uring_backend.hpp
  +3, uring_test_seams.hpp +16, uring_transport.cpp +30/−3, new parity test
  +295, xmake/tests.lua +8). No extra content.
- Library mtimes 16:13:35, harness binaries 16:43, campaign window
  16:55:04–16:57:38 (+0800): artifacts were not touched during or after the
  campaign, so the start-of-campaign hash gate plus unchanged mtimes give
  effective identity (see Finding P3-7 for the per-run drift gap).
- All files in `evidence/` are byte-identical (`cmp`) to the raw copies in
  `~/sluice-e1-e5/results{,-w03,-p5}/`; `ab.jsonl` sha256
  `076dd3019a6d5eec…` equals `provenance.txt:ab_jsonl_sha256` (recorded by
  the runner at campaign finish). `w03-combined.jsonl` = `w03.jsonl` +
  `w03-verify.jsonl` byte-exact (`cat` + `cmp`), sha `9ed02432…` matches
  `provenance-w03.txt`.

### Runner discipline (read line-by-line: run_e1_e5.sh, pipeline_ab_e1.cpp, bench_common.hpp)

Confirmed fail-closed: AB/BA rotation by rep parity (run_e1_e5.sh:139; data
confirms odd reps AB, even BA, and order effect on the median ratio is
negligible: AB 0.8938 vs BA 0.8977); per-rep pairing in the reducer
(e1_verdict.py:159-164, keyed `(rep,cfg,arm,phase)`); worktree SHA + clean
tree + path-overlap + same-library refusal gates; binary/lib/fixture hash
grep against arm-identity.txt; `ab.jsonl` exists ⇒ refuse; per-row JSON
validation (ok, known phase/arm, finite positive timings, zero
completion_errors/invalid_state, ops==requests) with `die` on any violation;
harness exits nonzero on any phase failure and the oracle phase verifies
every byte via `verify_pattern` (with an in-process oracle self-test that
proves the verifier discriminates); successful warmups emit no row (0 warmup
rows in 990); post-campaign cell-completeness check demands exactly one
timed+oracle+latency row per (rep,cfg,arm). Primary metric is
`CLOCK_PROCESS_CPUTIME_ID`-based process CPU per op (bench_common.hpp:42),
immune to preemption. **No path found that lets a biased sample through.**

### W-02 (my recomputation from `ab.jsonl`, own python)

Row counts: 990 rows = 11 cfg × 2 arms × 15 reps × 3 phases; 990 unique
cells, 0 duplicates, 0 missing, 0 warmup rows; 0 rows violating
ok/errors/ops/max_outstanding==depth; all 330 oracle rows ok.

| Config | my median ratio | claimed | my wins <1 | claimed | my max pair |
|---|---:|---:|---:|---:|---:|
| 4096-1024-1 (headline) | **0.6930** | 0.6930 | 15/15 | 15/15 | 0.7109 |
| 4096-1024-8 (headline) | **0.6239** | 0.6239 | 15/15 | 15/15 | 0.6571 |
| 4096-1024-64 | 0.5928 | 0.5928 | 15/15 | 15/15 | 0.6157 |
| 4096-256-1 | 0.8723 | 0.8723 | 15/15 | 15/15 | 0.8963 |
| 4096-64-1 | 0.9533 | 0.9533 | 15/15 | 15/15 | 0.9868 |
| 4096-64-64 | 0.8977 | 0.8977 | 15/15 | 15/15 | 0.9255 |
| 4096-16-8 | 0.9724 | 0.9724 | 14/15 | 14/15 | 1.0034 |
| 4096-16-1 (control) | 0.9962 | 0.9962 | 9/15 | 9/15 | 1.0262 |
| 65536-1024-1 | 0.8612 | 0.8612 | 15/15 | 15/15 | 0.9071 |
| 65536-1024-8 | 0.9171 | 0.9171 | 15/15 | 15/15 | 0.9567 |
| 1048576-64-8 (control) | 0.9958 | 0.9958 | 10/15 | 10/15 | 1.0227 |

Every claimed number reproduces to 4 decimals. Gates: headline medians
≤ 0.95 with 15/15 ≥ 12 wins — PASS both configs. NONREG: all 9 non-headline
medians ≤ 1.03 (controls 0.9962 / 0.9958) — PASS. Dose response monotone:
C16 0.9962 → C64 0.9533 → C256 0.8723 → C1024 0.6930. D1 ratio spread
0.6616–0.7109, D8 0.5999–0.6571 (docs claimed 0.662–0.711 / 0.600–0.657 ✓).
No pair anywhere exceeds 1.03 (w02-ab.md's "no single pair above 1.03
outside the C16 control" is if anything understated — even C16's max is
1.0262). p95 latency ratios (latency phase, my recomputation): all medians
≤ 1.05, controls 0.9825 (C16) and 1.0003 (1MiB); max individual pair 1.0886
(C16-D1, control noise, no gate applies). rusage split at headline:
utime ratio 0.455, stime ratio 0.842 (see P3-5).

### Frequency / noise

frequency.jsonl (330 rows, one per harness run): freq_before 1.197–3.500 GHz,
freq_after 1.197–3.489 GHz, median drop 259 MHz; loadavg1 0.78–1.55 (median
1.35 — see P3-4). E1 runs record a higher median freq_after (3.19 GHz) than
BASE (2.89 GHz) — expected: less work per wall second ⇒ longer sustained
boost. Could the 0.69 be a frequency artifact? No: the `perf stat` `:u`
instruction counts (100.00 time-enabled, no multiplexing on every run) are
frequency-independent and show an 80.7% instruction reduction at headline,
exactly accounted for by the removed scan iterations (below), the controls
sit at 0.996, and the dose response is monotone in C. Frequency noise
connot plausibly produce that signature. No SMT/CPU isolation was used;
environment.md records the permission boundary honestly ("no CPU isolation"),
though the "SMT sibling 12 idle during campaigns" claim is plausible but not
evidenced by any archived per-sibling sample (P3-13).

### perf / mechanism / strace attribution (all recomputed)

perf runs carry exactly 20,000 ops each (raw counts / 20000 reproduce every
per-op figure; 1MiB config: /500). All 8 runs report 100.00 time-enabled.

| Config | instr/op base→e1 (my ratio) | claimed | cycles ratio (my) | claimed |
|---|---|---:|---:|---:|
| 4096-1024-1 | 120,665.5 → 23,233.3 (**0.19253**) | 0.193 | 0.34714 | 0.347 |
| 4096-1024-8 | 70,245.7 → 16,033.6 (0.22825) | 0.228 | 0.39184 | 0.392 |
| 4096-16-1 | 25,270.0 → 23,111.3 (0.91456) | 0.915 | 0.98422 | 0.984 |
| 1048576-64-8 | 168,244.5 → 164,385.2 (0.97687) | 0.977 | 0.99570 | 0.996 |

Branch ratios 0.248/0.278/0.932/0.968 all reproduce. Cross-check: removed
instructions 120,665−23,233 = 97,432/op; instrumented scan iterations
173,317,120/20,000 = 8,665.9/op ⇒ ~11.24 instr/iteration — the arithmetic
claim in cpu-attribution.md is correct.

mechanism-scans.txt: BASE `iters == calls × capacity` **exactly** on all 11
configs (e.g. C1024/D1: 169,255×1024 = 173,317,120; 1MiB: 2,633×64 =
168,512); E1 `scans=0, iters=0, skips==calls` on all 11. Per-op values match
the doc table (8,666, 4,814, 150, 595, 2,288, 4,192, 5,167, 337) to the
displayed precision.

strace census: 80,004 `io_uring_enter` on both arms at both censused configs
(4096-1024-1, 4096-16-1) = 1 enter/op over 80K ops + 4 setup enters; 4
`io_uring_setup`, 4 `eventfd2`, 1 `futex`, identical shape both arms. The
docs correctly treat strace as a census (strace time never used as perf
time). No inconsistency between strace counts and perf/ab numbers.

### W-03 (my recomputation from `w03.jsonl`, 180 perf + 10 verify rows)

`w03-combined.jsonl` is the byte-exact concatenation (sha `9ed02432…` as
recorded) and the frozen reducer, fed the combined file, reproduces the
published medians. Verify rows: 10/10 (5 scenarios × 2 arms), all
`ok=true`, all `no_lost_wake=true`, `loop_kind=epoll+eventfd` on every row.

| Config | rank-paired (reducer/claimed) | **rep-paired (my recomputation)** | ratio of medians | rep wins | max rep pair |
|---|---:|---:|---:|---:|---:|
| 4096-1024-1 | 0.8587 | **0.8508** | 0.8651 | 15/15 | 0.9237 |
| 4096-1024-8 | 0.6171 | **0.6146** | 0.6137 | 15/15 | 0.6468 |
| 65536-1024-8 | 0.9206 | **0.9118** | 0.9228 | 14/15 | 1.2625 |

See Finding P2-1: the frozen reducer pairs rank-to-rank
(`e1_verdict.py:232`), not per-rep as protocol.md:78-80 preregisters. The
rep-paired numbers are slightly *stronger*; win counts are identical under
both pairings; all medians ≤ 1.03 either way; no gate outcome changes. One
64KiB pair regressed 1.26 (median unaffected; not claimed otherwise).

### P5 (my recomputation from `p5.jsonl`, 30 timed rows)

`p_events = 0` and `p_max = 0` on every row; `invariant_ok` on every row;
odd reps AB, even BA. BASE median cpu_ns/op 4872.2 (matches the doc).

| Pairing | median ratio | wins <1 | max pair |
|---|---:|---:|---:|
| rep-paired (as claimed) | **0.9907** | 9/15 | 1.1033 |
| rank-paired (reducer method) | 0.9931 | 11/15 | — |

The claimed 0.9907 (9/15) is the rep-paired median and reproduces exactly.
All ≤ 1.05. Correctly, `verdict.json` records `p_positive = NOT_MEASURED`
("P>0 never actually occurred") — the 0.9907 measures only the P=0
cancel-heavy path and must not be cited as a P>0 cost result; the docs say
exactly this. Caveats: the archived `p5-probe.jsonl` holds a single 2-sample
probe row, not the "4 runs, 420+ samples" cited in pending-control-pressure.md
(P3-9); and the ≤1.05 line was preregistered for P>0 cost, so quoting it for
this P=0 congestion A/B is informal (the NOT_MEASURED label contains the
damage).

### Verdict / threshold integrity

Thresholds agree three ways (protocol.md:11-26, e1_verdict.py:37-43,
verdict.json:thresholds): headline ≤0.95 & ≥12/15; NONREG ≤1.03; W-03 ≤1.03
& MIN_PAIRS=15 & verify no_lost_wake; P>0 ≤1.05 with p_events>0 required;
11-config multiple-comparison note present; correctness overrides
performance; CONDITIONAL cap implemented (e1_verdict.py:335-340). No
negative control was dropped, no best-sample selection exists (all 15 pairs
published per config). I re-ran the frozen reducer on the recorded inputs:
**verdict.json reproduces byte-identically**, selftest 14/14, exit 0,
`w02=PASS w03=PASS p5=NOT_MEASURED overall=CONDITIONAL`, `issues=[]` —
consistent with my recomputation. Environment claims verified against the
live host: kernel 7.2.9-201.fc44.x86_64, GCC 16.2.1, E5-2666 v3 10C/20T,
`perf_event_paranoid=2` (justifying `:u`), kptr_restrict=0, tracefs
unreadable, liburing 2.13-2.fc44, schedutil/intel_cpufreq — all as recorded.

## Findings

No P1 findings. The headline numbers, controls, verdict and identity
receipts all reproduce exactly by independent recomputation.

1. **P2-1 — W-03/P5 statistics in the frozen reducer are rank-paired, not
   the preregistered per-rep pairing.** `e1_verdict.py:232` and `:282`
   compute `zip(sorted(e), sorted(b))`; protocol.md:77-82 preregisters
   "true per-rep pairing (E1 row of rep i vs BASE row of rep i, same config,
   same AB/BA rotation slot)". w03-ab.md's table is titled "Paired E1/Base
   ratios" with a "pairs < 1" column but its medians are rank-paired
   quantile ratios. My rep-paired recomputation gives 0.8508 / 0.6146 /
   0.9118 (slightly stronger; identical win counts 15/15/14; all ≤ 1.03), so
   no gate or verdict changes — but the published numbers mislabel their
   method, and the P5 figure (0.9907, 9/15) *is* rep-paired, i.e. the two
   docs use different pairings without saying so. Fix in-round: republish
   the W-03 table with rep-paired medians (or label the method per column)
   and add one sentence to w03-ab.md. The reducer should not be edited
   post hoc; a note in the docs suffices for this round.
2. **P2-2 — identity.md records a wrong sha for the W-02 runner.**
   identity.md:58 lists `run_e1_e5.sh` = `b3063d600af12cad…`; the file that
   ran (and its docs copy) is `2de6822577f193285…`, which is exactly what
   `results/provenance.txt:runner_sha256` recorded at campaign start. The
   campaign evidence chain is internally consistent (provenance matches the
   current file), but an identity-gate document containing a false receipt
   must be corrected. (All other four frozen-framework shas in identity.md
   verify.)
3. **P2-3 — the W-03/P5 binary-identity receipts do not exist as claimed.**
   identity.md:49-52 says the `external_loop_ab_e1_*` and
   `pending_control_pressure_*` binary hashes are "recorded in
   `~/sluice-e1-e5/results*/arm-identity.txt` and the per-campaign
   provenance files". They are not: `results-w03/arm-identity.txt` and
   `results-p5/arm-identity.txt` are byte-identical copies of the W-02 file
   and pin the *pipeline* harness hashes (`4266b54d…`/`72e9807b…`), which
   never executed in those campaigns; neither `run_e1_w03.sh` nor
   `run_e1_p5.sh` hashes binaries or libraries (existence checks only), and
   neither checks the fixture hash (existence only) — protocol.md:61-64's
   "runner re-verifies every gate at start (… stale binaries, fixture hash,
   same-library refusal)" holds for the W-02 runner but not the W-03/P5
   runners. Mitigations: the harness *source* shas are recorded per campaign,
   the binaries are byte-identical across `results/`, `results-w03/`,
   `results-p5/` (verified by sha256), and the worktree SHA/clean-tree gates
   did run. Fix in-round: replace the copied arm-identity files with real
   ones (or append the actually-used binary hashes) and correct the
   identity.md sentence.
4. **P3-4 — environment.md:19 misstates campaign load.** "load average
   0.15–0.4 during campaigns" contradicts frequency.jsonl's per-run loadavg
   0.78–1.55 (median 1.35); the 0.15 reading is the idle capture in
   environment.txt taken before the campaigns. Actual load is consistent
   with the pinned benchmark process itself and no external contention, so
   the fix is editorial.
5. **P3-5 — "stime roughly unchanged" overstates.** cpu-attribution.md:56-58
   vs my recomputation from ab.jsonl: headline stime_ns_per_op median
   4622 → 3892 (ratio 0.842). Probably rusage stime tick-sampling noise, but
   the doc should carry the number instead of "roughly unchanged".
6. **P3-6 — perf/strace run shape is not documented.** The per-op figures
   imply exactly 20,000 ops per perf run and 80,004 enters (4×20,000 + 4
   context setups) per strace run; the actual commands and phase coverage
   are nowhere recorded. Reproducible only by inference; add the commands.
7. **P3-7 — lib/binary drift checks run once at campaign start only**, not
   around each run; a mid-campaign rebuild would go undetected by the
   runner (mitigated here by artifact mtimes predating the window).
8. **P3-8 — the reducer never compares `ab_jsonl_sha256` (provenance.txt)
   against the actual file**; tamper-evidence is recorded but unchecked. I
   verified it manually (`076dd301…` matches).
9. **P3-9 — probe/census evidence thinner than the text.** The "4 runs,
   420+ samples, 0 sticky observations" claim (pending-control-pressure.md)
   is backed in the archive by a single 2-sample row in p5-probe.jsonl plus
   an unarchived round log; `w03-combined.jsonl` itself is absent from
   `evidence/` (reproducible by concatenation, which I verified). The
   sticky-not-observed conclusion is independently supported by all 30
   campaign rows (p_events=0, p_max=0), so this is an evidence-archiving
   gap, not a result problem.
10. **P3-10 — preregistration is not VCS-verifiable.** The entire
    `docs/research/mw-hw-1/native-linux-e1/` directory is untracked in git
    (no commit on any branch contains it), and protocol.md's mtime (17:38)
    postdates the campaigns (16:55–17:26). The strongest preregistration
    evidence available is `e1_verdict.py` (mtime 16:52, pre-campaign) whose
    constants match the protocol and which reproduces verdict.json
    byte-identically. Commit the protocol before the next campaign.
11. **P3-11 — no p95 gate is actually preregistered.** protocol.md emits p95
    but gates nothing on it (the "P>0 ≤ 1.05" line is a different
    threshold); the reducer gates nothing on p95. Recomputed p95 ratios pass
    anyway (see W-02 section). Recorded so no one later believes a p95 gate
    was frozen.
12. **P3-12 — mech instrumentation source not archived.**
    `mech-instrument-e1.patch` is an explicitly "illustrative sketch"
    truncated before the real patch; the base-side instrumentation source is
    archived nowhere. mechanism-scans.txt is internally exact (iters ==
    calls × capacity on all 11 base configs) but the instrumented builds are
    not reproducible from the archive.
13. **P3-13 — unverifiable environment claims.** The tmpfs fixture no longer
    exists (its recorded sha can't be re-checked now, only the campaign-time
    gate attests it), and "SMT sibling 12 idle during campaigns" has no
    archived per-sibling sample.

## Protocol deviations

- W-03/P5 statistics computed rank-paired by the frozen reducer vs the
  protocol's per-rep pairing (P2-1) — the only deviation that touches a
  published number; recomputation shows it is immaterial to every gate.
- W-03/P5 runners re-verified SHAs, clean trees and fixture *existence* but
  not binary/library hashes or fixture *hash*, unlike the W-02 runner and
  protocol.md:61-64's blanket description (P2-3).
- The w03 verify+perf concatenation into `w03-combined.jsonl` is declared in
  protocol.md itself ("Deviations / notes"), was done byte-exactly, and fed
  the unmodified frozen reducer — procedurally sound.
- tracefs/tracepoint attribution NOT_RUN, per the documented no-sudo rule —
  declared, not a hidden deviation.

## Verdict

**APPROVE_WITH_CONDITIONS.** Every measurement claim I tested reproduces
exactly by independent recomputation: all 11 W-02 medians and win counts to
4 decimals, the 0.6930/0.6239 headline with 15/15 pairs, neutral controls
(0.9962/0.9958), monotone dose response, the 0.19253 instruction ratio with
exact scan-iteration accounting, byte-identical 80,004-enter strace censuses,
the W-03 and P5 medians under their respective pairing methods, and a
byte-identical verdict.json re-produced from the frozen reducer. The
frequency record (1.2–3.5 GHz swings, E1 running hotter) cannot explain the
effect: the frequency-independent `:u` instruction counts and the exact
mechanism accounting establish it as removed user-space work, and the
CONDITIONAL cap for unmeasured P>0 is applied exactly as preregistered. The
conditions: fix the W-03 pairing-method labeling (P2-1), correct the two
false identity receipts (P2-2, P2-3), and correct or soften the load-average
and stime wording (P3-4, P3-5). None of these changes any gate outcome or
the verdict.

## Residual limitations

- Benchmarks were not re-run (protocol reuse rule; ~1h+ per campaign); all
  statistics are recomputed from the recorded raw data, whose integrity is
  supported by matching runner-recorded sha256 receipts and byte-identical
  evidence copies.
- No rebuilds were performed (Reviewer A owns the build trees); library
  identity rests on recomputed hashes + clean worktrees, not on an
  independent rebuild of `76e610…`/`bb6ccd6…`.
- The fixture is gone from `/tmp`; its content is attested only by the
  campaign-time hash gate.
- perf/strace invocation command lines were not archived; their op-count
  basis was inferred (exactly) from the raw counts.
- I did not audit the external-loop harness's full implementation (real
  epoll_wait/poll_progress behavior) — that is Reviewer A's semantic lane;
  I verified its row discipline, `loop_kind` enforcement, verify rows, and
  that the numbers it produced are internally consistent with the rest.
- `perf_event_paranoid=2` means the HW counters exclude kernel; the
  kernel-side story rests on the strace census and rusage split, as the docs
  honestly state; no tracefs evidence exists.
