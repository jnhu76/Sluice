# R8 — independent review record

Two reviewers with independent context examined commits `9a429c0d`
(test repair) and `0267e9ab` (evidence), each instructed to read the code and
diff themselves rather than the author's summary. Both verdicts: **Approve**,
no P0, no P1. Reviews were run on the branch head before the review-response
commit below; both re-verified behavior empirically (suite runs, concurrent
executions; reviewer B additionally rebuilt the M1 mutant and measured the
witness-timeout teardown on all three subtests: r4 18.011 s, saturated
10.002 s, control 10.002 s, exit 1, no terminate, no joinable-thread leak).

## Reviewer A — DAG / synchronization / oracle / scope

Independently verified: `signal()` mutex atomicity; enqueue-before-signal
program order; per-scenario producer enumeration (including
`close_admission_on_progress_exhaustion_` and the injected-dispatch path,
both unreachable); baseline placement; witness-before-resume ordering in all
three tests; post-resume determinism (r4 revalidation-before-prepark,
saturated single park, control pass-before-control-check); assertion
strengthening only; teardown safety; deadline-bounded witness. Adversarial
interleaving search found none that passes on an invalid execution; the one
defect-shaped pass (r4 with a dropped fd-write inside `signal()`) is correct
scoping — that path is discriminated by the parked shapes (r3/r5/r8/r9) and
by M1. Corroborating runs: 120 concurrent executions, 0 failures.

## Reviewer B — adversarial break attempts

Attempted counterfeit witnesses, vacuous baselines, nondeterministic
post-witness interleavings, teardown hazards, and EAGAIN-precondition leaks;
all closed against the code. Confirmed `has_immediate_physical_work` never
short-circuits the probe for the ThreadPool backend (base default false).
Re-counted the baseline receipts (21 failures across both campaigns,
subtest distribution consistent). Ran 3 sequential + 48 contended executions
of the rebuilt head: all pass.

## Findings and dispositions

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | P2 | helper comment narrates the sufficiency argument (AGENTS: semantics belong in docs); hazard sentences are the permitted category | Accepted — trimmed to the misuse hazards, sufficiency argument referenced to this evidence dir (review-response commit) |
| 2 | P2 | the witness's producer-set premise (single gated worker is the only token producer in the scenario) is enforced by nothing in code | Accepted — the misuse note now names extraneous producers (zero-op submission, cancel, second worker) as invalidating the interleaving |
| 3 | P2 | witness spin takes `ProgressSource::mtx_` each iteration; unfair-mutex oversubscription could marginally delay the signal | Informational — bounded by the 10 s watchdog; 0/480 + 0/480 campaigns and both reviewers' runs support adequacy; no change |
| 4 | P2 (pre-existing) | drain-tail `discard()` terminates if a worker never enqueues; f1 spin unbounded and single-field | Out of confirmed-defect scope — recorded in the README Limitations for owner scheduling |
| 5 | P3 | doc phrasing: "write of 1 would exceed the eventfd maximum" is arithmetically imprecise | Accepted — reworded to the kernel's strict-inequality admission check (`2^64-2 + 1` rejected because the result must stay strictly below `2^64-1`) |

No P0/P1. Findings 1, 2 and 5 are closed by the review-response commit;
3 requires no change; 4 is recorded as standing debt outside this slice.
