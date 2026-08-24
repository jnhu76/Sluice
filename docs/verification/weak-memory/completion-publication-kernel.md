# Completion publication kernel — bounded weak-memory evidence (#197)

**Claim**: `MEMORY-MODEL-CHECKED (BOUNDED KERNEL)` — the Completion
publication/reset protocol's exact production atomic ordering, extracted into
two small kernels, is checked exhaustively within stated bounds under a C++
weak-memory model (RC11 and RA+RLX), including five broken-order negative
controls that the checker rejects. This is **not** whole-program verification
of Sluice, and no liveness property is claimed.

| Field | Value |
|-------|-------|
| Issue | #197 (child of #163, V3) |
| Kernel provenance revision | `d98d70dd57c85b5e4c59d92b384ffa95b6027e4a` (`master`) |
| Production source | `include/sluice/async/completion.hpp`, entry `include/sluice/async/async_io_context.hpp:345-388` |
| Kernels | [`spec/weakmem/completion-publication/kernel_publication.cpp`](../../spec/weakmem/completion-publication/kernel_publication.cpp) (K1), [`kernel_reset_reuse.cpp`](../../spec/weakmem/completion-publication/kernel_reset_reuse.cpp) (K2) |
| Runner | `scripts/weakmem/verify-completion-weak-memory.sh` (+ `_gen_mutants.py`) |
| Machine-readable artifact | [`docs/results/weak-memory/completion-publication.json`](../../results/weak-memory/completion-publication.json) (runner-generated, never hand-created) |
| Checker | GenMC v0.17.0 (commit `29b03a6`), built with LLVM 18.1.8 |
| CI wiring | **None** — separately-run evidence layer per #197 non-goals |

## Why this layer exists

TLA+ models decide interleaving-shaped protocol questions; TSan samples races
on the schedules the test suite happens to produce. Neither decides whether
the *exact* `memory_order` annotations on the Completion publication path are
sufficient under a weak memory model. This pilot extracts the real ordering
(not an idealized version) into checker inputs and decides it exhaustively
within bounds.

## Checker selection (value-demonstrated, not fashion)

GenMC is a stateless model checker that explores **all** executions of a
C/C++ program under a configurable memory model, with built-in data-race
detection. Its default **RC11** model is the model class matching C++
`memory_order` semantics; `--ra` (RA+RLX), `--tso`, and `--sc` are also run.
A pre-pilot smoke test on this host (WSL2) demonstrated the checker constrains
the property class: a correct message-passing kernel in the exact production
CAS shape PASSED under RC11, and a release→relaxed mutant FAILED. Build recipe
(user-local, no system pollution):

```sh
git clone --depth 1 https://github.com/MPI-SWS/genmc.git ~/tools/genmc-src
cd ~/tools/genmc-src && git checkout 29b03a6   # v0.17.0
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18 -B build -S .
cmake --build build -j
# binary: ~/tools/genmc-src/build/bin/genmc
```

One mechanical spelling note: GenMC's compilation path does not accept the
scoped enumerator spelling `std::memory_order::release`; the kernels use
`std::memory_order_release` — the same enumerator constant of the same
`std::memory_order` enumeration. No order is idealized or strengthened.

## Production protocol under check (provenance)

Publication side (backend/reap thread) — `completion.hpp` at the provenance
revision:

| Step | Site | Access | Order |
|------|------|--------|-------|
| A1 | `:321-327` `begin_binding_for_backend` | CAS `idle → binding` | `acq_rel` / `acquire` |
| A2 | `:367-371` `install_binding_for_backend` | plain writes `release_arena_`, `bound_slot_` | non-atomic |
| A3 | `:328-346` `commit_binding_to_outstanding` (submit-success LP) | CAS `binding → outstanding` | `acq_rel` / `acquire` |
| A4 | `:404-411` `publish_from_reap` head | CAS `outstanding → publishing` | `acq_rel` / `acquire` |
| A5 | `:412` (`:453-456`) `storage_.set` | plain writes `has_value`/`has_error`/`value`/`error` | non-atomic |
| A6 | `:413` + `:109-112` | `++counter` (process-wide atomic) then plain member write | seq_cst RMW / non-atomic |
| A7 | `:414` | `store(ready)` | **release** |

Observer side: `:200` `ready()` / `:217-222` `result()` — acquire load, plain
storage reads only when `ready` was observed.

Reset side (caller): `:235` acquire gate; `:249-252` CAS `ready → resetting`
(`acq_rel`); `:265-270` plain clears; `:273` `store(idle)` (**release**).

## Kernels and properties

### K1 — publication kernel (2 threads, 1 round, no loops)

Backend thread performs A1→A7; observer thread performs one acquire load and
branches on what it observes. Concrete properties (observation-conditional):

- **P1** — if an acquire load observes `ready`, the plain storage writes (A5)
  and the reap-seq stamp (A6) are visible: storage is exactly the published
  result (value + both flags), `reap_seq == 1`, and the binding payload (A2)
  is visible through the sequenced A3/A7 release chain.
