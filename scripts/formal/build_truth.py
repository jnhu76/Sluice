#!/usr/bin/env python3
"""FDG-0 Phase A — Xmake Build Truth layer (issue #298).

Answers ONE bounded question for the formal-impact resolver:

    what exact production build world is the resolver analyzing?

Authority chain (never regex-parses xmake.lua source text):

    Xmake configured project
        -> xmake show -t <target> --format=json      declared target facts
        -> xmake lua  (core.project API)             RESOLVED source membership
        -> xmake show --info=depgraph --format=json  dependency closure
        -> compile_commands.json                     per-TU compiler interpretation
        -> Build Manifest  (build/formal-impact/build-manifest.json)
        -> selected compile_commands entries -> SCIP index -> formal-impact graph

Xmake reality (measured on xmake v3.0.9+HEAD, 2026-09):
  * `xmake show -t X --format=json` exposes `files` as UNRESOLVED globs; the
    resolved membership comes from xmake's own glob expansion, queried via
    `xmake lua` + `target:sourcebatches()` (new files under a glob are seen
    immediately — verified).
  * `xmake show --info=depgraph --format=json` exposes the full target
    dependency graph: {root_targets, targets:[{name, deps}]}.
  * Configured option state reads via `xmake lua` + `core.project.config`
    `config.load()` (with-liburing etc.).
  * compile_commands.json contains DUPLICATE entries for the same TU under
    multiple targets/configurations; each entry's `-o` object path encodes
    its owning target (`build/.objs/<target>/...`, verified on 100% of 418
    entries) — that is the deterministic join key from target membership to
    per-TU compiler interpretation.

Fail-closed rules (FDG-0 Phase A §7, §17):
  * membership comes from Xmake, never from path prefixes;
  * an Xmake-owned TU with no compile_commands entry   -> UNKNOWN_BUILD_WORLD;
  * a selected TU with ambiguous incompatible entries  -> UNKNOWN_BUILD_WORLD;
  * unknown/stale build interpretation                 -> non-zero, never NO.

The system may be allowed not to know. It must never silently not know.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path, PurePosixPath

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

MANIFEST_SCHEMA = "sluice-build-manifest/1"
MANIFEST_PATH = REPO_ROOT / "build" / "formal-impact" / "build-manifest.json"

# Fixed list of build options recorded in the manifest. These are the options
# that can change implementation visibility / compile interpretation. The
# list is a fixed declared set (not a parse of xmake.lua); per-TU compile
# command interpretation is additionally recorded from compile_commands.json,
# so flag-level drift is caught even for options outside this list.
CONFIG_OPTIONS = [
    "with-liburing",
    "with-uring-registered-buffers",
    "with-uring-registered-files",
    "hardened",
    "mode",
    "plat",
    "arch",
    "toolchain",
]

# Object-path owner pattern: `build/.objs/<target>/...`. Verified on 100% of
# the repository's 418 compile_commands entries (xmake 3.0.9+HEAD).
OBJ_OWNER_RE = re.compile(r"/\.objs/([^/]+)/")

# Relevant xmake option for the liburing gate (defines SLUICE_HAS_LIBURING).
LIBURING_OPTION = "with-liburing"


class BuildTruthError(Exception):
    """Hard tooling/environment error: the build world cannot be represented.
    Mapped to exit 2 by the CLI (malformed invocation/tooling hard error)."""


# --- subprocess wrapper (monkeypatchable in hermetic tests) ------------------


def run_capture(args: list[str], cwd: Path = REPO_ROOT) -> str:
    result = subprocess.run(
        args, cwd=cwd, capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        raise BuildTruthError(
            f"{' '.join(args)} failed (exit {result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result.stdout


def run_capture_ok(args: list[str], cwd: Path = REPO_ROOT, ok: tuple = (0,)) -> str:
    result = subprocess.run(
        args, cwd=cwd, capture_output=True, text=True, timeout=120
    )
    if result.returncode not in ok:
        raise BuildTruthError(
            f"{' '.join(args)} failed (exit {result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return result.stdout


# --- Xmake queries -----------------------------------------------------------


def xmake_version() -> str:
    out = run_capture(["xmake", "--version"], cwd=REPO_ROOT)
    first = out.splitlines()[0] if out.splitlines() else ""
    # "xmake v3.0.9+HEAD.2b184e1, ..." -> "v3.0.9+HEAD.2b184e1"
    parts = first.split()
    version = parts[1] if len(parts) > 1 else first
    return version.rstrip(",")


CONFIG_LUA = (
    'import("core.project.config"); config.load()\n'
    "local keys = {"
    + ",".join(repr(k) for k in CONFIG_OPTIONS)
    + "}\n"
    "for _, k in ipairs(keys) do\n"
    '  local v = config.get(k)\n'
    '  if type(v) == "boolean" then v = v and "true" or "false" end\n'
    '  print(k .. "=" .. tostring(v))\n'
    "end\n"
)


def config_options() -> dict:
    """Read the current configured option state from Xmake itself."""
    out = run_capture(["xmake", "lua", "-c", CONFIG_LUA], cwd=REPO_ROOT)
    options: dict[str, str | None] = {}
    for line in out.splitlines():
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        if key not in CONFIG_OPTIONS:
            continue
        value = value.strip()
        if value == "nil":
            options[key] = None
        else:
            options[key] = value
    return {k: options.get(k) for k in CONFIG_OPTIONS}


def depgraph() -> dict:
    out = run_capture(
        ["xmake", "show", "--info=depgraph", "--format=json"], cwd=REPO_ROOT
    )
    try:
        return json.loads(out)
    except json.JSONDecodeError as exc:
        raise BuildTruthError(f"xmake depgraph output not JSON: {exc}") from exc


def target_metadata(name: str) -> dict:
    out = run_capture(["xmake", "show", "-t", name, "--format=json"], cwd=REPO_ROOT)
    try:
        data = json.loads(out)
    except json.JSONDecodeError as exc:
        raise BuildTruthError(
            f"xmake show -t {name} output not JSON: {exc}"
        ) from exc
    if not isinstance(data, dict) or not data.get("name"):
        raise BuildTruthError(
            f"xmake show -t {name} did not return target metadata "
            "(target missing?)"
        )
    return data


SOURCEBATCH_LUA = (
    'import("core.project.config"); config.load()\n'
    'import("core.project.project")\n'
    "local t = project.target(%s)\n"
    "if t == nil then return end\n"
    "local seen = {}\n"
    "local batches = t:sourcebatches()\n"
    "for _, b in pairs(batches) do\n"
    "  for _, f in ipairs(b.sourcefiles or {}) do\n"
    "    if not seen[f] then seen[f] = true print(f) end\n"
    "  end\n"
    "end\n"
)


def resolved_sources(name: str) -> list[str]:
    """Resolved source membership from Xmake's own glob expansion. This is
    the authoritative source list — new files under a target's glob are seen
    without reconfigure (verified)."""
    script = SOURCEBATCH_LUA % json.dumps(name)
    out = run_capture(["xmake", "lua", "-c", script], cwd=REPO_ROOT)
    files = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        files.append(str(PurePosixPath(line)))
    return sorted(set(files))


# --- compile_commands --------------------------------------------------------


def load_compdb(path: Path | None = None) -> list[dict]:
    if path is None:
        path = REPO_ROOT / "compile_commands.json"
    if not path.is_file():
        raise BuildTruthError(f"compile_commands.json not found: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise BuildTruthError(f"malformed compile_commands.json {path}: {exc}") from exc
    if not isinstance(data, list):
        raise BuildTruthError(f"compile_commands.json {path} is not a list")
    return data


def compdb_owner(entry: dict) -> str | None:
    """Owning target name from the entry's object path
    (`build/.objs/<target>/...`), or None when the entry carries no object
    path (an entry without a deterministic owner is not usable as build
    truth)."""
    args = entry.get("arguments")
    if isinstance(args, list):
        joined = " ".join(args)
    else:
        joined = str(entry.get("command", ""))
    match = OBJ_OWNER_RE.search(joined)
    return match.group(1) if match else None


def entry_identity(entry: dict) -> str:
    """Deterministic identity of one compile_commands entry (the exact
    compiler interpretation)."""
    payload = {
        "file": entry.get("file"),
        "directory": entry.get("directory"),
        "arguments": entry.get("arguments") or str(entry.get("command", "")),
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def entry_summary(entry: dict) -> dict:
    """Human-usable summary of one compile command: compiler, relevant
    defines, include dirs, and the remaining flags (the `-o` object path is
    the join key and is preserved inside `flags`)."""
    args = entry.get("arguments")
    if not isinstance(args, list):
        args = str(entry.get("command", "")).split()
    compiler = args[0] if args else None
    defines: list[str] = []
    includes: list[str] = []
    rest: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("-D"):
            defines.append(a)
        elif a in ("-I", "-isystem", "-iquote"):
            if i + 1 < len(args):
                includes.append(f"{a} {args[i + 1]}")
                i += 2
                continue
        elif a.startswith("-I") or a.startswith("-isystem="):
            includes.append(a)
        else:
            rest.append(a)
        i += 1
    return {
        "compiler": compiler,
        "defines": sorted(defines),
        "includes": sorted(includes),
        "flags": rest,
    }


# --- world selection ---------------------------------------------------------


def normalize_path(path: str) -> str:
    return str(PurePosixPath(path))


def dependency_closure(root_targets: list[str], graph: dict) -> list[str]:
    """BFS closure over the Xmake depgraph edges (order = BFS discovery)."""
    edges = {t["name"]: list(t.get("deps", [])) for t in graph.get("targets", [])}
    order: list[str] = []
    seen: set[str] = set()
    for root in root_targets:
        if root not in edges:
            raise BuildTruthError(
                f"selected target {root!r} not present in the Xmake depgraph"
            )
        frontier = [root]
        while frontier:
            name = frontier.pop(0)
            if name in seen:
                continue
            seen.add(name)
            order.append(name)
            for dep in edges.get(name, []):
                if dep not in seen:
                    frontier.append(dep)
    return order


def select_compdb_entries(compdb: list[dict], tu_path: str, world_targets: set[str]) -> list[dict]:
    """Compile_commands entries for one TU owned by a target in the selected
    world. Entries owned by targets outside the world (test seams, internal
    testing variants, unselected libs) are excluded — membership comes from
    Xmake target ownership, not from the source path."""
    tu = normalize_path(tu_path)
    out = []
    for entry in compdb:
        if normalize_path(str(entry.get("file", ""))) != tu:
            continue
        owner = compdb_owner(entry)
        if owner in world_targets:
            out.append(entry)
    return out


# --- manifest construction ---------------------------------------------------


def canonical_json(payload: dict) -> str:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def config_id(options: dict) -> str:
    return hashlib.sha256(canonical_json(options).encode("utf-8")).hexdigest()


def variant_id(root_targets: list[str], options: dict) -> str:
    root = root_targets[0] if root_targets else "target"
    gate = options.get(LIBURING_OPTION)
    suffix = "liburing" if gate == "true" else ("default" if gate == "false" else "unknown")
    return f"{root}-{suffix}"


def build_world(
    root_targets: list[str],
    options: dict,
    graph: dict,
    compdb: list[dict],
    head_sha: str,
    xmake_ver: str,
) -> dict:
    """Construct one selected build world (manifest `worlds[0]`). Fails
    closed (BuildTruthError) on missing/ambiguous compile interpretations."""
    closure = dependency_closure(root_targets, graph)
    world_targets = set(closure)

    targets: dict[str, dict] = {}
    for name in closure:
        meta = target_metadata(name)
        targets[name] = {
            "kind": meta.get("kind"),
            "direct_deps": sorted(d.get("name") for d in meta.get("deps", [])),
            "defines": sorted(d.get("value") for d in meta.get("defines", []) if d.get("value")),
            "includedirs": sorted(
                d.get("value") for d in meta.get("includedirs", []) if d.get("value")
            ),
            "source_globs": sorted(
                f.get("path") for f in meta.get("files", []) if f.get("path")
            ),
            "resolved_sources": resolved_sources(name),
            "compiler": _first_compiler_program(meta),
            "compiler_flags": _first_compiler_flags(meta),
        }

    # Union of all world TUs (each indexed exactly once even if multiple
    # world targets own it).
    tus: dict[str, dict] = {}
    for name in closure:
        for src in targets[name]["resolved_sources"]:
            tu = normalize_path(src)
            tus.setdefault(tu, {"owning_targets": []})["owning_targets"].append(name)
    for tu in tus:
        tus[tu]["owning_targets"] = sorted(set(tus[tu]["owning_targets"]))

    selected_entries = 0
    for tu in sorted(tus):
        entries = select_compdb_entries(compdb, tu, world_targets)
        if not entries:
            raise BuildTruthError(
                f"UNKNOWN_BUILD_WORLD: Xmake-owned TU {tu} has no compile_commands "
                "entry owned by the selected world (stale compdb? regenerate with "
                "'xmake project -k compile_commands')"
            )
        identities = {entry_identity(e) for e in entries}
        if len(identities) > 1:
            raise BuildTruthError(
                f"UNKNOWN_BUILD_WORLD: {len(entries)} incompatible compile_commands "
                f"candidates for Xmake-owned TU {tu} inside the selected world "
                "(no deterministic Xmake-derived way to choose)"
            )
        chosen = entries[0]
        summary = entry_summary(chosen)
        tus[tu].update(
            {
                "compdb_identity": identities.pop(),
                "compiler": summary["compiler"],
                "defines": summary["defines"],
                "includes": summary["includes"],
                "flags": summary["flags"],
            }
        )
        selected_entries += 1

    world = {
        "id": variant_id(root_targets, options),
        "root_targets": list(root_targets),
        "config": {k: options.get(k) for k in CONFIG_OPTIONS},
        "dependency_closure": closure,
        "targets": targets,
        "tus": {tu: tus[tu] for tu in sorted(tus)},
        "compdb": {
            "path": "compile_commands.json",
            "total_entries": len(compdb),
            "selected_entries": selected_entries,
        },
    }
    return world


def _first_compiler_program(meta: dict) -> str | None:
    compilers = meta.get("compilers") or []
    if compilers:
        return compilers[0].get("program")
    linker = meta.get("linker")
    return linker if isinstance(linker, str) else None


def _first_compiler_flags(meta: dict) -> str | None:
    compilers = meta.get("compilers") or []
    if compilers:
        return compilers[0].get("flags")
    return None


def compute_build_identity(manifest: dict) -> str:
    """Deterministic build-world identity. Changes when any relevant build
    interpretation changes: selected targets, TU set, dependency closure,
    defines/options, or per-TU compile commands (see FDG-0 Phase A §6)."""
    payload = {
        "schema": manifest.get("schema"),
        "head_sha": manifest.get("head_sha"),
        "xmake_version": manifest.get("xmake_version"),
        "config": manifest.get("config"),
        "worlds": manifest.get("worlds"),
    }
    return hashlib.sha256(canonical_json(payload).encode("utf-8")).hexdigest()


def build_manifest(
    root_targets: list[str],
    compdb: list[dict] | None = None,
    head_sha: str | None = None,
    xmake_ver: str | None = None,
    generated_at: str | None = None,
) -> dict:
    """Generate the Build Manifest for the CURRENT configured Xmake world."""
    import time

    if head_sha is None:
        head_sha = current_head()
    if xmake_ver is None:
        xmake_ver = xmake_version()
    if generated_at is None:
        generated_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    if compdb is None:
        compdb = load_compdb()
    options = config_options()
    graph = depgraph()
    world = build_world(root_targets, options, graph, compdb, head_sha, xmake_ver)
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "schema_version": 1,
        "generated_at": generated_at,
        "head_sha": head_sha,
        "xmake_version": xmake_ver,
        "config": {
            "id": config_id(options),
            "options": {k: options.get(k) for k in CONFIG_OPTIONS},
        },
        "worlds": [world],
        "provenance": {
            "BUILD": (
                "Xmake-derived target/source/config/build fact: xmake show "
                "--format=json, xmake lua core.project API (resolved "
                "membership), depgraph, and compile_commands.json object-path "
                "ownership join"
            ),
        },
    }
    manifest["build_id"] = compute_build_identity(manifest)
    return manifest


def current_head() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, capture_output=True, text=True
    )
    if result.returncode != 0:
        raise BuildTruthError("git rev-parse HEAD failed")
    return result.stdout.strip()


# --- load / verify -----------------------------------------------------------


def load_manifest(path: Path = MANIFEST_PATH) -> dict:
    if not path.is_file():
        raise BuildTruthError(f"build manifest not found: {path} (run 'index')")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise BuildTruthError(f"malformed build manifest {path}: {exc}") from exc
    if data.get("schema") != MANIFEST_SCHEMA:
        raise BuildTruthError(f"unsupported build manifest schema: {data.get('schema')!r}")
    return data


def verify_current_world(manifest: dict, compdb: list[dict] | None = None) -> dict:
    """Recompute the build identity for the CURRENT Xmake configuration and
    compare it against the manifest's recorded world. Returns
    {'fresh': True} or {'fresh': False, 'reason': <key>, 'detail': ...}.

    Raises BuildTruthError only when the current world cannot be computed at
    all (missing xmake/compdb) — callers decide whether that is a hard error
    or a fail-closed state."""
    options = config_options()
    graph = depgraph()
    if compdb is None:
        compdb = load_compdb()
    world = build_world(
        manifest["worlds"][0]["root_targets"],
        options,
        graph,
        compdb,
        current_head(),
        xmake_version(),
    )
    probe = {
        "schema": manifest.get("schema"),
        "head_sha": current_head(),
        "xmake_version": xmake_version(),
        "config": {"id": config_id(options), "options": {k: options.get(k) for k in CONFIG_OPTIONS}},
        "worlds": [world],
    }
    probe_id = hashlib.sha256(canonical_json(probe).encode("utf-8")).hexdigest()

    recorded_head = manifest.get("head_sha")
    recorded_cfg = (manifest.get("config") or {}).get("id")
    recorded_world = manifest.get("worlds", [{}])[0]

    mismatches = []
    if recorded_head != probe["head_sha"]:
        mismatches.append(
            f"manifest HEAD {recorded_head[:12] if recorded_head else None} != "
            f"current HEAD {probe['head_sha'][:12]}"
        )
    if recorded_cfg != probe["config"]["id"]:
        mismatches.append("manifest config != current Xmake configuration")
    if (recorded_world.get("id") or "") != world.get("id"):
        mismatches.append(
            f"manifest world {recorded_world.get('id')!r} != current world {world.get('id')!r}"
        )
    if recorded_world.get("tus", {}).keys() != world["tus"].keys():
        mismatches.append("manifest TU set != current resolved TU set")
    if probe_id != manifest.get("build_id"):
        mismatches.append("build identity changed (TU set / targets / defines / compile commands)")
    if mismatches:
        return {
            "fresh": False,
            "reason": "BUILD_WORLD_CHANGED",
            "detail": "; ".join(mismatches),
            "current_build_id": probe_id,
            "manifest_build_id": manifest.get("build_id"),
        }
    return {"fresh": True, "current_build_id": probe_id, "manifest_build_id": manifest.get("build_id")}
