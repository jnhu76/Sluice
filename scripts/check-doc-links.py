#!/usr/bin/env python3
"""check-doc-links.py — repository documentation link validator.

Checks:
  - Git-tracked Markdown: root README files (README.md, README.zh-CN.md),
    AGENTS.md, and every tracked Markdown under docs/. Generated/gitignored
    Markdown is excluded so the local and CI scan sets are identical.
  - Public-header doc references: tracked include/sluice/**/*.hpp comments are
    scanned for explicit repo-relative `docs/...` tokens. Broken or
    known-moved targets fail unless grandfathered per site in
    HEADER_STALE_ALLOWLIST (the #167 Step 4 backlog registry). Added after
    stale header pointers escaped the Markdown-only scan surface — the
    KNOWN_MOVED remap table never saw them because headers were never
    scanned (PR #207 review finding 1).
  - Markdown links [text](path) resolve to existing files/directories.
  - Backtick-quoted repository-relative paths (`path/to/file`) resolve.
  - Fails on broken links and stale moved-path references.
  - Reports historical paths separately but still fails unless explicitly allowlisted.
  - Skips generated TLA+ state artifacts.
  - Returns non-zero on every actionable problem.

Path resolution convention in this repository:
  - Markdown links [text](path) are resolved STRICTLY DOC-RELATIVE (matching
    GitHub rendering): `docs/foo.md` in `docs/history/closeout/e12-event.md`
    resolves to `docs/history/closeout/docs/foo.md`, NOT to
    `<repo>/docs/foo.md`. This catches links that the old root-first
    resolver falsely reported as valid.
  - Backtick references (`path/to/file`) are ambiguous — they may be paths
    or code identifiers. They use the NON_PATH_PATTERNS heuristic and try
    doc-relative first, then repo-root-relative, then zig-root as fallbacks.
  - Zig source references (`Io/fiber.zig`) resolve against the `zig/` subtree.
  - Glob patterns (`src/*.cpp`, `include/sluice/async/detail/queue_*.hpp`) are
    checked by globbing — they pass if >=1 file matches.
  - TLA+ module names (`CentralSpec/CentralInv/RefinesContract`) are skipped.
  - Code identifiers with `::`, `->`, or method-call dots are skipped.

Exit code:
  0 — no broken links, no stale paths, no unallowlisted historical references.
  >0 — count of actionable problems (broken + stale + unallowlisted historical).
"""

import glob as globmod
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Documents to scan. We resolve the set from `git ls-files` so the checker
# validates exactly what the repository commits — never generated/ignored
# Markdown that happens to live under docs/ during a local build. This keeps
# the local scan set identical to the CI scan set.
#
# The patterns mirror the historical hard-coded set: root README files,
# AGENTS.md, and every tracked Markdown under docs/.
_TRACKED_PATTERNS = [
    "README.md",
    "README.zh-CN.md",
    "AGENTS.md",
    "docs/*.md",
    "docs/**/*.md",
]


def _discover_scan_files() -> list[Path]:
    """Return the list of Markdown files to scan.

    Prefers `git ls-files` so generated/gitignored Markdown under docs/ is
    excluded (this is the structural reason the docs gate was originally
    deleted — see docs/changelog.md). Falls back to a direct rglob walk only
    when git is unavailable or the script runs outside a work tree, e.g. in
    the --self-test scratch path.
    """
    import subprocess

    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "--", *_TRACKED_PATTERNS],
            capture_output=True,
            text=True,
            check=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        # No git, or not inside a work tree. Fall back to a filesystem walk
        # so the checker remains usable in stripped/artifact checkouts.
        files = [
            ROOT / "README.md",
            ROOT / "README.zh-CN.md",
            ROOT / "AGENTS.md",
        ]
        files.extend(sorted((ROOT / "docs").rglob("*.md")))
        return [f for f in files if f.exists()]

    files = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            files.append(ROOT / line)
    return sorted(files)


# Public headers to scan for `docs/...` comment references. Same git-ls-files
# discovery discipline as the Markdown scan set. Scope is deliberately the
# USER surface (installed public headers); src/ comments are #167 Step 4.
_HEADER_TRACKED_PATTERNS = [
    "include/sluice/*.hpp",
    "include/sluice/**/*.hpp",
]


def _discover_header_files() -> list[Path]:
    """Return the list of public headers to scan for docs/ references."""
    import subprocess

    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "--", *_HEADER_TRACKED_PATTERNS],
            capture_output=True,
            text=True,
            check=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return sorted((ROOT / "include" / "sluice").rglob("*.hpp"))

    files = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            files.append(ROOT / line)
    return sorted(files)


# Files to scan (resolved lazily at import so self_test() can construct its
# own scan targets without depending on the work tree).
SCAN_FILES = _discover_scan_files()

# Public headers to scan for docs/ references (PR #207 review finding 1).
HEADER_SCAN_FILES = _discover_header_files()

# Grandfathered stale header doc references — the explicit, per-site registry
# of the pre-existing backlog. Root cause of the backlog: until the header
# scan existed, stale `docs/...` pointers in public headers escaped every
# gate because the checker scanned Git-tracked Markdown only (the KNOWN_MOVED
# remap table never even saw them — it only reports STALE MOVED PATH errors
# for scanned Markdown; it never silently remapped anything).
#
# Rules (fail-closed, mirroring scripts/gates/assert-hygiene.allowlist):
#   - exemptions are per site (header path + exact token), never per file;
#   - an entry whose token no longer occurs in the named header is itself an
#     error (zombie entries fail the gate — no amnesty for registry rot);
#   - #167 Step 4 (source-comment pass) re-points each comment at the current
#     authority and deletes its entry here in the same change.
#
# The registry is intentionally EMPTY: #167 Step 4 re-pointed all 34 backlog
# sites at their current authorities and removed the entries in the same
# change. It is kept as a mechanism (not deleted) so any NEW stale `docs/...`
# reference in a public header fails the gate immediately — there is no
# grandfathered backlog to extend.
HEADER_STALE_ALLOWLIST: dict[str, dict[str, str]] = {}

# Directories whose content is generated and should be skipped entirely
SKIP_DIRS = [
    ROOT / "spec" / "tla" / "states",
    ROOT / "docs" / "spec" / "states",
]

# Zig source root — references like `Io/fiber.zig` resolve here
ZIG_ROOT = ROOT / "zig" / "lib" / "std"

# Allowlisted historical paths — these may be referenced from current docs
# because they are historical artifacts; each entry is a path that is allowed
# to appear in backtick references without triggering a stale-path failure.
HISTORICAL_ALLOWLIST = {
    # docs/reviews/ moved to docs/history/reviews/
    "docs/reviews",
    # docs/ closeout/ is historical by nature
    "docs/history/closeout",
    "docs/history/archive",
    "docs/history/implementation-plans",
    "docs/history/reviews",
    "docs/history/formal-design",
}

# Environment-conditional references.
#
# These are path prefixes that documentation legitimately references but
# that are INTENTIONALLY untracked (gitignored). They exist on a developer's
# machine but NOT in CI (actions/checkout does not materialize gitignored
# content). Examples:
#   - zig/              : the Zig stdlib design reference (AGENTS.md §1 —
#                         "design reference only, not a dependency"). Docs
#                         like docs/adr/ADR-execution-model.md reference
#                         `Io/fiber.zig`, `Io/Uring.zig`, etc. via ZIG_ROOT.
#   - .c-review-results/: automated review findings (AGENTS.md §10).
#   - .agents/, .zcode/,
#     .mimocode/        : local agent/skill tooling.
#   - states/           : TLC transient disk state (.gitignore).
#
# A backtick reference that resolves under one of these prefixes is NOT a
# documentation defect and MUST NOT fail the gate. When the target is present
# locally it resolves normally; when it is absent (CI), we report it as an
# informational ENVIRONMENT-CONDITIONAL note rather than a broken path.
#
# This is what reconciles local-vs-CI: the scan set comes from `git ls-files`
# (no generated docs leak in), but legitimate refs to the untracked design
# reference / local-only artifacts must not be flagged as broken just because
# CI does not check them out.
ENVIRONMENT_CONDITIONAL_PREFIXES = {
    "zig",
    ".c-review-results",
    ".agents",
    ".zcode",
    ".mimocode",
    "states",
    "build",
    "hardening-artifacts",
    "failures",
    "fuzz",
}


