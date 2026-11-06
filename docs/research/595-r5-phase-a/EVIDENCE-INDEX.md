# #595 R5 Phase A — Evidence Index

Index of every R5 Phase-A artifact under `/home/jnhu/sluice-595-e5/r5/`
(excluding the two worktrees and the `build/` output tree, which hold code,
not evidence). sha256 values computed 2026-11-07 by the synthesis writer with
`sha256sum` (command run over the files listed below; output transcribed).
Baseline tree: R4_ORIGINAL `f3b25329bc8a64a201961c426a58adacfb8fdf08`;
cleanup branch `research-595-r5` @ `b191bb893432c4df0551d24c85843d8513ed5a55`
(C1 `c55165a219a67058c97941c60ae520d1b1b45996` + C2
`b191bb893432c4df0551d24c85843d8513ed5a55`); diag branch
`research-595-r5-diag` @ `21374a40` (per-op tail trace instrument). Main repo
`/home/jnhu/Source/Sluice` stayed clean on `master` @ `271b81bf` throughout;
`r4/` was read-only; GitHub was accessed read-only (no push/PR/label/comment).

## 1. Final Phase-A deliverables (this round's outputs)

| File | sha256 | Content |
|---|---|---|
| `CALL_PATTERNS.md` | `fd93b8c02e3920e5362db9166904c3a65bceb247901bd9fc8be1ccc9914045cf` | G1–G5 execution graphs (merged from both tracers), harmonized edge conventions, node index `f3b25329:file:function:line`, findings marked on figures, G5 p99 candidate DAG with PROVEN/CORRELATION/NO-MEASUREMENT edge grades; arm glossary §1.0 and frozen↔C2 identifier mapping §0.1 |
| `CODE_FINDINGS_AND_FIXES.md` | `22e2a44b7f1442a42270a7a68fb67b69a3785424c969f6db5e0cc57ba43a8d4c` | C1–C7 verdicts, fixed/clarified/candidate classification (incl. Candidate A row), equivalence evidence summary |
| `TAIL_ANALYSIS.md` | `f2c1fab3c908012334749bca9e6b9709ed34982baa12146fead8e6ec90102b2c` | Evidence inventory (R4 + fresh re-analyses + executed diagnostic), corrected literature matrix, open p99 sources, R4 failure classification |
| `NEXT_EXPERIMENT.md` | `39aa96bd260d369741fc640601305f678a6f3b062d2a0b502208c7656e5291b6` | Candidates A/B/C field-by-field (B: primary signal, `B_ops` default, suffix lifecycle, outer-loop change points), recommendation, Phase-B execution contract (design only) |
| `EVIDENCE-INDEX.md` | this file | index |

## 2. Analysis inputs (`r5/analysis/`)

