# Naming migration map

Columns: old path/symbol | new path/symbol | semantic basis | consumers
migrated. "Historical references" below means frozen records in Git history
(`docs/evidence/**`, `docs/review/**` at base `9845cee1`); they are not
rewritten and keep the old spellings valid at their pinned commits.

## Files

| Old | New | Basis | Consumers migrated |
|---|---|---|---|
| `tests/backend_conformance_e1_test.cpp` | `tests/backend_contract_conformance_test.cpp` | cross-backend File-contract conformance suite (its own header claim) | xmake/tests.lua target sources; apps/sluice-grep/README.md |
| `tests/uring_core_cutover_test.cpp` | `tests/uring_request_core_integration_test.cpp` | verifies the io_uring backend's integration with the request core (identity, cancel, observer, reclaim) | xmake/tests.lua |
| `tests/threadpool_core_cutover_test.cpp` | `tests/threadpool_request_core_integration_test.cpp` | same integration property on the ThreadPool backend | xmake/tests.lua |
| `tests/f3_preflight_cli_oracle.py` | `tests/cli_preflight_rejection_oracle.py` | verifies CLI preflight rejection before any target-file open (strace-observed) | new documented entries in apps/sluice-tail/README.md, apps/sluice-copy/README.md |
| `scripts/verify_c2e_progress.sh` | `scripts/verify_progress_source_model.sh` | sole runner of the ProgressSource TLA+ model (ledger G8/G4) | ledger G8 row |

## Xmake targets and helpers

| Old | New | Basis |
|---|---|---|
| `backend_conformance_e1_threadpool_test` / `_uring_test` | `backend_contract_conformance_threadpool_test` / `_uring_test` | the suite's contract-conformance subject, per profile |
| `e1_conformance_mut_<mechanism>` (10) | `backend_contract_conformance_mut_<mechanism>` | mutant of the conformance suite; mechanism suffix kept |
| `uring_cutover_mut_<mechanism>` (8) | `uring_core_integration_mut_<mechanism>` | mutant of the uring core-integration suite |
| `c2b_progress_target` / `c2c_uring_progress_target` (helpers) | `threadpool_progress_target` / `uring_progress_target` | the backend each helper wires |

## Mutant defines

| Old prefix | New prefix | Basis |
|---|---|---|
| `SLUICE_E1_MUTANT_*` (conformance set, 10) | `SLUICE_BACKEND_CONTRACT_MUTANT_*` | conformance mechanism injected |
| `SLUICE_E1_CONFORMANCE_URING` | `SLUICE_BACKEND_CONTRACT_CONFORMANCE_URING` | profile selector of the conformance suite |
| `SLUICE_E1_MUTANT_OVERFLOW_ABSENCE_SETTLES_INFLIGHT` | `SLUICE_URING_PROGRESS_MUTANT_OVERFLOW_ABSENCE_SETTLES_INFLIGHT` | consumed by the uring progress-race suite |
| `SLUICE_E1_MUTANT_POST_POISON_FLUSH_GATED` | `SLUICE_URING_EXTERNAL_LOOP_MUTANT_POST_POISON_FLUSH_GATED` | consumed by the uring external-loop suite |
| `SLUICE_B1C_MUTANT_{TRANSPORT_RETRY_UNADVERTISED,OVERFLOW_FLUSH_IGNORED}` | `SLUICE_URING_PROGRESS_MUTANT_*` | uring progress-race mutants |
| `SLUICE_B1C_MUTANT_HOST_IGNORES_DISPATCH_RETRY` | `SLUICE_URING_EXTERNAL_LOOP_MUTANT_HOST_IGNORES_DISPATCH_RETRY` | uring external-loop mutant |
| `SLUICE_B1C_MUTANT_*` (8 core-integration mechanisms) | `SLUICE_URING_CORE_MUTANT_*` | uring core-integration mutants |
| `SLUICE_C1_MUTANT_*` (5) | `SLUICE_REQUEST_CORE_MUTANT_*` | request-core observer-phase mutants |
| `SLUICE_C2E_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS` | `SLUICE_PROGRESS_SOURCE_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS` | progress-source close-admission mutant |
| `SLUICE_E2_MUTANT_M*` (12) | `SLUICE_SHUTDOWN_MUTANT_M*` | shutdown mutants; the ADR-0004 M-numbers are preserved |
| `SLUICE_B2_MUTANT_*` (6) | `SLUICE_PUBLIC_REQUEST_MUTANT_*` | public Request binding-discipline mutants |
| `SLUICE_D1_MUTANT_*` (6) | `SLUICE_REQUEST_SCOPE_MUTANT_*` | request-scope settlement mutants; disambiguates from root owner-decision D1 |