# Paths that are known to have moved; references to them from non-historical
# docs are stale-path errors unless allowlisted above.
# Maps old prefix -> new prefix for path prefix replacement.
KNOWN_MOVED = {
    "docs/reviews": "docs/history/reviews",
    "docs/design/e12-rwlock.md": "docs/history/implementation-plans/e12-rwlock.md",
    "docs/design/e13-select": "docs/history/implementation-plans/e13-select",
    "docs/design/e14": "docs/history/implementation-plans/e14",
    "docs/design/formal/e13": "docs/history/formal-design/e13",
    "docs/e10-e12-api-semantic-closure.md": "docs/history/closeout/e10-e12-api-semantic-closure.md",
    "docs/e12-queue-scheduler-integration.md": "docs/history/implementation-plans/e12-queue-scheduler-integration.md",
    "docs/e12-queue-state-machine.md": "docs/history/implementation-plans/e12-queue-state-machine.md",
    "docs/async-mutex-nothrow-authority.md": "docs/history/implementation-plans/async-mutex-nothrow-authority.md",
    "docs/async-runtime-construction-method.md": "docs/history/implementation-plans/async-runtime-construction-method.md",
    "docs/async-runtime-plan.md": "docs/history/implementation-plans/async-runtime-plan.md",
    # Root-level docs that moved to history/closeout/
    "docs/sync-before-async-readiness-gate.md": "docs/history/closeout/sync-before-async-readiness-gate.md",
    "docs/sync-io-model-gap-audit.md": "docs/history/closeout/sync-io-model-gap-audit.md",
    "docs/sync-io-next-jobs.md": "docs/history/implementation-plans/sync-io-next-jobs.md",
    "docs/async-source-inventory.md": "docs/history/implementation-plans/async-source-inventory.md",
    "docs/async-problem-statement.md": "docs/history/implementation-plans/async-problem-statement.md",
    "docs/async-design-alternatives.md": "docs/history/implementation-plans/async-design-alternatives.md",
    "docs/async-readiness-gate.md": "docs/history/implementation-plans/async-readiness-gate.md",
    "docs/async-next-jobs.md": "docs/history/implementation-plans/async-next-jobs.md",
    "docs/io-uring-spike.md": "docs/history/implementation-plans/io-uring-spike.md",
    "docs/io-uring-readiness-gate.md": "docs/history/implementation-plans/io-uring-readiness-gate.md",
    "docs/design-io-context.md": "docs/history/implementation-plans/design-io-context.md",
    "docs/zig-stdio-async-port-map.md": "docs/history/implementation-plans/zig-stdio-async-port-map.md",
    "docs/zig-stdio-migration-jobs.md": "docs/history/implementation-plans/zig-stdio-migration-jobs.md",
    "docs/zig-std-io-parity-audit.md": "docs/history/implementation-plans/zig-std-io-parity-audit.md",
    "docs/zig-std-io-source-inventory.md": "docs/history/implementation-plans/zig-std-io-source-inventory.md",
    "docs/zig-std-io-gap-calibration.md": "docs/history/archive/zig-std-io-gap-calibration.md",
    "docs/async-backend-parity.md": "docs/history/closeout/async-backend-parity.md",
    "docs/async-next-jobs.md": "docs/history/implementation-plans/async-next-jobs.md",
    "docs/buffered-fast-path.md": "docs/history/implementation-plans/design-buffered-fast-path.md",
    "docs/copy-strategy.md": "docs/history/implementation-plans/design-copy-strategy.md",
    "docs/io-context.md": "docs/history/implementation-plans/design-io-context.md",
    "docs/readv-writev-design-note.md": "docs/history/implementation-plans/design-readv-writev.md",
    "docs/optimization-decision-matrix.md": "docs/history/implementation-plans/bench-decision-matrix.md",
    "docs/optimization-runbook.md": "docs/history/implementation-plans/bench-optimization-runbook.md",
    "docs/next-steps-after-011.md": "docs/history/archive/next-steps-after-011.md",
    "docs/mvp-closeout.md": "docs/history/archive/mvp-closeout.md",
    "docs/liburing-validation-runbook.md": "docs/history/archive/liburing-validation-2026-07-03.md",
    "docs/formal/e13-select-formal-core-design.md": "docs/history/formal-design/e13-select-formal-core-design.md",
    "docs/formal/e13-select-formal-safety-design.md": "docs/history/formal-design/e13-select-formal-safety-design.md",
    # Root-level docs that moved to history/closeout/ or history/implementation-plans/
    "docs/e10-waitnode-wait-queue.md": "docs/history/closeout/e10-waitnode-wait-queue.md",
    "docs/e11-deadline-timer-wait.md": "docs/history/closeout/e11-deadline-timer-wait.md",
    "docs/e11-arch-recon-audit.md": "docs/history/closeout/e11-arch-recon-audit.md",
    "docs/e12-async-mutex.md": "docs/history/closeout/e12-async-mutex.md",
    "docs/e12-condition.md": "docs/history/closeout/e12-condition.md",
    "docs/e12-event.md": "docs/history/closeout/e12-event.md",
    "docs/e12-queue.md": "docs/history/closeout/e12-queue.md",
    "docs/e12-semaphore.md": "docs/history/closeout/e12-semaphore.md",
    "docs/e12-rwlock.md": "docs/history/implementation-plans/e12-rwlock.md",
    "docs/e12-queue-implementation-authorization.md": "docs/history/closeout/e12-queue-implementation-authorization.md",
    "docs/e12-queue-production-implementation.md": "docs/history/closeout/e12-queue-production-implementation.md",
    "docs/e12-queue-corrective-3.md": "docs/history/closeout/e12-queue-corrective-3.md",
    "docs/e12-cross-primitive-terminal-audit.md": "docs/history/closeout/e12-cross-primitive-terminal-audit.md",
    "docs/e13-select-preparation.md": "docs/history/closeout/e13-select-preparation.md",
    "docs/e13-select-p7-rollback-closeout.md": "docs/history/closeout/e13-select-p7-rollback-closeout.md",
    "docs/e12-sync-primitives-plan.md": "docs/history/implementation-plans/e12-sync-primitives-plan.md",
    "docs/sync-optimization-notes.md": "docs/history/closeout/sync-optimization-notes.md",
    "docs/bench-decision-matrix.md": "docs/history/implementation-plans/bench-decision-matrix.md",
    "docs/bench-methodology.md": "docs/history/implementation-plans/bench-methodology.md",
    "docs/design-flush-sync-durability.md": "docs/history/implementation-plans/design-flush-sync-durability.md",
    "docs/design-copy-strategy.md": "docs/history/implementation-plans/design-copy-strategy.md",
    "docs/design-buffered-fast-path.md": "docs/history/implementation-plans/design-buffered-fast-path.md",
    "docs/design-io-context.md": "docs/history/implementation-plans/design-io-context.md",
    "docs/design-wal-durability.md": "docs/history/implementation-plans/design-wal-durability.md",
    "docs/design-readv-writev.md": "docs/history/implementation-plans/design-readv-writev.md",
    "docs/mvp-closeout.md": "docs/history/archive/mvp-closeout.md",
    "docs/mvp-core-model.md": "docs/history/archive/mvp-core-model.md",
    "docs/next-steps-after-011.md": "docs/history/archive/next-steps-after-011.md",
    "docs/release-v0.1-mvp-checklist.md": "docs/history/archive/release-v0.1-mvp-checklist.md",
    "docs/zig-std-io-gap-calibration.md": "docs/history/archive/zig-std-io-gap-calibration.md",
    "docs/zig-std-io-parity-audit.md": "docs/history/implementation-plans/zig-std-io-parity-audit.md",
    "docs/zig-std-io-source-inventory.md": "docs/history/implementation-plans/zig-std-io-source-inventory.md",
    "docs/zig-stdio-async-port-map.md": "docs/history/implementation-plans/zig-stdio-async-port-map.md",
    "docs/zig-stdio-migration-jobs.md": "docs/history/implementation-plans/zig-stdio-migration-jobs.md",
    "docs/async-backend-parity.md": "docs/history/closeout/async-backend-parity.md",
    "docs/async-mutex-nothrow-implementation.md": "docs/history/closeout/async-mutex-nothrow-implementation.md",
    "docs/async-runtime-hang-and-gcc-corrective.md": "docs/history/closeout/async-runtime-hang-and-gcc-corrective.md",
    "docs/e0a-waiting-policy-audit.md": "docs/history/closeout/e0a-waiting-policy-audit.md",
    "docs/sync-before-async-readiness-gate.md": "docs/history/closeout/sync-before-async-readiness-gate.md",
    "docs/sync-io-model-gap-audit.md": "docs/history/closeout/sync-io-model-gap-audit.md",
    "docs/sync-runtime-bench-notes.md": "docs/history/closeout/sync-runtime-bench-notes.md",
    "docs/sync-runtime-bench-notes.md": "docs/history/closeout/sync-runtime-bench-notes.md",
    "docs/io-uring-spike.md": "docs/history/implementation-plans/io-uring-spike.md",
    "docs/io-uring-readiness-gate.md": "docs/history/implementation-plans/io-uring-readiness-gate.md",
    "docs/async-source-inventory.md": "docs/history/implementation-plans/async-source-inventory.md",
    "docs/async-problem-statement.md": "docs/history/implementation-plans/async-problem-statement.md",
    "docs/async-design-alternatives.md": "docs/history/implementation-plans/async-design-alternatives.md",
    "docs/async-readiness-gate.md": "docs/history/implementation-plans/async-readiness-gate.md",
    "docs/async-next-jobs.md": "docs/history/implementation-plans/async-next-jobs.md",
    "docs/sync-io-next-jobs.md": "docs/history/implementation-plans/sync-io-next-jobs.md",
    "docs/async-deferred-until-sync-baseline.md": "docs/history/implementation-plans/async-deferred-until-sync-baseline.md",
    "docs/async-design-alternatives.md": "docs/history/implementation-plans/async-design-alternatives.md",
    "docs/async-problem-statement.md": "docs/history/implementation-plans/async-problem-statement.md",
    "docs/async-readiness-gate.md": "docs/history/implementation-plans/async-readiness-gate.md",
    "docs/async-source-inventory.md": "docs/history/implementation-plans/async-source-inventory.md",
    "docs/bench-optimization-runbook.md": "docs/history/implementation-plans/bench-optimization-runbook.md",
    "docs/bench-results-sample.csv": "docs/history/implementation-plans/bench-results-sample.csv",
    "docs/bench-summary-sample.txt": "docs/history/implementation-plans/bench-summary-sample.txt",
    "docs/design-copy-strategy.md": "docs/history/implementation-plans/design-copy-strategy.md",
    "docs/design-flush-sync-durability.md": "docs/history/implementation-plans/design-flush-sync-durability.md",
    "docs/design-io-context.md": "docs/history/implementation-plans/design-io-context.md",
    "docs/design-readv-writev.md": "docs/history/implementation-plans/design-readv-writev.md",
    "docs/design-wal-durability.md": "docs/history/implementation-plans/design-wal-durability.md",
    "docs/io-uring-readiness-gate.md": "docs/history/implementation-plans/io-uring-readiness-gate.md",
    "docs/io-uring-spike.md": "docs/history/implementation-plans/io-uring-spike.md",
    "docs/zig-std-io-parity-audit.md": "docs/history/implementation-plans/zig-std-io-parity-audit.md",
    "docs/zig-std-io-source-inventory.md": "docs/history/implementation-plans/zig-std-io-source-inventory.md",
    "docs/zig-stdio-async-port-map.md": "docs/history/implementation-plans/zig-stdio-async-port-map.md",
    "docs/zig-stdio-migration-jobs.md": "docs/history/implementation-plans/zig-stdio-migration-jobs.md",
    "docs/e13-select-event-adapter.md": "docs/history/implementation-plans/e13-select-event-adapter.md",
    "docs/e13-select-locking-and-publication.md": "docs/history/implementation-plans/e13-select-locking-and-publication.md",
    "docs/e13-select-production-architecture.md": "docs/history/implementation-plans/e13-select-production-architecture.md",
    "docs/e13-select-production-test-plan.md": "docs/history/implementation-plans/e13-select-production-test-plan.md",
    "docs/e13-select-public-api.md": "docs/history/implementation-plans/e13-select-public-api.md",
    "docs/e13-select-state-machine.md": "docs/history/implementation-plans/e13-select-state-machine.md",
    "docs/e13-select-test-plan.md": "docs/history/implementation-plans/e13-select-test-plan.md",
    "docs/e13-select-timer-adapter.md": "docs/history/implementation-plans/e13-select-timer-adapter.md",
    "docs/e13-select-type-and-lifetime.md": "docs/history/implementation-plans/e13-select-type-and-lifetime.md",
    "docs/e14-threaded-evented-parity-preparation.md": "docs/history/implementation-plans/e14-threaded-evented-parity-preparation.md",
    "docs/e10-e12-api-semantic-closure.md": "docs/history/closeout/e10-e12-api-semantic-closure.md",
    "docs/e12-queue-scheduler-integration.md": "docs/history/implementation-plans/e12-queue-scheduler-integration.md",
    "docs/e12-queue-state-machine.md": "docs/history/implementation-plans/e12-queue-state-machine.md",
    "docs/async-mutex-nothrow-authority.md": "docs/history/implementation-plans/async-mutex-nothrow-authority.md",
    "docs/async-runtime-construction-method.md": "docs/history/implementation-plans/async-runtime-construction-method.md",
    "docs/async-runtime-plan.md": "docs/history/implementation-plans/async-runtime-plan.md",
    "docs/e12-rwlock.md": "docs/history/implementation-plans/e12-rwlock.md",
    # Root-level paths whose stale references were first observed in public
    # headers (PR #207 review finding 1) — the Markdown scan surface never
    # saw them. New locations verified against the current tree. NOTE:
    # docs/e13-select-formal-production-mapping.md deliberately NOT added —
    # historical documents still backtick-reference that old path, and a
    # KNOWN_MOVED entry would flip them from the historical exemption into
    # STALE errors; the header-side grandfather registry covers it instead.
    "docs/flush-sync-durability.md": "docs/history/implementation-plans/design-flush-sync-durability.md",
    "docs/e8-formal-corrective": "docs/history/closeout/e8-formal-corrective",
    # #167 Step 5 archive moves (2026-08-25): zero-consumer evidence/history
    # records moved out of docs/architecture/. The one public-header pointer
    # (submit_transaction.hpp -> issue-137 design) was removed by PR #208's
    # authority repoint; every other live reference was updated in the same
    # change. docs/history contains no stale references to these old paths
    # (verified before adding the entries), so the historical exemption is
    # not affected.
    "docs/architecture/phase-d2-uring-failure-noalloc-implementation-plan.md":
        "docs/history/implementation-plans/phase-d2-uring-failure-noalloc-implementation-plan.md",
    "docs/architecture/issue-137-submission-transaction-design.md":
        "docs/history/implementation-plans/issue-137-submission-transaction-design.md",
    "docs/architecture/issue-137-submission-transaction-compliance-gate.md":
        "docs/history/closeout/issue-137-submission-transaction-compliance-gate.md",
    "docs/architecture/issue-137-submission-transaction-mutation-evidence.md":
        "docs/history/closeout/issue-137-submission-transaction-mutation-evidence.md",
    "docs/architecture/c7-runtime-await-helpers-compliance-gate.md":
        "docs/history/closeout/c7-runtime-await-helpers-compliance-gate.md",
    "docs/architecture/phase-f-compliance-gate.md":
        "docs/history/closeout/phase-f-compliance-gate.md",
    "docs/architecture/phase-d3-uring-identity-waiter-gate.md":
        "docs/history/closeout/phase-d3-uring-identity-waiter-gate.md",
    "docs/architecture/phase-d4-uring-wait-close-drain-gate.md":
        "docs/history/closeout/phase-d4-uring-wait-close-drain-gate.md",
    # #167 Step 5c safe archive moves (2026-08-25): issue-110 / issue-123 /
    # phase-b moved to docs/history/. Every exact-path consumer was updated
    # atomically in the same change (issue-123: phase-g-backend-progress-wake.md
    # and issue-116-runtime-reentry-liveness.md; phase-b:
    # phase-b-compliance-gate.md, request_arena_test.cpp, xmake/tests/async.lua).
    # docs/history contains no stale references to these old paths (verified
    # before adding the entries), so the historical exemption is not affected.
    "docs/investigations/issue-110-dequeue-gate-generation-handshake.md":
        "docs/history/issues/issue-110-dequeue-gate-generation-handshake.md",
    "docs/investigations/issue-123-phase-g-closeout-parallel-flake.md":
        "docs/history/issues/issue-123-phase-g-closeout-parallel-flake.md",
    # #167 Step 5d (2026-08-25): issue-115 moved to docs/history/issues/ after
    # disposition adjudication (deferred-to-application-evidence = superseded
    # by the implemented evidence-derived corrective). Consumers updated
    # atomically: issue-115-runnable-publication-wake-gate.md and
    # post-freeze-final-report.md. docs/history contains no stale references to
    # the old path (verified), so the historical exemption is not affected.
    "docs/investigations/issue-115-runnable-publication-wake.md":
        "docs/history/issues/issue-115-runnable-publication-wake.md",
    "docs/design/phase-b-request-slot-reference.md":
        "docs/history/implementation-plans/phase-b-request-slot-reference.md",
}

