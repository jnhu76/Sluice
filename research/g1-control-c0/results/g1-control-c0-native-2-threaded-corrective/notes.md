# g1-control-c0-native-2-threaded-corrective — notes

Corrective-1 session (authored after the session). Prereg amendments 1-4
(G1-CONTROL-C0-PREREGISTRATION.md §16) govern this session.

## Scope

ONLY the affected threaded cells were re-executed (Corrective-1 P1-1):
2 ops x 5 cells x 2 fs x {F0-T, F1-T} x 7 seeded-interleaved rounds
= 280 formal runs, plus the standard 30-run Q0 re-qualification of the
modified bench binary. Single-thread evidence remains native-1
(UNAFFECTED). native-1's threaded subset is SUPERSEDED (retained
byte-identical, historical).

## Session timeline

1. `probe` — capable=true, features 0x3FFFF; FILE-ID-E0 reproduced;
   replacement-window: BOUNDARY-A CONFIRMED + POST-COMPLETION UPDATE
   CONTROL (Corrective-1 P1-2 relabel; the old in-flight retention claim
   is withdrawn — the executed topology updates the slot only after the
   CQE reap).
2. `generate` — fixtures byte-identical to native-1 (src sha256
   5324ebf6… 512 MiB / 8a3c4bf0… 1 GiB); expected dst hashes frozen.
3. `q0` — 30/30 PASS, 0 gate errors: the F0 single-worker data path of
   the modified bench is unchanged (Corrective-1 invariance check).
4. `formal --arms F0-T,F1-T` — 280 runs, 0 gate errors; run ids keep
   their full-matrix plan positions (same seed, same shuffle, threaded
   subset only). Every threaded run carries the corrective gate fields
   (threads_ready=4, threads_released=4, thread_gate_ready=true,
   thread_gate_release_after_transfer=true): workers parked across the
   measured span per frozen prereg §5.
5. `summarize` (scope threaded-corrective) + `composite` against
   native-1 — composite-summary.json written with provenance; campaign
   verdicts re-derived from native-1 single-thread only.
6. Validators: check_g1_control_c0_analysis.py --self-test PASS;
   single-session PASS (280 valid); --composite PASS (840 valid);
   check_g1_control_c0_probe_order.py PASS (structural + executed).

## Environment

- Same host/kernel/fs layout as native-1 (same day, no reboots, no
  sysctl/ulimit changes): Xeon E5-2666 v3, kernel 7.1.9-200.fc44,
  perf paranoid=2, liburing 2.14 (xrepo), bench Release/clang.
- Substrate (Corrective-1 disclosure, prereg Amendment-3): BOTH
  filesystem labels resolve to the SAME btrfs (/home, zstd:1,
  page-cache) — identical to native-1. This is required for
  comparability with the single-thread arms; the "tmpfs" label is the
  data-directory name, not the substrate. manifest.substrate_fstypes
  records the resolved values.

## Threaded (exploratory) results

- READ: isolated material cell — 4K d8 tmpfs ratio 1.0495 (robust
  separation); neighbors direction-consistent (1.02-1.05) but
  separation-failed => REGIME-LOCAL per the frozen rule; EXPLORATORY
  ONLY, cannot carry a campaign verdict (prereg §5/§13).
- WRITE: no material direction; ONE isolated regression-shaped cell
  (4K d1 tmpfs 0.8981) — per-cell observation only, no campaign verdict
  (frozen vocabulary has no isolated-regression verdict).
- The superseded native-1 threaded data pointed at a DIFFERENT cell
  (4K d32 tmpfs F1_FASTER 1.0383); the corrected run of that cell is
  NONE (1.0451, separation failed) — the pre-corrective threaded
  subset was not salvageable.
- THREADED-PROCESS ADVANTAGE: NOT ESTABLISHED (HOST-LOCAL). No verdict
  change; the campaign Gate A feeds on single-thread primaries
  (NOT ESTABLISHED, unchanged).
- Corrective-2 disposition (Amendment-5): the "tmpfs"-label rows above
  are SUPERSEDED — WRONG SUBSTRATE; the authoritative btrfs-label
  threaded cells are all NONE (READ 4K d8 1.0214). The session-local
  REGIME-LOCAL reading (1.0495) is a historical record of the
  mislabeled cell, excluded from the 3-session composite.
