# E1 Native-Linux Validation — Decision (Gates R7/R8)

Formal verdict of the E1 native-Linux validation round on host E5, produced by
the preregistered reducer (`scripts/e1_verdict.py`, selftest 14/14) from the
frozen campaign evidence, per `protocol.md`.

## FORMAL_VERDICT

**`CONDITIONAL`** (`evidence/verdict.json`, schema
`sluice-e1-native-linux-verdict/1`, `issues: []`).

- `w02 = PASS` — 11/11 configs complete (990 rows = 11 × 2 arms × 15 reps),
  headline `4096-1024-1` median paired ratio **0.6930** (15/15 < 1.0),
  `4096-1024-8` **0.6239** (15/15); every non-headline config ≤ 0.996
  (non-regression bound 1.03); no oracle, identity, legality or completeness
  rejection.
- `w03 = PASS` — real epoll+eventfd external loop, 15 interleaved pairs per
  config: per-rep paired (preregistered) medians **0.8508** / **0.6146** /
  **0.9118** (D1 / D8 / 64KiB; bound 1.03); the frozen reducer's
  rank-paired medians, which are what `verdict.json` records, are 0.8587 /
  0.6171 / 0.9206 — both pairings pass, no status flips (deviation and both
  figures recorded in `w03-ab.md` and `protocol.md`); 10/10 verify rows
  `no_lost_wake=true` (idle park/wake, zero-length, 64-op burst coalescing
  with K>0 counter evidence, real kernel cancel, detach terminal
  responsibility).
- `p_positive = NOT_MEASURED` — sticky intent (P>0) states never occurred on
  the real kernel (liburing 2.13 + kernel 7.2.9): the formal campaign probe
  observed 0 sticky states, and a post-review supplement archived during
  review remediation (`p5-probe-supplement.jsonl`, frozen probe binary,
  byte-identical recreated fixture) adds 12 runs / **1,731 monitor samples**
  with 0 sticky observations, 0 parity violations, and 6 samples catching
  live control-path SQEs with the derived count still 0; the observed
  congestion window is drained internally by `io_uring_submit` before the
  app-visible SQ ring saturates, so the sticky branch is unreachable in
  these real workloads. The cancel-path congestion A/B was neutral (median
  ratio 0.9907 ≤ 1.03) — this is P=0 evidence only and is NOT a P>0 cost
  result.
- Protocol rule applied: W-02 PASS + semantic PASS + P>0 evidence missing ⇒
  overall **at most CONDITIONAL**; a silent NATIVE_GO is excluded by
  construction (`protocol.md` thresholds; reducer `exit_rule`).

Semantic gate (correctness overrides performance): 12 suites pass; all 5
mutants killed, of which the two E1-specific counter mutants
(`missing_increment`, `missing_decrement`) are killed by **named parity
oracles** (`e1_mutant_oracle_test.cpp`, explicit divergence messages, not
incidental destructor aborts); adversarial scenarios pass (8-thread cancel
storm with live parity monitor, real `-EINVAL` submit poison with sticky
intent present, settle-then-drain shutdown, wedged-transport probe showing
BASE and E1 behave identically).

## Attribution answer (Q4)

- Mechanism: E1 scans = 0, skip rate 100% on all 11 configs; BASE iterates
  the router exactly `calls × capacity` (e.g. **8,666 iterations/op** at
  C1024/D1 ≈ 173M iterations per 20K-op run).
- perf (`:u`, 100% time-enabled): instructions/op ratio **0.193** at
  C1024/D1 (120,665 → 23,233); the removed ≈ 97.4K instructions/op are
  accounted for by the scan loop at ≈ 11.2 instructions/iteration.
- Kernel boundary unchanged: `io_uring_enter` = 80,004 on both arms (1
  enter/op; 4 `eventfd2` at setup); futex ≈ 0. The entire gain is
  user-space redundant-work elimination; the end-to-end ratio (0.693) is far
  weaker than the instruction ratio (0.193) because the profile is
  syscall/kernel-dominated — the residual cost lives in the io_uring
  submit/completion path and request/scope plumbing, i.e. the E2/E3 arms'
  territory, not the router scan.