# Regex patterns
# Markdown link: [text](target) — target may be URL, anchor, or path
MD_LINK_RE = re.compile(r'\[([^\]]*)\]\(([^)#]+)(?:#[^)]*)?\)')
# Backtick path: `some/path/to/file` (relative-looking, no spaces, has /)
BACKTICK_PATH_RE = re.compile(r'`([^`\s]*/[^`\s]+)`')

# Public-header docs/ token: `docs/<alnum>...`. The lookbehind excludes
# tokens embedded inside URLs (https://clang.llvm.org/docs/Foo.html) and
# identifiers glued onto the token (e.g. `godocs/`).
HEADER_DOC_REF_RE = re.compile(r'(?<![\w:./-])docs/[A-Za-z0-9][A-Za-z0-9_./-]*')
# Comment-wrap continuation: a token ending in `-` may continue on the next
# comment line — `docs/e13-select-locking-and-` + `// publication.md` — with
# an optional `//` or ` * ` comment prefix on the continuation line.
HEADER_WRAP_CONT_RE = re.compile(
    r'\s*\n[ \t]*(?://+[ \t]*|\*+[ \t]*)?([A-Za-z0-9][A-Za-z0-9_./-]*)'
)

# Patterns that look like file paths but are actually code identifiers / non-paths
# These are skipped to avoid false positives.
NON_PATH_PATTERNS = [
    # Absolute paths (OS-level, not repo files)
    r'^/',
    # Brace expansion: verify-e{8,9}-stability.sh
    r'\{.*\}',
    # Code identifiers with ::  (namespace / scope resolution)
    r'::',
    # Glob wildcards (handled separately, but skip pure wildcards)
    r'^\*$',
    # Branch names like feat/..., audit/... (git branches, not files)
    r'^(feat|audit|fix|refactor)/',
    # Single-letter directory like A/W/T/R
    r'^A/W',
    # Template placeholders like <phase/commit>
    r'^<',
    # try/catch
    r'^try/catch',
    # n/a
    r'^n/a$',
    # External references like /microsoft/stl, /xmake-io/xmake-docs
    r'^/[a-z]+/[a-z\-]+$',
    # Shell variable placeholders: $outroot, $repo, $TMPDIR, ${TMPDIR:-/tmp}
    r'^\$',
    # Code member calls like top->fire_on_resolve_locked(/*timer_won=*/true)
    r'->',
    # Environment variable assignments containing =
    r'=',
    # Home directory references like ~/.agents/skills/
    r'^~/',
    # Zig std lib wildcard paths like zig/lib/std/Io/*.zig
    r'.*/Io/\*\.zig$',
    # Code identifiers with dots (method calls): fiber.create/destroy, Fiber.create/destroy
    r'^[A-Za-z][A-Za-z0-9]*\.[A-Za-z]',
    # TLA+ module names: CentralSpec/CentralInv/RefinesContract
    r'^[A-Z][a-zA-Z]+/[A-Z][a-zA-Z]+/[A-Z][a-zA-Z]+$',
    # Coverage/generated artifact paths
    r'^coverage/',
    r'^findings/',
    # Non-existent test files referenced in historical docs (renamed/never existed)
    r'^tests/test_t3_simple\.cpp$',
    r'^tests/test_\*channel\*$',
    r'^tests/e11_timer_wait_test\.cpp$',
    r'^tests/e12_api_contract_probes\.cpp$',
    r'^tests/e12_async_(condition|mutex|queue|event|semaphore)_(authority_probe|test|death_test)\.cpp$',
    r'^tests/e12_async_mutex_nothrow_authority_probe\.cpp$',
    r'^tests/e12_cross_primitive_parity_test\.cpp$',
    r'^tests/e8_steal_test\.cpp$',
    r'^tests/uring_\*\.cpp$',
    r'^tests/tsa-probe/tsa_\*\.cpp$',
    r'^tests/\{.*\}\.cpp$',
    # Non-existent scripts referenced in historical docs
    r'^scripts/verify-e[89]-stability\.sh$',
    # Code member paths with slashes (not file paths)
    r'^(do_read|do_write|do_sync)$',
    r'^next_/prev_/home_$',
    r'^active/retired/consumed$',
    r'^waiting_size_/void_/ready_$',
    r'^try_acquire/acquire/acquire_until/cancel/release$',
    r'^condition_notify_one/all$',
    r'^queue_push/pop_admit_until$',
    r'^queue_grant_producer/consumer_locked$',
    r'^is_terminal/outcome/was_woken/was_cancelled$',
    r'^r\.buffer/seek/end$',
    r'^w\.buffer/end$',
    r'^expectedProdHead/Head$',
    r'^futures_/evented_fibers_/evented_stacks_$',
    r'^mtx_t\d+/t\d+_wrong_mutex_\*$',
    r'^sem_t\d+/t\d+_wrong_semaphore_\*$',
    # Shell variable assignments
    r'^workdir=',
    # Example/benchmark target names (not files)
    r'^examples/experimental_uring_write$',
    r'^bench/uring_write_bench$',
    r'^bench/support/blocking_io_pool\.\*$',
    # Non-existent result files
    r'^docs/results/sync-w1-w4-baseline\.md$',
    # Non-existent source files (historical references)
    r'^src/async/async_rwlock\.cpp$',
    r'^src/async/event\.cpp$',
    r'^src/async/queue_\*\.cpp$',
    r'^experimental/uring_write_batch\.\*$',
    # Code-style include paths (actual: include/sluice/...)
    r'^detail/posix_retry\.hpp$',
    r'^detail/select_port\.hpp$',
    r'^detail/select_registration\.hpp$',
    r'^detail/mutex_test_seam\.hpp$',
    r'^detail/select_\*\.hpp$',
    r'^sluice/measurement\.hpp$',
    # Glob patterns for include/src dirs
    r'^include/sluice/\*\.hpp$',
    r'^include/sluice/async/\*\.hpp$',
    r'^include/sluice/async/detail/queue_\*\.hpp$',
    r'^src/\*\.cpp$',
    r'^src/async/\*\.cpp$',
    # Shell glob patterns
    r'^src/experimental/uring_\*$',
    # More non-existent test files
    r'^tests/e12_event_test\.cpp$',
    r'^tests/e12_semaphore_test\.cpp$',
    r'^tests/e12_async_condition_test\.cpp$',
    r'^tests/e12_async_mutex_test\.cpp$',
    r'^tests/e12_async_queue_test\.cpp$',
    r'^tests/e12_cross_primitive_parity_test\.cpp$',
    r'^tests/e12_async_mutex_death_test\.cpp$',
    r'^tests/e12_async_mutex_nothrow_authority_probe\.cpp$',
    r'^tests/e12_async_condition_authority_probe\.cpp$',
    r'^tests/e12_async_mutex_authority_probe\.cpp$',
    r'^tests/e12_semaphore_authority_probe\.cpp$',
    r'^tests/e12_api_contract_probes\.cpp$',
    r'^tests/e11_timer_wait_test\.cpp$',
    r'^tests/e8_steal_test\.cpp$',
    # Code state names
    r'^Detached/ProducerOp/Ring/ConsumerOp/Released$',
    r'^head_/tail_$',
    r'^do_read/write/sync$',
    # Non-existent result files
    r'^docs/results/sync-durability-baseline\.md$',
    # Code member names
    r'^next_/prev_$',
    # Spec trace files
    r'^spec/tla/e13_select/.*\.keep$',
    # Code state names
    r'^woken/cancelled/expired$',
    r'^unresolved/woken/cancelled/expired$',
    r'^unused/submitted/pending/completed$',
    r'^unset/waiting/is_set$',
    r'^\(E/T/A\)$',
    # Code member/method names
    r'^user/set_user$',
    r'^user\(\)/set_user\(\)$',
    # Zig source references
    r'^Io/Evented\.zig$',
    # Glob patterns
    r'^benchmarks/\*\*$',
    r'^tests/test_\*queue\*$',
    # Spec directories (these exist but the checker resolves them as files)
    r'^spec/e12_semaphore/$',
    r'^spec/e11_timer_wait/$',
    r'^spec/e12_semaphore/README\.md$',
    # Formal directory reference
    r'^docs/formal/$',
    # More code identifiers
    r'^notify_one/all$',
    r'^cond_t\d+/t\d+$',
    r'^mtx_t\d+/t\d+$',
    r'^sem_t\d+/t\d+$',
    r'^rwlock_t\d+(/\d+)*$',
    r'^sem_t\d+/t\d+_\*$',
    r'^set/reset/wait/wait_until/cancel$',
    r'^lock/lock_until/cancel/unlock$',
    r'^cancel/notify_one/notify_all$',
    r'^push/push_until/try_push$',
    r'^pop/pop_until/try_pop$',
    r'^lock/try_lock$',
    r'^operational/tearing_down$',
    r'^Reserved/Held$',
    r'^\(T/U/V\)$',
    r'^auto&$',
    # GitHub references
    r'^jnhu76/Sluice#\d+$',
    r'^origin/master$',
    # External paths
    r'^forgejo/workflows$',
    r'^os/linux/IoUring$',
    r'^Io/net\*$',
    r'^sp/fp(/pc)?$',
    # Spec directory references
    r'^spec/e12_event/$',
    # Non-existent experimental headers
    r'^experimental/uring_write_batch\.hpp$',
    r'^experimental/uring_io_context\.hpp$',
    # Code types / expressions with spaces (won't match backtick regex, but just in case)
    r'^Result<size_t>',
    r'^auto&$',
    r'^auto&',
    # More test case IDs with letter suffixes or multiple segments
    r'^cond_t\d+[a-z]*/t\d+[a-z]*$',
    r'^rwlock_t\d+(/t\d+)+$',
    r'^sem_t\d+(/t\d+)+$',
    r'^mtx_t\d+(/t\d+)+$',
    # TLA+ module names (two segments)
    r'^ContractSpec/ContractInv$',
    # Code file with slash-separated line refs (code references, not doc links)
    r'^wait_queue\.hpp:\d+(/\d+)+$',
    # Code-style header references with line numbers (not doc links)
    r'^[a-z_]+\.hpp:\d+$',
    r'^[a-z_]+\.cpp:\d+$',
    r'^[a-z_]+\.hpp:\d+(/\d+)*$',
    r'^[a-z_]+\.cpp:\d+(/\d+)*$',
    r'^[a-z_]+\.cpp:\d+-\d+$',
    r'^[a-z_]+\.cpp:\d+,\d+.*$',
    r'^[a-z_]+\.hpp:\d+,\d+.*$',
    # Source file with comma-separated line refs and ellipsis
    r'^src/async/scheduler\.cpp:\d+,\d+,\d+,\d+,\.\.\.$',
]


