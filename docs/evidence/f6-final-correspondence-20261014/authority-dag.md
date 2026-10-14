# F6 Round 2 — Authority and Lifetime DAG (frozen master 821c489f)

Read-only reconstruction of every authority edge in the converged v1 tree,
then the nine adversarial risk probes against that DAG. Production names are
as of the frozen tree; every "single/sole/only" claim below was checked by
searching the whole canonical target (include/, src/, apps/), not by trusting
a phase receipt.

## 1. Authority nodes

| Node | Authority owned | Physically holds | Key files |
|---|---|---|---|
| `File` | Sole native fd ownership (open/close/durability/metadata identity) | fd, open policy | `src/core/*`, `include/sluice/file.hpp` |
| `detail/file_semantics.hpp` | Sole shared semantic oracle (legality, errno mapping, short-IO, durability reference rules) | pure functions/tables | `include/sluice/detail/file_semantics.hpp` |
| `NativeFileRef` / file borrow | Borrow-only access; no ownership transfer | fd reference + size | `include/sluice/file_resource.hpp` |
| `AsyncIoContext` | Sole public runtime entry; owns backend, progress source, request storage, shutdown state machine | unique_ptr backend, slots | `include/sluice/async/async_io_context.hpp` |
| `RequestCore` (slot array) | Sole accepted-request state machine: identity (context,slot,generation), publication ordering, reclaim predicate, cancel intent seam | slot storage + serialized transitions | `src/async/detail/request_core.cpp` |
| `Request<T>` | Sole public move-only settlement binding (take/consume/discard); sole public cancel entry | slot pin + generation | `include/sluice/async/request.hpp` |
| `RequestScope` | Owned-driver bounded tracking for W-02; early-return cancellation policy | preallocated tracking, owned driver pump | `include/sluice/async/request_scope.hpp` |
| `ProgressSource` | Driver/no-lost-wake authority; external-loop notification fd (W-03) | persistent signal state, fd | `src/async/*progress*` |
| `AsyncBackend` (seam) | Backend-neutral submission/completion protocol | interface only | `include/sluice/async/` |
| `ThreadPoolBackend` | Thread-pool execution profile | workers, queue | `include/sluice/async/threadpool_backend.hpp` |
| `UringBackend` | io_uring execution profile; poison health authority | ring, registered buffers | `include/sluice/async/uring_backend.hpp` |
| Observer attach state (inside RequestCore) | Delivery arbitration, at-most-once delivery, staged cancel-vs-delivery resolution | per-slot delivery records | `request_core.cpp` |
| External event loop (W-03 consumer) | Borrows notification fd; owns epoll/pump; must detach before shutdown | epoll, borrowed fd | consumer side |
| `StackfulIoHost` (candidate) | W-04 host control flow; NOT shipped/supported | fibers, driver | `include/sluice/async/stackful_io_host.hpp` (installed but candidate-labeled; no support claim) |

Retired-with-authority-removed (verified absent or inert at the frozen tree):
caller-owned `Completion<T>` (F2: removed entirely), legacy
FileReader/FileWriter/buffered wrappers (F4: removed; negative probes
`tests/package/f4_retirement/probes/`), Scheduler/Fiber runtime role (only
inert residue remains — finding F6-F08), `release_public_binding` edge
(F2; model keeps a compatibility flavor — finding F6-F03),
`detail/context_identity.hpp` from the installed package (F5).

## 2. Ownership and borrow edges (lifetime DAG)

```text
user File/buffers ──(borrow, must outlive)──> submitted ReadOp/WriteOp
AsyncIoContext ──owns──> AsyncBackend (unique_ptr, exact-type)
AsyncIoContext ──owns──> ProgressSource
AsyncIoContext ──owns──> RequestCore slot storage
Request<T> ──pin──> slot (settlement binding; move-only; at most one live pin per accepted request)
RequestScope ──owns──> accepted Request<T> set + tracking budget (never user buffers)
RequestScope ──drives──> context poll (owned driver; cannot steal an external loop's driver)
W-03 consumer ──borrows──> progress_notification_fd (interest registered; detach_progress_host() mandatory pre-shutdown)
Observer ──attaches to──> slot delivery state (never owns the operation or its result storage)
StackfulIoHost ──drives──> context (candidate; isolated from core authority)
backend ──borrow-touching refs──> user buffers until retirement proven (BACKEND-03)
```

Destruction order authority: user-owned File/buffers outlive the context;
`Request<T>` pins retire before slot reuse (reclaim predicate); scope
unwinds its accepted set before returning; external loop detaches before
shutdown; shutdown five-state machine (Open → AdmissionClosed → Settling →
ExecutionClosed → Destroyable, ADR-0004) refuses destruction while any
referenced owner is live (V21 fail-fast, V23 detach discipline).

