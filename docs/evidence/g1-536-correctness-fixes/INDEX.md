# #536 correctness-fix evidence index

Directory: `docs/evidence/g1-536-correctness-fixes/`
Slice: the two correctness-fix commits on PR #536's original branch,
`b88750e3` (bounded, allocation-free progress pass) and `64e3241b`
(suppression-test lifetime), on top of the frozen G1 adoption set.

This directory is additive. Nothing under
`docs/evidence/g1-403-native-optimization/` was modified, re-derived or
superseded; its raw JSONLs, reports and receipts stay as recorded at their own
SHAs.

## Files

| Path | What it holds |
|---|---|
| `FREEZE.md` | frozen configuration: SHAs, host, binary hashes, profile assertions, protocol, session map |
| `correspondence.md` | formal correspondence for the fixes: model-text identity, per-action argument, the liveness assumption, gate re-runs and their scope |
| `spec-ring.txt` | the frozen G1 `ab/final/spec.txt`, copied unchanged |
| `spec-noring.txt` | the no-ring spec (ThreadPool cases only; io_uring cases are not applicable to that build) |
| `ab-s1-b0-vs-fixed/` | session 1: frozen B0 vs fixed head, ring profile — `run.log` provenance, per-side JSONL per repetition, `AB.md` |
| `ab-s2-prefix-vs-fixed/` | session 2: frozen pre-fix arm (`b5c687ea…`) vs fixed head, ring profile |
| `ab-s3-noring/` | session 3: frozen B0 no-ring vs fixed head no-ring |
| `raw/negative-oracle/` | the new pass-bound test compiled against the **pre-fix** source: provenance (SHAs, binary hash) and five runs, all failing for the intended reason |
| `raw/positive-oracle/` | the same test file against the fixed source: provenance and eight runs, all passing |
| `raw/test-lifetime/` | open/close syscall census (`strace -f -c` and raw) for the suppression test before and after the RAII fix |
| `raw/suites/` | full-suite runs per configuration with `summary.txt` (exit code and wall time per stage) |
| `raw/mutation/` | mutation campaign at this head, with the per-mutant table |
| `formal/` | TLC re-run provenance and raw logs for `RequestCore.cfg` and `RequestCoreLive.cfg` |
| `RECEIPT.md` | the slice receipt: what was claimed, what was measured, what is not claimed |

## Reading order for a reviewer

1. `FREEZE.md` — is the comparison honest? (frozen binaries reused by hash, one new build, same host, interleaved protocol)
2. `correspondence.md` — does the formal evidence still apply, and what does it not cover?
3. `RECEIPT.md` — the claims and their verdicts, including the limitations.
4. the raw logs behind any number in the receipt.

## What this directory does not claim

* No new performance optimization. The A/B sessions here verify that the fixes
  did not discard the adopted G1 gains; new performance work belongs to the
  post-v0.0.2 issues.
* No conformance re-basing. The v1 conformance ledger's G1 entry keeps its
  status; this slice adds the correctness-fix record for the same requirements.
* No universal correctness. The suite, sanitizer, mutation and model evidence
  cover the exercised configurations on this host, at these commits.
