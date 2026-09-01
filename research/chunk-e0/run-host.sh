#!/usr/bin/env bash
# run-host.sh — CHUNK-E0 portable one-command host runner wrapper (#270).
#
# Thin dispatcher: locates a python3 and invokes the orchestration authority
# research/chunk-e0/scripts/run_host.py with the same arguments. All logic,
# preflight, measurement orchestration, validation, plotting, packaging and
# host-summary generation lives in run_host.py (the Python runner is the
# single execution authority; this file exists only for `./run-host.sh`
# ergonomics).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/scripts/run_host.py"

PYTHON=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1; then
        PYTHON="$cand"
        break
    fi
done
if [ -z "$PYTHON" ]; then
    echo "run-host.sh: no python3 found — a working Python 3 is required." >&2
    echo "run-host.sh: install python3 (e.g. 'dnf install python3' or 'apt install python3')." >&2
    exit 127
fi

exec "$PYTHON" "$SCRIPT" "$@"
