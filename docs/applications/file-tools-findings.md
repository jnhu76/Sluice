# File Tools Application Findings

**Track:** copy / hash / grep / tail (`apps/`) — application-driven
development round 1.
**Plan:** `docs/history/implementation-plans/file-tools-plan.md`.
**Environment:** WSL2 Linux x86_64, Clang Release (`-m release`), workloads
on tmpfs `/tmp` (warm page cache), 1 GiB inputs; comparisons against GNU
coreutils (cp 9.x, sha256sum 9.x, grep 3.12, tail 9.x) and ugrep 7.5 as a
SIMD+MT reference point.

---

## Tested applications

- `sluice-copy` (Version C default: temp file + atomic rename + directory
  durability)
- `sluice-hash` (streaming SHA-256)
- `sluice-grep` (streaming literal search)
- `sluice-tail` (bounded backward last-N + follow with clean SIGINT)

All four build and pass their suites in Debug and Release, use ONLY
`include/sluice/*` public headers, no `SLUICE_ASYNC_INTERNAL_TESTING`, no
`src/` includes, no private Scheduler access.

## Public APIs that worked well

- `RuntimeBuilder` / `ApplicationRuntime` start–submit–stop–drain–join
  lifecycle: usable unchanged for both run-to-completion (copy/hash/grep)
  and long-lived (tail follow) workloads;
- `RuntimeTaskContext::submit_read/write/sync_data/sync_all` +
  `await_completion`: the entire submit/await loop pattern drove all four
  apps; short reads/zero-progress handling per the Completion contract;
- positional `ReadOp` at DESCENDING offsets (tail's backward scan) worked
  with no special-casing;
- `ThreadPoolBackend` (real syscalls) with default bounded config across
  every app and test;
- `FakeAsyncBackend` (public header, tests only) gave deterministic
  fault injection with zero production seam;
- `CancelToken` observation at cooperative boundaries was enough for every
  cancellation path, including process-level SIGINT.

## Performance vs the system tools (Release, 1 GiB, tmpfs)

| tool | reference | sluice | ratio | note |
|---|---|---|---|---|
| copy | `cp` 0.59–0.68 s (1.5–1.7 GB/s) | d4 0.68–0.88 s (1.3–1.5 GB/s) | 1.1–1.3× | content byte-identical (`cmp`) |
| copy (sequential) | — | d1 1.0–1.3 s (0.8–1.0 GB/s) | 1.7–2.0× | pipeline depth pays |
| SHA-256 | `sha256sum` 2.70 s (378 MB/s) | 4.06–4.12 s (250 MB/s) | 1.5× | see perf note below |
| literal grep | GNU grep 0.012 s; ugrep 0.15 s | 0.80–0.85 s (1.2–1.3 GB/s) | ~65× / ~5.5× | see perf note below |
| tail -n 10 | 7 ms | 6 ms | **0.86× (faster)** | backward positional scan |
| tail -n 100000 | 9 ms | 61 ms | 6.8× | see below |

### V2 grep update (performance-attribution round, 2026-08)

The V1 grep numbers above predate the attribution round. The V2 matcher
(`apps/sluice-grep/matcher.cpp`, chunk-level anchor-memchr scan with an
incremental line cursor and a borrow-free SWAR newline count) removed the
V1 per-line `std::search` shape. Same-session measurements at 256 MiB
(`scripts/bench/perf-attribution.py` ladder, L4 = full engine,
GB/s medians):

| workload | V1 | V2 | Δ |
|---|---|---|---|
| sparse rare patterns (qz9/1b_z/16b/64b/rare1st/rep) | 0.89–1.92 | 2.43–4.54 | 2.4–3.1× |
| binary | 1.27 | 3.53 | 2.8× |
| dense common anchor (`the` all densities, `e`) | 0.74–0.94 | 0.79–1.08 | ≈1.1× |
| short lines | 0.42 | 0.51 | 1.2× |
| long / huge lines | 5.53 / 5.91 | 5.58 / 5.30 | ≈0.90–1.01× |

L6 CLI (1 GiB, fresh process): sluice-grep 1.9–2.1 GB/s on sparse patterns
(vs V1 ~1.2 GB/s; binary rows run at 1.86 GB/s), byte-identical output to
GNU grep/rg on every workload including binary (competitors run with `-a`
text-mode parity and LC_ALL=C; runner records per-tool md5s). Cold-first-run in a fresh process
measures ~2× the in-process steady-state engine on this host
(fresh-process/runtime-cold, page-cache state not guaranteed cold;
read-path page faults + host load). Sluice-owned symbols account for
<2% of sampled userspace symbols in that profile, but this alone does
not exclude Core overhead mediated through libc, kernel scheduling,
synchronization, or backend handoff; the effect is classified
OS/environment because it reproduces independent of the runtime shape
(also on ext4), not because of the symbol share. Semantic equivalence is proven by
`tests/sluice_grep_matcher_differential_test.cpp` (V2 vs the frozen V1
per-line reference on randomized inputs) plus the unchanged existing matcher
suite. See `docs/verification/performance-attribution.md` for the framework
and full tables.

`perf stat` (hardware counters, user-space):

- sha256sum: 11.0 G cycles, 21.6 G instructions, IPC 1.96 (OpenSSL
  **SHA-NI** accelerated);
- sluice-hash: 14.8 G cycles, **70.3 G instructions**, IPC 4.75 — the
  portable FIPS implementation issues 3.2× more instructions; the 1.5× wall
  gap is exactly the missing SHA-NI path, not the async runtime;
- sluice-grep: 1.77 G cycles, 7.6 G instructions — per-byte streaming scan
  + per-line emit;
- GNU grep's 12 ms is its kwset Boyer-Moore **skip loop** (it does not
  touch every byte) — a different algorithm class, not an I/O difference;