def should_skip(path: Path) -> bool:
    """Return True if `path` is inside a skip directory."""
    for sd in SKIP_DIRS:
        try:
            path.relative_to(sd)
            return True
        except ValueError:
            pass
    return False


def strip_line_ref(ref: str) -> str:
    """Strip trailing line references from a path.

    Handles:
      - Single line ref: :123
      - Line range: :123-456
      - Comma-separated: :119,145 or :199/222/245/266
      - Ellipsis: :403-...
      - Em-dash ranges: :2056–2097
      - TLA+ .cfg shorthand: E12Foo.tla/.cfg -> E12Foo.tla
    """
    # TLA+ .cfg shorthand: E12Foo.tla/.cfg -> E12Foo.tla
    if ref.endswith("/.cfg"):
        return ref[: -len("/.cfg")]
    # Comma-separated or slash-separated line lists: :119,145 or :199/222/245/266
    m = re.match(r'^(.+?)(:\d[\d,\-/]*\d)$', ref)
    if m:
        return m.group(1)
    # Ellipsis line ref: :403-...
    m = re.match(r'^(.+?)(:\d+-\.\.\.)$', ref)
    if m:
        return m.group(1)
    # Single line ref or range (including em-dash): :123 or :123-456 or :2056–2097
    m = re.match(r'^(.+?)(:\d+(?:[–\-]\d+)?)$', ref)
    if m:
        return m.group(1)
    return ref