| File | sha256 | Producing role |
|---|---|---|
| `analysis/env-context.md` | `4d1b66597b66f81b191b66ca1f7dd80ca675b97b28c82a4f41f0659923f78b0a` | environment/anchor verifier — native E5 facts, worktree table, anchor checks (PR 629, commit OIDs, diff sha256, byte-identical archived patch), R4 digest 21/21 confirmed, anomalies A1 (lock path), A2 (TAIL-LATENCY.md branch-only), A3 (issue 595 labels []) |
| `analysis/call-patterns-submit.md` | `fc1db4443bb2c9722832f3b71baee6e02b27575db9d6cb2fc460dd0c966e31b9` | submission-side tracer — G1/G2/G3 material, lock inventory, control-point judgment |
| `analysis/call-patterns-complete.md` | `790abd8b9ae0f0b473d64bf28143a514f59b94bcacf0c06fe743b76502d4f392` | completion/observation-side tracer — completion chain, G4 material, W02/W03 contrast, blob provenance vs R4 delta logs |
| `analysis/findings-c1-c4.md` | `752bfef69dad838be57034f103818ad590b3d86be433407965c70c67dd5fd866` | code-finding verifier C1–C4 (all CONFIRMED; C1 equivalence PROVEN) |
| `analysis/findings-c5-c7.md` | `3313f9fd79015d7d818ff9e8bf917424894fd07993d9474430a440a8a561781f` | code-issue verifier C5–C7 (all CONFIRMED) |
| `analysis/literature-matrix.md` | `cf9e4aad1421e5dcca7f584e4faaf149512b596b9a276b59f083c314cf4070be` | p99 literature-mechanism analyst — 11-row matrix (IX/ZygOS/Shinjuku/Shenango/Perséphone/NAPI), provenance §4 |
| `analysis/tail-evidence-review.md` | `ac48f93b58753f4c487df2a00b12ba563a2dc0ade490d260218a18a06164ea70` | p99 evidence inventory — R4 artifact levels, event partial order, fresh re-analyses, six hypotheses, REQUIRED_INFORMATION_GAP |
| `analysis/diagnostic-protocol.md` | `322eccb1e7ddd68f805a2166b04cee7a89a1f8d0846e2771d81ac84ea73bc0d5` | frozen (pre-collection) tail phase-decomposition protocol: 7 timestamps, arms A0/R, d16+d4, on/off perturbation audit, lock discipline, reporting obligations |
| `analysis/diagnostic-results.md` | `541693e4f69252f3c221e365765ba3662e44f83174f93f24ff718710e3e97ec4` | executed diagnostic report: coverage 480,000/480,000; on/off FAILED (+3.39/+3.96pp > ±3pp) ⇒ perturbed leads only; interval tables; tail membership; d4 sign flip reproduced |
| `analysis/corrections/C7-1-sign-convention.md` | `832517b7d2c5e393615942292981bdde9261cf078cb8a5c0343225fd4bf52639` | R−B orientation correction |
| `analysis/corrections/C7-2-figure-data-columns.md` | `446d1f1634421f030ec3f571113874837dd8bb967f0d6de61cf3e576b5282029` | figure-data index/unit correction record |
| `analysis/corrections/figure_data_normalize.py` | `db9a76c69a38a624e17033c7aa412dc11c9bf2da303824be8740c015b2c44026` | normalizer (read-only input) |
| `analysis/corrections/figure-data.normalized.csv` | `64ce3b463bcabad64bbaa491d55926801739a4eb4a25769282a219c6d4982b17` | adds source_cell/contrast/dataset/n_unit; values byte-identical to frozen CSV (run output rows=370 cell_corrections=42 arm_column_disambiguations=69) |
| `analysis/corrections/C7-3-shadow-oracle-protocol.md` | `73b2f30a8c88da8b7f64cc48d36db9590b4e7811f0231345d83d771dd25c8086` | shape-aware shadow oracle for R5+ (STRUCTURAL_ZERO class) |
| `analysis/corrections/C7-4-fprot-wording.md` | `7d1c286802f4129aa1b1f01c0adf76d4ab0c266e1913778f90977ff9978a39d4` | 39-FAIL wording rules |
| `analysis/corrections/C7-5-w02-d3-record.md` | `d3e1e8a61c0bb551f9629aa9a1f4b667234d00459378dd92d0696baf9d21d1d1` | W02-d3 harness-gap record + backfill assessment |
| `analysis/corrections/C7-6-resource-wording.md` | `992afab2ca1cd888fe852c284d27f37c6cf61db675b8210eb8059463ac319054` | resource-evidence wording rules |

## 3. Fix evidence (`r5/evidence/`)

| File | sha256 | Content |
|---|---|---|
| `evidence/cleanup-c1.patch` | `4ecbe2b6526e7e533c67c4eee08ee39f974300377533436e89b1222618373865` | C1 fix commit `c55165a2` (bounded eligible-prefix scan, 1 file) |
| `evidence/cleanup-c2.patch` | `39e9cf4e75e6d78122b6b502b95cfa6df4d11890351e2f40e3506dbacfb10bf6` | C2 fix commit `b191bb89` (owner-pass activity rename, 3 files, predicate unchanged) |
| `evidence/equivalence-c1-c2.md` | `78fd4414c78e4a0a1bedd9f59bf3e9f4bddacf1a834bf19c1cfa06646b853e61` | implementer's method + results: 18-scenario deterministic transport oracle 3 stages × 18/18 rc=0 byte-identical; in-repo suites table; W03 per-arm smoke + A/B scheduling-nondeterminism study |

## 4. Raw diagnostics (`r5/raw-diag/`)

