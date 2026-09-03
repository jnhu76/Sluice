# COPY-X0-REPORT — Explicit Copy as a Legal Transformation Boundary

- Campaign: COPY-X0 (G1-Control Candidate 2) under #227 / #259
- Preregistration FROZEN at `715c7711` — before any harness code, probe, or
  measurement existed. Amendments 1–3 appended additively (§16), each with
  its own commit; frozen thresholds, matrix, arms, fixtures, rounds and the
  §9 materiality rule were never edited.
- Base master: `ecd84259` (post PR #280)
- HEAD at evidence commit: `883d54f6` (all formal sessions commit-pinned,
  `dirty_tracked=false` recorded per session)
- Execution host: WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`, AMD
  Ryzen 7 5800H (8 vCPU), glibc 2.41, clang 22.1.8 Release, substrates
  tmpfs (`/tmp`, statfs-verified `0x1021994`) and ext4 (`/dev/sdd`,
  `0xef53`). **HOST-LOCAL ONLY.**

## Verdicts (mechanically derived by `validate_copy_x0.py --composite`;
`results/campaign-verdicts.json`)

```text
COPY-X0-CAPABILITY:              BLOCKED (measurement infrastructure)
COPY-X0-SEMANTIC-EQUIVALENCE:    EQUIVALENT FOR DECLARED COPY CONTRACT
COPY-X0-TRANSFORMATION-BOUNDARY: LEGAL TRANSFORMATION BOUNDARY SUPPORTED
COPY-X0-MINIMALITY:              LOCAL COPY BRANCH SUFFICIENT;
                                 GENERIC CAPABILITY FRAMEWORK NOT EARNED
COPY-X0-G1-CONTROL:              NOT ESTABLISHED (capability gate A unmeasured)

PROMOTION:                       STOP — NO C1
```

Gates: A_capability=false (BLOCKED), B_semantic=true, C_control_value=true
(validator sensitivity re-proven in-process), D_minimality=true (B3 section
= 81 lines; M6 design gate PASS).

A stop verdict is a successful campaign outcome. C1 is NOT authorized by
this evidence; no production code was touched.

---

# 1 What was executed

| Step | Result |
|---|---|
| Step 0 baseline + governing issues (#227 #259 #279 PR #280) | done |
| Step 1 as-built audit (`COPY-X0-AUDIT.md`, findings F-1..F-8) | done |
| Step 2 ladder/fixtures/mutants design | frozen in prereg |
| Step 3 prereg freeze | commit `715c7711`, before any code |
| Step 4 harness + validators + xmake target | commits `d674769f`..`24bc4d32` (two harness defect repairs found by smoke, disclosed in commit messages) |
| Step 5 self-tests | bench selftest 6/6; validator `--self-test` PASS (M1–M5 rejection, both §9 falsification directions); design gate PASS |
| Step 6 formal semantic session | `copy-x0-semantic-native-2` VALID (100 rows; native-1 superseded — see §4) |
| Step 7 formal performance matrix | **11 disclosed attempts, none passed the frozen A/A calibration bar** → BLOCKED (§5) |
| Step 8 verdict derivation | `--composite`, gate C re-proves validator sensitivity |
| Step 9–10 report + adversarial review | this document (§7) |

# 2 Mechanism facts established on this host (probe + fixtures)

- `copy_file_range` EXISTS and works **same-filesystem** on both tmpfs and
  ext4 (kernel 6.18 WSL2; glibc wrapper ≥2.30 → no silent libc fallback).
- **Cross-filesystem is refused both directions with EXDEV(18)** — tmpfs↔ext4
  are different filesystem types; the ≥5.19 same-type rule does not apply.
  This is a first-class semantic dimension (row 12), not an error.
- A single call moved the full 64 MiB (per-call cap ≥ 64 MiB observed).
- 5.3–5.18-era success-without-copy bug not applicable (6.18).
- Sparse source: all four arms produced a DENSE destination on both
  substrates (identical extents `[0,8MiB]` per SEEK_DATA/SEEK_HOLE witness)
  — no arm-divergent layout behavior on this host (row 13: recorded, no
  divergence to declare).

# 3 Semantic result (fixture-by-fixture, `copy-x0-semantic-native-2`)

All MUST MATCH rows (1–8) passed on ALL four arms on BOTH substrates:

| Fixture | Result |
|---|---|
| S1 full copy 1 MiB | all arms: exact bytes, size, offsets, accounting |
| S2 offsets src 4096 / dst 8192 | range exact; sentinel outside range intact; positional arms left shared offsets untouched; B1 (declared difference, row 9) advanced shared offsets exactly by bytes moved |
| S3 EOF before 64 KiB limit (16 KiB source) | all arms: clean success, moved=16384 — EOF-before-limit is success on every mechanism |
| S4 64 MiB call accounting | moved == xfer_bytes == n on all arms; partial-return accounting holds |
| S5 pipe source (non-regular) | B0 fails ESPIPE (positional precondition); **B1 (production copy_all, abstract Reader/Writer) succeeds** — the current library surface has NO regular-file precondition; B2 fails EINVAL (kernel); B3 fails closed naming `precondition_regular_file`. All four behaviors are the declared mechanism contracts — the difference is exactly what the boundary makes explicit |
| S6 cross-fs | B0/B1 copy correctly across fs; B2/B3 record EXDEV fail-closed (both directions, both sizes) |
| S6fb explicit fallback | caller-requested fallback runs the buffered arm cross-fs, completes the copy, and RECORDS it: `fallback_occurred=true`, `mechanism_executed=buffered_read_write`, refused attempt visible in xfer counts. Silent fallback is unrepresentable (M3) |
| S7 overwrite into existing dest | only the requested range overwritten |
| S8 sparse source | byte-exact on all arms; layout witnesses recorded, no divergence |
| S10 durability | zero sync-class syscalls in every arm of every row — no arm earns durability credit |

**SEMANTIC-EQUIVALENCE: EQUIVALENT FOR DECLARED COPY CONTRACT** — with the
declared differences of rows 9/11/12 verified, not assumed.

# 4 Corrective history (all disclosed, all evidence retained byte-identical)

| Event | Disposition |
|---|---|
| Harness: dst verification pread on O_WRONLY fd (EBADF) | fixed pre-formal (`24bc4d32` line history) |
| Harness: source checksum over-read on EOF fixture (S3) | fixed pre-formal |
| Harness: OpCounts clobbered by returned ArmResult | fixed pre-formal; found by smoke validator |
| Harness: scratch first-touch page faults INSIDE timed span (4K-cell −5.9 log2 artifact) | fairness fix: caller-owned prefaulted scratch, B1 setup hoisted pre-span — the unfair harness would have fabricated a 60× small-file win |
| Validator: perf id/phase classification (AA rows marked perf) | fixed |
| qualify-native-1 missing prereg §10.2 probe rows | driver repaired; native-1 SUPERSEDED-INCOMPLETE |
| Re-derived qualified chunk differs (256 KiB vs 1 MiB; selection flips inside host noise) | semantic/perf-native-1 (built on native-1's chunk) SUPERSEDED |
| perf native-2: host stalls in its own A/A (tmpfs 64M pair 0.023s vs 5.43s = 7.86 log2) | Amendment 2: mechanical A/A validity bar 0.50 p90 |
| Bar's max-over-all-cells form conflates µs-jitter (64K cells) with session degradation | Amendment 3: bar scoped to adjudicable cells (median wall ≥5ms); native-2/3 stay superseded on merits |
| Validator scale-check read nonexistent row field (label lives in id) → bar silently not gating | fixed; native-4 correctly failed thereafter |
| perf native-2..11 | all SUPERSEDED-DEGRADED (§5) |

# 5 Performance arm: BLOCKED — measurement infrastructure

Eleven full formal attempts (288 perf runs + 144 A/A runs each), executed
under the frozen protocol (chunk 256 KiB, seeded-interleaved 9 rounds,
4 arms, warm regime, `taskset -c 2`):

```text
attempt   tmpfs 64M AA p90    ext4 64M AA p90     (bar: ≤ 0.50 both)
native-2      7.864               4.020           catastrophic stalls
native-3      3.134               1.308
native-4      0.734               0.672
native-5      0.493               0.582
native-6      0.558               0.338
native-7      0.453               0.652
native-8      0.143               0.820
native-9      0.776               0.796
native-10     0.222               0.826
native-11     0.650               0.718   (fresh work roots)
```

Nearest-rank p90 of 9 pairs = max, per the frozen statistic. The host's
steady-state single-pair tail at ~25 ms spans (one Windows-side preemption
adds 15–60 ms) sits at 0.55–0.83; a compliant session requires both 64 MiB
cells simultaneously ≤0.50. The A/A phase exists precisely to certify this,
and it certified FAILURE: under the frozen rules this is

```text
infrastructure blocked → BLOCKED (not a performance verdict)
```

NOT claimed: any direction of kernel-transfer benefit or non-benefit. The
11 superseded sessions' raw rows are retained byte-identical; no number
from them enters any verdict. Reopen condition (explicit): a measurement
window/host whose A/A calibration passes the frozen gate; the frozen §9
rule then applies unchanged to that session.

# 6 Boundary / minimality result (independent of performance)

- The declared Copy floor (17 rows, no UNRESOLVED) held across four
  mechanisms — primitives stayed rigid while the composed operation legally
  admitted the transformation (T-COPY-X0's semantic half).
- M1: a primitive-labeled row carrying `copy_file_range` is structurally
  rejected by the validator — the transformation is legal ONLY inside the
  composed boundary (enforced, not asserted).
- M3: silent fallback is unrepresentable — fallback exists only as an
  explicit caller request plus a recorded decision + corroborating op
  counts (demonstrated in S6fb formal evidence).
- B3 is 81 lines (enum + policy + decision record + two dispatch branches +
  precondition check); M6 design gate green; no registry/planner/manager.
  **A generic capability framework is NOT earned** — exactly H5's predicted
  success shape.
- B1 measurement note: the smoke sessions (disclosed, non-formal) put the
  production `copy_all` path at NONE..SLOW (+13–16% median in some cells,
  signs unstable under the same noise) — no formal claim is made; the
  existing-control-tax question reopens with the capability question.

# 7 Adversarial self-review (goal §27, all 15 answered)

1. **Weak buffered baseline?** No — B0 chunk selected by the frozen host
   qualification (256 KiB from {8K..4M} × both substrates, max-median
   rule); matched offsets/bytes/cache/durability; the unfair-baseline
   mutant class (M5) is rejected by the validator, and the pathological
   16-byte control row is retained as demonstration. The early page-fault
   artifact that WOULD have fabricated a 60× win was caught and fixed
   pre-formal.
2. **Identical bytes/ranges?** Every run checksums dest vs source
   (fail-closed); every formal row carries `bytes_ok`/`size_ok` true.
3. **Final-byte equality vs semantic equivalence?** Progress, EOF,
   zero-progress, offsets, unsupported, cross-fs, fallback recording are
   all separately adjudicated rows — not just byte equality.
4. **Filesystem behavior differed?** Substrate identity is statfs-verified
   per run (label ↔ magic); cross-fs disposition recorded per direction.
5. **Silent fallback?** Impossible in B3's shape; M3 proves the validator
   rejects unrecorded fallback.
6. **Mechanism labeled?** Every row carries `mechanism_executed`,
   cross-checked against arm, outcome and op counts.
7. **Obtainable by a ~20-line standalone wrapper?** **Yes — and that is the
   finding, not a defeat:** B3 IS essentially that wrapper (81 lines). The
   demonstrated value is the contract+discipline pattern (declared floor,
   fail-closed precondition, recorded fallback, primitive non-grant), all
   of which is portable and none of which requires Sluice machinery. H1 is
   partially sustained: Sluice-specific control premium over a standalone
   function is NOT established.
8. **What does Sluice's explicit boundary add today?** Mechanically: the
   enforced separation (M1) and the observable-fallback precedent already
   in `CopyStrategy`'s design. Substantively: nothing beyond what a thin
   function provides — the campaign does not claim otherwise.
9. **Hidden policy?** No automatic mode exists anywhere in the campaign;
   `Auto` in production copy_all still resolves to exactly one named
   strategy.
10. **Scope enlarged after negative first result?** Amendments 1–3 are
    instrument-repair disclosures (pinning, calibration bar, bar scoping),
    each carrying the failed evidence forward; the decision rule was never
    touched. The near-misses that motivated Amendments 2/3 are published
    above.
11. **Same architecture if copy_file_range were 0% faster?** The semantic
    verdict (boundary + floor + minimality) is performance-independent and
    stands; the capability-dependent promotion would still be STOP.
12. **Same architecture if 2× faster?** Same answer: capability would still
    need a measurement-valid session; the architecture conclusion (thin
    branch, no framework) is unchanged.
13. **Semantics or benchmark excitement?** All surviving verdicts are
    semantic/structural; the perf arm contributed nothing to them.
14. **New abstraction earned by >1 real use case?** No — zero new
    production abstraction is proposed; MINIMALITY says the framework is
    NOT earned.
15. **Willing to close without C1?** Yes — this report closes COPY-X0 with
    STOP — NO C1.

# 8 What was falsified / what was not proven

**Falsified / bounded:**
- H2 partially falsified: transformation legality IS expressible narrowly
  (17-row floor, zero UNRESOLVED, one thin function) on this host's
  mechanism surface.
- H5 confirmed in its "success" direction: thin local branch suffices;
  framework inflation rejected (M6).
- The unfair-harness artifact class: caught pre-formal (would have
  fabricated a large small-file win).

**Not proven:**
- KERNEL TRANSFER VALUE on this host (BLOCKED — no measurement-valid
  session in 11 attempts).
- G1-CONTROL value (Gate A unmeasured; and H1's portability reading
  weakens any Sluice-specific premium claim independent of measurement).
- Semantic equivalence on any other kernel/filesystem/hole-preservation
  regime (e.g. real btrfs/XFS reflink behavior) — out of host scope.

# 9 Verification actually executed

```text
bench selftest                        6/6 PASS (loop rules rows 7/8)
validator --self-test                 PASS (M1–M5 + substrate + commit-pin
                                      + both §9 falsification directions
                                      + composite; re-proven inside every
                                      composite derivation)
check_copy_x0_design.py               PASS (+ its own self-test)
semantic-native-2 --session           VALID (100 rows re-derived)
qualify-native-2 --session            VALID (probe + chunk rule re-derived)
perf native-2..11                     all correctly REJECTED by the frozen
                                      calibration gate (the gate works)
substrate authority                   statfs-verified per session + live
commit pins                           dirty_tracked=false on every formal
                                      session at its recorded HEAD
same-work verification                per-run checksums/sizes, fail-closed
```

Not run: full C++ Debug/ASan/TSan suites — production code is untouched
(this is a research-only bench + scripts change); no public API, model, or
target default changed.

# 10 Machine-readable decision

```text
PROMOTION: STOP — NO C1
reason:    COPY-X0-CAPABILITY BLOCKED (measurement infrastructure);
           G1-CONTROL NOT ESTABLISHED
retained:  semantic + boundary + minimality evidence (valid, host-local)
reopen:    a measurement window passing the frozen A/A gate, then the
           frozen §9 rule applies unchanged; splice arm only under goal §8
           conditions
```
