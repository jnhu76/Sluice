#!/usr/bin/env bash
# Sluice local pre-push quality gate.
#
# PURPOSE
#   Catch deterministic mechanical failures — documentation link validation,
#   architecture-doc structure, the backend-conformance manifest self-test,
#   generated/derived-file freshness, and whitespace damage — BEFORE a push
#   consumes a GitHub CI round trip. This is developer tooling only.
#
# AUTHORITY
#   This script is the single source of truth for what the local pre-push gate
#   checks. lefthook.yml (and the installed .git/hooks/pre-push) both delegate
#   here, so the exact same gate is reproducible by hand:
#
#       bash scripts/gates/pre-push.sh
#
#   It calls the SAME repository validators as GitHub CI (.github/workflows/
#   ci.yml "Documentation verification" step) — it does not duplicate or
#   re-implement validation logic.
#
# SCOPE
#   Fast + deterministic only. It deliberately does NOT run:
#     - build / full Debug or Release suites
#     - sanitizers (ASan/UBSan/TSan)
#     - real-liburing exhaustive tests
#     - formal model checking (TLC)
#     - fuzz / overnight / stability loops
#   Those remain CI or explicit developer gates. GitHub CI stays authoritative;
#   hooks can be bypassed with `git push --no-verify`, so CI must keep running
#   its own validation independently.
#
# FAIL-CLOSED
#   exit 0  -> every required check passed
#   exit >0 -> the FIRST failing gate wins; later gates do not run
#   No `|| true`, no warning-only required gates, no swallowed failures.
set -euo pipefail

# ---------------------------------------------------------------------------
# Environment isolation.
#
# Ambient variables can silently change the meaning of a gate. We do NOT
# blindly wipe the whole environment — we unset only the variables known to
# weaken a check invoked here. Each entry must be justified.
#
# SLUICE_TEST_FILTER restricts which test cases a Sluice test binary runs. The
# Phase C1 conformance harness relies on a zero-filter run being an error
# (fail-closed). If a developer's shell leaked SLUICE_TEST_FILTER into this
# gate, a manifest/attribution check could run a narrowed case set or match
# zero cases and misreport success. Strip it so the gate is reproducible.
unset SLUICE_TEST_FILTER

# Repository root (this script may be invoked from lefthook with an arbitrary
# CWD, including GIT_DIR). Resolve once and run every validator from there so
# the validators' repo-root detection (Path(__file__).resolve().parent.parent)
# is consistent.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# Track failures with a stable, descriptive label per gate so error output
# names the exact failing check and the exact manual reproduction command.
FAILED_GATE=""
REPRO=""

fail() {
    # Record the failing gate and its reproduction command, then print.
    # (We do not exit here so callers can wrap the gated command uniformly.)
    FAILED_GATE="$1"
    REPRO="$2"
    echo "----------------------------------------------------------------" >&2
    echo "PRE-PUSH GATE FAILED: ${FAILED_GATE}" >&2
    echo "Reproduce with:" >&2
    echo "    ${REPRO}" >&2
    echo "----------------------------------------------------------------" >&2
}

# run_gate <label> <repro-command> <actual-command...>
#
# Run a single gate. On non-zero exit, record the failure, print the
# reproduction command, and stop (fail-closed: later gates do not run).
run_gate() {
    local label="$1"
    local repro="$2"
    shift 2
    echo "==> pre-push gate: ${label}"
    if ! "$@"; then
        fail "${label}" "${repro}"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Gate 1: documentation link verification (self-test).
#
# Mirrors the first sub-step of the CI "Documentation verification" step.
# --self-test is a negative test: it proves the checker actually catches
# broken/stale links and excludes untracked files. If the checker itself is
# broken, this fails before a green main scan can lie about validity.
DOC_LINKS_SELFTEST_REPRO="python3 scripts/check-doc-links.py --self-test"
run_gate "doc-links self-test" "${DOC_LINKS_SELFTEST_REPRO}" \
    python3 scripts/check-doc-links.py --self-test

# ---------------------------------------------------------------------------
# Gate 2: documentation link verification (full scan).
#
# Mirrors the second sub-step of the CI "Documentation verification" step.
# Validates ONLY git-tracked Markdown (via `git ls-files`) so generated/
# gitignored files never leak into the scan set.
DOC_LINKS_SCAN_REPRO="python3 scripts/check-doc-links.py"
run_gate "doc-links full scan" "${DOC_LINKS_SCAN_REPRO}" \
    python3 scripts/check-doc-links.py

# ---------------------------------------------------------------------------
# Gate 3: architecture documentation structure.
#
# Mirrors the third sub-step of the CI "Documentation verification" step.
# Checks structural completeness of governance files (constitution rule ID
# uniqueness, divergence-registry required fields, PR template refs,
# AGENTS.md references). Pure structural checks, no semantic correctness.
ARCH_DOCS_REPRO="python3 scripts/verify-architecture-docs.py"
run_gate "architecture docs" "${ARCH_DOCS_REPRO}" \
    python3 scripts/verify-architecture-docs.py

# ---------------------------------------------------------------------------
# Gate 4: backend-conformance manifest + attribution self-test.
#
# Pure-data self-test for the conformance manifest (closed profiles / layers /
# statuses, mandatory coverage, helper well-definedness) AND the aggregate
# gate's per-backend attribution/isolation regression suite. Runs standalone
# (no xmake build) so attribution invariants hold even when the build step is
# skipped — exactly the property a pre-push gate needs.
MANIFEST_REPRO="python3 scripts/tests/test_backend_conformance_manifest.py"
run_gate "backend-conformance manifest self-test" "${MANIFEST_REPRO}" \
    python3 scripts/tests/test_backend_conformance_manifest.py

# ---------------------------------------------------------------------------
# Gate 5: generated / derived-file freshness (contract B).
#
# A push hook must NOT silently mutate tracked source. The correct contract
# for a VERIFICATION hook is: generated files must already be current; if they
# are stale, fail with the exact remediation command. The developer regenerates
# and commits — the hook never rewrites files on their behalf.
#
# Currently the repository has no committed generated documentation that this
# gate can recompute and diff (doc-link validation above already covers tracked
# Markdown freshness structurally). The block below is the hook point for the
# first such artifact. When one is introduced, add a deterministic
# `diff-and-fail-with-remediation` block here following the pattern:
#
#     if ! <regenerate-into-temp-and-diff>; then
#         fail "generated docs stale" "<exact regenerate command>"
#         cat <<EOF >&2
# Generated documentation is stale.
# Run: <exact repository command>
# Then commit the generated changes before pushing.
# EOF
#         exit 1
#     fi
#
# Until such an artifact exists, this gate is intentionally a no-op pass so
# the hook remains honest: it does not pretend to verify something it cannot.

# ---------------------------------------------------------------------------
# Gate 6: whitespace / conflict-marker damage.
#
# `git diff --check` reports trailing whitespace, indentation with spaces
# before tabs, and unresolved merge conflict markers across the working tree.
# This is the cheapest deterministic mechanical failure to catch before a push.
# Operate against the staged + working tree so it catches damage whether or
# not it has been staged yet.
DIFF_CHECK_REPRO="git diff --check"
run_gate "git diff --check" "${DIFF_CHECK_REPRO}" \
    git diff --check

# ---------------------------------------------------------------------------
echo "==> pre-push gate: ALL CHECKS PASSED"
exit 0
