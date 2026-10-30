# research-612 — Direct P2C1: Prevalidated Register-Resident Read Compositions

Issue: jnhu76/Sluice#612 (product integration of #608's P2C1). Status:
**MERGED**; `P2C1_IN_MASTER = VERIFIED`.

## Research Question

Can the direct read path run whole read compositions as prevalidated,
register-resident steps — one entry admission, then per-step pread/read with
EINTR retry and outcome materialization, with no per-step re-validation —
without regression, contract violation, or public API/ABI change?

## Structure

- Candidate mechanism: single entry admission → private
  `prevalidated_pread_step`/`prevalidated_read_step` (pread/read + EINTR
  retry + count) → per-exit-style outcome materializer → register-resident
  compose loop. `direct_outcome`/`publish_certainty` deleted. The candidate
  used a `CompositionState` state machine; the merged version reduced this
  to a plain `confirmed_bytes` loop. The write path routes to
  machine-identical `write_at`/`write`.
- Research candidate branch `direct-read-compose-prevalidated-steps` @
  `deaf0301` (cut from C1 `77e5399a`; E5-local; research verdict REVISE at
  that stage).
- Integration ran a full matrix: A/A + A/B paired perf (16 ABBA pairs per
  shape), 7 named mutants + 3 C1 mutants, 15 fault scenarios, strace
  per-step one-syscall oracle, installed-SDK consumer probe, ABI symbol
  audit, WSL2 portability pass.

## Code References

- **Product**: merge `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` (master;
  P2C1 + C1 + #613, merge head `e4c9588a`). Rebased PR head `64508282`
  (diff byte-identical to the original P2C1, sha256 `b790936c…`).
- Original PR #612 branch head `70d48f5e` (4 own commits `42f5eef9` →
  `cc8ad2ab` → `b12fb551` → `70d48f5e` stacked on C1; E5-local only, never
  pushed; PR was rebased before merge). Base `bdfd0d8d`.
- Records on GitHub master: `docs/research/product-integration-records-archive/`
  (archived `99deb766`; INTEGRATION_LOG/PLAN + final-head per-sample
  receipts); authoritative R4 acceptance in
  `research/direct-beyond-parity/closure/perf-p2c1/SUMMARY.md`;
  raw receipts in `research/direct-beyond-parity/p2-raw/`; WSL2 boundary in
  `docs/research/retention-wsl2-audit/`; THREAD-01 disclosure in
  `docs/roadmap/v2-conformance.md` (SEM-05 row). No code reproduced here.

## Conclusions

- **Final acceptance (authoritative R4)**: `eofmix` **WIN_CONFIRMED**
  (−12.04 ns/op), `cursor` **WIN_CONFIRMED** (−20.17 ns/op),
  `comp-read-4k` **INCONCLUSIVE**; negative controls PARITY. Early,
  larger PR-text numbers (≈−19.0/−15.9/−15.1) are superseded — do not cite
  them as the acceptance result.
- Final-head session (16 pairs/shape): comp-read-4k −13.163 ns/op, CI95
  [−32.62,+9.13] crosses 0 → INCONCLUSIVE holds; eofmix −13.012 met the win
  standard this session; cursor −15.601 contaminated by a −12.8 ns/op A/A
  drift → R4 remains the authority for cursor.
- Correctness: 7/7 named mutants killed (independently re-verified as
  10/10 including C1); fault tests 15 scenarios, 0 divergences; strace
  oracle: exactly one syscall per step, 5 scenarios.
- Boundary: SDK/ABI unchanged (16/16 installed-SDK probe, 11 installed
  headers identical, `nm -g` global-symbol diff empty).
- Platform: WSL2 `NOT_DISTINGUISHABLE` (portability PASS; no measurable
  wall effect off-E5) — the win is an E5-native result.
- Contract: out-of-contract THREAD-01 concurrent close mid-composition now
  surfaces an EBADF-class backend_error instead of stopping at
  invalid_state; in-contract behavior unchanged; both trees are caller
  data-races. Disclosed in the conformance ledger, not hidden.
- Review trail: REVISE (guard-red + PR perf-text rewrite) resolved before
  merge; correctness APPROVE; no formal GitHub review — evidence is
  local-review-based.

## Next Steps

`comp-read-4k` is the open shape: it needs a protocol that separates the
~−12.8 ns/op A/A drift before any claim can move off INCONCLUSIVE. If
touching the compose loop again, keep the per-step one-syscall strace
oracle and the mutant set — they are what made the integration auditable.
The THREAD-01 concurrent-close boundary is accepted behavior, not a bug to
"fix" silently.
