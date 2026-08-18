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
  # Lock file next to this script; the sha256 inside is the authority.
  local lock_file="${BASH_SOURCE[0]%/*}/tla2tools.lock.json"
  local expected_sha
  expected_sha="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sha256"])' \
                  "$lock_file" 2>/dev/null || true)"

  _sluice_verify_jar() {
    # Args: jar path. Fails when the lock sha is unreadable or the jar hash
    # mismatches. (Audit 2026-08-18: this script CLAIMED checksum-verified
    # cache resolution but never verified — a silently drifted cache jar
    # (TLC 2026 build overwriting the locked v1.7.4) ran every suite
    # verifier while `verify.py doctor` correctly failed. Fail closed.)
    [[ -n "$expected_sha" ]] || return 1
    local actual_sha
    actual_sha="$(sha256sum "$1" 2>/dev/null | awk '{print $1}')"
    [[ "$actual_sha" = "$expected_sha" ]]
  }

  # 1. Explicit environment variable
  if [[ -n "${TLA2TOOLS_JAR:-}" ]] && [[ -f "$TLA2TOOLS_JAR" ]]; then
    if _sluice_verify_jar "$TLA2TOOLS_JAR"; then
      return 0
    fi
    echo "error: TLA2TOOLS_JAR checksum mismatch (not the locked release): $TLA2TOOLS_JAR" >&2
    echo "  fix: python3 scripts/formal/bootstrap.py" >&2
    return 1
  fi

  # 2. User cache (populated by bootstrap.py; checksum-verified for real)
  local cache_jar="${HOME}/.cache/sluice/formal/tla2tools.jar"
  if [[ -f "$cache_jar" ]]; then
    if _sluice_verify_jar "$cache_jar"; then
      export TLA2TOOLS_JAR="$cache_jar"
      return 0
    fi
    echo "error: cached tla2tools.jar checksum mismatch: $cache_jar" >&2
    echo "  (a different TLC build overwrote the locked cache — silent version drift)" >&2
    echo "  fix: python3 scripts/formal/bootstrap.py" >&2
    return 1
  fi

  # 3. Fail with an actionable message
  echo "error: tla2tools.jar not found." >&2
  echo "  searched: TLA2TOOLS_JAR, ~/.cache/sluice/formal/" >&2
  echo "  fix: python3 scripts/formal/bootstrap.py" >&2
  return 1
}

# Run resolution immediately when sourced.
if ! _sluice_resolve_jar; then
  exit 2
fi
