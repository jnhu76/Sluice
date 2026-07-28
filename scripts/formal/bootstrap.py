#!/usr/bin/env python3
"""Bootstrap the TLA+ tools cache for Sluice formal verification.

Downloads the official tla2tools.jar, verifies its SHA-256 against
scripts/formal/tla2tools.lock.json, and places it in the Sluice user cache
directory (~/.cache/sluice/formal/). Safe to re-run: skips download when the
cached jar already matches the lock checksum; fails loudly when a stale cache
has the wrong checksum.

Usage:
    python3 scripts/formal/bootstrap.py            # download if needed
    python3 scripts/formal/bootstrap.py --check    # offline: verify cache only
    python3 scripts/formal/bootstrap.py --force    # re-download even if cached
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
LOCK_FILE = SCRIPT_DIR / "tla2tools.lock.json"
CACHE_ROOT = Path.home() / ".cache" / "sluice" / "formal"


def _read_lock() -> dict:
    try:
        with LOCK_FILE.open("r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"error: lock file not found: {LOCK_FILE}", file=sys.stderr)
        sys.exit(2)
    except json.JSONDecodeError as e:
        print(f"error: lock file is not valid JSON: {e}", file=sys.stderr)
        sys.exit(2)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _cache_path(lock: dict) -> Path:
    filename = lock.get("cache_filename", "tla2tools.jar")
    return CACHE_ROOT / filename


def verify_cache(lock: dict) -> bool:
    """Return True when the cached jar exists and matches the lock checksum."""
    cached = _cache_path(lock)
    if not cached.is_file():
        return False
    expected = lock.get("sha256", "")
    actual = _sha256(cached)
    return bool(expected) and actual == expected


def download(lock: dict) -> Path:
    """Download the jar, verify checksum, atomically place into cache."""
    url = lock.get("official_url", "")
    if not url:
        print("error: lock file has no official_url", file=sys.stderr)
        sys.exit(2)
    expected = lock.get("sha256", "")
    if not expected:
        print("error: lock file has no sha256", file=sys.stderr)
        sys.exit(2)

    CACHE_ROOT.mkdir(parents=True, exist_ok=True)
    cached = _cache_path(lock)

    tmp_fd, tmp_path_str = tempfile.mkstemp(
        prefix=".tla2tools-", suffix=".jar", dir=str(CACHE_ROOT)
    )
    tmp_path = Path(tmp_path_str)
    os.close(tmp_fd)
    try:
        print(f"downloading {url} ...")
        urllib.request.urlretrieve(url, str(tmp_path))  # noqa: S310
        actual = _sha256(tmp_path)
        if actual != expected:
            print(
                f"error: checksum mismatch for downloaded jar\n"
                f"  expected: {expected}\n"
                f"  actual:   {actual}",
                file=sys.stderr,
            )
            sys.exit(2)
        # Atomic rename into the final cache location.
        shutil.move(str(tmp_path), str(cached))
        print(f"cached {cached}  (sha256 {actual})")
        return cached
    finally:
        if tmp_path.exists():
            tmp_path.unlink()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="offline mode: only verify the cache, do not download",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="re-download even when the cache is valid",
    )
    args = parser.parse_args(argv)

    lock = _read_lock()

    if not args.force and verify_cache(lock):
        cached = _cache_path(lock)
        print(f"cache ok: {cached}")
        return 0

    if args.check:
        print(
            "cache MISSING or STALE; --check mode, not downloading",
            file=sys.stderr,
        )
        return 2

    download(lock)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
