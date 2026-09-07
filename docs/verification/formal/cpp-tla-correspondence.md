# C++ ↔ TLA+ Correspondence and Drift Guard

This document is the stable navigation and claim-boundary map for Sluice's
C++ ↔ formal correspondence work. GitHub Issues remain the execution and
experiment records; this page explains which issue owns which question and
what evidence is currently earned.

The permanent rule is:

> **Green formal evidence is not automatically current C++ correspondence.**

And the fail-closed rule is:

> **The system may be allowed not to know. It must never silently not know.**

## Authority map

| Issue | Role | Current status / result |
|---|---|---|
| **#163** | Verification-architecture doctrine: C++-first bug finding, as-built modeling, evidence vocabulary, semantic trace layer | **CLOSED / durable doctrine.** Defines `TRACE-CONFORMANT (TESTED EXECUTIONS)` and keeps trace, weak-memory, fault/platform, and other evidence classes separate. |
| **#196** | V2 E9 trace-conformance substrate | **CLOSED / completed.** PR #202 established real deterministic C++ → semantic E9 events → declared prehistory/refinement → TLC replay against the matching E9 model, with positive and rejected negative traces. |
| **#298** | FDG-0 static formal-drift / impact research owner | **OPEN as research owner.** Phase C is closed with `METHOD_RECALL_NOT_EARNED`: 9/12 claim recall; C-001/F08, C-007/F03, C-012/F08 missed; C-011 exposed unsafe PRECISE narrowing. Static routing is research/advisory evidence, not enforcement authority. |
| **#305** | TV-1 historical sensitivity of the #196 semantic-trace channel | **CLOSED / completed.** PR #306 verdict `TRACE_CHANNEL_PARTIAL`: C-001 was discriminated; C-012 and C-007 remain trace-vocabulary gaps; C-011 proves trace acceptance does not imply downstream formal-suite freshness. |

Related but non-owning records:

- **#299 / PR #300** — SCIP method-selection precursor (`SCIP_GRAPH_EARNED`);
- **PR #302** — Xmake Build Truth;
- **PR #303** — Formal Facets research substrate;
- **PR #304** — Phase-C Historical Gold, `METHOD_RECALL_NOT_EARNED`;
- **PR #202** — #196 E9 trace-conformance implementation;
- **PR #306** — #305 TV-1 historical drift sensitivity;
- **#239** — deterministic scheduling/simulation evidence, complementary to
  trace validation but not the trace/correspondence owner.

Do not infer current execution status from an old task-book paragraph when a
later closure/status comment exists. Historical task books are intentionally
preserved as experiment records rather than rewritten after results are known.

## Two different correspondence channels

The static formal-impact channel and the semantic execution channel answer
different questions and must not be collapsed.

### 1. Static formal-impact channel

```text
git diff
   ↓
Xmake Build Truth
   ↓
SCIP structural recovery
   ↓
explicit anchors / facets
   ↓
candidate formal claims / suites requiring attention
```

Question:

> **Which formal evidence might this source/build change affect?**

Current evidence from #298 Phase C:

```text
Method C claim recall:        9/12 = 75%
claim misses:                 C-001/F08, C-007/F03, C-012/F08
aggregate silent NO:          0
unsafe PRECISE narrowing:     C-011
verdict:                      METHOD_RECALL_NOT_EARNED
```

Therefore this channel may support review and bounded scenario/suite
selection, but it is **not** authorized to silently clear semantic impact or
to act as an enforcement-grade "run only these formal suites" oracle.

Permanent static rules:

```text
UNKNOWN / stale / unresolved
    → remain visible and fail closed

UNKNOWN
    ≠ NO_FORMAL_IMPACT

structural reachability
    ≠ semantic equivalence

triggering anchor
    ≠ complete downstream formal-obligation scope
```

Phase C also demonstrated that one valid build world is not all relevant
build worlds: C-012 changed real liburing semantics that were absent from the
frozen `with-liburing=false` world.

### 2. Semantic trace-validation channel

```text
real deterministic C++ execution
   ↓
architecture-level semantic observations
   ↓
declared scenario prehistory / concrete→abstract refinement
   ↓
event→model-action mapping
   ↓
TLC replay against the named as-built model
   ↓
TRACE_ACCEPT / TRACE_REJECT / TRACE_COVERAGE_GAP
```

Question:

> **Can this named model realize this named executed C++ behavior?**

The first substrate is #196 / PR #202 for E9 park/wake. Its event vocabulary
includes:

```text
ParkCommitted
ParkEntered
ParkReturned
ParkRefused
WakePublished
```