- sluice-copy perf profile: top entries are pthread_mutex_lock + pthread_mutex_unlock
  (≈16%), `clock_gettime` (4.4%), `pthread_cond_wait` — the async
  control-plane (arena/admission/queue coordination), not application
  logic. No single app-level hotspot.

Interpretation (measure first, optimize later — brief §21):

- copy at 1.1–1.3× of `cp` with atomic output is already competitive;
- hash is compute-bound by the crypto implementation (swap-in of a
  SHA-NI-capable implementation is app-local, see F3);
- grep's 65× gap was algorithmic (SIMD/kwset) and out of scope for V1 by
  explicit rule (§23 no SIMD grep). The attribution round (V2 matcher)
  recovered 2.4–3.1× on sparse patterns and 2.8× on binary — the remaining
  gap vs GNU grep/rg is the algorithm class (skip loops that do not touch
  every byte) plus this host's cold-process read path;
- tail -n 10 is faster than coreutils; the 100k-lines case is 6.8× slower
  because sluice forward-streams the retained lines through the bounded
  assembler while GNU tail memsets a chunk ring — acceptable, documented.

## Application attribution table (2026-08 attribution round)

| App | main bottleneck | owner | evidence |
|---|---|---|---|
| grep | V1 per-line `std::search`; V2 fixed sparse/binary (2.4–3.1×/2.8×), dense-anchor rows now emit-bound; remaining gap is skip-loop algorithm class | APP | ladder L2/L4, perf (Sluice symbols <2% of sampled userspace symbols — non-exclusion wording per attribution corrective) |
| hash | portable SHA-256 (no SHA-NI): 3.2× instructions vs OpenSSL | APP | prior perf: 70 G instr vs 21.6 G |
| tail | bounded reverse-scan assembler vs memcpy ring (large-N) | APP | prior table (6.8× at 100k) |
| copy | V3 atomic copy at 1.1–1.3× `cp`; async coordination (mutex/cond/clock) is a visible but small share — see re-verification note below | APP (meets bar); CORE coordination re-measured | perf: mutex ~16% |

### copy coordination re-verification (2026-08)

The V1 finding attributed ~16% to `pthread_mutex_*` and 4.4% to
`clock_gettime` in sluice-copy. Re-profiled on the current baseline
(Release, 1 GiB tmpfs, Version C default, 3140 perf samples): the
coordination class is confirmed — `pthread_mutex_lock+unlock` ≈ 14%,
`pthread_cond_*` (wait/broadcast/signal/notify) ≈ 13%, `clock_gettime`
(libc + vdso) ≈ 6%, plus Scheduler/ThreadPool worker machinery ≈ 10%.
The async control plane (arena/admission/queue coordination + cond waits +
deadline clocks) is ≈ 30% of sluice-copy wall time on this host. It is an
accepted CORE observation: a potential future micro-optimization (e.g.
deduped clock queries on undeadlined waits) is NOT pursued in the
performance branch because it would touch Scheduler wait semantics and
belongs to the formal branch per the optimization promotion rule.
`pump_deadlines_locked` (1.2%) shows the no-deadline path touches the clock
— the exact candidate a future CORE change would target.

## Memory behavior (bounded by configuration, not input)

Measured max RSS (1 GiB inputs):

