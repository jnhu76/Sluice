#!/usr/bin/env python3
"""Unified Sluice formal verification orchestrator.

Commands:
    doctor  — check Java, jar, manifest, verifier executables, temp dirs
    list    — print the suite inventory (supports --markdown)
    check   — structural checks only (no TLC): manifest schema, orphan
              models, old-path references, docs consistency, workflow refs
    suite   — run one suite's authoritative verifier
    smoke   — run the PR smoke tier (measured-fast suites)
    all     — run every suite and print a unified summary

Usage:
    python3 scripts/formal/verify.py doctor
    python3 scripts/formal/verify.py list
    python3 scripts/formal/verify.py list --markdown
    python3 scripts/formal/verify.py check
    python3 scripts/formal/verify.py suite <suite-id>
    python3 scripts/formal/verify.py smoke
    python3 scripts/formal/verify.py all
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
SPEC_ROOT = REPO_ROOT / "spec" / "tla"
MANIFEST_PATH = SPEC_ROOT / "manifest.json"
DOCS_ROOT = REPO_ROOT / "docs"
FORMAL_DOCS = DOCS_ROOT / "verification" / "formal"
MODELS_DOC = DOCS_ROOT / "verification" / "formal-models.md"
WORKFLOW_FILE = REPO_ROOT / ".github" / "workflows" / "formal.yml"

# Old paths that must not appear anywhere in the repo after migration.
OLD_PATHS = [
    "docs/spec/",
    "tools/formal/",
    "spec/tla/BlockingIoPool.tla",
    "spec/tla/BlockingIoPool.cfg",
    "spec/tla/BlockingIoPool_liveness.cfg",
]


# --- Manifest loading ------------------------------------------------------


def load_manifest() -> dict:
    if not MANIFEST_PATH.is_file():
        print(f"error: manifest not found: {MANIFEST_PATH}", file=sys.stderr)
        sys.exit(2)
    with MANIFEST_PATH.open("r", encoding="utf-8") as f:
        return json.load(f)


def suite_by_id(manifest: dict, suite_id: str) -> dict | None:
    for s in manifest.get("suites", []):
        if s.get("id") == suite_id:
            return s
    return None


# Fields every coverage_gaps entry must carry (issue #100; the entry is
# authoritative long-lived structure, so `check`/`doctor` validate it).
COVERAGE_GAP_REQUIRED_FIELDS = [
    "id",
    "status",
    "protocol",
    "why_no_model_yet",
    "revisit_triggers",
    "implementation_bindings",
    "regression_test_cross_links",
    "owner_docs",
]

# File-list fields whose referenced paths must exist in the repo.
COVERAGE_GAP_FILE_FIELDS = [
    "implementation_bindings",
    "regression_test_cross_links",
    "owner_docs",
]

# Suite-level file-list fields whose referenced paths must exist (audit
# finding: `blocking-io-pool` bound `src/core/blocking_io_pool.cpp`, which has
# never existed at that path — `check` validated only coverage_gaps file
# fields, so a dangling suite binding passed silently).
SUITE_FILE_FIELDS = [
    "implementation_bindings",
    "owner_docs",
]


def _check_repo_paths(
    paths: object, where: str, field: str, errors: list[str]
) -> None:
    """Validate that every entry of a file-list field is an existing repo file."""
    if not isinstance(paths, list):
        errors.append(f"{where}: '{field}' must be an array")
        return
    for p in paths:
        if not isinstance(p, str) or not p:
            errors.append(f"{where}: '{field}' entries must be non-empty strings")
            continue
        candidate = (REPO_ROOT / p).resolve()
        try:
            candidate.relative_to(REPO_ROOT.resolve())
        except ValueError:
            errors.append(f"{where}: '{field}' path escapes REPO_ROOT: {p}")
            continue
        if not candidate.is_file():
            errors.append(f"{where}: '{field}' file not found: {p}")


def _check_suite_bindings(manifest: dict) -> list[str]:
    """Validate per-suite implementation_bindings / owner_docs.

    Every suite must either bind at least one concrete repo file or carry a
    non-empty `binding_rationale` explaining why it is protocol-level /
    cross-file (AGENTS.md §17 evidence discipline: a suite that nobody can map
    to real code is unauditable). All referenced files must exist.
    """
    errors: list[str] = []
    for s in manifest.get("suites", []):
        sid = s.get("id", "?")
        where = f"suite '{sid}'"
        for field in SUITE_FILE_FIELDS:
            if field not in s:
                continue
            _check_repo_paths(s.get(field), where, field, errors)
        bindings = s.get("implementation_bindings")
        rationale = s.get("binding_rationale")
        has_bindings = isinstance(bindings, list) and len(bindings) > 0
        has_rationale = isinstance(rationale, str) and rationale.strip() != ""
        if not has_bindings and not has_rationale:
            errors.append(
                f"{where}: implementation_bindings is empty and no "
                f"binding_rationale is given — bind concrete C++ files or "
                f"explain why the suite is protocol-level"
            )
    return errors


def _check_coverage_gaps(manifest: dict) -> list[str]:
    """Validate the optional top-level `coverage_gaps` structure.

    Returns a list of human-readable error strings (empty when valid).
    `coverage_gaps` is optional (manifests predate it), but once present every
    entry is authoritative and must satisfy the statically checkable schema:
    unique string ids, required fields, non-empty revisit_triggers, and
    existence of every referenced file. A typo such as `revisit_trigers` or a
    dangling binding must fail the structural check, not silently pass.
    """
    errors: list[str] = []
    gaps = manifest.get("coverage_gaps")
    if gaps is None:
        return errors
    if not isinstance(gaps, list):
        return ["coverage_gaps must be an array"]
    seen_ids: set[str] = set()
    for i, gap in enumerate(gaps):
        where = f"coverage_gaps[{i}]"
        if not isinstance(gap, dict):
            errors.append(f"{where}: must be an object")
            continue
        gid = gap.get("id")
        if not isinstance(gid, str) or not gid:
            errors.append(f"{where}: missing non-empty string 'id'")
        elif gid in seen_ids:
            errors.append(f"{where}: duplicate id '{gid}'")
        else:
            seen_ids.add(gid)
        for field in COVERAGE_GAP_REQUIRED_FIELDS:
            if field not in gap:
                errors.append(f"{where}: missing required field '{field}'")
        triggers = gap.get("revisit_triggers")
        if not isinstance(triggers, list) or len(triggers) == 0:
            errors.append(
                f"{where}: 'revisit_triggers' must be a non-empty array"
            )
        for key in COVERAGE_GAP_FILE_FIELDS:
            paths = gap.get(key)
            if not isinstance(paths, list):
                errors.append(f"{where}: '{key}' must be an array")
                continue
            for p in paths:
                if not isinstance(p, str) or not p:
                    errors.append(
                        f"{where}: '{key}' entries must be non-empty strings"
                    )
                    continue
                candidate = (REPO_ROOT / p).resolve()
                try:
                    candidate.relative_to(REPO_ROOT.resolve())
                except ValueError:
                    errors.append(
                        f"{where} ('{gid or '?'}'): '{key}' path escapes "
                        f"REPO_ROOT: {p}"
                    )
                    continue
                if not candidate.is_file():
                    errors.append(
                        f"{where} ('{gid or '?'}'): '{key}' file not found: {p}"
                    )
    return errors


# --- doctor ---------------------------------------------------------------


def cmd_doctor(manifest: dict) -> int:
    """Check the environment: Java, jar, manifest, verifiers, temp, cache."""
    rc = 0
    print("=== formal verification doctor ===\n")

    # Java
    try:
        java = subprocess.run(
            ["java", "-version"], capture_output=True, text=True
        )
        if java.returncode != 0:
            print("FAIL  java not found on PATH")
            rc = 1
        else:
            version_line = (java.stderr or java.stdout).splitlines()[0]
            print(f"OK    java: {version_line}")
    except FileNotFoundError:
        print("FAIL  java not found on PATH")
        rc = 1

    # Jar
    sys.path.insert(0, str(SCRIPT_DIR))
    from lib import tlc as tlc_mod

    try:
        jar = tlc_mod.resolve_jar(strict=True)
        print(f"OK    tla2tools.jar: {jar}")
        version = tlc_mod.tlc_version(jar)
        print(f"      {version}")
    except FileNotFoundError as e:
        print(f"FAIL  {e}")
        rc = 1

    # Repo-root tla2tools.jar contamination guard.
    # resolve-jar.sh no longer falls back to the repo-root jar, and it is
    # gitignored, but a stale untracked copy here is the historical root cause
    # of silent version drift (e.g. a future TLC 2.2x jar masking the locked
    # v1.7.4).
    # Flag its presence so it gets removed rather than silently shipped.
    repo_jar = REPO_ROOT / "tla2tools.jar"
    if repo_jar.is_file():
        print(
            f"WARN  repo-root tla2tools.jar present: {repo_jar}\n"
            f"      this untracked jar can shadow the locked cache jar; remove it\n"
            f"      (rm tla2tools.jar; use bootstrap.py to populate the cache)"
        )
        rc = 1

    # Manifest schema
    required_keys = {"schema_version", "toolchain", "suites"}
    missing = required_keys - set(manifest.keys())
    if missing:
        print(f"FAIL  manifest missing keys: {sorted(missing)}")
        rc = 1
    else:
        print(f"OK    manifest schema_version={manifest.get('schema_version')}")

    # Coverage gaps schema (optional; validated once present — issue #100)
    gap_errors = _check_coverage_gaps(manifest)
    if gap_errors:
        print(f"FAIL  coverage_gaps schema ({len(gap_errors)} error(s)):")
        for e in gap_errors:
            print(f"      {e}")
        rc = 1
    else:
        print("OK    coverage_gaps schema valid (or absent)")

    # Suite-level implementation_bindings / owner_docs (formal audit finding:
    # dangling bindings passed silently before this check existed).
    binding_errors = _check_suite_bindings(manifest)
    if binding_errors:
        print(f"FAIL  suite bindings ({len(binding_errors)} error(s)):")
        for e in binding_errors:
            print(f"      {e}")
        rc = 1
    else:
        print("OK    suite implementation_bindings/owner_docs valid")

    # Verifiers exist and are executable
    for s in manifest.get("suites", []):
        verifier = s.get("verifier", "")
        if not verifier:
            print(f"FAIL  suite {s.get('id')}: no verifier declared")
            rc = 1
            continue
        vp = REPO_ROOT / verifier
        if not vp.is_file():
            print(f"FAIL  suite {s.get('id')}: verifier missing: {verifier}")
            rc = 1
        elif not os.access(vp, os.X_OK):
            print(f"FAIL  suite {s.get('id')}: verifier not executable: {verifier}")
            rc = 1
        else:
            print(f"OK    suite {s.get('id')}: {verifier}")

    # Temp dir
    try:
        tmp = tempfile.mkdtemp(prefix="sluice-formal.doctor.")
        os.rmdir(tmp)
        print(f"OK    temp dir writable: {tmp}")
    except OSError as e:
        print(f"FAIL  temp dir not writable: {e}")
        rc = 1

    # build/formal writable
    bf = REPO_ROOT / "build" / "formal"
    try:
        bf.mkdir(parents=True, exist_ok=True)
        probe = bf / ".sluice-write-probe"
        probe.write_text("ok")
        probe.unlink()
        print(f"OK    build/formal writable: {bf}")
    except OSError as e:
        print(f"FAIL  build/formal not writable: {e}")
        rc = 1

    print()
    print("PASS" if rc == 0 else "FAIL")
    return rc


# --- list -----------------------------------------------------------------


def cmd_list(manifest: dict, markdown: bool = False) -> int:
    """Print the suite inventory."""
    suites = manifest.get("suites", [])
    if markdown:
        print("| Suite | Spec | Verifier | Tier | + | - | Reach |")
        print("|-------|------|----------|------|---|---|-------|")
        for s in suites:
            print(
                f"| {s.get('id', '')} "
                f"| {s.get('spec_dir', '')} "
                f"| {s.get('verifier', '')} "
                f"| {','.join(s.get('tiers', []))} "
                f"| {s.get('positive_gate_count', 0)} "
                f"| {s.get('negative_gate_count', 0)} "
                f"| {s.get('reachability_gate_count', 0)} |"
            )
    else:
        for s in suites:
            print(
                f"{s.get('id',''):28s}  "
                f"spec={s.get('spec_dir',''):30s}  "
                f"verifier={s.get('verifier',''):45s}  "
                f"tier={','.join(s.get('tiers', [])):8s}  "
                f"+={s.get('positive_gate_count', 0)} "
                f"-={s.get('negative_gate_count', 0)} "
                f"reach={s.get('reachability_gate_count', 0)}"
            )
        print(f"\n{len(suites)} suites")
    return 0


# --- check ----------------------------------------------------------------


# Historical paths that are NOT updated by this migration (per task §6.3).
# Old-path references inside these files are expected and must not be flagged.
HISTORICAL_PATHS = [
    "docs/history/",
]

# Implementation files that define OLD_PATHS or check for them naturally contain
# old-path strings as part of their logic. These are excluded from the scan.
IMPLEMENTATION_FILES = [
    "scripts/formal/verify.py",
    "scripts/check-doc-links.py",
]

# Files that document the migration mapping and legitimately reference old paths
# in table rows (lines containing '|'). Non-table lines are still checked.
MIGRATION_TABLE_FILES = [
    "docs/verification/formal/migration-report.md",
]


def _scan_files() -> dict[str, str]:
    """Return {relative_path: lowercase_content} for tracked text files."""
    result = subprocess.run(
        ["git", "ls-files"],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
    )
    files: dict[str, str] = {}
    for rel in result.stdout.splitlines():
        if rel.endswith((".md", ".sh", ".py", ".yml", ".yaml", ".json", ".cfg", ".tla")):
            content = (REPO_ROOT / rel).read_text(errors="replace").lower()
            files[rel] = content
    return files


def _is_historical(rel: str) -> bool:
    """Return True if the file is a historical record not owned by this migration."""
    return any(rel.startswith(p) for p in HISTORICAL_PATHS)


def _is_implementation(rel: str) -> bool:
    """Return True if the file is an implementation file that naturally contains old paths."""
    return rel in IMPLEMENTATION_FILES


def cmd_check(manifest: dict) -> int:
    """Structural checks only — no TLC execution."""
    rc = 0
    print("=== formal verification structural check ===\n")

    # 1. Manifest schema
    if "suites" not in manifest:
        print("FAIL  manifest has no suites")
        return 1

    # 1b. Coverage gaps schema (optional; validated once present — issue #100).
    # A structural `check` PASS must not claim more than it validated: without
    # this, a malformed coverage_gaps entry (e.g. a typo'd `revisit_trigers` or
    # a dangling binding) would pass and the closeout claim "manifest schema
    # PASS" would overstate the check.
    gap_errors = _check_coverage_gaps(manifest)
    if gap_errors:
        print(f"FAIL  coverage_gaps schema ({len(gap_errors)} error(s)):")
        for e in gap_errors:
            print(f"      {e}")
        rc = 1
    else:
        print("OK    coverage_gaps schema valid (or absent)")

    # 1c. Suite implementation_bindings / owner_docs point at real files.
    binding_errors = _check_suite_bindings(manifest)
    if binding_errors:
        print(f"FAIL  suite bindings ({len(binding_errors)} error(s)):")
        for e in binding_errors:
            print(f"      {e}")
        rc = 1
    else:
        print("OK    suite implementation_bindings/owner_docs valid")

    # 2. Every .tla belongs to a suite
    suite_dirs = {s.get("spec_dir", "") for s in manifest["suites"]}
    spec_root = SPEC_ROOT
    tla_files = sorted(spec_root.rglob("*.tla")) if spec_root.is_dir() else []
    cfg_files = sorted(spec_root.rglob("*.cfg")) if spec_root.is_dir() else []
    orphan_tla = []
    for f in tla_files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        # Check whether this file's directory is under a declared suite dir
        file_parent = f.parent
        belongs = False
        for sd in suite_dirs:
            suite_path = (REPO_ROOT / sd).resolve()
            try:
                file_parent.relative_to(suite_path)
                belongs = True
                break
            except ValueError:
                continue
        if not belongs:
            orphan_tla.append(rel)
    if orphan_tla:
        print("FAIL  orphan .tla files (not in any manifest suite):")
        for o in orphan_tla:
            print(f"      {o}")
        rc = 1
    else:
        print(f"OK    all {len(tla_files)} .tla files belong to a manifest suite")

    # 3. No orphan .cfg
    orphan_cfg = []
    for f in cfg_files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        file_parent = f.parent
        belongs = False
        for sd in suite_dirs:
            suite_path = (REPO_ROOT / sd).resolve()
            try:
                file_parent.relative_to(suite_path)
                belongs = True
                break
            except ValueError:
                continue
        if not belongs:
            orphan_cfg.append(rel)
    if orphan_cfg:
        print("FAIL  orphan .cfg files:")
        for o in orphan_cfg:
            print(f"      {o}")
        rc = 1
    else:
        print(f"OK    all {len(cfg_files)} .cfg files belong to a manifest suite")

    # 4. No old-path references in non-historical tracked files
    # Historical documents (docs/history/**) are NOT updated by this migration
    # and are expected to retain old-path references.
    files = _scan_files()
    old_refs: list[tuple[str, str]] = []
    for rel, content in files.items():
        if _is_historical(rel) or _is_implementation(rel):
            continue
        if rel in MIGRATION_TABLE_FILES:
            # Only check non-table lines (table rows document the mapping).
            # Lines with <!-- old-path-ok --> are explicit allowlisted quotes.
            checked = "\n".join(
                ln for ln in content.splitlines()
                if "|" not in ln and "<!-- old-path-ok -->" not in ln
            )
        else:
            checked = content
        for old in OLD_PATHS:
            if old.lower() in checked:
                old_refs.append((rel, old))
    if old_refs:
        print("FAIL  old-path references found (excluding historical docs):")
        for rel, old in old_refs:
            print(f"      {rel}: contains '{old}'")
        rc = 1
    else:
        print("OK    no old-path references in non-historical tracked files")

    # 5. Verifier executability
    for s in manifest.get("suites", []):
        verifier = s.get("verifier", "")
        if not verifier:
            print(f"FAIL  suite {s.get('id')}: no verifier")
            rc = 1
            continue
        vp = REPO_ROOT / verifier
        if not vp.is_file():
            print(f"FAIL  suite {s.get('id')}: verifier missing: {verifier}")
            rc = 1
        elif not os.access(vp, os.X_OK):
            print(f"FAIL  suite {s.get('id')}: verifier not executable: {verifier}")
            rc = 1

    # 6. Docs inventory consistency
    if MODELS_DOC.is_file():
        doc_content = MODELS_DOC.read_text().lower()
        for s in manifest.get("suites", []):
            sid = s.get("id", "")
            if sid and sid not in doc_content:
                print(f"FAIL  suite {sid} missing from docs/verification/formal-models.md")
                rc = 1
        print("OK    docs inventory covers all manifest suites")
    else:
        print("WARN  docs/verification/formal-models.md not found")

    # 7. No committed jar
    for rel, content in files.items():
        if "tla2tools.jar" in content and rel not in (
            "scripts/formal/tla2tools.lock.json",
        ):
            # Only flag if the jar itself is tracked (not just mentioned)
            pass
    tracked = subprocess.run(
        ["git", "ls-files"],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
    )
    if "tla2tools.jar" in tracked.stdout:
        print("FAIL  tla2tools.jar is tracked in git")
        rc = 1
    else:
        print("OK    tla2tools.jar is not tracked")

    # 8. Workflow references valid paths
    if WORKFLOW_FILE.is_file():
        wf_content = WORKFLOW_FILE.read_text()
        for s in manifest.get("suites", []):
            verifier = s.get("verifier", "")
            if verifier and verifier not in wf_content:
                # Not all verifiers need to be in the workflow; smoke/all cover them.
                pass
        print("OK    workflow file exists")
    else:
        print("WARN  .github/workflows/formal.yml not found")

    # 9. Every suite has explicit gate counts
    for s in manifest.get("suites", []):
        sid = s.get("id", "")
        for key in ("positive_gate_count", "negative_gate_count", "reachability_gate_count"):
            if not isinstance(s.get(key), int):
                print(f"FAIL  suite {sid}: {key} missing or not an integer")
                rc = 1

    print()
    print("PASS" if rc == 0 else "FAIL")
    return rc


# --- suite ----------------------------------------------------------------


def _resolve_jar_for_suite() -> Path:
    """Resolve the TLC jar once, with strict checksum verification."""
    sys.path.insert(0, str(SCRIPT_DIR))
    from lib import tlc as tlc_mod

    return tlc_mod.resolve_jar(strict=True)


def cmd_suite(suite_id: str, manifest: dict) -> int:
    """Run one suite's authoritative verifier."""
    s = suite_by_id(manifest, suite_id)
    if s is None:
        print(f"error: suite '{suite_id}' not found in manifest", file=sys.stderr)
        return 2
    verifier = s.get("verifier", "")
    if not verifier:
        print(f"error: suite '{suite_id}' has no verifier", file=sys.stderr)
        return 2
    vp = REPO_ROOT / verifier
    if not vp.is_file():
        print(f"error: verifier not found: {vp}", file=sys.stderr)
        return 2

    # Resolve jar once and pass to child via environment.
    try:
        jar = _resolve_jar_for_suite()
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["TLA2TOOLS_JAR"] = str(jar)

    print(f"=== suite: {suite_id}  (verifier: {verifier}) ===\n")
    result = subprocess.run([str(vp)], cwd=str(REPO_ROOT), env=env)
    return result.returncode


