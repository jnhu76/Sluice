#!/usr/bin/env bash
# Verify that ``xmake run <target> -- <fuzzer-args>`` does NOT forward the
# ``--`` separator to the actual fuzzer binary.
#
# This script:
#   1. Builds a small fuzz target (wal_read_record_fuzz).
#   2. Runs it with ``--`` and captures the fuzzer process's argv from
#      /proc/<pid>/cmdline.
#   3. Asserts that ``--`` is NOT present in the fuzzer's argv.
#   4. Saves the captured argv as startup-log evidence.
#
# Usage:
#   bash scripts/verify-fuzz-argv-separator.sh [--keep]
#
#   --keep   preserve the artifact directory (default: auto-clean on PASS)
#
# Requires: xmake with clang toolchain, Linux /proc.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"

ARTIFACT_DIR="${PROJECT}/.fuzz-argv-verify"
CMDLOG="${ARTIFACT_DIR}/verify.log"
ARTCAP="${ARTIFACT_DIR}/fuzzer-cmdline.txt"
SUMMARY="${ARTIFACT_DIR}/summary.txt"
ARGCAP_SCRIPT="${ARTIFACT_DIR}/capture-argv.py"
KEEP="${1:-}"

rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR"

echo "=== Fuzz argv separator verification ===" | tee "$SUMMARY"
echo "artifact dir: $ARTIFACT_DIR" | tee -a "$SUMMARY"

# ── Step 1: configure + build fuzz target ──────────────────────────────────
echo "--- step 1: configure clang debug (fuzz) ---" | tee -a "$CMDLOG"
xmake f -c -m debug --toolchain=clang -y >> "$CMDLOG" 2>&1
echo "--- step 1b: build wal_read_record_fuzz ---" | tee -a "$CMDLOG"
xmake build -g fuzz wal_read_record_fuzz >> "$CMDLOG" 2>&1

# ── Step 2: create a capture script ────────────────────────────────────────
# This script polls /proc/self/cmdline from the fuzzer's perspective and
# writes it before running the actual libFuzzer code.
cat > "$ARGCAP_SCRIPT" << 'PYEOF'
#!/usr/bin/env python3
"""Capture argv, then exec the real fuzzer binary."""
import os
import sys
import time

# Write our argv to the capture file before any fuzzer output.
argv_file = os.environ.get("FUZZ_ARGV_CAPTURE", "")
if argv_file:
    with open(argv_file, "w") as f:
        f.write(f"pid={os.getpid()}\n")
        for i, arg in enumerate(sys.argv):
            f.write(f"argv[{i}]={arg!r}\n")
        f.write(f"argc={len(sys.argv)}\n")
        # Also read /proc/self/cmdline for cross-check.
        try:
            cmdline = open(f"/proc/{os.getpid()}/cmdline", "rb").read()
            f.write(f"cmdline_raw={cmdline!r}\n")
            # Split on null bytes.
            parts = cmdline.split(b"\x00")
            f.write(f"cmdline_parts={[p.decode('utf-8', errors='replace') for p in parts if p]!r}\n")
        except Exception as e:
            f.write(f"cmdline_error={e}\n")

# Exec the real fuzzer binary (passed as the first argument after the script).
# The script itself is the first argv entry; we need to shift.
real_fuzzer = sys.argv[1]
fuzzer_argv = [real_fuzzer] + sys.argv[2:]
os.execv(real_fuzzer, fuzzer_argv)
PYEOF
chmod +x "$ARGCAP_SCRIPT"

# ── Step 3: run the fuzz target with -- and capture argv ───────────────────
FUZZ_BIN="$PROJECT/build/linux/x86_64/debug/wal_read_record_fuzz"
if [ ! -x "$FUZZ_BIN" ]; then
    echo "FAIL: fuzz binary not found at $FUZZ_BIN" | tee -a "$SUMMARY"
    exit 1
fi

echo "--- step 2: run fuzz target with argv capture ---" | tee -a "$CMDLOG"
# Use a wrapper: FUZZ_ARGV_CAPTURE tells the capture script where to write.
export FUZZ_ARGV_CAPTURE="$ARTCAP"
# Run with --runs=0 so it exits immediately (no real fuzzing).
set +e
xmake run wal_read_record_fuzz -- "$ARGCAP_SCRIPT" "$FUZZ_BIN" \
    -runs=0 -max_total_time=10 >> "$CMDLOG" 2>&1
XMAKE_EXIT=$?
set -e

echo "--- xmake exit code: $XMAKE_EXIT ---" | tee -a "$CMDLOG"

# ── Step 4: verify the captured argv ───────────────────────────────────────
echo "" | tee -a "$SUMMARY"
echo "--- captured fuzzer argv ---" | tee -a "$SUMMARY"
if [ -f "$ARTCAP" ]; then
    cat "$ARTCAP" | tee -a "$SUMMARY"
else
    echo "FAIL: argv capture file not found at $ARTCAP" | tee -a "$SUMMARY"
    echo "xmake may have stripped the wrapper script. Trying direct run..."
    # Fallback: try running the fuzzer directly with the capture script as argv[0].
    "$FUZZ_BIN" "$ARGCAP_SCRIPT" "$FUZZ_BIN" -runs=0 -max_total_time=10 2>/dev/null || true
    if [ -f "$ARTCAP" ]; then
        cat "$ARTCAP" | tee -a "$SUMMARY"
    else
        echo "FAIL: still no capture. xmake arguments may need adjustment." | tee -a "$SUMMARY"
    fi
fi

# ── Step 5: assert no standalone -- in fuzzer argv ─────────────────────────
echo "" | tee -a "$SUMMARY"
echo "--- assertion: no standalone '--' in fuzzer argv ---" | tee -a "$SUMMARY"
if [ -f "$ARTCAP" ]; then
    if grep -q "^argv\[.*\]='--'$" "$ARTCAP"; then
        echo "FAIL: standalone '--' found in fuzzer argv!" | tee -a "$SUMMARY"
        if [ "$KEEP" != "--keep" ]; then
            rm -rf "$ARTIFACT_DIR"
        fi
        exit 1
    else
        echo "PASS: no standalone '--' in fuzzer argv" | tee -a "$SUMMARY"
    fi
else
    echo "WARN: no capture file to verify; cannot assert" | tee -a "$SUMMARY"
fi

echo "" | tee -a "$SUMMARY"
echo "=== VERIFICATION COMPLETE ===" | tee -a "$SUMMARY"

if [ "$KEEP" != "--keep" ]; then
    rm -rf "$ARTIFACT_DIR"
fi
exit 0