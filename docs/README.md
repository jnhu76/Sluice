# Sluice documentation

This page is the navigation entry for everything under `docs/`. It is a map,
not a specification: it states where authority lives and what each record is
for. It adds no requirement, contract, or architecture decision, and it is not
an authority document. Only the root specification is normative.

## Current authority

```text
Target architecture / contract        docs/explicit-io-v1-final-decision.md   (sole normative root, v1-r3)
Implementation conformance            docs/roadmap/v1-conformance.md
Execution roadmap                     GitHub issue #390 → tickets #391–#403
Repository working rules              AGENTS.md
```

- **[v1 Architecture and Contract Reference](explicit-io-v1-final-decision.md)**
  owns v1 product scope, public behavior, responsibility boundaries, lifetime
  rules, and acceptance obligations. Every other document is derived from it or
  is evidence about a particular commit. No other document is a normative
  authority, and none stands as a peer of the root.
- **[v1 conformance ledger](roadmap/v1-conformance.md)** tracks which
  requirements have evidence and which do not. Adopting the target does not
  certify the code; nothing in this repository is VERIFIED without a recorded
  evidence entry there.
- **[Issue #390](https://github.com/jnhu76/Sluice/issues/390)** is the v1
  convergence roadmap. Tickets #391–#403 are the execution slices; they carry
  the current work, gates, and ownership for gaps found by audits.

## Reading order

```text
1. README.md                                     product overview
2. docs/README.md                                this map
3. docs/explicit-io-v1-final-decision.md         binding target contract
4. Issue #390                                    roadmap and responsibility map
5. the relevant execution ticket (#391–#403)     slice scope and acceptance
6. docs/roadmap/v1-conformance.md                implementation/evidence status
7. C++ / tests / TLA+ / Lean / kernel evidence   what a commit actually does
```

C++ headers and implementation establish current behavior at a specific commit;
a mismatch with the root is a conformance gap, not a license to change the
contract. A behavior change to the target must amend the root first, or in the
same PR.

## Current documents

| Path | Purpose |
|---|---|
| `docs/explicit-io-v1-final-decision.md` | Sole normative root (v1-r3) |
| `docs/roadmap/v1-conformance.md` | Implementation/evidence ledger |
| `docs/roadmap/v1-formal-evidence-map.md` | Non-normative formal evidence map (issue #391) |
| `docs/README.md` | This navigation page |

These four are the only current records under `docs/`. Everything else under
`docs/` is a retained historical record or a retired one kept for provenance.

`research/RESULTS.md` (repository root, outside `docs/`) holds retained research
conclusions; it is evidence and rationale, not an authority.

## Historical ADRs

| Path | Purpose |
|---|---|
| `docs/adr/0001-explicit-io-design-doctrine.md` | Historical design rationale and decision provenance |
| `docs/adr/0002-explicit-file-api-architecture.md` | Historical architecture rationale; superseded as v1 authority |
| `docs/adr/0002-explicit-file-api-architecture-views.md` | Historical visual companion to ADR-0002; ADR-era views, not the v1 target |

They stay in place rather than being archived because the root specification's
GOV-03 table and `AGENTS.md` name them as rationale or dated evidence, and
because their Accepted → Superseded state is itself provenance. Each carries a
supersession banner naming the root. Where a historical body still says
"normative", "frozen", "authority", or "must", that wording is scoped by the
banner to the decision the ADR originally recorded; it is not a current
constraint, an implementation roadmap, or a normative summary of v1.

## Archived evidence

`docs/archive/` holds records that describe earlier architecture authorities,
superseded implementation snapshots, research campaigns, or old conformance
ledgers. See the [archive index](archive/README.md) for the per-file provenance
table (original title, baseline, status, tracking issue, superseded-by) and the
old-path → current-path location map.

`docs/archive/README.md` is that index; it is a new navigation record, not one
of the archived historical records itself.

The root specification's supersession table (GOV-03) names each superseded
record at its current archive path and records the former path inline, so the
root is readable on its own.

`docs/archive/architecture/sluice-architecture.svg` depicts the superseded
ADR-0002-era architecture (Reader/Writer world plus Scheduler/Fiber runtime). It
is archived for provenance. The current target view is the informative Mermaid
diagram in `README.md`, which mirrors the root specification's ARCH/HOST/PROG
sections; if those ever differ, the root wins.

## Formal reports vs formal source

Two different things share the word "formal":

- **`docs/archive/formal/fcb1/`** — historical FCB1 campaign reports for the
  superseded async-primitive architecture. Historical evidence only. The
  [v1 formal evidence map](roadmap/v1-formal-evidence-map.md) (issue #391)
  assigns every existing formal asset its disposition and owns future formal
  obligations.
- **`formal/` (repository root)** — Lean and TLA+ sources, `.cfg` instances and
  `scripts/verify_formal.sh` / `scripts/verify_tla.sh`. These remain repository
  formal source evidence; their v1 disposition (REUSE / ADAPT / HISTORICAL /
  RETIRE_FROM_V1_EVIDENCE) is owned by #391, not by this index.

Formal models prove properties of the model, not ownership of an architecture.
Architecture selects proof obligations; proofs protect architecture; proofs do
not select architecture.

## Provenance rules

```text
archive != current authority
superseded ADR != current contract
historical PASS != v1 conformance
formal theorem != architecture ownership
```

An archived CLOSED/CONFORMANT/PROVED label describes the baseline and contract
it was written against. It does not transfer to the v1 target and cannot be
cited as v1 evidence.