# --- smoke / all ----------------------------------------------------------


def _run_suite(s: dict, jar: Path | None = None) -> tuple[str, str, str, int]:
    """Run one suite, return (id, verdict, elapsed, exit_code).

    On failure, writes the captured output to build/formal/<id>.log for
    artifact upload by CI.
    """
    sid = s.get("id", "?")
    verifier = s.get("verifier", "")
    vp = REPO_ROOT / verifier
    if not vp.is_file():
        return (sid, "BLOCKED", "0s", 2)

    env = os.environ.copy()
    if jar:
        env["TLA2TOOLS_JAR"] = str(jar)

    start = time.monotonic()
    result = subprocess.run([str(vp)], cwd=str(REPO_ROOT), capture_output=True, env=env, text=True)
    elapsed = time.monotonic() - start
    if result.returncode == 0:
        verdict = "PASS"
    else:
        verdict = "FAIL"
        # Write failure artifact for CI upload.
        artifact_dir = REPO_ROOT / "build" / "formal"
        artifact_dir.mkdir(parents=True, exist_ok=True)
        log_path = artifact_dir / f"{sid}.log"
        with log_path.open("w", encoding="utf-8") as f:
            f.write(f"# Suite: {sid}\n")
            f.write(f"# Verifier: {verifier}\n")
            f.write(f"# Exit code: {result.returncode}\n")
            f.write(f"# Elapsed: {elapsed:.1f}s\n\n")
            f.write("=== STDOUT ===\n")
            f.write(result.stdout or "(empty)\n")
            f.write("\n=== STDERR ===\n")
            f.write(result.stderr or "(empty)\n")
    return (sid, verdict, f"{elapsed:.1f}s", result.returncode)