def is_url(ref: str) -> bool:
    """Return True if ref is an external URL or mailto."""
    return ref.startswith(("http://", "https://", "mailto:", "#"))


def is_non_path(ref: str) -> bool:
    """Return True if ref looks like a code identifier, not a file path."""
    # Pre-check: code-style file references with line numbers
    # (e.g. wait_queue.hpp:199/222/245/266, scheduler.cpp:683,688,...)
    if re.search(r'\.(hpp|cpp):\d', ref):
        return True
    clean = strip_line_ref(ref)
    for pat in NON_PATH_PATTERNS:
        # Use search for patterns that can appear anywhere in the string
        # (e.g. ::, ->), match for anchored patterns.
        if pat.startswith("^") or pat.endswith("$"):
            if re.match(pat, clean):
                return True
        else:
            if re.search(pat, clean):
                return True
    return False


def looks_like_zig_path(ref: str) -> bool:
    """Return True if ref looks like a Zig std lib source path."""
    base = strip_line_ref(ref)
    # Ends with .zig
    if base.endswith(".zig"):
        return True
    # Starts with capitalized directory (Io, Uring, Threaded, etc.)
    parts = base.replace("\\", "/").split("/")
    if parts and parts[0] and parts[0][0:1].isupper() and "/" in base:
        return True
    return False


def is_tla_module(ref: str) -> bool:
    """Return True if ref looks like a TLA+ module name path."""
    base = strip_line_ref(ref)
    # TLA+ module paths: WordWord/WordWord/WordWord (three CamelCase segments)
    parts = base.replace("\\", "/").split("/")
    if len(parts) == 3 and all(p[:1].isupper() and p.isalnum() for p in parts):
        return True
    return False


def resolve_ref(doc_path: Path, ref: str, from_backtick: bool = False) -> Path | None:
    """Resolve a reference. Returns the resolved Path or None if it cannot
    be resolved as a repository path (e.g. URL, anchor-only, non-path).

    CRITICAL: Markdown links are resolved STRICTLY relative to the current
    document (doc_path.parent), NOT relative to the repository root. This
    matches how GitHub renders Markdown: a link `docs/foo.md` in
    `docs/history/closeout/e12-event.md` is interpreted by GitHub as
    `docs/history/closeout/docs/foo.md`, NOT as `<repo>/docs/foo.md`.

    Backtick references (from_backtick=True) are ambiguous — `foo/bar` could
    be a path or a code identifier — so they use the NON_PATH_PATTERNS
    heuristic and try repo-root-relative as a fallback.
    """
    if is_url(ref):
        return None  # external — skip

    if from_backtick and is_non_path(ref):
        return None  # code identifier, not a file path

    if is_tla_module(ref):
        return None  # TLA+ module name, not a file path

    # Strip line-number references
    clean_ref = strip_line_ref(ref)

    # Handle glob patterns: check if any files match
    if "*" in clean_ref or "?" in clean_ref:
        # For markdown links: doc-relative only
        if not from_backtick:
            matches = globmod.glob(str(doc_path.parent / clean_ref), recursive=True)
            if matches:
                return Path(matches[0])
            return (doc_path.parent / clean_ref).resolve()
        # For backtick: try doc-relative first, then repo-root-relative
        matches = globmod.glob(str(doc_path.parent / clean_ref), recursive=True)
        if matches:
            return Path(matches[0])
        matches = globmod.glob(str(ROOT / clean_ref), recursive=True)
        if matches:
            return Path(matches[0])
        return (ROOT / clean_ref).resolve()

    # === Markdown links: STRICTLY doc-relative ===
    # GitHub resolves [text](ref) relative to the current document's directory.
    # A link `docs/foo.md` in `docs/history/closeout/e12-event.md` becomes
    # `docs/history/closeout/docs/foo.md` on GitHub — which is broken.
    # The checker must flag this, not silently resolve it against the root.
    if not from_backtick:
        candidate = (doc_path.parent / clean_ref).resolve()
        if candidate.exists():
            return candidate
        # Not found relative to the document — this is a broken link.
        return candidate  # return the (non-existent) doc-relative path

    # === Backtick references: doc-relative first, then repo-root fallback ===
    # Backtick `foo/bar` is ambiguous: it could be a path or a code identifier.
    # We already passed the is_non_path() check above, so try path resolution.
    candidate = (doc_path.parent / clean_ref).resolve()
    if candidate.exists():
        return candidate

    # Try repo-root-relative as a fallback for backtick references
    candidate = (ROOT / clean_ref).resolve()
    if candidate.exists():
        return candidate

    # Try zig-root for Zig source references
    if looks_like_zig_path(clean_ref):
        candidate = (ZIG_ROOT / clean_ref).resolve()
        if candidate.exists():
            return candidate

    # Return the doc-relative path for error reporting
    return (doc_path.parent / clean_ref).resolve()


