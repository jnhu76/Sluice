# Sluice documentation

This page is the navigation entry for everything under `docs/`. It is a map,
not a specification: it states where authority lives and what each record is
for. It adds no requirement, contract, or architecture decision.

## Current authority

```text
Target architecture / contract        docs/explicit-io-v1-final-decision.md   (sole normative root)
Implementation conformance            docs/roadmap/v1-conformance.md
Execution roadmap                     GitHub issue #390 → tickets #391–#403
Repository working rules              AGENTS.md
```

- **[v1 Architecture and Contract Reference](explicit-io-v1-final-decision.md)**
  owns v1 product scope, public behavior, responsibility boundaries, lifetime
  rules, and acceptance obligations. Every other document is derived from it or
  is evidence about a particular commit.
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
| `docs/README.md` | This navigation page |

These three are the only current records under `docs/`. Everything else under
`docs/` is either a retained historical record or a retired one kept for
provenance.

`research/RESULTS.md` (repository root, outside `docs/`) holds retained research
conclusions; it is evidence and rationale, not an authority.

## Retained historical records

| Path | Purpose |
|---|---|
| `docs/adr/0001-*, 0002-*` | Historical rationale, retained in place with supersession banners; not current authority |
| `docs/archive/**` | Archived provenance and evidence; see the [archive index](archive/README.md) |

Neither is current authority. The ADRs stay in place because the root
specification and AGENTS.md cite them by path; archived records were moved
because their location did not carry authority.

## Archived material

`docs/archive/` holds records that describe earlier architecture authorities,
superseded implementation snapshots, research campaigns, or old conformance
ledgers. See the [archive index](archive/README.md) for the per-file provenance
table (original title, baseline, status, tracking issue, superseded-by).

```text
archive != current authority
superseded ADR != current contract
historical PASS != v1 conformance
formal theorem != architecture ownership
```

An archived CLOSED/CONFORMANT/PROVED label describes the baseline and contract
it was written against. It does not transfer to the v1 target and cannot be
cited as v1 evidence.

The root specification's supersession table (GOV-03) names each superseded
record at its current archive path and records the former path inline, so the
root is readable on its own. The archive index additionally carries the full
old-path → current-path table for every moved record.

## Formal material: reports vs sources

Two different things share the word "formal":

- **`docs/archive/formal/fcb1/`** — historical FCB1 campaign reports for the
  superseded async-primitive architecture. Historical evidence only. The
  current formal evidence map is being rebuilt by issue #391.
- **`formal/` (repository root)** — Lean and TLA+ sources, `.cfg` instances and
  `scripts/verify_formal.sh` / `scripts/verify_tla.sh`. These remain repository
  formal source evidence; their v1 disposition (REUSE / ADAPT / HISTORICAL /
  RETIRE_FROM_V1_EVIDENCE) is owned by #391, not by this index.

Formal models prove properties of the model, not ownership of an architecture.
Architecture selects proof obligations; proofs protect architecture; proofs do
not select architecture.

## Assets

`docs/archive/architecture/sluice-architecture.svg` depicts the superseded
ADR-0002-era architecture (Reader/Writer world plus Scheduler/Fiber runtime). It
is archived for provenance. The current target view is the informative Mermaid
diagram in `README.md`, which mirrors the root specification's ARCH/HOST/PROG
sections; if those ever differ, the root wins.
