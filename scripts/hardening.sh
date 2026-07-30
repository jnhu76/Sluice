#!/usr/bin/env bash
# SLUICE-LOCAL-HARDENING-GATE-1 — thin shell wrapper for the Python runner.
#
# This file is a ~15-line compatibility shim that delegates to the Python
# implementation in scripts/hardening.py. All logic (process management,
# timeouts, sanitizer classification, preflight, reporting, verdict) lives in
# the Python code.
#
#   ./scripts/hardening.sh             # full ~8h gate (default)
#   ./scripts/hardening.sh --smoke     # prove the runner wiring
#   ./scripts/hardening.sh --hours 6
#   ./scripts/hardening.sh --self-test # synthetic self-test
#   ./scripts/hardening.sh --help

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${SLUICE_HARDENING_PYTHON:-python3}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    printf 'ERROR: %s not found (set SLUICE_HARDENING_PYTHON or install Python >= 3.10)\n' "$PYTHON" >&2
    exit 2
fi

exec "$PYTHON" "$HERE/hardening.py" "$@"