| Path | Content |
|---|---|
| `raw-diag/LOCK_RECEIPT.md` (sha256 `792bb7343b29907fd07acd7986d5a98c8e66b3fb30e910fd30a338ff7b9ee0d6`) | 10 lock sessions 2026-11-06T15:54Z–16:04Z on `/home/jnhu/sluice-595-e5/MEASUREMENT.lock`: 9 sessions labeled `r5diag-A-build-smoke` (acquire at 15:54:34, 15:59:04, 15:59:20, 16:00:21, 16:01:11, 16:01:31, 16:01:59, 16:02:14, 16:03:10Z) + 1 session `r5diag-B-matrix` (16:04:26Z): UTC stamps, fuser, second-fd probes (all rc=1 = blocked OK), pre/post state JSON; two receipt defects honestly recorded (in-wrapper post-release probe self-blocked — clean release verified externally rc=0; `cpu_busy_100ms` rendered {} due to a sampler key bug) |
| `raw-diag/matrix/manifest.json` (`f83657e787414ba66c6ab1a188870210c59a7ba0d339f356f0d7f625387fb2d8`) | per-run manifest: binary sha256 `78a228f267005a8f9638f0249a1f0fc1ae01e599f4a029968c0a8d8fdba4a3ba`, 26 invocations (off-pair + 24 on-mode), env, lat summaries, "diagnostic-only, never mixed with clean blocks" |
| `raw-diag/matrix/off-W03-uring-4k-d16/p00-{0-A0,1-R}/` | off-mode perturbation-reference pair (bench.jsonl/log/err/attempt.json) |
| `raw-diag/matrix/on-W03-uring-4k-d16/pNN-pos-ARM/` (NN∈{00..05} = block, pos∈{0,1} = position in the interleaved A0→R / R→A0 ordering, ARM∈{A0,R} alternating with pos — 6 block×ordering combos × 2 arms = 12 runs; NOT a 6×2×2 expansion) | 12 on-mode d16 runs (each with `dump/tail-00{0,1}-*.bin` arena dumps; tail-001 = measured instrumented phase) |
| `raw-diag/matrix/on-W03-uring-4k-d4/…` (same 12-run layout) | 12 on-mode d4 control runs |
| `raw-diag/onmode-trace-analysis.json` (`c10101b6dcdbfe02e3b4028f3fef2ee7e75ae62ecdfb27913594689a87a91a59`) | per-file coverage/counters (480,000 records, 0 join failures, 0 overflow) |
| `raw-diag/aggregate-summary.json` (`bb7a47109479e842f013ad1f1859ce94ce1df8fffe02a12086cc963b0f962bbd`) | pooled/run-level interval percentiles, tail membership, presentation histograms, on/off check + verdict |
| `raw-diag/scripts/` | `lock_session.sh`, `run_diag_matrix.py`, `analyze_tail_trace.py`, `aggregate_tail_trace.py` (session drivers/analyzers for the diagnostic; one-shot tooling kept with the evidence it produced) |
| `raw-diag/smoke/` | build-smoke A0/R outputs incl. measured `.bin` dumps and analysis |
| `raw-diag/equiv/{baseline,c1,c2}/` | 64 files: per-suite outputs (18-scenario oracle full + per-scenario rc files, r4_arm_smoke, deterministic/research/fault suites, concurrency/cancel/shutdown regressions, W03 r3d16cap per-arm outputs), the two same-binary rerun proofs (`shutdown_lifecycle_test.rerun2.out`, `uring_request_core_integration_test.rerun2.out`), the A/B repeat study log (`c1/w03_ab_repeat_study.log`), and the C2 harness adaptation diff (`c2/harness_c2name_adaptation.diff`) |

## 5. Archived-from-branch (`r5/r4-archive/`)

| File | sha256 | Content |
|---|---|---|
| `r4-archive/TAIL-LATENCY.md` | `315e2cda137045f12d2b3e8e34995bcb0e27613a953b55ba188cc6071a72a9f3` | R4 TAIL-LATENCY.md, absent from local `r4/`, extracted (git show) from branch `research/595-r4-state-aware`; the only item copied from that branch |

## 6. Verification performed by the synthesis writer (this session)

- Spot re-verification of the tracers'/verifiers' key citations in the frozen
  tree `f3b25329` (via `git show f3b25329:<path>`): adapter loop `r3:442-576`
  (scan at `:448`, outstanding read at `:469`, pressure at `:474-476`,
  present_len at `:497`, cap_reuse/chunk_continue at `:561-567`, suffix
  singles at `:578-590`); kTable rows `r3:222-245`;
  `bench_w03:396-424` (refill/facts/submit), `:482-504` (A0), `:522-572`
  (park/drain); driver `bench_w03:182-264`; `poll()` structure
  `uring_backend.cpp:839-912`. All as cited — no discrepancy found.