def is_historical(resolved: Path) -> bool:
    """Return True if resolved path is under an allowlisted historical subtree."""
    rel = os.path.relpath(resolved, ROOT)
    return any(
        rel.startswith(a + "/") or rel == a
        for a in HISTORICAL_ALLOWLIST
    )


def is_source_historical(path: Path) -> bool:
    """Return True if the source file itself is a historical document.

    Historical source files may legitimately reference old/moved paths that
    no longer exist (e.g. `tools/formal/**` in a migration-era review doc).
    """
    rel = os.path.relpath(path, ROOT)
    return rel.startswith("docs/history/")


def environment_conditional_prefix(ref: str, resolved: Path) -> str | None:
    """Return the ENVIRONMENT_CONDITIONAL_PREFIXES entry that `ref` (or its
    resolved path) falls under, else None.

    A reference is environment-conditional if the target lives under a
    gitignored-but-legitimate subtree. Three detection strategies, because the
    resolver returns a doc-relative fallback when the real target is absent:

      1. Raw-ref prefix: `.c-review-results/...`, `.agents/skills/...`,
         `states/...`, `zig/...` — the ref text itself names the subtree.
      2. Zig-design-reference shape: `Io/*.zig`, `Io/Foo/*.zig`, etc. These
         resolve under ZIG_ROOT = zig/lib/std/Io (gitignored per AGENTS.md §1).
         Detected via looks_like_zig_path() regardless of how the resolver
         fell back, so this works in CI where zig/ is absent.
      3. Path-component match: any trailing path component equal to a
         name-based .gitignore dir rule (e.g. `states/` matches
         `spec/tla/e13_select/states`). Covers gitignore rules that are
         matched at any depth.

    Returns the matched prefix so the caller can emit a precise note.
    """
    clean = strip_line_ref(ref).lstrip("./")
    # Strategy 1: raw-ref prefix.
    for prefix in ENVIRONMENT_CONDITIONAL_PREFIXES:
        if clean == prefix or clean.startswith(prefix + "/"):
            return prefix
    # Strategy 2: Zig design-reference shape.
    if looks_like_zig_path(clean):
        return "zig"
    # Strategy 3: name-based .gitignore dir component anywhere in the path.
    try:
        rel = os.path.relpath(resolved, ROOT).replace("\\", "/")
    except ValueError:
        return None
    parts = [p for p in rel.split("/") if p]
    for prefix in ENVIRONMENT_CONDITIONAL_PREFIXES:
        if prefix in parts:
            return prefix
    return None


def is_known_moved(ref: str, doc_path: Path | None = None,
                  resolved: Path | None = None) -> str | None:
    """Return the new path if ref (or its resolved path) matches a known
    moved path, else None."""
    clean = strip_line_ref(ref)
    for old, new in KNOWN_MOVED.items():
        if clean.startswith(old + "/") or clean == old:
            return new
    # Also check the resolved path (for doc-relative refs that don't match
    # the raw ref pattern).
    if resolved is not None:
        try:
            rel = os.path.relpath(resolved, ROOT)
            for old, new in KNOWN_MOVED.items():
                if rel.startswith(old + "/") or rel == old:
                    return new
        except ValueError:
            pass
    # Also check the doc-relative resolved path.
    if doc_path is not None:
        doc_rel = (doc_path.parent / clean).resolve()
        try:
            rel = os.path.relpath(doc_rel, ROOT)
            for old, new in KNOWN_MOVED.items():
                if rel.startswith(old + "/") or rel == old:
                    return new
        except ValueError:
            pass
    return None


def strip_code_fences(text: str) -> str:
    """Blank out fenced code blocks while preserving character positions.

    GitHub does not render markdown links inside fenced code, and C++ lambda
    syntax (``[capture](args)``) otherwise false-positives as a markdown link.
    Fence marker lines and fenced content become spaces (newlines preserved)
    so line/offset arithmetic stays exact.
    """
    out: list[str] = []
    in_fence = False
    for line in text.splitlines(keepends=True):
        body, sep, _ = line.partition("\n")
        if body.lstrip().startswith("```"):
            in_fence = not in_fence
            out.append(" " * len(body) + sep)
            continue
        if in_fence:
            out.append(" " * len(body) + sep)
        else:
            out.append(line)
    return "".join(out)


def check_file(path: Path) -> tuple[list[str], list[str], list[str], list[str]]:
    """Check a single Markdown file.

    Returns (broken, stale, historical_unallowlisted, envcond).
    """
    broken = []
    stale = []
    historical = []
    envcond = []

    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        broken.append(f"{path}: cannot read: {e}")
        return broken, stale, historical, envcond

    # Links and backtick refs inside fenced code blocks are code, not
    # documentation references (GitHub does not render them as links).
    scan_text = strip_code_fences(text)
    line_of = lambda pos: scan_text[: pos].count("\n") + 1

    # --- Markdown links ---
    # Markdown links are ALWAYS treated as paths — never skipped by the
    # NON_PATH_PATTERNS heuristic. A markdown link [text](target) is an
    # explicit link; its target must resolve.
    for m in MD_LINK_RE.finditer(scan_text):
        ref = m.group(2).strip()
        resolved = resolve_ref(path, ref, from_backtick=False)
        if resolved is None:
            continue
        if not resolved.exists():
            moved = is_known_moved(ref, path, resolved)
            if moved:
                stale.append(
                    f"{path}:{line_of(m.start())}: "
                    f"STALE MOVED PATH: `{ref}` -> should be `{moved}`"
                )
            elif is_historical(resolved):
                historical.append(
                    f"{path}:{line_of(m.start())}: "
                    f"HISTORICAL (allowlisted): `{ref}`"
                )
            else:
                eco = environment_conditional_prefix(ref, resolved)
                if eco is not None:
                    envcond.append(
                        f"{path}:{line_of(m.start())}: "
                        f"ENVIRONMENT-CONDITIONAL (`{eco}/` is gitignored): `{ref}`"
                    )
                else:
                    broken.append(
                        f"{path}:{line_of(m.start())}: "
                        f"BROKEN MARKDOWN LINK: `{ref}` -> {resolved}"
                    )

    # --- Backtick paths ---
    # Backtick references are ambiguous — `foo/bar` could be a path or a
    # code identifier. The NON_PATH_PATTERNS heuristic applies here to
    # avoid false positives on code identifiers.
    # Lines with <!-- old-path-ok --> are explicit allowlisted quotes.
    lines = text.splitlines()
    for m in BACKTICK_PATH_RE.finditer(scan_text):
        ref = m.group(1).strip()
        lineno = text[: m.start()].count("\n")
        if "<!-- old-path-ok -->" in lines[lineno]:
            continue
        resolved = resolve_ref(path, ref, from_backtick=True)
        if resolved is None:
            continue
        if not resolved.exists():
            moved = is_known_moved(ref, path, resolved)
            if moved:
                stale.append(
                    f"{path}:{line_of(m.start())}: "
                    f"STALE MOVED PATH (backtick): `{ref}` -> should reference `{moved}`"
                )
            elif is_historical(resolved) or is_source_historical(path):
                historical.append(
                    f"{path}:{line_of(m.start())}: "
                    f"HISTORICAL (allowlisted): `{ref}`"
                )
            else:
                eco = environment_conditional_prefix(ref, resolved)
                if eco is not None:
                    envcond.append(
                        f"{path}:{line_of(m.start())}: "
                        f"ENVIRONMENT-CONDITIONAL (`{eco}/` is gitignored): `{ref}`"
                    )
                else:
                    broken.append(
                        f"{path}:{line_of(m.start())}: "
                        f"BROKEN BACKTICK PATH: `{ref}` -> {resolved}"
                    )

    return broken, stale, historical, envcond


