# History — Non-authoritative Records

This directory contains documents that are **not current authority**. They are preserved for historical traceability, closeout evidence, and implementation records.

## Contents

### `closeout/` — Phase closeout evidence

Documents that record the completion of a phase, job, or corrective. These contain implementation details, review outcomes, and test evidence that was used to close a phase.

Includes:
- E10–E12 phase closeout documents
- E13 preparation and rollback closeout
- Sync runtime closeout evidence
- Async runtime corrective evidence
- Review records (from `docs/reviews/`)

### `implementation-plans/` — Planning documents

Documents that record task planning, job cards, and phase roadmaps that are now obsolete because the work is complete or superseded.

Includes:
- Async next job cards
- Sync I/O next job cards
- E12 sync primitives plan
- Zig migration job cards
- Design documents from v0.1-mvp phase

### `superseded/` — Superseded documents

Documents whose semantic content has been superseded by a later accepted decision.

## Banner

Every historical document in this directory carries a banner:

> **Historical note:**
> This document records the state at `<phase/commit>`.
> It is not the current contract.
> See `<current document>` for current authority.

## Rule

Historical documents retain original technical claims unless those claims are dangerously ambiguous. The body is not rewritten to match the current implementation. When ambiguity exists, a banner is added — the body is not changed.