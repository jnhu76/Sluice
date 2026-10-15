#!/usr/bin/env bash
# Checks the retained perf captures against the committed records.
#
# The raw perf.data files are not committed (about 371 MB). This script is what
# makes them checkable: it re-reads each one, reports the recorded sample count,
# and compares its SHA-256 with the value MANIFEST.sha256 records, so a
# regenerated or truncated capture cannot be presented as the original.
#
# Environment:
#   PERF_DATA_DIR   directory holding the raw <case>.data (default: data/perf)
#
# Output: data/perf/SAMPLE-COUNTS.md
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
DATA_DIR="$EVIDENCE_DIR/data"
PERF_DATA_DIR="${PERF_DATA_DIR:-$DATA_DIR/perf}"
MANIFEST="$EVIDENCE_DIR/MANIFEST.sha256"

CASES="
w02-tp-4k-d16-w4:W02-threadpool-4k-d16-w4
w02-uring-4k-d16:W02-uring-4k-d16
w02-tp-4k-d16-w1:W02-threadpool-4k-d16-w1
w02-tp-4k-d16-w8:W02-threadpool-4k-d16-w8
w02-uring-4k-d64:W02-uring-4k-d64
w02-tp-64k-d16-w4:W02-threadpool-64k-d16-w4
w03-tp-4k-d16-w4:W03-threadpool-4k-d16-w4
w03-uring-4k-d16:W03-uring-4k-d16
w01-read-4k-seq:W01-read_at-4k-seq
w01-write-4k-seq:W01-write_at-4k-seq
w01-sync-data-4k:W01-sync_data-4k
"

out="$DATA_DIR/perf/SAMPLE-COUNTS.md"
{
    echo "# perf capture integrity"
    echo ""
    echo "Raw \`perf.data\` files are not committed; this table is what makes them"
    echo "checkable. \`manifest\` compares the file's SHA-256 with MANIFEST.sha256"
    echo "section 2. \`case\` and \`reps\` are read back from the run's own JSONL,"
    echo "\`samples\` from \`perf report --stats\`."
    echo ""
    echo "| case | recorded case | warmup+measured | SAMPLE events | size | manifest |"
    echo "|---|---|---|---|---|---|"
} >"$out"

failures=0
for entry in $CASES; do
    name="${entry%%:*}"
    data="$PERF_DATA_DIR/$name.data"
    if [ ! -f "$data" ]; then
        echo "    MISSING $data" >&2
        failures=$((failures + 1))
        continue
    fi
    describe="$(python3 "$TOOLS_DIR/verify_capture.py" "$DATA_DIR/perf/$name.jsonl" 2>/dev/null)"
    case_ran="$(printf '%s\n' "$describe" | sed -n 's/^case_ran=//p')"
    reps="$(printf '%s\n' "$describe" | sed -n 's/^warmup_reps=//p')+$(printf '%s\n' "$describe" | sed -n 's/^measured_reps=//p')"
    samples="$(perf report --stats -i "$data" 2>/dev/null | awk '/SAMPLE events/{print $3; exit}')"
    size="$(stat -c %s "$data")"
    want="$(grep -F "  $name.data" "$MANIFEST" | awk '{print $1}')"
    got="$(sha256sum "$data" | awk '{print $1}')"
    manifest=MISMATCH
    if [ -n "$want" ] && [ "$want" = "$got" ]; then manifest=ok; else failures=$((failures + 1)); fi
    printf '| %s | %s | %s | %s | %s | %s |\n' \
        "$name" "${case_ran:-UNREADABLE}" "${reps:-?}" "${samples:-0}" \
        "$(numfmt --to=iec "$size" 2>/dev/null || echo "$size")" "$manifest" >>"$out"
done

count="$(echo "$CASES" | grep -c ':')"
echo "" >>"$out"
if [ "$failures" -eq 0 ]; then
    echo "All $count captures match the manifest." >>"$out"
else
    echo "FAILURES: $failures capture(s) missing or not matching the manifest." >>"$out"
fi
echo "wrote $out (failures=$failures)"
[ "$failures" -eq 0 ] || exit 1
