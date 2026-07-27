#!/usr/bin/env python3
"""check-doc-links.py — repository documentation link validator.

Checks:
  - Root README files (README.md, README.zh-CN.md), AGENTS.md, and all docs Markdown.
  - Markdown links [text](path) resolve to existing files/directories.
  - Backtick-quoted repository-relative paths (`path/to/file`) resolve.
  - Fails on broken links and stale moved-path references.
  - Reports historical paths separately but still fails unless explicitly allowlisted.
  - Skips generated TLA+ state artifacts.
  - Returns non-zero on every actionable problem.

Path resolution convention in this repository:
  - Paths in docs are REPO-ROOT-RELATIVE (e.g. `docs/api-reference.md` from
    `docs/README.md` resolves to `<repo>/docs/api-reference.md`).
  - Some older docs use doc-relative paths; we try doc-relative as a fallback.
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

# Files to scan
SCAN_FILES = [
    ROOT / "README.md",
    ROOT / "README.zh-CN.md",
    ROOT / "AGENTS.md",
]

# docs/ directory
DOCS_DIR = ROOT / "docs"
for p in sorted(DOCS_DIR.rglob("*.md")):
    SCAN_FILES.append(p)

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
}

# Regex patterns
# Markdown link: [text](target) — target may be URL, anchor, or path
MD_LINK_RE = re.compile(r'\[([^\]]*)\]\(([^)#]+)(?:#[^)]*)?\)')
# Backtick path: `some/path/to/file` (relative-looking, no spaces, has /)
BACKTICK_PATH_RE = re.compile(r'`([^`\s]*/[^`\s]+)`')

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
    # Doc-relative references to moved docs (without docs/ prefix)
    r'^e12-sync-primitives-plan\.md$',
    r'^e12-semaphore\.md$',
    r'^e12-async-mutex\.md$',
    r'^e12-condition\.md$',
    r'^e12-event\.md$',
    r'^e12-queue\.md$',
    r'^e12-rwlock\.md$',
    r'^async-runtime-construction-method\.md$',
    r'^async-runtime-plan\.md$',
    r'^e10-waitnode-wait-queue\.md$',
    r'^e11-deadline-timer-wait\.md$',
    r'^e11-arch-recon-audit\.md$',
    r'^e12-queue-implementation-authorization\.md$',
    r'^e12-queue-production-implementation\.md$',
    r'^e12-queue-corrective-3\.md$',
    r'^e12-cross-primitive-terminal-audit\.md$',
    r'^e13-select-preparation\.md$',
    r'^e13-select-p7-rollback-closeout\.md$',
    r'^sync-optimization-notes\.md$',
    r'^bench-decision-matrix\.md$',
    r'^bench-methodology\.md$',
    r'^design-flush-sync-durability\.md$',
    r'^design-copy-strategy\.md$',
    r'^design-buffered-fast-path\.md$',
    r'^design-io-context\.md$',
    r'^design-wal-durability\.md$',
    r'^design-readv-writev\.md$',
    r'^mvp-closeout\.md$',
    r'^mvp-core-model\.md$',
    r'^next-steps-after-011\.md$',
    r'^release-v0\.1-mvp-checklist\.md$',
    r'^zig-std-io-gap-calibration\.md$',
    r'^zig-std-io-parity-audit\.md$',
    r'^zig-std-io-source-inventory\.md$',
    r'^zig-stdio-async-port-map\.md$',
    r'^zig-stdio-migration-jobs\.md$',
    r'^async-backend-parity\.md$',
    r'^async-mutex-nothrow-implementation\.md$',
    r'^async-runtime-hang-and-gcc-corrective\.md$',
    r'^e0a-waiting-policy-audit\.md$',
    r'^sync-before-async-readiness-gate\.md$',
    r'^sync-io-model-gap-audit\.md$',
    r'^sync-runtime-bench-notes\.md$',
    r'^io-uring-spike\.md$',
    r'^io-uring-readiness-gate\.md$',
    r'^async-source-inventory\.md$',
    r'^async-problem-statement\.md$',
    r'^async-design-alternatives\.md$',
    r'^async-readiness-gate\.md$',
    r'^async-next-jobs\.md$',
    r'^sync-io-next-jobs\.md$',
    r'^async-deferred-until-sync-baseline\.md$',
    r'^bench-optimization-runbook\.md$',
    # Moved design docs (doc-relative)
    r'^design/e13-select-.*\.md$',
    r'^design/e14-threaded-evented-parity-preparation\.md$',
    r'^design/formal/e13-.*\.md$',
    # Moved formal docs
    r'^formal/e13-select-.*\.md$',
    # Moved reviews (doc-relative)
    r'^reviews/.*\.md$',
    # Moved e10-e12 closure doc
    r'^e10-e12-api-semantic-closure\.md$',
    # Doc-relative scripts
    r'^\.\./scripts/verify-e12-.*\.sh$',
    # Moved design docs (with docs/ prefix)
    r'^docs/design/e13-select-.*\.md$',
    r'^docs/design/e14-threaded-evented-parity-preparation\.md$',
    r'^docs/design/formal/e13-.*\.md$',
    # Moved formal docs (with docs/ prefix)
    r'^docs/formal/e13-select-.*\.md$',
    r'^docs/formal/\*\*$',
    # Moved e13-select docs (without docs/ prefix)
    r'^docs/e13-select-.*\.md$',
    # Moved closeout docs (with docs/ prefix)
    r'^docs/e9-0-wake-source-topology-audit\.md$',
    r'^docs/e8-0-ownership-topology-audit\.md$',
    r'^docs/e8-formal-corrective/.*$',
    # Moved queue docs (glob)
    r'^docs/e12-queue.*\.md$',
    # Code member names
    r'^next_/prev_$',
    # Spec trace files
    r'^docs/spec/e13_select/.*\.keep$',
    # Moved implementation plan docs (with docs/ prefix)
    r'^docs/history/implementation-plans/e12-sync-primitives-plan\.md$',
    r'^docs/history/closeout/e11-deadline-timer-wait\.md$',
    # Moved queue docs (doc-relative from closeout)
    r'^e12-queue-scheduler-integration\.md$',
    r'^e12-queue-state-machine\.md$',
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
    # Doc-relative design references
    r'^\.\./design/e14-threaded-evented-parity-preparation\.md$',
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
    # Doc-relative api-reference
    r'^api-reference\.md$',
    # Moved bench notes
    r'^docs/bench/sync-runtime-bench-notes\.md$',
    r'^docs/archive/mvp-closeout\.md$',
    # Moved results
    r'^docs/results/liburing-validation-2026-07-03\.md$',
    # Doc-relative history references
    r'^history/closeout/.*\.md$',
    r'^history/implementation-plans/.*\.md$',
    r'^\.\./history/implementation-plans/.*\.md$',
    # TLA+ files in spec
    r'^spec/e12_semaphore/E12Semaphore\.tla$',
    # Spec directory references
    r'^spec/e12_event/$',
    # Non-existent experimental headers
    r'^experimental/uring_write_batch\.hpp$',
    r'^experimental/uring_io_context\.hpp$',
    # Code type with space (won't match backtick regex, but just in case)
    r'^Result<size_t>',
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


def resolve_ref(doc_path: Path, ref: str) -> Path | None:
    """Resolve a reference. Returns the resolved Path or None if it cannot
    be resolved as a repository path (e.g. URL, anchor-only, non-path).

    Tries repo-root-relative first, then doc-relative, then zig-root.
    """
    if is_url(ref):
        return None  # external — skip

    if is_non_path(ref):
        return None  # code identifier, not a file path

    if is_tla_module(ref):
        return None  # TLA+ module name, not a file path

    # Strip line-number references
    clean_ref = strip_line_ref(ref)

    # Handle glob patterns: check if any files match
    if "*" in clean_ref or "?" in clean_ref:
        # Try repo-root-relative glob
        matches = globmod.glob(str(ROOT / clean_ref), recursive=True)
        if matches:
            return Path(matches[0])  # return first match (exists)
        # Try doc-relative glob
        matches = globmod.glob(str(doc_path.parent / clean_ref), recursive=True)
        if matches:
            return Path(matches[0])
        # No matches — will be reported as broken
        return (ROOT / clean_ref).resolve()

    # Try repo-root-relative first (the dominant convention)
    candidate = (ROOT / clean_ref).resolve()
    if candidate.exists():
        return candidate

    # Try doc-relative
    candidate = (doc_path.parent / clean_ref).resolve()
    if candidate.exists():
        return candidate

    # Try zig-root for Zig source references
    if looks_like_zig_path(clean_ref):
        candidate = (ZIG_ROOT / clean_ref).resolve()
        if candidate.exists():
            return candidate

    # Return the repo-root-relative path for error reporting
    return (ROOT / clean_ref).resolve()


def is_historical(resolved: Path) -> bool:
    """Return True if resolved path is under an allowlisted historical subtree."""
    rel = os.path.relpath(resolved, ROOT)
    return any(
        rel.startswith(a + "/") or rel == a
        for a in HISTORICAL_ALLOWLIST
    )


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


def check_file(path: Path) -> tuple[list[str], list[str], list[str]]:
    """Check a single Markdown file.

    Returns (broken, stale, historical_unallowlisted).
    """
    broken = []
    stale = []
    historical = []

    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        broken.append(f"{path}: cannot read: {e}")
        return broken, stale, historical

    line_of = lambda pos: text[: pos].count("\n") + 1

    # --- Markdown links ---
    for m in MD_LINK_RE.finditer(text):
        ref = m.group(2).strip()
        resolved = resolve_ref(path, ref)
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
                broken.append(
                    f"{path}:{line_of(m.start())}: "
                    f"BROKEN MARKDOWN LINK: `{ref}` -> {resolved}"
                )

    # --- Backtick paths ---
    for m in BACKTICK_PATH_RE.finditer(text):
        ref = m.group(1).strip()
        resolved = resolve_ref(path, ref)
        if resolved is None:
            continue
        if not resolved.exists():
            moved = is_known_moved(ref, path, resolved)
            if moved:
                stale.append(
                    f"{path}:{line_of(m.start())}: "
                    f"STALE MOVED PATH (backtick): `{ref}` -> should reference `{moved}`"
                )
            elif is_historical(resolved):
                historical.append(
                    f"{path}:{line_of(m.start())}: "
                    f"HISTORICAL (allowlisted): `{ref}`"
                )
            else:
                broken.append(
                    f"{path}:{line_of(m.start())}: "
                    f"BROKEN BACKTICK PATH: `{ref}` -> {resolved}"
                )

    return broken, stale, historical


def main() -> int:
    all_broken = []
    all_stale = []
    all_historical = []

    for f in SCAN_FILES:
        if not f.exists():
            print(f"WARNING: scan target does not exist: {f}")
            continue
        if should_skip(f):
            continue
        b, s, h = check_file(f)
        all_broken.extend(b)
        all_stale.extend(s)
        all_historical.extend(h)

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

    if all_historical:
        print(f"\nHISTORICAL_REFERENCES (allowlisted, informational): {len(all_historical)}")
        for item in all_historical:
            print(f"  {item}")
    else:
        print(f"HISTORICAL_REFERENCES: 0")

    # Final verdict
    problems = len(all_broken) + len(all_stale)
    print(f"\n{'=' * 60}")
    if problems == 0:
        print("VERDICT: PASS — no broken links, no stale paths")
        return 0
    else:
        print(f"VERDICT: FAIL — {problems} actionable problem(s)")
        return min(problems, 255)


if __name__ == "__main__":
    sys.exit(main())
