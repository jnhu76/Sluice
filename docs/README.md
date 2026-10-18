# Sluice documentation

This page is the navigation entry for everything under `docs/`. It is a map,
not a specification: it states where authority lives and what each record is
for. It adds no requirement, contract, or architecture decision, and it is not
an authority document. Only the root specification is normative.

## Current authority

```text
Target architecture / contract        docs/explicit-io-contract.md     (sole normative root, v2-r1)
Implementation conformance            docs/roadmap/v2-conformance.md
Repository working rules              AGENTS.md
```

- **[v2 Architecture and Contract](explicit-io-contract.md)** (revision
  v2-r1) owns Sluice product scope, public behavior, responsibility
  boundaries, lifetime rules, and acceptance obligations. Every other
  document is derived from it or is evidence about a particular commit. No
  other document is a normative authority, and none stands as a peer of the
  root. The revision became the sole normative root when its adopting PR
  merged (GOV-05); that same PR superseded and removed the v1 root, the v1
  conformance ledger and the v1 formal evidence map from the active tree —
  Git history retains them.
- **[v2 conformance ledger](roadmap/v2-conformance.md)** tracks which v2
  obligations have recorded evidence, and the release-gate status. Adopting
  the target does not certify the code; nothing in
  this repository is VERIFIED without a recorded evidence entry there.

## Reading order

```text
1. README.md                                     product overview
2. docs/README.md                                this map
3. docs/explicit-io-contract.md                  binding target contract (v2-r1)
4. docs/roadmap/v2-conformance.md                implementation/evidence status
5. C++ / tests / TLA+ / kernel evidence        what a commit actually does
```

C++ headers and implementation establish current behavior at a specific commit;
a mismatch with the root is a conformance gap, not a license to change the
contract. A behavior change to the target must amend the root first, or in the
same PR.

## Current documents

| Path | Purpose |
|---|---|
| `docs/explicit-io-contract.md` | Sole normative root (v2-r1) |
| `docs/roadmap/v2-conformance.md` | Implementation/evidence ledger for v2 |
| `docs/performance/methodology.md` | Performance-measurement methodology (living rules for reproducible claims) |
| `docs/README.md` | This navigation page |

These four are the continuously maintained records under `docs/`. Everything
else under `docs/` is a retained historical record or a dated evidence record
kept for provenance: `docs/adr/` holds the adopted and superseded ADRs,
`docs/archive/` the archived records, and `docs/evidence/` only the dated
evidence records that active ledger rows or tooling still cite by path
(`v2-release-acceptance-20261017/`, `repo-hygiene-contraction-20261017/`,
`issue-528-release-doc-closeout-20261018/`, `pr-b-boundary-contraction-20261016/`,
`issue-524-requestcore-convergence-20261015/`,
`f6-final-correspondence-20261014/`, `f2-authority-contraction-20261013/`,
`g0-403-performance-baseline-20261015/`). Older campaign records — the Phase
M/F campaign, `docs/review/`, the F0/F3/F4/F5 experiments and the retired
formal corpora — live in Git history (base commit `9845cee1` and earlier)
rather than in the tree; the ledger's carried rows cite them there.

`research/RESULTS.md` (repository root, outside `docs/`) holds retained research
conclusions; it is evidence and rationale, not an authority.

## Adopted ADRs

| Path | Purpose |
|---|---|
| `docs/adr/0004-e2-shutdown-state-contract.md` | ADOPTED via PR #453 — subordinate implementation decision (E2 shutdown spelling); its state contract is restated in the root's section 17 |

ADR-0004 derives from the root and adds no scope. ADR-0003 (narrow stackful
I/O host) was superseded and retired from the active tree by the v2 adoption
(owner decision D1: the host bundle is outside v2 and is physically removed by
PR B); its Git history and the root's GOV-03 table record the
supersession, and it grants no surviving design or implementation authority.

## Historical ADRs

| Path | Purpose |
|---|---|
| `docs/adr/0001-explicit-io-design-doctrine.md` | Historical design rationale and decision provenance |
| `docs/adr/0002-explicit-file-api-architecture.md` | Historical architecture rationale; superseded as authority |
| `docs/adr/0002-explicit-file-api-architecture-views.md` | Historical visual companion to ADR-0002; ADR-era views, not the v2 target |

They stay in place rather than being archived because the root specification's
GOV-03 table and `AGENTS.md` name them as rationale or dated evidence, and
because their Accepted → Superseded state is itself provenance. Each carries a
supersession banner naming the root. Where a historical body still says
"normative", "frozen", "authority", or "must", that wording is scoped by the
banner to the decision the ADR originally recorded; it is not a current
constraint, an implementation roadmap, or a normative summary of v2.

## Archived evidence

`docs/archive/` holds records that describe earlier architecture authorities,
superseded implementation snapshots, research campaigns, or old conformance
ledgers. See the [archive index](archive/README.md) for the per-file provenance
table (original title, baseline, status, tracking issue, superseded-by) and the
old-path → current-path location map.

`docs/archive/README.md` is that index; it is a navigation record, not one
of the archived historical records itself.

`docs/archive/architecture/sluice-architecture.svg` depicts the superseded
ADR-0002-era architecture (Reader/Writer world plus Scheduler/Fiber runtime). It
is archived for provenance. The current target view is the informative Mermaid
diagram in `README.md`, which mirrors the root specification's ARCH/PROG
sections; if those ever differ, the root wins.

## Formal reports vs formal source

Two different things share the word "formal":

- **`docs/archive/formal/fcb1/`** — historical FCB1 campaign reports for the
  superseded async-primitive architecture. Historical evidence only.
- **`formal/tla/` (repository root)** — the four living TLA+ models
  (RequestCore, ObserverCore, ProgressSource, ShutdownCore) and their `.cfg`
  instances. `scripts/verify_tla.sh` gates the first three;
  `scripts/verify_progress_source_model.sh` gates ProgressSource. The
  living-model scope, evidence pointers and coverage limitations are owned by
  the root's VERIFY-06 and the [v2 conformance ledger](roadmap/v2-conformance.md)
  (group G8). The retired synchronization-primitive TLA+ corpus and the Lean
  FCB1 corpus are superseded history in Git history.

Formal models prove properties of the model, not ownership of an architecture.
Architecture selects proof obligations; proofs protect architecture; proofs do
not select architecture.

## Provenance rules

```text
archive != current authority
superseded ADR != current contract
historical PASS != v2 conformance
formal theorem != architecture ownership
```

An archived or superseded CLOSED/CONFORMANT/PROVED label describes the baseline
and contract it was written against. It does not transfer to the v2 target and
cannot be cited as v2 evidence.
