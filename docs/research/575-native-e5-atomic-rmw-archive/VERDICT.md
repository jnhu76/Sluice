# VERDICT — Sluice #575 Native E5 Atomic RMW Elimination Research

```
THREADPOOL_VERDICT = NO_CHANGE_JUSTIFIED
URING_VERDICT      = NO_CHANGE_JUSTIFIED
ISSUE_575_VERDICT  = NO_CHANGE_JUSTIFIED
PRODUCT_CHANGE_AUTHORIZED = NO
MERGED = NO
```

Closeout revision (2026-10-24): after issue closeout, the research records
were revised for evidence integrity — the withdrawn "~0.4–1 % UR locked-
RMW cost" phrasing was removed from the README (it contradicted this
verdict's retraction), the multi-worker TP plain-increment control rows
were marked `INVALID_FOR_CAUSAL_INFERENCE` as undefined-behavior
observations, code-shape findings were relabeled
`CODE_SHAPE_CONFOUNDING_SUPPORTED` (no specific layout mechanism claimed),
the W-03 proxy-calibration limitation was restated, and the statistics
language was tightened (exact computation, not exact coverage). No verdict
changes; no raw measurement was altered. The UR positive-count oracle gap
found by the campaign was closed on master via PR #585 (test-only).

## What was established

1. **Semantics (R1, confirmed by Review A's independent re-verification):**
   `ThreadPoolBackend::syscall_count_` and `UringAsyncBackend::submit_flushes_`
   are observation-only derived summaries whose sole consumer is the
   macro-gated internal test seam. They hold no semantic authority, no
   ordering role, no production reader, and no public-ABI exposure. Removal
   from Release via `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` is
   semantically safe and preserves all internal counting behavior
   (deterministic suite all-green on every arm; TP-M1 and UR-M1 mutants
   killed by their intended counter assertions, re-executed by Review A).
2. **Codegen (R2):** neither RMW is compiler-eliminated — `lock addq`
   survives GCC 16.2.1 -O3 through optimized GIMPLE, the archive objects
   and the linked stripped Release executables (byte-neighborhood
   verification). The WSL2 GCC 15 preliminary step was replaced by this
   native E5 GCC 16 verification per the owner override
   (`NOT_RUN_BY_OWNER_OVERRIDE` for the WSL2 environment only).
3. **Performance (R5–R8, corrected statistics per Review B):**
   - Mechanism level: removal eliminates ~1 instruction per counted event
     (−0.022 % at the highest-frequency TP config; assembly-proven).
   - Runtime level: no established locked-RMW cost for either backend
     (TP control arm ≈ BASE at the single-worker attribution config, with
     the multi-worker ctrl rows `INVALID_FOR_CAUSAL_INFERENCE` as data-race
     observations; UR's suggestive W-03 trend overlaps the A/A band; the
     first-draft UR d1 claim is retracted as a CI artifact).
   - Product level: **no material end-to-end benefit.** The measured
     ±0.7–3.8 % swings between arms are binary-specific code-shape
     artifacts — proven by the plain-increment control arms and consistent
     with the ~0.03 % physical bound — and they net to ≈ 0 or slightly
     negative for the actual candidates (TP C at d1-w1: +0.6 % [1.000,
     1.011] knife-edge slower; UR null everywhere with one in-band W-03
     point).
4. **Methodological deliverables:** the one-character tmpfs-path-name bias
   (~1 % on identical binaries, R5); the exact-vs-Monte-Carlo bootstrap CI
   hazard at n = 15 (Review B); the UR positive-count oracle gap and its
   closing test (`tests/uring_flush_count_oracle_test.cpp`, committed on
   the research branch); the plain-increment control-arm protocol for
   separating lock semantics from code shape.

## Why NO_CHANGE_JUSTIFIED (not PROPOSE_UPSTREAM, not BLOCKED)

- `PROPOSE_UPSTREAM` requires earned correctness **and** native performance
  evidence of material end-to-end benefit. Correctness is earned; the
  performance benefit is affirmatively absent. The issue's own rule — "a
  lower instruction count alone is not a product performance win" — decides
  this.
- The operation was **not** compiler-eliminated, and its cost is **not
  empirically material**: the locked RMW's own cost is below the rig's
  resolution (TP) or unconfirmed at the ~0.5–1 % level (UR), while the
  deletion patches' code-shape side effects are of the same magnitude and
  mixed sign — i.e., any future removal PR gains nothing measurable and
  risks ±1–4 % layout luck in either direction.
- Nothing blocked the campaign: every gate ran to completion on E5 native
  with two independent reviews.

## What remains an owner decision

The counters are dead product state under the minimal-necessary-work
thesis (MW-HW-0 P0). An owner may still choose to remove them on
semantic/minimal-work grounds — the research branch carries the exact
patches and the new oracle for that purpose — but per this campaign's
evidence such a PR **must not cite performance as its justification**, and
it must re-verify its own binaries' end-to-end numbers, since the measured
swings are binary-specific. The `perf_event_paranoid` owner override
(2 → 1) used for kernel-inclusive counters during this campaign is
restored to 2 at campaign close.

## Evidence index

`R0-IDENTITY.md` → `R1-AUTHORITY-AND-DAG.md` → `R2-BASELINE-CODEGEN.md` →
`R3-ARMS.md` → `R4-CORRECTNESS.md` / `R4-MUTATION-RECEIPTS.md` →
`R5-CALIBRATION.md` → `R6-THREADPOOL.md` → `R7-URING.md` →
`R8-CAUSAL-ANALYSIS.md` → `R9-REVIEWS.md`; raw data under `raw/`,
patches under `patches/`, assembly under `assembly/`, scripts under
`scripts/` (all self-tested). Per-file SHA-256: `manifest.json`.
