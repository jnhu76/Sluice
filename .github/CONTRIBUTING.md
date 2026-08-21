# Contributing to Sluice

Sluice is an experimental C++20 I/O/control-flow library with deliberately strict architecture, correctness, and evidence requirements. Contributions are welcome, but correctness-sensitive work should be narrow, reviewable, and tied to an explicit issue or accepted design authority.

## Before opening an issue

1. Search open and closed issues first.
2. Use the issue form that best matches the work:
   - bug / correctness failure;
   - architecture or design proposal;
   - audit / research finding;
   - performance investigation.
3. For a bug, provide the exact revision, environment, observed behavior, and the strongest reproducer/evidence available.
4. For concurrency/liveness bugs, prefer a deterministic causal schedule or stable state barrier over timing-only stress.
5. A model defect is not automatically a C++ defect. Classify evidence honestly.
6. A design smell or research idea does not authorize implementation by itself.

Newly filed issues enter **TRIAGE** until a maintainer classifies their evidence, scope, and lifecycle. Filing an issue does not by itself authorize implementation.

See [ISSUE_LIFECYCLE.md](ISSUE_LIFECYCLE.md) for tracker states and defer/reopen rules.

## Issue lifecycle

Open issues are reserved for triage, actionable work, or intentional tracking:

- **TRIAGE** — newly filed / not yet classified; no implementation authority yet;
- **ACTIVE** — currently executing or explicitly selected;
- **READY / EXPERIMENT** — bounded evidence/design work is allowed, but production modification may still be gated;
- **ON-TOUCH** — gradual maintenance ledger; no big-bang cleanup;
- **UMBRELLA / TRACKING** — decomposition/governance only; focused children carry implementation;
- **DEFERRED** — closed with GitHub state reason `not planned`, with a concrete reopen trigger;
- **COMPLETED** — closed with GitHub state reason `completed` only when accepted scope is actually delivered.

Do not keep speculative or policy-blocked work permanently open merely as a reminder.

## Branches and commits

Use a focused branch from current `master`. Suggested prefixes:

```text
fix/<issue>-<slug>
feat/<issue>-<slug>
refactor/<issue>-<slug>
test/<issue>-<slug>
formal/<issue>-<slug>
perf/<issue>-<slug>
docs/<issue>-<slug>
chore/<slug>
```

Keep commits causally separable where practical. For correctness work, a good shape is:

```text
1. deterministic regression / evidence
2. minimal production repair
3. as-built formal model / negative controls (if applicable)
4. documentation / compliance evidence
5. performance evidence (if hot path changed)
```

Avoid unrelated formatting, cleanup, or opportunistic refactors in a correctness PR.

## Correctness workflow

Follow `AGENTS.md` and the accepted ADRs. In particular:

- reproduce or establish the defect before broad repair;
- add a regression that can fail on pre-fix code when feasible;
- make the smallest production change consistent with the accepted architecture;
- run focused evidence first, then the complete applicable gates;
- never mask a failure with retries, sleeps, weakened assertions, skipped targets, or warning-only gates.

For C++ ↔ TLA+ work, the repository doctrine is C++-first:

```text
real C++ failure / concrete C++ hypothesis
        -> deterministic C++ evidence
        -> TLA+ exploration / falsification
        -> minimal C++ repair
        -> pre-fix RED / post-fix GREEN
        -> as-built model of repaired C++
        -> pre-fix mutant CEX / repaired model PASS
```

A green TLC run alone is not evidence that the C++ implementation is formally verified.

## Pull requests

Every PR should:

- link its governing issue or explain why no issue is needed;
- state exactly what changed and what did not;
- identify architecture impact;
- provide exact commands/results actually executed;
- distinguish deterministic evidence from stress/environment observations;
- record performance A/B evidence for scheduler/atomic/hot-path changes when practical;
- list residual risks and re-track meaningful residual scope before using a closing keyword.

Use `.github/pull_request_template.md` as the review contract.

## Architecture-sensitive changes

Changes involving async I/O ownership, request lifecycle, Completion publication, cancellation, scheduler wake/progress, synchronization primitives, runtime ownership, resource bounds, shutdown/drain, or io_uring ownership require the architecture compliance process in `AGENTS.md §8` before production implementation.

Unknown authority, wake, failure, or capacity semantics block implementation.

## Tests and evidence

The minimum Linux Clang Debug baseline is documented in `AGENTS.md`. Additional gates depend on change class and may include:

- focused deterministic regression;
- Release;
- ASan/UBSan;
- TSan;
- real-liburing tests;
- negative compile gates;
- formal positive and negative-control suites;
- repository mechanical/pre-push gates;
- performance attribution/benchmark evidence.

Report only commands that were actually executed.

## Residuals and closing issues

A merged PR must not silently erase residual work. Before closing an issue:

- verify the accepted scope is complete;
- move genuine residual scope to a named follow-up issue if needed;
- use `completed` only for delivered scope;
- use `not planned` for deliberate defer/rejection with a reopen trigger;
- preserve historical issue text rather than rewriting the past to match the present.
