# SEMANTIC-DIET-0 Report

> Bounded semantic-diet campaign between the G1-Control verdict and the #227
> thesis rewrite. Question under test: does Sluice's public / architectural /
> runtime semantic surface contain semantics kept mainly for hypothetical
> future Control / specialization that no current correctness, lifetime,
> ownership, boundedness, product behavior, or earned capability requires?

**Date:** 2026-09-04
**Branch:** `research/semantic-diet-0`
**Baseline (audited HEAD):** `eba6e3bb` (master; merged PR #284 — G1-Control closure)
**Verdict:** **PASS — SPECULATIVE SEMANTIC SURFACE REMOVED** (single K5 cluster; everything else independently justified)

---

## 1. Scope

Audited the full public surface: `include/sluice/**` (sync core, async, async
detail, experimental), current-authority docs (`docs/architecture`,
`docs/reference`, `docs/roadmap`, `docs/design`), tests, fuzz targets, examples,
benches, and the research record. Read actual callsites and tests, not just
names. Accepted the frozen G1-Control verdict (#283 / PR #284): strong Control
retired; Semantic Authority retained candidate-specific only; Control
specialization CLOSED BY DEFAULT.

## 2. Current semantic surface (inventory)

The complete 30-row inventory with per-element contract, consumer, and
evidence is in `SEMANTIC-DIET-0-AUDIT.md`. Surface classes:

- **Correctness/identity/lifetime (K1):** Completion terminal state machine,
  RequestHandle (context, slot, generation), RequestArena/RequestSlot/RequestKey,
  stale-completion prevention, cancellation precedence (CancelToken epoch),
  publication ordering, reader/writer short-I/O and zero-progress contracts,
  positional I/O, fail-fast vocabulary.
- **Resource/boundedness (K2):** request capacity, arena capacity, blocking-I/O
  pool bounds, copy limits, shutdown/drain, admission bounds.
- **Natural product semantics (K3):** copy_all + CopyLimit, Auto/Scratch/
  BufferedFirst strategy selection, SyncableWriter durability boundary,
  IoContext backend boundary, Batch (grouped completions — no group-admission
  claim), Future/Group, sync primitives, ApplicationRuntime lifecycle.
- **Mechanism with real consumers (K4):** WaitPolicy/EventedWaitPolicy seam
  (ADR-execution-model, both policies implemented), backend capability queries
  (supports_request_identity / has_split_wait_capability /
  has_bounded_split_wait_capability / supports_bounded_wait), experimental
  io_uring stubs.
- **Speculative (K5):** the CopyStrategy deferred-slot cluster (below).

## 3. Classification

The only K5 cluster: **CopyStrategy deferred slots** —
`VectorDeferred` / `FileRangeDeferred` / `SendfileDeferred` / `SpliceDeferred`
plus their supporting mechanism (`UnsupportedStrategyPolicy`,
`CopyOptions::unsupported_policy`, `CopyDecision::unsupported_requested`,
`CopyStats::strategy_deferred_*`). Their documented rationale is
"Forward compatibility — future strategies (vector copy, kernel zero-copy) can
be added as reserved enum slots" (docs/history/implementation-plans/
design-copy-strategy.md:39-40). They have no correctness need, no product need,
no earned capability, and no production consumer; the only test/example/fuzz
consumers are self-referential mechanism tests.

No K4 public leakage was found (all exposed capability/mechanism seams have
real consumers). No current-authority docs overclaim Control authority
(grep scan; the only close call — BATCH-X0's "fused admission" — is recorded
as a divergence witness, and the code matches the no-group-admission
contract).

## 4. What was kept

- All K1/K2/K3 semantics and all K4 mechanism seams.
- The three implemented CopyStrategies (Auto/Scratch/BufferedFirst) and
  CopyDecision observability (requested/selected/reason/path usage).
- All stats structs (caller-owned nullable observability, AC-1a).
- Batch, Cancel, Completion, RequestHandle, WaitPolicy, backends — untouched.
- Historical/research docs that record the old hypothesized Control chain
  (design-copy-strategy.md, COPY-X0 audit, reviews, archive) — preserved as
  evidence, not rewritten.

## 5. What was removed / hidden

Removed (all in the K5 cluster):

- `CopyStrategy::VectorDeferred/FileRangeDeferred/SendfileDeferred/SpliceDeferred`
- `UnsupportedStrategyPolicy` + `to_string(UnsupportedStrategyPolicy)`
- `CopyOptions::unsupported_policy`
- `CopyDecision::unsupported_requested`
- `CopyStats::strategy_deferred_rejected_calls` / `strategy_deferred_fallback_calls`
- `is_deferred()` dispatch + deferred branch in `src/copy.cpp` + `to_string` cases
- `tests/copy_deferred_strategy_test.cpp` (whole file), deferred cases in
  `copy_strategy_stats_test.cpp`, deferred to_string/default checks in
  `copy_strategy_test.cpp`, deferred scenarios in `examples/mvp_copy_strategy.cpp`
- Fuzz: `StrategyKind::DeferredReject/DeferredFallback`, `strategy:mod5` →
  `mod3` encoding, deferred oracle branches, corpus seeds `deferred_reject` /
  `deferred_fallback`, MANIFEST rows
- Docs: `docs/reference/api.md` + `api.zh-CN.md` CopyStrategy/CopyOptions/
  CopyDecision sections; test-target count 203 → 202 in the two mechanical-facts
  rows (architecture gate + history issue).

Nothing was merely hidden; the removed surface was dead contract, so deletion
is the honest disposition. No generic Control toggle was added (§11 of the
campaign — Control specialization stays CLOSED BY DEFAULT as a governance
default, not a runtime switch).

## 6. Why

Deletion admission rules D1–D7 all pass (full table in the AUDIT §6.2): no
correctness invariant or product behavior depends on the slots; consumers are
self-referential; v0.0.1 is tag-only with no semver/stability promise; the
primary rationale is exactly speculative future mechanism reservation; removal
does not touch G1-Safety evidence; and the three implemented strategies fully
preserve remaining copy behavior. Adversarial checks A1–A6 (campaign §14) all
resolve to KEEP/DEFER not applying.

The alternative — keeping the slots "in case" — is precisely the design debt
the campaign exists to remove, and the repo's own RX-1 lesson applies: "measure
enough to falsify a direction before paying its complexity cost; negative
evidence is a stop signal."

## 7. What was explicitly NOT removed

- Request identity / generation / stale-handle rejection (K1) — G1-Safety
  experimental targets are preserved (campaign §4 non-inference).
- Completion terminal semantics, cancellation precedence, ownership/lifetime,
  resource boundedness, shutdown/drain, publication ordering — STOP GATE B
  protected categories; none were candidates.
- Batch (BATCH-X0 falsified *information → authority*, not Batch's value;
  the contract already matches the no-group-admission verdict).
- Copy as a composed operation (COPY-X0: legal transformation boundary
  SUPPORTED; thin semantics kept, no specialization framework built).
- Auto/Scratch/BufferedFirst and CopyDecision (implemented, used, tested).

## 8. Risks

- **Future mechanism re-add:** a real vector-copy/copy_file_range/sendfile/
  splice strategy re-adds an enum value. Non-breaking (no consumer exists).
- **"Design intent evidence" loss:** COPY-X0's F-2 used FileRangeDeferred as
  design-intent evidence. The campaign is closed; the historical record
  (AUDIT + research docs) preserves the intent. The observable-fallback
  principle ("fallback legal only when explicitly requested AND explicitly
  reported") remains documented in the archived design document.
- **Public enum removal:** the slots were in the v0.0.1 tag. They were
  documented as NOT implemented; no program can observe any behavior from them
  other than rejection. Disclosed in this report/PR.
- No remaining risk to correctness, boundedness, or current product semantics.

## 9. Validation

- `git diff --check`: OK.
- Debug Clang: `xmake build sluice_core` OK; full `xmake build -g test` OK;
  full `xmake test -v`: **202/202 passed** (was 203 targets; one removed).
- Release Clang: `sluice_core` + copy tests pass.
- Focused copy/measurement/limit tests (debug + release): pass.
- Fuzz: `copy_all_fault_fuzz` builds; smoke run over the updated 46-seed corpus
  (new `mod3` encoding): init + 2000 runs, no crash.
- `python3 scripts/check-doc-links.py`: PASS.
- `python3 scripts/verify-architecture-docs.py`: PASS.
- `bash scripts/gates/pre-push.sh` (doc links, architecture docs, mechanical
  facts, assert-hygiene, claim-hygiene): **ALL CHECKS PASSED**.
- ASan/TSan/negative-compile: N/A — no ownership/concurrency/API-signature
  change (removed dead enum values + supporting fields; no behavior mutation).
- io_uring real-liburing: N/A — sync-core change only.

## 10. Impact on the next #227 rewrite

- Sluice now has **zero** semantic surface whose only justification is a
  hypothetical future optimizer/mechanism. The rewrite can state
  "Minimal semantics. No speculative optimization contract. Control
  specialization closed by default" with mechanical support.
- The G1-Safety / Boundary / Performance mainlines and #227 rewrite proceed
  with an audit-complete baseline: every retained semantic element has an
  identified correctness/resource/product/mechanism justification.
- Nothing in this campaign pre-empts #227's own decisions; it only removes
  surface the rewrite no longer needs to explain.

---

## Closure questions (campaign §24)

**Q1** Does Sluice have public semantic surface kept for hypothetical Control?
Yes — exactly one cluster (CopyStrategy deferred slots), removed.

**Q2** Which semantic elements have value independent of Control?
All K1/K2/K3/K4 items (identity, terminal, cancellation, lifetime, bounds,
shutdown, product ops, mechanism seams with consumers).

**Q3** What must be kept pending G1-Safety? All K1/K2 categories and STOP-GATE-B
protected semantics (untouched).

**Q4** What can be safely removed/hidden? The CopyStrategy deferred-slot
cluster (removed).

**Q5** Is the public semantic model simpler after removal? Yes — CopyStrategy
is now 3 implemented values; CopyOptions/CopyDecision/CopyStats carry no
deferred-mechanism fields; the copy contract has no "nameable-but-impossible"
states.

**Q6** Does the simplification sacrifice correctness/boundedness/product
semantics? No — all remaining strategies are implemented and honored;
byte-level copy behavior is unchanged; stats/decision observability for real
strategies is unchanged.

**Q7** Does Sluice still carry "API reserved for a future optimizer" design
debt? No — no remaining reserved/future-only surface was found.

**Q8** Is the desired end state reached?
```text
Minimal semantics.                 YES (for the audited surface)
No speculative optimization contract. YES
Control specialization closed by default. YES (governance default; no toggle)
```