- **P2** — if an acquire load observes `outstanding`, the binding payload is
  fully installed (production invariant I2).

### K2 — reset/reuse kernel (3 threads, 2 rounds, bounded waits)

Adds the caller reset chain (observe → P3a reads → C2 CAS → C3 clears →
C4 idle release), a second publication round through a fresh Phase-B claim,
and a late observer with phased bounded waits (wait budget 3 per phase; the
phases disambiguate rounds because `idle`/`ready` occur in both).

- **P3a** — a caller that acquire-observes round-1 `ready` (and wins C2) reads
  exactly the round-1 result, not the initial sentinel.
- **P3b** — an acquire observer of round-2 `ready` reads exactly the round-2
  result (the round-1 value `42` and the reset sentinel `0` are both
  distinguishable stales), sees the round-2 payload, and `reap_seq == 2`.

Modeling stand-in (documented, semantic-preserving): production gates round-2
reaping through the arena's backend-ready linkage; the kernel gates it with
bounded waits. Budget exhaustion takes a give-up path — properties simply do
not fire in that execution, so budgets trade exploration breadth for
termination, never soundness.

## Results (from the committed artifact)

| Input | rc11 | ra | sc | tso |
|-------|------|----|----|-----|
| K1 clean (5 executions each) | PASS | PASS | PASS | PASS |
| K2 clean (67 244 executions each) | PASS | PASS | PASS | PASS |
| N1 — A7 ready store → relaxed | **REJECTED** | **REJECTED** | PASS | REJECTED |
| N1b — observer acquire → relaxed | **REJECTED** | **REJECTED** | PASS | REJECTED |
| N3 — A3 commit CAS → relaxed | **REJECTED** | **REJECTED** | PASS | REJECTED |
| N2 — K2 round-2 observe acquire → relaxed | **REJECTED** | REJECTED | PASS | REJECTED |
| N4 — C4 idle store → relaxed | **REJECTED** | REJECTED | PASS | REJECTED |
| RA1 — A1 begin-binding CAS → relaxed | PASS (redundancy) | — | — | — |

Gated requirement: every control MUST FAIL under rc11 and ra. The `sc`/`tso`
columns are recorded, not gated: `sc` repairs every control (no reordering at
all), while `tso` **also rejects them** — under the C++ model semantics GenMC
implements, a relaxed publication leaves the non-atomic payload accesses racy
even on a TSO-shaped machine; hardware store-buffer FIFO order does not
license the race. This is the model-level reason relaxed publication is
undefined behavior rather than a benign x86 idiom.

### N4: a pre-registered prediction disproved by the checker

The #197 audit comment pre-registered C4 (`idle` release store) as a
**predicted-redundant** weakening, reasoning that "the round-2 claim CASes
(acq_rel) re-establish ordering". The checker **disproved** that prediction:
an acquire RMW reading from a **relaxed** store establishes no
synchronizes-with edge, so the caller's C3 plain clears race with the next
claimant's writes. N4 was upgraded from redundancy observation to negative
control — the clearest demonstration in this pilot that the kernel constrains
real production orderings rather than tautologies. (The genuinely redundant
weakening found is RA1: A1's acquire reads the initial state write — no
cross-thread edge — and its release half is subsumed by the later
same-thread A3/A7 releases.)

## Bounds and limitations

- **Bounded kernel**: 2–3 threads, 1–2 publication rounds, wait budget 3 per
  phase. Executions where a waiter exhausts its budget are explored via
  give-up paths; every execution in which an observation occurs within budget
  is fully explored.
- **Property class**: safety assertions + RC11 data-race detection only. No
  liveness, no fairness, no overflow reasoning.
- **Not whole-program**: the kernels check the Completion object's protocol;
  the arena, scheduler, and backend progress machinery are out of scope
  (their TLA+ models and TSan gates remain the evidence layers there).
- **CAS-loser fail-fast paths unmodeled**: production treats a lost
  claim/commit/publish/reset CAS as a fail-fast authority violation; the
  kernels arrange single-claimant shapes and `assert(win)` instead. The
  memory-ordering question under check is the winner path's visibility
  edges, not loser arbitration.
- GenMC checks the LLVM-IR lowering of these operations under the selected
  model; that is the standard meaning of a GenMC-class result, and it is what
  "BOUNDED KERNEL" scopes the claim to.

## Reproduction

```sh
# checker (once, user-local — see recipe above)
bash scripts/weakmem/verify-completion-weak-memory.sh --self-test
bash scripts/weakmem/verify-completion-weak-memory.sh \
  --artifact /tmp/completion-publication.json
```

The runner fails closed: a missing checker, an invalid explicit `GENMC_BIN`,
generator pattern drift, an unexpectedly failing kernel, or a control that
stops failing each abort with a named leg and reproduction command. The
committed artifact under `docs/results/weak-memory/` was generated by the
runner at the recorded revision; it is never hand-edited.
