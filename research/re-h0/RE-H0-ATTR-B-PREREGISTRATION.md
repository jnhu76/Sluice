# RE-1U-ATTR-B PREREGISTRATION — CASE B causal ablation

Campaign #277 (authority #227), corrective experiment required by the
#278 review. Prereg P9 requires for CASE B (`T_backend MATERIAL`):
`census → rank → ONE causal ablation → remeasure`. The original
campaign stopped after census/rank; this document freezes the missing
one-ablation step BEFORE any ATTR-B launch. No ATTR-B number existed
when this file was written. It inherits the frozen authority of
`RE-H0-PREREGISTRATION.md` (P1–P14) wherever it does not explicitly
override it; nothing in the original frozen protocol changes.

## A1. Treated family (census-ranked, code-audited)

The router fixed-table extent-probe family — the top-ranked Sluice
symbol in the formal census (`RouterEntry::size()`, 4.8 % of process
user instructions as captured, Z2 S read) plus the same extent probe at
the other per-op CQE-path sites. Source-verified per-op probe sites
(`src/async/uring_backend.cpp`):

```text
S1  find_live_router_cookie_  scan bound + miss sentinel (production
    #else branch; the internal-testing branch's reverse/forward scans
    and miss sentinels are treated equivalently)
S2  handle_one_cqe            miss compare (router_index == size())
S3  finalize_operation_terminal_  range check (router_index >= size())
S4  retire_router_entry_      range check (router_index >= size())
```

`find_live_router_index_` (cancel path) is NOT exercised by this
workload and is treated for symmetry only. The `RequestSlot` /
`PreparedUringOp` census symbols have NO per-op `size()` call site in
the source (code audit: the arena bounds with `capacity_`, the prepared
table is indexed without extent probes) — they are `-O0` symbolization
artifacts (A2), are NOT treatable, and are NOT part of this ablation.

Machine-code audit at the measured release optimization (`-O3`): each
site recomputes the extent from the vector header (2 loads + sub + shr
+ magic-multiply ≈ 6–9 instructions); ≈ 4 recomputations per op →
direct-removal upper bound ≈ 25–30 instr/op ≈ 1.2–1.5 % of the formal
Z1b→Z2 delta (≈ 2 000 instr/op at S read). This bound is recorded as
the code-level expectation; the thresholds in A6 are frozen from the
census ranking (A3), not from this bound.

## A2. Census instrument disclosure

The symbolized census ran on a `releasedbg` rebuild. In this project
`releasedbg` injects NO optimization flag (only `mode.debug`,
`mode.release`, `mode.valgrind` rules are registered in `xmake.lua`;
clang default `-O0`, symtab only, no DWARF). At `-O0` each
`vector<T>::size()` is an outlined call computing a pointer difference
with an integer division by `sizeof(T)` (`idivq`) — absent at `-O3`.
Census per-symbol shares are properties of the `-O0` symbolization
build: they are used to RANK families and confirm per-op paths, never
as release-binary share authority. All formal instruction/wall
measurements are bound to the release binaries by SHA (`7401213f…`)
and are unaffected. (Contrast probe, pre-freeze: same S-read workload,
`-O3` user-CPU ≈ 30 ms/rep vs `-O0` ≈ 212 ms/rep.)

## A3. Falsifiable hypothesis (H1)

H1: the router extent-probe family is a causal contributor to the
Z1b→Z2 instruction residual at S read. If the census ranking
transferred to the release binary, removing the family's per-op extent
recomputations would recover a fraction of the residual on the order of
the family's captured census share (4.8 % of process instructions
≈ ~7 % of the Z1b→Z2 delta — an upper reading, since the captured share
includes the `-O0` division artifact).

## A4. Treatment F07 (research-only, semantics-identical)

Seam: `f07_skip_extent_reprobes` in
`src/async/tax0_ablation_seams.hpp` (guarded
`SLUICE_ASYNC_INTERNAL_TESTING`; production targets never define it).
R1 effect: the per-op sites read a construction-cached extent instead
of recomputing it from the vector header.

Invariance fact: `router_` is constructed once at `request_capacity`
and never resized (no resize/reassign on any path); the cached extent
returns the identical value at every site on every path, including
concurrent retire/install interleavings (the value cannot change).
Same semantics, same router, same Completion, same request identity,
same output; R0 (flag off) is bit-identical production behavior.

