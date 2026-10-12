# F0 §1.7 External Risk Adjudication: #506 BlockingIoPool

Candidate: #506 BlockingIoPool physical retirement
PR branch: codex/f4-506-pool
Head: `f0647a705f8d522394fdb1fdd86a9e258d93d6ea`
Base: `f3521b45ead3963162e518758ac1820004adcfc2` (origin/master, post-#509)
Status: **CANDIDATE — pending owner approval**

## Scope

This document adjudicates each deleted public surface. It does not approve the deletion; the final F0 §1.7 sign-off remains with the human owner.

## Interface 1: `sluice/blocking_io_pool.hpp`

| Field | Assessment |
|-------|------------|
| **Symbol / type summary** | `sluice::BlockingIoPool` — a bounded generic worker task pool. Public class offering `submit`, `try_submit`, `wait_idle`, `shutdown`, worker-count/depth queries and a `Task<T>::get()` result cell; this pool is not a File I/O operation interface. |
| **v0.0.1 public presence** | Present in tag `v0.0.1` and documented in the v0.0.1 README as part of `sluice_core`. |
| **Current in-tree consumers** | None after #506. No production, application, test, or template reference remains. |
| **Canonical equivalent or migration** | No equivalent replacement in v1. The canonical blocking surface is `sluice::blocking::read_at` / `write_at` in `sluice/blocking/file.hpp`, but it is not a drop-in replacement for the pool abstraction. Consumers must migrate to direct per-operation blocking calls or to the async runtime. |
| **Breakage impact** | Compile-time failure for any external consumer including `<sluice/blocking_io_pool.hpp>`. Link-time or binary-compatibility breakage for external consumers of the exported pool constructor, methods, `make_blocking_io_pool`, or `detail::enqueue_job` definitions formerly supplied through `libsluice_core.a`; no callable replacement is guaranteed. |
| **Recommended disposition** | Accept deletion under F0 §1.7 because (a) the surface is not exercised by v1 canonical workloads W-01/W-02/W-03, (b) no in-tree consumer remains, and (c) the retained `sluice/blocking/file.hpp` surface covers the ordinary-utility blocking I/O use case. |
| **Owner approval** | ☐ Pending |

## Interface 2: `sluice/detail/blocking_io_pool_impl.hpp`

| Field | Assessment |
|-------|------------|
| **Symbol / type summary** | Implementation-detail header for `BlockingIoPool`: worker state, task wrapper, and internal queue structures. Located under `detail/`, but installed by the package rule. |
| **v0.0.1 public presence** | Present in tag `v0.0.1` as an installed header. |
| **Current in-tree consumers** | None after #506. |
| **Canonical equivalent or migration** | No replacement; this is an implementation substrate of the retired pool. |
| **Breakage impact** | Compile-time failure for consumers including the detail header directly. Typical external usage is through `blocking_io_pool.hpp`, so this deletion is usually co-broken with Interface 1. |
| **Recommended disposition** | Accept deletion together with Interface 1. The `detail/` path signals implementation instability, but because it was installed in v0.0.1 it still requires explicit F0 §1.7 authorization. |
| **Owner approval** | ☐ Pending |

## Summary

| Deleted surface | v0.0.1 public | In-tree consumer | Equivalent | Recommended action |
|-----------------|---------------|------------------|------------|--------------------|
| `sluice/blocking_io_pool.hpp` | Yes | None | None (manual migration) | Delete with F0 §1.7 approval |
| `sluice/detail/blocking_io_pool_impl.hpp` | Yes (installed detail) | None | None | Delete with F0 §1.7 approval |

## Evidence references

- B-01 package evidence: `docs/evidence/b01-506-blocking-io-pool-retirement.md`
- Raw evidence on host: `/tmp/b01-evidence/pr-508`
- Negative/positive probes: `/tmp/b01-evidence/pr-508-probes`, `/tmp/b01-evidence/pr-508-probes-uring`
