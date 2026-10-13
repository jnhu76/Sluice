# F2 mutation campaign — kill-set comparison

64 named mutation builds, two runs each, on three trees:

| Tree | Any-run deaths | Both-run deaths |
|---|---|---|
| baseline `0ed16588` | 63 / 64 | 62 / 64 |
| F2 before review `912315ac` | 63 / 64 | 62 / 64 |
| F2 head (see `gates.json`) | 63 / 64 | 63 / 64 |

`comparison.json` asserts set equality, not just counts, and it holds: the killed
set is identical on all three trees, and the same single mutant survives on all
three.

- Survivor `uring_cutover_mut_drop_outcome` — path-dependent by construction: the
  mutation drops the outcome instead of publishing it, and that path is caught
  by the protocol-unreachable invariant guard rather than by a scenario
  assertion, so it survives the suite-driven campaign on every tree, including
  the baseline. Its guard is exercised by the cutover suite under the paired
  mutation; the survivor is not a coverage change.
- `stackful_host_mut_stop_implicit_cancel` is timing-flaky on every tree: it died
  in one of two runs on the baseline and on the pre-review F2 tree, and in both
  runs on the head. The kill set is unaffected (it is killed on all three), and
  the flakiness is a property of that mutant's interleaving, not of this slice.

Files: `campaign.sh` (the runner: two runs per mutant target, exit code recorded
per run), `mutant-targets.txt` (the 64 targets), `*-results.tsv` (per-tree raw
records), `compare-runs.py` (the comparison and its assertions),
`comparison.json` (its output, including the per-run detail).

No mutant target compiles a test source that this slice edited except the two
`backend_conformance_e1_*` mutants, which compile
`tests/backend_conformance_e1_test.cpp` (the ring control tag change is
compile-only and guarded to the ring branch); the campaign was re-run on the
final head for exactly that reason.
