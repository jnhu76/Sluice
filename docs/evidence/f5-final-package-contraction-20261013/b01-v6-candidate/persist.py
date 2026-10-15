#!/usr/bin/env python3
"""Assemble the durable evidence subset of an F5 B-01 v6 capture.

The capture directory holds four clean-room builds (before/after x no-ring/
real-ring), their installed prefixes, per-probe logs, per-consumer link maps and
the archive-member symbol dumps. Only the text artefacts are durable evidence
here: every log, closure, link map and probe source, plus the command ledger
with the sha256 of every artefact that is reproducible from the two pinned SHAs
(the source archives and the installed-prefix tarballs). Binaries and archives
stay in the execution tree and are re-derivable with `reproduce.py`.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import tarfile

TEXT_SUFFIXES = (".log", ".json", ".cpp", ".txt", ".map", ".hpp", ".lua", ".symbols")
DIRECTORIES = [
    "before-consumers-n", "before-consumers-y", "after-consumers-n", "after-consumers-y",
    "before-evidence-n", "before-evidence-y", "after-evidence-n", "after-evidence-y",
    "before-members-n", "before-members-y", "after-members-n", "after-members-y",
]

parser = argparse.ArgumentParser()
parser.add_argument("--capture", required=True)
parser.add_argument("--destination", required=True)
args = parser.parse_args()

capture = pathlib.Path(args.capture).resolve()
destination = pathlib.Path(args.destination).resolve()
destination.mkdir(parents=True, exist_ok=True)

for name in ("commands.json", "summary.json", "inventory.lua"):
    shutil.copyfile(capture / name, destination / name)
shutil.copytree(capture / "toolchain", destination / "toolchain", dirs_exist_ok=True)
if pathlib.Path(__file__).resolve() != destination / "persist.py":
    shutil.copyfile(__file__, destination / "persist.py")

manifest = {}
with tarfile.open(destination / "raw-package-evidence.tar.gz", "w:gz") as archive:
    for directory in DIRECTORIES:
        for path in sorted((capture / directory).rglob("*")):
            if not path.is_file() or path.suffix not in TEXT_SUFFIXES:
                continue
            relative = str(path.relative_to(capture))
            manifest[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
            archive.add(path, arcname=relative)
(destination / "raw-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

summary = json.loads((destination / "summary.json").read_text())
artifact_hashes = {}
for stage in ("before", "after"):
    for ring in ("n", "y"):
        artifact_hashes[stage + "-" + ring + "-package"] = summary[stage + "-" + ring]["package_sha256"]
        artifact_hashes[stage + "-" + ring + "-source-archive"] = summary[stage + "-" + ring]["source_archive_sha256"]
(destination / "excluded-artifacts.json").write_text(json.dumps(artifact_hashes, indent=2) + "\n")

print("persisted", len(manifest), "text artefacts; source and prefix archives recorded by hash")
