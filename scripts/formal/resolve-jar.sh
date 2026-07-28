#!/usr/bin/env bash
# resolve-jar.sh — shared TLA+ jar resolution for Sluice formal verifiers.
#
# Source this file from verifier scripts:
#   source "$(dirname "${BASH_SOURCE[0]}")/resolve-jar.sh"
#
# After sourcing, $TLA2TOOLS_JAR is set to the resolved jar path.
#
# Resolution order:
#   1. TLA2TOOLS_JAR environment variable (if set and file exists)
#   2. User cache populated by bootstrap.py (~/.cache/sluice/formal/tla2tools.jar)
#   3. Fail with actionable message
#
# This script does NOT fall back to /tmp/tla2tools.jar or repo-root jar
# because those paths are not checksum-verified. Use bootstrap.py instead.

_sluice_resolve_jar() {
  # 1. Explicit environment variable
  if [[ -n "${TLA2TOOLS_JAR:-}" ]] && [[ -f "$TLA2TOOLS_JAR" ]]; then
    return 0
  fi

  # 2. User cache (populated by bootstrap.py, checksum-verified)
  local cache_jar="${HOME}/.cache/sluice/formal/tla2tools.jar"
  if [[ -f "$cache_jar" ]]; then
    export TLA2TOOLS_JAR="$cache_jar"
    return 0
  fi

  # 3. Fail with actionable message
  echo "error: tla2tools.jar not found." >&2
  echo "  searched: TLA2TOOLS_JAR, ~/.cache/sluice/formal/" >&2
  echo "  fix: python3 scripts/formal/bootstrap.py" >&2
  return 1
}

# Run resolution immediately when sourced.
if ! _sluice_resolve_jar; then
  exit 2
fi
