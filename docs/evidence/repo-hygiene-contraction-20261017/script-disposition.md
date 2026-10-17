# Script disposition ledger

Zero-based audit of `scripts/` at base `9845cee1` (17 scripts). Consumers
were checked across CI workflows, xmake, other scripts, tests, app READMEs
and active authority docs, separately from references inside frozen dated
records. "Impact" = what deletion loses today; Git history retains every
removed file (base `9845cee1`, v1-campaign records also at `c001bb1e`).

| Script | Purpose | Callers | Unique oracle / obligation | Disposition | Removal impact |
|---|---|---|---|---|---|
| `check_cpp_comment_authority.py` | comment-hygiene scan over C/C++ comments | CI `ci.yml` | only live guard for the AGENTS comment rule | KEEP | loses the standing CI gate |
| `test_check_cpp_comment_authority.py` | its discriminator suite | CI `ci.yml` | scanner self-test | KEEP | — |
| `verify_v2_package.py` | v2 clean-room install-surface authority (11-header boundary, negative probes, ODR) | documented entry (tests/package/README.md); ledger G9/RG3 evidence producer | the Appendix B boundary | KEEP (seam allowlist follows the request_scope macro rename) | loses the G9 boundary gate |
| `test_verify_v2_package.py` | its 14-test discriminator suite | CI `ci.yml` | verifier self-test | KEEP | — |
| `verify_tla.sh` | TLC gate for RequestCore/ObserverCore/ShutdownCore | documented entry (docs/README.md); ledger G8 re-run rule | the three C++-mirrored models | KEEP, shrunk from 11 model stages to 3 (retired-primitive stages removed with their corpus) | loses the G8 model gate |
| `verify_c2e_progress.sh` | TLC gate for ProgressSource (21 configurations) | none automated; ledger G8/G4 re-run obligation | the fourth active model | RENAME_AND_KEEP -> `verify_progress_source_model.sh` | loses the ProgressSource gate |
| `f0_surface_inventory.py` | include-graph census printout | none (frozen docs/review records only) | none current | RETIRE | nothing in build/CI/release |
| `capture_f3_sanitizer_instrumentation.py` | nm capture from a preserved F3-era build root (asserts exactly 18 binaries) | none (frozen records) | none current; cannot run against HEAD shapes | RETIRE | nothing |
| `verify_f3_tail_owned_driver.sh` | rebuilds pinned SHA `143e8d45`, never HEAD | one frozen review record | none current | RETIRE | nothing |
| `verify_f4_retirement.py` | pre-v2 25-header retirement experiment driver; docstring declares SUPERSEDED; `--matrix` broken on HEAD (inputs removed with PR B) | `verify_f4_postmerge.py`, `tests/package/f4_retirement/*` (all retired together) | superseded by `verify_v2_package.py` negative probes | RETIRE | in-tree F4 evidence re-verification (Git history keeps it) |
| `verify_f4_postmerge.py` | one-shot post-merge check of the archived F4 campaign at pinned historical SHAs | `tests/package/f4_retirement/test_postmerge.py` (retired) | none current | RETIRE | nothing |
| `verify_m_final_adoption.py` | v1 Phase M adoption verifier; fails at HEAD (reads the deleted v1 root; pre-v2 drift pins) | none (frozen records) | superseded by v2 adoption records | RETIRE | nothing |
| `verify_mr_disposition.py` + `test_verify_mr_disposition.py` | consistency of the closed M-R disposition overlay | `mr-disposition-guard.yml` (removed with them) | the M-R campaign closed per `docs/review/m-phase-final-closeout.json`; no v2 row/root/gate reads those JSONs | RETIRE with the workflow | one CI job protecting a closed dated record set |
| `verify_liburing_config_switch.py` | #520 liburing n<->y stale-object regression driver | none (dated records) | completed 2026-10-14 campaign; ledger rows cite xmake targets and the v2 verifier instead | RETIRE | reproduce path of a dated record |
| `verify_liburing_identity_binding.py` | F-10 probe-vs-production identity-binding oracle | none (dated records) | completed campaign; partial overlap with the above | RETIRE | reproduce path of a dated record |

Result: 17 -> 6 script files (`check_cpp_comment_authority.py`,
`test_check_cpp_comment_authority.py`, `verify_v2_package.py`,
`test_verify_v2_package.py`, `verify_tla.sh`,
`verify_progress_source_model.sh`). Every retained
script is invoked by CI, documented as an entry point in `docs/README.md` or
`tests/package/README.md`, or named by a ledger row.

CI: `.github/workflows/mr-disposition-guard.yml` removed (job exit tied to a
closed campaign); `ci.yml`, `open-code-review.yml` unchanged;
`pages.yml` removed with `visuals/sluice-v1/` (its only payload).
`lefthook.yml` removed: its gate script `scripts/gates/pre-push.sh` had been
deleted in `5f62b55b`, so the hook was nominal-only. Developers who installed
the hook run `lefthook uninstall` once.
