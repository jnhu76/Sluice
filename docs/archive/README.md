# Sluice documentation archive

These records describe earlier architecture authorities, implementation
snapshots, research campaigns, or conformance ledgers.

They are retained for provenance and evidence only.

They do not override:

```text
docs/explicit-io-v1-final-decision.md     sole normative root for the v1 target
docs/roadmap/v1-conformance.md            current implementation/evidence ledger
```

No archived clause is incorporated by reference into the v1 target. If a rule
must govern v1, it appears in the root specification. Nothing in this archive
certifies v1 conformance, architecture ownership, or a current work item.
Historical CLOSED/CONFORMANT/PROVED labels are scoped to the baseline and
contract each record examined; Git history preserves the exact text and the
decision trail for every file below.

Old paths referenced inside archived bodies (for example `docs/roadmap/…` or
`docs/formal/…`) are the wording of the time; the location table below is the
current one. Markdown links inside archived files have been repointed so they
still resolve; historical prose was not rewritten.

## Location map (old path → current path)

| Old path | Current path |
|---|---|
| `docs/mission.md` | `docs/archive/architecture/mission.md` |
| `docs/architecture.md` | `docs/archive/architecture/architecture.md` |
| `docs/assets/sluice-architecture.svg` | `docs/archive/architecture/sluice-architecture.svg` |
| `docs/audit/code-reality-audit.md` | `docs/archive/audits/code-reality-audit.md` |
| `docs/audit/io-architecture-gap-1.md` | `docs/archive/audits/io-architecture-gap-1.md` |
| `docs/roadmap/explicit-file-conformance.md` | `docs/archive/explicit-file-roadmap/explicit-file-conformance.md` |
| `docs/roadmap/explicit-file-app-consumer-census.md` | `docs/archive/explicit-file-roadmap/explicit-file-app-consumer-census.md` |
| `docs/roadmap/explicit-file-legacy-surface-audit.md` | `docs/archive/explicit-file-roadmap/explicit-file-legacy-surface-audit.md` |
| `docs/roadmap/explicit-file-legacy-surface-audit-final-review.md` | `docs/archive/explicit-file-roadmap/explicit-file-legacy-surface-audit-final-review.md` |
| `docs/roadmap/explicit-file-vectored-decision.md` | `docs/archive/explicit-file-roadmap/explicit-file-vectored-decision.md` |
| `docs/formal/*.md` (campaign reports) | `docs/archive/formal/fcb1/*.md` |

`docs/adr/0001-*` and `docs/adr/0002-*` were deliberately left in place: their
Accepted → Superseded state is itself provenance, and each carries a
supersession banner. They are historical rationale only.

## Provenance index

