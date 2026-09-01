#!/usr/bin/env python3
"""import_host.py — bring a CHUNK-E0 host evidence archive into the repo (#270).

    python3 research/chunk-e0/scripts/import_host.py <archive.tar.zst|tar.gz>
        [--expect-sha256 <hex>] [--allow-nonvalid] [--dry-run]

Verification pipeline (fail-closed, in order):
  1. archive integrity — sha256 checked against the `.sha256` sidecar and/or
     `--expect-sha256`; with neither, the import REFUSES (compute the hash
     out-of-band first).
  2. extraction — safe-member check (relative paths only, no traversal).
  3. member integrity — every file re-hashed against the archive's inner
     SHA256SUMS.
  4. schema — session/manifest.json kind=sweep, runner.json present,
     gates.json gate_errors == 0, run_ids exactly equal to the frozen
     seeded ordering (driver.ordered_run_ids — single ordering truth),
     per-cell repetition counts complete.
  5. status — status.txt must be VALID unless --allow-nonvalid (non-VALID
     sessions are placed but are never formal evidence).

On success the session is placed at
    research/chunk-e0/results/host-<host-id>/<session-id>/
plus the archive's plots/ under the same parent, and an IMPORT-NOTE.json
records provenance. This script NEVER commits, merges, or edits any report.
Imported evidence is HOST-LOCAL RESULT ONLY.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CAMPAIGN_DIR = REPO / "research" / "chunk-e0"
SCRIPTS_DIR = CAMPAIGN_DIR / "scripts"
RESULTS_DIR = CAMPAIGN_DIR / "results"
PLOTS_DEST = CAMPAIGN_DIR / "plots-host"

sys.path.insert(0, str(SCRIPTS_DIR))
import chunk_e0 as driver  # noqa: E402


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def fail(msg: str) -> None:
    print(f"IMPORT REFUSED — {msg}", file=sys.stderr)
    sys.exit(1)


def verify_outer(archive: Path, expect: str | None) -> str:
    got = sha256_file(archive)
    sidecar = Path(str(archive) + ".sha256")
    if sidecar.is_file():
        recorded = sidecar.read_text().split()[0].strip().lower()
        if recorded != got:
            fail(f"archive sha256 mismatch vs sidecar: {got} != {recorded}")
        print(f"[import] outer sha256 OK vs sidecar: {got}")
    elif expect:
        if expect.lower() != got:
            fail(f"archive sha256 mismatch vs --expect-sha256: "
                 f"{got} != {expect.lower()}")
        print(f"[import] outer sha256 OK vs --expect-sha256: {got}")
    else:
        fail(f"no .sha256 sidecar next to {archive.name} and no "
             f"--expect-sha256 given (computed {got}; verify out-of-band "
             "and retry)")
    return got


def safe_extract(archive: Path, dest: Path) -> None:
    if archive.name.endswith(".tar.zst"):
        rc = subprocess.run(["tar", "-I", "zstd", "-xf", str(archive),
                             "-C", str(dest)]).returncode
        if rc != 0:
            fail("tar -I zstd extraction failed (is zstd installed?)")
        return
    import tarfile
    try:
        with tarfile.open(archive, "r:*") as tf:
            for m in tf.getmembers():
                if m.name.startswith("/") or ".." in Path(m.name).parts:
                    fail(f"unsafe archive member: {m.name}")
            tf.extractall(dest)  # noqa: S202 — members vetted above
    except tarfile.TarError as e:
        fail(f"extraction failed: {e}")


def verify_members(stage: Path) -> None:
    sums = stage / "SHA256SUMS"
    if not sums.is_file():
        fail("archive has no SHA256SUMS (not produced by run_host.py "
             "package step?)")
    checked = 0
    for line in sums.read_text().splitlines():
        if not line.strip():
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            fail(f"malformed SHA256SUMS line: {line!r}")
        want, rel = parts[0].strip(), parts[1].strip()
        p = stage / rel
        if not p.is_file():
            fail(f"SHA256SUMS member missing: {rel}")
        if sha256_file(p) != want:
            fail(f"member hash mismatch: {rel}")
        checked += 1
    print(f"[import] member integrity OK: {checked} files re-hashed")


def find_session_dir(stage: Path) -> Path:
    hits = [p.parent for p in stage.rglob("manifest.json")
            if (p.parent / "raw" / "runs.jsonl").is_file()]
    if len(hits) != 1:
        fail(f"expected exactly one session dir, found {len(hits)}")
    return hits[0]


def verify_session(session_dir: Path, allow_nonvalid: bool) -> dict:
    manifest = json.loads((session_dir / "manifest.json").read_text())
    if manifest.get("kind") != "sweep":
        fail(f"session kind={manifest.get('kind')} != sweep (this importer "
             "only archives formal sweep sessions)")
    am_path = session_dir / "ARCHIVE-MANIFEST.json"
    if not am_path.is_file():
        fail("ARCHIVE-MANIFEST.json missing (not produced by run_host.py "
             "package step?)")
    archive_manifest = json.loads(am_path.read_text())
    # The archive dir is fixed-name "session/"; ARCHIVE-MANIFEST carries the
    # real session id for placement.
    session_id = archive_manifest.get("session")
    if not session_id or "/" in str(session_id):
        fail(f"ARCHIVE-MANIFEST session id invalid: {session_id!r}")
    rj = session_dir / "runner.json"
    if not rj.is_file():
        fail("runner.json missing (not a runner-created full session)")
    runner = json.loads(rj.read_text())
    gates = json.loads((session_dir / "gates.json").read_text())
    if gates.get("gate_errors", -1) != 0:
        fail(f"gates.json gate_errors={gates.get('gate_errors')}")

    # Bind the driver to the extracted tree, then reuse the single ordering
    # authority for run-id provenance.
    driver.RESULTS = session_dir.parent
    sid = session_dir.name
    runs = driver.load_runs(sid)
    ids = [r["run_id"] for r in runs]
    cells = [(c, d) for c in driver.CHUNKS for d in driver.DEPTHS]
    expected = driver.ordered_run_ids(cells, driver.ROUNDS)
    if sorted(ids) != sorted(expected):
        missing = [i for i in expected if i not in set(ids)]
        extra = [i for i in set(ids) if i not in set(expected)]
        fail(f"run-id provenance broken: missing={missing[:5]} "
             f"extra={extra[:5]}")
    if len(ids) != len(set(ids)):
        fail("duplicate run_ids")
    counts: dict[tuple, int] = {}
    for r in runs:
        if not r.get("ok"):
            fail(f"non-ok run recorded: {r['run_id']}")
        counts[(r["chunk"], r["depth"])] = \
            counts.get((r["chunk"], r["depth"]), 0) + 1
    bad = [cd for cd in cells if counts.get(cd, 0) != driver.ROUNDS]
    if bad:
        fail(f"repetition count wrong for {len(bad)} cells "
             f"(e.g. {bad[0]}: {counts.get(bad[0], 0)})")

    status = (session_dir / "status.txt").read_text().strip() \
        if (session_dir / "status.txt").is_file() else "UNKNOWN"
    if status != "VALID" and not allow_nonvalid:
        fail(f"session status={status}; pass --allow-nonvalid to import "
             "a non-VALID session (it will NOT be formal evidence)")
    print(f"[import] session schema OK: {len(runs)} runs, 0 gate errors, "
          f"ordering provenance OK, status={status}")
    return {"runner": runner, "status": status, "runs": len(runs),
            "session_id": session_id}


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Import a CHUNK-E0 host evidence archive (#270).")
    ap.add_argument("archive", type=Path)
    ap.add_argument("--expect-sha256", default=None,
                    help="expected archive sha256 (when no .sha256 sidecar)")
    ap.add_argument("--allow-nonvalid", action="store_true",
                    help="allow importing non-VALID sessions "
                         "(never formal evidence)")
    ap.add_argument("--dry-run", action="store_true",
                    help="verify only; place nothing")
    args = ap.parse_args()

    archive = args.archive.resolve()
    if not archive.is_file():
        fail(f"archive not found: {archive}")
    outer_sha = verify_outer(archive, args.expect_sha256)

    with tempfile.TemporaryDirectory(prefix="chunk-e0-import-") as td:
        stage = Path(td)
        safe_extract(archive, stage)
        verify_members(stage)
        session_dir = find_session_dir(stage)
        info = verify_session(session_dir, args.allow_nonvalid)
        plots_dir = next((p for p in stage.iterdir()
                          if p.is_dir() and p.name == "plots"), None)

        host_id = info["runner"].get("host_id", "UNKNOWN-host")
        sid = info["session_id"]
        dest = RESULTS_DIR / f"host-{host_id}" / sid
        if dest.exists():
            fail(f"destination already exists: {dest}")
        has_plots = plots_dir is not None and plots_dir.is_dir() and \
            any(plots_dir.glob("*.svg"))
        print(f"[import] plan: {archive.name} -> {dest}"
              + (f" + plots -> {PLOTS_DEST}" if has_plots else ""))
        if args.dry_run:
            print("[import] DRY RUN OK — verification passed, "
                  "nothing placed")
            return
        shutil.copytree(session_dir, dest)
        if has_plots:
            PLOTS_DEST.mkdir(parents=True, exist_ok=True)
            for f in plots_dir.glob("*.svg"):
                shutil.copy2(f, PLOTS_DEST / f"{host_id}-{f.name}")
        (dest / "IMPORT-NOTE.json").write_text(json.dumps({
            "schema": "chunk-e0-import-1.0",
            "imported_at_utc": datetime.now(timezone.utc)
            .strftime("%Y-%m-%dT%H:%M:%SZ"),
            "archive": archive.name,
            "archive_sha256": outer_sha,
            "host_id": host_id,
            "session": sid,
            "session_status": info["status"],
            "formal_evidence": info["status"] == "VALID",
            "note": "HOST-LOCAL RESULT ONLY. Placed by import_host.py; "
                    "no commit/merge/report edits performed.",
        }, indent=1) + "\n")
    print(f"[import] PLACED: {dest}")
    print("[import] reminder: no auto commit — review, then commit "
          "deliberately. Claims stay HOST-LOCAL.")


if __name__ == "__main__":
    main()
