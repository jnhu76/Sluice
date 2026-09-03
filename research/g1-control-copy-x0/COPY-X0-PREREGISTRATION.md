# COPY-X0-PREREGISTRATION — frozen before any formal evidence

- Campaign: COPY-X0 (G1-Control Candidate 2) under #227 / #259
- Status at freeze: audit complete (`COPY-X0-AUDIT.md`); NO harness code, NO
  probe result, NO measurement exists yet at this commit.
- Freeze rule: this document is frozen at its commit hash (recorded below).
  Any later change is additive amendment only (§16), never silent.
- Execution host at freeze: WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`,
  AMD Ryzen 7 5800H, 8 logical CPUs; substrates: tmpfs (`/tmp`), ext4
  (`/dev/sdd`). No btrfs exists on this host; the goal text's "btrfs" column
  is replaced by ext4 — the actual host filesystem — and this substitution is
  itself a frozen decision (the C0 campaign's lesson: never execute a
  substrate label that does not resolve to its canonical fstype).

## FREEZE COMMIT

```text
(recorded by the freeze commit itself; formal evidence sessions must record
HEAD == this commit's descendant with dirty_tracked=false)
```

---

# §1 Research question

> Does an explicit composed `Copy` semantic boundary grant a useful,
> mechanically bounded transformation authority that primitive `read` /
> `write` operations must not have, while preserving the relevant observable
> contract?

Working thesis T-COPY-X0:

```text
Explicit composed operations can serve as Legal Transformation Boundaries.
```

NOT the question: "is copy_file_range faster than read+write" (that is only
the KERNEL TRANSFER VALUE sub-question, = B2 vs B0).

# §2 Hypotheses to falsify

- H1 — Copy boundary unnecessary: a competent application calls
  `copy_file_range()` directly and obtains the same value; Sluice adds wrapper
  ceremony only.
- H2 — Transformation legality cannot be expressed narrowly: honest
  equivalence between buffered and kernel transfer requires huge option sets /
  hidden behavior / capability registries / policy engines.
- H3 — Performance advantage disappears under a competent baseline
  (chunk-qualified B0, matched offsets/bytes/durability/cache state).
- H4 — Semantic differences dominate: partial progress / EOF / offsets /
  unsupported / cross-fs / sparse differences make kernel transfer not a
  legal implementation of one Copy contract.
- H5 — Sluice-specific machinery beyond one thin function + one narrow
  branch + one explicit fallback rule is needed (it is not; if it is, STOP).

# §3 The composed Copy operation under study (declared shape)

All arms implement the SAME declared operation `CopyRange`:

```text
CopyRange(src_fd, src_off, dst_fd, dst_off, n_bytes)
    -> {status: ok(bytes_moved) | error(reason)} + mechanism record
```

- identity copy of `n_bytes` from regular-file source to regular-file
  destination, positional (explicit offsets; shared file offsets untouched by
  the operation except where a declared difference is named in §6 row 9);
- the caller selects the MECHANISM explicitly (buffered | file_range);
  there is NO `automatic` mode in COPY-X0 (goal §14 hostility rule);
- on mechanism unsupported/cross-fs refusal the default policy is FAIL
  (`ReturnInvalidState`-shaped); an explicit caller-requested fallback to
  buffered is legal ONLY if the mechanism record reports it (never silent).

Primitive `read`/`write` operations elsewhere in Sluice remain non-grants: no
arm may perform a kernel-transfer transformation under a read/write label
(M1 enforces detectability).

# §4 Arms (frozen ladder)

| Arm | Definition | Sluice production code? |
|---|---|---|
| B0 | competent standalone buffered copy: `pread`/`pwrite` loop, chunk = host-qualified (§10), inline EINTR retry, matched offsets/bytes | none (raw POSIX in bench) |
| B1 | the REAL current library composed copy: `sluice::copy_all(FileReader, FileWriter, scratch, CopyOptions)` with scratch = qualified chunk; fresh fds pre-positioned via `lseek` for offset fixtures (audit F-1/F-3: copy_all is non-positional) | YES — sluice_core, untouched |
| B2 | competent standalone `copy_file_range` loop: non-NULL `off_in/off_out`, per-call `len` = qualified chunk, partial-return accounting loop, zero-progress rule (§6 row 8) | none (raw POSIX in bench) |
| B3 | research-only Sluice-shaped Copy boundary: ONE thin function `copy_range_x0(..., Mechanism, UnsupportedPolicy)` implementing §3 — descriptor validation, explicit mechanism dispatch (buffered | file_range), explicit observable fallback record, `CopyDecisionX0{requested, selected, reason, mechanism_executed, fallback_occurred}` returned per call. file_range arm is B2's loop wrapped by the boundary ceremony | research-only (in bench); production untouched |

B3 must remain ≤ ~150 lines and contain no registry/planner/manager (M6
enforces structurally). If B3 cannot be written that thin, STOP (H5 → thesis
not earned).

Derived quantities:

```text
KERNEL TRANSFER VALUE        = B2 relative to B0
SLUICE EXISTING CONTROL TAX  = B1 relative to B0
LEGAL-TRANSFORMATION VALUE/COST = B3 relative to B2 and B1
G1-CONTROL VALUE             ≠ B2 > B0 (never inferred from it)
```

# §5 Frozen semantic equivalence matrix (the minimum Copy floor)

Classifications: `MUST MATCH` / `MAY DIFFER IF EXPLICITLY DECLARED` /
`OUT OF CURRENT CONTRACT` / `UNRESOLVED — BLOCK FORMAL A/B`.

| # | Dimension | Classification | Adjudication rule |
|---|---|---|---|
| 1 | bytes_moved == actual bytes moved | MUST MATCH | per-run witness; any arm reporting wrong count = fixture FAIL |
| 2 | dest bytes in [dst_off, dst_off+n) == src bytes in [src_off, src_off+n) | MUST MATCH | full-range checksum + sampled exact compare |
| 3 | dest bytes outside the written range untouched | MUST MATCH | sentinel prefill compared after |
| 4 | dest file size == max(old_size, dst_off+n) | MUST MATCH | stat after |
| 5 | source content unchanged | MUST MATCH | checksum before/after |
| 6 | EOF before requested n → success with bytes_moved < n | MUST MATCH | S3 |
| 7 | partial returns accounted (sum of per-call progress == bytes_moved) | MUST MATCH | S4 call-accounting witness |
| 8 | zero progress on non-empty request without terminal error → deterministic error (0 only legal as EOF) | MUST MATCH | loop rule compiled into B2/B3; sensitivity proven by validator mutant M4 (kernel cannot be forced to return 0 mid-file deterministically) |
| 9 | shared file offsets: untouched (positional arms B0/B2/B3) | MAY DIFFER IF EXPLICITLY DECLARED | B1 (production copy_all) advances shared offsets from pre-positioned start — the one declared difference, recorded per run; harness verifies B1 offsets advanced exactly by bytes_moved |
| 10 | source error / dest error → error return, never false success | MUST MATCH (shape) | error provenance granularity (errno vs IoError code) MAY DIFFER — recorded, not adjudicated; no deterministic dest-error fixture is forced (OUT — forcing ENOSPC/EFBIG nondeterministically would be worse than omitting) |
| 11 | unsupported mechanism (non-regular fd) | MAY DIFFER IF EXPLICITLY DECLARED | S5: pipe source: B0/B1 MUST succeed with correct bytes; B2/B3 MUST fail with unsupported-class error AND B3's decision record MUST name the precondition |
| 12 | cross-filesystem copy | MAY DIFFER IF EXPLICITLY DECLARED | S6: actual kernel disposition recorded per direction (tmpfs→ext4, ext4→tmpfs, same-fs controls); B0/B1 MUST succeed; B2/B3 outcome = whatever the kernel does, RECORDED, never silently retried |
| 13 | sparse source → dest physical layout | OUT OF CURRENT CONTRACT (byte-content contract only); S8 records layout divergence as named-difference evidence (SEEK_HOLE extents recorded, not adjudicated) | fixture records, verdict notes |
| 14 | durability | OUT OF CURRENT CONTRACT — NO arm syncs; no durability credit (S10 witness: zero sync-class syscalls in any arm's op counts + code audit) | recorded |
| 15 | cancellation / async observation | OUT OF COPY-X0 (synchronous harness) | — |
| 16 | concurrent source/dest mutation | OUT OF COPY-X0 | — |
| 17 | same-file aliasing | OUT OF COPY-X0 (harness never aliases; copy_file_range EINVAL noted as mechanism fact) | — |

**No row is UNRESOLVED at freeze** → formal A/B is unblocked. Any row that
becomes contested during execution is appended as amendment, never edited.

# §6 (reserved — floor table is §5)

# §7 Fixtures (deterministic; formal semantic session)

All fixtures on BOTH substrates (tmpfs, ext4) unless stated; every fixture
runs every applicable arm; every run emits one JSONL row with mechanism
record and all witnesses.

| Fixture | Setup | Assertions |
|---|---|---|
| S1 normal full copy | 1 MiB patterned source, empty dest, offsets 0 | rows 1,2,4,5,7,9; checksums |
| S2 non-zero offsets | src_off=4096, dst_off=8192, n=64 KiB, 1 MiB patterned files, dest sentinel-prefilled | rows 1,2,3,4,5,9 |
| S3 EOF before limit | request n=64 KiB from 16 KiB source | rows 1,2,6 |
| S4 partial progress | 64 MiB copy (≥2 partial returns whenever per-call cap < size; call-accounting recorded per arm) | rows 1,7 (+ row 8 rule by mutant) |
| S5 unsupported mechanism | pipe read-end as source fd, n=4 KiB | row 11 |
| S6 cross-filesystem | tmpfs→ext4, ext4→tmpfs, tmpfs→tmpfs, ext4→ext4 (4 KiB and 1 MiB) | row 12 |
| S7 existing destination contents | 1 MiB sentinel dest, overwrite [8192, +64 KiB) | rows 2,3,4 |
| S8 sparse source | 8 MiB sparse: hole [0,2M), data [2M,3M), hole [3M,8M) | bytes MUST MATCH (rows 1,2); layout recorded (row 13) |
| S9 mutation | OUT OF COPY-X0 | — |
| S10 durability witness | every fixture row carries op counts; sync-class count must be 0 for all arms | row 14 |

# §8 Performance matrix (formal perf session)

```text
sizes:        4 KiB, 64 KiB, 1 MiB, 64 MiB
substrates:   tmpfs, ext4   (fail-closed statfs identity per run)
regime:       WARM page cache: source written then fully read once before
              the measured set; dest file pre-created per run (same protocol
              for every arm); no fsync anywhere
arms:         B0, B1, B2, B3 (identity copy, offsets 0)
rounds:       9 paired rounds per cell; arm order within a round permuted by
              seeded PRNG (seed frozen per session: 20260903X0)
pairing:      all four arms in a round copy the SAME source file instance
              and same-size dest slots, back-to-back
A/A noise:    phase 0 of the perf session: B0 twice under shuffled labels,
              9 rounds × 4 sizes × 2 substrates → per-cell p90 of
              |log2 paired ratio| envelope (C0/COPY-AB-1 nearest-rank rule)
metrics:      wall time per copy (primary; median across rounds),
              CPU time (getrusage utime+stime), per-call counts
              (read/write/copy_file_range syscalls as counted by the arm
              loops), bytes moved (must equal size)
```

64 MiB × 4 cells × 4 arms × 9 rounds = 288 formal perf rows + 144 A/A rows.
4 KiB is a control cell (expected syscall-latency-dominated), never optimized
around.

# §9 Frozen materiality rule (chosen for this campaign, not copied)

```text
cell direction (arm X vs B0, per cell):
    median_log2(X/B0) ≤ log2(0.95)  (≥5% faster)
    AND sign test: ≥7/9 rounds with X faster
        → cell FAST
    median_log2(X/B0) ≥ log2(1.05) AND ≥7/9 slower → cell SLOW
    otherwise → cell NONE

cell verdict MATERIAL (not regime-local):
    cell FAST  AND  support: ≥1 neighboring size (same substrate) OR same
    size (other substrate) also FAST with ≥3% median AND ≥6/9 sign
    → MATERIAL
    cell FAST without support → REGIME-LOCAL
    cell SLOW always reported; ≥5% + ≥7/9 → REGRESSION cell

campaign KERNEL TRANSFER BENEFIT (B2 vs B0):
    ≥1 MATERIAL cell            → REGIME SUPPORTED, MATERIAL
    only REGIME-LOCAL FAST      → REGIME-LOCAL
    no FAST cell                → NOT ESTABLISHED
    infrastructure blocked      → BLOCKED (not a performance verdict)
```

Rationale recorded at freeze: whole-copy workloads at these sizes have
host-local dispersion (WSL2, shared Windows host); 5% + sign-test + neighbor
support requires regime coherence instead of one lucky cell. Threshold may
NOT be changed after the first formal perf row exists.

# §10 Qualification (post-freeze, pre-formal; recorded, rule-frozen)

Q0 session `copy-x0-qualify-native-1`:
1. binary self-tests + fixture smoke on both substrates;
2. mechanism existence probe: `copy_file_range` on (tmpfs,tmpfs),
   (ext4,ext4), (tmpfs,ext4), (ext4,tmpfs) — records return/errno, per-call
   cap (binary-searched once at 64 MiB… recorded as observed maximum
   successful single-call len), glibc/kernel presence (ENOSYS check);
3. B0 chunk qualification: candidates {8 KiB, 64 KiB, 256 KiB, 1 MiB, 4 MiB}
   × {tmpfs, ext4} × 5 reps at 64 MiB warm regime.
   FROZEN SELECTION RULE: qualified chunk = candidate minimizing
   max(median_tmpfs, median_ext4); tie → smaller chunk. B0 and B1 both use
   it (B1 scratch = chunk); B2/B3 per-call len = chunk.
   Host-0 CHUNK-E0 values are NOT imported (different host; audit F-6).

Probe dispositions do NOT precondition the matrix: unsupported outcomes are
fixtures evidence (S5/S6 re-record them formally); only a total ENOSYS
(absent syscall) BLOCKS B2/B3 formal perf (then CAPABILITY = BLOCKED with
the probe as evidence — that is a valid campaign outcome, not a failure).

# §11 Validator rules (fail-closed; `scripts/validate_copy_x0.py`)

Modes: `--self-test`, `--session <dir>`, `--composite <dirs...>`.
Hard requirements:
1. substrate authority: per run, label→path→`statfs` fstype must equal the
   canonical fstype recorded in environment.json; mismatch = invalid session;
2. every formal row carries `mechanism_executed`
   (`buffered_read_write` | `copy_file_range` | `unsupported_error`) and it
   must be consistent with arm + outcome + op counts (e.g. bytes_moved>0
   with mechanism copy_file_range requires cfr_calls≥1 and rw-read==0 for
   the transfer phase);
3. exact-byte witness: per run, dest checksum + size must equal the
   fixture/matrix expectation;
4. verdict re-derivation: semantic fixture verdicts and ALL performance
   verdicts (§9) are re-derived from raw rows; stored verdict must equal
   derived verdict (C0 Corrective-1 P1-3 lesson);
5. formal sessions must be commit-pinned (HEAD recorded, `dirty_tracked`
   false; tracked-files-only pin per C0 Corrective-2);
6. any row bearing a `mutant` or `control` id in a formal corpus = invalid;
7. rep-count/round structure must match §8 exactly (per cell 9 rows, arms
   complete, permutation present);
8. `--self-test` must prove by injected rows that M1–M5 are REJECTED and
   that both falsification directions of the §9 rule (fabricated benefit,
   erased benefit) are caught.

# §12 Fallback policy (frozen answer to goal §14)

- NO automatic mechanism selection exists in COPY-X0.
- B3's default: mechanism requested = mechanism attempted; unsupported →
  error naming the precondition (fail-closed).
- B3's explicit opt-in: caller passes `UnsupportedPolicy::FallbackToBuffered`
  → fallback runs ONLY IF recorded: `fallback_occurred=true`,
  `mechanism_executed` reflects the mechanism that moved bytes, and the
  row's op counts corroborate. A fallback not recorded = invalid (M3).
- The report must answer: caller-choice vs hidden selection. Frozen
  expectation to falsify: hidden selection is NOT justified by anything in
  this campaign; if evidence contradicts, that is an amendment with data.

# §13 Mutants (validator/design sensitivity; never in formal corpora)

| Mutant | Construct | Expected detection |
|---|---|---|
| M1 hidden primitive transformation | row: arm=B1, mechanism_executed=copy_file_range | validator rule 2 rejects (STRUCTURAL REJECTION) |
| M2 wrong progress | row: bytes_moved ≠ actual (dest size/checksum contradict) | rules 2/3 reject |
| M3 silent fallback | row: fallback happened (op counts show buffered) but mechanism says copy_file_range, fallback_occurred absent | rule 2/12 rejects |
| M4 misleading success | row: bytes_moved=0 + status ok on non-EOF fixture context | rules 2/3 + row-8 rule reject |
| M5 weak baseline | formal row: B0 with chunk ≠ qualified chunk (control row at 16-byte chunk recorded as demonstration) | rule 7 + §10 chunk pin reject |
| M6 framework inflation | synthetic source text containing CapabilityRegistry/TransferManager/ResourceManager/DataToken/planner/autotuner | `scripts/check_copy_x0_design.py` gate fires |

# §14 Provenance (per session environment.json)

git HEAD, dirty_tracked (tracked-files-only), bench binary sha256, compiler
+ mode, kernel, libc, per-label root path + statfs fstype + mount source,
storage device, CPU, run parameters, seeds, per-run mechanism_executed,
fallback state, timing clock (CLOCK_MONOTONIC), getrusage. Derived summaries
(CSV/JSON) must be reproducible from raw JSONL by a committed script.

# §15 Stop / promotion gates (frozen)

Gate A (capability): B2 vs B0 MATERIAL in ≥1 fairly-compared regime, OR an
independently strong non-performance capability found (e.g. S6 shows a
declared, bounded cross-fs behavior a raw caller cannot obtain honestly).
Gate B (semantic boundary): §5 floor preserved — all MUST MATCH rows pass on
all arms; differences exist only in declared rows.
Gate C (Sluice control value): result does NOT reduce to "call
copy_file_range directly" — requires at least one mechanically demonstrated
control property (M1 rejection enforcing primitive non-grant; M3 observable
fallback; S5/S6 declared preconditions).
Gate D (minimality): B3 ≤ thin branch; M6 gate green; no second real use
case demanded a framework.

STOP conditions (any → no C1): the nine conditions of the task brief §22.
A STOP verdict is a successful campaign outcome.

# §16 Amendments

Append-only. Numbered, dated, signed with commit hash. Frozen thresholds,
matrix, arms, fixtures, and classifications are NEVER edited; amendments may
only add observations, supersede sessions with disclosed defects (C0
Corrective-2 pattern: old evidence retained byte-identical, excluded from
derived numbers), or add narrowly-justified secondary mechanisms (splice —
only under the goal §8 conditions).

# §16 Amendments

## Amendment 1 (2026-09-03, pre-formal) — CPU pinning of bench invocations

Smoke calibration on the execution host showed A/A dispersion (p90 of
|log2 paired ratio| up to ~0.46) dominated by WSL2 scheduler placement.
Per goal §10 ("CPU placement where useful"), every bench invocation in
every session is now pinned (`taskset -c 2`, recorded per session in
environment.json as `cpu_pin`). Protocol hygiene only: thresholds, matrix,
arms, fixtures, rounds, and materiality rules are untouched. Adopted
BEFORE any formal session exists.

## Amendment 2 (2026-09-03, post-native-2 supersession) — A/A calibration bar

Formal perf session copy-x0-perf-native-2 recorded mechanically documented
host stalls in its own A/A calibration (tmpfs 64 MiB pair 0.023s vs 5.43s =
7.86 log2; ext4 64 MiB pair 16×), poisoning round signs for every cell.
Freeze a mechanical validity bar, effective immediately and enforced by the
validator (fail-closed):

```text
a perf session is measurement-valid only if the A/A envelope
(max over cells of per-cell p90 |log2 paired ratio|) ≤ 0.50
```

Sessions exceeding the bar are SUPERSEDED — DEGRADED HOST CONDITIONS
(retained byte-identical, excluded from every derived number) and re-run
under new session names; every attempt is committed and disclosed. This
converts stall-detection into a pre-declared calibration gate — reruns are
lawful only through this gate, never through inspection of verdict
outcomes. Thresholds, matrix, arms, fixtures, rounds, and the §9
materiality rule remain untouched.

# §17 Host claim scope

All conclusions HOST-LOCAL (WSL2 kernel 6.18.33.2, Ryzen 7 5800H, tmpfs +
ext4). No claim about other kernels, filesystems, substrates, or hosts.
Kernel mechanism facts version-bound to this kernel; glibc version recorded
per session.
