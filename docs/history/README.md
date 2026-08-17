# History — Non-authoritative Records

This directory contains documents that are **not current authority**. They are preserved for historical traceability, closeout evidence, and implementation records.

## Contents

### `application-designs/` — Superseded application designs

Application-workload designs that were not implemented and were superseded by
a later application round.

### `archive/` — Early records

v0.1-mvp-era closeouts, checklists, and point-in-time audits.

### `closeout/` — Phase closeout evidence

Documents that record the completion of a phase, job, or corrective. These contain implementation details, review outcomes, and test evidence that was used to close a phase.

Includes:
- E10–E12 phase closeout documents
- E13 preparation and rollback closeout
- Sync runtime closeout evidence
- Async runtime corrective evidence
- Review records (from `docs/history/reviews/`)

### `implementation-plans/` — Planning documents

Documents that record task planning, job cards, and phase roadmaps that are now obsolete because the work is complete or superseded.

Includes:
- Async next job cards
- Sync I/O next job cards
- E12 sync primitives plan
- Zig migration job cards
- Design documents from v0.1-mvp phase
- Implemented designs moved out of `docs/design/` (e.g. the E16 application
  runtime design, the M1-A await API horse race)

### `issues/` — Closed issue records

Diagnostic and corrective records for closed issues. Live investigations live
under `docs/investigations/`.

## Banner

Every historical document in this directory carries a banner:

> **Historical note:**
> This document records the state at `<phase/commit>`.
> It is not the current contract.
> See `<current document>` for current authority.

## Rule

Historical documents retain original technical claims unless those claims are dangerously ambiguous. The body is not rewritten to match the current implementation. When ambiguity exists, a banner is added — the body is not changed.