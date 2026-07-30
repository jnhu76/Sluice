#!/usr/bin/env bash
# SLUICE-LOCAL-OVERNIGHT-GATE-1 — local overnight correctness gate.
#
# Moves expensive, repeatable verification (Debug soak, TSan hot set,
# ASan+UBSan, libFuzzer campaigns) OFF the limited-budget GitHub Actions
# gate and onto a developer Linux/Clang machine. It REUSES the existing
# xmake targets, tests and fuzz harnesses; it builds no new framework and
# changes no production code.
#
#   ./scripts/overnight-local.sh             # full ~8h gate (default)
#   ./scripts/overnight-local.sh --smoke     # prove the runner wiring (not a gate)
#   ./scripts/overnight-local.sh --hours 6
#   ./scripts/overnight-local.sh --self-test # controlled TIMEOUT + keep-going proof
#   ./scripts/overnight-local.sh --help
#
# Env overrides (CLI args win where applicable):
#   SLUICE_OVERNIGHT_HOURS    total budget in hours           (default 8)
#   SLUICE_PHASE_TIMEOUT      per-command timeout, seconds     (default 1200)
#   SLUICE_FUZZ_SECONDS       override total fuzz budget (s)   (default: split)
#   SLUICE_KEEP_GOING         0 = stop after current command   (default 1)
#
# Verdicts / exit codes:
#   PASS        -> 0   required baseline + Final Debug OK, no sticky fail/
#                     timeout/sanitizer/crash, >=1 critical TSan target ran,
#                     ASan+UBSan full suite ran, >=1 fuzz target ran.
#   INCOMPLETE  -> 0   no real failure, but a required evidence family was
#                     not executed (budget/platform/environment).
#   HOLD        -> 1   any sticky failure/timeout/sanitizer report/new fuzz
#                     crash; or required baseline/Final Debug failure; or all
#                     critical TSan targets unavailable.
#   arg/env err -> 2   bad CLI args, missing core tools (xmake/clang/git).
#
# Logging is a first-class deliverable: every external command produces a
# self-contained per-command log plus a structured events.tsv row; a global
# run.log tracks the human-readable timeline; failures/index.tsv indexes only
# real failures. See docs/testing/overnight-local.md.
#
# NOTE on text tools: sed/awk/grep are invoked via `command` so a parent
# interactive shell that wraps grep (e.g. an agent search shim) cannot
# corrupt the runner's own text processing. xmake/clang subprocesses use the
# real system binaries regardless.
set -uo pipefail

# ===========================================================================
# Constants — authoritative target candidate lists. Existence is verified at
# runtime against the cached per-config target snapshot; nothing is assumed.
# ===========================================================================

# Phase C (TSan) hot set — concurrency-relevant targets. All 18 were present
# at audit; the runner must not fail the whole night if one is later removed.
readonly TSAN_HOT_SET=(
    multi_worker_test
    multi_worker_coord_test
    scheduler_worker_topology_race_test
    application_runtime_worker_topology_test
    runnable_dup_publication_test
    runnable_steal_test
    external_wake_test
    wake_handle_lifetime_test
    scheduler_wait_test
    wait_queue_test
    wait_queue_external_wake_test
    wait_queue_unlink_topology_test
    select_multi_worker_test
    runtime_wait_test
    backend_conformance_test
    threadpool_backend_test
    uring_backend_test
    sluice_copy_integration_test
)

# Phase D (ASan+UBSan) hot set — ownership/parse/lifetime-sensitive targets.
readonly ASAN_HOT_SET=(
    select_registration_rollback_test
    group_evented_admission_exception_safety_test
    application_runtime_resource_test
    async_io_context_test
    batch_reap_order_test
    async_queue_primitive_test
    runtime_wait_test
    sluice_copy_integration_test
)

# Phase E (fuzz) targets — Clang/libFuzzer-only (xmake/fuzz.lua gates them).
readonly FUZZ_TARGETS=(
    wal_read_record_fuzz
    wal_roundtrip_fuzz
    copy_all_fault_fuzz
)

# Per-target libFuzzer -max_len profiles (resource bounds). Mirrors
# scripts/run-wal-copy-fuzz-smoke.sh + replay-wal-copy-fuzz-corpus.sh.
declare -A FUZZ_MAXLEN=(
    [wal_read_record_fuzz]=1048576
    [wal_roundtrip_fuzz]=262144
    [copy_all_fault_fuzz]=8192
)

# Baseline acceptance consumers (public-only headers) + negative-compile
# scripts — driven exactly as .github/workflows/ci.yml does.
readonly ACCEPTANCE_CONSUMERS=(public_api_acceptance async_foundation_quickstart)
readonly NEG_COMPILE_SCRIPTS=(
    scripts/verify-async-api-negative-compile.sh
    scripts/verify-async-identity-negative-compile.sh
)

# Sanitizer signature patterns. An exit-zero log that contains one of these
# is STILL a FAIL (correction: final #9).
readonly TSAN_SIG='WARNING: ThreadSanitizer|WARNING: data race|WARNING: deadlock'
readonly ASAN_SIG='ERROR: AddressSanitizer|AddressSanitizer:|LeakSanitizer'
readonly UBSAN_SIG='runtime error:|SUMMARY: UndefinedBehaviorSanitizer'

# Kill-after grace (s) for the TERM->KILL escalation window. Short on purpose
# so a hung command cannot consume another full phase timeout.
readonly KILL_AFTER_S=10

readonly EXIT_PASS=0
readonly EXIT_HOLD=1
readonly EXIT_ARG_ENV=2

# ===========================================================================
# Globals.
# ===========================================================================

PROJECT_ROOT=""
RUN_DIR=""              # overnight-artifacts/<stamp>-<sha>/
RUN_LOG=""              # $RUN_DIR/run.log
EVENTS_TSV=""           # $RUN_DIR/events.tsv
FAILURES_INDEX=""       # $RUN_DIR/failures/index.tsv
WORKTREE_DIFF=""        # $RUN_DIR/worktree.diff
MODE=""                 # overnight | smoke | selftest
HOURS=8
PHASE_TIMEOUT=1200
FUZZ_SECONDS_OVERRIDE=""
KEEP_GOING=1
START_EPOCH=0
DEADLINE_EPOCH=0
HEAD_SHA=""
HEAD_SHORT=""
WORKTREE_DIRTY=0
NPROC=1
BASELINE_EPOCH_END=0
BASELINE_OK=0
FINAL_DEBUG_OK=0

# Sticky verdict state (correction #7): a FAIL can never be cleared by a PASS.
declare -g STICKY_HOLD=0
declare -gi SOAK_ITER=0 SOAK_PASS=0 SOAK_FAIL=0 SOAK_TIMEOUT=0
declare -gi TSAN_SET_ITER=0 TSAN_EXEC=0 TSAN_PASS=0 TSAN_FAIL=0 TSAN_SKIP=0 TSAN_TIMEOUT=0 TSAN_RACE=0
declare -gi TSAN_CRITICAL_EXEC=0
declare -gi ASAN_FULL_OK=0 ASAN_HOT_EXEC=0 ASAN_HOT_PASS=0 ASAN_SAN_ERR=0 ASAN_SKIP=0
declare -gi FUZZ_EXEC=0 FUZZ_CRASH=0 FUZZ_PASS=0
declare -g  FUZZ_RESULTS=""