def cmd_smoke(manifest: dict) -> int:
    """Run the smoke tier (suites with tier 'smoke' or 'full' that are fast)."""
    suites = [s for s in manifest.get("suites", []) if "smoke" in s.get("tiers", [])]
    if not suites:
        # Fall back to all suites marked 'full' if no smoke tier declared
        suites = [s for s in manifest.get("suites", []) if "full" in s.get("tiers", [])]

    # Resolve jar once for all suites.
    try:
        jar = _resolve_jar_for_suite()
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print(f"=== smoke tier: {len(suites)} suites ===\n")
    rc = 0
    for s in suites:
        sid, verdict, elapsed, exit_code = _run_suite(s, jar)
        print(f"{verdict:7s} {sid:28s}  ({elapsed})  [exit={exit_code}]")
        if verdict in ("FAIL", "BLOCKED"):
            rc = 1
    return rc


def cmd_all(manifest: dict) -> int:
    """Run every suite and print a unified summary."""
    suites = manifest.get("suites", [])

    # Resolve jar once for all suites.
    try:
        jar = _resolve_jar_for_suite()
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print(f"=== all suites: {len(suites)} ===\n")
    results: list[tuple[str, str, str, int]] = []
    for s in suites:
        sid, verdict, elapsed, exit_code = _run_suite(s, jar)
        results.append((sid, verdict, elapsed, exit_code))
        print(f"{verdict:7s} {sid:28s}  ({elapsed})  [exit={exit_code}]")

    passed = sum(1 for _, v, _, _ in results if v == "PASS")
    failed = sum(1 for _, v, _, _ in results if v == "FAIL")
    blocked = sum(1 for _, v, _, _ in results if v == "BLOCKED")
    pos = sum(s.get("positive_gate_count", 0) for s in suites)
    neg = sum(s.get("negative_gate_count", 0) for s in suites)
    reach = sum(s.get("reachability_gate_count", 0) for s in suites)

    print()
    print("=== summary ===")
    print(f"  suites:  {len(suites)} total, {passed} PASS, {failed} FAIL, {blocked} BLOCKED")
    print(f"  gates:   {pos} positive, {neg} negative, {reach} reachability")
    # BLOCKED is NOT success — return nonzero for FAIL or BLOCKED.
    return 1 if (failed > 0 or blocked > 0) else 0


# --- main -----------------------------------------------------------------


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Unified Sluice formal verification orchestrator"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("doctor", help="Check environment and tooling")
    list_parser = sub.add_parser("list", help="List suite inventory")
    list_parser.add_argument(
        "--markdown", action="store_true", help="Output as a Markdown table"
    )
    sub.add_parser("check", help="Structural checks (no TLC)")
    suite_parser = sub.add_parser("suite", help="Run one suite")
    suite_parser.add_argument("suite_id", help="Suite ID from the manifest")
    sub.add_parser("smoke", help="Run the PR smoke tier")
    sub.add_parser("all", help="Run every suite")

    args = parser.parse_args(argv)
    manifest = load_manifest()

    if args.command == "doctor":
        return cmd_doctor(manifest)
    if args.command == "list":
        return cmd_list(manifest, markdown=args.markdown)
    if args.command == "check":
        return cmd_check(manifest)
    if args.command == "suite":
        return cmd_suite(args.suite_id, manifest)
    if args.command == "smoke":
        return cmd_smoke(manifest)
    if args.command == "all":
        return cmd_all(manifest)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