def extract_header_doc_refs(text: str) -> list[tuple[str, int]]:
    """Extract repo-relative `docs/...` tokens from header text.

    Returns (token, 1-based line of the token start). Sentence punctuation is
    stripped from the token tail; a token ending in `-` is joined with its
    comment-wrap continuation on the next line before resolution.
    """
    refs = []
    for m in HEADER_DOC_REF_RE.finditer(text):
        token = m.group(0)
        line = text[: m.start()].count("\n") + 1
        if token.endswith("-"):
            cont = HEADER_WRAP_CONT_RE.match(text, m.end())
            if cont is not None:
                token += cont.group(1)
        token = token.rstrip(".,;:!?)\"'")
        refs.append((token, line))
    return refs


def _header_allowlist_key(path: Path) -> str:
    """Registry key for a header: repo-relative posix path when inside the
    repository, else the resolved absolute posix path (self-test scratch)."""
    try:
        return path.resolve().relative_to(ROOT.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def check_header_file(
    path: Path,
    allowlist: dict[str, dict[str, str]] | None = None,
) -> tuple[list[str], list[str], list[str]]:
    """Check one public header for stale/broken docs/ references.

    Returns (broken, stale, grandfathered) message lists. A broken or
    known-moved token registered in the allowlist is reported as grandfathered
    (informational); an unregistered one is actionable. Allowlist entries
    whose token no longer occurs in the header are reported as broken — the
    registry must shrink with the backlog, not outlive it.
    """
    if allowlist is None:
        allowlist = HEADER_STALE_ALLOWLIST
    key = _header_allowlist_key(path)
    entries = allowlist.get(key, {})

    broken: list[str] = []
    stale: list[str] = []
    grandfathered: list[str] = []

    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        broken.append(f"{key}: cannot read: {e}")
        return broken, stale, grandfathered

    seen: set[str] = set()
    for token, line in extract_header_doc_refs(text):
        clean = strip_line_ref(token)
        moved = is_known_moved(clean)
        if (ROOT / clean).exists():
            continue
        if moved:
            msg = (
                f"{key}:{line}: STALE HEADER DOC REF: `{clean}` "
                f"-> should reference `{moved}`"
            )
            if clean in entries:
                grandfathered.append(msg + f" [grandfathered: {entries[clean]}]")
                seen.add(clean)
            else:
                stale.append(msg)
        else:
            msg = f"{key}:{line}: BROKEN HEADER DOC REF: `{clean}`"
            if clean in entries:
                grandfathered.append(msg + f" [grandfathered: {entries[clean]}]")
                seen.add(clean)
            else:
                broken.append(msg)

    for token in sorted(set(entries) - seen):
        broken.append(
            f"{key}: HEADER ALLOWLIST ZOMBIE: `{token}` no longer appears in "
            f"this header (remove the registry entry)"
        )
    return broken, stale, grandfathered


def main() -> int:
    all_broken = []
    all_stale = []
    all_historical = []
    all_envcond = []
    all_grandfathered = []
    scanned_header_keys = set()

    for f in SCAN_FILES:
        if not f.exists():
            print(f"WARNING: scan target does not exist: {f}")
            continue
        if should_skip(f):
            continue
        b, s, h, e = check_file(f)
        all_broken.extend(b)
        all_stale.extend(s)
        all_historical.extend(h)
        all_envcond.extend(e)

    for f in HEADER_SCAN_FILES:
        if not f.exists():
            print(f"WARNING: header scan target does not exist: {f}")
            continue
        scanned_header_keys.add(_header_allowlist_key(f))
        hb, hs, hg = check_header_file(f)
        all_broken.extend(hb)
        all_stale.extend(hs)
        all_grandfathered.extend(hg)

    # Registry hygiene: entries naming headers that are no longer scanned
    # (renamed/moved/deleted) are zombies — the backlog must stay anchored to
    # the real tree.
    for key in sorted(set(HEADER_STALE_ALLOWLIST) - scanned_header_keys):
        all_broken.append(
            f"{key}: HEADER ALLOWLIST ZOMBIE: header is not in the scan set "
            f"(remove the registry entry)"
        )

    # Report
    print("=" * 60)
    print("check-doc-links.py — documentation link validation")
    print("=" * 60)

    if all_broken:
        print(f"\nBROKEN_MARKDOWN_LINKS/BROKEN_BACKTICK_PATHS: {len(all_broken)}")
        for item in all_broken:
            print(f"  {item}")
    else:
        print(f"\nBROKEN_MARKDOWN_LINKS: 0")

    if all_stale:
        print(f"\nSTALE_REPOSITORY_PATHS: {len(all_stale)}")
        for item in all_stale:
            print(f"  {item}")
    else:
        print(f"STALE_REPOSITORY_PATHS: 0")

    if all_envcond:
        print(f"\nENVIRONMENT-CONDITIONAL (gitignored, present locally, absent in CI): {len(all_envcond)}")
        for item in all_envcond:
            print(f"  {item}")
    else:
        print(f"ENVIRONMENT-CONDITIONAL: 0")

    if all_historical:
        print(f"\nHISTORICAL_REFERENCES (allowlisted, informational): {len(all_historical)}")
        for item in all_historical:
            print(f"  {item}")
    else:
        print(f"HISTORICAL_REFERENCES: 0")

    if all_grandfathered:
        print(f"\nHEADER_DOC_REFERENCES (grandfathered #167 Step 4 backlog, informational): {len(all_grandfathered)}")
        for item in all_grandfathered:
            print(f"  {item}")
    else:
        print(f"HEADER_DOC_REFERENCES (grandfathered): 0")

    # Final verdict
    problems = len(all_broken) + len(all_stale)
    print(f"\n{'=' * 60}")
    if problems == 0:
        print("VERDICT: PASS — no broken links, no stale paths")
        return 0
    else:
        print(f"VERDICT: FAIL — {problems} actionable problem(s)")
        return min(problems, 255)


def self_test() -> int:
    """Negative tests for the checker itself.

    Verifies that the checker actually catches:
      1. A deliberately broken markdown link (non-existent target).
      2. A deliberately stale moved-path reference (known-moved doc).
      3. A markdown link that matches a former NON_PATH_PATTERNS entry
         (e.g. `e12-event.md`) — must be flagged, not silently skipped.
      8. A public-header docs/ reference scan miss: stale-moved token, broken
         token, URL-embedded docs/ (must be excluded), comment-wrapped token
         (must be joined) — the PR #207 review finding 1 regression guard.
      9. The header grandfather registry: registered tokens pass as
         grandfathered; zombie entries (token gone from the header) fail.

    Returns the number of test failures (0 = all passed).
    """
    import tempfile

    failures = 0

    # --- Test 1: deliberately broken markdown link ---
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", delete=False, encoding="utf-8"
    ) as f:
        f.write("[broken](docs/does_not_exist_anywhere.md)\n")
        f.write("[good](README.md)\n")
        tmp1 = Path(f.name)

    b, s, _, _ = check_file(tmp1)
    if not any("docs/does_not_exist_anywhere.md" in x for x in b):
        print("SELF-TEST FAIL: broken markdown link not detected")
        failures += 1
    else:
        print("SELF-TEST PASS: broken markdown link detected")
    tmp1.unlink()

    # --- Test 2: lambda-looking code inside a fenced block is NOT a link ---
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", delete=False, encoding="utf-8"
    ) as f:
        f.write("```cpp\n")
        f.write('rt.submit([&fd](RuntimeTaskContext& ctx) { return 0; });\n')
        f.write("```\n")
        f.write("[good](README.md)\n")
        tmp2 = Path(f.name)

    b, s, _, _ = check_file(tmp2)
    if any("RuntimeTaskContext" in x for x in b + s):
        print("SELF-TEST FAIL: lambda in fenced code flagged as a link")
        failures += 1
    else:
        print("SELF-TEST PASS: fenced code lambda not flagged as a link")
    tmp2.unlink()

    # --- Test 3: deliberately stale moved-path reference ---
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", delete=False, encoding="utf-8"
    ) as f:
        f.write("[stale](docs/e12-event.md)\n")
        tmp3 = Path(f.name)

    b, s, _, _ = check_file(tmp3)
    if not any("docs/e12-event.md" in x for x in s):
        print("SELF-TEST FAIL: stale moved path not detected")
        failures += 1
    else:
        print("SELF-TEST PASS: stale moved path detected")
    tmp3.unlink()

    # --- Test 4: doc-relative old name (formerly masked by NON_PATH_PATTERNS) ---
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", delete=False, encoding="utf-8"
    ) as f:
        # e12-event.md was in NON_PATH_PATTERNS; now it must be flagged.
        f.write("[masked](e12-event.md)\n")
        tmp3 = Path(f.name)

    b, s, _, _ = check_file(tmp3)
    flagged = any("e12-event.md" in x for x in b) or any("e12-event.md" in x for x in s)
    if not flagged:
        print("SELF-TEST FAIL: formerly-masked doc name not detected")
        failures += 1
    else:
        print("SELF-TEST PASS: formerly-masked doc name detected")
    tmp3.unlink()

    # --- Test 4: markdown link is NEVER skipped by is_non_path heuristic ---
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", delete=False, encoding="utf-8"
    ) as f:
        # `e12-sync-primitives-plan.md` was in NON_PATH_PATTERNS; as a
        # markdown link it must now be flagged as broken/stale.
        f.write("[plan](e12-sync-primitives-plan.md)\n")
        tmp4 = Path(f.name)

    b, s, _, _ = check_file(tmp4)
    flagged = any("e12-sync-primitives-plan.md" in x for x in b) or \
              any("e12-sync-primitives-plan.md" in x for x in s)
    if not flagged:
        print("SELF-TEST FAIL: markdown link to old doc name was silently skipped")
        failures += 1
    else:
        print("SELF-TEST PASS: markdown link to old doc name is flagged")
    tmp4.unlink()

    # --- Test 5: nested doc with repo-root-looking link (P1-04 regression) ---
    # This is the critical regression test: a markdown link that looks like a
    # repo-root-relative path (e.g. `docs/spec/e12_event/`) but is in a nested
    # document. GitHub resolves it relative to the current document, so it
    # becomes `docs/history/closeout/docs/spec/e12_event/` — which is broken.
    # The checker must NOT silently resolve it against the repo root.
    with tempfile.TemporaryDirectory() as tmpdir:
        nested_dir = Path(tmpdir) / "docs" / "history" / "closeout"
        nested_dir.mkdir(parents=True)
        nested_file = nested_dir / "test-nested.md"
        # This link exists at the repo root (docs/spec/e12_event/) but NOT
        # relative to the nested document. It must be flagged as broken.
        nested_file.write_text("[spec](docs/spec/e12_event/)\n", encoding="utf-8")

        b, s, _, _ = check_file(nested_file)
        if not any("docs/spec/e12_event/" in x for x in b):
            print("SELF-TEST FAIL: nested doc repo-root-looking link was silently resolved")
            failures += 1
        else:
            print("SELF-TEST PASS: nested doc repo-root-looking link is flagged as broken")

    # --- Test 6: scan set excludes untracked / gitignored Markdown ---
    # Structural regression guard: the scan set MUST come from `git ls-files`
    # so generated Markdown under docs/ (e.g. build artifacts) is never
    # validated. A stray untracked .md in the work tree must not appear in
    # SCAN_FILES. We cannot run this when git is unavailable.
    import shutil as _shutil
    if _shutil.which("git") is not None:
        # SCAN_FILES is resolved at import; reach into _discover_scan_files
        # with a temp work tree to exercise the exclusion directly.
        import subprocess as _sp
        try:
            in_tree = _sp.run(
                ["git", "-C", str(ROOT), "rev-parse", "--is-inside-work-tree"],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
        except _sp.CalledProcessError:
            in_tree = "false"
        if in_tree == "true":
            untracked = ROOT / "docs" / "_self_test_untracked_probe.md"
            existed = untracked.exists()
            try:
                untracked.write_text("# probe\n", encoding="utf-8")
                scan_paths = {p.resolve() for p in _discover_scan_files()}
                if untracked.resolve() in scan_paths:
                    print("SELF-TEST FAIL: untracked Markdown leaked into scan set")
                    failures += 1
                else:
                    print("SELF-TEST PASS: untracked Markdown excluded from scan set")
            finally:
                if not existed:
                    untracked.unlink(missing_ok=True)

    # --- Test 7: refs to intentionally-untracked dirs are environment-conditional ---
    # Documents like docs/adr/ADR-execution-model.md reference the Zig design
    # reference tree (zig/, gitignored per AGENTS.md §1) via `Io/fiber.zig`
    # etc. These resolve locally (ZIG_ROOT) but NOT in CI (actions/checkout
    # does not materialize gitignored content). Such refs MUST be reported as
    # ENVIRONMENT-CONDITIONAL notes, never as BROKEN — otherwise the docs gate
    # is structurally un-enforceable in CI. The same applies to local-only
    # artifact dirs (.c-review-results/, .agents/, states/).
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".md", delete=False, encoding="utf-8"
    ) as f:
        # A Zig-ref-shaped backtick that cannot resolve (no zig/ in this
        # scratch context). Must land in envcond, not broken.
        f.write("see `Io/fiber.zig` and `.c-review-results/run.md`\n")
        tmp7 = Path(f.name)

    b, s, _, e = check_file(tmp7)
    eco_hits = [x for x in e if "ENVIRONMENT-CONDITIONAL" in x]
    broken_leaks = [x for x in b if "Io/fiber.zig" in x or ".c-review-results" in x]
    if eco_hits and not broken_leaks:
        print("SELF-TEST PASS: gitignored-dir refs reported as environment-conditional")
    else:
        if broken_leaks:
            print("SELF-TEST FAIL: gitignored-dir refs leaked into broken set")
        else:
            print("SELF-TEST FAIL: gitignored-dir refs not flagged as environment-conditional")
        failures += 1
    tmp7.unlink(missing_ok=True)

    # --- Test 8: public-header docs/ reference scan (PR #207 review finding 1) ---
    # Root cause this guards against: stale `docs/...` pointers in public
    # headers historically escaped every gate because the checker scanned
    # Markdown only. The header scan must catch stale-moved and broken tokens,
    # must NOT flag docs/ embedded in URLs, and must join comment-wrapped
    # tokens before resolving.
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".hpp", delete=False, encoding="utf-8"
    ) as f:
        f.write("// See docs/e12-event.md for the model.\n")
        f.write("// Broken: docs/reference/does_not_exist_anywhere.md\n")
        f.write("// URL-embedded: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html\n")
        f.write("// Wrapped: docs/e10-waitnode-wait-\n")
        f.write("// queue.md\n")
        tmp8 = Path(f.name)

    hb, hs, _ = check_header_file(tmp8, allowlist={})
    ok_stale = any("docs/e12-event.md" in x for x in hs)
    ok_broken = any("docs/reference/does_not_exist_anywhere.md" in x for x in hb)
    ok_url = not any("ThreadSafetyAnalysis" in x for x in hb + hs)
    ok_wrap = any("docs/e10-waitnode-wait-queue.md" in x for x in hs)
    if ok_stale and ok_broken and ok_url and ok_wrap:
        print("SELF-TEST PASS: header stale/broken/URL/wrap classification correct")
    else:
        print(
            "SELF-TEST FAIL: header scan classification "
            f"(stale={ok_stale} broken={ok_broken} url_excluded={ok_url} wrap_joined={ok_wrap})"
        )
        failures += 1
    tmp8.unlink(missing_ok=True)

    # --- Test 9: header grandfather registry is honored AND self-pruning ---
    # A registered stale token passes as grandfathered; a registry entry whose
    # token no longer occurs in the header is itself an error (no zombie
    # amnesty for the #167 Step 4 backlog).
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".hpp", delete=False, encoding="utf-8"
    ) as f:
        f.write("// docs/e12-event.md\n")
        tmp9 = Path(f.name)

    key9 = tmp9.resolve().as_posix()
    al9 = {key9: {
        "docs/e12-event.md": "test grandfather reason",
        "docs/totally-gone.md": "test zombie entry",
    }}
    hb, hs, hg = check_header_file(tmp9, allowlist=al9)
    ok_grand = any("docs/e12-event.md" in x for x in hg) and not hs
    ok_zombie = any("HEADER ALLOWLIST ZOMBIE" in x and "docs/totally-gone.md" in x for x in hb)
    if ok_grand and ok_zombie:
        print("SELF-TEST PASS: header grandfather honored and zombie entry detected")
    else:
        print(
            "SELF-TEST FAIL: header registry semantics "
            f"(grandfathered={ok_grand} zombie_detected={ok_zombie})"
        )
        failures += 1
    tmp9.unlink(missing_ok=True)

    return failures


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