declare -g FINALIZED=0    # idempotent-finalization guard (correction #4)

# Per-config target cache (correction #5): mode -> "1" once snapshot written.
declare -g TARGET_CACHE_MODE=""
declare -gA TARGET_CACHE=()
# Per-fuzz-target artifact isolation (correction #6): baseline artifact counts.
declare -gA FUZZ_ARTIFACT_BASE=()

# ===========================================================================
# Utility helpers
# ===========================================================================

die() { printf '%s\n' "$*" >&2; exit "$EXIT_ARG_ENV"; }

iso_ts() {
    local e="${1:-$(date +%s)}"
    date --iso-8601=seconds -d "@$e" 2>/dev/null || date -r "$e" --iso-8601=seconds 2>/dev/null || printf 'epoch=%s' "$e"
}

log() {
    # Concise progress to terminal + global timeline in run.log.
    local msg="$*"
    printf '%s\n' "$msg"
    [[ -n "$RUN_LOG" && -f "$RUN_LOG" ]] && printf '%s %s\n' "$(iso_ts)" "$msg" >>"$RUN_LOG" || true
}

# Strip ANSI + collapse whitespace (multi-column `xmake show -l targets` -> tokens).
strip_ansi_tokens() {
    command sed -E 's/\x1b\[[0-9;]*m//g' | tr -s '[:space:]' '\n' \
        | command grep -E '^[A-Za-z0-9_.+-]+$'
}

# ===========================================================================
# Target cache (correction #5) — one `xmake show -l targets` per config.
# ===========================================================================

cache_targets() {
    local mode="$1"
    local out="${RUN_DIR}/${mode}-targets.txt"
    if [[ -n "${TARGET_CACHE[$mode]:-}" && -f "$out" ]]; then
        TARGET_CACHE_MODE="$mode"
        return 0
    fi
    if xmake show -l targets 2>/dev/null | strip_ansi_tokens | sort -u >"$out"; then
        TARGET_CACHE[$mode]=1
        TARGET_CACHE_MODE="$mode"
        log "[targets] cached ${mode} snapshot -> ${out} ($(wc -l <"$out" | tr -d ' ') targets)"
    else
        log "[targets] WARN: failed to snapshot ${mode} targets; existence checks -> SKIP"
        : >"$out"
        TARGET_CACHE[$mode]=1
        TARGET_CACHE_MODE="$mode"
    fi
}

target_exists() {
    local name="$1"
    local out="${RUN_DIR}/${TARGET_CACHE_MODE}-targets.txt"
    [[ -f "$out" ]] && command grep -qxF "$name" "$out"
}

first_signature() {
    local logf="$1" pat="$2"
    [[ -f "$logf" ]] || return 0
    command grep -m1 -E "$pat" "$logf" 2>/dev/null || true
}

# ===========================================================================
# Per-command execution + first-class logging (corrections #1, #2, #3).
# ===========================================================================

# Append one events.tsv row.
append_event() {
    local phase="$1" iter="$2" target="$3" cmode="$4" classif="$5"
    local raw_exit="$6" dur="$7" timeout_s="$8" logp="$9"; shift 9
    local cmd; cmd=$(printf '%q ' "$@"); cmd="${cmd% }"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$(date +%s)" "$phase" "$iter" "$target" "$cmode" "$classif" \
        "$raw_exit" "$dur" "$timeout_s" "$logp" "$cmd" >>"$EVENTS_TSV"
}

# Append one failures/index.tsv row (REAL failures only — final detail #1).
append_failure() {
    local phase="$1" target="$2" classif="$3" sig="$4" logp="$5"
    mkdir -p "${RUN_DIR}/failures"
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$(date +%s)" "$phase" "$target" "$classif" "${sig:-(no signature)}" "$logp" \
        >>"$FAILURES_INDEX"
}

# Core runner. Args (then "--" then cmd...):
#   phase iteration target cmode timeout_s outlog sanitizer_kind [san_env ...] -- cmd...
# sanitizer_kind: none|tsan|asan
run_logged() {
    local phase="$1" iteration="$2" target="$3" cmode="$4" timeout_s="$5"
    local outlog="$6" san_kind="$7"; shift 7
    local -a san_env=()
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--" ]]; then shift; break; fi
        san_env+=("$1"); shift
    done
    [[ $# -gt 0 ]] || { log "[${phase}] ERROR: no command for run_logged"; return 99; }

    local cmd_str; cmd_str=$(printf '%q ' "$@"); cmd_str="${cmd_str% }"
    mkdir -p "$(dirname "$outlog")"

    # HEADER flushed BEFORE the command starts (correction #3): an interruption
    # still leaves a useful, self-contained partial log.
    {
        printf '## sluice overnight command log\n'
        printf 'phase=%s\n' "$phase"
        printf 'iteration=%s\n' "$iteration"
        printf 'target=%s\n' "$target"
        printf 'mode=%s\n' "$cmode"
        printf 'head=%s\n' "$HEAD_SHA"
        printf 'worktree_dirty=%s\n' "$WORKTREE_DIRTY"
        printf 'command=%s\n' "$cmd_str"
        printf 'sanitizer=%s\n' "$san_kind"
        local p
        for p in "${san_env[@]}"; do printf 'sanitizer_env=%s\n' "$p"; done
        printf 'timeout_s=%s\n' "$timeout_s"
        printf 'start_iso=%s\n' "$(iso_ts)"
        printf 'start_epoch=%s\n' "$(date +%s)"
        printf -- '----- output -----\n'
    } >"$outlog"

    # PLAIN timeout (correction #2): no --preserve-status; TERM then KILL after a
    # short grace. Output appended live so the partial log stays useful.
    local start_e; start_e=$(date +%s)
    # Run under timeout, capturing the REAL exit code. Note: `! cmd; rc=$?`
    # would capture the negation (0), not the original code, so we use the
    # `cmd || rc=$?` idiom which preserves the true exit status.
    local rc=0
    command timeout -s TERM -k "${KILL_AFTER_S}s" "${timeout_s}s" "$@" >>"$outlog" 2>&1 || rc=$?
    local end_e; end_e=$(date +%s)
    local dur=$((end_e - start_e))

    # Escalation witness (final detail #2): never infer KILL solely from 137.
    # 124 = the initial TERM fired; true KILL cannot be determined portably.
    local escalated="no"
    [[ $rc -eq 124 ]] && escalated="term_sent_kill_after_inferred_unknown"

    # Classification (correction #9): scan sanitizer signatures even on exit 0.
    local classif="PASS" sig=""
    if [[ $rc -eq 0 ]]; then
        case "$san_kind" in
            tsan) sig=$(first_signature "$outlog" "$TSAN_SIG") ;;
            asan) sig=$(first_signature "$outlog" "$ASAN_SIG")
                  [[ -z "$sig" ]] && sig=$(first_signature "$outlog" "$UBSAN_SIG") ;;
        esac
        [[ -n "$sig" ]] && classif="SANITIZER_FAIL"
    elif [[ $rc -eq 124 ]]; then
        classif="TIMEOUT"
    elif [[ $rc -eq 77 ]]; then
        classif="SKIP"   # final detail #3: 77 is SKIP unless documented otherwise
    else
        case "$san_kind" in
            tsan) sig=$(first_signature "$outlog" "$TSAN_SIG") ;;
            asan) sig=$(first_signature "$outlog" "$ASAN_SIG")
                  [[ -z "$sig" ]] && sig=$(first_signature "$outlog" "$UBSAN_SIG") ;;
        esac
        if [[ -n "$sig" ]]; then classif="SANITIZER_FAIL"; else classif="FAIL"; fi
    fi

    {
        printf -- '----- end output -----\n'
        printf 'end_iso=%s\n' "$(iso_ts)"
        printf 'end_epoch=%s\n' "$end_e"
        printf 'duration_s=%s\n' "$dur"
        printf 'raw_exit=%s\n' "$rc"
        printf 'classification=%s\n' "$classif"
        printf 'term_to_kill_escalation=%s\n' "$escalated"
        [[ -n "$sig" ]] && printf 'sanitizer_signature=%s\n' "$sig"
    } >>"$outlog"

    append_event "$phase" "$iteration" "$target" "$cmode" "$classif" "$rc" \
        "$dur" "$timeout_s" "$outlog" "$@"

    # Index REAL failures only (final detail #1). HOLD is a run verdict, not a
    # per-command class, so it is never written here.
    case "$classif" in
        FAIL|TIMEOUT|SANITIZER_FAIL|BUILD_FAIL|FUZZ_CRASH)
            append_failure "$phase" "$target" "$classif" "$sig" "$outlog" ;;
    esac

    echo "$classif"
}

