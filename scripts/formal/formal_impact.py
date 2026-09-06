#!/usr/bin/env python3
"""FTLR-0 / SCIP-PILOT formal impact resolver (issue #299) + FDG-0 Phase A
Build Truth (issue #298).

Answers ONE bounded question:

    a C++ diff possibly affects which formal claims / TLA+ suites,
    so which formal review is required?

Chain (issue #299 §1):

    git diff -> changed C++ symbol -> SCIP structural graph
             -> nearest registered formal anchor -> formal claim
             -> TLA+ suite / trace / bridge evidence

FDG-0 Phase B (#298) adds an OPTIONAL facet routing layer over that chain:
a trusted explicit anchor reach may narrow the reopened formal targets to
the affected facets' suites (PRECISE); every coarse/unknown/stale/unresolved
state stays CONSERVATIVE_ALL on the full parent claim suite set. Facets are
internal subdivisions of existing claims (never new claims); facet mapping
is EXPLICIT registry authority (spec/formal/anchors.json schema 2).

FDG-0 Phase A (#298) replaces the pilot's `file.startswith("src/")` source
selection with Xmake Build Truth: the SCIP graph is built from the selected
production world (sluice_async + its dependency closure) joined to exact
compile_commands entries via Xmake target ownership. The Build Manifest
(build/formal-impact/build-manifest.json) records the build identity; a
changed or unverifiable build world fails closed (UNKNOWN_BUILD_WORLD /
BUILD_WORLD_CHANGED / BUILD_GRAPH_STALE), never silently NO.

Hard rules (#299 §10, §24 + #298 corrective-1):

    * NO_FORMAL_IMPACT requires: a VERIFIED build world (checked BEFORE the
      empty-diff short-circuit, C2), no anchor hit, no SCIP path hit, no
      implementation_bindings hit, and no unresolved-risk condition.
      "SCIP could not see it" is UNKNOWN, never NO.
    * Build identity is part of implementation identity: unknown/stale
      build world -> UNKNOWN formal impact -> fail closed — regardless of
      whether any C++ file changed (C3: config-only or membership-only
      drift fails closed too).
    * A graph without a build identity block (schema /1 / synthetic) is
      UNKNOWN_BUILD_WORLD and fails closed by default (C1); the explicit
      `--allow-legacy-graph-for-eval` opt-in (check/impact) is for
      evaluation fixtures only and never bypasses a FAILED verification.
    * The compdb join is per-TU exact: a TU never borrows a sibling or
      dependency target's compile command (C4), and the owning target comes
      only from the `-o` object path (C6). Unrelated compdb cardinality
      does not enter the build identity (C5).
    * Impact findings never claim a semantic disposition: the resolver
      always reports `semantic disposition: UNDETERMINED`. Whether a TLA+
      model actually needs updating is decided by later bounded semantic
      review (human; the pilot's advisory LLM experiment at most suggests).
    * This is a method-selection experiment. It is NOT wired into pre-push
      or CI, and it does not modify formal claims.

Subcommands:
    build-world  generate the Build Manifest from the current Xmake config
    index        refresh compile_commands, generate the Build Manifest, build
                 the SCIP index + symbol graph from the selected world
    check        validate the anchor registry + anchor resolution (S1-S3)
    impact       classify a diff's formal impact (DIRECT/STRUCTURAL/COARSE/UNKNOWN/NO)
    explain      show one claim's anchors, suites, evidence, resolution
    adjudicate   assemble the reduced-context LLM adjudication prompt (experiment)

Exit contract (corrective-1 + FDG-0 Phase A): the default CLI fails closed.

    NO/DIRECT/STRUCTURAL/COARSE      -> exit 0
    UNKNOWN_FORMAL_IMPACT            -> exit 1
    stale graph                      -> exit 1 (aggregate UNKNOWN + candidates)
    UNRESOLVED non-gated anchor      -> exit 1
    frontier/traversal UNKNOWN       -> exit 1
    missing graph for a C++ diff     -> exit 1
    build world changed / unknown    -> exit 1 (aggregate UNKNOWN + candidates;
                                        applies to an EMPTY diff too, C2)
    legacy graph, no build identity  -> exit 1 unless --allow-legacy-graph-for-eval

Tooling/environment hard errors (missing xmake metadata, malformed target
metadata, missing/ambiguous compile command at build time) -> exit 2.

`--allow-unknown-for-eval` is the explicit experiment opt-in that lets the
evaluation harness observe UNKNOWN results with exit 0; it never changes the
reported classification. `check` requires a fresh graph and full anchor
resolution by default; `check --structure-only` explicitly scopes the run
to registry validation.

Usage:
    python3 scripts/formal/formal_impact.py build-world
    python3 scripts/formal/formal_impact.py index
    python3 scripts/formal/formal_impact.py check
    python3 scripts/formal/formal_impact.py impact --range master..HEAD
    python3 scripts/formal/formal_impact.py explain F08
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
ANCHORS_PATH = REPO_ROOT / "spec" / "formal" / "anchors.json"
MANIFEST_PATH = REPO_ROOT / "spec" / "tla" / "manifest.json"

# FDG-0 Phase C analysis-root seam (issue #298 Phase C; task book §5/§16):
# the AUTHORITY artifacts (anchor registry, formal manifest, and registry
# path validation) always resolve against REPO_ROOT — the repository this
# tooling lives in. ANALYSIS_ROOT is the repository/worktree whose git
# history, Xmake build world, and SCIP artifacts are analyzed. Unset (the
# normal case) it equals REPO_ROOT and every default path is unchanged.
ANALYSIS_ROOT = (
    Path(os.environ["SLUICE_FDGC_ANALYSIS_ROOT"]).resolve()
    if os.environ.get("SLUICE_FDGC_ANALYSIS_ROOT")
    else REPO_ROOT
)
BUILD_DIR = ANALYSIS_ROOT / "build" / "formal-impact"
GRAPH_PATH = BUILD_DIR / "graph.json"
SCIP_PATH = BUILD_DIR / "index.scip"
COMPDB_PATH = ANALYSIS_ROOT / "compile_commands.json"
COMPDB_SRC_PATH = BUILD_DIR / "compile_commands.src.json"
SCIP_CLANG_BIN = BUILD_DIR / "bin" / "scip-clang"
SCIP_CLANG_LOCK = SCRIPT_DIR / "scip-clang.lock.json"

# FDG-0 Phase A: the selected production root target (issue #298 §4). The
# formalized build world is this target plus its Xmake dependency closure.
DEFAULT_BUILD_ROOT = "sluice_async"

GRAPH_SCHEMA = "sluice-formal-impact-graph/2"
# Schema /1 (pre-Build-Truth graphs) still loads; graphs without a build
# identity block cannot be world-verified and are handled explicitly.
GRAPH_SCHEMAS = {"sluice-formal-impact-graph/1", GRAPH_SCHEMA}
# FDG-0 Phase B: registry schema 2 adds optional per-anchor ids and optional
# per-claim facets (internal subdivisions of an existing claim, never new
# claims). Schema 1 registries stay loadable; declaring facets under schema 1
# is invalid.
REGISTRY_SCHEMA = 2
REGISTRY_SCHEMAS = {1, 2}

# Fallback traversal depth. The depth experiment (issue #299 §11) compares
# 0..3; the measured choice and its recall/explosion data live in
# docs/results/formal/ftlr0-scip-pilot.json.
DEFAULT_MAX_DEPTH = 2

# BFS frontier cap: reaching it means candidate explosion; the query then
# carries an explicit UNKNOWN risk instead of silently truncating.
FRONTIER_CAP = 4096

CXX_EXTS = {".cpp", ".cc", ".cxx", ".c", ".h", ".hh", ".hpp", ".hxx"}

# --- provenance vocabulary (corrective-1 C5) ---------------------------------
#
# Every edge in the derived graph is NOT equally "SCIP precise". scip-clang
# 0.4.0 emits no enclosing ranges, so each reference occurrence is attributed
# to the nearest-preceding named definition — a heuristic, not a
# compiler-proven enclosure. Provenance travels with the results so no
# consumer can mistake a heuristic path for compiler-verified structure.

P_EXPLICIT = "EXPLICIT"
P_COMPILER = "COMPILER"
P_HEURISTIC = "HEURISTIC"
P_FALLBACK = "FALLBACK"
P_BUILD = "BUILD"

PROVENANCE_LEGEND = {
    P_EXPLICIT: "registry-declared binding (spec/formal/anchors.json: anchor <-> claim)",
    P_COMPILER: "compiler-derived fact (SCIP symbol identity / definition positions)",
    P_HEURISTIC: (
        "nearest-preceding-named-definition attribution (scip-clang 0.4.0 emits "
        "no enclosing ranges; NOT a compiler-proven enclosure)"
    ),
    P_FALLBACK: "file-level coarse mapping (manifest implementation_bindings / anchor files)",
    P_BUILD: "Xmake-derived target/source/config/build fact (Build Manifest / compile_commands.json)",
}


class ImpactError(Exception):
    pass


# --- registry ---------------------------------------------------------------


def load_registry(path: Path = ANCHORS_PATH) -> dict:
    if not path.is_file():
        raise ImpactError(f"anchor registry not found: {path}")
    try:
        registry = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ImpactError(f"malformed anchor registry {path}: {exc}") from exc
    return registry


def load_manifest(path: Path = MANIFEST_PATH) -> dict:
    if not path.is_file():
        raise ImpactError(f"formal manifest not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ImpactError(f"malformed formal manifest {path}: {exc}") from exc


def validate_registry(registry: dict, manifest: dict, root: Path = REPO_ROOT) -> list[str]:
    """Structural registry validation. Returns a list of problems (empty =
    valid). Deliberately deterministic: duplicate ids, unknown suites,
    missing paths, unknown vocab, missing fields — plus the FDG-0 Phase B
    facet invariants (§18 of the phase-B task book): anchor/facet id
    uniqueness, anchor_ref existence, facet-suite ⊆ parent, facet-suite
    union == parent, zero-anchor/zero-target facets, evidence paths, and
    the declared trace-event vocabulary where an existing authority owns
    it."""
    problems: list[str] = []
    schema_version = registry.get("schema_version")
    if schema_version not in REGISTRY_SCHEMAS:
        problems.append(f"schema_version must be one of {sorted(REGISTRY_SCHEMAS)}")
    suite_ids = {s.get("id") for s in manifest.get("suites", [])}
    claim_class_vocab = set(registry.get("claim_class_vocabulary", []))
    roles_vocab = set(registry.get("anchor_roles", []))
    seen_ids: set[str] = set()

    for claim in registry.get("claims", []):
        cid = claim.get("id")
        if not cid:
            problems.append("claim without id")
            continue
        if cid in seen_ids:
            problems.append(f"duplicate claim id: {cid}")  # S1
            continue
        seen_ids.add(cid)
        for field in ("title", "claim_class", "cpp_anchors", "formal_suites"):
            if field not in claim:
                problems.append(f"claim {cid}: missing field {field}")
        if claim.get("claim_class") not in claim_class_vocab:
            problems.append(f"claim {cid}: unknown claim_class {claim.get('claim_class')!r}")
        for suite_id in claim.get("formal_suites", []):
            if suite_id not in suite_ids:
                problems.append(f"claim {cid}: formal suite not in manifest: {suite_id}")  # S2
        for anchor in claim.get("cpp_anchors", []):
            if "file" not in anchor or "symbol" not in anchor:
                problems.append(f"claim {cid}: anchor missing file/symbol")
                continue
            if not (root / anchor["file"]).is_file():
                problems.append(f"claim {cid}: anchor file missing: {anchor['file']}")
            if "role" in anchor and anchor["role"] not in roles_vocab:
                problems.append(f"claim {cid}: unknown anchor role {anchor['role']!r}")
        _validate_claim_facets(claim, cid, suite_ids, root, problems, schema_version)
        for evidence in claim.get("evidence", []):
            epath = evidence.get("path")
            if not epath:
                problems.append(f"claim {cid}: evidence entry without path")
            elif not (root / epath).exists():
                problems.append(f"claim {cid}: evidence path missing: {epath}")
    return problems


def _validate_trace_vocab(cid: str, fid: str, events: list) -> list[str]:
    """Validate declared trace events against the EXISTING vocabulary
    authority (e9_trace_validate KNOWN_EVENTS) — mechanically checkable
    because that validator already owns the vocabulary. Model-action names
    are NOT mechanically checked: no TLA parsing exists to make them
    compiler-authoritative, so they stay declared EXPLICIT metadata.

    Fail-closed (Phase-B corrective-1 C5): a facet that declares
    trace_events DEPENDS on the vocabulary authority — if that authority
    cannot be loaded or owns no KNOWN_EVENTS, validation must produce a
    problem rather than silently accepting arbitrary events. A facet with
    no declared trace_events does not depend on the authority at all."""
    if not events:
        return []
    try:
        import e9_trace_validate
    except Exception:  # noqa: BLE001 — declared vocabulary authority unavailable
        return [
            f"claim {cid}: facet {fid}: trace_events declared but the "
            f"vocabulary authority (e9_trace_validate) is unavailable"
        ]
    known = getattr(e9_trace_validate, "KNOWN_EVENTS", None)
    if not known:
        return [
            f"claim {cid}: facet {fid}: trace_events declared but the "
            f"authority owns no KNOWN_EVENTS vocabulary"
        ]
    return [
        f"claim {cid}: facet {fid}: unknown trace event {e!r} "
        f"(not in the declared authority vocabulary)"
        for e in events
        if e not in known
    ]


def _validate_claim_facets(
    claim: dict, cid: str, suite_ids: set, root: Path, problems: list[str], schema_version
) -> None:
    facets = claim.get("facets")
    if not facets:
        return
    if schema_version != 2:
        problems.append(f"claim {cid}: facets require registry schema_version 2")
        return
    anchor_ids: set[str] = set()
    for anchor in claim.get("cpp_anchors", []):
        aid = anchor.get("id")
        if not aid:
            problems.append(
                f"claim {cid}: faceted claim has anchor without id: {anchor.get('symbol')!r}"
            )
            continue
        if aid in anchor_ids:
            problems.append(f"claim {cid}: duplicate anchor id: {aid}")
        anchor_ids.add(aid)
    parent_suites = set(claim.get("formal_suites", []))
    facet_ids: set[str] = set()
    covered: set[str] = set()
    suite_union: set[str] = set()
    for facet in facets:
        fid = facet.get("id")
        if not fid:
            problems.append(f"claim {cid}: facet without id")
            continue
        if fid in facet_ids:
            problems.append(f"claim {cid}: duplicate facet id: {fid}")
        facet_ids.add(fid)
        refs = facet.get("anchor_refs", [])
        if not refs:
            problems.append(f"claim {cid}: facet {fid}: zero anchors")
        for ref in refs:
            if ref not in anchor_ids:
                problems.append(f"claim {cid}: facet {fid}: unknown anchor_ref: {ref!r}")
            else:
                covered.add(ref)
        fsuites = facet.get("formal_suites", [])
        if not fsuites:
            problems.append(f"claim {cid}: facet {fid}: zero formal targets")
        for s in fsuites:
            if s not in suite_ids:
                problems.append(f"claim {cid}: facet {fid}: formal suite not in manifest: {s}")
            elif s not in parent_suites:
                problems.append(f"claim {cid}: facet {fid}: suite not in parent claim: {s}")
            suite_union.add(s)
        labels = facet.get("semantic_labels") or {}
        problems.extend(_validate_trace_vocab(cid, fid, labels.get("trace_events") or []))
        for ev_path in facet.get("evidence", []) or []:
            if not (root / ev_path).exists():
                problems.append(f"claim {cid}: facet {fid}: evidence path missing: {ev_path}")
    missing = sorted(parent_suites - suite_union)
    if missing:
        problems.append(
            f"claim {cid}: parent formal suite(s) omitted from every facet: {missing}"
        )
    unref = sorted(anchor_ids - covered)
    if unref:
        problems.append(f"claim {cid}: faceted anchor(s) referenced by no facet: {unref}")


# --- graph ------------------------------------------------------------------


class Graph:
    """Query wrapper over the derived graph.json."""

    def __init__(self, data: dict):
        self.head_sha = data["head_sha"]
        self.generated_at = data["generated_at"]
        self.toolchain = data.get("toolchain", {})
        # Provenance of the derived edges (corrective-1 C5). Older graphs
        # without the block default to the documented scip-clang 0.4.0 shape.
        self.provenance = data.get("provenance", {})
        # FDG-0 Phase A: the Build Manifest identity this graph was built
        # from. Absent on schema /1 and synthetic graphs: world verification
        # is then impossible and is skipped explicitly (never silently).
        self.build = data.get("build") or {}
        self.nodes: dict[str, dict] = data["nodes"]
        self.reverse: dict[str, list[str]] = data["reverse"]
        self.documents: dict[str, list[str]] = data["documents"]
        self.def_positions: dict[str, list] = data.get("def_positions", {})
        self.stats = data.get("stats", {})

    @property
    def edge_provenance(self) -> str:
        """Provenance of reference-derived edges (see PROVENANCE_LEGEND)."""
        return self.provenance.get("reference_edges", P_HEURISTIC)

    @classmethod
    def load(cls, path: Path = GRAPH_PATH) -> "Graph":
        if not path.is_file():
            raise ImpactError(
                f"formal-impact graph not found: {path} (run: "
                f"python3 scripts/formal/formal_impact.py index)"
            )
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise ImpactError(f"malformed graph {path}: {exc}") from exc  # S9
        if data.get("schema") not in GRAPH_SCHEMAS:
            raise ImpactError(f"unsupported graph schema: {data.get('schema')!r}")
        return cls(data)

    def symbols_by_segments(self, anchor_segments: list[str]) -> list[str]:
        """All defined symbols whose name-segment path ends with the anchor
        segments (template/overload families resolve as one family)."""
        hits = []
        for sym, node in self.nodes.items():
            segs = node["segments"]
            if len(segs) >= len(anchor_segments) and segs[-len(anchor_segments):] == anchor_segments:
                hits.append(sym)
        return sorted(hits)

    def file_symbols(self, path: str) -> list[str]:
        return self.documents.get(path, [])

    def symbol_def_files(self, sym: str) -> list[str]:
        node = self.nodes.get(sym)
        return sorted(node["def_files"]) if node else []


def resolve_anchors(registry: dict, graph: Graph | None) -> dict:
    """Resolve every registered anchor to SCIP symbol families.

    Returns {claim_id: {anchor_index: Resolution}} where Resolution carries
    status (resolved / UNRESOLVED_ANCHOR / unverified), the matching symbol
    families, and def-site drift vs the registry file.
    """
    out: dict[str, list] = {}
    for claim in registry.get("claims", []):
        resolutions = []
        for anchor in claim.get("cpp_anchors", []):
            anchor_segments = anchor["symbol"].split("::")
            if graph is None:
                resolutions.append(
                    {
                        "symbol": anchor["symbol"],
                        "file": anchor["file"],
                        "status": "unverified",
                        "symbols": [],
                        "resolved_files": [],
                    }
                )
                continue
            syms = graph.symbols_by_segments(anchor_segments)
            resolved_files = sorted({f for s in syms for f in graph.symbol_def_files(s)})
            drift = sorted(set(resolved_files) - {anchor["file"]}) if syms else []
            resolutions.append(
                {
                    "symbol": anchor["symbol"],
                    "file": anchor["file"],
                    "config_gate": anchor.get("config_gate"),
                    "status": "resolved" if syms else "UNRESOLVED_ANCHOR",
                    "symbols": syms,
                    "resolved_files": resolved_files,
                    "def_site_drift": drift,
                }
            )
        out[claim["id"]] = resolutions
    return out


# --- diff handling ----------------------------------------------------------


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ANALYSIS_ROOT, capture_output=True, text=True
    )
    if result.returncode != 0:
        raise ImpactError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def git_rev(rev: str) -> str:
    return git_output("rev-parse", "--verify", rev).strip()


HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def diff_to_changes(diff_text: str) -> dict:
    """Parse `git diff -U0` output into {path: {'status': M/A/D, 'hunks':
    [(start, end), ...]}} using new-side line numbers (1-based, inclusive).
    Deleted files are recorded under their old path with status D."""
    changes: dict[str, dict] = {}
    current: dict | None = None
    new_path = None
    old_path = None
    for line in diff_text.splitlines():
        if line.startswith("diff --git "):
            current = None
            new_path = None
            old_path = None
            continue
        if line.startswith("--- "):
            old_path = line[4:].strip()
            if old_path.startswith("a/"):
                old_path = old_path[2:]
            continue
        if line.startswith("+++ "):
            new_path = line[4:].strip()
            if new_path == "/dev/null":
                # Deleted file: attribute to the old path.
                new_path = old_path
                current = changes.setdefault(new_path, {"status": "D", "hunks": []})
                continue
            if new_path.startswith("b/"):
                new_path = new_path[2:]
            current = changes.setdefault(new_path, {"status": "M", "hunks": []})
            continue
        if line.startswith("new file mode"):
            if current is not None:
                current["status"] = "A"
            continue
        if line.startswith("@@"):
            if current is None:
                continue
            match = HUNK_RE.match(line)
            if not match:
                raise ImpactError(f"unparsable hunk header: {line}")
            start = int(match.group(1))
            count = int(match.group(2)) if match.group(2) is not None else 1
            if count == 0:
                # Pure deletion hunk: no new-side lines. Record the insertion
                # point so file-level checks still see the file as touched.
                current["hunks"].append((max(start, 1), max(start, 1)))
            else:
                current["hunks"].append((start, start + count - 1))
            continue
    return {path: info for path, info in changes.items() if path}


def changed_files_from_range(base: str, head: str) -> dict:
    diff_text = git_output("diff", "--no-color", "--no-renames", "-U0", f"{base}..{head}")
    return diff_to_changes(diff_text)


def changed_files_from_head() -> dict:
    diff_text = git_output("diff", "--no-color", "--no-renames", "-U0", "HEAD")
    return diff_to_changes(diff_text)


# --- impact engine -----------------------------------------------------------


DIRECT = "DIRECT_FORMAL_IMPACT"
STRUCTURAL = "STRUCTURAL_FORMAL_IMPACT"
COARSE = "COARSE_FORMAL_IMPACT"
UNKNOWN = "UNKNOWN_FORMAL_IMPACT"
NO_IMPACT = "NO_FORMAL_IMPACT"

CLASS_ORDER = {DIRECT: 4, STRUCTURAL: 3, COARSE: 2, UNKNOWN: 1, NO_IMPACT: 0}


class ClaimIndex:
    """Static (SCIP-independent) view of the registry + manifest."""

    def __init__(self, registry: dict, manifest: dict):
        self.claims = registry["claims"]
        self.suite_bindings: dict[str, list[str]] = {
            s["id"]: list(s.get("implementation_bindings", []))
            for s in manifest.get("suites", [])
        }
        self.suite_paths: dict[str, list[str]] = {
            s["id"]: [s["spec_dir"]]
            + [p for p in s.get("owner_docs", [])]
            for s in manifest.get("suites", [])
        }
        # claim -> binding files (registry anchor files + bound implementation
        # files of the claim's suites)
        self.claim_files: dict[str, set[str]] = {}
        for claim in self.claims:
            files: set[str] = {a["file"] for a in claim.get("cpp_anchors", [])}
            for suite_id in claim.get("formal_suites", []):
                files.update(self.suite_bindings.get(suite_id, []))
            self.claim_files[claim["id"]] = files

    def claims_for_file(self, path: str) -> list[str]:
        return sorted(cid for cid, files in self.claim_files.items() if path in files)


def is_cxx(path: str) -> bool:
    return Path(path).suffix.lower() in CXX_EXTS


def changed_symbols(graph: Graph, changes: dict) -> dict:
    """Map changed files to changed symbol families.

    Two attribution rules, mirroring the graph builder's own attribution so
    diff hunks and index references see the same world:
      a) exact: a defined symbol whose definition range intersects a hunk
         (COMPILER provenance: the def position is compiler-derived);
      b) enclosing: a hunk inside a function body attributes to the
         nearest-preceding named definition in the same file (HEURISTIC
         provenance per PROVENANCE_LEGEND — not a compiler-proven enclosure).

    Returns {symbol: {'files': sorted files, 'via': sorted (path, line),
    'attribution': COMPILER|HEURISTIC}}; the strongest rule that hit wins.
    """
    out: dict[str, dict] = {}
    for path, info in sorted(changes.items()):
        if info["status"] == "D":
            continue  # deleted file: file-level handling
        positions = graph.def_positions.get(path, [])
        for start, end in info["hunks"]:
            hunk_start0 = start - 1  # graph positions are 0-based lines
            # (a) exact def-range intersection
            for sym in graph.file_symbols(path):
                node = graph.nodes.get(sym)
                if not node or node.get("ns"):
                    continue  # namespace hubs are never "changed symbols"
                if not node.get("def_range"):
                    continue
                r = node["def_range"]
                def_line_start, def_line_end = r[0], r[2]
                if hunk_start0 <= def_line_end and def_line_start <= end - 1:
                    entry = out.setdefault(
                        sym, {"files": [], "via": [], "attribution": P_COMPILER}
                    )
                    entry["attribution"] = P_COMPILER  # exact hit is the strongest rule
                    if path not in entry["files"]:
                        entry["files"].append(path)
                    entry["via"].append((path, def_line_start + 1))
            # (b) enclosing definition of the first changed line
            enclosing = None
            for line0, _col, sym in positions:
                node = graph.nodes.get(sym)
                if node is None or node.get("ns"):
                    continue
                if line0 <= hunk_start0:
                    enclosing = (sym, line0)
                else:
                    break
            if enclosing is not None:
                sym, line0 = enclosing
                entry = out.setdefault(
                    sym, {"files": [], "via": [], "attribution": P_HEURISTIC}
                )
                if path not in entry["files"]:
                    entry["files"].append(path)
                entry["via"].append((path, line0 + 1))
    return out


def files_without_symbols(graph: Graph, changes: dict, changed_syms: dict) -> list[str]:
    """Changed C++ files that produced no symbol-level attribution at all."""
    attributed_files = {path for entry in changed_syms.values() for path in entry["files"]}
    return sorted(
        path
        for path, info in changes.items()
        if path not in attributed_files and is_cxx(path)
    )


def find_structural_hits(
    graph: Graph,
    anchor_symbols: dict[str, set[str]],
    start_symbols: set[str],
    max_depth: int,
) -> tuple[dict[str, list[list[str]]], list[str]]:
    """BFS from changed symbols along direction-monotonic reference paths
    (a pure callee chain or a pure caller chain — never mixed) to anchors.

    Both directions are conservative recall (#299 §11 "direct
    reference/caller neighborhood"): a changed helper that CALLS a
    formalized authority may alter how the protocol is exercised, and a
    helper USED BY an anchor may change the anchor's behavior. Mixing
    directions within one path is forbidden: it lets a shared low-level
    utility (e.g. retry_on_eintr) connect two unrelated call trees, which
    the T4 specimen shows is a false-positive pump. Paths are deterministic
    (frontier processed in sorted order); frontier-cap saturation adds an
    explicit UNKNOWN risk without dropping hits.
    """
    hits: dict[str, list[list[str]]] = {}
    risks: list[str] = []
    start_sorted = sorted(start_symbols)

    forward: dict[str, list[str]] = {}
    for sym, node in graph.nodes.items():
        if node["refs"]:
            forward[sym] = sorted(node["refs"])

    def bfs(neighbors_of, anchors):
        parent: dict[str, str | None] = {sym: None for sym in start_sorted}
        frontier = list(start_sorted)
        visited = set(start_sorted)
        depth = 0
        found: list[list[str]] = []
        while frontier and depth <= max_depth:
            for sym in frontier:
                if sym in anchors:
                    path = [sym]
                    cur = sym
                    while parent[cur] is not None:
                        cur = parent[cur]
                        path.append(cur)
                    found.append(list(reversed(path)))
            if depth == max_depth:
                break
            nxt: set[str] = set()
            for sym in frontier:
                if graph.nodes.get(sym, {}).get("ns"):
                    # Namespace nodes are reference hubs: their reverse edge
                    # set is "every symbol spelling the qualifier" (the T6
                    # false-positive pump). Traversal never expands them.
                    continue
                for nb in neighbors_of(sym):
                    if nb not in visited:
                        visited.add(nb)
                        parent[nb] = sym
                        nxt.add(nb)
            if len(visited) > FRONTIER_CAP:
                raise FrontierExceeded()
            frontier = sorted(nxt)
            depth += 1
        return found

    class FrontierExceeded(Exception):
        pass

    for claim_id, anchors in sorted(anchor_symbols.items()):
        found: list[list[str]] = []
        for neighbors_of in (
            lambda sym: forward.get(sym, ()),
            lambda sym: graph.reverse.get(sym, ()),
        ):
            try:
                found.extend(bfs(neighbors_of, anchors))
            except FrontierExceeded:
                risks.append(
                    f"traversal frontier cap {FRONTIER_CAP} exceeded for claim {claim_id}"
                )
        if found:
            uniq = sorted({tuple(p) for p in found})
            hits[claim_id] = [list(p) for p in uniq]
    return hits, risks


def make_path_record(hops: list[str], start_attribution: str, edge_provenance: str) -> dict:
    """Attach per-hop provenance to a structural path (corrective-1 C5).

    hop 0 = the changed symbol (COMPILER when the hunk intersected its
    definition range, HEURISTIC when attributed nearest-preceding);
    every later hop is a reference-derived edge whose provenance is the
    graph's edge attribution (HEURISTIC for scip-clang 0.4.0)."""
    hop_prov = [start_attribution] + [edge_provenance] * (len(hops) - 1)
    uses_heuristic = any(h == P_HEURISTIC for h in hop_prov)
    return {
        "hops": list(hops),
        "hop_provenance": hop_prov,
        "provenance": P_HEURISTIC if uses_heuristic else (hop_prov[0] if hop_prov else P_COMPILER),
        "uses_heuristic_attribution": uses_heuristic,
    }


def demote_claim(entry: dict, reason_key: str, candidate_reason: str | None) -> None:
    """Strip authoritative confidence from a claim whose evidence basis is
    incomplete (stale graph / unresolved anchor), preserving the candidate
    the current evidence suggested (corrective-1 C2 semantics)."""
    if CLASS_ORDER[entry["class"]] > CLASS_ORDER[UNKNOWN]:
        entry["candidate_class"] = entry["class"]
        entry["class"] = UNKNOWN
        if candidate_reason:
            entry["candidate_reason"] = candidate_reason
    entry["unverified_reason"] = reason_key


def classify_impact(
    graph: Graph | None,
    graph_error: str | None,
    changes: dict,
    claim_index: ClaimIndex,
    anchor_families: dict[str, set[str]],
    unresolved_anchor_claims: set[str],
    unresolved_gated_claims: set[str],
    max_depth: int,
    expected_head: str | None = None,
    build_state: dict | None = None,
    facet_index: dict | None = None,
) -> dict:
    """Core classification. Never raises; always fail-closed on uncertainty.

    Returns a result dict with:
      classification: one of the five states (aggregate max)
      claims: [{id, class, candidate_class?, unverified_reason?, paths, via,
                facet_scope, revalidation_targets, reached_anchor_ids?,
                affected_facets?, candidate_facets?}]
      paths: [{hops, hop_provenance, provenance, uses_heuristic_attribution}]
      changed_symbols: {..., attribution}
      risks / unknown_reason / fail_closed / fail_closed_reasons
      candidate_classification / candidate_claims (stale graph only)
      graph_present / graph_head / stale_index
      semantic_disposition: UNDETERMINED (always)

    FDG-0 Phase B: when facet_index is present, every impacted claim also
    carries facet routing fields. PRECISE narrows revalidation_targets to
    the affected facets' suites ONLY on a trusted explicit anchor reach;
    every untrusted state stays CONSERVATIVE_ALL on the full parent set.
    """
    risks: list[str] = []
    unknown_reasons: list[str] = []
    traversal_risks: list[str] = []
    stale = False
    cxx_changed = [p for p in changes if is_cxx(p)]
    if graph is not None and expected_head is not None and graph.head_sha != expected_head:
        stale = True
        if cxx_changed:
            risks.append(
                f"stale index: graph head {graph.head_sha[:12]} != diff head {expected_head[:12]}; "
                "symbol-level results are UNVERIFIED_STALE_GRAPH — rebuild with "
                "'formal_impact.py index' at the queried head"
            )
            unknown_reasons.append("UNVERIFIED_STALE_GRAPH")

    claim_hits: dict[str, dict] = {}
    changed_syms: dict[str, dict] = {}
    start_symbols: set[str] = set()

    def add_hit(cid: str, cls: str, paths: list[dict] | None, via: str):
        entry = claim_hits.setdefault(cid, {"class": NO_IMPACT, "paths": [], "via": [], "coarse": False})
        if CLASS_ORDER[cls] > CLASS_ORDER[entry["class"]]:
            entry["class"] = cls
        if paths:
            seen = {tuple(p["hops"]) for p in entry["paths"]}
            for p in paths:
                if tuple(p["hops"]) not in seen:
                    entry["paths"].append(p)
                    seen.add(tuple(p["hops"]))
        entry["via"].append(via)

    if graph is None:
        if cxx_changed:
            risks.append(f"no SCIP-derived graph available: {graph_error}")
            unknown_reasons.append("MISSING_GRAPH")
            for path in sorted(cxx_changed):
                risks.append(
                    f"{path}: C++ change without a symbol graph — structural reachability "
                    "cannot be established (fail-closed UNKNOWN unless file-level bound)"
                )
                for cid in claim_index.claims_for_file(path):
                    add_hit(cid, COARSE, None, f"{path} (file-level binding, no graph)")
                    claim_hits[cid]["coarse"] = True
                if not claim_index.claims_for_file(path):
                    risks.append(f"{path}: no file-level formal binding known; impact UNKNOWN")
    else:
        edge_prov = graph.edge_provenance
        changed_syms = changed_symbols(graph, changes)
        start_symbols = set(changed_syms)
        direct_claims: set[str] = set()
        for sym in start_symbols:
            for cid, anchors in sorted(anchor_families.items()):
                if sym in anchors:
                    direct_claims.add(cid)
                    add_hit(
                        cid,
                        DIRECT,
                        [make_path_record([sym], changed_syms[sym]["attribution"], edge_prov)],
                        f"{sym} (changed symbol is a registered anchor)",
                    )
        structural, traversal_risks = find_structural_hits(
            graph,
            {cid: syms for cid, syms in anchor_families.items() if cid not in direct_claims},
            start_symbols,
            max_depth,
        )
        for cid, paths in sorted(structural.items()):
            for p in paths:
                start_attr = changed_syms.get(p[0], {}).get("attribution", P_HEURISTIC)
                add_hit(
                    cid,
                    STRUCTURAL,
                    [make_path_record(p, start_attr, edge_prov)],
                    f"structural path depth {len(p) - 1}",
                )
        if traversal_risks:
            risks.extend(traversal_risks)
            unknown_reasons.append("TRAVERSAL_UNKNOWN")

        # File-level coarse fallback + unindexed-change risk (per file).
        unattributed = files_without_symbols(graph, changes, changed_syms)
        for path in unattributed:
            bound_claims = claim_index.claims_for_file(path)
            # No symbol-level information for this file (deleted file, new
            # code absent from the index, or a hunk outside any definition).
            for cid in bound_claims:
                add_hit(cid, COARSE, None, f"{path} (file-level binding; no symbol-level hit)")
                claim_hits[cid]["coarse"] = True
            if info_has_symbolless_change(changes[path]):
                risks.append(
                    f"{path}: C++ change could not be attributed to any indexed symbol "
                    "(new/unindexed code or non-definition hunk)"
                )
                unknown_reasons.append("UNATTRIBUTED_CXX_CHANGE")

        # Unresolved registered anchors strip authoritative confidence from
        # the claims they belong to when those claims are touched
        # (fail-closed demotion, preserving the candidate evidence).
        for cid in sorted(unresolved_anchor_claims):
            gated = cid in unresolved_gated_claims
            anchor_files = {
                a["file"]
                for claim in claim_index.claims
                if claim["id"] == cid
                for a in claim.get("cpp_anchors", [])
            }
            for path in sorted(changes):
                if path in anchor_files or path in claim_index.claim_files.get(cid, set()):
                    gated_note = (
                        "unresolved under its declared config gate "
                        "(this index config cannot see it)"
                        if gated
                        else "UNRESOLVED at current HEAD (renamed/moved/removed?)"
                    )
                    entry = claim_hits.setdefault(
                        cid, {"class": UNKNOWN, "paths": [], "via": [], "coarse": False}
                    )
                    candidate = entry.get("candidate_class", entry["class"])
                    demote_claim(
                        entry,
                        "CONFIG_GATE_UNRESOLVED" if gated else "UNRESOLVED_ANCHOR",
                        (
                            f"resolved anchors of {cid} still support {candidate}; "
                            f"the authority set is incomplete ({entry.get('unverified_reason')})"
                        )
                        if CLASS_ORDER[candidate] > CLASS_ORDER[UNKNOWN]
                        else None,
                    )
                    entry["via"].append(f"{path}: registered anchor of {cid} is {gated_note} — fail closed")
                    unknown_reasons.append("UNRESOLVED_ANCHOR")

    def aggregate_class() -> str:
        if claim_hits:
            return max((e["class"] for e in claim_hits.values()), key=lambda c: CLASS_ORDER[c])
        if graph is None and cxx_changed:
            return UNKNOWN
        if risks:
            return UNKNOWN
        return NO_IMPACT

    # Candidate snapshot BEFORE stale/build demotion (what the current graph
    # suggests), then the demotions themselves (corrective-1 C2 + FDG-0
    # Phase A): a stale graph or a changed build world never carries
    # authoritative confidence.
    candidate_classification = aggregate_class()
    candidate_claims = None
    candidate_reason = None
    if stale and cxx_changed:
        for cid, entry in claim_hits.items():
            if CLASS_ORDER[entry["class"]] > CLASS_ORDER[UNKNOWN]:
                demote_claim(
                    entry,
                    "UNVERIFIED_STALE_GRAPH",
                    "current (stale) graph suggests this class; rebuild the index "
                    "at the queried head to verify",
                )
        if candidate_classification != UNKNOWN:
            candidate_claims = sorted(
                cid for cid, e in claim_hits.items() if e.get("candidate_class")
            )
            candidate_reason = (
                f"the stale graph (built at {graph.head_sha[:12]}, queried at "
                f"{expected_head[:12]}) suggests {candidate_classification}; "
                "UNVERIFIED until the index is rebuilt at the queried head"
            )

    # Corrective-1 C3 (permanent rule): a build-world mismatch fails closed
    # UNCONDITIONALLY — config-only or membership-only drift with no changed
    # C++ file is still a wrong-world analysis. NO_FORMAL_IMPACT requires a
    # verified build world AND no impact.
    build_fail = build_state is not None and build_state.get("verified") is not True
    if build_fail:
        reason_key = build_state["reasons"][0] if build_state.get("reasons") else "UNKNOWN_BUILD_WORLD"
        detail = build_state.get("detail", "")
        for cid, entry in claim_hits.items():
            if CLASS_ORDER[entry["class"]] > CLASS_ORDER[UNKNOWN]:
                demote_claim(entry, reason_key, detail)
        if candidate_classification != UNKNOWN:
            candidate_claims = sorted(
                cid for cid, e in claim_hits.items() if e.get("candidate_class")
            )
            candidate_reason = (
                f"the graph's recorded build world no longer matches the "
                f"current build world ({reason_key}); {detail}"
            )
        unknown_reasons.append(reason_key)
        risks.append(f"build world mismatch ({reason_key}): {detail}")
    classification = aggregate_class()

    non_gated_unresolved = sorted(set(unresolved_anchor_claims) - set(unresolved_gated_claims))

    fail_closed_reasons: list[str] = []
    if stale and cxx_changed:
        fail_closed_reasons.append(
            f"UNVERIFIED_STALE_GRAPH: graph head {graph.head_sha[:12]} != queried head "
            f"{expected_head[:12]} — rebuild 'formal_impact.py index' at the queried head"
        )
    if graph is None and cxx_changed:
        fail_closed_reasons.append(
            "MISSING_GRAPH: a C++ diff cannot be assessed without the SCIP-derived graph"
        )
    if non_gated_unresolved and cxx_changed:
        fail_closed_reasons.append(
            "UNRESOLVED_ANCHOR (non-gated): " + ", ".join(non_gated_unresolved)
            + " — registry integrity precondition failed"
        )
    if traversal_risks:
        fail_closed_reasons.append(
            "TRAVERSAL_UNKNOWN: structural traversal hit the frontier cap — "
            "candidate paths are incomplete"
        )
    if build_fail:
        fail_closed_reasons.append(
            f"{build_state['reasons'][0]}: {build_state.get('detail')} — "
            "the graph does not describe the current build world; re-run "
            "'formal_impact.py index'"
        )
    if classification == UNKNOWN:
        fail_closed_reasons.append(f"UNKNOWN_FORMAL_IMPACT: {unknown_reason_key(unknown_reasons)}")
    fail_closed = bool(fail_closed_reasons)

    claims_out = []
    for cid, entry in sorted(claim_hits.items()):
        claim = next(c for c in claim_index.claims if c["id"] == cid)
        out_entry = {
            "id": cid,
            "title": claim.get("title", ""),
            "class": entry["class"],
            "paths": entry["paths"],
            "via": sorted(set(entry["via"])),
            "formal_suites": claim.get("formal_suites", []),
            "evidence": [e.get("path") for e in claim.get("evidence", [])],
            "provenance": claim_provenance(entry),
        }
        if entry.get("candidate_class"):
            out_entry["candidate_class"] = entry["candidate_class"]
            out_entry["unverified_reason"] = entry.get("unverified_reason")
            out_entry["candidate_reason"] = entry.get("candidate_reason")
        if facet_index is not None:
            # FDG-0 Phase B facet routing (task book §13/§14). Reached
            # anchors = DIRECT triggering anchors + structural path
            # terminals. Trust requires: a trusted symbol-level reach
            # (DIRECT/STRUCTURAL, never the coarse file fallback), a fresh
            # graph, a verified build world, no unresolved anchor and no
            # traversal uncertainty on the claim. Any demotion (stale graph
            # / build drift / unresolved) keeps the facet evidence only as
            # candidate diagnostics.
            trusted = (
                entry["class"] in (DIRECT, STRUCTURAL)
                and not entry.get("coarse")
                and not entry.get("candidate_class")
                and cid not in unresolved_anchor_claims
                and not traversal_risks
                and not build_fail
                and not (stale and cxx_changed)
            )
            reached = facet_reached_families(facet_index, cid, start_symbols, entry["paths"])
            out_entry.update(
                facet_route(claim, facet_index.get(cid), reached, trusted)
            )
        claims_out.append(out_entry)

    result = {
        "classification": classification,
        "claims": claims_out,
        "changed_symbols": {
            sym: {
                "display": graph.nodes[sym]["display"] if graph else sym,
                "files": v["files"],
                "attribution": v["attribution"],
            }
            for sym, v in sorted(changed_syms.items())
        },
        "changed_files": sorted(changes),
        "risks": sorted(set(risks)),
        "graph_present": graph is not None,
        "graph_head": graph.head_sha if graph else None,
        "stale_index": stale,
        "build_world": build_state,
        "semantic_disposition": "UNDETERMINED",
        "provenance_legend": PROVENANCE_LEGEND,
        "fail_closed": fail_closed,
        "fail_closed_reasons": fail_closed_reasons,
    }
    if classification == UNKNOWN:
        result["unknown_reason"] = unknown_reason_key(unknown_reasons)
        result["unknown_reasons"] = sorted(set(unknown_reasons))
    if candidate_claims is not None:
        result["candidate_classification"] = candidate_classification
        result["candidate_claims"] = candidate_claims
        result["candidate_reason"] = candidate_reason
    return result


def unknown_reason_key(unknown_reasons: list[str]) -> str:
    for key in (
        "UNVERIFIED_STALE_GRAPH",
        "MISSING_GRAPH",
        "UNRESOLVED_ANCHOR",
        "TRAVERSAL_UNKNOWN",
        "UNATTRIBUTED_CXX_CHANGE",
        "UNKNOWN_BUILD_WORLD",
        "UNKNOWN_BUILD_MANIFEST",
        "BUILD_WORLD_CHANGED",
        "BUILD_GRAPH_STALE",
    ):
        if key in unknown_reasons:
            return key
    return "RISK_CONDITION"


def claim_provenance(entry: dict) -> dict:
    """Claim-level provenance summary (corrective-1 C5)."""
    prov = {"anchor_binding": P_EXPLICIT, "symbol_identity": P_COMPILER}
    if any(p.get("provenance") == P_HEURISTIC for p in entry["paths"]):
        prov["structural_attribution"] = P_HEURISTIC
    if entry.get("coarse"):
        prov["coarse_mapping"] = P_FALLBACK
    return prov


# --- FDG-0 Phase B: formal facets (issue #298) --------------------------------
#
# Facets are internal subdivisions of an EXISTING claim (never new claims).
# Facet routing is anchor/path-derived: a trustworthy DIRECT/STRUCTURAL
# reach of an explicit anchor selects the facets that explicitly contain
# that anchor; every other state (COARSE fallback, UNKNOWN, stale graph,
# stale build world, unresolved anchor, frontier overflow, claim without
# facets) stays CONSERVATIVE_ALL with the FULL parent suite set. Facet
# precision is optional enrichment; it may never convert an impact into NO
# and never narrows a conservative answer. Facet mapping provenance is
# EXPLICIT (hand-maintained cross-domain authority, spec/formal/anchors.json).

FACET_PRECISE = "PRECISE"
FACET_CONSERVATIVE_ALL = "CONSERVATIVE_ALL"


def build_facet_index(registry: dict, graph: Graph | None) -> dict | None:
    """Per-claim facet routing table: {claim_id: {family_to_anchor,
    facets}} where family_to_anchor maps each RESOLVED SCIP symbol family
    of an id-carrying anchor to that anchor's id. Returns None when the
    registry declares no facets at all (legacy shape: results carry no
    facet fields)."""
    if graph is None:
        return None
    if not any(c.get("facets") for c in registry.get("claims", [])):
        return None
    resolutions = resolve_anchors(registry, graph)
    index: dict[str, dict] = {}
    for claim in registry.get("claims", []):
        facets = claim.get("facets") or []
        if not facets:
            continue
        id_by_anchor_symbol = {
            a["symbol"]: a["id"] for a in claim.get("cpp_anchors", []) if a.get("id")
        }
        family_to_anchor: dict[str, str] = {}
        for res in resolutions.get(claim["id"], []):
            aid = id_by_anchor_symbol.get(res["symbol"])
            if aid and res["status"] == "resolved":
                for fam in res["symbols"]:
                    family_to_anchor[fam] = aid
        index[claim["id"]] = {
            "family_to_anchor": family_to_anchor,
            "facets": {
                f["id"]: {
                    "anchor_refs": list(f.get("anchor_refs", [])),
                    "formal_suites": list(f.get("formal_suites", [])),
                    "semantic_labels": f.get("semantic_labels"),
                    "note": f.get("note"),
                }
                for f in facets
                if f.get("id")
            },
        }
    return index


def facet_reached_families(
    facet_index: dict | None,
    cid: str,
    start_symbols: set[str],
    structural_paths: list[dict],
) -> set[str]:
    """Anchor families that route facets for one claim (task book §13/§14):
    DIRECT triggering anchors (changed symbol == registered anchor) plus
    the TERMINAL hop of each structural path (never intermediate helpers,
    never a second both-direction sweep — claim-level conservatism already
    covers recall; facet selection is identity-based)."""
    if not facet_index:
        return set()
    fdata = facet_index.get(cid)
    if not fdata:
        return set()
    fmap = fdata["family_to_anchor"]
    reached = {s for s in start_symbols if s in fmap}
    for p in structural_paths:
        hops = p.get("hops") or []
        if hops and hops[-1] in fmap:
            reached.add(hops[-1])
    return reached


def facet_route(
    claim: dict, fdata: dict | None, reached_families: set[str], trusted: bool
) -> dict:
    """Decide the facet routing fields for one impacted claim.

    Routing semantics (phase-B task book §13/§14): a DIRECT hit routes by
    the TRIGGERING anchor id(s) (the changed symbol that IS a registered
    anchor); a STRUCTURAL hit routes by the TERMINAL anchor(s) of the
    structural paths. The claim's facet set is the union of the facets
    containing those anchors. PRECISE requires trust (fresh graph, verified
    build world, symbol-level reach, no unresolved/traversal uncertainty);
    every other state — including a claim WITHOUT facets — is
    CONSERVATIVE_ALL with the FULL parent set (issue #298 §4.3/§16).
    """
    parent = list(claim.get("formal_suites", []))
    out: dict = {"facet_scope": FACET_CONSERVATIVE_ALL, "revalidation_targets": parent}
    if fdata is None:
        return out
    anchor_ids = sorted(
        {fdata["family_to_anchor"][s] for s in reached_families if s in fdata["family_to_anchor"]}
    )
    out["reached_anchor_ids"] = anchor_ids
    facet_ids = sorted(
        fid
        for fid, f in fdata["facets"].items()
        if anchor_ids and set(f["anchor_refs"]) & set(anchor_ids)
    )
    if not trusted or not anchor_ids or not facet_ids:
        # Untrusted or unmapped reach: keep the diagnostic, never narrow.
        if anchor_ids:
            out["candidate_facets"] = {
                "affected_facets": [
                    {
                        "id": fid,
                        "formal_suites": fdata["facets"][fid]["formal_suites"],
                    }
                    for fid in facet_ids
                ],
                "note": (
                    "diagnostic only: facet_scope stays CONSERVATIVE_ALL while the "
                    "reach is untrusted (stale graph / build drift / unresolved "
                    "anchor / coarse fallback / traversal uncertainty)"
                ),
            }
        return out
    targets = sorted({s for fid in facet_ids for s in fdata["facets"][fid]["formal_suites"]})
    # Belt and braces: a facet target outside the parent (registry defect)
    # or an empty union must never narrow — fall back to the parent set.
    if not targets or not set(targets) <= set(parent):
        return out
    affected = []
    for fid in facet_ids:
        entry = {"id": fid, "formal_suites": fdata["facets"][fid]["formal_suites"]}
        if fdata["facets"][fid].get("semantic_labels"):
            entry["semantic_labels"] = fdata["facets"][fid]["semantic_labels"]
        affected.append(entry)
    out.update(
        {
            "facet_scope": FACET_PRECISE,
            "affected_facets": affected,
            "revalidation_targets": targets,
        }
    )
    return out


def info_has_symbolless_change(info: dict) -> bool:
    # Deleted files and deletion-only hunks carry no new-side symbols; both
    # were already recorded as (point) hunks, so any recorded hunk counts.
    return bool(info["hunks"])


# --- commands ----------------------------------------------------------------


def cmd_index(args) -> int:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    started = time.time()

    if not COMPDB_PATH.is_file() and not args.no_refresh_compdb:
        print(
            f"error: {COMPDB_PATH} not found. It is generated from the current "
            "xmake config by 'xmake project -k compile_commands' (or by running "
            "'index', which regenerates it automatically).",
            file=sys.stderr,
        )
        return 2

    manifest = load_manifest()
    registry = load_registry()
    problems = validate_registry(registry, manifest)
    if problems:
        print("error: anchor registry invalid; fix before indexing:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 2

    # --- FDG-0 Phase A: refresh compile_commands, generate Build Manifest ---
    # The compdb is regenerated from the CURRENT xmake configuration so the
    # exact pipeline "Xmake source graph -> compile_commands -> Build
    # Manifest -> SCIP" holds (issue #298 §8, §9). A manual compdb edit or a
    # stale file cannot silently drop or inject a production TU.
    import build_truth as bt

    if not args.no_refresh_compdb:
        print("==> regenerating compile_commands.json from the xmake config ...")
        try:
            bt.run_capture_ok(["xmake", "project", "-k", "compile_commands"])
        except bt.BuildTruthError as exc:
            print(f"error: compile_commands regeneration failed: {exc}", file=sys.stderr)
            return 2

    try:
        build_manifest = bt.build_manifest([DEFAULT_BUILD_ROOT])
    except bt.BuildTruthError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    manifest_out = bt.MANIFEST_PATH
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(build_manifest, indent=1), encoding="utf-8")

    world = build_manifest["worlds"][0]
    selected_tus = set(world["tus"])
    compdb = bt.load_compdb()
    # Exact per-TU owner join (corrective-1 C4): the SAME rule the manifest
    # builder used, so what scip-clang indexes is exactly the world's
    # selected compile interpretation set.
    src_entries = bt.select_world_entries(compdb, world)
    if not src_entries:
        print("error: selected build world produced no compile_commands entries", file=sys.stderr)
        return 2
    COMPDB_SRC_PATH.write_text(json.dumps(src_entries, indent=1), encoding="utf-8")
    print(f"==> selected world {world['id']}: {len(world['dependency_closure'])} target(s), "
          f"{len(selected_tus)} TUs, {len(src_entries)} compile commands")

    if not SCIP_CLANG_BIN.is_file():
        print(
            f"error: scip-clang not present at {SCIP_CLANG_BIN}. Run: "
            "bash scripts/formal/bootstrap-scip-clang.sh",
            file=sys.stderr,
        )
        return 2

    print(f"==> indexing {len(src_entries)} production TUs with scip-clang ...")
    index_started = time.time()
    result = subprocess.run(
        [
            str(SCIP_CLANG_BIN),
            f"--compdb-path={COMPDB_SRC_PATH}",
            f"--index-output-path={SCIP_PATH}",
            "--log-level=warning",
            "--no-progress-report",
        ],
        cwd=ANALYSIS_ROOT,
    )
    if result.returncode != 0:
        print("error: scip-clang failed (STOP_SCIP_TOOLCHAIN_BLOCKED territory)", file=sys.stderr)
        return 2
    index_secs = time.time() - index_started

    sys.path.insert(0, str(SCRIPT_DIR))
    import scip_index

    decode_started = time.time()
    meta, documents = scip_index.parse_index(SCIP_PATH.read_bytes())
    graph = scip_index.build_graph(documents)
    decode_secs = time.time() - decode_started

    head_sha = git_rev("HEAD")
    graph_json = {
        "schema": GRAPH_SCHEMA,
        "head_sha": head_sha,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        # FDG-0 Phase A: the exact build world this graph was built from.
        # build_id is the deterministic Build Manifest identity; impact/check
        # verify it against the CURRENT Xmake configuration and fail closed
        # on drift (issue #298 §6, §12).
        "build": {
            "schema": bt.MANIFEST_SCHEMA,
            "manifest": "build/formal-impact/build-manifest.json",
            "build_id": build_manifest["build_id"],
            "world_id": world["id"],
            "config_id": build_manifest["config"]["id"],
            "compdb_entries": len(src_entries),
            "selected_tus": len(selected_tus),
            "provenance": P_BUILD,
        },
        "toolchain": {
            "scip_clang": meta.tool_version,
            "scip_clang_bin_sha256": sha256_file(SCIP_CLANG_BIN),
            "project_root": meta.project_root,
            "compdb_entries": len(src_entries),
            "selected_world": world["id"],
            "lock_file": "scripts/formal/scip-clang.lock.json",
        },
        # Edge provenance (corrective-1 C5): scip-clang 0.4.0 emits no
        # enclosing ranges, so every reference edge attributes its
        # occurrence to the nearest-preceding named definition — a
        # heuristic, not a compiler-proven enclosure. Identity stays
        # compiler-derived.
        "provenance": {
            "symbol_identity": P_COMPILER,
            "reference_edges": P_HEURISTIC,
            "build_world": P_BUILD,
            "note": (
                "reference edges attribute each SCIP occurrence to the "
                "nearest-preceding named definition in its document; path "
                "consumers must not treat them as compiler-proven enclosures"
            ),
        },
        **scip_index.graph_to_json(graph),
        "timings": {
            "scip_index_seconds": round(index_secs, 1),
            "decode_graph_seconds": round(decode_secs, 1),
        },
        "artifacts": {
            "index_bytes": SCIP_PATH.stat().st_size,
            "graph_bytes": 0,
        },
    }
    payload = json.dumps(graph_json, indent=1, sort_keys=True)
    GRAPH_PATH.write_text(payload, encoding="utf-8")

    total_secs = time.time() - started
    print("==> index built")
    print(f"    head:            {head_sha[:12]}")
    print(f"    build world:     {world['id']}  build_id {build_manifest['build_id'][:12]}")
    print(f"    scip-clang:      {meta.tool_version}")
    print(f"    documents:       {graph['stats']['documents']}")
    print(f"    symbols:         {graph['stats']['symbols']}")
    print(f"    reference edges: {graph['stats']['reference_edges']}")
    print(f"    index artifact:  {SCIP_PATH.stat().st_size / 1e6:.1f} MB")
    print(f"    graph artifact:  {len(payload) / 1e6:.1f} MB (gitignored, rebuildable)")
    print(f"    wall time:       {total_secs:.1f}s (scip {index_secs:.1f}s, decode+graph {decode_secs:.1f}s)")
    return 0


def cmd_build_world(args) -> int:
    """Generate the Build Manifest from the current Xmake configuration and
    write it to build/formal-impact/build-manifest.json (issue #298 §3)."""
    import build_truth as bt

    try:
        manifest = bt.build_manifest([DEFAULT_BUILD_ROOT])
    except bt.BuildTruthError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    out = bt.MANIFEST_PATH
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    world = manifest["worlds"][0]
    print(f"==> build manifest written: {out}")
    print(f"    schema:        {manifest['schema']}")
    print(f"    head:          {manifest['head_sha'][:12]}")
    print(f"    xmake:         {manifest['xmake_version']}")
    print(f"    config id:     {manifest['config']['id'][:12]}")
    print(f"    world:         {world['id']}  roots={world['root_targets']}")
    print(f"    closure:       {world['dependency_closure']}")
    print(f"    targets:       {len(world['targets'])}")
    print(f"    TUs:           {len(world['tus'])}")
    print(f"    compdb:        {world['compdb']['selected_entries']}/{world['compdb']['total_entries']} entries selected")
    print(f"    build_id:      {manifest['build_id']}")
    return 0


def sha256_file(path: Path) -> str:
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest()


# --- FDG-0 Phase A: build-world verification ---------------------------------


def check_build_world(graph: Graph | None, manifest_path: Path | None = None) -> dict:
    """Verify that the graph's recorded build world matches the current
    Xmake configuration + Build Manifest (issue #298 §6, §12).

    Returns a state dict (never raises):
      verified: True  — graph build identity == manifest == current world
                False — a build-truth mismatch (fail closed)
                None  — graph carries no build identity (schema /1 or
                        synthetic graph): the build world is UNKNOWN. This
                        is a fail-closed state by default (corrective-1 C1);
                        only the explicit `--allow-legacy-graph-for-eval`
                        opt-in may proceed past it, and only when verified
                        is None (a verification FAILURE is never bypassed).
    """
    if graph is None or not graph.build:
        return {
            "verified": None,
            "reasons": ["UNKNOWN_BUILD_WORLD"],
            "detail": (
                "graph carries no build identity block (pre-Build-Truth or "
                "synthetic graph); the build world is UNKNOWN and fails "
                "closed by default (--allow-legacy-graph-for-eval is the "
                "explicit evaluation-only opt-in)"
            ),
        }
    try:
        import build_truth as bt

        manifest = bt.load_manifest(manifest_path) if manifest_path else bt.load_manifest()
    except Exception as exc:  # noqa: BLE001 — any failure here is fail-closed
        return {
            "verified": False,
            "reasons": ["UNKNOWN_BUILD_MANIFEST"],
            "detail": f"build manifest unavailable: {exc}",
        }
    graph_build_id = graph.build.get("build_id")
    if graph_build_id != manifest.get("build_id"):
        return {
            "verified": False,
            "reasons": ["BUILD_GRAPH_STALE"],
            "detail": (
                f"graph build_id {str(graph_build_id)[:12]} != manifest "
                f"build_id {str(manifest.get('build_id'))[:12]} — the graph "
                "was not built from the current Build Manifest; re-run 'index'"
            ),
            "manifest_build_id": manifest.get("build_id"),
        }
    try:
        current = bt.verify_current_world(manifest)
    except Exception as exc:  # noqa: BLE001 — cannot compute the world
        return {
            "verified": False,
            "reasons": ["UNKNOWN_BUILD_WORLD"],
            "detail": f"current build world cannot be computed: {exc}",
        }
    if not current.get("fresh"):
        return {
            "verified": False,
            "reasons": ["BUILD_WORLD_CHANGED"],
            "detail": current.get("detail"),
            "current_build_id": current.get("current_build_id"),
            "manifest_build_id": current.get("manifest_build_id"),
        }
    return {
        "verified": True,
        "reasons": [],
        "build_id": manifest.get("build_id"),
        "current_build_id": current.get("current_build_id"),
        "detail": "build world verified (graph == manifest == current Xmake config)",
    }


def build_claim_index_and_families(registry: dict, manifest: dict, graph: Graph | None):
    """Returns (claim_index, anchor_families, unresolved_claims,
    unresolved_gated_claims). `unresolved_claims` is ALL claims with at
    least one UNRESOLVED anchor; `unresolved_gated_claims` is the subset
    whose every unresolved anchor carries a declared config_gate (a visible
    declared gap, not a registry defect)."""
    claim_index = ClaimIndex(registry, manifest)
    if graph is None:
        return claim_index, {}, set(), set()
    resolutions = resolve_anchors(registry, graph)
    families: dict[str, set[str]] = {}
    unresolved: set[str] = set()
    unresolved_gated: set[str] = set()
    for claim in registry["claims"]:
        cid = claim["id"]
        syms: set[str] = set()
        claim_gated = True
        for res in resolutions[cid]:
            if res["status"] == "resolved":
                syms.update(res["symbols"])
            elif res["status"] == "UNRESOLVED_ANCHOR":
                unresolved.add(cid)
                if not res.get("config_gate"):
                    claim_gated = False
        if syms:
            families[cid] = syms
        if cid in unresolved and claim_gated:
            unresolved_gated.add(cid)
    return claim_index, families, unresolved, unresolved_gated


def load_graph_or_none(path: Path = GRAPH_PATH) -> tuple[Graph | None, str | None]:
    try:
        return Graph.load(path), None
    except ImpactError as exc:
        return None, str(exc)


def cmd_check(args) -> int:
    registry = load_registry(Path(args.registry) if args.registry else ANCHORS_PATH)
    manifest = load_manifest(Path(args.manifest) if args.manifest else MANIFEST_PATH)
    problems = validate_registry(registry, manifest)
    if problems:
        print("FAIL: anchor registry validation:")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("OK    registry structure (unique ids, manifest suites, paths, vocab)")
    if args.structure_only:
        print("OK    structure-only check requested: registry validation passed "
              "(anchor resolution NOT verified)")
        return 0

    # Default check: a fresh graph + full anchor resolution is the pass
    # condition (corrective-1 C1). A missing or stale graph is a failure.
    graph, graph_error = load_graph_or_none(Path(args.graph) if args.graph else GRAPH_PATH)
    if graph is None:
        print(f"FAIL  anchor resolution unverifiable: {graph_error}")
        print("FAIL  default check requires the SCIP graph; run 'index', or pass "
              "--structure-only for registry-only validation")
        return 1
    if graph.head_sha != git_rev("HEAD"):
        print(f"FAIL  graph is stale for HEAD: graph head {graph.head_sha[:12]} != "
              f"working HEAD — results would not describe the current tree")
        print("FAIL  rebuild with 'formal_impact.py index', or pass --structure-only")
        return 1

    # FDG-0 Phase A + corrective-1 C1: the graph must describe the CURRENT
    # build world. A graph WITH a build identity block is verified against
    # the Build Manifest and the live Xmake configuration; any drift is a
    # failure. A graph WITHOUT one is UNKNOWN_BUILD_WORLD and fails closed
    # by default; --allow-legacy-graph-for-eval is the explicit
    # evaluation-only opt-in (historical schema /1 fixtures, synthetic
    # graphs) and is reported in the output. It never bypasses a FAILED
    # verification.
    legacy_ok = getattr(args, "allow_legacy_graph_for_eval", False)
    if not graph.build:
        if not legacy_ok:
            print("FAIL  UNKNOWN_BUILD_WORLD: graph carries no build identity block "
                  "(pre-Build-Truth / synthetic graph); the build world cannot be "
                  "verified and 'build world unknown' must not silently pass")
            print("FAIL  rebuild with 'formal_impact.py index', or pass "
                  "--allow-legacy-graph-for-eval for evaluation-only runs")
            return 1
        print("NOTE  LEGACY GRAPH ACCEPTED FOR EVAL (--allow-legacy-graph-for-eval): "
              "build-world verification skipped; do not use for authoritative runs")
    else:
        try:
            import build_truth as bt

            bm_path = Path(args.build_manifest) if args.build_manifest else None
            build_manifest = bt.load_manifest(bm_path) if bm_path else bt.load_manifest()
            if graph.build.get("build_id") != build_manifest.get("build_id"):
                print(f"FAIL  graph build_id {str(graph.build.get('build_id'))[:12]} != "
                      f"manifest build_id {str(build_manifest.get('build_id'))[:12]} — "
                      "the graph was not built from the current Build Manifest")
                print("FAIL  rebuild with 'formal_impact.py index'")
                return 1
            current = bt.verify_current_world(build_manifest)
        except Exception as exc:  # noqa: BLE001 — cannot verify is a failure
            print(f"FAIL  build world unverifiable: {exc}")
            return 1
        if not current.get("fresh"):
            print(f"FAIL  build world changed: {current.get('detail')}")
            print("FAIL  rebuild with 'formal_impact.py index' at the current "
                  "HEAD/configuration, or pass --structure-only")
            return 1
        print(f"OK    build world verified (world {graph.build.get('world_id')}, "
              f"build_id {graph.build.get('build_id', '')[:12]})")

    resolutions = resolve_anchors(registry, graph)
    bad = 0
    gated = 0
    for cid, entries in sorted(resolutions.items()):
        anchors_by_symbol = {
            a["symbol"]: a for a in next(c for c in registry["claims"] if c["id"] == cid)["cpp_anchors"]
        }
        for res in entries:
            gate = anchors_by_symbol.get(res["symbol"], {}).get("config_gate")
            if res["status"] == "resolved":
                drift = f" (def-site drift: {', '.join(res['def_site_drift'])})" if res.get("def_site_drift") else ""
                print(f"OK    {cid}  {res['symbol']}  ->  {len(res['symbols'])} symbol(s){drift}")
            elif gate:
                gated += 1
                print(
                    f"WARN  {cid}  {res['symbol']}  UNRESOLVED under config gate {gate} "
                    f"(declared gap; index with the gate enabled to verify)"
                )
            else:
                bad += 1
                print(f"FAIL  {cid}  {res['symbol']}  UNRESOLVED_ANCHOR at HEAD "
                      f"{graph.head_sha[:12]} — renamed/moved/removed?")
    if bad:
        print(f"FAIL: {bad} unresolved anchor(s) (S3 fail-closed)")
        return 1
    if gated:
        print(f"NOTE  {gated} config-gated anchor(s) unresolved under the current index config "
              "(declared, visible, fail-closed on touch in impact)")
    print(f"OK    all registered anchors resolve at HEAD {graph.head_sha[:12]}")
    return 0


def resolve_range(args) -> tuple[str | None, str | None, dict, str | None]:
    """Returns (base, head, changes, expected_head)."""
    if args.diff_file:
        diff_text = Path(args.diff_file).read_text(encoding="utf-8")
        # --assume-head declares the patch's provenance HEAD; without it the
        # caller asserts the graph matches the diff's world and the
        # staleness check is skipped.
        assume_head = getattr(args, "assume_head", None)
        return None, assume_head, diff_to_changes(diff_text), assume_head
    if args.working_tree:
        return None, git_rev("HEAD"), changed_files_from_head(), git_rev("HEAD")
    if not args.range:
        raise ImpactError("one of --range / --diff-file / --working-tree is required")
    if ".." not in args.range:
        raise ImpactError("--range must be <base>..<head>")
    base, head = args.range.split("..", 1)
    head_sha = git_rev(head)
    return base, head, changed_files_from_range(base, head_sha), head_sha


def cmd_impact(args) -> int:
    registry = load_registry(Path(args.registry) if args.registry else ANCHORS_PATH)
    manifest = load_manifest(Path(args.manifest) if args.manifest else MANIFEST_PATH)
    problems = validate_registry(registry, manifest)
    if problems:
        print("error: anchor registry invalid:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 2

    base, head, changes, expected_head = resolve_range(args)
    graph, graph_error = load_graph_or_none(Path(args.graph) if args.graph else GRAPH_PATH)
    claim_index, families, unresolved, unresolved_gated = build_claim_index_and_families(
        registry, manifest, graph
    )
    facet_index = build_facet_index(registry, graph)

    # Corrective-1 C2: Build Truth verification is a PRECONDITION of
    # NO_FORMAL_IMPACT, so it runs BEFORE the empty-diff short-circuit.
    # NO_FORMAL_IMPACT requires a verified build world AND no impact.
    build_state = check_build_world(
        graph, Path(args.build_manifest) if args.build_manifest else None
    )
    legacy_ok = getattr(args, "allow_legacy_graph_for_eval", False)
    # The eval opt-in applies ONLY to a graph that has no build identity
    # block (verified is None). A verification FAILURE (verified False) is
    # never bypassed, with or without the flag.
    legacy_bypass = (
        legacy_ok
        and graph is not None
        and not graph.build
        and build_state.get("verified") is None
    )
    build_verified = build_state.get("verified") is True or legacy_bypass

    if not changes and build_verified:
        if getattr(args, "json", False):
            print(json.dumps({
                "classification": NO_IMPACT,
                "claims": [],
                "changed_files": [],
                "changed_symbols": {},
                "risks": [],
                "graph_present": graph is not None,
                "build_world": None if legacy_bypass else build_state,
                "build_world_eval_skipped": (
                    "legacy graph without build identity accepted for eval "
                    "(--allow-legacy-graph-for-eval); build-world verification skipped"
                ) if legacy_bypass else None,
                "semantic_disposition": "UNDETERMINED",
                "fail_closed": False,
                "fail_closed_reasons": [],
                "empty_diff": True,
                "range": args.range,
                "base": base,
                "head": head,
                "exit_code": 0,
            }, indent=2, sort_keys=True))
        else:
            print("NO_FORMAL_IMPACT (empty diff; build world verified)")
        return 0

    result = classify_impact(
        graph,
        graph_error,
        changes,
        claim_index,
        families,
        unresolved,
        unresolved_gated,
        max_depth=args.max_depth,
        expected_head=expected_head,
        build_state=None if legacy_bypass else build_state,
        facet_index=facet_index,
    )
    result["range"] = args.range
    result["base"] = base
    result["head"] = head
    if legacy_bypass:
        result["build_world_eval_skipped"] = (
            "legacy graph without build identity accepted for eval "
            "(--allow-legacy-graph-for-eval); build-world verification skipped"
        )

    # Exit contract (corrective-1 C1): fail closed on UNKNOWN, stale graph,
    # unresolved non-gated anchors, traversal UNKNOWN, or a missing graph
    # for a C++ diff. --allow-unknown-for-eval is the explicit experiment
    # opt-in for the evaluation harness; it changes the exit code only,
    # never the classification.
    allow_unknown = getattr(args, "allow_unknown_for_eval", False)
    exit_code = 1 if (result["fail_closed"] and not allow_unknown) else 0

    if args.json:
        result["exit_code"] = exit_code
        print(json.dumps(result, indent=2, sort_keys=True))
        return exit_code

    print("FORMAL IMPACT")
    print()
    if result.get("build_world_eval_skipped"):
        print(f"build world:   SKIPPED-FOR-EVAL ({result['build_world_eval_skipped']})")
        print()
    elif result["build_world"] is not None:
        bw = result["build_world"]
        if bw.get("verified") is True:
            print(f"build world:   OK ({bw.get('build_id', '')[:12]})")
        else:
            reasons = ",".join(bw.get("reasons") or ["UNKNOWN_BUILD_WORLD"])
            print(f"build world:   FAIL-CLOSED {reasons} — {bw.get('detail')}")
        print()
    print("changed:")
    for path in result["changed_files"]:
        print(f"  {path}")
    if result["changed_symbols"]:
        print("  symbols:")
        for sym, info in result["changed_symbols"].items():
            print(f"    {info['display']}  [{info['attribution']}]")
    print()
    classification = result["classification"]
    if classification == NO_IMPACT:
        print("NO_FORMAL_IMPACT")
        print("  (no anchor hit, no SCIP path hit, no file-level binding hit, no risk condition)")
    else:
        print(classification + ":")
        for claim in result["claims"]:
            print(f"  {claim['id']} — {claim['title']}")
            print(f"    class:        {claim['class']}")
            if claim.get("candidate_class"):
                print(f"    candidate:    {claim['candidate_class']} "
                      f"[{claim['unverified_reason']}] — NOT authoritative")
                if claim.get("candidate_reason"):
                    print(f"                  {claim['candidate_reason']}")
            for p in claim["paths"]:
                print("    path:")
                for i, sym in enumerate(p["hops"]):
                    display = graph.nodes[sym]["display"] if graph and sym in graph.nodes else sym
                    arrow = "      " if i == 0 else "      -> "
                    print(f"{arrow}{display}")
                prov_note = (
                    " (nearest-preceding attribution — not compiler-proven enclosure)"
                    if p["uses_heuristic_attribution"]
                    else ""
                )
                print(f"      provenance: {p['provenance']}{prov_note}")
            print(f"    provenance:   {json.dumps(claim['provenance'], sort_keys=True)}")
            if "facet_scope" in claim:
                anchors_txt = (
                    ", ".join(claim.get("reached_anchor_ids", [])) or "(none reached)"
                )
                print(f"    facet scope:  {claim['facet_scope']} (reached anchors: {anchors_txt})")
                for facet in claim.get("affected_facets", []):
                    labels = facet.get("semantic_labels") or {}
                    label_bits = []
                    if labels.get("trace_events"):
                        label_bits.append("trace: " + ", ".join(labels["trace_events"]))
                    if labels.get("model_actions"):
                        label_bits.append("actions: " + ", ".join(labels["model_actions"]))
                    suffix = f"  [{'; '.join(label_bits)}]" if label_bits else ""
                    print(
                        f"      facet {facet['id']} -> {', '.join(facet['formal_suites'])}{suffix}"
                    )
                cand = claim.get("candidate_facets")
                if cand:
                    print(
                        "      candidate facets (DIAGNOSTIC ONLY, not authoritative): "
                        + ", ".join(f["id"] for f in cand.get("affected_facets", []))
                    )
            for via in claim["via"]:
                if via.startswith("cxx "):
                    display = graph.nodes[via]["display"] if graph and via in graph.nodes else via
                    print(f"    via: {display} (changed symbol is a registered anchor)")
                else:
                    print(f"    via: {via}")
            review_targets = claim.get("revalidation_targets") or claim["formal_suites"]
            narrowed = (
                "facet targets" if claim.get("facet_scope") == "PRECISE" else "all claim suites"
            )
            if review_targets:
                print(f"    required review ({narrowed}):")
                for suite in review_targets:
                    print(f"      {suite}")
            if claim["evidence"]:
                print("    evidence:")
                for e in claim["evidence"]:
                    print(f"      {e}")
    if result.get("candidate_classification") is not None:
        print()
        print(
            f"UNVERIFIED candidates (stale graph / build world): "
            f"candidate_classification={result['candidate_classification']} "
            f"candidate_claims={result['candidate_claims']}"
        )
        print(f"  {result['candidate_reason']}")
    if result["risks"]:
        print()
        print("risks (fail-closed conditions):")
        for risk in result["risks"]:
            print(f"  - {risk}")
    print()
    if result["fail_closed"]:
        print("FAIL-CLOSED:")
        for reason in result["fail_closed_reasons"]:
            print(f"  - {reason}")
        if allow_unknown:
            print("  (--allow-unknown-for-eval: exiting 0 for the experiment harness; "
                  "the fail-closed condition above is unchanged)")
    print(f"confidence: {classification}")
    print("semantic disposition: UNDETERMINED")
    print("  (this resolver never claims a TLA+ update is or is not required;")
    print("   that verdict belongs to bounded semantic review)")
    return exit_code


def cmd_explain(args) -> int:
    registry = load_registry(Path(args.registry) if args.registry else ANCHORS_PATH)
    manifest = load_manifest(Path(args.manifest) if args.manifest else MANIFEST_PATH)
    claim = next((c for c in registry["claims"] if c["id"] == args.claim), None)
    if claim is None:
        print(f"error: unknown claim id: {args.claim}", file=sys.stderr)
        return 2
    graph, _ = load_graph_or_none(Path(args.graph) if args.graph else GRAPH_PATH)
    print(f"CLAIM {claim['id']} — {claim.get('title', '')}")
    print(f"  claim_class:     {claim.get('claim_class')}")
    print(f"  source evidence: {claim.get('source_evidence')}")
    print("  provenance:")
    print(f"    anchor <-> claim binding: {P_EXPLICIT} (registry-declared)")
    if graph:
        print(f"    graph symbol identity:    {P_COMPILER}")
        print(f"    graph reference edges:    {graph.edge_provenance}")
        print(f"      ({PROVENANCE_LEGEND.get(graph.edge_provenance, '')})")
    print("  C++ anchors:")
    resolutions = resolve_anchors(registry, graph) if graph else None
    facet_index = build_facet_index(registry, graph) if graph else None
    fdata = facet_index.get(claim["id"]) if facet_index else None
    for i, anchor in enumerate(claim.get("cpp_anchors", [])):
        line = f"    [{anchor.get('role', '?')}] {anchor['symbol']}  ({anchor['file']})"
        if anchor.get("config_gate"):
            line += f"  [config_gate: {anchor['config_gate']}]"
        print(line)
        if anchor.get("id"):
            containing = sorted(
                fid
                for fid, f in (fdata or {}).get("facets", {}).items()
                if anchor["id"] in f["anchor_refs"]
            )
            print(f"      anchor id: {anchor['id']}  facets: {', '.join(containing) or '(none)'}")
        print(f"      {anchor.get('note', '')}")
        if resolutions:
            res = resolutions[claim["id"]][i]
            if res["status"] == "resolved":
                print(
                    f"      resolved: {len(res['symbols'])} SCIP symbol(s)"
                    + (f"; def-site drift {res['def_site_drift']}" if res.get("def_site_drift") else "")
                )
            else:
                print(f"      status: {res['status']}")
    if fdata:
        print("  facets (EXPLICIT hand-maintained mapping; internal subdivisions, not new claims):")
        for fid, f in sorted(fdata["facets"].items()):
            print(f"    {fid}")
            print(f"      anchors: {', '.join(f['anchor_refs'])}")
            print(f"      formal targets: {', '.join(f['formal_suites'])}")
            labels = f.get("semantic_labels") or {}
            if labels.get("source"):
                print(f"      label source: {labels['source']}")
            if labels.get("trace_events"):
                print(f"      trace events: {', '.join(labels['trace_events'])}")
            if labels.get("model_actions"):
                print(f"      model actions: {', '.join(labels['model_actions'])}")
            if f.get("note"):
                print(f"      note: {f['note']}")
        print(
            "      facet routing is anchor/path-derived: PRECISE only on a trusted "
            "explicit anchor reach; COARSE/UNKNOWN/stale/unresolved stay "
            "CONSERVATIVE_ALL on the full parent suite set"
        )
    print("  formal suites:")
    for suite in claim.get("formal_suites", []):
        entry = next((s for s in manifest["suites"] if s["id"] == suite), {})
        bindings = ", ".join(entry.get("implementation_bindings", [])) or "(none)"
        print(f"    {suite}")
        print(f"      manifest implementation_bindings: {bindings}")
        print(f"      spec_dir: {entry.get('spec_dir')}")
        print(f"      verifier: {entry.get('verifier')}")
    print("  evidence:")
    for e in claim.get("evidence", []):
        print(f"    [{e.get('kind')}] {e.get('path')}")
    print("  notes:")
    print(f"    {claim.get('notes', '')}")
    return 0


def cmd_adjudicate(args) -> int:
    """Assemble the reduced-context LLM adjudication prompt (#299 §13).

    The graph stays the authority; the LLM only interprets candidate
    findings. This command writes the prompt file; verdicts are recorded
    separately by the evaluation harness and are ADVISORY forever.
    """
    registry = load_registry(Path(args.registry) if args.registry else ANCHORS_PATH)
    manifest = load_manifest(Path(args.manifest) if args.manifest else MANIFEST_PATH)
    base, head, changes, expected_head = resolve_range(args)
    graph, graph_error = load_graph_or_none(Path(args.graph) if args.graph else GRAPH_PATH)
    claim_index, families, unresolved, unresolved_gated = build_claim_index_and_families(
        registry, manifest, graph
    )
    result = classify_impact(
        graph,
        graph_error,
        changes,
        claim_index,
        families,
        unresolved,
        unresolved_gated,
        max_depth=args.max_depth,
        expected_head=expected_head,
    )

    full_corpus = getattr(args, "full_corpus", False)
    hit_ids = {c["id"] for c in result["claims"]}
    claims_for_prompt = registry["claims"] if full_corpus else [
        c for c in registry["claims"] if c["id"] in hit_ids
    ]

    parts = [
        "You are adjudicating whether a C++ change requires formal-claim review.",
        "Verdict vocabulary (choose per claim):",
        "REVALIDATE_REQUIRED | MODEL_UPDATE_LIKELY | CLAIM_BOUNDARY_REVIEW |",
        "NO_SEMANTIC_CHANGE_LIKELY | INSUFFICIENT_EVIDENCE",
        "Rules: your verdict is advisory; NO_SEMANTIC_CHANGE_LIKELY never refreshes",
        "anything automatically; INSUFFICIENT_EVIDENCE fails closed.",
        "",
        f"diff source: {args.range or args.diff_file or 'working tree'}",
        "",
        "=== C++ DIFF ===",
    ]
    if args.diff_file:
        parts.append(Path(args.diff_file).read_text(encoding="utf-8"))
    else:
        head_sha = git_rev(head) if head else "HEAD"
        parts.append(git_output("diff", "--no-color", "-U3", f"{base}..{head_sha}" if base else "HEAD"))
    parts.append("")
    parts.append("=== CHANGED SYMBOLS (from SCIP graph) ===")
    for sym, info in result["changed_symbols"].items():
        parts.append(f"- {info['display']} ({', '.join(info['files'])})")
    parts.append("")
    parts.append(
        "=== SCIP PATHS TO FORMAL ANCHORS (deterministic engine output; path"
        " provenance HEURISTIC = nearest-preceding attribution, not a"
        " compiler-proven enclosure) ==="
    )
    parts.append(json.dumps(result["claims"], indent=1))
    parts.append("")
    parts.append("=== FORMAL CLAIM METADATA ===")
    for claim in claims_for_prompt:
        parts.append(
            json.dumps(
                {
                    "id": claim["id"],
                    "title": claim["title"],
                    "claim_class": claim["claim_class"],
                    "anchors": [a["symbol"] for a in claim["cpp_anchors"]],
                    "suites": claim["formal_suites"],
                    "notes": claim["notes"],
                },
                indent=1,
            )
        )
        for suite in claim["formal_suites"]:
            entry = next((s for s in manifest["suites"] if s["id"] == suite), {})
            if entry.get("notes"):
                parts.append(f"--- suite {suite} manifest notes (excerpt) ---")
                parts.append(entry["notes"][:2000])
    prompt = "\n".join(parts)
    out_path = Path(args.out)
    out_path.write_text(prompt, encoding="utf-8")
    approx_tokens = len(prompt) // 4
    print(f"adjudication prompt written: {out_path}")
    print(f"  context bytes:  {len(prompt)}")
    print(f"  approx tokens:  {approx_tokens}")
    print(f"  claims included: {len(claims_for_prompt)} ({'FULL CORPUS baseline' if full_corpus else 'reduced SCIP-candidate set'})")
    return 0


def add_artifact_args(p):
    """Artifact-location overrides: lets self-tests run hermetically against
    synthetic registry/manifest/graph files and lets humans point the
    resolver at an alternate artifact set."""
    p.add_argument("--registry", help="anchor registry JSON (default: spec/formal/anchors.json)")
    p.add_argument("--manifest", help="formal manifest JSON (default: spec/tla/manifest.json)")
    p.add_argument("--graph", help="symbol graph JSON (default: build/formal-impact/graph.json)")
    p.add_argument("--build-manifest",
                   help="Build Manifest JSON (default: build/formal-impact/build-manifest.json)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_index = sub.add_parser("index", help="Build SCIP index + symbol graph")
    p_index.add_argument("--max-depth", type=int, default=DEFAULT_MAX_DEPTH, help=argparse.SUPPRESS)
    p_index.add_argument("--no-refresh-compdb", action="store_true",
                         help="do not regenerate compile_commands.json from the "
                              "current xmake config before indexing (dangerous: "
                              "the selected world is joined to the on-disk compdb)")
    p_index.set_defaults(func=cmd_index)

    p_bw = sub.add_parser("build-world",
                          help="Generate the Build Manifest from the current Xmake config")
    p_bw.set_defaults(func=cmd_build_world)

    p_check = sub.add_parser("check", help="Validate registry + anchor resolution")
    p_check.add_argument("--structure-only", action="store_true",
                         help="validate registry structure only (no graph required); "
                              "default check requires a FRESH graph + full anchor resolution")
    p_check.add_argument("--allow-legacy-graph-for-eval",
                         action="store_true",
                         help="EVALUATION OPT-IN: accept a graph without a build identity "
                              "block (schema /1 fixture / synthetic); default fails closed "
                              "with UNKNOWN_BUILD_WORLD. Never bypasses a FAILED verification.")
    add_artifact_args(p_check)
    p_check.set_defaults(func=cmd_check)

    def add_query_args(p):
        q = p.add_mutually_exclusive_group()
        q.add_argument("--range", help="git diff range <base>..<head>")
        q.add_argument("--diff-file", help="read a unified diff from file instead of git")
        q.add_argument("--working-tree", action="store_true", help="diff working tree vs HEAD")
        p.add_argument("--assume-head",
                       help="with --diff-file: declare the patch's provenance HEAD "
                            "(enables the stale-graph check)")
        p.add_argument("--max-depth", type=int, default=DEFAULT_MAX_DEPTH,
                       help=f"structural traversal depth (default {DEFAULT_MAX_DEPTH})")
        p.add_argument("--json", action="store_true", help="machine-readable output")

    p_impact = sub.add_parser("impact", help="Classify formal impact of a diff")
    add_query_args(p_impact)
    p_impact.add_argument("--allow-unknown-for-eval", action="store_true",
                          help="EXPERIMENT OPT-IN: exit 0 on UNKNOWN/fail-closed results so the "
                               "evaluation harness can observe them; never changes classification")
    p_impact.add_argument("--allow-legacy-graph-for-eval", action="store_true",
                          help="EVALUATION OPT-IN: accept a graph without a build identity "
                               "block (schema /1 fixture / synthetic); default fails closed "
                               "with UNKNOWN_BUILD_WORLD. Never bypasses a FAILED verification.")
    add_artifact_args(p_impact)
    p_impact.set_defaults(func=cmd_impact)

    p_explain = sub.add_parser("explain", help="Show one claim's full record")
    p_explain.add_argument("claim", help="claim id (e.g. F08)")
    add_artifact_args(p_explain)
    p_explain.set_defaults(func=cmd_explain)

    p_adj = sub.add_parser("adjudicate", help="Assemble reduced-context LLM adjudication prompt")
    add_query_args(p_adj)
    p_adj.add_argument("--out", required=True, help="output prompt file path")
    p_adj.add_argument("--full-corpus", action="store_true",
                       help="include ALL claims (full-corpus LLM baseline, not reduced set)")
    add_artifact_args(p_adj)
    p_adj.set_defaults(func=cmd_adjudicate)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ImpactError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
