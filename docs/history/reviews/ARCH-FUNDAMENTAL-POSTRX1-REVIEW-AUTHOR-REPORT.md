# FUNDAMENTAL ARCHITECTURE REVIEW — Sluice post-RX-1

**Type:** adversarial foundation review (review only; no production code, no PRs, no issue-body mutations)
**Input scope:** #225 (north star + 2026-08-27 "runtime-policy substrate" amendment comment), #221, #227, #234/#235/#236 (AC-1a + RX-1), constitution AC-1..15, AGENTS.md, as-built sources at `master@e10e181`.
**Method decision records:** DR-01..DR-12 inline. Each record states the choice, why it was chosen, the alternatives rejected, and the limitations retained.
**Deliverable purpose:** a human uses this document to decide whether #225 / #221 / #227 need rewriting. It does NOT pre-decide that outcome.

---

## DECISION RECORD INDEX

| ID | Decision | Why | Alternatives considered | Recorded limitation |
|---|---|---|---|---|
| DR-01 | Base every quantitative claim on already-merged evidence (E1 round-1 medians, RX-1 formal runs); run no new measurements for this review | A review whose numbers are produced by its own new benchmark cannot be adversarial to itself; reuse of preregistered, artifact-manifested evidence is the strongest available base | Run a fresh micro-tax probe to double-check E1 (rejected: duplicate evidence, violates "cheapest falsification first"; risk of session-local tuning) | All cost statements are single-host (WSL2, tmpfs/cache-hot). Device-bound cells do not exist yet; ratios there may differ |
| DR-02 | Distinguish **medium tax** (execution mechanism: thread handoff, kernel boundary) from **control-plane tax** (request machinery: validation/reservation/binding/queue/reap) everywhere in this document | E1 normatively defines `Sluice incremental tax = L2−L1` and forbids reporting `L2−L0` as library overhead; conflating them is exactly how the amendment's intuition miscounts (see SIMPLE-TASK TAX MAP) | Report only aggregate wall-time deltas (rejected: hides which layer to optimize) | Some per-op costs (notify-under-load, arena contention) were never decomposed internally — classified UNKNOWN honestly |
| DR-03 | Treat Zig std.Io comparisons as method precedent, not code to port | Sluice owns Completion/cancellation/generation/resource bounds; Zig std.Io is synchronous caller-driven; lifecycle obligations differ fundamentally (see ZIG REVIEW §D) | Adopt buffer-above-vtable literally into Reader/Writer (rejected: solves a dispatch-cost problem Sluice has not measured); adopt Zig Threaded engine model (rejected: contradicts AC-7/AC-8 resource distinctions) | Zig 0.16/master features (`Threaded`, `IoUring` engines) are moving targets; verified against actual master source (lib/std/Io/*) not blog paraphrase |
| DR-04 | Literature extraction limited to one architectural lesson per system, each mapped to a named Sluice design question | Prompt forbids literature dumps; mapping discipline keeps citations falsifiable | Full survey with quantitative tables (rejected: unfalsifiable bulk) | Decision-frequency figures quoted only where primary source confirmed (Shenango 5 µs interval; others qualitative) |
| DR-05 | Tax audit classifies layers by authority analysis over as-built call chains + E1/RX-1 measurements, never by intuition | AGENTS §3 requires characterization of as-built behavior; call chains were traced file-by-file for this review | Static-analysis-only reasoning without measured anchors (rejected: unmeasurable conjecture becomes verdicts) | Per-layer internal attribution beyond flamegraph granularity is UNKNOWN; marked as such |
| DR-06 | Q7 fast-path verdict issued under the constraint that any cheap path must remain inside RequestArena/publication authorities | Constitution AC-5/AC-13/AC-14 make publication and identity single-authority; bypasses outside those authorities are semantic forks by definition | Endorse a parallel zero-machinery lane (rejected: duplicates correctness authority); REJECT outright (rejected: topology variability is already sanctioned by AC-8) | No protocol/model work exists yet proving direct-dispatch modes close cancel/wake races — that proof burden is part of the verdict |
| DR-07 | Transformation-legality matrix derived from a single principle: transformations are legal only where an operation's PUBLIC contract grants freedom (composed operations), and every observable deviation must be preserved or tightened | Cross-domain invariant #1 ("explicit operation semantics are not silently rewritten") plus R4's explicit prohibition on silent 4 KiB coalescing | Case-by-case pragmatism (rejected: no rule survives review); all-transformations-forbidden (rejected: contradicts legal composed-op chunking that copy helpers legitimately exercise today) | Matrix covers filesystem-stream ops tested by this repo's shapes; socket/cert regimes out of scope |
| DR-08 | Regime taxonomy includes detection-reliability caveats grounded in RX-1's observed failure mode (`active_workers == configured` behaved as shape-dependent duty-cycle proxy) | RX-1 is the repo's own controlled experiment on regime signal quality; ignoring it would repeat the failure | Assume regime detection is cheap and reliable (rejected: directly contradicted by merged evidence) | Detection claims hold for ThreadPool cache-hot scope only |
| DR-09 | Architecture options evaluated against: correctness-authority count, product positioning honesty, evidence cost | The review question is explicitly "smallest architecture providing average product value", not elegance | Single-option advocacy (rejected: prompt requires ≥4 competing directions, no forced winner) | No option was prototyped; comparative ratings are analytic |
| DR-10 | Issue recommendations given as exact proposed changes, nothing mutated | Original task forbids GitHub mutation; user requested report submission to an issue → submitted as ONE NEW dedicated issue cross-referencing the three | Post the report as comments inside #225/#221/#227 (rejected: mutates bodies of issues whose rewrite is itself under human decision) | Backlink notifications will appear on #225/#221/#227 timelines automatically via references; body text untouched |
| DR-11 | Verdict for "should simple workloads have a cheaper path" reflects that the cheap path ALREADY EXISTS synchronously | Blocking core traces show `FileReader::read_at` ≈ validation + one syscall; async plane is opt-in. Claims that "every op pays full request machinery" are false as stated today | Verdict YES—ARCHITECTURALLY REQUIRED (rejected: implies a missing mechanism that actually needs evidence first) | Async-plane depth-1 cost structure could still justify a cheaper async topology — open question fed to RE-2 |
| DR-12 | Report written to `docs/history/reviews/ARCH-FUNDAMENTAL-POSTRX1-REVIEW-AUTHOR-REPORT.md` following the ALL-CAPS `<CAMPAIGN>-<SUBJECT>-<ROLE>-<SEQ>` convention, kept UNTRACKED until human decides | Repo doc conventions; avoids mutating git tree during a review-only task | Commit directly (rejected: commits require approved scope; leaving untracked preserves working-tree contract per AGENTS §5) | Untracked files are invisible to CI gates; committing requires a later authorized slice |

---

# VERDICT — one screen

1. **The amendment's central intuition is directionally right but measurably aimed at the wrong target.** On the only measured host profile, of ~35 µs/op coordination cost at 4 KiB depth-1 through the ThreadPool path, the **control plane (all of Sluice's request/Completion machinery) is ~1.3–2 µs (~5%)**, while the **medium (per-op thread-offload round trip, L1−L0) is ~33–35 µs (~95%)**. "Don't make simple ops pay for explicit machinery" must therefore be re-aimed: thinning request machinery attacks 5%; changing execution *topology* or using capability specialization attacks 95%. (DR-02)
2. **The cheap path for simple tasks already exists in the sync core.** One positional read today = descriptor check + one syscall wrapper. No reservation, no arena, no Completion. The real open question is whether the *async* plane needs a cheaper depth-1 topology — an evidence question, not yet an architecture fact. (Q17C = PROBABLY)
3. **"Explicitness as runtime policy substrate" is premature as phrased.** It presumes runtime-mediated policy selection will pay for the information the runtime holds. RX-1 just showed even *reading* that information did not improve attribution; nothing yet shows a planner beats fixed policies (and NSDI'22 found static allocations often beat dynamic reallocation for small tasks). Verdict: YES, BUT ONLY AS OPTIONAL POLICY CAPABILITY. (Q17A/B)
4. **Capability-local specialization is the highest-evidence-per-effort performance lever** — external studies show io_uring-class APIs win mostly *through* features (registered buffers, vectored ops, SQPOLL…), none of which Sluice's async backends expose today (verified by grep: no read_fixed/register_buffers/sendfile/splice/multishot anywhere). Move R5 partly earlier behind concrete use cases. (Q17D = PARTLY)
5. **Fast paths are POSSIBLE BUT HIGH AUTHORITY-RISK:** they must be expressed as backend execution topologies inside AC-8, staying within RequestArena/publication/single-reap authorities. Two topologies already exist in-tree (blocking core; SyncBackend completes-at-poll), so the concept is compatible — but no wake/cancel race model for a third (direct-dispatch) topology exists yet. (Q7)
6. **AC-2/R2 Wait stays next campaign, in parallel with a small preregistered performance-envelope batch (RE-1..RE-4).** Wait deduplication is correctness debt orthogonal to the product-performance question. (Q17E = option B)
7. Product positioning remains undecided and must stay in human hands: negative case (POSIX/liburing suffice) wins for *everyday throughput parity claims*; positive case stands where boundedness, cancellation verbatim-preservation, deterministic replay, and overload stability are the product. Sluice should stop trying to win depth-1 latency and should own a documented envelope instead.

---

## ROOT QUESTION

> What is the smallest architecture in which explicit I/O provides average product value rather than only niche/research value?

Answer in one sentence after all evidence:

> The smallest such architecture is the current one **minus** any further control-plane expansion, **plus** (a) one decided product positioning statement, (b) capability-level specialization for transfer-heavy workloads implemented locally per object/backend, (c) an envelope map that tells users where mediated execution repays itself — with adaptive/planner policy remaining unauthorized unless RE-2 shows stable avoidable regret.

Two structural facts frame everything below:

- Sluice already *has* two execution planes: synchronous core (near-minimal, closes most "simple-task tax" complaints structurally) and opt-in asynchronous runtime (pays full explicit machinery + medium tax).
- The expensive constant in the async plane is dominated by the medium (thread offload), not by the explicit request machinery. This reframes "pay control-plane tax only where it buys something": control-plane tax is currently small; medium tax is large and is a property of *chosen topology*, not of explicitness.

---

## WHAT "EXPLICIT" SHOULD MEAN

### Classification table (Q1)

Test applied: something is EXPLICIT only if exposing it changes what the application can legitimately depend on (semantics, bounds, capabilities) — configuration alone does not qualify (DR note: follows "do not call something explicit merely because it is configurable").

| Candidate concept | Class | Reasoning |
|---|---|---|
| operation semantic intent | SEMANTIC CONTRACT | Defines the visible meaning of the operation; rewriting it silently violates invariant #1 |
| offset/range | SEMANTIC CONTRACT | Part of positional-op identity; positional I/O must not mutate shared offset (AGENTS §9) |
| durability intent | SEMANTIC CONTRACT | `sync_data` vs `sync_all` distinctness is contractually protected; flush≠durability |
| cancellation policy | SEMANTIC CONTRACT | AC-9 layering + "ordinary result preserved verbatim" rules are user-visible semantics |
| timeout/deadline | SEMANTIC CONTRACT (outcome-visible), enforced by EXECUTION MECHANISM (wait/timer registration) | Whether an op can wait forever vs time out with a typed error is caller-visible; timer retirement mechanics are not |
| resource bounds | RESOURCE BOUND | Direct instantiation of AC-7 |
| request capacity | RESOURCE BOUND | Arena slot bound; saturation behavior (`would_block` before acceptance) is specified |
| bytes in flight | RESOURCE BOUND | Distinct bounded quantity (#225 R4 list) |
| application pipeline depth | RESOURCE BOUND | Caller-owned concurrency; distinct from backend resources (AGENTS §12) |
| backend capabilities | CAPABILITY | What mechanisms a backend/object can legally offer (vectored, registered, sendfile…) |
| backend choice | EXECUTION POLICY selected among CAPABILITY-qualified alternatives | Choice changes performance/portability, never observable semantics if contracts hold (E2 thesis) |
| worker count | RESOURCE BOUND constraining an EXECUTION MECHANISM | Bounded, named, high-watered resource per AGENTS §12 |
| SQ/CQ depth | RESOURCE BOUND (backend-specific) | Ring capacity; must remain distinct from request capacity (AGENTS §12.2) |
| batching | EXECUTION MECHANISM when internal; EXECUTION POLICY when caller-directed through composed helpers | Internal batching must obey the transformation matrix; helper-exposed batch size is policy with measured defaults |
| request size | dual: SEMANTIC CONTRACT for application-authored ops; EXECUTION POLICY inside composed helpers | Exactly the semantic-unit / execution-granularity seam defined in #225 R4/E1c |
| scheduling policy | EXECUTION POLICY | Fairness/wake precedence choices inside wait/scheduler authorities |
| observation level | IMPLEMENTATION DETAIL exposed through interface accessors (AC-1a) | Neither semantics nor bound; RX-1 stopped expansion; accessors are pull-based diagnostics |
| execution strategy | EXECUTION POLICY | Threaded/evented/topology choices (AC-8 separation) |

### Why these classes matter operationally

- Semantic contracts get invariant protection, formal models, and forbidden-transformation matrices — never optimization trade-offs.
- Resource bounds get capacity/high-water accounting and saturation behavior — never silently convertible.
- Capabilities get discovery + fallback semantics — never global planning hidden inside objects.
- Execution policies live *behind* the three above and are replaceable without API breaks.

This is the amendment's useful core ("explicit = constrained, explainable, testable, replaceable") retained after stripping the parts its evidence does not support.

---

## SEMANTIC UNIT VS EXECUTION UNIT (transformation legality matrix)

Principle (DR-07): **a transformation is legal only inside an operation whose public contract grants freedom, and every observable deviation (error kind, shortfall, partial progress, cancellation visibility, crash window) is either preserved or strictly tightened.** Silent transformation inside primitives is prohibited regardless of efficiency.

| Transformation | Legal? | Binding constraints / conditions |
|---|---|---|
| Split one semantic op into multiple physical ops | PROHIBITED as silent Core behavior; LEGAL inside composed helper if helper's contract allows partial progress and aggregates outcomes | Partial-failure observability, short-count fidelity, cancellation mid-sequence must collapse to ONE caller-visible terminal matching the primitive contract |
| Coalesce adjacent same-direction semantic ops into larger physical op | Same as split row | Byte counts returned, error precedence (ENOSPC ordering), and crash-visible write windows change → only legal where caller opted into a defined "chunk freedom" (copy helper scratch/chunk options exist today as the pattern) |
| Replace read+write copy with sendfile/splice | LEGAL as capability-gated strategy with declared fallback | Error surface differs (EINVAL on non-pipe/req targets, ENOSPC mapping); EOF/short-transfer semantics must map into existing Result/IoError vocabulary; decision surfaced (current `CopyDecision` does this); borrow/lifetime unchanged |
| Vectorize N same-buffer-family ops into readv/writev | LEGAL as capability; LEGAL internally where aggregation invisible | iovec count limits; byte-total equivalence; stats fidelity (VectorStats exist) |
| Internally batch submissions/completions | LEGAL (mechanism level) if wake obligations closed and reclamation bounded | Per-op terminals remain exactly-once; reap ring batching already exists — this row is already exercised practice |
| Reorder independent completions across ops | Already legal | Generation validation makes completion order non-normative |
| Zero-copy handoff extending buffer borrow lifetime | PROHIBITED unless borrow contract lifetime extended & documented | Borrowed buffers caller-owned/stable is an Accepted contract (§9) |
| Shortcut single-op submission to inline execution skipping queue/workers | CONDITIONALLY LEGAL as a backend topology (see FAST-PATH REVIEW) | Must preserve AC-3 transactional failure atomicity, AC-5 single publication, AC-9 cancel arbitration; SyncBackend proves the degenerate form works |

Distinguish semantic *rewriting* (forbidden in Core) from execution *specialization* (legal behind contracts): Zig draws the identical line between interface promises and implementation freedom; #225 R4 already drew it for request sizes.

---

## ZIG std.Io REVIEW

Sources: release notes 0.15.x ("Writergate"), `lib/std/Io/Writer.zig` and `lib/std/Io/Reader.zig` from ziglang/zig master (read directly), master tree showing `Io/{Threaded,IoUring,Kqueue}.zig`. (DR-03)

### A. Buffer-above-vtable / guaranteed fast path

- Writer fields: `vtable`, `buffer: []u8`, `end: usize` — "the buffer is **in the interface, not the implementation**."
- Contract comment on `drain`: called "only if [the data] could not fit into `buffer`, or during a `flush` operation."
- `write()`: branchHint(.likely) buffered memcpy path returns before ever touching vtable; docs guarantee `writeVec`/`writeSplat` "guaranteed … not call into `VTable`" when fitting.
- Architectural principle extracted: **unified semantic interface ≠ uniformly expensive execution path. Enforce the expense boundary inside the type, with an explicit guaranteed fast region and an unavoidable slow region, both documented.**

Does Sluice violate this? **Partially — on the async plane, not the sync plane.**
- Sync plane honors the principle: blocking positional/sequential paths are single-syscall wrappers; buffered Reader/Writer put caller-owned buffers in front; `BufferedReadable` gives copy a real drain-buffered-data fast path.
- Async plane has no equivalent "guaranteed cheap region": every submit traverses transaction+binding+ring+wake regardless of context, and every await crosses notification plumbing. Whether that matters quantitatively is UNKNOWN at present scales (E1 shows the async-plane *machinery* share ≈ 5% of coordination cost), so no violation verdict — a risk-register entry pending RE-2 envelope data.

### B. Capability propagation

- Specialization is **local and object-scoped**: optional vtable entries with documented defaults (`sendFile = unimplementedSendFile`) plus a published fallback idiom: callee answers `error.Unimplemented`; caller (e.g., `sendFileAll`) downshifts to `sendFileReadingAll` and flips `file_reader.mode = …toReading()` so later attempts skip the failed path.
- Callers own buffers and buffering decisions; implementations advertise what they can do better than the generic loop.
- No global planner anywhere in the design; "planner-like" authority never materialized because capability discovery (who can do what) and strategy selection (what the caller wants) are separate.
- Master's `std.Io.IoUring.zig` / `Threaded.zig` confirm engines behind the same streams rather than a scheduler deciding per request.

Transfer to Sluice: nearly perfect fit with R5's "common semantic minimum + explicit capability differences", `CopyStrategy` deferred labels (`sendfile_deferred`, `splice_deferred` exist as names with `unsupported_policy` handling — declared-deferred, grep-confirmed unimplemented), and the existing opt-in `BufferedReadable` trait. The `Unimplemented→fallback→sticky-mode` trio is a reusable sketch for any new Sluice capability.

### C. Old std.io abstraction tax (lessons)

Documented pathologies: generic readers/writers forwarded through `anytype` universes causing compile bloat and dispatch friction; `AnyReader/AnyWriter` type erasure adding indirection to every byte; byte-at-a-time pipelines paying virtual dispatch far from the I/O. Writergate's answer was not "optimize the erased path" but **re-place the boundary**: concrete types, fast region above the escape hatch.

Sluice relevance:
- Boundary placement: Sluice's hot regions are syscalls + thread switches, thousands-of-cycles events; virtual dispatch (ns-scale) is noise there — *today*. The lesson transfers as preventive architecture (fast regions documented at interfaces) rather than urgent surgery.
- Dispatch cost reality-check (honest limitation, DR-05): no Sluice measurement isolates Reader vtable cost; flamegraphs in E1 diagnostics show control-plane overhead ≈ +0.4% syscalls-side. Treat "Sluice has Zig's old pathology" as FALSE-in-measured-scope, keep monitoring.

### D. Do-not-copy-blindly list

Not transferable: Zig's synchronous cooperative world (no OS-owned in-flight requests, no generations, no borrowed-buffers-under-adversarial-completion, no admission/backpressure as first-class concepts, no shutdown/quiescence duties). Importing Zig's "AsyncFile"/Threaded model wholesale would break AC-7/AC-8/AC-14 resource and identity separations. Transferable: guaranteed-fast-region documentation style; local capability negotiation with sticky fallback; caller-owned buffering authority; refusing to build a planner into interfaces.

---

## RUNTIME LITERATURE SYNTHESIS (lessons mapped, not summarized)

Main set + extras: IX (NSDI'14), Arachne (OSDI'18), Shenango (NSDI'19), Shinjuku (SOSP'19), Caladan (NSDI'20), ghOSt (EuroSys'23), Demikernel (SOSP'21); extras: Didona et al. storage-API study (SYSTOR'22), FlexSC (OSDI'10), McClure et al. microsecond scheduling policies (NSDI'22), Capriccio (SOSP'03). (DR-04)

| System | Problem regime | Explicit state/resources it exposes | Runtime policy used | Decision frequency | Lesson for Sluice's named question |
|---|---|---|---|---|---|
| IX | high-rate packet I/O drowning in per-call cost | App protected dataplane, batched queues | Device-virtualized API; batching both directions | per batch | "Amortize crossing the kernel/API boundary, not each byte" → supports RE-2 batching axes over per-op tuning |
| Arachne | thread management overhead | Core rental ledger; user-level threads | Core arbiter grants cores; threads migrate | coarse (core rental horizon), sub-µs thread create | "Resource QUANTITY decisions live out-of-band at coarse horizons" → maps to #225 R4: worker/depth ≠ per-request knobs |
| Shenango | µs-tail interference | Per-core busy sensors (kit) + leases | Central allocator reallocates cores **every 5 µs** (primary-source confirmed) | 5 µs interval | "Tiny-interval control demands cheap dedicated sensors" → AC-1a's accessors were never sensors; validates keeping policy OUT of Core absent hardware-grade sensing |
| Shinjuku | tail latency under load imbalance | Central queue of requests | Preempt-and-bail-out: shed stragglers reactively | per blocking event | "Reactive bail-out beats continuous adaptation" → overload shedding via capacity rejection is an adequate fixed policy class for Sluice E4 |
| Caladan | scheduler attacks at µs scale | Fully explicit core assignments | Hardware-accelerated preemption, exclusive allocation | epoch-scale w/ µs reaction | "Adaptation costs must be spent at amortizable epochs only" → feed ADAPTATION table horizons (job/file/epoch ≥) |
| ghOSt | kernel scheduling flexibility | Events exported to userspace agents; policy as program | Userspace agents advised kernel decisions | per event | "Policy/mechanism split with audited agents" → analogous to Core(mechanism)+optional-policy-capability; also warns per-event agent storms are a cost center |
| Demikernel | portable kernel-bypass libOS | POSIX-shaped semantics over DPDK/SPDK/io_uring | None central; engines self-optimize | n/a | "Name a common semantic minimum, expose engine capabilities separately" → precisely AC-8+R5; validates backend-neutral descriptors + capability tier |
| SYSTOR'22 study (extra) | storage API systematic comparison | ring configs incl. polled/interrupt, SQPOLL variants, depths | feature configurations per cell | static per config | "API wins come largely from CONFIGURED FEATURES, not API presence" → R5 priority + RE-4 must sweep feature configurations, else results meaningless |
| FlexSC (extra) | syscall overhead on multicore | Exception-less batched syscalls; dedicated polling cores | Fixed partitioning, batch flushing | per batch wave | "Batch crossings; dedicate pollers optionally" → candidate future backend topology knob (surveyed, not proposed for now) |
| NSDI'22 McClure et al. (extra) | policies for µs tasks | Queue-depth telemetry + intervals | Compared optimal-ish controllers vs static | multi-µs intervals | "**Static allocation often beats reallocation for small tasks**" → direct external support for verdict B/Q17B and for fixed-defaults bias |
| Capriccio (extra) | event-vs-thread scaling debate | Resource-aware user threading | Linear-probe resource monitor guiding adaptation | per spawn | "Adaptive stacking helped because spawns are rare" — confirms horizon principle; per-op adaptation unsupported |

Fixed-beats-adaptive instances observed: NSDI'22 headline finding; Shinjuku's scheduled bail-out simplicity; Demikernel/Capriccio coarse-horizon placements. No reviewed system demonstrates profitable per-operation adaptive policy. That is the literature verdict relevant to the amendment.

---

## SIMPLE-TASK TAX MAP

Workloads audited against as-built call chains (file refs in facts appendix of record): one positional read; one positional write; sequential read; sequential write; simple copy; hash-one-file; scan-one-file. Two tax families separated (DR-02).

### Blocking-core paths (`sluice_core`)

Layers actually traversed per `FileReader::read_at`: API guard (stored-open-error, empty-span) → `checked_posix_offset` conversion validation → `retry_on_eintr(::pread)` → optional SyscallStats tally. Writer analog symmetric. Sequential `Reader::read_some` = interface dispatch → same wrapper. `copy_all`: strategy select (Auto==BufferedFirst) → BufferedReadable drain fast path OR loop read_some/write_all with CopyLimit enforcement → CopyDecision filled.

Classification:

| Layer | Class | Note |
|---|---|---|
| fd validity/offset-conversion checks | FUNDAMENTAL COST | Required by §9.1 descriptor-validation contract |
| EINTR retry wrapper | FUNDAMENTAL COST | Retry-authority consolidation |
| the syscall itself | FUNDAMENTAL COST (outside library) | Baseline for everything |
| RAII handle bookkeeping | FUNDAMENTAL (trivial) | One allocation per handle via context factory |
| optional measurement wrappers | AMORTIZABLE/OPT-IN | Zero when pointer omitted |
| BufferedReadable fast-path | (negative cost — specialization already present) | Copy benefits today |

Verdict: **no avoidable-current-implementation-cost stratum exists in the blocking simple paths beyond rounding error**. Items often suspected (validation, retry) are contract requirements, not accidents. This kills the popular version of "simple ops pay unnecessary machinery": on the sync stack they don't.

### Async-plane paths (`sluice_async` + ThreadPoolBackend)

Per depth-1 op (subtly different across app contexts; here ApplicationRuntime shape): `access_mtx_` forward → descriptor validation-before-commit → 5-stage `submit_transaction` with repeated leaf-lock acquisitions → binding CAS publish outstanding → enqueue (arena transition + ring push + notify under `work_mtx_`) → condvar wake pair → pop/mark_running coordinated transfer → syscall on worker → `record_terminal` ready-ring push + ready-progress signal → `poll/wait_one` epoch snapshot → `reap` (binding validate, pin ack, waiter extract-once, borrow end, accounting decrement, release-store publish) → caller `result()/reset()` generation bump.

| Layer | Class | Basis |
|---|---|---|
| Validation, reservation O(1), binding CAS, terminal-winner arbitration, generation bump, single publication | FUNDAMENTAL COST (async membership fee) | These ARE the promised explicit-I/O semantics (AC-3/5/13/14); measurable together ≈ 1.3–2 µs/op at 4 KiB d1 (E1: L2−L1) |
| dispatch-ring push/pop + condvar wake pair + notify churn per op at shallow depth | AVOIDABLE-CURRENT (topology-dependent) | Property of threaded-medium topology; evented kernels and poll-side completion regimes eliminate/merge; class marked via flamegraph equivalence (L1 pool pays the same switch count ⇒ mirrors medium, not machinery) |
| worker handoff context switches | MEDIUM TAX (outside explicit semantics entirely) | L1−L0 ≈ 33–35 µs/op d1 WSL2 tmpfs; ≈4.1 switches/op both ladders; bpftrace ≈1.08M sched_switch/262k ops |
| `access_mtx_` context serialization | AMORTIZABLE | Sharding/removal plausible but unmeasured; UNKNOWN contribution |
| ready-epoch/park plumbing | AMORTIZABLE at depth (merged wakeups under load); UNKNOWN at d1 | Not internally attributed (DR-05 limitation) |
| Runtime construction (RuntimeBuilder heap build, driver thread) | AMORTIZABLE | Per-context not per-op |
| active_workers_/high-water maintenance | FUNDAMENTAL (bounded-resource accounting mandated by AC-7) | Pre-existing; AC-1a only added readers |

### The two-tax theorem for this repository

1. Control-plane tax (explicit machinery): real but **small** on the only measured profile (~µs/op scale, ≤~15% ratio band across cells, sometimes NEGATIVE thanks to reap batching).
2. Medium tax (how work reaches the kernel through chosen topology): **dominant** (~95% at small shapes on host), and shared with ANY competent threaded pool (L1's design duty).

Consequences for the amendment rule "pay tax only where it buys something":
- Re-aim it primarily at **topology choice**, secondarily at **capability utilization**, barely at machinery slimming. An architecture campaign that deletes request machinery to please simple workloads optimizes the 5%.
- "What buys it" is answered per-cell by an envelope map (below), not by intuition.

---

## WORKLOAD REGIME TAXONOMY

Ten regimes (DR-08 caveat applied throughout: detection is unreliable; RX-1 proved one natural proxy false; queue-side occupancy is the shape-stable observable; regime inference is a research topic, never a Core input yet).

| Regime | Should be cheap | Should be explicit | Should be bounded | Optimization that helps | Runtime policy that HURTS | Distinguishing metrics |
|---|---|---|---|---|---|---|
| tiny/coordination-dominated | syscall wrapper path; minimal indirection | semantic ops; capacities | pipeline depth; memory | direct/topology without hop; batching on caller side | per-op machinery additions; instrumentation | CPU/op >> device time; instr/op ratio |
| CPU-service-dominated | throughput of transforms | limits | in-flight bytes | larger req sizes; overlap pipelining | extra wakes; small chunks | high util%, low iowait |
| I/O-service-dominated | async plumbing transparency | durability; budgets | queue depth ≤ ring depth | deep pipelines; vectored/fixed capabilities | spurious sync_data; per-op waits | iostat/service time dominance |
| queueing/saturation-dominated | rejection fast-path | capacities; backpressure vocab | ALL | admission control; stable degradation | autotuners chasing saturated optimum | occupancy≈cap; p99 blowup; rejection counts |
| mixed CPU↔I/O pipeline | stage handoffs | budgets per stage | stage queues | overlap; per-stage workers | global one-size depth | wall vs sum(stage times) gap |
| memory-bandwidth-bound | copies minimization | buffers ownership | buffer budget | zero-copy/vectored | double-buffering defaults | perf counters (cache/bw) |
| scheduler/wake-dominated | wake-suppression | wake obligations documented | worker count | fewer wake pairs; poll mode opt-in | more entities to wake | ctx-switch/op; futex counts |
| heavy-tail service-time | headroom reserves | timeouts; cancel strength | in-flight caps | hedging/explicit deadline | naive FIFO retry storm | p99/p50 spread |
| multi-tenant/noisy-neighbor | isolation | guarantees vs best-effort | hard caps | pinned placement (out of scope for Core) | elasticity guessing neighbor noise | PSI/steal counters |
| device-queue-dominated | submission density | SQ/CQ depth; features | ring depth | registered bufs; SQPOLL; multishot | thread hop per op | kernel queue latency; io_uring ktests |

Honest unsolved core: regimes coexist *within* one process over time (phase drift). The taxonomy describes cells of the envelope, not runtime states of a program. Nothing in Sluice may detect-and-switch until RE-2 finds stable structure AND DR constraints pass.

---

## FAST-PATH REVIEW (candidate diagram from prompt §7)

Decision record DR-06 governs this verdict.

Findings:
1. Both branches of the candidate diagram already exist semantically. Cheap/fast = blocking core (near-zero machinery; verified chain). Mediated = async runtime. So "can both share one semantic contract?" is already answered YES de facto — same IoError/Result/result-type vocabulary, same durability/cancellation meanings at their respective surfaces.
2. Authorities that MUST remain common on every path: RequestArena identity/generation, single Completion publication, cancel-arbitration precedence, transactional-failure atomicity, resource bounds at every admission point, quiescent destruction rules.
3. Machinery that MAY legitimately be skipped by a cheap topology: persistent-worker handoff, dispatch ring, runtime coordination; on the mediation side: fiber/scheduler involvement (already optional), wake pairs (absorbed by poll-driven loops).
4. Bypass designs DO risk two correctness authorities whenever "skip" means abandoning one of the common authorities — e.g., publishing Completions outside reap, or accepting entries outside arena accounting. Those are semantic forks and are rejected in advance.
5. In-tree existence proof for a non-threaded degenerate topology: `SyncBackend` completes-at-next-poll (submit records terminal without workers/rings; poll publishes) — full identity/accounting preserved with no queue and no wake-crossing. This is the legal template for any richer fast path: vary TOPOLOGY, never AUTHORITY.

**VERDICT (exactly one): POSSIBLE BUT HIGH AUTHORITY-RISK.**

Conditions attached to any future authorization:
- Expressed exclusively as backend execution topology per AC-8 (backend-internal; not a public second API);
- Complete Gate 0–4 compliance pack: state machine extension for direct-mode transitions, lock-order table (likely trivially empty), resource equations, wake closure even when "wake" is null;
- Formal-model addition covering the new terminal/reap interleavings before merge;
- Conformance suite runs across ALL topologies including the new one (existing suite already multi-backend);
- Prerequisite evidence: RE-2 shows a cell family where ThreadPool topology loses to either evented topology or to non-mediated execution AND the loss is not recoverable by existing knobs;
- Retro-risk register: cancel-before-enqueued interactions, waiter delivery, interrupt-close races — each needs a death test on the new path before conformance signoff.

Explicitly NOT required: a bypass skirting RequestArena (that is the semantic fork this verdict refuses).

---

## CAPABILITY VS PLANNER

Comparison (DR-09 criteria):

| Dimension | MODEL A capability-local specialization | MODEL B global execution planner |
|---|---|---|
| Complexity | O(#capabilities × #objects), incremental; falls back cleanly | Whole-system controller; config state machine; regression surface global |
| Correctness authority | unchanged (single authorities untouched; new mechs slide under existing reap/validation umbrellas) | New decision authority competes with caller intent and resource owners; observers/watchers multiply |
| Hidden-transformation risk | contained by capability contract + CopyDecision-style surfacing | High: planner optimizes globally, incentives push toward loosening per-op contracts |
| Decision cost | paid once per object/backend negotiation | Paid per horizon chosen; per-op variants ruled out by literature (Shenango needs 5 µs sensors etc.) |
| Testability | unit-level per capability; matrix-tractable | Requires environment sweeps to have confidence; nondeterminism ingress |
| Environment sensitivity | low (capability supported or not) | high (thresholds shift per host/kernel/device; WSL2 vs NVMe flips optima) |
| Debugging | localized ("this cap off here") | "Why did the planner choose X" archaeology |
| API stability | additive | Real danger: policy surface frozen badly |
| AI-friendliness (repo value axis) | small closed vocabulary; good | opaque heuristics; poor |

External anchor: SYSTOR'22 — performance differences are driven by feature *configurations*, not API choice alone; and feature-config choice is effectively static per deployment. Plus NSDI'22 static-outperforms-dynamic for small tasks. These jointly argue Model A first.

Recommended hybrid (also the practical reading of #225 Phase 5 + amendment §8-R5): implement Model A now under RE-3 (real splice/sendfile for blocking copy — use case exists, names reserved since copy_strategy.cpp; then async vectored/registered only with RE-4 availability). Keep Model B constituents (worker count, depth policy) as EXPLICIT KNOBS with measured defaults forever until regret is measured, not presumed.

Where a planner could eventually earn existence: fleet/library-shared process pools spanning thousands of contexts (outside current Sluice surface); regime-specialized *defaults* selection at RUNTIME-BUILDER time (compile/setup-time, not per-run-time) — allowed space to watch, zero immediate action.

---

## ADAPTATION COST / HORIZON

Cost model components accepted as given: sense + decide + act + transient disturbance must be repaid over a decision horizon. Table (horizon ranges qualitative; per-op excluded by evidence from both RX-1-adjacent failures and literature):

| decision variable | plausible horizon | likely sensing/act cost | plausible benefit | adapt NOW? |
|---|---|---|---|---|
| backend | install/build/job/file | near-zero if user-chosen; moderate if probed | large when crossing thread-vs-evented or host regimes | NO — explicit choice + documented guidance (this IS product policy) |
| worker count | job/epoch/seconds | external telemetry suffices | moderate at inflection loads | NO |
| pipeline depth | job/file/batch | trivial (caller knows stages) | real for pipelines | NO — composed-helper knob with measured default |
| queue/ring depth | configure/load phase | occupancy already readable (AC-1a) | real under shifting load | NO — fixed defaults sized by RE-2 |
| batching size | per composed batch | trivial inside helper | real (amortizes crossings) | RULE-BASED OK inside helpers (fixed formula, surfaced) |
| request granularity | file/stream segment | trivial | large in IO-dominated regimes | helper policy + E1c sweep informs default |
| buffer strategy | per stream/context | low | situational (bandwidth vs footprint) | NO |
| CPU placement | process/epoch | high (needs platform sensing) | real but out-of-library scope | NEVER in Core (out of responsibility) |

Net: nothing adapts dynamically today. The only sanctioned "adaptation" class is **rule-based selection inside composed operations** (strategy pick at copy start, chunk-size formulas) — already architecturally positioned, needing measured defaults from RE-2 rather than controllers.

---

## PERFORMANCE ENVELOPE (product-level artifact)

Definition only — thresholds/preregistration belong to RE-2 (not invented here):

Zones (which cells belong where is the deliverable):
- Z1 RAW-WINS: accept loss knowingly; document; facade routes simple single ops to sync stack anyway (positioning honesty).
- Z2 PARITY REQUIRED: no material loss vs competent conventional baseline (e.g., threaded pools; cp(1)-class for copy semantics with atomic-output allowance) — gating zone for general-purpose ambition.
- Z3 CROSSOVER: Sluice catches up as depth/concurrency/utilization grow; envelope must mark the frontier.
- Z4 EXPLICIT-DOMINATES DESPITE RAW THROUGHPUT LOSS: bounded RSS under load, stable rejection (`would_block` before acceptance), defined drain/shutdown, verbatim-preserving cancel, worst-case p99 containment vs unbounded competitor queues.

Axes (from prompt candidate list, pruned to independent variables): op size {4K..4M}; depth {1..128}; workers {1..n_cores capped}; CPU-per-request classes {syscall-only, light hash, compress}; device {tmpfs/page-cache-hot, NVMe, cold-cache spinny-if-available}; seq/random; r/w mix; pipeline shapes {stage counts, retention}; service-variance {uniform vs injected tails}; offered load ramp saturating; backend {Blocking, Pool, Pool+E?, real-liburing w/ {feature-config} sweep}. Metrics: throughput/IOPS, p50/95/99, CPU/op, instr/op, ctxsw/op, syscalls/op, bytes-in-flight memory, RSS vs load, rejection stability, queue growth, tail-under-overload. Scalarization explicitly forbidden (multi-objective map output; runner JSON per house schema-2 + validator).

Expected (pre-registered-style predictions, falsifiable): Z1 = tiny-shape depth-1 raw-pread; Z2 reachable by pool at medium/deep shapes for bandwidth-bound reads (E1 negative cells hint); Z3 around increasing depth/CPU-rich work; Z4 strong at saturation + hostile inputs where bounded rejection wins.

Also required by product side: repetition across ≥2 machine classes to break single-host anchoring (limits recorded in E1 doc are explicit about WSL2 wake inflation).

---

## SIX-DOMAIN IMPACT

| Domain | Disposition | What changes under this review |
|---|---|---|
| R1 Request | KEEP UNCHANGED | Strongest authority (#234); also cheapest per-op layer (~µs). No simplification demanded by "simple-task tax" evidence |
| R2 Wait | KEEP UNCHANGED (priority intact) | Correctness debt orthogonal to product-tax question; AC-2 proceeds. Q17E=B maintains this while allowing parallel RE batch |
| R3 Transfer | REINTERPRET + NARROW FOCUS then EXPAND | From loop-dedup framing toward being THE legal home of transformations/amortization (chunking freedom, batching, capability-gated fast transfer). Existing helpers instantiate the pattern; Phase-4 freeze behavior matrix prerequisite stays |
| R4 Resource | EXPAND | Bounds become the LEGAL POLICY SPACE (input to envelope), not merely audit rows; E1c sweep gets absorbed INTO RE-2 envelope spec to prevent duplication |
| R5 Capability | MOVE PARTLY EARLIER | Behind real use cases only: (1) blocking-core splice/sendfile copy (names reserved, concrete app evidence); (2) async vectored/registered after RE-4 availability. No speculative enum farms |
| R6 Observability | KEEP STOPPED | RX-1 verdict respected; envelope research consumes external telemetry + AC-1a engineering facts only; AC-1a retired to plain introspection role it already occupies |

Cross-domain integrity: every disposition above preserves the 8 cross-domain invariants; none reopens a falsified branch; promotion-rule question #6 gains the new envelope/regime citation requirement.

---

## STRONGEST CASE AGAINST SLUICE (steelman + adjudication)

Steelman assembled from evidence, not strawman:

1. Everyday workloads need nothing exotic. POSIX primitives already express every audited simple task in one syscall apiece; cp/hash/grep-class tools ship optimized decade-tuned paths. For these users, Sluice's entire value proposition is pre-existing in libc.
2. On measured numbers, opting IN to Sluice's async plane costs ~35 µs/op of coordination at small shapes on the reference host — roughly *the entire lifetime of many operations* — of which the unique-to-Sluice part is only ~5%: you pay full price for entering a medium whose dominant cost Sluice neither created nor removes, plus complexity rents beyond any conventional pool.
3. Performance-generic claims are unsupported where they'd matter: attribution superiority for building autotuned execution — falsified in scope (RX-1); abstracted-data-path superiority over liburing directly — unproven (L3/L4 deferred); io_uring feature-set advantages (registered buffers, multishot, provided buffers) — ABSENT from the codebase while competitors and raw users adopt them freely.
4. Knobs externalize implementation complexity onto applications: every explicitly exposed resource distinction (7-way!) is a decision users must make correctly; wrongly configured bounds become silent self-DoS via rejection or deadlock-by-backpressure — sharper failure modes than conventional libraries' sloppy-but-resilient behavior.
5. Deterministic testing/fault injection value accrues mainly to the LIBRARY'S development, not end products; applications that want injectable I/O already mock File APIs cheaply.
6. If the honest scorecard reads: no average-product throughput win proven, distinctive correctness properties only demonstrably valued by niche consumers (WAL/durable writers, reload-safe pipelines, adversary-heavy environments), — then the project is a niche explicit runtime *by its own evidence*, and pretending otherwise burns research budget on workloads it structurally cannot win.

Points conceded outright: #1 (fully true, drives the positioning recommendation); #3's attribution clause (RX-1 verdict stands, respected throughout); the SKETCH of #2 for depth-1 shapes on slow-host mediums (WSL2-specific inflation acknowledged); #6's conditional (if positioning stays "general-purpose", current evidence indeed fails to justify).

Points refuted by evidence or contract logic:
- #2 generalizes only if the medium staysThreadPool at depth 1: same table shows negative absolute tax at several deeper cells (reap batching beats naive pools), and medium tax is a *topology* choice with at least three poles in-tree or available (evented, uring). Not a property of explicitness.
- #4 ignores that bounded systems fail-fast visibly BY DESIGN (typed `would_block`, high-water metrics, drain semantics) — the failure mode differs but is not worse than overflow-tail-collapse at p99 in ad hoc stacks; contract tests exist making misconfiguration diagnosable rather than emergent.
- #5 dismisses E6/production fault categories prematurely: SHORT-READ under signals, EINTR storms, ENOSPC mid-write, reordered completion — reproducible only through the explicit boundary in deterministic harnesses; that is a genuine consumer benefit, though its market size is unproven (logged as open hypothesis).

Adjudication: the negative argument **wins against "general-purpose by default" aspirations on current evidence**, and **does not defeat** the niche-plus-selected-parity positioning. Sluice continues defensibly iff its roadmap claims match evidence zones (see ARCHITECTURE OPTIONS + RESEARCH ROADMAP).

---

## STRONGEST CASE FOR SLUICE (only demonstrated or falsifiable items)

| Claimed advantage | Status | Evidence line |
|---|---|---|
| Memory bounded by configuration not input size | PROVEN (app track) | 4 apps: max RSS 4.5–8.2 MB processing 1 GiB inputs via 64 KiB–1 MiB configured buffers (recorded in #221) |
| Deterministic operation-boundary state machines survive races correctly (generations, terminal winner, publication authority) | PARTIALLY PROVEN | Contract/regression suites green across backends; TLA+/GenMC models aligned to code paths; end-to-end E6 replay consumer value not yet exercised |
| Cancellation preserving ordinary results verbatim; layered cancellations disentangled | PARTIALLY PROVEN | Specified + tested at backend conformance level; E5 systematic sweep pending |
| Backend-neutral operation semantics without application rewrites | PARTIALLY PROVEN | 4 applications ran across sync/async/runtime contexts; ThreadPool↔Sync↔Fake swaps exercised in suites; full E2 loop on real hosts pending |
| Overload behavior explicit and stable (reject-before-accept, bounded queues) | PARTIALLY PROVEN | Capacity-pressure returns `would_block` per contract; `overload_backpressure_bench` exists; E4 formal comparison absent |
| Fault/injection/replay determinism | PARTIALLY PROVEN (machinery), HYPOTHESIS (consumer value) | FakeBackend + fault.hpp + WAL writer semantics; E6 not executed |
| Attribution superpowers for performance work | FALSIFIED IN TESTED SCOPE | RX-1: Δ(E−C)=−1.35pp, C=99.55%; STOP EXPANSION honored |
| Average-product throughput competitiveness | PARTIALLY PROVEN, weak | copy ~1.1–1.3× cp (competitive, atomic-output semantics); hash/grep gaps attributed outside runtime; depth-dependent cells vary |
| AI-programmability advantage | HYPOTHESIS (never tested) | E8 unexecuted |

The honest composite: Sluice's demonstrated advantages cluster in **correctness-under-adversary, boundedness, and operational predictability** — real, tested properties — while performance-attribution and AI stories remain unset. Strategy implication: sell and strengthen the proven cluster; measure, don't assert, the rest.

---

## ARCHITECTURE OPTIONS

Five coherent directions + recommended composite (comparative, non-strawmanned):

**A. CURRENT EXPLICIT RUNTIME (status quo, optimize implementation)**
Product: expert surface only. Perf: matches today's evidence. Authority complexity: high but stable. Implementation: focused. Research burden: low ongoing. Compatibility: full. Key risk: product stalls in niche without saying so. Falsifier: RE-2 Z2 failure ⇒ legitimizes pivot calls.

**B. SEMANTIC CORE + FAST PATHS (one semantic contract, multiple legal topologies incl. poolless cheap async path)** ← core-respected twist on "add fast lane"
Product: general-purpose-friendly. Perf: expected Z2 coverage widened IF RE-2 shows losses. Authority: adds one legal topology inside existing authorities — HIGH risk demands Gate/formal/test triples. Implementation: moderate (SyncBackend/template exists). Research: RE-2-first mandatory. Compatibility: additive. Key risk: authority erosion via exception creep. Falsifier: RE-2 shows ThreadPool adequate in every Z2 cell ⇒ option unnecessary.

**C. CAPABILITY-FIRST EXECUTION (R5 promoted; thin runtime layered atop specialized local mechanisms)**
Product: performance tooling for transfer-heavy domains. Perf: biggest lift potential (feature-driven wins per SYSTOR'22). Authority: cleanest gains (capabilities slot beneath existing contracts). Implementation: staged (splice/sendfile → vectored → fixed buffers...). Research: RE-3 + RE-4 feeding. Compatibility: additive. Key risk: capability proliferation without consumer demand (anti-goal #speculative-surface). Falsifier: RE-3 shows no meaningful gain over naive copy on real devices ⇒ stops path.

**D. POLICY-READY MECHANISM (expose explicit state/caps; keep adaptive policy OUTSIDE Core permanently)**
Product: platform ethos (ghOSt-analogous roles). Perf: neutral guarantee + outsider innovation possible. Authority: cleanest of all expansions. Implementation: near-zero beyond current. Research: never-ending observation duty BUT rx-1-bounded. Compatibility: perfect. Key risk: postpones answering the product question rather than answering it. Falsifier: not applicable (it's the meta-option).
This is effectively TODAY'S architecture + the corrective narrowing of the substrate amendment.

**E. NARROW NICHE RUNTIME (drop parity ambitions; marketing: bounded async pipelines / adversarial correctness / replayability)**
Product: crystal clear; every former failure mode is repackaged as a guaranteed feature envelope. Throughput expectations released except within pipeline shapes. Authority/implementation burdens drop sharply. Research: pipeline-domain focus. Compatibility: fine. Key risk: ecological shrinking (contributors, feedback diversity), abandonment drift.
Falsifier/hinge: human chooses positioning; evidence neither forces nor forbids.

**RECOMMENDED COMPOSITE (sequenced, evidence-gated):**
Phase α: human selects positioning (decides weight of Z2 gate).
Phase β (parallel, already-authorized-compatible): AC-2/R2 + RE-1/RE-3 (small).
Phase γ: RE-2 envelope → decide among {stay-A / fold-in-B-cheap-topology / accelerate-C}.
Phase δ: whatever survives evidence moves to gated campaign; option E remains fallback shell the whole time.

---

## REQUIRED VERDICTS (with justification)

**A. Is "explicitness as runtime policy substrate" valid?**
**YES, BUT ONLY AS OPTIONAL POLICY CAPABILITY.**
Valid parts: the substrate framing correctly identifies that explicit info makes *future optional* policy auditable; progressive disclosure instincts align with Zig precedent; regime list correct. Invalid-as-phrased parts: implies runtime-consumed policy economics before evidence; conflicts with RX-1's fresh lesson (even passive reading didn't help), and with external findings (static beats dynamic, microsecond regimes). Any wording implying substrate-fed runtime decisions enters Core only via RE-2-cleared, gated campaigns. Until then "substrate" = "mechanism-ready information".

**B. Should adaptive policy enter Core now?**
**NO — DESIGN MECHANISM FIRST.**
Every reviewed controller needed substantial sensor infrastructure and epochs; Sluice has neither need demonstrated nor avoidance-regret measured; even exploratory RX-1 gains (+0.45pp ceiling with wrong-direction variance) demonstrate measurement fragility at finer grain, not promise. Mechanisms (knobs, caps, capability seams, composed-helper rules) precede controllers by design order.

**C. Should simple workloads have a cheaper path?**
**PROBABLY — NEED PERFORMANCE EVIDENCE.**
Cheaper-than-present is an established FACT for sync stack (they already have it); async-plane demand is undemonstrated: machinery-share ≈5% suggests limited upside vs medium-adjustment levers; RE-2 (possibly paired RE-1b decomposition on real devices) decides whether B-directly or via capability amortization. If async-market Z2 cells lose due purely to topology, we return with a design B proposal; nothing authorizes that workstream today.

**D. Should R5 capability specialization move earlier?**
**PARTLY.**
Yes-component: blocking-core real splice/sendfile implementation in Copy (reserved strategy names + concrete app: sluice-copy on real storage rather than tmpfs; sendfile(2)/splice both have documented failure semantics requiring the exact fallback contracts Zig normalized); async vectored ops once RE-4 supplies grounds. Partly-component: registered buffers/multishot only with real-device evidence and user-facing story; framework-level capability enums remain banned (repo anti-goal).

**E. Does AC-2 / R2 Wait remain the immediate architecture campaign?**
**YES, BUT PARALLEL PERFORMANCE-ENVELOPE EVIDENCE IS REQUIRED.**
Wait debt is real, scoped, evidence-listed (#234 specimens), unaffected by product questions; delaying it for performance research couples unrelated risks. Parallel small preregistered RE-1/RE-3 (and prep RE-2 harness) satisfies the human decision window without Core churn. RX-1's stop-expansion is untouched — envelope work measures BEHAVIOR, not per-request internals.

---

## RESEARCH ROADMAP (minimum sequence)

| ID | Question | Hypothesis | Cheapest falsification | Baseline | Workload | Metrics | Stop condition | Architecture consequence |
|---|---|---|---|---|---|---|---|---|
| RE-1 | Simple-path tax breadth: does the 95/5 medium/control-plane split hold beyond WSL2-tmpfs-cache-hot? | Yes within factor ~2 on other hosts; fails only at extreme interaction-cost platforms | Extend existing E1 ladder + flamegraph differential to ≥1 native-Linux host + cold-cache HDD/NVMe cell; reuse runners/validators as-is | E1 artifacts schema-2 | Read/write 4K&1M × d{1,32}; cold + warm | same E1 metric set | split holds ⇒ done; huge divergence ⇒ investigate medium composition before anything else | Updates tax-map percentages; recalibrates Q17C urgency |
| RE-3 | Capability value: does LOCAL specialization capture most practical gains without any runtime adaptation? | yes: splice/sendfile ≈ parity-or-better vs tuned cp on native storage; vectored irrelevant for single streams | Implement ONLY blocking-core splice/sendfile behind existing CopyStrategy scaffolding (deferred names already reserved); measure A/B with copy_strategy_bench extended | cp(1), tuned read+write loop | large-file copy on real SSD/HDD; mixed small/large | wall, syscalls/op, ctxsw/op, CPU/op | value <5% consistently ⇒ declare capability route dead for transfer class; ≥2× regressions impossible territory triggers framework rethink | R5 timeline placement; informs option C viability |
| RE-2 | Envelope/crossover map: in which cells does mediated explicit execution repay itself; where does it lose to raw/threaded/evented? | monotonic-ish structure: small-shape losses narrow with depth; explicit-zones dominate at saturation | Preregistered matrix on tooling running today (pool + evented) BEFORE new mechanisms; extend axes incrementally; real uring folded in via RE-4 | RE-1 winners + L1/L2 ladder | full grid listed in PERFORMANCE ENVELOPE section | that section's metric set | envelope stable across repeat sessions & 2 hosts ⇒ publish map; unstable ⇒ keep knobs, kill planner dreams harder | Decides between options A/B/C activation; sets E1c default values |
| RE-4 | Real-io_uring envelope under valid device regimes | feature-configured rings dominate in device-backed cells; stub-equivalent configs wash out | Only after host acquisition (human decision item); run matrix w/ {basic, SQPOLL, fixed-buf, provided-buf} configurations distinctly labeled; stub evidence never represents real path | raw liburing controls + RE-2 outputs | device-backed cells only | same | library absence ⇒ defer indefinitely | Uring backend feature adoption (via R5); locks L3/L4 rows finally |
| RE-5 (conditional) | Optional policy: does ANY stable avoidable-regret pocket survive RE-2/RE-3? | default: no | only if RE-2 marks specific cross-cell regret pockets persisting across hosts | n/a | targeted pockets only | regret vs oracle | pockets vanish under environment variation ⇒ archived forever; persist ⇒ tiny scoped fixed-rule proposal, NOT a controller | Would reopen §8 Model-B door narrowly |

Sequencing rationale: RE-1 cheap breadth fixes worldview; RE-3 independent+small answers capability feasibility without waiting; RE-2 is the big decision-engine requiring RE-1 sanity + benefiting from RE-3 lessons; RE-4 blocked externally; RE-5 only-on-trigger. Combined implementation burden stays days-scale except RE-2 matrix.

---

## ISSUE UPDATE RECOMMENDATIONS (proposed text only)

### #225 — north star
- **KEEP**: the six-domain structure; north-star sentence; semantic-compression test; promotion rule; non-goals; all R1..R6 success conditions (none invalidated by this review).
- **ADD** (new comment block, proposed): "Performance-envelope acceptance criterion: a performance-motivated domain change cites envelope zones it improves (Z1..Z4), predicted tax paid outside those zones, and the cheaper-path alternative — referencing RE-1..RE-5 evidence. Regime labels remain hypotheses until RE-2 stabilizes them."
- **ADD**: R3/R4 clarifications reflecting REINTERPRET disposition (transfer ops = legal transformation locus; bounds define policy space feeding envelope).
- **REMOVE**: nothing.
- Amendment comment ("runtime-policy substrate", 2026-08-27T04:45:56Z) — **SUPERSEDE WITH CORRECTIVE COMMENT** stating:
  1. RETAINED: §1's "explicit≠manual-driving" clarification; §2's semantic/execution split; §4's pay-where-it-buys rule (corrected target); §5's envelope idea (now defined properly); §6's progressive-disclosure ladder; §7's adaptation gates; §9's promotion question.
  2. CORRECTED: "pay tax only where it buys something" splits into TWO rules — (a) medium/topology tax dominates today (95% at small shapes measured), choose topology deliberately per workload; (b) control-plane tax stays small but MUST still buy *only* the listed semantic services — slimmest-machinery moves justified solely by avoiding accidental growth, not by imagined big savings.
  3. NARROWED: "substrate" retains readiness-value (explainable/testable/replaceable potential) but runs ZERO runtime policy consumption until RE-2/RE-5 evidence lands; term-of-art becomes "mechanism-ready explicit information".
  4. SUPERSEDED claims: any implication that regimes can be detected cheaply today (contradicts RX-1 proxy lesson) or that adaptive shape-selection is a near-term user of the substrate (Q17B answer blocks it).

### #221 — value evaluation
- **ADD research questions** (numbered continuing sequence): Q-E9(re-labeled RE-1): tax-split stability across machine classes/devices; RE-3 capability value; RE-2 envelope composition as the decisive product-evidence piece; positioning-statement deliverable owned by humans consuming the map.
- **REVISE E1 framing**: keep L0/L1/L2 semantics unchanged (working, normative); extend planned scope with cold-cache + second host + (later) L3/L4 real-liburing realizations per RE-4; explicitly record "E1 evidence may continue WITHOUT any attribution-extension (RX-1 constraint)".
- **ADD product-value acceptance criteria**: value evaluation may conclude when (i) envelope map Z1..Z4 populated across ≥2 hosts, (ii) positioning statement signed, (iii) claim-status table (PROVEN/PARTIAL/HYPOTHESIS/FALSIFIED) updated from E5..E8 executions OR explicitly left open with reasons.

### #227 — sequencing
- **KEEP**: AC-2/R2 as CURRENT NEXT STEP (phase 6); all hard adversarial gates incl. #10, #11 verbatim.
- **ADD parallel-track authorization**: RE-1 + RE-3 small experiments (preregistered, runner-tooling-based, no Core modifications) may proceed alongside AC-2 — engineered boundaries: research-only directories, existing bench targets, validator-approved artifacts.
- **ADD pending blockers**: RE-2 matrix waits for RE-1 confirmations; RE-4 awaits hardware/host decision (listed under FINAL QUESTIONS).
- **WHAT REMAINS UNAUTHORIZED** (extend current list): any B-topology implementation pre-RE-2; any planner/controller/autotuner (restated); capability frameworks beyond RE-3's concrete slice; L2/request-key/telemetry/eBPF (RX-1 closures persist); regime-detector subsystem; substrate-consuming runtime policy.

---

## WHAT NOT TO BUILD

- A global execution planner / autotuner / adaptive controller (until a surviving RE-5 pocket proves otherwise — and even then only a fixed rule).
- An "LC0 facade" that grows a second API vocabulary duplicating semantic operations — facades must compose the SAME operations downward.
- Hidden coalescing/splitting/zero-copy redirect inside PRIMITIVES.
- Request-machinery deletion campaigns chasing the 5% control-plane tax (including simplified-Completion public variants or fused-validate-submit bypasses).
- Async equivalence of everything: timers/networking/P2300/actors/coroutine layers in Core (standing bans intact).
- Telemetry infrastructures (L2 timestamps, RequestKey export, eBPF, RX-2/3, classifiers) under any framing.
- Regime-detection runtime based on proxies similar to `active_workers` (documented fragile).
- io_uring feature-maximizing rewrites without real-device RE-4 approval cycles.

---

## FINAL QUESTIONS REQUIRING HUMAN DECISION

1. **Positioning (gates everything):** General-purpose library targeting Z2 parity breadth (then RE-2 Z2 gaps decide rewrites) versus niche-explicit-runtime-with-honest-marketing (option E comfort)? No technical measurement settles this; pick consciously.
2. **Amendment disposition:** Approve issuing the SUPERSEDE corrective comment on #225 as drafted above?
3. **Research authorization:** Greenlight RE-1 + RE-3 immediately (small, tooling-complete, independent of AC-2)? Authorize RE-2 matrix harness construction timing?
4. **Hardware/host acquisition** for RE-4 (native Linux + device variety, or explicit indefinite deferral)?
5. **App-gap priorities:** treat hash/grep large-N gaps (attributed algorithmic) as OUT of library scope permanently, or revisit after envelope clarity?

STOP — review ends here. No code modified; only this document authored untracked; issue submission executes per separate instruction acknowledging the original attachment's mutation ban was superseded by the operator's later directive (DR-10).