# ===========================================================================
# Verdict / budget helpers
# ===========================================================================

mark_hold() { STICKY_HOLD=1; }

remaining_time() {
    local now; now=$(date +%s)
    echo $((DEADLINE_EPOCH - now))
}

# Final Debug reserve (final detail #6): max(20m, 1.5 x baseline elapsed).
final_debug_budget() {
    local baseline=$((BASELINE_EPOCH_END - START_EPOCH))
    [[ $baseline -lt 0 ]] && baseline=0
    local one_half=$((baseline + baseline / 2))
    local twenty=$((20 * 60))
    if (( one_half > twenty )); then echo "$one_half"; else echo "$twenty"; fi
}

# ===========================================================================
# Idempotent finalization (correction #4)
# ===========================================================================

write_summary() {
    [[ $FINALIZED -eq 1 ]] && return 0
    FINALIZED=1

    local verdict="PASS"
    if (( STICKY_HOLD == 1 )); then
        verdict="HOLD"
    else
        # INCOMPLETE if a required evidence family never executed (final #7).
        local incomplete=""
        (( BASELINE_OK == 0 )) && incomplete="${incomplete}baseline-did-not-complete "
        (( TSAN_CRITICAL_EXEC == 0 )) && incomplete="${incomplete}no-critical-tsan-target-executed "
        (( ASAN_FULL_OK == 0 )) && incomplete="${incomplete}asan-ubsan-full-suite-not-executed "
        (( FUZZ_EXEC == 0 )) && incomplete="${incomplete}no-fuzz-target-executed "
        (( FINAL_DEBUG_OK == 0 )) && incomplete="${incomplete}final-debug-not-completed "
        [[ -n "$incomplete" ]] && verdict="INCOMPLETE"
    fi

    local finish_e; finish_e=$(date +%s)
    local dur_s=$((finish_e - START_EPOCH))
    local dur_h=$((dur_s / 3600))
    local dur_m=$(((dur_s % 3600) / 60))
    local dur_sec=$((dur_s % 60))

    {
        printf 'SLUICE OVERNIGHT: %s\n\n' "$verdict"
        printf 'HEAD: %s\n' "$HEAD_SHORT"
        printf 'Started: %s\n' "$(iso_ts "$START_EPOCH")"
        printf 'Finished: %s\n' "$(iso_ts "$finish_e")"
        printf 'Duration: %dh%02dm%02ds\n' "$dur_h" "$dur_m" "$dur_sec"
        printf 'Worktree: %s\n' "$([[ $WORKTREE_DIRTY -eq 1 ]] && echo dirty || echo clean)"
        printf 'Mode: %s\n' "$MODE"
        printf 'Budget hours: %s\n' "$HOURS"
        printf '\n'
        printf 'Baseline:\n'
        printf '  Debug configure/build: %s\n' "$([[ $BASELINE_OK -eq 1 ]] && echo PASS || echo FAIL)"
        printf '  Final Debug suite: %s\n' "$([[ $FINAL_DEBUG_OK -eq 1 ]] && echo PASS || echo FAIL)"
        printf '\n'
        printf 'Debug soak:\n'
        printf '  Iterations: %s\n' "$SOAK_ITER"
        printf '  Passed: %s\n' "$SOAK_PASS"
        printf '  Failed: %s\n' "$SOAK_FAIL"
        printf '  Timeouts: %s\n' "$SOAK_TIMEOUT"
        printf '\n'
        printf 'TSan:\n'
        printf '  Set iterations: %s\n' "$TSAN_SET_ITER"
        printf '  Target executions: %s\n' "$TSAN_EXEC"
        printf '  Passed: %s\n' "$TSAN_PASS"
        printf '  Failed: %s\n' "$TSAN_FAIL"
        printf '  Timeouts: %s\n' "$TSAN_TIMEOUT"
        printf '  Skipped: %s\n' "$TSAN_SKIP"
        printf '  Races: %s\n' "$TSAN_RACE"
        printf '  Critical targets executed: %s\n' "$TSAN_CRITICAL_EXEC"
        printf '\n'
        printf 'ASan+UBSan:\n'
        printf '  Full suite: %s\n' "$([[ $ASAN_FULL_OK -eq 1 ]] && echo PASS || echo FAIL)"
        printf '  Hot target executions: %s\n' "$ASAN_HOT_EXEC"
        printf '  Hot target passed: %s\n' "$ASAN_HOT_PASS"
        printf '  Skipped: %s\n' "$ASAN_SKIP"
        printf '  Sanitizer errors: %s\n' "$ASAN_SAN_ERR"
        printf '\n'
        printf 'Fuzz:\n'
        if [[ -n "$FUZZ_RESULTS" ]]; then
            local line
            while IFS= read -r line; do [[ -z "$line" ]] && continue
                printf '  %s\n' "$line"; done <<<"$FUZZ_RESULTS"
        else
            printf '  (none executed)\n'
        fi
        printf '  Targets executed: %s\n' "$FUZZ_EXEC"
        printf '  Crash artifacts (new): %s\n' "$FUZZ_CRASH"
        printf '\n'
        printf 'Failures:\n'
        if [[ -f "$FAILURES_INDEX" ]] && [[ -s "$FAILURES_INDEX" ]]; then
            awk -F '\t' '{ printf "  %s %s %s -> %s\n", $2, $3, $4, $6 }' "$FAILURES_INDEX"
        else
            printf '  none\n'
        fi
        printf '\n'
        printf 'Artifacts: %s\n' "$RUN_DIR"
    } >"${RUN_DIR}/summary.txt"

    cat "${RUN_DIR}/summary.txt"
}

