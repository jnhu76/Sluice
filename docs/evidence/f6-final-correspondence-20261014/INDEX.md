# F6 Final Evidence Correspondence Audit — 2026-10-14

| Field | Value |
|---|---|
| Task | F6_FINAL_EVIDENCE_CORRESPONDENCE_AUDIT |
| Mode | READ_ONLY_CODE_AND_FORMAL / EVIDENCE_OUTPUT_ONLY |
| Normative root | `docs/explicit-io-v1-final-decision.md`, revision **v1-r4** (adopted via PR #488 at `d4973100a31dccaa098cf4227f84e6c9c0afdee7`) |
| Tracking ledger | `docs/roadmap/v1-conformance.md` |
| Umbrella issue | #402 (OPEN throughout F6; closed only by the final owner) |
| Frozen production content SHA | `821c489fb081308d5ef95b13a14cc6435ac78d41` |
| F6 entry commit (this audit's baseline) | `fdb4b743c8f24bdcc4ec446c4bc7efbd386e61c3` (docs-only on top of the frozen content) |
| Deliverables head | recorded in `final-receipt.md` at completion |

## Freeze & Provenance — R0

### Frozen tree relationship (verified 2026-10-14)

- `git rev-parse HEAD` = `fdb4b743…` = the F6 entry commit recorded in the ledger's
  "2026-10-14 F6 entry" section.
- `git diff --name-status 821c489f..fdb4b743` = exactly one file,
  `docs/roadmap/v1-conformance.md` (+12 lines, the F6 entry row). **No
  production, build, test, model or historical-evidence file differs between the
  frozen content SHA and the audit baseline.** `821c489f` is an ancestor of HEAD.
- Working tree clean at audit start; all F6 outputs are new files under this
  directory only.

### Phase F terminal states, adoption basis, merge SHAs and acceptance evidence

| Phase | Final state | Owner decision / adoption basis | Merge SHA (master) | Post-merge / acceptance evidence |
|---|---|---|---|---|
| Phase M (prereq) | CLOSED/ADOPTED | #458; terminal packet PR #491 (merge `22606ae2`, reviewed head `aabca28f`) | `22606ae2` | #491 independent APPROVE + post-merge guard; ledger "Phase M terminal decision" section |
| E (prereq #401) | CLOSED/ADOPTED | PR #453, human final review APPROVE; ADR-0004 ADOPTED | `eb7bf055ad4fce5540de0f876609ab38c45e385b` | Ledger SHUT row VERIFIED (E2 record; V20–V23/V26, formal safety+liveness, 171-run TLA gate attempt 2 PASS) |
| F2 authority contraction | **CLOSED** | Owner `ACCEPT_BREAKAGE_NOW` on the installed-surface break (`installed-surface-risk-disposition.md`), recorded at decision head `acbd1a73`; CI `38402996519` | `27a5041a17ed02e8a2c9396fedfa34600fb35378` (PR #516) | Post-merge re-verification: merged tree byte-identical to `acbd1a73`; CI `38403388160` 4/4. Evidence: `docs/evidence/f2-authority-contraction-20261013/` (gates.json, final-closeout-receipt.md) |
| F3 app/host closeout | **CLOSED** | Active human CLI approval bound to PR #513 HEAD `350e7898`; #474 scoped technical acceptance | `f4a9723410539025cffa3422c5f6841ef795c755` (PR #513); closeout #515 | `docs/evidence/f3-postmerge-closeout-20261013` — four app/package configs, dual native-Clang sanitizers, 4 CI jobs |
| F4 legacy retirement | **ADOPTED_SCOPED** | #514/#506 (legacy File/stream native authority + BlockingIoPool P2 retirement) | `de551ed09555b8ad4d118a250baf373062a01d0e` (PR #514) | #505, #506 CLOSED; fresh four-profile recheck + independent technical acceptance (2026-10-12 overlay) |
| F5 final package contraction | **CLOSED** | Owner `ACCEPT_BREAKAGE_NOW` + scoped #518 merge authorization at branch tip `5ef4f801` | `f99acbc3a64cf4eae356ef39b99d88e89183f579` (PR #518) | Post-merge B-01 v7 capture + installed probes on actual master `573d616c` (`postmerge-closeout-20261014.md`): 25 headers / 13 async members / 15 objects, removed_present 0, W-01–W-03 consumers run |
| pre-F6 infra (#519/#520/#521) | merged; #519/#520/#521 CLOSED as FIXED_VERIFIED | PR #522 (same-dir ring-switch safety, Clang D1 mutation build, RequestScope TSan link) + F-10 dependency-identity correction | `573d616cc1ee79ec7a3b3876351799708237db3a` (PR #522) | Every gate re-executed green on the real post-merge master (`postmerge/` in `pre-f6-infra-stabilization-20261014/`); round-4 evidence-integrity re-check CLEAN |

### Current issue states (verified via GitHub API 2026-10-14)

OPEN: #402 (this audit's umbrella; stays OPEN pending final owner acceptance),
#390 (v1 roadmap; tracks through release). CLOSED: #516/#518/#522 (PRs),
#505, #506, #474, #458, #463, #475, #519, #520, #521.

### Authority order (v1-r4, effective)

1. Root `docs/explicit-io-v1-final-decision.md` v1-r4 — sole normative authority.
2. Adopted ADRs deriving from it: **ADR-0003** (ADOPTED, DESIGN_AUTHORITY_ONLY —
   no supported-profile claim), **ADR-0004** (ADOPTED via PR #453). ADR-0001 and
   ADR-0002 carry explicit `HISTORICAL / SUPERSEDED` banners and retain no
   normative force.
3. Ledger `docs/roadmap/v1-conformance.md` — tracking only, cannot relax.
4. Everything else (README, issues, evidence dirs, research) — navigation or
   dated evidence.

### Candidate / adopted / merged / post-merge-verified distinction applied in this audit

Only content that is **merged to master and post-merge verified** is treated as
current state. Specifically:

- F2/F3/F5 closure receipts are dated post-merge records bound to actual master
  SHAs — current. The F5 candidate README above its closeout addendum stays a
  pre-merge record and is not used as current state.
- The pre-F6 stabilization slice's `IMPLEMENTED_VERIFIED` status is upgraded to
  current by its `postmerge/` receipts at `573d616c`.
- Old issue bodies (including #402's CURRENT overlay), old ledger overlays and
  Phase M-era sections are audit **inputs** only; where they conflict with the
  receipts above, the receipts govern (see findings in `findings.md` for the one
  such conflict found: the #402 CURRENT overlay staleness).
- Historical TLA/Lean corpus per `docs/roadmap/v1-formal-evidence-map.md`
  baseline dispositions (no `REUSE`) — historical, never auto-current.

## Deliverables in this directory

| File | Round | Content |
|---|---|---|
| `INDEX.md` | R0 | This file: freeze/provenance, authority order |
| `requirements-matrix.tsv` | R1 | Exhaustive correspondence: 113 root identifiers (71 clause IDs across 22 families + S1–S10 + L1–L5 + V01–V27), 14 columns, forward + reverse mapping |
| `authority-dag.md` | R2 | Authority/lifetime DAG reconstructed from production source + 9 adversarial risk probes + retirement inventory |
| `formal-assets.tsv` | R3 | Every formal asset (26 rows) with retention / claim-eligibility / proof-scope axes |
| `package-and-claims.md` | R4+R5 | Package/consumer contract audit + current claim integrity |
| `findings.md` | R6 | Adversarial findings with the mandated field set (18 findings; P0=0, P1=0) |
| `gates.json` | Gates | Final hard-gate evaluation (all #402 gates with per-gate basis) |
| `independent-review.md` | Review | Independent adversarial review pass (13 attacks on this audit) |
| `final-receipt.md` | Receipt | Completion receipt and final technical verdict |
| `raw/` | all | Clean-room consumer sources + results (`cleanroom-consumers.txt`, `w0{1,2,3}_*.cpp`), TLA run results (`tla-results.txt`) |

## Re-verification executed by this audit (frozen content, 2026-10-14)

- Debug `--liburing=y` (real ring): full suite **66/66 passed**.
- Debug `--liburing=n` (honest stub): full suite **52/52 passed**.
- Archive/object counts on fresh debug builds: `libsluice_core.a` 2 members,
  `libsluice_async.a` 13 members (= 15 library objects); installed-header glob
  arithmetic 9 (core side) + 16 (async side, `detail/context_identity.hpp`
  de-installed) = **25** — matches the R4 baseline table.
- Clean-room installed-prefix consumers (W-01/W-02/W-03) compile, link and run
  against `/tmp/f6-prefix` only, zero non-system deps (`raw/cleanroom-consumers.txt`).
- TLA+ safety re-verification at the frozen tree, pinned tla2tools 1.7.4
  (checksum verified): ObserverCore clean (5,123 states, depth 16),
  ProgressSourceSafety clean (21,784 states, depth 18), ShutdownCore clean
  (47,521 states, depth 21), **RequestCore.cfg complete** (152,240,323+
  states generated, 26,566,666+ distinct, depth 53, 8m09s, no errors).
  RequestCoreLive.cfg fresh run in flight at audit close — supplementary;
  E2-dated receipt carries REQ-06/L1 (finding F6-F04). `raw/tla-results.txt`.
- Raw logs: `raw/`.