Consumers migrated for defines: `xmake/tests.lua` (definitions), the
injection sites in `src/async/**` and `include/sluice/async/request_scope.hpp`,
`scripts/verify_v2_package.py` `INSTALLED_MUTANT_SEAM_ALLOWLIST` (the two
installed request_scope template seams) and the counter-example fixture in
`scripts/test_verify_v2_package.py`.

## Unchanged on purpose (current contract vocabulary)

- W-01/W-02/W-03 workload names: `bench_w0{1,2,3}_*.cpp`,
  `tests/direct_w01_consumer_probe.cpp`,
  `tests/package/consumers/w0{1,2,3}_*.cpp` (the root's appendix names the
  probe and clean-room consumers), the `w03-handoff` phase tags.
- Requirement-ID mutant prefixes `SLUICE_ERR01_*`, `SLUICE_THREAD01_*`,
  `SLUICE_LIFE_*`; ledger groups G1-G10, release gates RG1-RG7, owner
  decisions D1-D3, V-numbers.
- ADR-0004 file name and its M1-M12 mutant numbering (adopted decision
  vocabulary).

## In-file content tags

`"sluice e1 ..."`, `"sluice b1b ..."`, `"sluice b1c ..."`,
`"sluice c1f ..."` temp-file content tags became
`"sluice backend ..."`, `"sluice threadpool core ..."`,
`"sluice uring core ..."`; `/tmp/sluice_{e1_conformance,e1_...,b1b_cutover,
b1c_cutover,e2_shutdown,e2_death}_XXXXXX` templates became
`/tmp/sluice_{backend_contract,uring_core,threadpool_core,shutdown_lifecycle,
shutdown_death}_XXXXXX`. One read-back assertion updated with the tag:
`memcmp(buffer, "sluice b", 8)` -> `"sluice u", 8` (commit `9387af2e`).

## Review-round additions (2026-10-17)

The independent review swept wider token sets; the following moved in the
fix commits, same mechanical class (no assertion or oracle change):

- Test-side `/tmp` templates: `/tmp/sluice_{b1a_ownership,b2_request,
  b2_violation,d1_scope,tail_e4,tail_e4_cli,tail_e4_child}_XXXXXX` ->
  `/tmp/sluice_{core_ownership,public_request,public_violation,
  request_scope,tail_lifecycle,tail_cli,tail_child}_XXXXXX`;
  `/tmp/sluice_f1_{cce,cshut,cadm,cprog,crl,cdur,w01,w02,w03}_XXXXXX` ->
  the same suffixes without the `f1_` token.
- `tests/package/consumers/negative_async_symbols_in_core.cpp`:
  `sluice_f1_negative_use_async` -> `sluice_negative_use_async` (the gate
  is the failing link, not the symbol name).
- Build-graph and gate-script prose: catalogue comments in
  `xmake/tests.lua` and stage-labeled echo headers in
  `scripts/verify_tla.sh` reworded to behavior descriptions; residual
  "D1/B2/F1/PR B" phrasing in `scripts/verify_v2_package.py` and
  duplicated campaign phrasing in `scripts/verify_progress_source_model.sh`
  collapsed. Pointers to dated campaign records (Git history) remain the
  traceability pattern.

## Bench tooling

`G0_{WARMUP,MEASURED,JSONL,BIN_DIR,CASE_FILTER,TMP_DIR,DISK_DIR,SKIP_DISK,
DIAGNOSTICS,REFERENCE_DIR}` -> `SLUICE_BENCH_*`; `/tmp/sluice-g0-bench` ->
`/tmp/sluice-bench`, `/tmp/g0prof` -> `/tmp/sluice-bench-prof`. The
`--stage measured|diagnostics|all` matrix parameter names a run phase of the
matrix itself and stays. The g0-403 baseline evidence directory remains the
default comparison reference.