finalize_and_exit() {
    local code=$?
    write_summary
    exit "$code"
}

# ===========================================================================
# Phase A — Debug baseline
# ===========================================================================

phase_baseline() {
    log "[baseline] configuring clang debug"
    cache_targets debug

    local out
    out=$(run_logged baseline 0 configure debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/baseline.log" none -- xmake f -c -m debug --toolchain=clang -y)
    [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] configure"; mark_hold; }

    out=$(run_logged baseline 0 sluice_core debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/baseline-core.log" none -- xmake build sluice_core)
    [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] sluice_core build"; mark_hold; }

    out=$(run_logged baseline 0 sluice_async debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/baseline-async.log" none -- xmake build sluice_async)
    [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] sluice_async build"; mark_hold; }

    out=$(run_logged baseline 0 build-test-group debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/baseline-build-test.log" none -- xmake build -g test)
    [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] build -g test"; mark_hold; }

    out=$(run_logged baseline 0 full-suite debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/baseline-suite.log" none -- xmake test -v)
    if [[ "$out" == "PASS" ]]; then BASELINE_OK=1; else log "[baseline][FAIL] full suite"; mark_hold; fi

    local script name
    for script in "${NEG_COMPILE_SCRIPTS[@]}"; do
        name=$(basename "$script" | command sed -E 's/\.(sh)$//')
        if [[ -f "${PROJECT_ROOT}/${script}" ]]; then
            out=$(run_logged baseline 0 "$name" debug "$PHASE_TIMEOUT" \
                "${RUN_DIR}/baseline-${name}.log" none -- bash "$script")
            [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] ${name}"; mark_hold; }
        else
            log "[baseline][SKIP] ${script} (missing)"
        fi
    done

    local c
    for c in "${ACCEPTANCE_CONSUMERS[@]}"; do
        if target_exists "$c"; then
            out=$(run_logged baseline 0 "build:${c}" debug "$PHASE_TIMEOUT" \
                "${RUN_DIR}/baseline-build-${c}.log" none -- xmake build "$c")
            [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] build ${c}"; mark_hold; }
            out=$(run_logged baseline 0 "run:${c}" debug "$PHASE_TIMEOUT" \
                "${RUN_DIR}/baseline-run-${c}.log" none -- xmake run "$c")
            [[ "$out" == "PASS" ]] || { log "[baseline][FAIL] run ${c}"; mark_hold; }
        else
            log "[baseline][SKIP] ${c} (target absent)"
        fi
    done

    BASELINE_EPOCH_END=$(date +%s)
}

# ===========================================================================
# Phase B — Debug full-suite soak (sticky failure, correction #7)
# ===========================================================================

phase_debug_soak() {
    local budget="$1"
    [[ $BASELINE_OK -eq 1 ]] || { log "[debug-soak][SKIP] baseline build failed"; return 0; }
    local per_cmd_timeout="$PHASE_TIMEOUT"
    local iter=0 consec_fail=0
    log "[debug-soak] budget=${budget}s per-cmd-timeout=${per_cmd_timeout}s"
    local rem
    while rem=$(remaining_time); (( rem > per_cmd_timeout )); do
        (( budget > 0 )) || break
        iter=$((iter + 1))
        SOAK_ITER=$iter
        log "[debug-soak] iteration ${iter}"
        local out; out=$(run_logged debug-soak "$iter" full-suite debug "$per_cmd_timeout" \
            "${RUN_DIR}/debug-soak/iteration-$(printf '%04d' "$iter").log" none -- xmake test -v)
        case "$out" in
            PASS)
                SOAK_PASS=$((SOAK_PASS + 1)); consec_fail=0
                ;;
            TIMEOUT)
                SOAK_TIMEOUT=$((SOAK_TIMEOUT + 1)); consec_fail=$((consec_fail + 1)); mark_hold
                ;;
            FAIL|SANITIZER_FAIL)
                SOAK_FAIL=$((SOAK_FAIL + 1)); consec_fail=$((consec_fail + 1)); mark_hold
                # Retries are reproduction evidence (correction #7): they cannot
                # clear the sticky failure. Up to 2 extra attempts, recorded.
                local r=0
                while (( r < 2 )); do
                    r=$((r + 1))
                    local rout; rout=$(run_logged debug-soak "${iter}-retry${r}" full-suite debug \
                        "$per_cmd_timeout" \
                        "${RUN_DIR}/debug-soak/iteration-$(printf '%04d' "$iter")-retry${r}.log" \
                        none -- xmake test -v)
                    log "[debug-soak] iteration ${iter} retry ${r}: ${rout} (reproduction evidence; sticky unchanged)"
                done
                ;;
            SKIP) ;;
        esac
        budget=$((budget - per_cmd_timeout))
        (( budget <= 0 )) && break
        (( consec_fail >= 3 )) && { log "[debug-soak] 3 consecutive failures; stopping soak"; break; }
        (( KEEP_GOING == 0 )) && { log "[debug-soak] KEEP_GOING=0; stopping"; break; }
    done
    log "[debug-soak] done (iters=${SOAK_ITER} pass=${SOAK_PASS} fail=${SOAK_FAIL} timeout=${SOAK_TIMEOUT})"
}

# ===========================================================================
# Phase C — TSan hot set
# ===========================================================================

phase_tsan() {
    local budget="$1"
    log "[tsan] configuring clang tsan"
    local out; out=$(run_logged tsan 0 configure tsan "$PHASE_TIMEOUT" \
        "${RUN_DIR}/tsan/configure.log" none -- xmake f -c -m tsan --toolchain=clang -y)
    [[ "$out" == "PASS" ]] || { log "[tsan][FAIL] configure; skipping phase"; mark_hold; return 0; }

    cache_targets tsan
    out=$(run_logged tsan 0 build-test-group tsan "$PHASE_TIMEOUT" \
        "${RUN_DIR}/tsan/build-test.log" none -- xmake build -g test)
    [[ "$out" == "PASS" ]] || { log "[tsan][FAIL] build -g test; skipping hot set"; mark_hold; return 0; }

    local per_cmd_timeout="$PHASE_TIMEOUT"
    local set_iter=0 t rem
    log "[tsan] budget=${budget}s per-cmd-timeout=${per_cmd_timeout}s targets=${#TSAN_HOT_SET[@]}"
    while rem=$(remaining_time); (( rem > per_cmd_timeout )) && (( budget > 0 )); do
        set_iter=$((set_iter + 1)); TSAN_SET_ITER=$set_iter
        for t in "${TSAN_HOT_SET[@]}"; do
            rem=$(remaining_time); (( rem <= per_cmd_timeout )) && break
            (( budget <= 0 )) && break
            if ! target_exists "$t"; then
                TSAN_SKIP=$((TSAN_SKIP + 1))
                append_event tsan "$set_iter" "$t" tsan SKIP - - - - - xmake-run "$t"
                log "[tsan][SKIP] iteration ${set_iter}: ${t} (absent)"
                continue
            fi
            log "[tsan] iteration ${set_iter}: ${t}"
            local res; res=$(run_logged tsan "$set_iter" "$t" tsan "$per_cmd_timeout" \
                "${RUN_DIR}/tsan/iteration-$(printf '%04d' "$set_iter")-${t}.log" tsan \
                "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1" -- \
                xmake run "$t")
            TSAN_EXEC=$((TSAN_EXEC + 1))
            case "$res" in
                PASS) TSAN_PASS=$((TSAN_PASS + 1)); TSAN_CRITICAL_EXEC=$((TSAN_CRITICAL_EXEC + 1)) ;;
                TIMEOUT) TSAN_TIMEOUT=$((TSAN_TIMEOUT + 1)); mark_hold ;;
                SKIP) TSAN_SKIP=$((TSAN_SKIP + 1)) ;;
                SANITIZER_FAIL) TSAN_RACE=$((TSAN_RACE + 1)); TSAN_FAIL=$((TSAN_FAIL + 1)); TSAN_CRITICAL_EXEC=$((TSAN_CRITICAL_EXEC + 1)); mark_hold ;;
                FAIL) TSAN_FAIL=$((TSAN_FAIL + 1)); TSAN_CRITICAL_EXEC=$((TSAN_CRITICAL_EXEC + 1)); mark_hold ;;
            esac
            budget=$((budget - per_cmd_timeout))
            (( KEEP_GOING == 0 )) && { log "[tsan] KEEP_GOING=0; stopping"; break 2; }
        done
    done
    log "[tsan] done (set_iters=${TSAN_SET_ITER} exec=${TSAN_EXEC} pass=${TSAN_PASS} fail=${TSAN_FAIL} timeout=${TSAN_TIMEOUT} skip=${TSAN_SKIP} race=${TSAN_RACE})"
}

