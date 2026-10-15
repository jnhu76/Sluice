#!/usr/bin/env bash
# Regenerates MANIFEST.sha256.
#
# Section 1 lists every committed artifact with its SHA-256, so a reader can
# check that a working tree matches what the report was written against.
# Section 2 lists the raw perf.data captures, which are deliberately not
# committed; their hashes and sizes are preserved from the previous manifest
# unless the file is present, in which case it is re-measured.
#
# Environment:
#   PERF_DATA_DIR   directory holding the raw <case>.data (default: data/perf)
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
MANIFEST="$EVIDENCE_DIR/MANIFEST.sha256"
PERF_DATA_DIR="${PERF_DATA_DIR:-$EVIDENCE_DIR/data/perf}"
tmp="$(mktemp "${TMPDIR:-/tmp}/sluice-manifest.XXXXXX")"
trap 'rm -f "$tmp"' EXIT

previous_section2=""
if [ -f "$MANIFEST" ]; then
    previous_section2="$(sed -n '/^# raw perf.data/,$p' "$MANIFEST")"
fi

{
    echo "# SHA-256 manifest for the #403 flame-graph profiling evidence"
    echo "# generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "#"
    echo "# Section 1: committed artifacts (path relative to this directory)."
    echo "#           svg/ hashes verify the committed bytes only: flamegraph.pl iterates a"
    echo "#           Perl hash internally, so frame ordering can differ between regenerations"
    echo "#           of identical input (REPORT.md section 3, limitation 9)."
    echo "#           In *.run.log and *.strace.log, host-specific prefixes are replaced with"
    echo "#           \$SLUICE_ROOT, \$EVIDENCE_DIR and \$G0_DISK_DIR; nothing else was edited."
    echo "# Section 2: raw perf captures, deliberately NOT committed (about 371 MB);"
    echo "#           they are the input to tools/fold_stacks.sh. tools/verify_perf_captures.sh"
    echo "#           re-checks them against this section and reports their sample counts."
    echo ""
    (cd "$EVIDENCE_DIR" && find . -type f \
        -not -path "./MANIFEST.sha256" \
        -not -path "./tools/flamegraph/*" \
        | sed 's|^\./||' | LC_ALL=C sort | xargs -d '\n' sha256sum)
    echo ""
    echo "$previous_section2"
} >"$tmp"

# Re-measure any raw capture that is present, so a rebuilt capture cannot keep a
# stale hash.
while read -r hash size name; do
    [ -f "$PERF_DATA_DIR/$name" ] || continue
    new_hash="$(sha256sum "$PERF_DATA_DIR/$name" | awk '{print $1}')"
    new_size="$(stat -c %s "$PERF_DATA_DIR/$name")"
    sed -i "s|^$hash  $size  $name\$|$new_hash  $new_size  $name|" "$tmp"
done < <(printf '%s\n' "$previous_section2" | grep -E '^[0-9a-f]{64}  [0-9]+  ')

mv "$tmp" "$MANIFEST"
echo "wrote $MANIFEST ($(grep -cE '^[0-9a-f]{64}  ' "$MANIFEST") entries)"