| tool | config | max RSS | bound formula |
|---|---|---|---|
| sluice-copy | 1 MiB × depth 4 | 8.2 MB | ≈ buffer×depth + O(depth) |
| sluice-hash | 1 MiB buffer | 5.2 MB | ≈ 1×buffer + O(1) |
| sluice-grep | 1 MiB buffer | 5.3 MB | ≈ buffer + max_line + line |
| sluice-tail | 64 KiB buffer | 4.5 MB | ≈ buffer + line carry |

RSS is flat across 1 GiB inputs — memory is bounded by configuration as
the README formulas claim.

## Memory-leak evidence

- **valgrind** (`--leak-check=full`) on all four Release binaries over real
  workloads: `All heap blocks were freed -- no leaks are possible`,
  `ERROR SUMMARY: 0 errors`, including `sluice-tail -f` with an append +
  SIGINT end-to-end under valgrind;
- **ASan+UBSan** (`-m asanubsan`, `detect_leaks=1`): clean for all four
  apps including the follow path after one fix (below);
- the follow path initially tripped ASan `stack-use-after-scope`
  (`apps/sluice-tail/main.cpp`): the sigwait trampoline context lived
  inside the `if (follow)` block while the waiter thread dereferenced it
  after the block closed. Fixed by hoisting `SigCtx` to main scope;
  re-verified clean. This is exactly the class of defect the sanitizer
  pass exists for — the fast path "worked" by stack-slot luck.

## Friction observed

- `RuntimeTaskContext` has no spawn/timer/sleep capability, so a follow
  loop must park a worker thread in `sleep_for` (bounded, sliced) — the
  long-lived workload cannot express "wait for readiness OR timeout"
  natively;
- signal-safe cancellation required the app to own a `sigwait` thread;
  `request_stop()` is thread-safe but not async-signal-safe, and nothing
  public documents a canonical pattern;
- the "run one task to completion and collect its Result" shape
  (app-owned slot + mutex + cv + `std::ref(task)`) was re-implemented in
  three apps;
- strict CLI parsing (digits-only size/worker parser + caps) duplicated in
  four apps;
- no public way to query "how many bytes are readable" on an fd — tail
  follow uses `fstat` from inside the task (fine, but it is raw POSIX in
  app code, invisible to Sluice's error model).

## Missing public capabilities (candidates only — nothing promoted)

1. a public wait-for-readiness / timer op (or an Evented read) so follow
   loops do not park a worker;
2. a documented signal→cancel bridge pattern (or an async-signal-safe
   request hook);
3. fd-size/stat-in-the-I/O-model helper;
4. a "run_to_completion" task helper returning `Result<T>`.

## Local app-side workarounds

- tail: sliced `sleep_for` polling (≤1 stat/interval, measured idle CPU
  ≈ 0: 1 scheduler tick per 3 s);
- tail/main: `sigwait` thread;
- hash: app-local SHA-256 (no dependency-surface change);
- copy: temp+rename implemented app-side over plain POSIX (`safe_output`),
  no core changes.

## Candidate reusable abstractions

| abstraction | apps using it | status |
|---|---|---|
| bounded line assembler (carry + newline split + long-line drop) | grep, tail | **repeated (2)** — recorded, NOT promoted |
| strict CLI decimal parser + caps | all 4 | repeated (4) — an apps/support extraction is justified NEXT round if a 5th app appears; still not core |
| task-terminal slot + done_cv runner | copy, hash, grep | repeated (3) — strongest foundation candidate (see #4 above), needs its own design issue |
| safe temp+rename output | copy | single app — app-local |

## DO NOT PROMOTE YET

Per the track rules: nothing new under include, no shared framework, no
core promotion from this round. The four candidates above each need a
separate design issue with a second real consumer's evidence first.

## Foundation issue candidates

| # | finding | severity | proposed action |
|---|---|---|---|
| F1 | no timer/readiness wait in `RuntimeTaskContext` → follow parks a worker in `sleep_for` | gap (liveness/efficiency) | focused design issue: public sleep op or Evented file readiness |
| F2 | no documented signal→cancellation bridge | gap (usability) | document the sigwait pattern in the ApplicationRuntime ADR or expose a helper |
| F3 | no crypto primitive; portable SHA-256 is 1.5× slower than SHA-NI coreutils on hash-heavy workloads | accepted for V1 | app-local swap-in path identified; revisit if a second hashing app appears |
| F4 | backward-scan tail of very large N is 6.8× slower than GNU tail | accepted for V1 | optimization backlog (chunk-ring emission), not a foundation issue |
| F5 | ASan-only stack-use-after-scope in follow signal path | **fixed** in-app | none (kept here as evidence for the sanitizer gate's value) |

No foundation correctness blockers were found: every behavior in this
track was expressible with public APIs.