# ===========================================================================
# Phase D — ASan + UBSan
# ===========================================================================

phase_asanubsan() {
    local budget="$1"
    log "[asanubsan] configuring clang asanubsan"
    local out; out=$(run_logged asanubsan 0 configure asanubsan "$PHASE_TIMEOUT" \
        "${RUN_DIR}/asanubsan/configure.log" none -- xmake f -c -m asanubsan --toolchain=clang -y)
    [[ "$out" == "PASS" ]] || { log "[asanubsan][FAIL] configure; skipping phase"; mark_hold; return 0; }

    cache_targets asanubsan
    out=$(run_logged asanubsan 0 build-test-group asanubsan "$PHASE_TIMEOUT" \
        "${RUN_DIR}/asanubsan/build-test.log" none -- xmake build -g test)
    [[ "$out" == "PASS" ]] || { log "[asanubsan][FAIL] build -g test; skipping hot set"; mark_hold; return 0; }

    # One full suite first (authoritative command, correction #4).
    log "[asanubsan] full suite"
    local full; full=$(run_logged asanubsan 0 full-suite asanubsan "$PHASE_TIMEOUT" \
        "${RUN_DIR}/asanubsan/full-suite.log" asan \
        "ASAN_OPTIONS=halt_on_error=1:detect_leaks=1" \
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" -- \
        xmake test -v)
    if [[ "$full" == "PASS" ]]; then ASAN_FULL_OK=1; else mark_hold; fi
    [[ "$full" == "SANITIZER_FAIL" ]] && ASAN_SAN_ERR=$((ASAN_SAN_ERR + 1))
    budget=$((budget - PHASE_TIMEOUT))
    (( budget <= 0 )) && { log "[asanubsan] budget exhausted after full suite"; return 0; }

    local per_cmd_timeout="$PHASE_TIMEOUT" t rem
    log "[asanubsan] hot set budget=${budget}s per-cmd-timeout=${per_cmd_timeout}s"
    for t in "${ASAN_HOT_SET[@]}"; do
        rem=$(remaining_time); (( rem <= per_cmd_timeout )) && break
        (( budget <= 0 )) && break
        if ! target_exists "$t"; then
            ASAN_SKIP=$((ASAN_SKIP + 1))
            append_event asanubsan 0 "$t" asanubsan SKIP - - - - - xmake-run "$t"
            log "[asanubsan][SKIP] ${t} (absent)"
            continue
        fi
        log "[asanubsan] ${t}"
        local res; res=$(run_logged asanubsan 0 "$t" asanubsan "$per_cmd_timeout" \
            "${RUN_DIR}/asanubsan/${t}.log" asan \
            "ASAN_OPTIONS=halt_on_error=1:detect_leaks=1" \
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" -- \
            xmake run "$t")
        ASAN_HOT_EXEC=$((ASAN_HOT_EXEC + 1))
        case "$res" in
            PASS) ASAN_HOT_PASS=$((ASAN_HOT_PASS + 1)) ;;
            TIMEOUT) mark_hold ;;
            SANITIZER_FAIL) ASAN_SAN_ERR=$((ASAN_SAN_ERR + 1)); mark_hold ;;
            SKIP) ASAN_SKIP=$((ASAN_SKIP + 1)); ASAN_HOT_EXEC=$((ASAN_HOT_EXEC - 1)) ;;
            FAIL) mark_hold ;;
        esac
        budget=$((budget - per_cmd_timeout))
        (( KEEP_GOING == 0 )) && { log "[asanubsan] KEEP_GOING=0; stopping"; break; }
    done
    log "[asanubsan] done (full_ok=${ASAN_FULL_OK} hot_exec=${ASAN_HOT_EXEC} hot_pass=${ASAN_HOT_PASS} skip=${ASAN_SKIP} san_err=${ASAN_SAN_ERR})"
}

# ===========================================================================
# Phase E — fuzz (isolated artifacts, dynamic split, correction #6)
# ===========================================================================

count_fuzz_artifacts() {
    find "$1" -maxdepth 1 -type f \( -name 'crash-*' -o -name 'leak-*' -o -name 'timeout-*' \
        -o -name 'oom-*' -o -name 'panic-*' \) 2>/dev/null | wc -l | tr -d ' '
}

