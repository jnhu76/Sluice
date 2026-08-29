#!/usr/bin/env bash
# Sluice local pre-push quality gate.
#
# PURPOSE
#   Catch deterministic mechanical failures — documentation link validation,
#   architecture-doc structure, the backend-conformance manifest self-test,
#   mechanical facts, performance-evidence structural validation, the
#   changed-lines assert-family policy (AGENTS.md §9.2), and whitespace
#   damage — BEFORE a push consumes a GitHub CI round trip. This is
#   developer tooling only.
#
# USAGE
#   bash scripts/gates/pre-push.sh                       # manual: staged + working tree
#   git push (lefthook)                                  # hook: pushed ref-pair ranges (stdin)
#   bash scripts/gates/pre-push.sh --range <base>..<head>  # explicit range(s), stdin not read
#
#   The --range mode exists for CI: a clean checkout has an empty
#   `git diff HEAD`, so manual mode would silently scan nothing there.
#   GitHub CI passes the pull-request range explicitly (see
#   .github/workflows/ci.yml "Repository mechanical gates" step). The range
#   may be repeated; each one is scanned by the changed-lines gates.
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
# Argument parsing (before any gate runs, so usage errors are immediate).
#
# --range <a>..<b> : explicit revision range for the changed-lines gates
#                    (whitespace check + assert-hygiene scan). Repeatable.
#                    stdin is NOT read in this mode.
EXPLICIT_DIFF_ARGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --range)
            if [ $# -lt 2 ]; then
                echo "pre-push.sh: --range requires a <base>..<head> argument" >&2
                exit 1
            fi
            EXPLICIT_DIFF_ARGS+=("$2")
            shift 2
            ;;
        *)
            echo "pre-push.sh: unknown argument: $1" >&2
            echo "usage: bash scripts/gates/pre-push.sh [--range <base>..<head>]..." >&2
            exit 1
            ;;
    esac
done

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
# uniqueness, divergence-registry required fields, implementation-map target
# references, PR template refs, AGENTS.md references). Pure structural checks,
# no semantic correctness.
ARCH_DOCS_SELFTEST_REPRO="python3 scripts/verify-architecture-docs.py --self-test"
run_gate "architecture docs self-test" "${ARCH_DOCS_SELFTEST_REPRO}" \
    python3 scripts/verify-architecture-docs.py --self-test

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
# Gate 5b: performance-evidence machine enforcement.
#
# docs/verification/performance-engineering.md requires performance claims
# to be backed by machine-readable artifacts, not prose. Two self-tests
# prove the instrument works (runner pure logic; validator detectors fire
# on planted violations), then the committed evidence artifacts under
# docs/results/performance-attribution/ are structurally validated: git SHA
# + dirty state (+ provenance note when dirty), release build, environment
# fingerprint, workload params, raw samples, and statistics consistent with
# the samples. Absolute speed thresholds deliberately stay out of gates.
PERF_RUNNER_SELFTEST_REPRO="python3 scripts/bench/perf-attribution.py self-test"
run_gate "perf-attribution runner self-test" "${PERF_RUNNER_SELFTEST_REPRO}" \
    python3 scripts/bench/perf-attribution.py self-test

PERF_EVIDENCE_SELFTEST_REPRO="python3 scripts/bench/perf-evidence-validate.py --self-test"
run_gate "perf evidence validator self-test" "${PERF_EVIDENCE_SELFTEST_REPRO}" \
    python3 scripts/bench/perf-evidence-validate.py --self-test

PERF_EVIDENCE_REPRO="python3 scripts/bench/perf-evidence-validate.py"
run_gate "performance evidence artifacts" "${PERF_EVIDENCE_REPRO}" \
    python3 scripts/bench/perf-evidence-validate.py

# ---------------------------------------------------------------------------
# Gate 5c: assert-hygiene self-test.
#
# Plants each violation shape (bare assert / <cassert> / <assert.h> /
# production side of a testing guard) and requires every detector to fire
# with zero false positives (static_assert, <cstdint>/<cstddef>, comments,
# context/removed lines, guarded seam lines, allowlisted paths, and tests/
# must all pass). Proves the changed-lines assert-family policy gate
# (AGENTS.md §9.2, docs/architecture/failure-model.md) actually catches.
ASSERT_HYGIENE_SELFTEST_REPRO="python3 scripts/gates/assert-hygiene.py --self-test"
run_gate "assert-hygiene self-test" "${ASSERT_HYGIENE_SELFTEST_REPRO}" \
    python3 scripts/gates/assert-hygiene.py --self-test

# ---------------------------------------------------------------------------
# Gate 5d: failure-envelope matrix (#198, child of #163 V5).
#
# docs/verification/failure-envelope.json is the machine-checkable
# phase x fault x required-outcome matrix consolidating failure-model.md
# (T1-T7) and the C2b-C2e / D2-D4 mutation-evidence layers. The gate is
# FILE-SCOPED (no diff dependence): a row going stale — unknown vocabulary,
# a VERIFIED row losing its evidence, a dangling ref/case/target/anchor
# pointer, a case/target tuple whose owning source does not support it
# (provenance: the case must occur in a source file the target builds), a
# silently green open row, a PENDING spanning row, or a phase losing its
# last VERIFIED row — fails the gate even if the row itself was not touched
# in the pushed range.
FAIL_ENV_SELFTEST_REPRO="python3 scripts/gates/failure-envelope.py --self-test"
run_gate "failure-envelope self-test" "${FAIL_ENV_SELFTEST_REPRO}" \
    python3 scripts/gates/failure-envelope.py --self-test

