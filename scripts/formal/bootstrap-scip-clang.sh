#!/usr/bin/env bash
# Bootstrap the pinned scip-clang binary for the FTLR-0 SCIP pilot (#299).
#
# Mirrors the tla2tools bootstrap discipline (scripts/formal/bootstrap.py +
# tla2tools.lock.json): a pinned artifact, verified by sha256, installed
# into a gitignored build directory. The SCIP index itself is never
# committed; a clean checkout re-runs this script and 'formal_impact.py
# index'.
#
# USAGE
#   bash scripts/formal/bootstrap-scip-clang.sh
#
# FAIL-CLOSED
#   exit 0  -> binary installed and sha256-verified against the lock file
#   exit >0 -> download or verification failed; nothing is trusted
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

LOCK_FILE="${SCRIPT_DIR}/scip-clang.lock.json"
INSTALL_DIR="${REPO_ROOT}/build/formal-impact/bin"
INSTALL_PATH="${INSTALL_DIR}/scip-clang"

URL="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["download_url"])' "${LOCK_FILE}")"
EXPECTED_SHA="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sha256"])' "${LOCK_FILE}")"
VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "${LOCK_FILE}")"

if [ "${EXPECTED_SHA}" = "FILL_ME_BEFORE_COMMIT" ]; then
    echo "error: ${LOCK_FILE} still carries the placeholder sha256" >&2
    echo "       (the lock must be pinned before the pilot is committed)" >&2
    exit 1
fi

mkdir -p "${INSTALL_DIR}"

if [ -x "${INSTALL_PATH}" ]; then
    ACTUAL_SHA="$(sha256sum "${INSTALL_PATH}" | cut -d' ' -f1)"
    if [ "${ACTUAL_SHA}" = "${EXPECTED_SHA}" ]; then
        echo "==> scip-clang ${VERSION} already installed and verified: ${INSTALL_PATH}"
        exit 0
    fi
    echo "==> existing binary hash mismatch; re-downloading"
    rm -f "${INSTALL_PATH}"
fi

TMP_PATH="${INSTALL_PATH}.download"
echo "==> downloading scip-clang ${VERSION} from ${URL}"
if command -v curl >/dev/null 2>&1; then
    curl --fail --location --silent --show-error --output "${TMP_PATH}" "${URL}"
else
    echo "error: curl is required" >&2
    exit 1
fi

ACTUAL_SHA="$(sha256sum "${TMP_PATH}" | cut -d' ' -f1)"
if [ "${ACTUAL_SHA}" != "${EXPECTED_SHA}" ]; then
    echo "error: downloaded scip-clang sha256 mismatch" >&2
    echo "       expected: ${EXPECTED_SHA}" >&2
    echo "       actual:   ${ACTUAL_SHA}" >&2
    rm -f "${TMP_PATH}"
    exit 1
fi

mv "${TMP_PATH}" "${INSTALL_PATH}"
chmod +x "${INSTALL_PATH}"
echo "==> scip-clang ${VERSION} installed (sha256 verified): ${INSTALL_PATH}"
"${INSTALL_PATH}" --version