## Comparison with the WSL2 study

WSL2 headline medians were 0.858 (D1) / 0.731 (D8); native E5 exceeds them
(0.693 / 0.624) with the same mechanism signature (scans → 0, enter counts
unchanged, controls neutral). The WSL2 result reproduces qualitatively and
understates the native gain.

## Productization recommendation

**CONDITIONAL-APPROVE for the productization track**, subject to the
following obligations — none of which this research round is allowed to
discharge itself:

1. **P_POSITIVE_COST follow-up (blocking for full GO).** The sticky-cancel
   P>0 path remains unmeasured on any real kernel we control. Before (or in)
   the productization PR's ledger entry, either (a) reproduce P>0 on a
   kernel/liburing combination where app-visible SQ saturation occurs (older
   liburing without the internal CQ drain, or an `io_uring_enter`-driven
   submission path), or (b) obtain the cost bound through the existing
   controlled seam measurement, clearly labeled as seam-based, and record
   which one was done in the v2 conformance ledger. Until then the ≤1.05
   engineering line remains unverified.
2. **Named-oracle tests travel with the patch.** The productization PR must
   include `uring_pending_control_parity_test`, both named-oracle mutant
   targets, and the adversarial scenarios, with the same kill semantics
   observed here (named parity oracle, not destructor aborts).
3. **Authority-contraction audit in the ledger entry.** E1 adds a derived
   summary owned by `dispatch_mtx_`; the ledger entry must record that the
   router scan remains the semantic authority when count > 0, that the
   counter is non-authoritative, and the justified-mechanism answers
   (AGENTS.md Mechanism Justification Gate) for `pending_control_count_`.
4. **No scope change.** The productization PR carries only the E1 patch
   scope (backend counter + gate + tests); no public API, root-spec or
   runtime-authority change.

## NEXT_OWNER_GATE

- Update `docs/roadmap/v2-conformance.md` with the E1 obligation group and
  cite this round's evidence (verdict.json + reviews) per GOV-02.
- Discharge obligation 1 above and record the result.
- Keep Issue #563 open; this research PR stays open unmerged as evidence.

## Reviews (Gate R8)

Two independent reviews with separated responsibilities are attached:
[review-A.md](review-A.md) (semantics/concurrency/correctness) and
[review-B.md](review-B.md) (measurement methodology/statistics/attribution).
Both verdicts: **APPROVE_WITH_CONDITIONS**; neither raised a P1.

- Review A reproduced the semantic claims end to end (parity suite, both
  named-oracle mutant kills with byte-identical rebuilt binaries,
  adversarial scenarios, reducer byte-exact reproduction of
  `verdict.json`, source-level invariant audit) and raised one P2: probe
  statistics cited in this round's docs were not archived. Resolved in-round
  by archiving real supplementary probe evidence
  (`results-p5/p5-probe-supplement.jsonl`, 12 runs / 1,731 samples / 0
  positives, clearly labeled post-review) and rewording the unarchived
  citations.
- Review B recomputed every headline statistic from raw data (all match),
  reproduced `verdict.json` byte-identically, and raised three P2s, all
  resolved in-round: (1) the frozen reducer rank-pairs W-03 medians contrary
  to the protocol's per-rep pairing — both pairings now reported side by
  side (`w03-ab.md`, `protocol.md` deviations), rep-paired values are
  slightly stronger and flip no gate; (2) `identity.md` recorded a stale
  draft SHA for `run_e1_e5.sh` — corrected to the value the campaign's own
  provenance recorded (`2de68225…`); (3) `results-w03/arm-identity.txt` and
  `results-p5/arm-identity.txt` are stale verbatim W-02 copies — now flagged
  as non-receipts in `identity.md`, with post-hoc hashes of the actual
  W-03/P5 harness binaries recorded there.

The formal verdict `CONDITIONAL` is unchanged by the reviews and the
remediations above; the productization obligations in the next section
stand. One Review A note is carried into obligation 1: `no_lost_wake` is a
bounded-liveness oracle and cannot exclude a wake masked by an unrelated
subsequent one; the deterministic protocol suites beneath it remain the
load-bearing correctness evidence.