FAIL_ENV_REPRO="python3 scripts/gates/failure-envelope.py"
run_gate "failure-envelope matrix" "${FAIL_ENV_REPRO}" \
    python3 scripts/gates/failure-envelope.py

# ---------------------------------------------------------------------------
# Gate 6: whitespace / conflict-marker damage + assert-hygiene changed-lines
# scan across the selected revision range(s).
#
# `git diff --check` reports trailing whitespace, indentation with spaces
# before tabs, and unresolved merge conflict markers. The assert-hygiene gate
# scans the ADDED lines of the same range(s) for unregistered assert-family
# additions (AGENTS.md §9.2).
#
# Range selection, first match wins:
#   1. explicit — `--range <a>..<b>` (repeatable): scan exactly the given
#      range(s); stdin is not read. This is the CI mode: a clean checkout has
#      an empty `git diff HEAD`, so manual mode would silently scan nothing.
#   2. hook — a real pre-push invocation receives the pushed ref-pairs on
#      stdin, one per line:
#          <local-ref> <local-sha> <remote-ref> <remote-sha>
#      For each pair we scan the actual pushed range
#      "<remote-sha>..<local-sha>" so damage in the commits being pushed is
#      caught even when the working tree happens to be clean.
#   3. manual — no ranges and no stdin pairs: fall back to the staged +
#      working tree so the script stays a useful manual gate. (Reading stdin
#      only when needed also keeps interactive manual runs from blocking on a
#      tty.)
MODE=""          # "explicit" | "hook" | "manual"
DIFF_ARGS=()

if [ "${#EXPLICIT_DIFF_ARGS[@]}" -gt 0 ]; then
    MODE="explicit"
    DIFF_ARGS=("${EXPLICIT_DIFF_ARGS[@]}")
else
    # `mapfile` returns an empty array when stdin is not a pipe / is empty,
    # which is exactly the manual-invocation case.
    mapfile -t PUSH_REF_PAIRS
    if [ "${#PUSH_REF_PAIRS[@]}" -gt 0 ]; then
        MODE="hook"
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
            # New branch (remote all zeros): diff against that local commit's
            # full tree. Otherwise check the pushed range
            # "<remote-sha>..<local-sha>".
            if [ "$remote_sha" = "0000000000000000000000000000000000000000" ]; then
                DIFF_ARGS+=("$local_sha")
            else
                DIFF_ARGS+=("${remote_sha}..${local_sha}")
            fi
        done
    else
        MODE="manual"
    fi
fi

if [ "$MODE" = "manual" ]; then
    echo "==> pre-push gate: git diff --check (working tree; manual invocation)"
    echo "    (no stdin ref-pairs and no --range; checking staged + working tree)"
    run_gate "git diff --check (working tree; manual invocation)" \
        "git diff --check" \
        git diff --check
    echo "==> pre-push gate: assert-hygiene (working tree; manual invocation)"
    echo "    (no stdin ref-pairs and no --range; checking staged + working tree)"
    run_gate "assert-hygiene (working tree; manual invocation)" \
        "python3 scripts/gates/assert-hygiene.py" \
        python3 scripts/gates/assert-hygiene.py
    run_gate "claim-hygiene (working tree; manual invocation)" \
        "python3 scripts/gates/claim-hygiene.py" \
        python3 scripts/gates/claim-hygiene.py
elif [ "${#DIFF_ARGS[@]}" -eq 0 ]; then
    # Hook mode where every pushed pair was a deletion: nothing to check.
    echo "==> pre-push gate: git diff --check (${MODE} ranges)"
    echo "    (no pushable content: all refs deleted; nothing to check)"
    echo "==> pre-push gate: assert-hygiene (${MODE} ranges)"
    echo "    (no pushable content: all refs deleted; nothing to check)"
    echo "==> pre-push gate: claim-hygiene (${MODE} ranges)"
    echo "    (no pushable content: all refs deleted; nothing to check)"
else
    # Reproduction for a failure lists the exact ranges scanned. The ranges
    # are echoed so a CI/hook log shows what was actually scanned.
    echo "==> pre-push gate: ${MODE} range(s) for changed-lines gates: ${DIFF_ARGS[*]}"
    run_gate "git diff --check (${MODE} ranges)" \
        "git diff --check ${DIFF_ARGS[*]}" \
        git diff --check "${DIFF_ARGS[@]}"
    run_gate "assert-hygiene (${MODE} ranges)" \
        "python3 scripts/gates/assert-hygiene.py ${DIFF_ARGS[*]}" \
        python3 scripts/gates/assert-hygiene.py "${DIFF_ARGS[@]}"
    run_gate "claim-hygiene (${MODE} ranges)" \
        "python3 scripts/gates/claim-hygiene.py ${DIFF_ARGS[*]}" \
        python3 scripts/gates/claim-hygiene.py "${DIFF_ARGS[@]}"
fi

# ---------------------------------------------------------------------------
echo "==> pre-push gate: ALL CHECKS PASSED"
exit 0
