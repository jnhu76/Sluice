#!/usr/bin/env bash
# Sluice local pre-push quality gate.
#
# PURPOSE
#   Catch deterministic mechanical failures — documentation link validation,
#   architecture-doc structure, the backend-conformance manifest self-test, and
#   whitespace damage — BEFORE a push consumes a GitHub CI round trip. This is
#   developer tooling only.
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
# Gate 5: mechanical facts (identifier near-miss, doc LOC claims, split
# layout, SHA/tracker references) — self-test then repository scan.
#
# Added post-freeze (issue #113 / PR #114 review): AI-authored changes can be
# semantically sound while getting spelling, counting, and cross-references
# wrong; those facts are checked mechanically, not by re-review. The
# self-test plants one violation per detector and requires each to fire
# (fail-closed against a broken checker), then the real scan runs against
# the working tree / committed docs.
MECH_FACTS_SELFTEST_REPRO="python3 scripts/gates/mechanical-facts.py --self-test"
run_gate "mechanical facts self-test" "${MECH_FACTS_SELFTEST_REPRO}" \
    python3 scripts/gates/mechanical-facts.py --self-test

MECH_FACTS_REPRO="python3 scripts/gates/mechanical-facts.py"
run_gate "mechanical facts" "${MECH_FACTS_REPRO}" \
    python3 scripts/gates/mechanical-facts.py

# ---------------------------------------------------------------------------
# Gate 6: whitespace / conflict-marker damage across the PUSHED ranges.
#
# `git diff --check` reports trailing whitespace, indentation with spaces
# before tabs, and unresolved merge conflict markers.
#
# A real pre-push invocation receives the pushed ref-pairs on stdin, one line
# per ref:
#     <local-ref> <local-sha> <remote-ref> <remote-sha>
# For each pair we check the actual pushed range "<remote-sha>..<local-sha>"
# so damage in the commits being pushed is caught even when the working tree
# happens to be clean (a working-tree-only check would miss it).
#
# When invoked manually (no stdin, e.g. `bash scripts/gates/pre-push.sh`) there
# are no ref-pairs to read. In that case fall back to checking the staged +
# working tree so the script stays a useful manual gate. Detect this by reading
# stdin into an array; an empty array means manual invocation.
DIFF_CHECK_LABEL="git diff --check (pushed ranges)"

# Read all stdin ref-pair lines up front so we can decide range-vs-tree mode.
# `mapfile` returns an empty array when stdin is not a pipe / is empty, which
# is exactly the manual-invocation case.
mapfile -t PUSH_REF_PAIRS

# Build the set of `git diff --check` revisions to evaluate:
#   - hook mode (ref-pairs present): one range per pair, "<remote>..<local>"
#   - manual mode (no ref-pairs):    a single empty-args working-tree check
DIFF_ARGS=()
if [ "${#PUSH_REF_PAIRS[@]}" -gt 0 ]; then
    for pair in "${PUSH_REF_PAIRS[@]}"; do
        # pair = "<local-ref> <local-sha> <remote-ref> <remote-sha>"
        # shellcheck disable=SC2086  # intentional word-splitting on 4 fields
        set -- $pair
        local_sha="${2:-}"
        remote_sha="${4:-}"
        # Skip refs being deleted (local_sha all zeros): no range to check.
        case "$local_sha" in
            0000000000000000000000000000000000000000) continue ;;
        esac
        # New branch (remote all zeros): diff against that local commit's full
        # tree. Otherwise check the pushed range "<remote>..<local>".
        if [ "$remote_sha" = "0000000000000000000000000000000000000000" ]; then
            DIFF_ARGS+=("$local_sha")
        else
            DIFF_ARGS+=("${remote_sha}..${local_sha}")
        fi
    done
    # If every pair was a deletion, there is nothing to check; pass this gate.
    if [ "${#DIFF_ARGS[@]}" -eq 0 ]; then
        echo "==> pre-push gate: ${DIFF_CHECK_LABEL}"
        echo "    (no pushable content: all refs deleted; nothing to check)"
    else
        # Reproduction for a hook-mode failure lists the exact ranges checked.
        DIFF_CHECK_REPRO="git diff --check ${DIFF_ARGS[*]}"
        run_gate "${DIFF_CHECK_LABEL}" "${DIFF_CHECK_REPRO}" \
            git diff --check "${DIFF_ARGS[@]}"
    fi
else
    # Manual invocation: no stdin ref-pairs. Fall back to the staged + working
    # tree so `bash scripts/gates/pre-push.sh` remains a useful pre-push probe.
    DIFF_CHECK_LABEL="git diff --check (working tree; manual invocation)"
    DIFF_CHECK_REPRO="git diff --check"
    echo "==> pre-push gate: ${DIFF_CHECK_LABEL}"
    echo "    (no stdin ref-pairs; checking staged + working tree)"
    run_gate "${DIFF_CHECK_LABEL}" "${DIFF_CHECK_REPRO}" \
        git diff --check
fi

# ---------------------------------------------------------------------------
echo "==> pre-push gate: ALL CHECKS PASSED"
exit 0
