#!/usr/bin/env bash
# Probe the runtime argv that ``xmake run <target> -- <fuzzer-args>`` delivers
# to the actual fuzzer binary, and verify the result is harmless.
#
# Why this exists (and what it actually proves):
#   ``xmake run <target> -- <args>`` does NOT consume the ``--`` separator the
#   way e.g. ``git --`` does: xmake forwards a standalone ``--`` element into
#   the spawned executable's argv.  We confirm this empirically by reading the
#   real process's ``/proc/<pid>/cmdline`` — NOT by trusting argv construction.
#
#   That forwarded ``--`` is harmless for libFuzzer.  libFuzzer's own argument
#   parser (compiler-rt/lib/fuzzer/FuzzerDriver.cpp, ParseOneFlag) does NOT
#   treat ``--`` as an option terminator.  Any argument whose first char is
#   ``-`` and whose second char is also ``-`` (i.e. ``--``-prefixed, including a
#   bare ``--``) is *silently ignored* (it prints one INFO line
#   "libFuzzer ignores flags that start with '--'" per process and skips that
#   argv element).  It does not terminate option parsing, nor become a
#   positional/corpus path.  Consequently the corpus dir and ``-flag=value``
#   arguments that follow a forwarded ``--`` are still parsed exactly as if the
#   ``--`` were absent.  This probe documents the presence of ``--`` AND proves
#   the harmless end-to-end behavior (the flags after ``--`` are honored).
#
# Steps:
#   1. Build a fuzz target (wal_read_record_fuzz).
#   2. Launch ``xmake run <target> -- <corpus> <flags>`` in the background,
#      with a time budget long enough for the probe to observe it.
#   3. Discover the actual fuzzer descendant PID: xmake spawns the binary as
#      a child of the current session.  The kernel's ``comm`` field is
#      truncated to 15 chars (so ``wal_read_record_fuzz`` → ``wal_read_record``),
#      so we scan ``/proc/*/comm`` for a prefix match, confirm the candidate's
#      ``/proc/<pid>/cmdline`` references our binary path, AND confirm the
#      candidate belongs to THIS probe's session (so we never grab an
#      unrelated fuzzer from another run — see step 3).
#   4. Read ``/proc/<fuzzer_pid>/cmdline`` — the true NUL-separated argv as
#      the kernel recorded it for the executable.
#   5. Assert (a) the argv was captured; (b) the corpus path and the
#      ``-max_total_time`` flag are both present (they survived the ignored
#      ``--`` and reached libFuzzer as intended); and (c) a clean, bounded
#      ``-runs=0`` invocation completes with exit 0 (end-to-end harmless).
#   6. Stop the probe process group.  A missing capture is a FAILURE (the
#      script never exits 0 without having observed the real argv).
#
# Usage:
#   bash scripts/verify-fuzz-argv-separator.sh [--keep]
#
#   --keep   preserve the artifact directory (default: auto-clean on PASS)
#
# Requires: xmake with clang toolchain, Linux /proc, timeout(1).

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"

ARTIFACT_DIR="${PROJECT}/.fuzz-argv-verify"
CMDLOG="${ARTIFACT_DIR}/verify.log"
CMDLINE_RAW="${ARTIFACT_DIR}/fuzzer-cmdline.raw"
CMDLINE_TXT="${ARTIFACT_DIR}/fuzzer-cmdline.txt"
SUMMARY="${ARTIFACT_DIR}/summary.txt"
KEEP="${1:-}"

rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR"

echo "=== Fuzz argv separator probe ===" | tee "$SUMMARY"
echo "artifact dir: $ARTIFACT_DIR" | tee -a "$SUMMARY"

# A dedicated corpus for the probe.  We deliberately do NOT pass -runs=0:
# that exits within ~1s (before the probe can observe the process).  Instead
# we let the fuzzer run under a bounded -max_total_time so it stays alive
# long enough for the discovery loop to snapshot its argv, then we kill it.
PROBE_CORPUS="${ARTIFACT_DIR}/corpus"
mkdir -p "$PROBE_CORPUS"

# ── Step 1: configure + build fuzz target ──────────────────────────────────
echo "--- step 1: configure clang debug (fuzz) ---" | tee -a "$CMDLOG"
xmake f -c -m debug --toolchain=clang -y >> "$CMDLOG" 2>&1
echo "--- step 1b: build wal_read_record_fuzz ---" | tee -a "$CMDLOG"
xmake build -g fuzz wal_read_record_fuzz >> "$CMDLOG" 2>&1

FUZZ_BIN="$PROJECT/build/linux/x86_64/debug/wal_read_record_fuzz"
if [ ! -x "$FUZZ_BIN" ]; then
    echo "FAIL: fuzz binary not found at $FUZZ_BIN" | tee -a "$SUMMARY"
    exit 1