Measured binary: `tax0_ablation_bench` — the SAME harness linked
against `sluice_async_internal_testing` (TAX-0D precedent; production
`sluice_async` untouched). The R0 arm of this binary IS production
behavior.

## A5. Arms, cell, protocol

- Arms: `z1b` (semantic floor), `z2r0` (Z2, flag off = production
  behavior), `z2r1` (Z2, F07 R1). One binary for all three arms.
- Cell: S = 4 KiB × d8, READ only (the cleanest CASE B witness:
  T_backend MATERIAL in RE-1U AND 1.391 MATERIAL in RE-2U).
- Filesystems: btrfs primary + tmpfs control (control never supports
  the real-I/O claim).
- Protocol: P6/P7/P8 inherited unchanged (per combo: 1 wall launch of
  11 reps + two R7/R14 perf double-difference pairs = 5 launches;
  warmup 2; pinned CPUs; blocked-interleaved order from the frozen
  seed; write settle n/a — READ only; NO retries, fail-closed).
- Same-work: per-rep `word_sum` accounting + runner fail-closed
  validation, identical to RE-1U.
- Launches: 3 arms × 2 fs × 5 = **30**.

## A6. Analysis authority (frozen rule)

`attr_b_verdict` in `scripts/re_h0_analysis.py` (extended; diagnostics
in `check_re_h0_analysis.py` authored first). Per filesystem block,
after the inherited fail-closed block validation (A2 rules: missing/
duplicate arm, failed rep, error text, sha mismatch, word_sum mismatch
all refuse aggregation):

```text
instr(X)   = median of the two independent double-difference estimates
denom      = instr(z2r0) − instr(z1b)          (denominator > 0 required,
                                                else SessionInvalid)
fraction_i = (z2r0_est_i − z2r1_est_i) / (z2r0_est_i − z1b_est_i)
             for each independent estimate i
```

```text
MATERIAL_RECOVERY   both fraction_i >= 0.05
NO_RECOVERY         both fraction_i <  0.02
PARTIAL_RECOVERY    otherwise (measured fraction reported)
```

Band rationale (frozen): MATERIAL_RECOVERY ≥ 0.05 means the recovery
reaches the family's ENTIRE captured census share — the family is at
least as load-bearing at `-O3` as the census suggested. NO_RECOVERY
< 0.02 means below half the captured share — the census signal does not
transfer to the release binary. Between: the family's causal magnitude
is bounded but attribution stays incomplete. Wall R1/R0 ratio is
reported as sanity (no wall gate; the claim is instruction-layer).
Cross-check (reported, not gated): `z2r0` instruction level vs the
formal RE-1U z2 S-read level (different binary, same production
semantics; noise-level agreement expected).

## A7. Outcome → attribution mapping (frozen; every outcome is legal)

```text
MATERIAL_RECOVERY  → causal contributor identified at/above
                     census-share magnitude; attribution for this
                     family COMPLETE; residual remainder redistributed
NO_RECOVERY        → the census-ranked family is causally immaterial
                     at release optimization; the distributed-
                     implementation interpretation (REPORT §5.2) is
                     strengthened by causal evidence; attribution
                     remains INCOMPLETE (no identified causal hotspot)
PARTIAL_RECOVERY   → family magnitude bounded by the measured fraction;
                     attribution remains INCOMPLETE
```

No outcome authorizes a production change, a candidate selection, or a
"required semantics" reading (REPORT §5.2 stands in all outcomes).

## A8. Launch accounting after ATTR-B

```text
formal RE-H0      720  (RE-1U 200 + RE-1 120 + RE-2U 200 + RE-2P 200)
qualification     160  (qual262)
RE-1U-ATTR-B       30  (this experiment)
campaign total    910
```

## A9. #262 stop law (inherited)

Any unexpected `-ECANCELED`, named drain stall, wait error, teardown
failure or same-work mismatch in any ATTR-B launch ⇒ CELL INVALID /
CAMPAIGN PAUSE, evidence preserved to the session directory, posted to
#262. No retry-until-clean, ever.