| Path | Original title / role | Baseline | Original status | Tracking | Superseded by |
|---|---|---|---|---|---|
| `architecture/mission.md` | Sluice 宗旨 — mission and six long-term principles | pre-ADR research baseline (no commit pin in file) | FROZEN (historical) | — | v1 root PROD-01 / ARCH-01 |
| `architecture/architecture.md` | Sluice 当前架构快照 — dated implementation view | `253fabe79c425ad5473e1bf4efc30896023762bc` | Verified snapshot at that baseline | old conformance roadmap | v1 root (target view) |
| `architecture/sluice-architecture.svg` | Sluice Architecture — overview diagram | ADR-0002 / mission era | historical asset | — | root ARCH-01 views + README Mermaid view |
| `audits/code-reality-audit.md` | Code-First Architecture Reality Audit (SLUICE-CODE-FIRST-ARCHITECTURE-REALITY-AUDIT-1) | `7f5a3f59515911fabc3c41a3846b0bad3bef1688` (audit 2026-09-13) | `ARCHITECTURE_MATCH_WITH_CORRECTIVES` against ADR-0002 | issues #373 / #374 | v1 root GOV-03 (dated evidence; claims require source/scope checks) |
| `audits/io-architecture-gap-1.md` | SLUICE-IO-ARCH-GAP-AUDIT-1 — capability/gap audit | `baa6c91ce240b0890bfb3e6c12e917ba619be700` (PR #324 era) | FROZEN, verdict `IO_ARCH_GAP_AUDIT_COMPLETE` | PR #324 chain | ADR-0002-era decisions; v1 root |
| `explicit-file-roadmap/explicit-file-conformance.md` | Explicit File Architecture Conformance Roadmap | `d7511349990cbb3ef340e158c7816fe3296b0575` | superseded ledger (CLOSED/CONFORMANT scoped to ADR-0002) | old roadmap A1–A8 | v1 root GOV-03; `roadmap/v1-conformance.md` |
| `explicit-file-roadmap/explicit-file-app-consumer-census.md` | Explicit File App Consumer Census | `d7511349990cbb3ef340e158c7816fe3296b0575` | census record (non-authority by its own terms) | old roadmap A2 / issue #342 | v1 root; consumer audit re-owned by #402 |
| `explicit-file-roadmap/explicit-file-legacy-surface-audit.md` | Explicit File Legacy Surface Audit (§14 dispositions) | `ff916c37c6bab0f9a2bc7555a19bd8a2080af639` | 29 DELETE / 1 CONVERGE | issue #355 | v1 root MIG-02 / #402 |
| `explicit-file-roadmap/explicit-file-legacy-surface-audit-final-review.md` | Final human review addendum to the legacy surface audit | same proof root; PR #368 review | `PASS_WITH_2_MINOR_CORRECTIVES` | issue #355 | v1 root MIG-02 / #402 |
| `explicit-file-roadmap/explicit-file-vectored-decision.md` | Explicit File Vectored Operation Decision | `d9ae9fde0e78bd626ff708533b57f995a7ca984c` | evidence-gated KEEP/not-carried decision under ADR-0002 §5.3 | issue #355 | v1 root PROD-03 / SEM-01 (vectored stays outside required v1) |
| `formal/fcb1/campaign-verdict.md` | FCB1-POST-V23-STACK-379-385 — campaign verdict | FCB1 stack, Lean 4.33.1 | campaign synthesis record | issue #375 (PR #385) | v1 root GOV-03; formal evidence map owned by #391 |
| `formal/fcb1/stage-0-v2-base-calculus.md` | Stage 0-V2 — Frozen Base Calculus and Observation Model | FCB1 repair stack (PRs #376–#385) | FROZEN (V2.3) | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-1-v2-event.md` | Stage 1V2.2 — Event Core | FCB1 stack | stage card, re-adjudicated | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-2-v2-semaphore.md` | Stage 2V2.3 — Semaphore core | FCB1 stack (#378) | stage card, replayed | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-3-v2-mutex.md` | Stage 3V2.3 — AsyncMutex core | FCB1 stack (#379) | stage card, replayed | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-4-v2-condition.md` | Stage 4V2.3 — AsyncCondition core | FCB1 stack (#380) | stage card, replayed | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-5-v2-rwlock.md` | Stage 5V2.3 — AsyncRwLock core | FCB1 stack (#381) | stage card, replayed | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-6-v2-queue.md` | Stage 6V2.3 — AsyncQueue core | FCB1 stack (#382) | stage card, replayed | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-7-v2-select.md` | Stage 7V2.3 — Select core | FCB1 stack (#383) | stage card, replayed | issue #375 | #391 evidence reset |
| `formal/fcb1/stage-8-v2-driver.md` | Stage 8V2.3 — Scheduler driver | FCB1 stack (#384) | stage card, `RESEARCH/DEFER` | issue #375 | #391 evidence reset |

Notes:

- The Lean/TLA+ **sources** these stage cards describe live in `formal/` at the
  repository root and are deliberately not archived here. Their v1 disposition
  is owned by issue #391.
- The issue #387 code-first architecture reconstruction report was never
  committed to the repository; its authoritative copy is issue #387 itself. The
  baseline reconstruction facts it records are dated evidence for
  `c64f005e6e59e791f26a7ab4a594c33954f096dd`.
- Baseline SHAs above are the ones recorded in each file's own header; where a
  file states no SHA, the row says so rather than inventing one.