## 3. Cross-cutting authority invariants (each independently re-checked)

1. **Single settlement authority**: exactly one move-only public binding
   (`Request<T>`) per accepted request; no second surviving state machine
   (F2 contraction verified: no `Completion` type, no `release_public_binding`).
2. **Single native resource authority**: `File` is the only owner of fds;
   retired Reader/Writer world removed (F4 probes compile-fail on presence).
3. **Publication before public observation**: slot publication completes
   before any observer/consumer observation; `MutReadyBeforePayload` and
   friends guard it.
4. **Backend/host firewall**: backends and core carry no host-specific
   identity/continuation/routing authority; the stackful host drives through
   the public seam only.
5. **Direct execution independence**: `sluice_core` builds, tests and serves
   W-01 with no async dependency (negative probe: async symbols absent from
   core archive).
6. **No hidden blocking fallback**: profile selection is explicit; the
   no-liburing stub fails construction honestly (#521) rather than falling
   back to a thread pool silently.

## 4. Nine adversarial risk probes (R2)

| # | Adversarial question | Method | Result |
|---|---|---|---|
| A1 | Can two settlement bindings coexist for one accepted request? | Search for any second binding type/alias; execute take-twice, move-assign-over paths | No second binding exists; take-twice and assign-over are the guarded V08/V21 fail-fast paths |
| A2 | Does any code path release the public binding before the terminal is public? | Enumerate all release/retire call sites in request_core.cpp and request.hpp | Only consume/discard/dtor paths release, each requiring pub established; the one historical violation class is the always-on fail-fast (public_request_release_violation_test) |
| A3 | Can a slot be reused while a delivery/execution pin is live? | Reclaim predicate audit + MutLazyReclaim/MutReclaimIgnoresCtl/MutReclaimIgnoresPub mutants | Reclaim requires all pins retired; mutants killed |
| A4 | Can shutdown destroy a context with live referenced owners? | Five-state machine audit + MutM1/MutM8 + V21/V23 lifecycle tests | Refusal/fail-fast paths hold; ordered obligations enforced |
| A5 | Does any backend hold host identity or continuation authority? | Inspect AsyncBackend seam + both backends for host hooks/ids/routing | None; seam is host-neutral; StackfulIoHost is a driver above the seam |
| A6 | Is the external loop's fd interest really retired on detach (no stale wake after context death)? | V23 discriminator (M11) + re-borrow arm; clean-room reproduced the refusal-then-detach ordering | Interest retired before fd close; re-borrow allocates new registration; stale-wake-toward-destroyed-context discriminator is internal and kills M11 |
| A7 | Can the scope steal or block an externally-owned driver? | RequestScope construction paths | Owned-driver mode only; external retained-state mode documented for W-02/W-03 |
| A8 | Does ThreadPool cancellation pretend to interrupt an in-flight syscall? | BACKEND-03 audit + semantic_effect_outcome dispositions | No false interruption claim; dispositions and effect reports honest (V14) |
| A9 | Does any retained-but-unreferenced mechanism still claim authority (zombie authority)? | Whole-tree sweep for retired-role names (Scheduler, Completion, FileReader/Writer, RequestHandle, RequestArena, op_helpers) | Only three residues, all inert: friend-declaration residue (F6-F08), model ReleaseBind flavor (F6-F03), historical docs (correctly labeled) |

## 5. Retirement inventory (what F2–F5 removed, re-verified absent)

| Retired mechanism | Phase | Verification at frozen tree |
|---|---|---|
| caller-owned `Completion<T>` canonical authority | F2 | type absent; canonical binding is `Request<T>` only |
| `release_public_binding` release edge | F2 | call sites absent (model compatibility flavor remains — F6-F03) |
| Scheduler/Fiber runtime, await helpers, waiter/observer test world | F2/F3 | no class definitions; app sources carry no awaits; only inert friend residue (F6-F08) |
| FileReader/FileWriter/buffered/copy/wal/limit/strategy/pool/observed/iovec/memory/reader/writer surfaces | F4 | `tests/package/f4_retirement/probes/` fail if any return; all fail as required |
| Pre-F5 installed header set (incl. detail/context_identity.hpp) + archive bloat | F5 | fresh install = 25 headers (9 core + 16 async), 2 archives, 15 async objects; context_identity.hpp absent from package |

No production authority edge in this DAG depends on a retired mechanism, and
no retired mechanism retains authority at the frozen tree.
