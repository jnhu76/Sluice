#!/usr/bin/env python3
"""FTLR-0 / SCIP-PILOT formal impact resolver (issue #299).

Answers ONE bounded question for the pilot:

    a C++ diff possibly affects which formal claims / TLA+ suites,
    so which formal review is required?

Chain (issue #299 §1):

    git diff -> changed C++ symbol -> SCIP structural graph
             -> nearest registered formal anchor -> formal claim
             -> TLA+ suite / trace / bridge evidence

Hard rules (#299 §10, §24):

    * NO_FORMAL_IMPACT requires: no anchor hit, no SCIP path hit, no
      implementation_bindings hit, and no unresolved-risk condition.
      "SCIP could not see it" is UNKNOWN, never NO.
    * Impact findings never claim a semantic disposition: the resolver
      always reports `semantic disposition: UNDETERMINED`. Whether a TLA+
      model actually needs updating is decided by later bounded semantic
      review (human; the pilot's advisory LLM experiment at most suggests).
    * This is a method-selection experiment. It is NOT wired into pre-push
      or CI, and it does not modify formal claims.

Subcommands:
    index       build the SCIP index (scip-clang) and the symbol graph
    check       validate the anchor registry + anchor resolution (S1-S3)
    impact      classify a diff's formal impact (DIRECT/STRUCTURAL/COARSE/UNKNOWN/NO)
    explain     show one claim's anchors, suites, evidence, resolution
    adjudicate  assemble the reduced-context LLM adjudication prompt (experiment)

Exit contract (corrective-1): the default CLI fails closed.

    NO/DIRECT/STRUCTURAL/COARSE      -> exit 0
    UNKNOWN_FORMAL_IMPACT            -> exit 1
    stale graph                      -> exit 1 (aggregate UNKNOWN + candidates)
    UNRESOLVED non-gated anchor      -> exit 1
    frontier/traversal UNKNOWN       -> exit 1
    missing graph for a C++ diff     -> exit 1

`--allow-unknown-for-eval` is the explicit experiment opt-in that lets the
evaluation harness observe UNKNOWN results with exit 0; it never changes the
reported classification. `check` requires a fresh graph and full anchor
resolution by default; `check --structure-only` explicitly scopes the run
to registry validation.

Usage:
    python3 scripts/formal/formal_impact.py index
    python3 scripts/formal/formal_impact.py check
    python3 scripts/formal/formal_impact.py impact --range master..HEAD
    python3 scripts/formal/formal_impact.py explain F08
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
ANCHORS_PATH = REPO_ROOT / "spec" / "formal" / "anchors.json"
MANIFEST_PATH = REPO_ROOT / "spec" / "tla" / "manifest.json"
BUILD_DIR = REPO_ROOT / "build" / "formal-impact"
GRAPH_PATH = BUILD_DIR / "graph.json"
SCIP_PATH = BUILD_DIR / "index.scip"
COMPDB_PATH = REPO_ROOT / "compile_commands.json"
COMPDB_SRC_PATH = BUILD_DIR / "compile_commands.src.json"
SCIP_CLANG_BIN = BUILD_DIR / "bin" / "scip-clang"
SCIP_CLANG_LOCK = SCRIPT_DIR / "scip-clang.lock.json"

GRAPH_SCHEMA = "sluice-formal-impact-graph/1"
REGISTRY_SCHEMA = 1

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

PROVENANCE_LEGEND = {
    P_EXPLICIT: "registry-declared binding (spec/formal/anchors.json: anchor <-> claim)",
    P_COMPILER: "compiler-derived fact (SCIP symbol identity / definition positions)",
    P_HEURISTIC: (
        "nearest-preceding-named-definition attribution (scip-clang 0.4.0 emits "
        "no enclosing ranges; NOT a compiler-proven enclosure)"
    ),
    P_FALLBACK: "file-level coarse mapping (manifest implementation_bindings / anchor files)",
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
    missing paths, unknown vocab, missing fields."""
    problems: list[str] = []
    if registry.get("schema_version") != REGISTRY_SCHEMA:
        problems.append(f"schema_version must be {REGISTRY_SCHEMA}")
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
        for evidence in claim.get("evidence", []):
            epath = evidence.get("path")
            if not epath:
                problems.append(f"claim {cid}: evidence entry without path")
            elif not (root / epath).exists():
                problems.append(f"claim {cid}: evidence path missing: {epath}")
    return problems


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
        if data.get("schema") != GRAPH_SCHEMA:
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
        ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True
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
) -> dict:
    """Core classification. Never raises; always fail-closed on uncertainty.

    Returns a result dict with:
      classification: one of the five states (aggregate max)
      claims: [{id, class, candidate_class?, unverified_reason?, paths, via}]
      paths: [{hops, hop_provenance, provenance, uses_heuristic_attribution}]
      changed_symbols: {..., attribution}
      risks / unknown_reason / fail_closed / fail_closed_reasons
      candidate_classification / candidate_claims (stale graph only)
      graph_present / graph_head / stale_index
      semantic_disposition: UNDETERMINED (always)
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

    # Candidate snapshot BEFORE stale demotion (what the current graph
    # suggests), then the stale demotion itself (corrective-1 C2): a stale
    # graph never carries authoritative confidence.
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


def info_has_symbolless_change(info: dict) -> bool:
    # Deleted files and deletion-only hunks carry no new-side symbols; both
    # were already recorded as (point) hunks, so any recorded hunk counts.
    return bool(info["hunks"])


# --- commands ----------------------------------------------------------------


def cmd_index(args) -> int:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    started = time.time()

    if not COMPDB_PATH.is_file():
        print(
            f"error: {COMPDB_PATH} not found. Generate it from the current xmake "
            "config with: xmake project -k compile_commands",
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

    compdb = json.loads(COMPDB_PATH.read_text(encoding="utf-8"))
    # Production graph: src/ TUs only. The full-tree compdb includes tests,
    # bench, examples, fuzz — irrelevant to formal anchor traversal and ~5x
    # the index cost. Header symbols are indexed through the src TUs that
    # include them. With `--with-liburing=y` configuration the compdb carries
    # the REAL uring variants (SLUICE_HAS_LIBURING); the stub variants index
    # only the public interface. Both may be present; merging is additive.
    src_entries = [e for e in compdb if e.get("file", "").startswith("src/")]
    if not src_entries:
        print("error: compile_commands.json has no src/ entries", file=sys.stderr)
        return 2
    COMPDB_SRC_PATH.write_text(json.dumps(src_entries, indent=1), encoding="utf-8")

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
        cwd=REPO_ROOT,
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
        "toolchain": {
            "scip_clang": meta.tool_version,
            "scip_clang_bin_sha256": sha256_file(SCIP_CLANG_BIN),
            "project_root": meta.project_root,
            "compdb_entries": len(src_entries),
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
    print(f"    scip-clang:      {meta.tool_version}")
    print(f"    documents:       {graph['stats']['documents']}")
    print(f"    symbols:         {graph['stats']['symbols']}")
    print(f"    reference edges: {graph['stats']['reference_edges']}")
    print(f"    index artifact:  {SCIP_PATH.stat().st_size / 1e6:.1f} MB")
    print(f"    graph artifact:  {len(payload) / 1e6:.1f} MB (gitignored, rebuildable)")
    print(f"    wall time:       {total_secs:.1f}s (scip {index_secs:.1f}s, decode+graph {decode_secs:.1f}s)")
    return 0


def sha256_file(path: Path) -> str:
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest()


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

    if not changes:
        print("NO_FORMAL_IMPACT (empty diff)")
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
    )
    result["range"] = args.range
    result["base"] = base
    result["head"] = head

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
            for via in claim["via"]:
                if via.startswith("cxx "):
                    display = graph.nodes[via]["display"] if graph and via in graph.nodes else via
                    print(f"    via: {display} (changed symbol is a registered anchor)")
                else:
                    print(f"    via: {via}")
            if claim["formal_suites"]:
                print("    required review:")
                for suite in claim["formal_suites"]:
                    print(f"      {suite}")
            if claim["evidence"]:
                print("    evidence:")
                for e in claim["evidence"]:
                    print(f"      {e}")
    if result.get("candidate_classification") is not None:
        print()
        print(
            f"UNVERIFIED_STALE_GRAPH candidates: candidate_classification="
            f"{result['candidate_classification']} candidate_claims={result['candidate_claims']}"
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
    for i, anchor in enumerate(claim.get("cpp_anchors", [])):
        line = f"    [{anchor.get('role', '?')}] {anchor['symbol']}  ({anchor['file']})"
        if anchor.get("config_gate"):
            line += f"  [config_gate: {anchor['config_gate']}]"
        print(line)
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


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_index = sub.add_parser("index", help="Build SCIP index + symbol graph")
    p_index.add_argument("--max-depth", type=int, default=DEFAULT_MAX_DEPTH, help=argparse.SUPPRESS)
    p_index.set_defaults(func=cmd_index)

    p_check = sub.add_parser("check", help="Validate registry + anchor resolution")
    p_check.add_argument("--structure-only", action="store_true",
                         help="validate registry structure only (no graph required); "
                              "default check requires a FRESH graph + full anchor resolution")
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
