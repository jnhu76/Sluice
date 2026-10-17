# Deletion safety DAG

For every removed asset: entrypoint -> dependency -> output -> consumer ->
contract obligation, and which retained asset carries the obligation after
removal. Rule applied: no removal may leave a current v2 obligation without
its witness.

## Scripts (entries: CI / documented / ledger)

1. `verify_v2_package.py` chain: CI self-test -> clean-room package ->
   G9/RG3 rows. UNTOUCHED (allowlist macro spelling follows the rename;
   verifier re-run recorded in `verification-record.md`).
2. `verify_mr_disposition.py` chain: `mr-disposition-guard.yml` ->
   `docs/review/m-*.json` -> M-R disposition closure (dated, completed
   record `m-phase-final-closeout.json`, "VERIFIED for M-R"). Edge removed:
   the CI consistency check. Obligation carrier: Git history at `9845cee1`
   reproduces the closed record; no v2 row or release gate reads the JSONs
   (verified by tree-wide grep at base: only the workflow and the two
   scripts referenced them).
3. `verify_m_final_adoption.py` chain: manual -> v1 root file (already
   deleted) -> FAILED at HEAD before this round. Edge already dead; removal
   formalizes it.
4. F4 pair + `tests/package/f4_retirement/` chain: manual ->
   `probes/*.cpp` + `w04_stackful_candidate.cpp` (already deleted by PR B)
   -> archived F4 evidence. Live obligation carrier:
   `verify_v2_package.py` negative probes (G9 rows), which assert the four
   DELETED_HEADERS absent from tree and prefix and compile the 15
   uninstalled detail headers against the prefix with pinned diagnostics.
5. liburing pair chain: manual -> #520/F-10 campaign records (dated). No
   current obligation cites them; the liburing config-identity behavior
   they validated is owned by `xmake/uring.lua` (SLUICE_LIBURING_PROFILE)
   and covered by the build matrix.
6. `verify_formal.sh` chain: manual -> Lean corpus (removed with it) ->
   FCB1 campaign verdicts (dated, `docs/archive/formal/fcb1/`). No v2
   obligation (ledger G8 historical row).
7. `verify_tla.sh` retired stages chain: manual -> retired TLA+ corpus
   (removed with them) -> v1 stage verdicts (Git history). Active stages
   retained: RequestCore/ObserverCore/ShutdownCore gates.
8. `verify_c2e_progress.sh` chain: renamed, not removed; obligation intact.
9. `lefthook.yml` chain: hook -> `scripts/gates/pre-push.sh` (missing since
   `5f62b55b`) -> v1-era validators (all deleted). Edge already dead;
   removal deletes a nominal reference only. CI remains the mechanical
   gate authority.
10. `pages.yml` chain: push to `visuals/sluice-v1/**` -> Pages deploy of the
    v1 walkthrough (non-normative, superseded architecture). No v2
    obligation; README links removed. The already-deployed site is
    unaffected until repository settings change.

## Docs and corpora

- `docs/review/**` (260 files): consumers were the M-R guard (removed), the
  F0 inventory script (removed), `test_grep_matrix_runner.py` (removed) and
  one app-README link (rewritten to in-README validation). No ledger row or
  root clause cites `docs/review/` by path.
- 21 `docs/evidence/` campaign dirs + 10 loose records: no active-tree
  references outside other dated records (checked by per-dir `git grep` at
  base). The five dirs cited by active rows/tooling are retained unmodified:
  `pr-b-boundary-contraction-20261016` (G9, RG1, verifier, package README),
  `issue-524-requestcore-convergence-20261015` (G2/G8),
  `f6-final-correspondence-20261014` (G1 SEM-06),
  `f2-authority-contraction-20261013` (G8 71/72 record),
  `g0-403-performance-baseline-20261015` (bench reference baseline).
  Known cosmetic residue inside kept frozen records: three textual pointers
  to now-deleted sibling records (f6 -> docs/review/f1-matrix,
  f6 -> f3-postmerge-closeout, issue-524 -> f6-final-closeout); left
  unmodified under the no-silent-rewrite rule.
- `docs/roadmap/phase-m-f-live-handoff-20261012.md`: only `docs/README.md`
  listed it (index updated). `b1-1-request-core-evidence.md` retained —
  `formal/tla/RequestCore.tla` and kept issue-524 records cite it.
- Retired formal corpora (124 TLA+ files, 15 Lean files): ledger G8 row
  records the removal; Git history `c001bb1e`/`9845cee1` retains them.

## Test fixtures

- `tests/support/direct_pread_shim.c`: consumers were frozen evidence
  scripts only; no xmake target compiled it.
- `tests/test_grep_matrix_runner.py`: manual-only; its subject
  (`docs/review/f3-grep-evidence-20261011/run-matrix.sh`) is dated evidence
  removed in the same round.
