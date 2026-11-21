#!/usr/bin/env bash
# Shared TLC bootstrap: resolves a pinned, checksum-verified tla2tools.jar.
# Sourced by scripts/verify_tla.sh and scripts/verify_progress_source_model.sh;
# the caller sets `tla` to its formal/tla directory before sourcing.

if [[ -z "${tla:-}" ]]; then
    echo "tlc_bootstrap.sh must be sourced with tla set" >&2
    exit 1
fi

TLA2TOOLS_VERSION="1.7.4"
TLA2TOOLS_SHA256="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
TLA2TOOLS_URL="https://github.com/tlaplus/tlaplus/releases/download/v${TLA2TOOLS_VERSION}/tla2tools.jar"

jar="$tla/tla2tools.jar"

jar_sha() {
    sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

if [[ -n "${SLUICE_TLA2TOOLS_JAR:-}" ]]; then
    if [[ ! -f "$SLUICE_TLA2TOOLS_JAR" ]]; then
        echo "SLUICE_TLA2TOOLS_JAR=$SLUICE_TLA2TOOLS_JAR not found: cannot verify" >&2
        exit 1
    fi
    if [[ "$(jar_sha "$SLUICE_TLA2TOOLS_JAR")" != "$TLA2TOOLS_SHA256" ]]; then
        echo "SLUICE_TLA2TOOLS_JAR=$SLUICE_TLA2TOOLS_JAR SHA-256 mismatch: refusing to verify" >&2
        echo "expected: $TLA2TOOLS_SHA256" >&2
        exit 1
    fi
    jar="$SLUICE_TLA2TOOLS_JAR"
    echo "== TLC jar (SLUICE_TLA2TOOLS_JAR override, checksum ok): $jar =="
elif [[ -f "$jar" && "$(jar_sha "$jar")" == "$TLA2TOOLS_SHA256" ]]; then
    echo "== TLC jar (cached, checksum ok): v$TLA2TOOLS_VERSION =="
elif [[ -f "$jar" ]]; then
    echo "formal/tla/tla2tools.jar exists but its SHA-256 does not match pinned" >&2
    echo "v$TLA2TOOLS_VERSION ($TLA2TOOLS_SHA256); delete it, or point SLUICE_TLA2TOOLS_JAR" >&2
    echo "at a known jar: cannot verify against an untrusted binary" >&2
    exit 1
else
    echo "== TLC jar absent: bootstrapping pinned v$TLA2TOOLS_VERSION =="
    curl -fSL --retry 3 -o "$jar" "$TLA2TOOLS_URL" || {
        rm -f "$jar"
        echo "download of tla2tools v$TLA2TOOLS_VERSION failed: cannot verify" >&2
        echo "needs: java, curl and network; or pre-provision SLUICE_TLA2TOOLS_JAR" >&2
        exit 1
    }
    if [[ "$(jar_sha "$jar")" != "$TLA2TOOLS_SHA256" ]]; then
        rm -f "$jar"
        echo "tla2tools SHA-256 mismatch: refusing to verify" >&2
        exit 1
    fi
fi
