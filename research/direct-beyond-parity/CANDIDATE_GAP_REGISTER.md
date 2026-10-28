# Candidate Gap Register — Direct Beyond-Parity (P1 Synthesis)

Merged from the six P1 audits' GapCandidates, with magnitudes corrected by
the executed P1 measurement campaign (`P1_MEASUREMENT_PLAN.md`, `p1-raw/*`).
Companion documents: `DIRECT_COST_FLOOR.md` (cost model, ladders, calibers),
`DIRECT_HOTPATH_LEDGER.md` (machine work). Legend as in P0.

**Merge policy.**

- *Magnitude correction*: where a paired wall measurement exists for the gap's
  op, the measured Δ(B1−N1) median/CI replaces static ns estimates as the
  headline; static instruction counts are retained as machine-level evidence.
  Session MDEs (read 5.535 / write 15.695 / metadata 10.216 / cold 1416.185
  ns) are the detectability floors.
- *measuredSupport*: `true` iff a campaign number directly supports **this**
  gap (paired delta or cross-arm PMU delta isolating the gap's mechanism);
  `false` means measurements bound, corroborate, or aggregate it without
  isolating it.
- *Exclusions*: anything already concluded `NO_CHANGE_JUSTIFIED` by #600 is
  **not re-registered** (see §3). Per-item ns splits inside composition gaps
  are static-derived `[UNCERTAIN]`; only aggregates are measured.

## 1. Operations at PARITY_REACHED

| Op | Basis |
|---|---|
| `read_at` | Wall: PARITY at 0B/1B/512B/4K/1M, random, cold-4K (§5.5, session MDE 5.535); static 39 vs 40 matched arm. NO_CLAIM cells: 64B (contaminated), 64K (bandwidth band), eof-mix single-op (+9.037, CI spans 0). |
| `read` | Static: 29 vs 35 matched arm (B1 leaner); no ladder arm. |
| `write_at` | Wall: PARITY at every size incl. 0B (+2.002 sub-MDE) and random; cold NO_CLAIM; static 39 vs 40. |
| `write` | Static: 29 vs 35 matched arm; no ladder arm. |
| `File::open` (with `close`, paired) | Wall: pair +11.487 < max(10.216, 1%·1596.9=15.969); static +13 hygiene gap G-OPEN-01 remains (low). |
| `File::close` | Static Δ0 instruction-for-instruction; included in measured pair. |
| `size` | Wall −6.609 PARITY (< 10.216); PMU −18.0 instr/op (Sluice fewer). |
| `resize` | Static Δ0 (28 vs 28, cold-block +2 only); no ladder arm. |
| `sync_data` | Static Δ1 (layout jmp), identical shapes; no ladder arm. |
| `sync_all` | Static Δ1 (layout jmp); no ladder arm. |
| `write_all_at` | Wall +11.392 PARITY (< MDE 15.695, CI spans 0) **at the preregistered wall caliber**; instruction-caliber excess (+58.1/op PMU) stands as gap G-WRITECOMP-01. |

Not at parity: **`read_exact_at`** — measured EXCESS in two cells
(+18.992 seq / +16.129 EOF-mix, both CI-excluding, ≥MDE); `read_exact`,
`write_all` — static composition gaps, never measured; `file_info` — wall
middle band (+30.312, CI spans 0) with instruction parity (−2.0/op)
`[UNCERTAIN]`.

**Standing rule for un-laddered ops:** `read`, `write`, `read_exact`,
`write_all`, `resize`, `sync_data`, `sync_all` have no wall-clock ladder arm
in any round; their rows above are static-verdict only, and the G-REX/G-WALL
entries below carry explicit measurement preconditions. No P2 claim about
these paths may cite an extrapolated dynamic number as if measured.

## 2. Register entries

### Measured aggregates (new, synthesis-level)

**G-READCOMP-01 — read_exact_at composition userspace excess** · severity
high · confidence high · measuredSupport **true**
- Source: compose step re-enters public `read_at` (`src/blocking_file.cpp:297-299`); machinery `:269-283`, `include/sluice/detail/file_semantics.hpp:232-272`, `direct_outcome` `:241-265` (`p1-audit/composition-read.md` §1).
- Trigger workload: any read_exact_at (single- or multi-step) on an admitted request; measured at 4 KiB.
- Necessary work: one admission per public call, one pread per step, the observable `CompositionOutcome` payload (SEM-05/ERR-02).
- Excess work: static +37 instr vs matched-native composition control (100 vs 63; +25/short step) = per-step re-admission ≈13 + dead state block 8 + double exit materialization ≈9; syscall count identical (1.000–1.3333/op both tiers).
- Magnitude (measured): **+18.992 ns/op** median, 95% CI [+7.788, +24.326], 13/16 pairs (seq 4 KiB); **+16.129**, CI [+6.234, +25.061], 14/16 (EOF mix); PMU +118.2 instr/op ≈ 3× the single-op +38.8 constant. Both ≥ MDE 5.535 → EXCESS.
- Mechanism: the union of G-REA-01/02/03 (prevalidated step primitive, register-resident progress, direct sret construction).
- Falsifier: combined candidate branch (77e5399a + mechanism) with a new A1X matched arm, 16 ABBA pairs at 4 KiB: aggregate prediction ≈ −7…−10 ns; a paired delta < 3 ns refutes the static decomposition. **Beat-target must be preregistered** against the cross-round contradiction (DIRECT_COST_FLOOR §6.2): #600's post-C1/C2 AX−A2 residual was +9.7…+9.9 ns `[TRANSCRIBED]`; this session measured ≈+19.0/+18.992 ns for the same quantity shape — the candidate must state which number it claims to beat and settle the 2× spread against a fresh same-session baseline. P2 must add composition mutant seams + named killers first (none exist at B1; C2's step seams lived only in the reverted worktree).
- measuredSupport rationale: two independent wall cells + PMU isolate the composition tier (pairing against matched-native *composition* arms).

**G-WRITECOMP-01 — write_all_at composition instruction excess** · severity
medium · confidence high · measuredSupport **true**
- Source: step lambda re-enters public `write_at` (`src/blocking_file.cpp:313-315`); staging `:272-282`, `:241-265` (`p1-audit/combined-writes.md` §1).
- Trigger: any write_all_at; measured at 4 KiB single-step.
- Necessary work: one admission per call, one pwrite per step, final CompositionOutcome (ERR-02).
- Excess: static +29 instr single-step / +17 per additional step vs A1-pattern arm; per-iteration 10-instr staging incl. one dead 16-B store; syscall count identical (1.000/op both tiers).
- Magnitude (measured): wall +11.392 median, CI [−13.205, +26.325] — sub-MDE (15.695) → wall PARITY; **PMU +58.1 instr/op** cross-tier (WX−W1x, ≈3× the +19 single-op cross-tier constant). Same-caliber composition-increment ratio: (WX−W2)/(W1x−W1) = +61.0/+22.0 ≈ **2.8×** — B1's own composition machinery costs ≈2.8× N1's own composition machinery.
- Mechanism: union of G-WALLAT-01/02/03.
- Falsifier: candidate branch + preregistered write ladder rerun: a static −17/step candidate must show ≥ static-shaped instruction drop and stay green on the §7 oracle set (short-write/zero-progress/EINTR-impossible-count variants incl. the missing write-side impossible-count script); wall ≥ MDE would establish measurability, sub-MDE keeps it instruction-caliber only.
- measuredSupport rationale: the PMU cross-arm delta directly measures the composition-tier instruction excess; wall does not (sub-MDE, disclosed).

### Composition-read items (members of G-READCOMP-01)

**G-REA-01 — per-step re-admission** · medium · high confidence ·
measuredSupport **false**
- Source: `src/blocking_file.cpp:297-299`; machine 0xfc0-0xfff (≈13 instr/step: fd reload, closed, access, or/js transfer re-test, movabs/lea/sub/cmp/jb range recompute).
- Trigger: every step of every read_exact_at on an admitted request.
- Necessary work: one SEM-03/04 admission per public call + one pread per step.
- Excess: re-proves the once-established outer verdict; THREAD-01 + the documented invariant (`blocking_file.cpp:267-268`) make re-validation unnecessary.
- Magnitude: ≈13 instr/step ≈ 2–3 ns [UNCERTAIN per-item]; **measured bound**: #600's C2 removed exactly this item and measured ≈ −0.2 ns (below the #600 MDE) `[TRANSCRIBED]` — the item alone is wall-inert; it contributes only inside the measured G-READCOMP-01 aggregate.
- Mechanism: prevalidated step primitive (C2 shape) or hoisting invariants out of the loop.
- Falsifier: C2-style candidate vs a fresh A1X arm, 16 ABBA at 4 KiB; AX−A1X < MDE 5.535 confirms dynamic inertness of the item alone.

**G-REA-02 — dead CompositionState round-trip** · medium · high ·
measuredSupport **false**
- Source: `include/sluice/detail/file_semantics.hpp:243-263`; machine 0x1250-0x127f: 8 instr / 7 memory ops per step incl. a 16-B `movdqa`+`movaps` store to 0x20(%rsp) that no instruction reads (full-function reader enumeration).
- Trigger: every read_exact_at step reaching the progress merge.
- Necessary work: none — slots are never read or re-stored from registers before their only read.
- Magnitude: ≈8 instr/step ≈ 2–3 ns [UNCERTAIN]; C2 never removed this mechanism, so its null does **not** bound it — leading carrier candidate for the measured +19 ns aggregate.
- Mechanism: keep confirmed/end/stop in registers; build the outcome only at exit.
- Falsifier: candidate with the block removed: objdump shows no 0x20(%rsp)/movaps; full matrix + fault tests + admission mutants green; paired ABBA delta measured.

**G-REA-03 — double exit materialization** · medium · high ·
measuredSupport **false**
- Source: `direct_outcome` `src/blocking_file.cpp:241-265`; machine 0x10d3-0x1132 + dead zero-stores 0x1285/0x1294 (~12 memory ops vs the matched native's 5 direct sret stores).
- Trigger: every read_exact_at exit (complete/EOF/error).
- Magnitude: ≈7–9 instr ≈ 2–3 ns per exit [UNCERTAIN]; aggregate-measured only.
- Mechanism: write the sret fields directly per exit flavor.
- Falsifier: same candidate protocol; static check = single materialization site; dynamic = paired delta.

### Composition-read, shared-cursor twin (never measured)

**Measurement precondition (hard, applies to all three G-REX entries):**
`read_exact` has **no ladder arm in any round** — no wall claim (excess,
parity, or regression) may be made or projected from these entries until a
committed twin ladder arm (shared-cursor `read` composition, N1-matched
control included) exists and a 16-ABBA cell has run. Until then every number
below is static or extrapolated from the `read_exact_at` twin and must be
cited as such.

**G-REX-01 / G-REX-02 / G-REX-03** — read_exact equivalents of G-REA-01/02/03
· G-REX-01 medium, G-REX-02 medium, G-REX-03 low · high confidence ·
measuredSupport **false**
- Sources: `src/blocking_file.cpp:327-328` (re-admission ≈9 instr/step, 0x1790-0x17ad); `file_semantics.hpp:243-263` (dead block 8 instr/step, 0x19d0-0x19ff, dead store to 0x10(%rsp)); `direct_outcome` (~12 mem ops + dead stores 0x1a05/0x1a14).
- Magnitude: static 82 vs 51 control (+31 single-step, +19/step); per-item ≈2–3 ns [UNCERTAIN]; **no measured ladder history at all** (P0 §4.13 residue — read_exact has no arm) — dynamic share extrapolated from the read_exact_at twin.
- Mechanism/falsifier: same as the REA items with a shared-cursor ladder arm.

### Combined-write items (members of G-WRITECOMP-01)

**G-WALLAT-01 — per-step re-validation (write_all_at)** · medium · high ·
measuredSupport **false**
- Source: `src/blocking_file.cpp:313-315` + :99-106; machine 0x13c0-0x13ff — exactly 12 of 44 loop instructions re-prove closed/access/range/transfer.
- Trigger: multi-step write_all_at (short writes: interrupted streams, kernel-segmented large transfers, scripted short counts); single-step path executes the block once.
- Magnitude: static 12/44 instr/step; ≈5–8 ns/step [UNCERTAIN, #600-caliber extrapolation]; measured aggregate +58.1 instr/op (G-WRITECOMP-01), wall sub-MDE.
- Mechanism: write-side prevalidated step primitive (C2 analogue); public write_at unchanged.
- Falsifier: preregistered write ladder (AW2 vs AW1, 16 ABBA, 1B–64KiB): per-size Δ ≤ MDE 15.695 downgrades to unmeasurable; prototype must show −12 instr/step statically and a new mutant GREEN→RED→GREEN.

**G-WALLAT-02 — per-iteration outcome staging + spills (write_all_at)** ·
medium · medium · measuredSupport **false**
- Source: `src/blocking_file.cpp:272-282` + :241-265; machine 0x1650-0x167f (10 vs A1-arm 3 per iteration), total on caller-saved r8 (0x141b/0x1425), EINTR feed store (0x140d).
- Trigger: every write_all_at call; heaviest in multi-step shapes.
- Necessary work: publishing exactly one final CompositionOutcome (ERR-02).
- Magnitude: ≈10 instr/iter ≈ 4–7 ns [UNCERTAIN]; the remainder of the +29 single-step excess beyond G-WALLAT-01.
- Mechanism: confirmed/total in callee-saved registers; build the outcome at exit only (A1 arm already demonstrates the shape).
- Falsifier: staging-out-of-loop source variant shows the static drop and stays green on the §7 oracles.

**G-WALLAT-03 — dead 16-B store per iteration (write_all_at)** · low · high ·
measuredSupport **false**
- Source: machine 0x1677 (`movaps → 0x20(%rsp)`), sole occurrence in the function, never read.
- Magnitude: 1 store16/iteration — almost certainly below MDE; pure excess and the canary of the staging pattern.
- Mechanism: none direct; expected to vanish with G-WALLAT-02.
- Falsifier: disappears from the disassembly when staging leaves the loop.

**G-WALL-01 / G-WALL-02 / G-WALL-03** — write_all (shared-cursor) equivalents
· medium / low / low · high · measuredSupport **false**
- **Measurement precondition (hard, all three entries):** `write_all` has no
  ladder arm — no wall claim may be made until a committed twin arm
  (shared-cursor `write` composition, N1-matched control included) runs a
  16-ABBA cell; the numbers below are static or extrapolated from the
  `write_all_at` twin.
- Sources: `src/blocking_file.cpp:340-341` (7 of 34 loop instr re-validate, 0x1b10-0x1b2d); staging `:272-282` (0x1d50-0x1d7f, total on rcx 0x1b3e/0x1b48); dead store 0x1d77.
- Magnitude: static +22 single-step / +11 per step vs A1 arm; per-item ≈3–5 ns [UNCERTAIN]; **no write_all ladder arm** — extrapolated from the write_all_at twin.
- Mechanism/falsifier: same as WALLAT items with a shared-cursor arm.

### Single-op hygiene items

**G-READAT-01 / G-READ-01 — null-data guard (read_at / read)** · low · high
confidence · measuredSupport **false**
- Source: `src/blocking_file.cpp:86-87` (read_at) and :126-127 (read): 3 instr + 1 not-taken branch per nonzero-length call, guarding `span{nullptr, n>0}` — unconstructible without UB (std::span precondition), no v2-r1 text, no oracle (all null-buffer oracles use size-0 spans, handled by the zero-length branch); SEM-03 makes caller memory validity a caller precondition.
- Magnitude: ≈3 instr, sub-ns; **measured bound**: the 0B read cell (+3.338 ns, CI-excluding but sub-MDE 5.535 → BELOW_NOISE) bounds the whole userspace wrapper, not this guard; far below any measurable effect.
- Mechanism: delete the guards (one candidate branch covers both sites) **or** anchor the defensive rejection in the root per the AGENTS.md mechanism-justification gate. Note the frozen N1 comparator carries the same check (`ladder.cpp`-pattern), so removal yields no beyond-parity gain; a successor N1 arm must drop it in lockstep to keep the pairing clean.
- Falsifier: candidate branch recompiled with both deletions: (a) −3 instr/−1 branch per function; (b) full semantic matrix (4-config 58/58 + precedence + range + errno suites) predicted green (grep evidence: no `span(nullptr,n)` construction in tests/); (c) 16-ABBA 1B/4KiB deltas within noise. Any red suite or measurable delta falsifies the classification.

**G-READAT-02 — EINTR re-entry redundant re-spill** · low · low confidence ·
measuredSupport **false** — record-only, not proposed for admission
- Source: `src/blocking_file.cpp:89-92` (re-entry points read_at+0x5c / read+0x479): 4 of 7 re-entry instructions re-store already-spilled values.
- Trigger: signal during a blocking pread/read (EINTR retry back-edge); no admitted workload triggers it at measurable frequency (#600 ledger cells carry no EINTR frequency term).
- Magnitude: ≈7 instr per retry, error-path only.
- Mechanism (if ever admitted): hoist the re-entry point to the argument-reload block.

**G-WRITEAT-01 — write-path ladder coverage (P0 §4.13)** · low · low ·
measuredSupport **true** — **RESOLVED by the P1 campaign**
- The audit declared an evidence-coverage gap ("no E5 ladder exists for write paths"); the falsifier (extend the ladder, run preregistered ABBA) was executed: write-sweep 0B–1MiB, random, cold, composition cells all ran `[RUN]`. Outcome: wall PARITY at every claimable size (Δ +0.025…+2.3 ns, all sub-MDE 15.695); stable +19 instr/op PMU constant; syscall parity 1.000 pwrite64/op. Register entry retained for traceability only; no action item remains except the cold-write NO_CLAIM cell (no cold-write A/A exists — would need one only if a cold write claim is ever needed).

**G-WRITEAT-02 — null-data guard (write_at + write)** · low · low ·
measuredSupport **false**
- Source: `src/blocking_file.cpp:107-108` (write_at) and :144-145 (write): 2–3 instr + 1 branch per site, same UB input class as G-READAT-01; A1-pattern comparator carries it; ~2–3 instr ≪ MDE.
- Mechanism/falsifier: same decision gate as G-READAT-01 (delete both sites in one branch, or register a mutation seam + oracle pinning them — today neither exists).

**G-OPEN-01 — open_flags_for constant-table layout** · low · medium ·
measuredSupport **false**
- Source: `src/file_resource.cpp:31-58` (compiled into `File::open` +0x81..0xbc, +0x160..0x190): 6 flag-immediate loads + jump chain; 9 executed conditional branches vs comparator 6; +13 success-path instr (57 vs 44).
- Trigger: every legal-domain File::open (all 9 mode combinations).
- Necessary work: NUL scan, read_only+truncate rejection, correct flag words (O_CLOEXEC + access triple [+ O_CREAT|O_EXCL|O_TRUNC]), one `::open(0644)`, errno mapping, Result<File> construction.
- Magnitude: ≈13 instr ≈ 6–9 ns userspace [UNCERTAIN conversion]; ≤~1% of the ≥1 µs open syscall floor — **measured**: open+close pair +11.487 ns, CI [−15.501, +32.513] spans 0 → PARITY (< 1%·1596.9); PMU pair +74.0 instr/op direction-consistent but includes the whole pair, not this item.
- Mechanism: arithmetic flag construction (OR composition) with a unit-table assertion pinning all 9 output words.
- Falsifier: (a) refactor objdump success path ≤ comparator count; (b) tmpfs short-path open/close 16-ABBA — if no repeatable gain (likely, kernel floor dominates), the #600 admission rule rejects standalone admission; static ablation only.

**G-META-01 — 144-B stat zero-initialization (file_info / size)** · low ·
medium · measuredSupport **false**
- Source: `src/blocking_file.cpp:179` (`struct ::stat st{}`), inlined into size (:193-198): `pxor` + 9×`movaps` = 10 instr / 9 st / 144 B; fstat success overwrites every consumed field (st_mode/st_size/st_dev/st_ino).
- Trigger: every file_info/size call on an open File.
- Magnitude: ≈10 instr ≈ 5–7 ns userspace [UNCERTAIN]; ~0.5–1% of the ≈0.5–1 µs fstat floor — **measured**: file_info wall +30.312 NO_CLAIM (CI spans 0) and PMU −2.0 instr/op, i.e. the ladder mirror carries the same initialization (matched-arm discipline, D3 pattern), so the campaign measured *parity vs the matching discipline*, not vs a no-init floor.
- Mechanism: declare the two buffers uninitialized (no buffer reuse/thread-local — that would be new runtime mechanism with re-entrancy hazards).
- **Admissibility gate has no executable form today**: the audit's gate was
  "the MSan configuration of the 58-test matrix stays clean", but the build
  defines no msan mode (`xmake.lua:5-20` offers only asan/tsan/ubsan/
  asanubsan + debug/release/valgrind), and ASan/UBSan do **not** detect
  reads of uninitialized memory — so the 4-config matrix cannot arbitrate
  this gap. P2 must first add an msan mode (or an equivalent
  uninitialization oracle, e.g. MSan-built focused probes of the two ops)
  before the noinit variant may be admitted; until then this item is
  static-only.
- Falsifier: (a) rebuild → both symbols show noinit shape (49→39 / 31→22 instr); (b) an **msan-built** test configuration (to be added; not part of the existing 4-config matrix) stays clean — no uninitialized value consumed by the projections; (c) size/file_info 16-ABBA — expected n.s. vs the fstat floor.

## 3. Explicit non-gaps (not re-registerable)

Per #600 D3 `NO_CHANGE_JUSTIFIED` and the P1 audits' confirmations, the
following are ABI/compiler cost emitted identically by matched-native arms —
they must **not** be re-registered as candidate gaps: spill/restore shape
across the EINTR loop (`posix_retry.hpp:7-15`); 24/33/56-B sret Result
scaffolding (incl. `Result<CompositionOutcome>`); the dead `os_errno`
zero-store on success; the `Result<void>` bool stack round-trip (3 instr
where 1 would do, byte-identical in N1); +1/+2 cold-block layout `jmp`s
(sync_data/sync_all/resize); the B0-era D1 duplicate sign test (verified
absent at B1). Additionally **not** registered as performance gaps: the
impossible-count guards (2 instr/step per compose loop; no v2-r1 requirement,
but oracle-pinned by `impossible_count_stops_immediately` — keep/remove is an
owner decision under the mechanism-justification gate) and the dududu adapter
double-fstat (P0 §4.5; adapter-side defect, out of Direct scope).

## 4. Mutation/injection seam deficits binding any P2 candidate

Any P2 branch touching compose/direct_outcome must first add named mutant
seams + killer assertions (B1 has **no** composition seam: only the three
admission mutants `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED`,
`…_ADMIT_INVALID_RANGE`, `…_ADMIT_UNREPRESENTABLE_SIZE` exist; C2's two step
seams lived only in the reverted worktree). Required seam set: progress
advances by requested-not-confirmed; zero-progress retry; failure reported
accounted; impossible-count skip (plus the missing write-side
impossible-count script). fdatasync/fsync/ftruncate/open/fstat are not in the
`NativeCall` injection enum (`src/file_test_seams.hpp:16-22`) — EINTR/errno
arms of those ops have no deterministic fault oracle today. See the per-audit
"mutation" notes in `p1-audit/*.md`.
