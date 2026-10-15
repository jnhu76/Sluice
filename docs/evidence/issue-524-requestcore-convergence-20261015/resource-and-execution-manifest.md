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

The model files were not modified after these SHAs were taken: the two later
commits in this slice touch `docs/` only, and
`git diff 28f0fe33 <docs head> -- formal/tla scripts/verify_tla.sh` is empty.

## Host ("X99" of the issue) — actual observed configuration

| Property | Value |
|---|---|
| Host | `E5` / `192.168.31.75` |
| CPU | Intel Xeon E5-2666 v3 @ 2.90 GHz |
| Topology | 1 socket, 10 physical cores, 20 logical CPUs (SMT on), 1 NUMA node |
| RAM | 62 GiB total |
| Storage | 394 GiB free on `/home` (metadirs under `/home/jnhu/.cache/sluice/tla524`) |
| JVM | OpenJDK 25.0.4.1 |
| TLC | tla2tools 1.7.4, pins verified before every run |

The issue's planning values were "Xeon E5-2667 v3, 64 GB, 8 workers, 32 GB
heap, one TLC JVM". The measured host is a 10-core/20-thread E5-2666 v3 with
62 GiB, so per the issue's own rule ("the formal configuration follows the
host's actual topology, memory, cgroup limits, storage and safety-preflight
observations") the runs used **16 workers** and **`-Xmx48g`**, one TLC JVM at a
time (16 of 20 logical CPUs; 48 of 62 GiB, leaving headroom for the OS and the
disk-backed fingerprint sets). No cgroup memory cap was observed to bind.

## Runs

Command shape (both gates), identical to the repository gate's shape with the
heap and worker count made explicit:

```bash
java -Xmx48g -cp formal/tla/tla2tools.jar tlc2.TLC -workers 16 -deadlock \
    -config formal/tla/<cfg> -metadir <persistent metadir> formal/tla/RequestCore.tla
```

| Gate | Log (in-tree, committed) | Metadir (persistent, not in-tree) | Exit code | Result |
|---|---|---|---|---|
| safety — `RequestCore.cfg`, 20 invariants | `raw/tlc/safety.log` | `/home/jnhu/.cache/sluice/tla524/safety` | **0** | **PASS** — `Model checking completed. No error has been found.`; 31,289,609 generated / 5,359,732 distinct / 0 on queue / depth 53; 48 s |
| liveness — `RequestCoreLive.cfg`, 20 invariants + `PROPERTY L1` + `PROPERTY L5` | `raw/tlc/liveness.log` (partial) | `/home/jnhu/.cache/sluice/tla524/liveness` | none recorded | **NOT A RESULT** — terminated by owner instruction at 17:27 after ~3.5 min (see `raw/tlc/STOPPED-liveness.txt`). No violation had been reported, but the exploration was incomplete, so nothing about L1/L5 is claimed from it |

Neither run used a temporary script that deletes run directories: the metadirs
were created explicitly, left in place, and logs are committed text.

Observed processes: runner script `63143`; safety TLC JVM `63151` (completed,
exit 0); liveness TLC JVM `63266` (stopped on owner instruction).

### Liveness: what remains

The gate is un-run, not failed. The command to complete it is recorded in
`raw/tlc/STOPPED-liveness.txt`. Expected cost on this host is ~30 minutes
(reviewer B independently completed the same gate on the converged model in
28 m 49 s at 12 workers with the same state space; that result is *reviewer
scratch evidence*, recorded in `review.md`, and is deliberately not promoted to
this manifest's formal receipt).

### Prior independent runs on the converged model (evidence, not receipts)

Reviewer B, from its own scratch directories: reference safety clean
(31,289,609 / 5,359,732 / depth 53), reference liveness clean with L1 and L5
over the complete space (10 incremental temporal passes, no
`Temporal properties were violated`), both subsumption witnesses clean. These
corroborate the architecture but are not substituted for the formal runs above.

## Reproduce

```bash
# fast level (seconds to a minute): parse, config, constants, 19 violating gates
docs/evidence/issue-524-requestcore-convergence-20261015/raw/run-preflight.sh /tmp/pf524

# complete level (one JVM at a time; do not run concurrently on this host)
WORKERS=16 HEAP=48g \
  docs/evidence/issue-524-requestcore-convergence-20261015/raw/run-full-tlc.sh
```