TV-1 (#305 / PR #306) tested that existing channel against historical
Phase-C failure classes without tuning the static resolver or the model.

Final TV-1 evidence:

| Historical case | Static Phase-C result | Semantic/runtime result | Interpretation |
|---|---|---|---|
| C-001 / F08 lost wake | **MISSED** | repaired `TRACE_ACCEPT`; historical-defect mutant `TRACE_REJECT` + `EXECUTION_DIVERGED` | Existing E9 channel has real historical discrimination where vocabulary/scenario cover the protocol. |
| C-012 / F08 poison wake | **MISSED**, config-world confounded | `with-liburing=false` = `NOT_APPLICABLE_BUILD_WORLD`; real liburing world distinguishes repaired/broken at runtime, but trace = `TRACE_COVERAGE_GAP` | Build world and trace vocabulary are independent coverage dimensions. |
| C-011 scheduler liveness | top-level surfaced; PRECISE omitted E12 obligation | E9 corpus accepts, E12 suite independently passes; no mechanical propagation relation | Behavioral trace correspondence does not establish downstream formal-impact propagation. |
| C-007 / F03 winner outcome | **MISSED** | `TRACE_COVERAGE_GAP` | Current E9 vocabulary does not observe that semantic domain. |

For C-001, replaying the same broken six-event sequence with no declared
prehistory still rejected. The event sequence therefore carries the
**discriminative verdict**; the correct scenario-bound prehistory/refinement
mapping carries the **C-001-specific semantic localization**. Both are part of
the correspondence contract.

## Exact meaning of trace results

`TRACE_ACCEPT` means only:

> The named executed observation sequence, under the declared scenario and
> refinement mapping, is admitted by the named model/build evidence scope.

It does **not** mean:

```text
all C++ executions conform
all schedules were explored
the C++ memory model is proved
all downstream formal suites are current
the public contract is correct
the whole C++ implementation is formally verified
```

`TRACE_COVERAGE_GAP` and `NOT_APPLICABLE_BUILD_WORLD` are valid evidence
outcomes. They must never be converted to `TRACE_ACCEPT` merely because no
contradiction was observed.

## Build and revision provenance

TV-1 exposed an important evidence-integrity boundary. The #196 trace schema's
revision field is a caller-declared **source identity**; it is not, by itself,
a machine attestation of compiler flags, build world, or executable bytes.

TV-1 repaired its own evidence by using a committed source revision, recording
build variants separately, forcing mutant rebuilds, and requiring the expected
negative-control failure signature.

If trace validation is ever promoted beyond research evidence, a stronger
artifact should machine-record at least:

```text
source revision
model revision / model blob identity
Build Truth build_id
configured build world
relevant compiler defines/options
executable or build-artifact identity
scenario identity
```

That hardening is a future promotion requirement, not an authorization to
build a new framework now.

## Current promotion boundary

Neither #298 Phase C nor #305 TV-1 authorizes:

```text
AST-SF / CodeQL expansion by default
LLM semantic authority
auto-clear / auto-refresh of formal correspondence
production pre-push or PR-CI formal narrowing
whole-program tracing
a generic trace event bus
claiming trace acceptance as downstream suite freshness
```

The currently supported architecture is:

```text
static structure
   → advisory candidate scope / scenario selection

semantic execution
   → tested-execution correspondence evidence

weak-memory / deterministic scheduling / fault / platform evidence
   → separate Safety evidence layers
```

A future promotion must start from a new bounded, falsifiable hypothesis; it
must not tune the frozen Phase-C or TV-1 evidence until a positive verdict is
obtained.

## Repository evidence pointers

- `docs/verification/formal/e9-trace-conformance.md` — #196 event vocabulary,
  mappings, prehistory/refinement rules, and corpus semantics;
- `docs/verification/formal/tv1-trace-drift-sensitivity.md` — #305 historical
  sensitivity experiment and `TRACE_CHANNEL_PARTIAL` result;
- `docs/verification/formal/formal-impact-pilot.md` — static impact substrate;
- `docs/verification/formal/fdg0-phase-a-build-truth.md` — Xmake Build Truth;
- `docs/verification/formal/fdg0-phase-b-formal-facets.md` — facet experiment;
- `docs/verification/formal/fdg0-phase-c-historical-gold.md` — real-history
  false-negative boundary and `METHOD_RECALL_NOT_EARNED` result;
- `docs/verification/formal-models.md` — model inventory and claim scope;
- `spec/tla/manifest.json` — formal suite inventory.