phase_fuzz() {
    local budget="$1"
    log "[fuzz] configuring clang debug (fuzz build)"
    local out; out=$(run_logged fuzz 0 configure debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/fuzz/configure.log" none -- xmake f -c -m debug --toolchain=clang -y)
    [[ "$out" == "PASS" ]] || { log "[fuzz][FAIL] configure; skipping fuzz"; mark_hold; return 0; }

    cache_targets debug
    out=$(run_logged fuzz 0 build-fuzz-group debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/fuzz/build.log" none -- xmake build -g fuzz)
    [[ "$out" == "PASS" ]] || { log "[fuzz][FAIL] build -g fuzz; skipping fuzz"; mark_hold; return 0; }

    local existing=() t rem
    for t in "${FUZZ_TARGETS[@]}"; do
        if target_exists "$t"; then existing+=("$t"); else
            append_event fuzz 0 "$t" debug SKIP - - - - - xmake-run "$t"
            log "[fuzz][SKIP] ${t} (absent under clang config)"
        fi
    done
    local n=${#existing[@]}
    (( n == 0 )) && { log "[fuzz] no fuzz targets present; skipping"; return 0; }

    # Dynamic per-target split (plan Q#1). Explicit override is the TOTAL fuzz
    # budget; otherwise use the phase budget.
    local total_budget="$budget"
    [[ -n "$FUZZ_SECONDS_OVERRIDE" ]] && total_budget="$FUZZ_SECONDS_OVERRIDE"
    (( total_budget <= 0 )) && { log "[fuzz] budget exhausted; skipping"; return 0; }
    local per=$((total_budget / n))
    (( per < 5 )) && per=5
    log "[fuzz] targets=${n} total_budget=${total_budget}s per-target=${per}s"

    local dict="${PROJECT_ROOT}/fuzz/dictionaries/wal_record.dict"
    local nightly_root="${PROJECT_ROOT}/.nightly-corpus"
    mkdir -p "$nightly_root"

    for t in "${existing[@]}"; do
        rem=$(remaining_time); (( rem < per )) && { log "[fuzz] not enough time for ${t}; stopping"; break; }

        local committed=""
        case "$t" in
            wal_read_record_fuzz) committed="${PROJECT_ROOT}/fuzz/corpus/wal_read_record" ;;
            wal_roundtrip_fuzz)   committed="${PROJECT_ROOT}/fuzz/corpus/wal_roundtrip" ;;
            copy_all_fault_fuzz)  committed="${PROJECT_ROOT}/fuzz/corpus/copy_all_fault" ;;
        esac
        local work_corpus="${nightly_root}/${t}"
        mkdir -p "$work_corpus"
        # Seed the persistent nightly corpus ONCE if empty. Never clear an
        # existing corpus (prompt §15).
        if [[ -d "$committed" ]] && [[ -z "$(ls -A "$work_corpus" 2>/dev/null)" ]]; then
            cp -a "${committed}/." "$work_corpus/" 2>/dev/null || true
        fi

        local artifact_dir="${RUN_DIR}/fuzz/${t}"
        mkdir -p "$artifact_dir"
        FUZZ_ARTIFACT_BASE[$t]=$(count_fuzz_artifacts "$artifact_dir")   # isolation baseline

        local before_files before_bytes
        before_files=$(find "$work_corpus" -type f 2>/dev/null | wc -l | tr -d ' ')
        before_bytes=$(find "$work_corpus" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')

        local maxlen="${FUZZ_MAXLEN[$t]:-1048576}"
        local -a fargs=()
        [[ -f "$dict" ]] && fargs+=("-dict=${dict}")
        log "[fuzz] ${t}: ${per} seconds"

        # Wrapper timeout = per-target budget + a generous finalize grace. A
        # well-behaved libFuzzer process honors -max_total_time=<per> and then
        # writes its corpus/summary; the grace (180s) absorbs slow init, corpus
        # merge and sanitizer teardown so such a run is not mis-killed.
        local res; res=$(run_logged fuzz 0 "$t" debug "$((per + 180))" \
            "${RUN_DIR}/fuzz/${t}.log" asan \
            "ASAN_OPTIONS=halt_on_error=1:detect_leaks=1" \
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" -- \
            xmake run "$t" -- "$work_corpus" \
            "-max_total_time=${per}" \
            "-artifact_prefix=${artifact_dir}/" \
            "-rss_limit_mb=1024" \
            "-max_len=${maxlen}" \
            "-timeout=120" \
            "${fargs[@]}")
        FUZZ_EXEC=$((FUZZ_EXEC + 1))

        local after_files after_bytes
        after_files=$(find "$work_corpus" -type f 2>/dev/null | wc -l | tr -d ' ')
        after_bytes=$(find "$work_corpus" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')
        {
            printf 'target\t%s\n' "$t"
            printf 'corpus_before_files\t%s\n' "$before_files"
            printf 'corpus_before_bytes\t%s\n' "$before_bytes"
            printf 'corpus_after_files\t%s\n' "$after_files"
            printf 'corpus_after_bytes\t%s\n' "$after_bytes"
            printf 'artifact_prefix\t%s\n' "$artifact_dir"
            printf 'classification\t%s\n' "$res"
        } >"${RUN_DIR}/fuzz/${t}.corpus-stats.tsv"

        # Classify using NEWLY generated artifacts only (correction #6).
        local base=${FUZZ_ARTIFACT_BASE[$t]:-0}
        local now_arts new_arts
        now_arts=$(count_fuzz_artifacts "$artifact_dir")
        new_arts=$((now_arts - base)); (( new_arts < 0 )) && new_arts=0

        local verdict="PASS"
        case "$res" in
            PASS)
                if (( new_arts > 0 )); then verdict="CRASH"; FUZZ_CRASH=$((FUZZ_CRASH + 1)); mark_hold
                else FUZZ_PASS=$((FUZZ_PASS + 1)); fi
                ;;
            SKIP) verdict="SKIP" ;;
            TIMEOUT)
                # A fuzz process killed by the wrapper timeout is a genuine
                # TIMEOUT regardless of artifacts. With a new timeout-* artifact
                # libFuzzer itself flagged a slow input (CRASH); otherwise the
                # wrapper truncated the run. Either way it is HOLD-worthy: the
                # runner must never label a timed-out run PASS.
                if (( new_arts > 0 )); then verdict="CRASH(timeout-artifact)"; FUZZ_CRASH=$((FUZZ_CRASH + 1))
                else verdict="TIMEOUT(no-artifact)"; fi
                mark_hold
                ;;
            SANITIZER_FAIL|FAIL)
                verdict="CRASH(${res})"; FUZZ_CRASH=$((FUZZ_CRASH + 1)); mark_hold ;;
        esac
        FUZZ_RESULTS+="${t}: ${per}s, ${verdict}"$'\n'
        log "[fuzz] ${t}: ${verdict} (new artifacts=${new_arts})"
        (( KEEP_GOING == 0 )) && { log "[fuzz] KEEP_GOING=0; stopping"; break; }
    done
    log "[fuzz] done (exec=${FUZZ_EXEC} pass=${FUZZ_PASS} crash=${FUZZ_CRASH})"
}

# ===========================================================================
# Phase F — Final Debug confirmation
# ===========================================================================

