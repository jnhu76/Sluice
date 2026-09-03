# g1-control-c0-native-3-tmpfs-corrective — notes

Corrective-2 session (authored after the session). Prereg amendments 1-5
(G1-CONTROL-C0-PREREGISTRATION.md §16) govern this session; Amendment-5
registers it.

## Scope

ONLY the missing tmpfs half of the frozen matrix was executed
(Corrective-2 P1-1): 2 ops x 5 cells x 1 fs (tmpfs, REAL) x 4 arms x
7 seeded-interleaved rounds = 280 formal runs, plus the standard 30-run
Q0 qualification ON THE SAME REAL TMPFS. The btrfs halves of the frozen
matrix remain authoritative from native-1 (F0/F1) and native-2
(F0-T/F1-T); the mislabeled "tmpfs" rows of those sessions (both labels
resolved to btrfs) are SUPERSEDED — WRONG SUBSTRATE, retained
byte-identical, excluded from every derived number.

## Execution discipline (Corrective-2 P2)

Clean commit-pinned execution: the corrective tooling was committed
BEFORE any session artifact existed; environment.json (written at
generate time) records head 45f1ff7054e8b1f146bef7af59b80b25b3f844ef
with dirty_tracked=False (tracked files identical to HEAD; the session's
own untracked output does not unpin the tooling). Driver path + sha256
and bench binary sha256 recorded in environment.json. The bench binary
is byte-identical to the Corrective-1 binary (sha256 6e75182e…; no C++
change in Corrective-2).

## Substrate (the point of this session)

G1C0_FS_ROOT_TMPFS=/tmp/g1c0-tmpfs redirects the "tmpfs" label root onto
the real tmpfs (/tmp, 32 GiB); the fail-closed substrate gate verified
the resolved fstype == tmpfs before generate/q0/formal and the manifest
records substrate={tmpfs: {root: /tmp/g1c0-tmpfs, fstype: tmpfs}}.
environment.json filesystems.tmpfs = {type: tmpfs, source: tmpfs}.

## Session timeline

1. `probe` — capable=true, features 0x3FFFF; FILE-ID-E0 reproduced
   (ORDINARY-FD WRONG-TARGET / FIXED L0 BINDING PRESERVED TARGET);
   replacement-window: BOUNDARY-A CONFIRMED + POST-COMPLETION UPDATE
   CONTROL. FORMAL_ELIGIBLE: YES. (Capability preflight ran on the
   DATA_ROOT scratch dir, substrate-independent by design; the
   substrate gate binds generate/q0/formal.)
2. `generate --fs tmpfs` — fixtures byte-identical to native-1/native-2
   (src sha256 5324ebf6… 512 MiB / 8a3c4bf0… 1 GiB); expected dst hashes
   frozen; substrate gate PASS (tmpfs resolves tmpfs).
3. `q0` — 30/30 PASS, 0 gate errors, ON REAL TMPFS: the single-worker
   uring data path is qualified on the preregistered substrate.
4. `formal --fs tmpfs` — 280 runs, 0 gate errors; run ids keep their
   full-matrix plan positions (same seed, same shuffle, tmpfs subset).
   Every threaded run carries the corrective gate fields (threads_ready
   = threads_released = 4, thread_gate_ready = true,
   thread_gate_release_after_transfer = true).
5. `summarize` (scope tmpfs-corrective) — session-local verdicts READ/
   WRITE = FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED (basis:
   session-local; btrfs neighbors absent in-session).
6. `composite native-1 native-2 native-3` — 3-session substrate-
   authoritative composition; campaign verdicts READ/WRITE =
   FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED.
7. Validators: check_g1_control_c0_analysis.py --self-test PASS
   (incl. neighbor mutants N1 64 KiB / N2 btrfs / N4 regression /
   N3 isolated control); single-session PASS (280 valid, incl.
   substrate-authority + clean-pin gates); --composite PASS (1120 ok
   runs across the three sessions; 560 valid frozen composite runs);
   check_g1_control_c0_probe_order.py PASS.

## Results (real tmpfs, single-thread F0 vs F1, ns/op median ± MAD)

```text
READ  4K d1  tmpfs:  10721.4 ± 433.2  vs 12479.1 ± 494.7  ratio 0.8591 F1_SLOWER
READ  4K d8  tmpfs:   1591.4 ±  87.0  vs  1564.9 ±  37.6  ratio 1.0169 NONE
READ  4K d32 tmpfs:   1315.8 ±  47.6  vs  1398.6 ±  88.2  ratio 0.9408 NONE
READ 64K d1  tmpfs:  20626.9 ± 1200.6 vs 20887.5 ± 447.3  ratio 0.9875 NONE
READ  2M d1  tmpfs: 359523   ± 14896  vs 313580 ± 5933    ratio 1.1465 F1_FASTER (regime cell, ISOLATED, not verdict-bearing)
WRITE (all 5 cells): NONE (ratios 0.9807-1.0118)
```

- The btrfs sub-material READ 4 KiB trend (1.6-3.6% F1-faster, native-1)
  does NOT replicate on the preregistered tmpfs substrate: d8 1.0169,
  d32 0.9408 (direction flipped), and d1 is a material ISOLATED
  F1_SLOWER cell (0.8591). The frozen vocabulary has no
  isolated-regression verdict, so the campaign verdict stays NOT
  ESTABLISHED; the cell is recorded as a per-cell observation.
- The Corrective-1 threaded REGIME-LOCAL cell (btrfs 4K d8 1.0495) does
  NOT replicate on real tmpfs (threaded 4K d8 tmpfs 1.0284, NONE):
  threaded verdicts are NOT ESTABLISHED on both substrates (exploratory
  only in every case).
- Cross-substrate observation (now a REAL regime comparison for the
  first time): READ 4K d1 flips direction between substrates (btrfs
  F1_FASTER 1.0336 isolated vs tmpfs F1_SLOWER 0.8591 material) —
  direction consistency across substrates fails at d1; under the frozen
  rule neither cell can borrow the other as a supporting neighbor.

## Environment

- Same host/kernel as native-1/native-2 (same day, no reboots): Xeon
  E5-2666 v3, kernel 7.1.9-200.fc44, perf paranoid=2, bench Release/
  clang, liburing 2.14 (xrepo, unchanged binary).
- Substrate: tmpfs label on REAL tmpfs (/tmp/g1c0-tmpfs, 32 GiB);
  btrfs label unused in this session (out of scope; default root
  unchanged).
