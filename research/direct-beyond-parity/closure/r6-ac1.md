# R6/AC-1 — E5 semantic closure for positional vectored read/write (#605)

- Issue: Sluice #605 (AC-1, M1 Case A). Scope: mission §9 R6 — the ten
  semantic/design closure items, executed on host E5 with independent
  probe programs. **Design closure only: no public API is adopted, no
  product code is added, no v2-r1 text changes, PR stays OPEN.**
- Verdict: **DESIGN_READY_FOR_OWNER** — all ten items have measured or
  explicitly reasoned answers; the remaining items are Owner decisions,
  enumerated in `OWNER_DECISION_PACKET.md`.

## Deliverables

| File | Content |
|---|---|
| `SEMANTICS.md` | Semantic table S1–S18 (kernel ground truth, each row with its raw evidence file) + mapping to SEM-03/04/05/ERR-02 + atomicity-stance section + kernel-clamp disclosure |
| `ERROR_MATRIX.md` | Error matrix E1–E17 with observed kernel behavior and draft mappings + NOT_RUN_WITH_REASON register |
| `E5_ORACLE.md` | Per-cell oracle definitions (what each distinguishes), observed outcomes, product-oracle obligations if adopted, reproduction commands |
| `API_DRAFT.md` | ABI/API draft (explicitly non-adopted), admission algorithm reusing existing machinery, support matrix, fallback design (what is legal/forbidden) |
| `OWNER_DECISION_PACKET.md` | Decisions OQ-M1-1 (atomicity stance), OQ-M1-2 (iov-fault rows), OQ-M1-3 (reject vs split, quantified), naming/limit surface, sequencing; red-line compliance statement |
| `probes/r6ac1_probe.c` | The independent probe (no Sluice headers; raw `SYS_preadv`/`SYS_pwritev`; 13 cells; machine-readable output) |
| `probes/run_all.sh` | One-command reproduction of every archived raw file |
| `raw/` | 25 archived run outputs + `env-pin.txt` (host/toolchain/probe identity) |

## The ten mission items — answers in one line each

1. **Accurate semantic boundary** — measured, 18 rows (`SEMANTICS.md`
   S1–S18); headline: one call = one contiguous extent in array order;
   short count = exact prefix; EOF-0 success; cursor untouched (incl.
   via `dup`); glibc wrapper ≡ raw syscall.
2. **iov-fault oracle (OQ-M1-2)** — measured and **fs-split**: read-side
   mid-fault = short success on tmpfs and btrfs; write-side mid-fault =
   short success on tmpfs but −1/EFAULT with nothing landed on btrfs
   (stable ×3 each). Contract must treat EFAULT-write effects as unknown
   (`ERROR_MATRIX.md` E10/E11).
3. **`IOV_MAX` upper bound** — exact boundary measured: k=1024 OK,
   k=1025 → EINVAL, pre-transfer, state unchanged; limit must come from
   `sysconf(_SC_IOV_MAX)` (`r6ac1-iovmax.txt`).
4. **Length accumulation overflow** — positive Σ above the kernel
   per-call cap (≈2 GiB) is **silently clamped** into a short success
   (2147483648 requested → 2147479552 returned); negative-as-`ssize_t` Σ
   reaches the kernel as a wild range → EFAULT, so the library must
   reject it itself before any call (`r6ac1-sum-overflow.txt`).
5. **Null/Empty vector** — `iovcnt=0` → success 0 with no effect (the
   candidate never reaches the kernel here); NULL iov with count ≥ 1 →
   EFAULT; zero-length entries skip cleanly (`r6ac1-null-empty.txt`).
6. **Partial read/write** — read: exact prefix across iov boundaries,
   trailers untouched; write: real partial via `RLIMIT_FSIZE` is a
   **silent clamp** (no signal), fully-blocked → EFBIG+SIGXFSZ
   (`r6ac1-short-read.txt`, `r6ac1-fsize-partial-write.txt`).
7. **EINTR** — seccomp-injected: −1/EINTR with zero effect both
   directions, guards intact; "EINTR with partial count" is not
   producible on regular files (`r6ac1-eintr.txt`; register).
8. **Offset not implicitly changed** — cursor invariant incl. through
   `dup`; negative and `INT64_MAX` offsets → EINVAL
   (`r6ac1-cursor.txt`).
9. **SEM-03/04/05 mapping** — table in `SEMANTICS.md` §2; every row
   maps onto existing v2 text with no contract change; admission reuses
   `precheck_data_op`/`checked_posix_offset` classes.
10. **Support matrix & fallback** — Linux/BSD-only primitive (not
    POSIX); `lseek`+`readv` lowering forbidden (SEM-04); k×`read_at`
    lowering only as a declared profile, never silent; non-Linux →
    `not_supported` (`API_DRAFT.md` §3–4).

## Measured surprises worth the Owner's attention

1. **Write-side iov-fault is fs-dependent** (item 2 above) — the single
   strongest reason the contract cannot pin "fault ⇒ short success" or
   "fault ⇒ EFAULT/nothing"; ERR-02 unknown-remainder is the only honest
   row.
2. **No whole-extent atomicity observable** — a single `preadv` tore
   against a concurrent buffered writer at the same low rate as 8×`pread`
   (0–2 vs 1–3 per 5000). The man-page "contiguous block" sentence is not
   a snapshot guarantee; OQ-M1-1 Option C (promise atomicity) is excluded
   by evidence.
3. **Kernel clamp** — requests above ≈2 GiB silently become short
   counts; a "single syscall = whole extent" legend would be false at
   large totals even below the k limit.
4. **The k=1025 rejection is clean and pre-transfer** (state unchanged),
   which makes both OQ-M1-3 options contract-clean.

## Evidence conventions

Every measured number in the documents cites a raw file under `raw/`
(`[RUN]`); transcribed claims cite their archived source (`[TRANSCRIBED]`);
unmeasurable items are registered NOT_RUN_WITH_REASON with the gap owner
(`ERROR_MATRIX.md`, `E5_ORACLE.md` §3). No conclusion in this directory is
marked PASS without an executed check. Performance numbers from P3 are
cited, not re-measured — this phase adds no formal benchmark and runs in
no measurement lane beyond its own serial probe session.

## Boundary compliance

- PR is Research/Design only, base `master`, stays **OPEN**, merges
  nothing.
- No public API adopted: `API_DRAFT.md` is a draft; the product tree is
  untouched.
- v2-r1 contract text: untouched.
- Other agents' work: this directory adds a new subtree
  (`closure/r6-ac1/` + `closure/r6-ac1.md`) that no existing PR owns;
  frozen worktrees and #602 history untouched.
