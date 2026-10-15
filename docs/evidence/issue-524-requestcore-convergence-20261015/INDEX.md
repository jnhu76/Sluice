# #524 — RequestCore model convergence (F6-F03)

Converges `formal/tla/RequestCore.tla` to the current v1-r4 C++ `RequestCore`
protocol by removing the retired pre-F2 caller-owned `Completion` release
flavor (`ReleaseBind`), and migrates the mutation oracle that was bound to it.
Production code, public API and the root specification are untouched.

BASE_SHA `9a501d1c8839c2f1375d48937c9bf4df18b93b26` →
model head `28f0fe3397114dbcda491851fd1932b02349df49`.

## Status at this record

| Gate | Status |
|---|---|
| R0 frozen baseline + isolated worktree | DONE |
| R1 correspondence reconstructed, incl. the dependency sweep | DONE — `correspondence-before.md` |
| R2 retired flavor removed | DONE — commit `083e1bb2` |
| R3 mutation oracle migrated | DONE — commit `28f0fe33` |
| R4 independent model-preservation review | DONE — one `APPROVE_MODEL_FREEZE`, two `REQUEST_CHANGES` on documentation/receipts only, no correspondence failure — `review.md` |
| R4 round-2 review of the pushed head `07ca71e8` | `REQUEST_CHANGES` — model, mutants, cfgs, gate diff and BASE drift all accepted; four evidence/classification findings, all corrected here — `review.md` |
| R5 fast preflight (parse, config, constants, 19 violating gates) | PASS 19/19 — `safety-preflight.md` |
| R6 complete safety run on the frozen snapshot | **PASS** (exit 0, 31,289,609 / 5,359,732, 48 s) |
| R6 complete liveness run on the frozen snapshot | **NOT RUN** — stopped on owner instruction; no L1/L5 claim is made — `resource-and-execution-manifest.md` |
| R7 evidence, ledger and map updates, PR | open |

`MODEL_STATUS = MODEL_CONVERGED_PENDING_FULL_TLC`.

## Files

| File | Contents |
|---|---|
| `correspondence-before.md` | R1: the four-action correspondence table, every `binding_live` write site and its guard, why an inflight release is diagnosed rather than legal, the behaviour-inclusion argument for the deleted edge, fairness and L1/L5 premises, and the implicit-dependency sweep that found the two mutants leaning on the deleted edge |
| `correspondence-after.md` | the converged action vocabulary with its C++ owners, the new `InvBindImpliesPublished`, and the requirement-level effect (VERIFY-02, F6 REQ-01/REQ-05) |
| `transition-diff.md` | the five-code-change delta, the isolation control that attributes the whole behavioural change to one deleted disjunct, the lost state sets separated into protocol projection and ghost-inclusive state, the no-stuck-state argument, and the old-vs-converged state counts |
| `mutation-gates.md` | the gate inventory change, why the retired mutant is retired rather than renamed, the new `MutDiscardKeepsBind` with its kill and its distinctness from both siblings, and the `MutReclaimIgnoresPub` reclassification as a subsumption witness with teeth |
| `review.md` | the R4 record: three independent verdicts, every finding and its disposition, plus the round-2 review of the pushed head and its four corrected findings |
| `safety-preflight.md` | R5: the fast level (parse/config/constant/cleanup/reference integrity) and the 19/19 violating-gate results, plus the explicit statement of what the fast level does not establish |
| `resource-and-execution-manifest.md` | the frozen snapshot SHAs, the host's measured topology, the run configuration, commands, logs, metadirs, exit codes and the reproduction commands |
| `formal-assets.tsv` | the #524 successor classification of the RequestCore formal assets (the frozen F6 `formal-assets.tsv` is historical and was not overwritten) |
| `raw/run-preflight.sh` | the fast preflight driver (reproducible, re-runs the 19 violating gates with the gate's verdict semantics) |
| `raw/run-full-tlc.sh` | the complete-run driver (safety then liveness, one JVM, persistent logs and metadir, exit codes) |
| `raw/tlc/safety.log` | complete safety run log |
| `raw/tlc/liveness.log` | **partial** liveness log — see `raw/tlc/STOPPED-liveness.txt`; not a result |
| `raw/tlc/exit-codes.tsv` | exit codes (`safety 0`, `liveness STOPPED_ON_OWNER_REQUEST`) |

## What this record does not claim

- No formal L1/L5 result and no formal liveness receipt: the liveness gate was
  stopped before completing. An independent reviewer's complete-space liveness
  run exists as corroborating scratch evidence in `review.md`; it is not
  substituted for the receipt, which stays PENDING (`formal-assets.tsv`).
- No end-to-end `scripts/verify_tla.sh` run (ten models, ~170 configurations);
  only the RequestCore slice changed, and the models it also covers are
  untouched (`git diff 9a501d1c HEAD -- formal/tla` is confined to
  `RequestCore.tla` and `RequestCore*.cfg`).
- No refinement proof. The model is now exact for the release sub-graph and
  remains an abstraction for the pre-existing items disclosed in the B1-1 note
  (no observer pin in `Reclaim`; no zero-op accept action).
- No C++ behaviour claim: production, tests and the root spec are byte-identical
  to BASE, so the existing C++ evidence stands unchanged.
