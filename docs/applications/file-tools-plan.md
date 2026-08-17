# File Tools Application Track — Audit and Plan (Phase A)

**Status:** ACTIVE — plan for the copy / hash / grep / tail application track
(task brief "Sluice File Applications Track", 2026-08-17).
**Governing principle:** *Application asks; foundation responds.* No new
foundation abstractions are designed in this track. Gaps are recorded, worked
around locally, and proposed as issues.

---

## 1. Current master capabilities (audited, not assumed)

### 1.1 Existing application

`apps/sluice-copy/` is complete through **Version B**:

| Module | Role | Status |
|---|---|---|
| `main.cpp` | CLI entry; opens files, truncates destination directly | Version A/B only |
| `cli_parse.{hpp,cpp}` | strict decimal parsing, resource caps, `--sync` policy | done |
| `file_domain.{hpp,cpp}` | regular-file domain + same-inode (hardlink) rejection | done |
| `copy_task.{hpp,cpp}` | sequential + bounded pipeline copy over `ApplicationRuntime` + `ThreadPoolBackend`, backend-injecting variants | done |

**Version C (safe temp output + atomic rename + directory durability) is NOT
implemented.** `main.cpp` truncates the destination directly
(`apps/sluice-copy/main.cpp:94`); a mid-copy failure can leave a partial
destination (documented limitation).

Six test targets exist (`sluice_copy_{integration,fault,pipeline_integration,
pipeline_stress,pipeline_contract,cli_parse,file_domain}_test`) — the
registration pattern (app sources compiled into test targets) lives in
`xmake/tests/async.lua:242-421`; the single app target is declared in
`xmake/apps.lua`.

### 1.2 Public async surface available to applications

- `RuntimeBuilder` / `ApplicationRuntime` / `RuntimeTaskContext`
  (`include/sluice/async/application_runtime.hpp`):
  `submit_read/write/sync_data/sync_all` (+ `_request` identity variants),
  `await_completion`, `cancel_waiter`, `cancel_token`. **No spawn capability,
  no public timer/sleep, no file-readiness wait.**
- `ThreadPoolBackend` — real syscalls, configurable
  `ThreadPoolConfig{request_capacity, worker_count}`.
- `FakeAsyncBackend` — public header; deterministic fault injection
  (`auto_bytes/auto_error/auto_eof/auto_short_then_full`,
  `complete_oldest_*`). Usable by tests only (apps use ThreadPoolBackend).
- Positional `ReadOp{fd,dst,len,offset}` / `WriteOp` — `pread`/`pwrite`
  semantics; offset-based backward scanning works without mutating a shared
  file offset.

### 1.3 Conventions to follow

- App structure: `main.cpp` + `cli_parse.{hpp,cpp}` + `<tool>_task.{hpp,cpp}`
  (+ focused local modules); app-local RAII helpers; **public headers only**;
  no `SLUICE_ASYNC_INTERNAL_TESTING`; no `src/` includes.
- Tests: `tests/<app>_<topic>_test.cpp` using `tests/harness.hpp`
  (`SLUICE_TEST_CASE` / `SLUICE_CHECK`, exact-name `SLUICE_TEST_FILTER`),
  registered in `xmake/tests/async.lua` with app sources compiled in.
- Resource limits: fixed `kMax*` constants validated in the public task entry
  points (not only the CLI), mirroring `copy_task.hpp:44-47`.
- stdout = application data; stderr = diagnostics. Unified exit codes
  `0 success / 1 usage / 2 I/O / 3 canceled` (grep adopts the traditional
  `0 match / 1 no-match / 2 error`, documented per brief §17).

---

## 2. Directory / target / test plan

```text
apps/
├── sluice-copy/   (exists; + safe_output.{hpp,cpp}, Version C)
├── sluice-hash/   README.md main.cpp cli_parse.{hpp,cpp} sha256.{hpp,cpp} hash_task.{hpp,cpp}
├── sluice-grep/   README.md main.cpp cli_parse.{hpp,cpp} matcher.{hpp,cpp} grep_task.{hpp,cpp}
└── sluice-tail/   README.md main.cpp cli_parse.{hpp,cpp} tail_task.{hpp,cpp}
```

