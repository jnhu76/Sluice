# #524 resource and execution manifest

Frozen snapshot: `MODEL_HEAD_SHA = 28f0fe3397114dbcda491851fd1932b02349df49`
(`formal(request): remove retired ReleaseBind flavor` `083e1bb2` +
`formal(test): migrate retired binding-release mutant` `28f0fe33`).

## Model / toolchain SHAs

| Artifact | SHA-256 |
|---|---|
| `formal/tla/RequestCore.tla` | `4ad6dbe5cd4af19e12a846f06818622e099a37899474206fdd6984e445725aa1` |
| `formal/tla/RequestCore.cfg` | `82c31d615fdeb73d947342b2a8990072c738069cfa2e1363455a53314f7f8edf` |
| `formal/tla/RequestCoreLive.cfg` | `062c8c3e6b873a5bfd518856952ceacd942dab2f08bf9e6145c34be4f771f09d` |
| `formal/tla/RequestCoreMutDiscardKeepsBind.cfg` | `26e149befe1b644a178663a7e0839e9378f746817ec9f0dd16f674f4a53f5ab7` |
| `scripts/verify_tla.sh` | `2e38fabae8a4224e98b34124675c325f5fd8353f4ce9661c9ced7bae3d90a756` |
| `formal/tla/tla2tools.jar` (TLC 1.7.4, pinned) | `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` |

The model files were not modified after these SHAs were taken: every later
commit in this slice touches `docs/` only, and
`git diff 28f0fe33 <docs head> -- formal/tla scripts/verify_tla.sh` is empty.

## Frozen snapshot used by the R6 runs

The complete runs are taken from a snapshot directory, not from a working tree
that can move under a running JVM:
`/home/jnhu/.cache/sluice/tla524/frozen-28f0fe33`. The driver
`raw/run-liveness-formal.sh` copies the artifacts in, re-verifies all four
SHA-256 values above, and refuses to start while another TLC process is alive.
The directory was created at liveness launch with `RequestCore.tla`,
`RequestCoreLive.cfg` and `tla2tools.jar`; `RequestCore.cfg` was added when the
safety confirmation run was taken, with its frozen hash checked against the
table above. It now holds all four frozen artifacts.

## Host ("X99" of the issue) — actual observed configuration

| Property | Value |
|---|---|
| Host | `E5` / `192.168.31.75` |
| CPU | Intel Xeon E5-2666 v3 @ 2.90 GHz |
| Topology | 1 socket, 10 physical cores, 20 logical CPUs (SMT on), 1 NUMA node |
| RAM | 62 GiB total (45 GiB free at run start) |
| Storage | 393 GiB free on `/home` (snapshot, logs and metadirs under `/home/jnhu/.cache/sluice/tla524`) |
| JVM | OpenJDK 25.0.4.1 |
| TLC | tla2tools 1.7.4, pins verified before every run |

The issue's planning values were "Xeon E5-2667 v3, 64 GB, 8 workers, 32 GB
heap, one TLC JVM". The measured host is a 10-core/20-thread E5-2666 v3 with
62 GiB, so per the issue's own rule ("the formal configuration follows the
host's actual topology, memory, cgroup limits, storage and safety-preflight
observations") the runs used **16 workers** and **`-Xmx48g`**, one TLC JVM at a
time (16 of 20 logical CPUs; 48 of 62 GiB, leaving headroom for the OS and the
disk-backed fingerprint set). No cgroup memory cap was observed to bind.

## Runs

Command shape (all gates), identical to the repository gate's shape with the
heap and worker count made explicit:

```bash
java -Xmx48g -cp <snapshot>/tla2tools.jar tlc2.TLC -workers 16 -deadlock \
    -config <snapshot>/<cfg> -metadir <persistent metadir> <snapshot>/RequestCore.tla
```

| Gate | Log (in-tree, committed) | Exit code | Result |
|---|---|---|---|
| safety — `RequestCore.cfg`, 20 invariants (receipt of record) | `raw/tlc/safety.log` | **0** | **PASS** — `Model checking completed. No error has been found.`; 31,289,609 generated / 5,359,732 distinct / 0 on queue / depth 53; 48 s |
| liveness — `RequestCoreLive.cfg`, 20 invariants + `PROPERTY L1` + `PROPERTY L5` | `raw/tlc/liveness.log` | **0** | **PASS** — `Model checking completed. No error has been found.` with `Finished checking temporal properties`; 12 temporal branches over the complete state space (64,316,784 total distinct states = 12 × 5,359,732); same graph as safety: 31,289,609 / 5,359,732 / 0 on queue / depth 53; 24 min 20 s wall (2026-10-15 19:12:35 → 19:36:56) |
| safety confirmation from the snapshot (auxiliary, not a receipt) | `raw/tlc/safety-from-snapshot.log` | **0** | **PASS** — identical counters in 48 s; taken with the liveness receipt to give the safety gate snapshot provenance and to settle the maximum-outdegree statistic (below) |

