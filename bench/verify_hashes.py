#!/usr/bin/env python3
"""Re-checks every sha256 recorded in COMMIT.txt against the committed bytes.

COMMIT.txt lists bare file names under section headers, so the section decides
the directory: the v2 block resolves under raw/ and the v1 block under
raw-v1-oracle-degenerate/, while the archived-captures and removed-artifact
blocks already carry their own evidence-relative path. Two kinds of entry are not
plain files: the trace's uncompressed hash, which is verified by decompressing
the committed .gz in memory, and the artifacts the evidence review removed on
purpose, which are reported as uncommitted rather than missing.

Exit status is 0 only when every entry that should be present matches.
"""
from __future__ import annotations

import gzip
import hashlib
import pathlib
import re
import sys

EVIDENCE = pathlib.Path(__file__).resolve().parent.parent / (
    "docs/evidence/g0-403-performance-baseline-20261015")

SECTIONS = (
    ("raw evidence hashes (v2", "raw", False),
    ("raw evidence hashes (v1", "raw-v1-oracle-degenerate", False),
    ("archived captures", "", False),
    ("removed after the evidence review", "", True),
)

ENTRY = re.compile(r"^([0-9a-f]{64})\s+(\S+)(.*)$")


def section_for(header: str):
    for marker, directory, uncommitted in SECTIONS:
        if marker in header:
            return directory, uncommitted
    return None, False


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    text = (EVIDENCE / "COMMIT.txt").read_text()
    directory, uncommitted = None, False
    checked = uncommitted_entries = 0
    problems = []

    for line in text.splitlines():
        if line.startswith("#"):
            header = line.lstrip("# ").strip()
            found, is_uncommitted = section_for(header)
            if found is not None or is_uncommitted:
                directory, uncommitted = found, is_uncommitted
            continue
        match = ENTRY.match(line.strip())
        if not match or directory is None:
            continue
        digest, name, trailer = match.groups()
        if uncommitted:
            uncommitted_entries += 1
            continue
        if "(uncompressed" in trailer:
            archive = EVIDENCE / directory / (name + ".gz")
            if not archive.is_file():
                problems.append(f"{name}: {archive.relative_to(EVIDENCE)} is missing")
                continue
            actual = hashlib.sha256(gzip.decompress(archive.read_bytes())).hexdigest()
            if actual != digest:
                problems.append(f"{name}: restored bytes hash {actual}")
            else:
                checked += 1
                print(f"ok  {name} (decompressed from {archive.name})")
            continue
        path = EVIDENCE / directory / name
        if not path.is_file():
            problems.append(f"{name}: {path.relative_to(EVIDENCE)} is missing")
            continue
        actual = sha256_file(path)
        if actual != digest:
            problems.append(f"{name}: committed bytes hash {actual}")
        else:
            checked += 1
            print(f"ok  {path.relative_to(EVIDENCE)}")

    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"verify_hashes: {checked} entries match, {len(problems)} problem(s), "
          f"{uncommitted_entries} entries deliberately uncommitted")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
