# F5 independent adversarial review — candidate head `3aa3b857`

Two independent adversarial reviews were run against the exact candidate head
`3aa3b857e9fe19abc7575013cbc28e5b3e186cdf` (the documentation head whose
compiled inputs are identical to the implementation head
`a23a94cdfbfa90f6ad6aff2d1b6706c305a4245c`; PR #518 draft). Both reviewers
inspected source, build rules and raw artifacts rather than the author's
summaries, and were asked to produce counterexamples.

- **Review A — production correctness** (challenges 1–7 and 12): verdict
  **none-requiring-change** for the retained package/library closure; six
  findings, all minor or informational, none a package or runtime breakage.
- **Review B — evidence and honesty** (challenges 8–12): verdict
  **blockers-found**; one blocker, one major and eight minor/informational
  findings, all evidence-integrity defects in the packet, none showing a
  capture produced by a wrong tree.

## Dispositions (all applied at the head named below)

| # | Finding (review) | Severity | Disposition |
|---|---|---|---|
| A1 | `tests/package/f4_retirement/probes/retained.cpp` is an in-tree consumer of removed headers, and the Family C row claimed "every in-tree consumer is inside the removal batch" | minor | Row qualified; residual `F5-F4PROBE` recorded in `closure-manifest.md`/`.json`. F4 evidence stays frozen; probe supersession is an F4-owner/F6 follow-up |
| A2 | Installed `fiber.hpp` still declares Scheduler-era members (`friend class Scheduler;`, `CompletionWaitOutcome`, `set_entry`, `cancel_token`, `cancel_state`, `execution_tag`, `completion_wait_outcome`) with zero retained users | minor | Recorded as residual `F5-W04FIBER` with disposition `RETAIN_WITH_HOST`; removal is a further per-surface break on the pending W-04 host decision |
| A3 | `cancel.hpp` was classified CANONICAL_REQUIRED although F1 classifies it with the W-04 host closure and it is included only by `stackful_io_host.hpp`/`fiber.hpp` | minor | Reclassified to `OPTIONAL_CANDIDATE_W04_HOST_CLOSURE` in the manifest (JSON and table) with the basis recorded |
| A4 | Guarded test/mutant regions remain in installed headers; stale `scheduler.hpp` line in `scripts/verify_f1_package.py` CONFIG_REQUIREMENTS | informational | No action: recorded as pre-existing; the F1 verifier is left unchanged to preserve F1 provenance (residual `F5-F1VERIFIER`) |
| A5 | `library_tus_compiled=34`/`tree_tus_total=36` self-contradictory; member enumeration listed 15 names for 14 | informational | Corrected to 32 compiled (2 core + 30 async) / 34 tree TUs with the 2 orphan experimental ones; enumeration corrected to 9 `scheduler*` + 3 `select*` + `queue_port` + `wait_policy` |
| A6 | Lean/TLA models still cite retired headers | informational | No action: already HISTORICAL/ADAPT/RETIRE_FROM_V1_EVIDENCE per the formal evidence map; no model file changed (residual `F5-FORMAL`) |
| B1 | `README.md` listed `independent-review.md` before it existed | blocker | This file exists; the README entry is accurate as of the commit that adds it |
| B2 | Manifest asserted trigger T1 fired while its own JSON and the disposition record say no external-use signal was observed | major | Reworded: "T1 did NOT fire — no external-use signal observed; UNKNOWN, not zero" |
| B3 | "Every capture names the implementation head" was false for the matrix and probes | minor | `run-matrix.py` now records the worktree head in `toolchain.log`; `run-probes.py` records `source_sha` in `probes.json`; the execution record states the exact binding per capture and re-ran both captures |
| B4 | Base TU counts contradicted the base tree and the B-01 capture | minor | Corrected (32 compiled / 34 in-tree) |
| B5 | The T4 entry read as delivered | minor | Reworded to `NOT_RESOLVED / DECISION_REQUESTED` with `OWNER_APPROVAL=PENDING` |
| B6 | Real-ring attribution cited the profile label; the smoke test can skip when the ring is unavailable | minor | Execution record now cites the ring-only tests executing without `NOT RUN` markers, the B-01 `w03_external_loop(uring): PASS` run, and the throw-on-`io_uring_queue_init`-failure property; the label caveat is stated |
| B7 | E1–E5 were named but never itemized | minor | Explicit E1–E5 status table added to `installed-surface-risk-disposition.md` (E1 PENDING; E2/E4/E5 recorded satisfied; E3 sequencing satisfied, authorization pending) |
| B8 | Wrong filename for the TSan link-attempt log | informational | Path corrected to `matrix/tsan-n-request_scope_test-link-attempt.log` |
| B9 | "21 retired `[[noreturn]]` diagnostics" imprecise | informational | Reworded to 19 `[[noreturn]]` diagnostics plus `evented_admission_check` and its helper (21 entities) |
| B10 | Vestigial `friend class Scheduler;` in the installed host header | informational | Same disposition as A2 (`F5-W04FIBER`) |

## Fixes verified by re-execution

- Matrix capture re-run at the branch head with head recording enabled:
  `F5_MATRIX_SIX_PROFILES_PASS` (debug-n 52/52, debug-y 66/66, release-n 52/52,
  release-y 66/66, asanubsan-n 52/52, tsan-n 51/51).
- Installed probes re-run for both ring profiles with `source_sha` recorded:
  25 headers, 0 standalone failures, ODR link+run pass, 0 removed headers
  present, before-prefix control 49 headers with all 24 removed headers present.
- The invoked-and-frozen historical captures from other phases were not
  modified (verified by `git diff 14a27a83..HEAD -- docs/evidence/f1*
  docs/evidence/f2* docs/evidence/f3* docs/evidence/f4*` being empty).

## Not re-done

The reviews did not re-run F2/F3/F4 validations and did not reopen their
findings; every F5 claim above is bound to a capture produced by this slice.
No production source changed as a result of the review: all dispositions are
evidence, classification or wording corrections.

## Re-check round at `ce62c05b` and the round-2 dispositions

Both reviewers re-inspected the dispositioned head and re-ran their checks against
the raw artifacts.

- **Review A re-check: `none-requiring-change` (unchanged)** for production and
  package correctness; all six initial findings confirmed dispositioned, with
  three precision defects found in the newly written text (below).
- **Review B re-check: `blockers-found`** — one requested correction was still
  missing, plus precision defects; every technical gate, capture and SHA binding
  verified again, and no capture was shown to be produced by a wrong tree.

Round-2 findings and dispositions (all applied in the round-2 commit):

| # | Finding (re-check) | Severity | Disposition |
|---|---|---|---|
| R1 | The Family A table row still read "triggers T1–T3 all fired" while the bullet and JSON said T1 did not fire | blocker | Row reworded to "T2/T3 fired; T1 assessed — no external-use signal observed (UNKNOWN, not zero)" |
| R2 | Execution record and the review record claimed the matrix head was recorded in `matrix.json` as well as `toolchain.log`; only `toolchain.log` carries it | minor | Both texts now name `matrix/toolchain.log` only |
| R3 | `probes-commands.txt` did not include the `--source-sha` argument recorded in `probes.json` | minor | Commands updated with the argument and the prefix-hash binding note |
| R4 | The E2 row claimed every per-surface row cites MIG-02/HOST-01; the internal/test/experimental rows cite D-13/M-A4/F1 | minor | E2 reworded to the A/B/C capability rows |
| R5 | The execution record still described the fail-fast removal as "21 removed diagnostics (… group_lifetime_fail_fast)" | minor | Aligned with the disposition wording: 19 `[[noreturn]]` diagnostics plus `evented_admission_check` and its helper (21 entities) |
| R6 | `cancel.cpp.o` stayed in the canonical member list although the header was reclassified to the host closure | minor | Moved to the host-closure members (9 canonical async + 4 host) in the manifest JSON and table, with the `CancelToken::request()` consumer cited |
| R7 | The probe ELF binaries had been committed under `installed-probes/` | informational | Removed from the repository; the probe commands record that binaries/objects stay in the execution tree |
| R8 | The matrix PASS banner was not persisted | informational | The capture stdout is persisted as `matrix/matrix-run.log` |

R8 evaluation evidence is illustrative: the reviewer's own recomputation of every
`log_sha256` in `matrix.json` matched the regenerated logs, the TSan exclusion
entry and its `multiple definition of 'operator new'` reason are intact, and both
`probes.json` files re-verify (25 headers, 0 standalone failures, ODR link/run
exit 0, 0 removed headers present, control prefix 49 headers with all 24 removed
headers present).
