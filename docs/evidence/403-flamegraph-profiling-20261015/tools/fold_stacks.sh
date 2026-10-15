#!/usr/bin/env bash
# Folds every captured perf.data into FlameGraph input (gzipped).
#
# Needs root: kernel symbol resolution reads /proc/kallsyms, which is zeroed for
# unprivileged readers.
#
# Environment:
#   FLAMEGRAPH_DIR  checkout of https://github.com/brendangregg/FlameGraph
#                   (default: $TOOLS_DIR/flamegraph). The scripts are CDDL-1.0
#                   and are deliberately not vendored into this MIT repository.
set -uo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
EVIDENCE_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
DATA_DIR="$EVIDENCE_DIR/data"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$TOOLS_DIR/flamegraph}"

if [ ! -f "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]; then
    echo "missing $FLAMEGRAPH_DIR/stackcollapse-perf.pl" >&2
    echo "clone https://github.com/brendangregg/FlameGraph there or set FLAMEGRAPH_DIR" >&2
    exit 1
fi

mkdir -p "$DATA_DIR/folded"

for f in "$DATA_DIR"/perf/*.data; do
    name="$(basename "$f" .data)"
    echo "=== fold $name $(date -Is)"
    perf script -f -i "$f" 2>/dev/null \
        | perl "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" --kernel \
        | gzip -9 > "$DATA_DIR/folded/$name.folded.gz"
    perf script -f -i "$f" 2>/dev/null \
        | perl "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" --kernel --tid \
        | gzip -9 > "$DATA_DIR/folded/$name.tid.folded.gz"
    echo "    $(zcat "$DATA_DIR/folded/$name.folded.gz" | wc -l) folded lines"
done

echo "=== fold done $(date -Is)"
