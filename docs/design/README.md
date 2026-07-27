# Proposed Designs

This directory indexes design documents that are **not yet approved** for implementation.

## Design status rules

A design document is active only when:
- **Status: Proposed** — under review, not yet binding
- **Status: Accepted and awaiting implementation** — approved but not started

All other documents are historical or closeout records (see `docs/history/`).

## Current proposed designs

| Design | Owner | Status | Blocking decisions | Related ADR |
|--------|-------|--------|-------------------|-------------|
| [E12-F AsyncRwLock](e12-rwlock.md) | Async runtime | Proposed | E12-F design review | ADR-execution-model |
| [E13 Select](./e13-select-production-architecture.md) | Async runtime | Proposed | E13 design authorization | ADR-execution-model |
| [E14 Evented Parity](e14-threaded-evented-parity-preparation.md) | Async runtime | Proposed | E14 design authorization | ADR-execution-model |

## Not yet proposed

- E16 Application Runtime — not yet discussed or approved. Do not present as accepted.

## Navigation

- **Accepted, implemented** — see `docs/architecture/`
- **Historical design docs** — `docs/history/implementation-plans/`
- **Active ADRs** — `docs/adr/README.md`