- Cross-check of the two tracers vs the two verifiers on overlapping claims
  (control point, outstanding() semantics, snapshot staleness, lock counts,
  W02/W03 divergence, d2serial structural zero): consistent; notation
  conventions harmonized (CALL_PATTERNS.md §0); no factual conflict to
  adjudicate. R−B orientation mixing in the literature matrix corrected
  (TAIL_ANALYSIS.md §2 COR-1, verified against TAIL-LATENCY §5 and
  RESULTS.md:188/394 — same comparison, different denominators).
- Mermaid structural self-check of `CALL_PATTERNS.md` (no mermaid CLI on the
  host: `mmdc` absent; `npx @mermaid-js/mermaid-cli` not installed — an
  install would require a network download of a Chromium-bundled package and
  was not attempted). Command: `python3 /tmp/r5_mermaid_check.py
  /home/jnhu/sluice-595-e5/r5/CALL_PATTERNS.md` → `7 mermaid blocks / PASS:
  quotes balanced; {[( )]} brackets balanced outside quotes;
  subgraph/par-alt-loop constructs closed; edge-label pipes balanced`
  (exit 0). A follow-up character scan flagged two lines containing `;` —
  confirmed to be the terminating semicolons of the intentional `&gt;` HTML
  entities inside quoted labels, not statement separators. This is a syntax
  structure check only, not a renderer-verified layout.
- sha256 computations for this index (§1–§5); patch hashes match the values
  declared in `evidence/equivalence-c1-c2.md`.
- Main-repo cleanliness re-checked at synthesis end: `git -C
  /home/jnhu/Source/Sluice status --porcelain` empty; branch `master` @
  `271b81bf` (behind origin by 2; no fetch/pull/push performed).
- **Revision pass (2026-11-07, after independent review):** all 27 review
  items were fixed in the four deliverables (edge direction and cv-wake
  convention in G1; A0-path consistency G1/G2.1; signal-enumeration and
  identifier-mapping fixes; predicate parenthesization; completed line
  ranges; group-size/enter-count caliber unified across documents with the
  `max_refill` facts; Candidate A classified in the SUMMARY table;
  block↔attempt mapping; corrected wake-percentage arithmetic; five-interval
  additive sums for d16/d4 with unrounded values; instrument-inflation
  derivation; pair-dimension wording; corpus/denominator scoping for the
  SHADOW diag and G5 E18; COR-1 aggregation statement with the
  non-interconvertibility demonstration; lead-level wording; missing R4
  line references; counterfactual-semantic fix for Candidate A; Candidate B
  primary signal/read-point/`B_ops` default and suffix-lifecycle/outer-loop
  change points; full-gate rejection conditions; matrix-path glob and
  session-count fixes; arm-B/Candidate-B/`B_ops` disambiguation; arm
  glossary; stats-lock caveat on A0). Verification facts added during this
  pass were themselves source-checked: `bench_w03:337-339` (adapter not
  constructed for `a0`), `r3:29-39,160-181` (arm taxonomy),
  `bench_w03:384-385,842-859` (max_refill semantics and case list),
  `bench_w02` function extent (`run_pipeline_phase_r3:468-712`, next
  definition `fill_stats` at `:714`), `note_outstanding` call sites
  (`uring_backend.cpp:459-465,495,610`; absent from `submit_request`
  `:735-757`), `uring_backend.cpp:794-798,914-927` (zero-op/admission-close
  signals), `aggregate-summary.json` d4 tail-band means, and
  `r4/raw/bsupp/B-SUPP-W03-D16/DESCRIPTIVE.json` (`paired_diff_B_minus_R_pct`
  container values 9.0363/32.1672/14.5017/8.2123). The mermaid structural
  check was re-run after the edits: 7 blocks, PASS (exit 0).

## 7. Known gaps in this evidence set

- The tail diagnostic's formal attribution is blocked by the failed on/off
  window; only one off-mode pair exists (`raw-diag/matrix/off-…`). See
  TAIL_ANALYSIS.md §3 U1–U6.
- No PMU/IRQ attribution was possible (`perf_event_paranoid=2`), and kernel
  instants (SQE consumption, CQE write) are unavailable by construction.
- The R5 measurement-lock path differs from R4's (env-context §8 A1);
  receipts in `raw-diag/LOCK_RECEIPT.md` use the R5 top-level path per the
  round constraint. Whether the r3 lock must also be held for cross-round
  exclusion remains an open policy question for the round owner.
- Phase-B experiments were designed only (NEXT_EXPERIMENT.md); nothing was
  executed; no product code was merged; GitHub was not mutated.
