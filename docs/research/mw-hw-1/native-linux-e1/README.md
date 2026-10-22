# E1 Native-Linux Validation (MW-HW-1)

Research round: independent, reproducible, falsifiable validation of the E1
`pending_control_count_` zero-count skip patch on bare-metal Linux (host E5),
per the protocol frozen in this round's task. Research-only: no production
code is changed by this branch, no PR is merged, Issue #563 stays open.

**Verdict: `CONDITIONAL`.**

- W-02 native A/B: **PASS** — headline 4KiB/C1024/D1 median ratio **0.6930**
  (15/15 pairs), D8 **0.6239** (15/15); controls C16 and 1MiB neutral
  (0.996 both); all non-regression gates met.
- W-03 real external loop: **PASS** — rep-paired (preregistered) D1 0.8508 /
  D8 0.6146 / 64KiB 0.9118 (reducer's rank-paired values: 0.8587 / 0.6171 /
  0.9206), 10/10 no-lost-wake verify rows on a real epoll+eventfd loop.
- P>0 sticky-cancel cost: **NOT_MEASURED on the real kernel** — sticky states
  never occurred on liburing 2.13 + kernel 7.2.9 (formal probe + archived
  12-run/1,731-sample supplement, 0 positives, 0 parity violations;
  mechanism documented); the real cancel-path congestion A/B was neutral
  (0.9907). Per the frozen protocol this caps the round at CONDITIONAL.
- Attribution: E1 removes 100% of router scans (instrumented), 80.7% of user
  instructions at the headline config, with byte-identical `io_uring_enter`
  counts — pure user-space redundant-work elimination; the remaining cost is
  syscall/kernel-dominated.

## Documents

| File | Gate | Content |
|---|---|---|
| [environment.md](environment.md) | R0 | Host, toolchain, permission boundary, experiment tree |
| [identity.md](identity.md) | R0 | SHAs, patch scope, library/binary hashes, campaign receipts |
| [protocol.md](protocol.md) | R2/R7 | Preregistered thresholds, matrices, fail-closed procedure |
| [correctness.md](correctness.md) | R1 | Suites, mutant kills (named oracles), adversarial scenarios |
| [w02-ab.md](w02-ab.md) | R3 | 15-pair interleaved A/B, all 11 configs |
| [w03-ab.md](w03-ab.md) | R4 | Real external-loop A/B + interleave verify |
| [pending-control-pressure.md](pending-control-pressure.md) | R5 | P>0 attempts, mechanism analysis, cancel-storm A/B |
| [cpu-attribution.md](cpu-attribution.md) | R6 | Instrumented scans/op, perf stat, strace census |
| [decision.md](decision.md) | R7 | Formal verdict and productization recommendation |
| [review-A.md](review-A.md) | R8 | Independent concurrency/semantic review |
| [review-B.md](review-B.md) | R8 | Independent performance/experimental-integrity review |

## Evidence index

`evidence/` carries the campaign raw data (ab.jsonl 990 rows, w03, p5,
perf/strace/mechanism artifacts, R1 suite+mutant outputs, verdict.json,
semantic-report.json, all provenance and identity receipts). `scripts/`
carries the frozen runner/reducer/selftest and the research-only harnesses.

Larger or host-local artifacts (worktrees, builds, smoke campaigns) remain on
E5 under `/home/jnhu/sluice-e1-e5/` with hashes recorded in `identity.md`;
replay commands are in `scripts/run_e1_e5.sh --help`-style headers and
`protocol.md`.

## Reproduction sketch (on E5)

```
git worktree add --detach ~/sluice-e1-e5/base 381756370980f75e4beaf47310db4b8b9a99053d
git worktree add -b research/native-linux-e1-pending-control-skip ~/sluice-e1-e5/candidate master
cd ~/sluice-e1-e5/candidate && git apply docs/research/mw-hw-1/patches/e1-pending-control-skip.patch && git commit -a
# build both arms per target with --liburing=y, then:
bash docs/research/mw-hw-1/native-linux-e1/scripts/run_e1_e5.sh <results-dir> 15 2
python3 docs/research/mw-hw-1/native-linux-e1/scripts/e1_verdict.py --w02 <results-dir> \
    --w03 <w03-combined.jsonl> --p5 <p5.jsonl> --semantic <semantic-report.json> --out verdict.json
```
