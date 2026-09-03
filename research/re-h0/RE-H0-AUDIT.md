# RE-H0 AUDIT — as-built ladder audit on current master

Campaign: RE-H0 (#277), branch `research/re-h0-native-performance-closure`,
BASE master `7653fd8d`. This audit precedes preregistration freeze; it is
read-only evidence about the measurement instruments and the production
paths they will measure.

## 1. Instruments under audit

| instrument | arms | last code change | status |
| --- | --- | --- | --- |
| `research/tax0/bench/tax0_z_ladder_bench.cpp` | Z1 Z1b Z1bw Z2 Z3 | `04e19c56` (original TAX-0B harness) | UNCHANGED since freeze |
| `bench/e1_abstraction_tax_bench.cpp` | L0 L1 L2 | `fb41a188` (original E1 harness) | UNCHANGED since freeze |

Both binaries link PRODUCTION `sluice_async` (no internal-testing seams in
the measured path; the guarded F01/F02 ablation seams exist only behind
`SLUICE_ASYNC_INTERNAL_TESTING`, which the Release bench build does not
define — verified: release build emits no `ablation` JSON field).

## 2. Production code measured (as-built)

Last production async change before this campaign: `d4a329c4` (R1 reverse
router scan landing, #274; merged `82af856f`), plus drain-contract tests
#275/#276. The Z2/Z3 and L2 arms therefore measure the post-A0/A1
production baseline: router known tax removed, #258 READY-drain cleanup
landed, #262 harness-side drain fail-closed. This is the RE question's
intended baseline.

## 3. Z-ladder arm audit vs the frozen semantic ladder

Frozen census: `TAX0-A2-CONTROL-PLANE-SEMANTIC-FLOOR.md`
(`z_ladder_preregistration`, frozen @ `9670224`). Mechanical as-built check
of every checklist item against the current source:

| requirement | arm | as-built evidence | verdict |
| --- | --- | --- | --- |
| bounded in-flight | Z1b/Z1bw | fixed slot table, window `submit_k - consume_k < D`, explicit `outstanding` counter + max witness | OK |
| stable request identity | Z1b/Z1bw | never-reused cookie = op sequence number (`submit_k`), generation-tagged slot form `gen*D+slot` | OK |
| stale-completion protection | Z1b/Z1bw | `z1b_cqe_terminal`: cookie not naming slot's current in-flight occupant ⇒ dropped, never delivered | OK |
| exactly-once publication | Z1b/Z1bw | single `IN_FLIGHT -> TERMINAL` transition; double terminal is a harness failure (`fatal_on_double`) | OK |
| safe buffer lifetime | all arms | slot-local buffers, refilled only at submit of current occupant, consumed before slot returns EMPTY | OK |
| one continuation | Z1bw | consumer parks on per-slot terminal predicate under shared mutex; reaper thread publishes terminal + notify | OK |
| no per-op heap allocation | Z1b/Z1bw | all state fixed preallocated arrays | OK |
| same-work 13-item list | all arms | identical fd/offset sequence/bytes/depth(=request_capacity=queue depth)/buffer reuse/file+cache state/durability (writeback only)/completion count/error policy (exit 3)/write refill from shared splitmix64 master block `0xE1E1E1E121212121`/read word-sum inline in every arm | OK |
| self-check | harness | `--self-check` state-machine gate passes on this host | OK |

### 3.1 Z3 timing boundary (mission §6 requirement)

The Z3 measured span (`run_rep`, z3 arm) includes, per rep:

- one task admission (`rt->submit`),
- the full depth-D submit/await loop,
- per-op `await_completion` + result read + `cc.reset()`,
- `TaskResultSlot` publication + `wait_and_take`.

The Z1bw measured span includes, per rep: reaper thread **create/join**
(inside the span) + the same submit/consume loop with a lost-wake-safe
condition-variable wait per op. Z2 preallocates its `Completion` array in
setup; Z3 constructs its `comp(D)` vector inside the task body (inside the
span; D ≤ 8 default constructions, negligible). Z1 allocates two small
per-rep vectors (`res`, `done`) inside the span (2 heap allocations per
rep, ~ns/op scale at ≥ 65536 ops/rep — negligible, recorded).

Consequences, reported rather than hidden:

- Z3 vs Z1bw is a **matched rep-envelope comparison**: both spans cover
  one full rep including their own admission/continuation machinery.
  They are NOT instruction-for-instruction identical substrates; the
  subtraction is reported as `T_runtime = Z3 / Z1bw` with this boundary
  note attached (mission §6 option B, explanatory layers).
- Z1bw pays one thread create/join per measured rep (~tens of µs against
  ≥ 65 ms reps ⇒ < 0.1 %); Z3 pays one task admission per measured rep.
  Both are per-rep, not per-op, costs of the continuation obligation as
  built. Neither is tuned away post hoc.

## 4. ThreadPool ladder audit (RE-1)

`e1_abstraction_tax_bench` implements exactly the #227 RE-1 ladder:

- L0 raw blocking `pread`/`pwrite` at parallelism `--depth` (strided raw
  threads, no queue),
- L1 competent fixed `std::thread` pool (W persistent workers,
  mutex+condvar bounded ring of capacity depth) running direct
  `pread`/`pwrite`,
- L2 the real public path: `ApplicationRuntime` + `ThreadPoolBackend`
  (`request_capacity == depth`, `worker_count == W`, scheduler workers 1),
  one task driving a depth-D Completion pipeline.

Decomposition authority: `T_pool = L1 / L0`, `T_sluice = L2 / L1`
(NOT `L2 / L0`). Same-work: fail-closed ops/bytes/word-sum accounting,
internal untimed write verification, shared master block. Worker sizing
rule frozen for this campaign: `W = depth` (one worker per outstanding
blocking op — the standard competent sizing for a fixed blocking pool).

## 5. Geometry feasibility (probes, pre-freeze)

1-rep probes on Host-0 (btrfs + /tmp tmpfs), warm cache, all five Z arms
and all three e1 ladders completed cleanly at both frozen geometries:

- Cell S (4 KiB × d8, 256 MiB/rep): ~65–215 ms/rep.
- Cell L (2 MiB × d2, 1 GiB/rep): ~160–400 ms/rep (btrfs write the most
  expensive at ~380 ms incl. writeback).
- `perf stat -e instructions:u,cycles:u` works (paranoid=2, :u counted).
- 2 MiB × d2 is natively supported by the unchanged harness
  (`depth * request_size = 4 MiB` ≤ 1 GiB budget; size multiple of 4096).

Campaign byte budget at the frozen protocol (11 + 7 + 14 + 7 + 14 = 53
reps per combo): RE-1U ≈ 8 blocks × 5 arms; RE-1 ≈ 8 blocks × 3 ladders;
each combo ≤ ~21 s. No budget risk; no protocol reduction needed.

## 6. Environment qualification (RE-0H-H0)

Native Fedora 44, kernel 7.1.9-200.fc44 x86_64 — not virtualized
(no WSL/container). Real block storage: SATA SSD (`sda`, ROTA=0),
btrfs `compress=zstd:1,ssd,discard=async` on `/home`. tmpfs control at
`/tmp` (32 GiB). Real io_uring + liburing 2.13, clang 22.1.8,
perf 7.1.9 with `perf_event_paranoid=2` (`:u` counters usable).

`lsblk` shows an unmounted `nvme0n1` (476.9G, existing partition table,
no Linux filesystem/mount). Per mission scope the campaign environment is
FIXED to SATA SSD btrfs + tmpfs control; the NVMe device is NOT
provisioned for this host's Linux use and is recorded here as
NOT AVAILABLE for the campaign. This does not change the external-validity
conclusion: no cross-host / modern-NVMe / ARM claim is made (see #270).

RE-0H verdict vocabulary: **HOST-0 QUALIFIED FOR HOST-LOCAL NATIVE
MEASUREMENT; NOT QUALIFIED FOR BROAD NEAR-NATIVE / CROSS-HOST CLAIM.**