phase_final_debug() {
    log "[final-debug] configuring clang debug"
    local out; out=$(run_logged final 0 configure debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/final-configure.log" none -- xmake f -c -m debug --toolchain=clang -y)
    [[ "$out" == "PASS" ]] || { log "[final-debug][FAIL] configure"; mark_hold; return 0; }

    out=$(run_logged final 0 build-test-group debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/final-build.log" none -- xmake build -g test)
    [[ "$out" == "PASS" ]] || { log "[final-debug][FAIL] build -g test"; mark_hold; return 0; }

    out=$(run_logged final 0 full-suite debug "$PHASE_TIMEOUT" \
        "${RUN_DIR}/final-debug.log" none -- xmake test -v)
    if [[ "$out" == "PASS" ]]; then FINAL_DEBUG_OK=1; else log "[final-debug][FAIL] full suite"; mark_hold; fi
}

# ===========================================================================
# Self-test (final detail #5) — controlled SYNTHETIC failures; never HOLD.
# ===========================================================================

# Like run_logged but records synthetic FAIL/TIMEOUT WITHOUT touching the
# overnight verdict (no mark_hold, no failures/index.tsv row).
run_logged_synthetic() {
    local phase="$1" iteration="$2" target="$3" cmode="$4" timeout_s="$5"
    local outlog="$6" san_kind="$7"; shift 7
    local -a san_env=()
    while [[ $# -gt 0 ]]; do
        if [[ "$1" == "--" ]]; then shift; break; fi
        san_env+=("$1"); shift
    done
    [[ $# -gt 0 ]] || { log "[${phase}] ERROR: no command"; return 99; }

    local cmd_str; cmd_str=$(printf '%q ' "$@"); cmd_str="${cmd_str% }"
    mkdir -p "$(dirname "$outlog")"
    local start_e; start_e=$(date +%s)
    {
        printf '## sluice overnight SYNTHETIC self-test log\n'
        printf 'synthetic=yes\n'
        printf 'phase=%s\niteration=%s\ntarget=%s\nmode=%s\n' "$phase" "$iteration" "$target" "$cmode"
        printf 'command=%s\n' "$cmd_str"
        printf 'timeout_s=%s\n' "$timeout_s"
        printf 'start_iso=%s\nstart_epoch=%s\n' "$(iso_ts)" "$start_e"
        printf -- '----- output -----\n'
    } >"$outlog"
    local rc=0
    command timeout -s TERM -k "${KILL_AFTER_S}s" "${timeout_s}s" "$@" >>"$outlog" 2>&1 || rc=$?
    local end_e; end_e=$(date +%s)
    local classif="PASS"
    if [[ $rc -eq 124 ]]; then classif="TIMEOUT"; elif [[ $rc -ne 0 ]]; then classif="FAIL"; fi
    {
        printf -- '----- end output -----\n'
        printf 'end_iso=%s\nend_epoch=%s\nduration_s=%s\nraw_exit=%s\n' "$(iso_ts)" "$end_e" \
            "$((end_e - start_e))" "$rc"
        printf 'classification=%s\n' "$classif"
        printf 'synthetic=yes (no overnight HOLD)\n'
    } >>"$outlog"
    append_event "$phase" "$iteration" "$target" "$cmode" "SYNTHETIC_${classif}" \
        "$rc" "$((end_e - start_e))" "$timeout_s" "$outlog" "$@"
    echo "$classif"
}

self_test() {
    MODE="selftest"
    # self-test needs its own tiny run dir + env record + trap.
    setup_run_dir
    setup_environment_record
    WORKTREE_DIFF="${RUN_DIR}/worktree.diff"
    trap finalize_and_exit EXIT
    DEADLINE_EPOCH=$((START_EPOCH + 300))

    local pass=1
    log "[self-test] SYNTHETIC: proving TIMEOUT classification (tiny deadline)"
    local tout; tout=$(run_logged_synthetic selftest 1 timeout-sleep debug 1 \
        "${RUN_DIR}/selftest-timeout.log" none -- sleep 30)
    if [[ "$tout" == "TIMEOUT" ]]; then log "[self-test] TIMEOUT observed: OK"; else
        log "[self-test] FAIL: expected TIMEOUT got ${tout}"; pass=0; fi

    log "[self-test] SYNTHETIC: proving a failed command does not abort the script"
    local failres; failres=$(run_logged_synthetic selftest 2 false-target debug 30 \
        "${RUN_DIR}/selftest-fail.log" none -- false)
    if [[ "$failres" == "FAIL" ]]; then log "[self-test] FAIL observed: OK"; else
        log "[self-test] FAIL: expected FAIL got ${failres}"; pass=0; fi

    log "[self-test] proving execution continues after the failure"
    local okres; okres=$(run_logged_synthetic selftest 3 true-target debug 30 \
        "${RUN_DIR}/selftest-ok.log" none -- true)
    if [[ "$okres" == "PASS" ]]; then log "[self-test] post-failure command ran: OK"; else
        log "[self-test] FAIL: expected PASS got ${okres}"; pass=0; fi

    write_summary
    if (( STICKY_HOLD != 0 )); then log "[self-test] FAIL: synthetic failure leaked into HOLD"; pass=0; fi
    if (( pass == 1 )); then log "[self-test] PASS"; exit "$EXIT_PASS"; else exit "$EXIT_HOLD"; fi
}

# ===========================================================================
# Setup
# ===========================================================================

setup_run_dir() {
    local stamp; stamp=$(date +%Y%m%d-%H%M%S)
    local sha="${HEAD_SHORT:-$(git -C "$PROJECT_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo nogit)}"
    HEAD_SHORT="$sha"
    RUN_DIR="${PROJECT_ROOT}/overnight-artifacts/${stamp}-${sha}"
    mkdir -p "$RUN_DIR"
    RUN_LOG="${RUN_DIR}/run.log"
    EVENTS_TSV="${RUN_DIR}/events.tsv"
    FAILURES_INDEX="${RUN_DIR}/failures/index.tsv"
    mkdir -p "${RUN_DIR}/failures"
    WORKTREE_DIFF="${RUN_DIR}/worktree.diff"
    printf 'ts_epoch\tphase\titeration\ttarget\tmode\tclassification\traw_exit\tduration_s\ttimeout_s\tlog_path\tcommand\n' \
        >"$EVENTS_TSV"
    printf 'ts_epoch\tphase\ttarget\tclassification\tdiagnostic_signature\tlog_path\n' \
        >"$FAILURES_INDEX"
    : >"$RUN_LOG"
    if [[ $WORKTREE_DIRTY -eq 1 ]]; then
        git -C "$PROJECT_ROOT" diff >"${WORKTREE_DIFF}" 2>/dev/null || true
    fi
}

setup_environment_record() {
    {
        printf '## sluice overnight environment\n'
        printf 'date_iso=%s\n' "$(date --iso-8601=seconds 2>/dev/null || date)"
        printf 'head=%s\n' "$HEAD_SHA"
        printf 'worktree_dirty=%s\n' "$WORKTREE_DIRTY"
        printf 'uname=%s\n' "$(uname -a)"
        printf 'clang_version=%s\n' "$(clang++ --version 2>/dev/null | head -1)"
        printf 'xmake_version=%s\n' "$(xmake --version 2>/dev/null | head -1)"
        printf 'mode=%s\n' "$MODE"
        printf 'script_args=%s\n' "$*"
        printf 'budget_hours=%s\n' "$HOURS"
        printf 'started=%s\n' "$(iso_ts "$START_EPOCH")"
        printf 'nproc=%s\n' "$NPROC"
        printf 'phase_timeout_s=%s\n' "$PHASE_TIMEOUT"
        printf 'keep_going=%s\n' "$KEEP_GOING"
        [[ -n "$FUZZ_SECONDS_OVERRIDE" ]] && printf 'fuzz_seconds_override=%s\n' "$FUZZ_SECONDS_OVERRIDE"
        printf '\n## git status --short\n'
        git -C "$PROJECT_ROOT" status --short 2>/dev/null || true
    } >"${RUN_DIR}/environment.txt"
}

require_tools() {
    local missing=0 t
    for t in bash date timeout tee mkdir git xmake clang clang++; do
        if ! command -v "$t" >/dev/null 2>&1; then
            printf 'ERROR: required tool not found: %s\n' "$t" >&2
            missing=1
        fi
    done
    (( missing )) && exit "$EXIT_ARG_ENV"
}

usage() {
    cat <<'EOF'
Usage: scripts/overnight-local.sh [--smoke | --hours N | --self-test | --help]

Runs the local overnight correctness gate (Linux + Clang). REUSES the existing
xmake targets, tests and fuzz harnesses; changes no production code.

Modes:
  (default)            full ~8h gate (Debug soak, TSan hot set, ASan+UBSan,
                       libFuzzer, Final Debug). Budget split: Final Debug is
                       reserved first (max(20m, 1.5x baseline)); remaining is
                       soak 25% / TSan 25% / ASan+UBSan 12.5% / fuzz = rest.
  --smoke              one pass of every phase (NOT a correctness gate; proves
                       the runner wiring). Each fuzz target ~10s.
  --hours N            total budget in hours (default 8; env SLUICE_OVERNIGHT_HOURS).
  --self-test          controlled SYNTHETIC TIMEOUT + keep-going proof; exits 0
                       on success. Never produces an overnight HOLD.
  --help               this message.

Env overrides (CLI args win where applicable):
  SLUICE_OVERNIGHT_HOURS   total budget hours (default 8)
  SLUICE_PHASE_TIMEOUT     per-command timeout seconds (default 1200)
  SLUICE_FUZZ_SECONDS      override TOTAL fuzz budget seconds (default: split)
  SLUICE_KEEP_GOING        0 = stop after the current command but still write
                           summary + logs (default 1)

Verdicts (summary.txt + exit code):
  PASS       exit 0   required gates OK, no failures, every evidence family ran.
  INCOMPLETE exit 0   no real failure, but a required family did not execute.
  HOLD       exit 1   any sticky failure / timeout / sanitizer / new fuzz crash,
                      or a required baseline/Final-Debug failure, or all critical
                      TSan targets unavailable.
  arg/env    exit 2   bad arguments or missing core tools.

Artifacts land in overnight-artifacts/<stamp>-<sha>/ (gitignored): run.log,
events.tsv, summary.txt, environment.txt, worktree.diff, per-config target
snapshots, per-command logs under debug-soak/ tsan/ asanubsan/ fuzz/, and
failures/index.tsv. Persistent fuzz corpus lives under .nightly-corpus/ (never
cleared). This runner does NOT replace deterministic unit tests or formal
models, and does NOT modify code or commit results.
EOF
}

parse_args() {
    MODE="overnight"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --smoke) MODE="smoke"; shift ;;
            --hours)
                [[ $# -ge 2 ]] || die "--hours requires a value"
                HOURS="$2"; shift 2
                [[ "$HOURS" =~ ^[0-9]+\.?[0-9]*$ ]] || die "--hours must be a number"
                ;;
            --self-test) MODE="selftest"; shift ;;
            --help|-h) usage; exit "$EXIT_PASS" ;;
            *) die "unknown argument: $1 (try --help)" ;;
        esac
    done
    # Env defaults (CLI --hours wins; otherwise env, otherwise 8).
    [[ $HOURS -eq 8 ]] && [[ -n "${SLUICE_OVERNIGHT_HOURS:-}" ]] && HOURS="$SLUICE_OVERNIGHT_HOURS"
    [[ -n "${SLUICE_PHASE_TIMEOUT:-}" ]] && PHASE_TIMEOUT="$SLUICE_PHASE_TIMEOUT"
    [[ -n "${SLUICE_FUZZ_SECONDS:-}" ]] && FUZZ_SECONDS_OVERRIDE="$SLUICE_FUZZ_SECONDS"
    [[ -n "${SLUICE_KEEP_GOING:-}" ]] && KEEP_GOING="$SLUICE_KEEP_GOING"
}

# ===========================================================================
# Main
# ===========================================================================

main() {
    PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    cd "$PROJECT_ROOT" || die "cannot cd to project root"

    require_tools
    parse_args "$@"

    START_EPOCH=$(date +%s)
    HEAD_SHA=$(git rev-parse HEAD 2>/dev/null || echo unknown)
    HEAD_SHORT=$(git rev-parse --short=12 HEAD 2>/dev/null || echo nogit)
    [[ -n "$(git status --porcelain 2>/dev/null)" ]] && WORKTREE_DIRTY=1
    NPROC=$(nproc 2>/dev/null || echo 1)

    # self-test short-circuits before budget/deadline math.
    if [[ "$MODE" == "selftest" ]]; then self_test; exit $?; fi

    local fuzz_override=""
    if [[ "$MODE" == "smoke" ]]; then
        HOURS=${SLUICE_OVERNIGHT_HOURS:-1}
        DEADLINE_EPOCH=$((START_EPOCH + HOURS * 3600))
        fuzz_override="30"   # ~10s/target across 3 targets
    else
        local secs; secs=$(awk -v h="$HOURS" 'BEGIN{printf "%d", h*3600}')
        DEADLINE_EPOCH=$((START_EPOCH + secs))
    fi
    [[ -n "$FUZZ_SECONDS_OVERRIDE" ]] && fuzz_override="$FUZZ_SECONDS_OVERRIDE"

    setup_run_dir
    setup_environment_record "$@"
    WORKTREE_DIFF="${RUN_DIR}/worktree.diff"

    trap finalize_and_exit EXIT    # idempotent (correction #4)

    if (( WORKTREE_DIRTY == 1 )); then
        log "[setup] WARNING: worktree is DIRTY — recorded in worktree.diff (not refused)"
    fi
    log "[setup] mode=${MODE} budget_hours=${HOURS} deadline=$(iso_ts "$DEADLINE_EPOCH") run=${RUN_DIR}"

    # Phase A: baseline (fixed).
    phase_baseline

    # Budget split (final detail #6): reserve Final Debug first.
    local remain=$((DEADLINE_EPOCH - BASELINE_EPOCH_END))
    local fd_budget; fd_budget=$(final_debug_budget)
    local pool=$((remain - fd_budget)); (( pool < 0 )) && pool=0
    local soak_b=$((pool * 25 / 100))
    local tsan_b=$((pool * 25 / 100))
    local asan_b=$((pool * 125 / 1000))
    local fuzz_b=$((pool - soak_b - tsan_b - asan_b)); (( fuzz_b < 0 )) && fuzz_b=0
    [[ -n "$fuzz_override" ]] && fuzz_b="$fuzz_override"
    log "[budget] remain=${remain}s final_debug_reserved=${fd_budget}s pool=${pool}s | soak=${soak_b}s tsan=${tsan_b}s asan=${asan_b}s fuzz=${fuzz_b}s"

    phase_debug_soak "$soak_b"
    phase_tsan "$tsan_b"
    phase_asanubsan "$asan_b"
    phase_fuzz "$fuzz_b"

    phase_final_debug

    write_summary
    if (( STICKY_HOLD == 1 )); then exit "$EXIT_HOLD"; else exit "$EXIT_PASS"; fi
}

main "$@"