One xmake target per app (`xmake/apps.lua`, group `apps`). Tests:

```text
tests/sluice_copy_safe_output_test.cpp      (Version C unit/fault)
tests/sluice_copy_atomic_integration_test.cpp (real fs end-to-end)
tests/sluice_hash_sha256_test.cpp           (NIST vectors + streaming)
tests/sluice_hash_cli_parse_test.cpp
tests/sluice_hash_integration_test.cpp      (real files, multi-file, bounds)
tests/sluice_hash_fault_test.cpp           (FakeAsyncBackend injection)
tests/sluice_grep_cli_parse_test.cpp
tests/sluice_grep_matcher_test.cpp          (cross-buffer, boundaries)
tests/sluice_grep_integration_test.cpp      (real files, multi-file ordering)
tests/sluice_grep_fault_test.cpp
tests/sluice_tail_cli_parse_test.cpp
tests/sluice_tail_scan_test.cpp             (last-N backward scan)
tests/sluice_tail_integration_test.cpp      (real files + follow + cancel)
```

No `apps/common/`, no shared header outside app dirs; small duplication is
preferred over speculative abstraction (brief §5).

---

## 3. Per-application design decisions

### 3.1 sluice-copy — Version C safe output (Phase B)

Flow (brief §6.1):

```text
open+validate src (regular file)                     [file_domain reuse]
stat dst if it exists (reject dir; reject same inode as src)
mkstemp("<dst_dir>/.sluice-copy.tmp.XXXXXX") -> temp fd + path
fchmod(temp, src_mode & 0777)                        [permissions policy]
run_pipelined_copy(src_fd, temp_fd, ...)             [unchanged Version B]
    + sync policy applies to the TEMP fd (copy_task already does this)
close temp
rename(temp, dst)                                    [atomic replacement]
if sync != none: fsync(dst_dir fd)                   [directory durability]
on ANY failure before rename: unlink(temp), report, dst untouched
```

- Default **atomic**; `--no-atomic` retains the previous direct-truncate
  behavior (baseline path for existing tests and comparison).
- Cancellation / copy failure / sync failure never touch an existing dst.
- Documented scope: atomicity = rename atomicity on the same filesystem;
  crash durability only with `--sync data|all` (file data + parent dir);
  metadata preserved = permission bits only (no owner/group/xattr/timestamps);
  symlinks not followed specially (source must be a regular file — a symlink
  source is followed by open; destination symlink is replaced by the renamed
  file).
- Not rsync (brief §6.3): no recursion/ACL/xattr/reflink/delta.

### 3.2 sluice-hash (Phase C)

- `sluice-hash [options] <file>...`; `--buffer-size`, `--workers`.
- **SHA-256, app-local implementation** (`apps/sluice-hash/sha256.{hpp,cpp}`),
  a straight FIPS 180-4 implementation validated against NIST test vectors.
  Rationale: the repo has no crypto dependency surface; pulling OpenSSL or a
  third-party hash lib into the only target that needs it adds a build
  dependency (this sandbox has restricted package fetch); implementing the
  published standard with official vectors is not inventing crypto. Recorded
  in findings for review; swap-in of an external implementation later is a
  local change.
- Streaming: `read chunk (positional, ThreadPoolBackend) -> sha256.update ->
  next`; files processed in CLI order, one at a time (V1); `--workers` sizes
  the Runtime (backend worker count), keeping the runtime topology honest
  without complicating output ordering.
- Output `<hex digest>  <filename>` per input (sha256sum-compatible shape);
  read errors isolate per file (report to stderr, continue, final exit 2).
- Bounds: `kMinBufferSize 4 KiB`, `kMaxBufferSize 64 MiB`, `kMaxWorkers 64`;
  memory ≈ `1 × buffer_size` + O(1) state.

### 3.3 sluice-grep (Phase D)

- `sluice-grep [options] <pattern> <file>...`; `-n` line numbers.
- Literal byte-oriented substring matching only (no regex). Empty pattern
  policy: matches every line (documented; matches grep tradition).
- Streaming matcher (`matcher.{hpp,cpp}`): per chunk, keep
  `pattern_len-1` trailing bytes as carry so a match spanning the buffer
  boundary is found; line carry keeps an incomplete trailing line until `\n`
  or EOF (final line without newline is still searched/emitted).
