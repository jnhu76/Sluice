# F2 authority contraction — evidence index

Slice: #402 F2, Request/Result/Identity authority contraction (clean break).
Baseline master: `0ed16588b7101f108aa7282074ba3d568f9d34e9` (F3 and F4 closed).
Worktree: `/home/jnhu/Source/Sluice-f2`, branch `f2-clean-authority`, draft PR
jnhu76/Sluice#516. The original `master` worktree was not modified.

`gates.json` in this directory is the authoritative binding: it names the
implementation head every capture below was run against, the profile rows with
their pass counts and per-target log hashes, and the external gates (GitHub CI,
TLA+) with their outcomes. A capture is evidence for the revision recorded
there, not for whatever the branch happens to point at now.

| Path | What it proves |
|---|---|
| `minimal-state-ledger.md` | Which runtime state was removed and why it had no independent authority, which apparently redundant state was audited and kept with a named obligation, and the measured package deltas |
| `execution-record.md` | Commit structure, requirement mapping, canonical surface, every executed gate, and the two defects the slice's own gates found (R14 wake-budget oracle, ASan stack-use-after-scope) |
| `installed-surface-risk-disposition.md` | The exact external/installed break, per-surface, with migration and the pending owner decision |
| `b01-v5-candidate/` | `reproduce.py` (dual-profile clean-room package capture: before/after × no-ring/real-ring), `persist.py`, `commands.json` (every command with argv, cwd, exit code and log hash), `summary.json`, `raw-package-evidence.tar.gz` + `raw-manifest.json` (714 text artefacts with hashes), `excluded-artifacts.json` (the reproducible big artefacts, by hash) |
| `installed-probes/` | Per-installed-header standalone compile, cross-translation-unit link and ODR identity probe, and the removed-header negative control measured on the before prefix |
| `matrix/run-matrix.py` | The six-profile runner (Debug/Release × no-ring/real-ring, native Clang ASan+UBSan and TSan scopes) with its per-profile unavailability rule |
| `r14-load-attribution/` | Why one migrated scenario was restructured, with the load, quiet-machine and wake-latency measurements that attributed the failure to the test cell rather than the library |
| `mutants/` | The 64-mutant campaign on three trees (baseline, pre-review F2, head) with the comparison script that asserts kill-set equality, the surviving mutant and the timing-flaky one |
| `ci/` | GitHub CI runs for this branch, including the head run with its job list |
| `formal/` | TLA+ gate log and the model-to-C++ correspondence note for the transitions this slice touched |
| `blocker-w03-stall.md` | **CLOSED blocker**: the F2 head stalled the W-03 external-loop scenario under two-core starvation while the baseline did not; measurement and failing-check record as it stood open at the captured head |
| `w03-root-cause.md` | The stall's root-cause closure: deterministic handoff-pause reproduction (causal witness, corrected-protocol witness, missing-notification negative), application-handoff classification with the empty production diff, the test-only corrective commits, the three-tree differential stress campaign and the re-run gates at the corrected head |
| `uring-poison-post-accept/` | **CORRECTED** gap §1: the pre-verified poison × post-accept DAG, the deterministic RED strand at the unfixed head, the minimum production correction (post-poison retirement through the failure terminal under `claim_execution` arbitration) and the GREEN receipt |
| `e1-gate-lifetime/` | **CORRECTED** gap §3: the E1 test's `GateGuard` teardown race — deterministic RED 8/8, the guard-waits-for-exit fix plus gate-before-backend ownership ordering (test-only), GREEN receipt and 12-copy contention recheck |
| `retire-delivery-moved-from/` | **CORRECTED** gap §2: the moved-from `retire_delivery` dereference classification (no documented precondition, benign-rejection sibling contract, reachable via a held context reference) and the guard + oracle |
| `formal/rcore-live-reuse.md` | The `rcore-live` reconciliation: INCOMPLETE at this capture, VERIFIED_REUSABLE historical proof via the byte-identical input identity table, F2 formal acceptance ACCEPTED_BY_REUSE |
| `known-preexisting-gaps.md` | Three gaps the review found that predate this slice; all three corrected at the final head with deterministic receipts |
| `final-stress/` | The final-head focused differential stress vs the baseline worktree: campaign script, environment, results (240/240) and the per-run logs |
| `collect-gates.py` | Asserts that every capture names the same head and writes `gates.json`; a capture bound to a different revision fails here instead of being averaged in |

Reproduction, in order:

```
python3 docs/evidence/f2-authority-contraction-20261013/b01-v5-candidate/reproduce.py \
    --repo "$PWD" --before 0ed16588b7101f108aa7282074ba3d568f9d34e9 \
    --after "$(git rev-parse HEAD)" --output /tmp/f2-b01-v5
python3 docs/evidence/f2-authority-contraction-20261013/installed-probes/run-probes.py \
    --prefix /tmp/f2-b01-v5/after-installed-n \
    --before-prefix /tmp/f2-b01-v5/before-installed-n --output /tmp/f2-probes
python3 docs/evidence/f2-authority-contraction-20261013/matrix/run-matrix.py \
    --repo "$PWD" --output /tmp/f2-matrix
SLUICE_TLA2TOOLS_JAR=<pinned tla2tools.jar> bash scripts/verify_tla.sh
```

Room cannot be made for the exact numbers in this file: they live in
`gates.json` and in each capture's own `summary.json`, `results.tsv` and
`commands.json`, so a stale copy can never be mistaken for the record.
