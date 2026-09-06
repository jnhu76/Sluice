#!/usr/bin/env python3
"""FDG-0 Phase C — historical candidate inventory (issue #298 Phase C).

Answers ONE bounded question:

    which real historical commits are candidates for the historical
    gold corpus, classified by deterministic, auditable rules?

Hard rule (Phase C task book §0/§8): this inventory is produced BEFORE any
gold label and BEFORE any resolver run. It uses only ordinary git history
(log / show / diff) and the CURRENT registry/manifest authorities to flag
touches; it never runs formal_impact.py and never scores anything.

Candidate categories are deterministic sampling heuristics derived from
commit subjects and changed paths — NOT gold labels. Gold assignment is a
separate, later, resolver-independent human audit recorded in
docs/results/formal/fdg0-phase-c-gold.json.

Usage:
    python3 scripts/formal/fdg0_phase_c_inventory.py \
        [--range BASE_SHA..HEAD_SHA] [--out PATH] [--summary-only]

Default range: repository root .. the FDG-0 Phase B merge (b6e24e67),
frozen as the Phase-C history range at gold-freeze time.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

DEFAULT_BASE = "b02f3938"  # repository root commit (2026-07-02)
DEFAULT_HEAD = "b6e24e67"  # FDG-0 Phase B merge (Phase-C base)

CXX_EXTS = {".cpp", ".cc", ".cxx", ".c", ".h", ".hh", ".hpp", ".hxx"}

ANCHORS_PATH = REPO_ROOT / "spec" / "formal" / "anchors.json"
MANIFEST_PATH = REPO_ROOT / "spec" / "tla" / "manifest.json"

FORMAL_PREFIXES = (
    "spec/",
    "docs/verification/",
    "docs/results/",
    "docs/investigations/",
    "scripts/formal/",
    "docs/architecture/",
)
TESTS_PREFIX = "tests/"
ASYNC_PREFIXES = ("src/async/", "include/sluice/async/")
BUILD_PATHS = ("xmake.lua", "xmake/")

# Deterministic subject-signal classes. These are SAMPLING heuristics over
# the recorded subject text; the gold audit re-inspects each selected diff.
POSITIVE_SIGNAL_RE = re.compile(
    r"fix|race|wake|lost|epoch|generation|terminal|cancel|completion|"
    r"publication|stale|arbitr|winner|deadlock|liveness|orphan|stall|"
    r"poison|admission|exactly.once|dup|rearm|corrective|close.*window|"
    r"harden|atomic|serialize|reconcil|skew|orphan",
    re.IGNORECASE,
)
RENAME_SIGNAL_RE = re.compile(
    r"rename|move|split.*\bTU\b|code motion|relocat|pure code", re.IGNORECASE
)
MECHANICAL_SIGNAL_RE = re.compile(
    r"^docs\(|^style\(|^chore\(|comment.only|comment-only|formatting",
    re.IGNORECASE,
)


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True
    )
    if result.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def load_authority_paths() -> tuple[set[str], set[str]]:
    """Anchor files (current registry) + bound implementation files (current
    manifest) — used only as TOUCH flags on historical commits; the current
    authority is the Phase-C evaluation authority (task book §17)."""
    registry = json.loads(ANCHORS_PATH.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    anchors = {
        a["file"] for c in registry.get("claims", []) for a in c.get("cpp_anchors", [])
    }
    bound = {
        p
        for s in manifest.get("suites", [])
        for p in s.get("implementation_bindings", [])
    }
    return anchors, bound


def classify_files(files: list[dict], anchors: set[str], bound: set[str]) -> dict:
    paths = [f["path"] for f in files]
    return {
        "anchor_file": sorted(set(paths) & anchors),
        "bound_file": sorted(set(paths) & (bound - anchors)),
        "formal_artifact": sorted(
            p for p in paths if p.startswith(FORMAL_PREFIXES)
        ),
        "async_cxx": sorted(
            p
            for p in paths
            if p.startswith(ASYNC_PREFIXES) and Path(p).suffix.lower() in CXX_EXTS
        ),
        "build_config": sorted(p for p in paths if p in BUILD_PATHS or p.startswith("xmake/")),
        "tests_only": sorted(p for p in paths if p.startswith(TESTS_PREFIX)),
    }


def subsystems_of(touches: dict) -> list[str]:
    subs: list[str] = []
    anchor_async = touches["async_cxx"]
    joined = "\n".join(anchor_async + touches["anchor_file"] + touches["bound_file"])
    if "park_wake" in joined:
        subs.append("scheduler-park-wake")
    if re.search(r"scheduler", joined):
        subs.append("scheduler")
    if "request_arena" in joined or "request_slot" in joined:
        subs.append("request-arena")
    if "completion" in joined:
        subs.append("completion")
    if "cancel" in joined:
        subs.append("cancel")
    if "threadpool" in joined:
        subs.append("threadpool")
    if "uring" in joined:
        subs.append("uring")
    if "blocking_io_pool" in joined or "src/sluice" in joined:
        subs.append("sync-io")
    if touches["build_config"]:
        subs.append("build-config")
    if touches["formal_artifact"]:
        subs.append("formal-maintenance")
    if touches["tests_only"] and not anchor_async and not touches["anchor_file"]:
        subs.append("tests-only")
    if not subs:
        subs.append("other")
    return sorted(set(subs))


def candidate_category(row: dict) -> str:
    if row["merge"]:
        return "MERGE"
    subject = row["subject"]
    touches = row["touches"]
    if touches["formal_artifact"]:
        return "FORMAL_MAINTENANCE"
    if RENAME_SIGNAL_RE.search(subject):
        return "RENAME_MOVE"
    if MECHANICAL_SIGNAL_RE.match(subject):
        if touches["anchor_file"] or touches["bound_file"]:
            return "COMMENT_FORMAT_NEAR_AUTHORITY"
        return "DOCS_STYLE"
    if touches["build_config"] and not touches["async_cxx"]:
        return "BUILD_CONFIG"
    if POSITIVE_SIGNAL_RE.search(subject) and (
        touches["anchor_file"] or touches["bound_file"] or touches["async_cxx"]
    ):
        return "POSITIVE_FORMAL_CANDIDATE"
    if subject.startswith(("feat(", "fix(")) and touches["async_cxx"]:
        return "ASYNC_SEMANTIC_CANDIDATE"
    if touches["tests_only"] and not touches["async_cxx"]:
        return "TESTS_ONLY"
    if touches["async_cxx"] or touches["anchor_file"] or touches["bound_file"]:
        return "ASYNC_OTHER"
    return "UNRELATED"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--range", default=f"{DEFAULT_BASE}..{DEFAULT_HEAD}")
    ap.add_argument("--out", default="docs/results/formal/fdg0-phase-c-candidates.json")
    ap.add_argument("--summary-only", action="store_true")
    args = ap.parse_args(argv)

    if ".." not in args.range:
        raise SystemExit("--range must be BASE..HEAD")
    base, head = args.range.split("..", 1)
    base_sha = git("rev-parse", "--verify", base).strip()
    head_sha = git("rev-parse", "--verify", head).strip()
    anchors, bound = load_authority_paths()

    sep = "\x1f"  # unit-separator field delimiter (never in one-line subjects)
    log_format = sep.join(["%H", "%P", "%ad", "%s"])
    out = git(
        "log",
        "--no-renames",
        f"--date=iso-strict",
        f"--format={log_format}",
        f"{base_sha}..{head_sha}",
    )
    commits: list[dict] = []
    for line in out.splitlines():
        if not line.strip():
            continue
        sha, parents, date, subject = line.split(sep, 3)
        name_status = git(
            "diff-tree", "--no-commit-id", "--name-status", "-r", sha
        )
        files = []
        for entry in name_status.splitlines():
            if not entry.strip():
                continue
            status, _, path = entry.partition("\t")
            files.append({"status": status.strip(), "path": path.strip()})
        numstat = git("diff-tree", "--no-commit-id", "--numstat", "-r", sha)
        ins = dele = 0
        for entry in numstat.splitlines():
            parts = entry.split("\t")
            if len(parts) >= 3:
                ins += int(parts[0]) if parts[0].isdigit() else 0
                dele += int(parts[1]) if parts[1].isdigit() else 0
        row = {
            "sha": sha,
            "parents": parents.split(),
            "date": date,
            "subject": subject,
            "merge": len(parents.split()) > 1,
            "files": files,
            "insertions": ins,
            "deletions": dele,
        }
        row["touches"] = classify_files(files, anchors, bound)
        row["subsystems"] = subsystems_of(row["touches"])
        row["candidate_category"] = candidate_category(row)
        commits.append(row)

    by_category: dict[str, int] = {}
    for row in commits:
        by_category[row["candidate_category"]] = (
            by_category.get(row["candidate_category"], 0) + 1
        )

    payload = {
        "schema": "sluice-fdg0-phase-c-candidates/1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "range": {"base": base_sha, "head": head_sha},
        "authority": {
            "anchors": "spec/formal/anchors.json (current, schema 2)",
            "manifest": "spec/tla/manifest.json (current)",
            "note": (
                "touch flags use the CURRENT authority paths; categories are "
                "deterministic sampling heuristics over subjects/paths, not "
                "gold labels; no resolver ran during inventory (task book §8)"
            ),
        },
        "commit_count": len(commits),
        "category_counts": dict(sorted(by_category.items())),
        "commits": commits,
    }
    if not args.summary_only:
        out_path = REPO_ROOT / args.out
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(payload, indent=1), encoding="utf-8")
        print(f"==> inventory written: {out_path}")
    print(f"    range:   {base_sha[:12]}..{head_sha[:12]}")
    print(f"    commits: {len(commits)}")
    for cat, n in sorted(by_category.items()):
        print(f"    {cat:34s} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