- `max line bytes` policy: a line longer than `--max-line-bytes` (default
  1 MiB, cap 64 MiB) is reported per-line to stderr and skipped (matching
  continues at the next newline); memory stays bounded by
  `buffer_size + max_line_bytes + pattern_len`.
- Binary / invalid UTF-8: byte-oriented; no grapheme semantics; NUL bytes do
  not stop the scan (documented difference from GNU grep's binary detection).
- Multi-file: files processed in CLI order, lines emitted in file order —
  deterministic output; prefix `filename:` only when >1 input file.
- Exit codes: traditional `0 matched / 1 no match / 2 error` (documented).

### 3.4 sluice-tail (Phase E)

- `sluice-tail [options] <file>`; `-n <count>` (default 10; `-n 0` allowed),
  `-f` follow, `--buffer-size`, `--poll-interval <ms>` (50–5000, default 200),
  `--max-line-bytes` (default 1 MiB, cap 64 MiB; a longer line — during the
  initial tail OR follow — is reported to stderr and skipped with correct
  line accounting, the same policy as sluice-grep).
- last-N: bounded backward scan from EOF using positional reads
  (`offset` descends by `buffer_size`); count newlines; stop when N newlines
  found or offset 0. Memory ≈ `buffer_size + retained tail lines ≤
  N × max_line_bytes`.
- Follow mode (finite → long-lived workload):
  - read forward from the post-scan offset until EOF;
  - at EOF: `std::this_thread::sleep_for(poll_interval)` then re-check
    size via `fstat`;
  - truncation (size < offset): notice to stderr, restart reading from
    offset 0 (coreutils-style);
  - **no busy spin**: a single bounded poll per interval; idle CPU ≈ 0
    (acceptance requirement, brief §22).
- Descriptor-follow semantics only (documented): rename/replacement rotation
  keeps following the ORIGINAL inode; `-F` reopen-by-name is out of scope.
- Ctrl-C / cancellation (brief §11): SIGINT/SIGTERM blocked in main; a
  dedicated `sigwait` thread calls `ApplicationRuntime::request_stop()`
  (noexcept, worker-safe) from a real thread — never from the signal handler
  context; the follow task observes `cancel_token()` at loop boundaries
  (≤ poll_interval latency), drains its outstanding Completion, and returns;
  main waits for task completion, then `drain()` + `join()`. No
  `std::exit()` bypass. Follow ended by signal exits **0** (the documented
  normal end of `tail -f`, matching GNU convention); a follow aborted by an
  I/O error exits 2.

---

## 4. Pre-identified foundation constraints (recorded, not fixed here)

| # | Constraint | Class | V1 handling |
|---|---|---|---|
| F1 | No public timer/sleep op or file-readiness wait; `RuntimeTaskContext` cannot express "wait for data OR timeout" without parking a worker thread | gap (findings) | tail uses bounded `sleep_for` polling inside the task; a worker thread is parked for the interval (single-task app; documented) |
| F2 | Signal-safe cancellation entry: `request_stop()` takes locks; calling it from a handler is unsafe | gap (findings) | `sigwait` thread pattern; no private hooks |
| F3 | No crypto primitive in tree | decision | app-local FIPS 180-4 SHA-256 + NIST vectors |
| F4 | Follow-mode rotation detection (inotify/direvent) absent | out of scope | descriptor-follow only, documented |

None of these require (or permit) foundation edits in this track.

---

## 5. Phase order and gates

| Phase | Deliverable | Gate before next phase |
|---|---|---|
| A | this plan | — |
| B | copy Version C + tests | Debug full suite green |
| C | sluice-hash + tests | Debug full suite green |
| D | sluice-grep + tests | Debug full suite green |
| E | sluice-tail + tests | Debug full suite green |
| F | findings doc, root README navigation, final report | Debug + Release green |

Baseline (AGENTS.md §6): Clang Debug configure/build/test before touching
production code; Release (§16.1-class: new binaries, no public header change)
at phase F and on demand; `git diff --check` throughout. Sanitizer gates: not
required by the change classes here (no foundation ownership/lifetime code is
modified); smoke ASan+UBSan run for the new app code is recorded as optional
evidence if time permits.