Run metadata (pid, start, end, exit code, workers, heap, host, metadir, the
three artifact hashes) is committed as `raw/tlc/liveness-run.tsv`; the exit-code
table is `raw/tlc/exit-codes.tsv`.

Neither run used a temporary script that deletes run directories: the metadirs
were created explicitly and left in place, and the logs are committed text.
TLC itself removes the fingerprint files of a metadir when a run completes
successfully (the reference metadirs are therefore empty afterwards); nothing
was deleted by hand, and the owner-stopped run's metadir
(`.../tla524/liveness`) is still on disk, untouched.

Runs observed: safety TLC JVM `63151` (receipt of record, exit 0);
owner-stopped liveness JVM `63266` and aborted second attempt `64722`
(`raw/tlc/STOPPED-liveness.txt`); formal liveness JVM `65752` (exit 0);
safety confirmation JVM `66146` (exit 0).

### How the liveness result is read

`scripts/verify_tla.sh`'s `run_live_clean` verdict requires all three of:
exit code 0, `Model checking completed. No error has been found.`, and
`Finished checking temporal properties`. All three hold. Beyond the gate's own
criteria:

- **Both properties are instantiated for every identity.** `RequestCoreLive.cfg`
  is hash-pinned above and declares exactly `PROPERTY L1` and `PROPERTY L5`;
  TLC reports `Implied-temporal checking--satisfiability problem has 12
  branches`, which is |Ids| × 2 = (2 slots × generations 0..2 = 6 identities) ×
  {L1, L5}. Dropping either property would give 6, so the branch count is the
  evidence that L5 was not skipped. Both halves also bite in the negative
  controls: `rcore-mut-strand-post-accept` (an L1-side obligation) and
  `rcore-mut-lazy-reclaim` / `rcore-mut-ctl-after-settled` (L5-side) are killed
  by `Temporal properties were violated` in the R5 preflight.
- **The last pass runs on the completed graph.** TLC distinguishes the two:
  every incremental pass says `current state space` (0 s to 1 min 37 s), and the
  final one says `Checking 12 branches of temporal properties for the complete
  state space with 64316784 total distinct states` (1 min 59 s). That total is
  12 × 5,359,732 — one copy of the safety gate's distinct-state set per branch —
  so the liveness property set was evaluated on exactly the graph the safety
  gate checked, not on a smaller one.
- **No temporal violation was reported**: zero occurrences of
  `Temporal properties were violated` in the log, and the run ended with
  `Model checking completed. No error has been found.`
- TLC's own completeness caveat (fingerprint collisions) is printed by the run:
  `based on the actual fingerprints: val = 6.5E-7`.

### Counting note: why liveness reports a larger total than safety

The liveness gate evaluates 12 temporal branches, each over its own copy of the
reachable set, so TLC's `total distinct states` is 12 × the model's distinct
count (final values: 64,316,784 versus 5,359,732). The model's own counters are
identical in the two gates — 31,289,609 generated / 5,359,732 distinct / depth
53 / 0 on queue — which is also why the liveness wall time (24 min 20 s) is
~30× the safety wall time (48 s) on the same graph: the temporal passes and the
12-fold fingerprint set, not a different state space. (The pre-#524 liveness
gate over the 26,662,372-state graph took ~18.5 h with a 1 h 17 m final pass,
per the B2 ledger row; the converged graph is ~5× smaller and the run is
correspondingly shorter.)

### The maximum-outdegree statistic (previously unexplained)

TLC prints an average-outdegree line. It varies across runs of *identical*
artifacts, so it is not a property of the model:

| Run | Model + cfg | Mode | max outdegree | generated / distinct / depth |
|---|---|---|---|---|
| safety (receipt) | converged, `RequestCore.cfg` | safety | 11 | 31,289,609 / 5,359,732 / 53 |
| isolation control (reviewer B) | BASE minus `\/ ReleaseBind(s)`, `RequestCore.cfg` | safety | 10 | 31,289,609 / 5,359,732 / 53 |
| liveness (receipt) | converged, `RequestCoreLive.cfg` | liveness | 10 | 31,289,609 / 5,359,732 / 53 |
| safety confirmation | converged, `RequestCore.cfg` | safety | 10 | 31,289,609 / 5,359,732 / 53 |

The same converged cfg that reported 11 reported 10 when re-run, with
byte-identical counters, so the earlier contrast (11 with the mutant switch
present vs 10 in the isolation control) cannot be read as a difference between
the models. The load-bearing counters agree exactly in all four runs; the
statistic feeds no verdict.

## Reproduce

```bash
# fast level (seconds to a minute): parse, config, constants, 19 violating gates
docs/evidence/issue-524-requestcore-convergence-20261015/raw/run-preflight.sh /tmp/pf524

# complete level (one JVM at a time; do not run concurrently on this host)
WORKERS=16 HEAP=48g \
  docs/evidence/issue-524-requestcore-convergence-20261015/raw/run-full-tlc.sh

# the formal liveness receipt, from a hash-verified frozen snapshot
WORKERS=16 HEAP=48g \
  docs/evidence/issue-524-requestcore-convergence-20261015/raw/run-liveness-formal.sh
```