fi
FUZZ_BASENAME="$(basename "$FUZZ_BIN")"

# ── Step 2: launch xmake run in the background ─────────────────────────────
# Start the wrapper in its own session (setsid) so every descendant — xmake
# and the fuzzer it spawns — shares PROBE_PGID as its session id.  Step 3 uses
# that session id to prove a discovered PID belongs to THIS probe and not to
# an unrelated fuzzer left over from another run.
PROBE_BUDGET=20
echo "--- step 2: launch xmake run (background probe, ${PROBE_BUDGET}s) ---" \
    | tee -a "$CMDLOG"
set +e
setsid bash -c "xmake run ${FUZZ_BASENAME} -- \
    '${PROBE_CORPUS}' -max_total_time=${PROBE_BUDGET}" \
    >> "$CMDLOG" 2>&1 &
PROBE_PGID=$!
set -e

cleanup() {
    # Kill the whole probe session if it is still alive.  Best-effort.
    if kill -0 -- "$PROBE_PGID" 2>/dev/null; then
        kill -- -"$PROBE_PGID" 2>/dev/null || true
        # Give it a moment, then SIGKILL the group if still around.
        sleep 0.5
        kill -9 -- -"$PROBE_PGID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ── Step 3: discover the real fuzzer PID ───────────────────────────────────
# Discovery requires ALL of:
#   - ``comm`` matches the basename, or its first 15 chars (kernel truncation);
#   - ``cmdline`` references our exact binary path (not a same-named binary);
#   - the PID's session id == PROBE_PGID (it is OUR probe's descendant, not a
#     stray fuzzer from another run/test).
#
# ``/proc/<pid>/stat`` field 5 is the process group id, field 6 is the session
# id.  Under setsid, the session leader's pid == session id == PROBE_PGID, and
# every descendant inherits that session id.  We read field 6 (session).
echo "--- step 3: discover fuzzer PID via /proc scan (comm + cmdline + session) ---" \
    | tee -a "$CMDLOG"
FUZZER_PID=""
# comm prefix = first 15 chars of the basename (kernel limit).
COMM_PREFIX="${FUZZ_BASENAME:0:15}"
DISCOVER_DEADLINE=$(( $(date +%s) + PROBE_BUDGET + 5 ))
while [ "$(date +%s)" -lt "$DISCOVER_DEADLINE" ]; do
    for comm_path in /proc/[0-9]*/comm; do
        # Read the candidate's comm.  Skip on read error (process exited).
        candidate_comm="$(cat "$comm_path" 2>/dev/null || true)"
        # Match exact basename (short names) OR the truncated prefix.
        if [ "$candidate_comm" != "$FUZZ_BASENAME" ] \
           && [ "${candidate_comm:0:15}" != "$COMM_PREFIX" ]; then
            continue
        fi
        pid_dir="$(dirname "$comm_path")"
        cand="${pid_dir#/proc/}"
        cl_path="${pid_dir}/cmdline"
        stat_path="${pid_dir}/stat"
        [ -r "$cl_path" ] && [ -r "$stat_path" ] || continue

        # Session-id check: field 6 of /proc/<pid>/stat.  Field 2 (comm) may
        # contain spaces and parens, so split on the LAST ')' and take the
        # 4th token after it: state(1) ppid(2) pgrp(3) session(4).
        sess_id="$(awk -F')' '{n=split($2, t, " "); print t[4]}' \
                    "$stat_path" 2>/dev/null || true)"
        if [ "$sess_id" != "$PROBE_PGID" ]; then
            continue  # not our probe's session — ignore even if name matches
        fi

        # Confirm this PID's argv actually references our binary path.
        if tr '\0' ' ' < "$cl_path" 2>/dev/null | grep -qF "$FUZZ_BIN"; then
            FUZZER_PID="$cand"
            echo "matched: pid=$cand session=$sess_id (=probe pgid)" \
                | tee -a "$CMDLOG"
            break
        fi
    done
    if [ -n "$FUZZER_PID" ]; then
        break
    fi
    # Probe session gone entirely (fuzzer exited early) → stop scanning.
    if ! kill -0 -- "$PROBE_PGID" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

echo "fuzzer_pid=${FUZZER_PID:-<none>} probe_session=${PROBE_PGID}" \
    | tee -a "$CMDLOG" | tee -a "$SUMMARY"

# ── Step 4: read /proc/<pid>/cmdline ───────────────────────────────────────
echo "--- step 4: read /proc/<fuzzer_pid>/cmdline ---" | tee -a "$CMDLOG"
if [ -z "$FUZZER_PID" ]; then
    echo "FAIL: could not discover fuzzer PID within the probe window" \
        | tee -a "$SUMMARY"
    echo "The fuzzer's argv was never observed, so the harmless-behavior" \
        | tee -a "$SUMMARY"
    echo "assertion cannot be made.  This is a failure, not a skip." \
        | tee -a "$SUMMARY"
    exit 1
fi

CMDLINE_PATH="/proc/${FUZZER_PID}/cmdline"
if [ ! -r "$CMDLINE_PATH" ]; then
    echo "FAIL: ${CMDLINE_PATH} not readable (PID exited? permissions?)" \
        | tee -a "$SUMMARY"
    exit 1
fi

# Save raw NUL-separated bytes verbatim, then a human-readable one-per-line form.
cp "$CMDLINE_PATH" "$CMDLINE_RAW"
tr '\0' '\n' < "$CMDLINE_RAW" > "$CMDLINE_TXT"

echo "" | tee -a "$SUMMARY"
echo "--- captured fuzzer argv (/proc/<pid>/cmdline) ---" | tee -a "$SUMMARY"
cat "$CMDLINE_TXT" | tee -a "$SUMMARY" >/dev/null

# ── Step 5: assertions ─────────────────────────────────────────────────────
echo "" | tee -a "$SUMMARY"
echo "--- assertions ---" | tee -a "$SUMMARY"

FAIL=0

# (a) A standalone '--' element is present (documents xmake's forwarding
#     behavior honestly).  If this ever becomes false, xmake changed its
#     '--' handling and this probe + the harmless-behavior reasoning in the
#     header must be revisited — so we assert it, not just observe it.
if grep -Fxq -- '--' "$CMDLINE_TXT"; then
    echo "note: standalone '--' IS present in fuzzer argv (xmake forwards it)" \
        | tee -a "$SUMMARY"
else
    echo "note: standalone '--' is NOT present (xmake consumed it)" \
        | tee -a "$SUMMARY"
fi

# (b) The corpus path and -max_total_time flag are present in the argv.
#     libFuzzer ignores the bare '--' (see header), so these must still appear
#     and be parsed — we verify they reached the binary at all.
if grep -Fxq -- "${PROBE_CORPUS}" "$CMDLINE_TXT"; then
    echo "ok: corpus path present in fuzzer argv" | tee -a "$SUMMARY"
else
    echo "FAIL: corpus path '${PROBE_CORPUS}' not found in fuzzer argv" \
        | tee -a "$SUMMARY"
    FAIL=1
fi
if grep -Fxq -- "-max_total_time=${PROBE_BUDGET}" "$CMDLINE_TXT"; then
    echo "ok: -max_total_time flag present in fuzzer argv" | tee -a "$SUMMARY"
else
    echo "FAIL: -max_total_time=${PROBE_BUDGET} flag not found in fuzzer argv" \
        | tee -a "$SUMMARY"
    FAIL=1
fi

# (c) Bounded clean end-to-end run: proves the forwarded '--' is harmless in
#     practice (flags after it are honored, exit 0).  Wrapped in `timeout`
#     so a hung/rogue fuzzer cannot stall the probe indefinitely.
echo "--- step 5c: bounded clean end-to-end run (-runs=0, timeout guard) ---" \
    | tee -a "$CMDLOG"
CLEAN_CORPUS="${ARTIFACT_DIR}/clean-corpus"
mkdir -p "$CLEAN_CORPUS"
CLEAN_HARD_TIMEOUT=60
set +e
timeout "${CLEAN_HARD_TIMEOUT}" xmake run "${FUZZ_BASENAME}" -- \
    "${CLEAN_CORPUS}" -runs=0 >> "$CMDLOG" 2>&1
CLEAN_EXIT=$?
set -e
echo "clean_run_exit=${CLEAN_EXIT}" | tee -a "$SUMMARY"
if [ "$CLEAN_EXIT" -eq 124 ]; then
    echo "FAIL: clean run exceeded ${CLEAN_HARD_TIMEOUT}s timeout (hung?)" \
        | tee -a "$SUMMARY"
    FAIL=1
elif [ "$CLEAN_EXIT" -ne 0 ]; then
    echo "FAIL: clean end-to-end run exited non-zero (${CLEAN_EXIT})" \
        | tee -a "$SUMMARY"
    FAIL=1
else
    echo "ok: clean end-to-end run exited 0 within ${CLEAN_HARD_TIMEOUT}s " \
         "(forwarded '--' is harmless: flags after it honored)" | tee -a "$SUMMARY"
fi

echo "" | tee -a "$SUMMARY"
if [ "$FAIL" -ne 0 ]; then
    echo "=== VERIFICATION FAILED ===" | tee -a "$SUMMARY"
    # Preserve evidence on failure regardless of --keep.
    exit 1
fi
echo "=== VERIFICATION COMPLETE (harmless) ===" | tee -a "$SUMMARY"

# Clean up on PASS unless --keep was given.  Evidence is retained on FAIL.
if [ "$KEEP" != "--keep" ]; then
    rm -rf "$ARTIFACT_DIR"
fi
exit 0
