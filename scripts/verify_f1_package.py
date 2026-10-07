#!/usr/bin/env python3
"""F1 clean-room package verification (#402 Phase F1).

Builds the libraries, installs them into a fresh prefix, freezes the
machine-readable manifest and the archive/object/symbol baseline, then
compiles/links/runs the clean-room external consumers against the installed
prefix ONLY, and runs the negative package probes.

Gates evaluated:
  F1_B_PACKAGE_PREFIX_READY
  F1_B_PROVENANCE_VERIFIED
  CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY
  PACKAGE_USAGE_REQUIREMENTS_AND_DECLARATIONS_MATCH_BASELINE
  F1_C_EXTERNAL_WORKLOADS_RECORDED

Usage:
  python3 scripts/verify_f1_package.py --manifest-out M.json \
      --archive-baseline-out B.json                  # no-liburing profile
  python3 scripts/verify_f1_package.py --liburing --uring-prefix DIR \
      --manifest-out M.json --archive-baseline-out B.json
  python3 scripts/verify_f1_package.py --check-frozen M.json \
      --archive-baseline-out /tmp/B.json             # reproduce a frozen
                                                     # manifest: committed frozen
                                                     # baseline == manifest-bound
                                                     # sha256 == fresh baseline

--check-frozen pins the production baseline to the frozen manifest's
PRODUCTION_BASELINE_SHA (it must exist as a commit and be an ancestor of
HEAD), so a reproduction is invariant to later docs-only advancement of
master/origin; only src/include drift from the pinned baseline fails the
provenance gate. A freeze run (no --check-frozen) discovers the baseline as
the merge-base with origin/master per the F1 protocol. The pinned baseline
must be present in the local history: a shallow checkout (fetch-depth 1)
cannot validate ancestry, so a CI reproduction run needs fetch-depth: 0 or an
explicit fetch of the pinned SHA.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONSUMERS = REPO / "tests" / "package" / "consumers"

# Manifest keys that legitimately differ between the freezing run and a later
# reproduction run; --check-frozen ignores them when comparing.
VOLATILE_MANIFEST_KEYS = {
    "GENERATED_UTC",
    "VERIFICATION_HEAD",
    "VERIFICATION_HEAD_DIRTY",
    "ORIGIN_MASTER_TIP",
}

P0_CONSUMERS = ["w01_direct"]  # core archive only
P1_CONSUMERS = [
    "w02_pipeline",
    "w03_external_loop_threadpool",
    "w04_stackful_candidate",
    "contract_request_lifetime",
    "contract_admission",
    "contract_cancel_effect",
    "contract_observer",
    "contract_progress",
    "contract_shutdown",
    "contract_durability",
    "contract_backend_availability",
]
# Built in both profiles; the uring argv variant reports UNAVAILABLE without
# the liburing profile and runs the real backend with it.
P1_CONDITIONAL = ["w03_external_loop_uring"]


def run(cmd, **kw):
    print(f"+ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run([str(c) for c in cmd], **kw)


def capture(cmd):
    return run(cmd, capture_output=True, text=True).stdout.strip()


def git_out(*args, cwd=REPO):
    return run(["git", *args], capture_output=True, text=True, cwd=cwd).stdout.strip()


def git_ok(*args, cwd=REPO):
    return run(["git", *args], capture_output=True, text=True, cwd=cwd).returncode == 0


def resolve_production_baseline(repo, frozen_data=None):
    """Production baseline for this verification run.

    --check-frozen pins it to the frozen manifest's PRODUCTION_BASELINE_SHA:
    the frozen identity must not move with the branch tip, so it is validated
    (exists as a commit, is an ancestor of HEAD) instead of being re-derived,
    and origin/master is never consulted. A freeze run discovers it as the
    merge-base with origin/master (the F0/adopted baseline) per the F1
    protocol.
    """
    if frozen_data is None:
        return git_out("merge-base", "HEAD", "origin/master", cwd=repo)
    if not isinstance(frozen_data, dict):
        raise SystemExit("--check-frozen: frozen manifest is not a JSON object")
    pinned = str(frozen_data.get("PRODUCTION_BASELINE_SHA") or "")
    if not pinned:
        raise SystemExit("--check-frozen: frozen manifest has no PRODUCTION_BASELINE_SHA")
    if not git_ok("cat-file", "-e", f"{pinned}^{{commit}}", cwd=repo):
        raise SystemExit(
            f"--check-frozen: pinned production baseline {pinned} is not a "
            "commit in this repository")
    if not git_ok("merge-base", "--is-ancestor", pinned, "HEAD", cwd=repo):
        raise SystemExit(
            f"--check-frozen: pinned production baseline {pinned} is not an "
            "ancestor of HEAD; the frozen identity does not bind this tree")
    return pinned


def production_diff_paths(repo, baseline_sha):
    """src/include files changed from the production baseline to HEAD; the
    provenance gate requires this set to be empty."""
    out = git_out("diff", "--name-only", f"{baseline_sha}..HEAD", "--", "src",
                  "include", cwd=repo)
    return out.splitlines() if out else []


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def compiler_id():
    out = run(["g++", "--version"], capture_output=True, text=True).stdout
    return out.splitlines()[0].strip() if out else "unknown"


def check_clean_room(command_strs, prefix):
    """Gate: no compile/link command may reference the source or build tree."""
    violations = []
    prefix_str = str(prefix)
    for cmd in command_strs:
        tokens = cmd.split()
        for i, tok in enumerate(tokens):
            for flag in ("-I", "-isystem", "-iquote", "-imacros", "-L",
                         "-include", "-idirafter"):
                if tok.startswith(flag) and len(tok) > len(flag):
                    path = tok[len(flag):]
                elif tok in (flag,):
                    path = tokens[i + 1] if i + 1 < len(tokens) else ""
                else:
                    continue
                rp = str(Path(path).resolve())
                if prefix_str == rp or rp.startswith(prefix_str + os.sep):
                    continue
                if rp == str(REPO) or rp.startswith(str(REPO) + os.sep):
                    violations.append((cmd, flag, rp))
    return violations


INCLUDE_RE = re.compile(r"^\s*#\s*include\s*(.+?)\s*$")


def scan_consumer_includes():
    """Consumer sources may include only bare angle-bracket headers. Quoted
    includes, absolute paths, `..` traversal and macro operands can each reach
    source-tree headers, so every include operand must be `<...>` with no
    quote, no leading `/` and no `..` segment."""
    violations = []
    for src in sorted(CONSUMERS.glob("*.cpp")):
        for lineno, line in enumerate(src.read_text().splitlines(), 1):
            m = INCLUDE_RE.match(line.strip())
            if not m:
                continue
            operand = m.group(1)
            ok = (operand.startswith("<") and operand.endswith(">")
                  and '"' not in operand and not operand.lstrip("<").startswith("/")
                  and ".." not in operand)
            if not ok:
                violations.append((src.name, lineno, operand))
    return violations


CONSUMER_ENV_KEEP = (
    "PATH", "HOME", "LANG", "LC_ALL", "LC_CTYPE", "TERM", "TMPDIR",
    "LD_LIBRARY_PATH",
)
ENV_PATH_VARS = ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
                 "OBJC_INCLUDE_PATH", "CXXFLAGS", "CFLAGS", "CPPFLAGS",
                 "LDFLAGS", "LIBRARY_PATH")


def consumer_env(extra=None):
    """Consumers see a whitelisted environment. An include/lib path variable
    pointing into the repository would silently reintroduce the source tree,
    so its presence is a hard failure, not a scrub."""
    env = {k: v for k, v in os.environ.items() if k in CONSUMER_ENV_KEEP}
    for var in ENV_PATH_VARS:
        if var not in os.environ:
            continue
        for part in os.environ[var].split(os.pathsep):
            if not part:
                continue
            rp = str(Path(part).resolve())
            if rp == str(REPO) or rp.startswith(str(REPO) + os.sep):
                raise SystemExit(
                    f"clean-room violation: env {var} contains repository path {part}")
    if extra:
        env.update(extra)
    return env


def compile_consumer(build_dir, prefix, sources, out, extra_cxxflags, extra_ldflags,
                     command_log, extra_env=None):
    cxx = "g++"
    objdir = build_dir / (out.name + ".objs")
    objdir.mkdir(parents=True, exist_ok=True)
    objs = []
    env = consumer_env(extra_env)
    for src in sources:
        obj = objdir / (Path(src).stem + ".o")
        cmd = [cxx, "-std=c++20", "-Wall", "-Wextra", f"-I{prefix}/include",
               *extra_cxxflags, "-c", src, "-o", obj]
        command_log.append(" ".join(str(c) for c in cmd))
        r = run(cmd, env=env)
        if r.returncode != 0:
            return False
        objs.append(obj)
    cmd = [cxx, "-std=c++20", *objs, f"-L{prefix}/lib", *extra_ldflags, "-o", out]
    command_log.append(" ".join(str(c) for c in cmd))
    r = run(cmd, env=env)
    if r.returncode != 0:
        return False
    # Record the runtime library dependencies of every consumer binary.
    ldd = run(["ldd", out], capture_output=True, text=True)
    command_log.append(f"ldd {out}: " + " | ".join(
        line.strip() for line in ldd.stdout.splitlines() if line.strip()))
    return True


def expect_link_failure(build_dir, prefix, source, extra_cxxflags, command_log):
    """Negative probe: the link must fail (exit nonzero)."""
    cxx = os.environ.get("CXX", "g++")
    obj = build_dir / (Path(source).stem + ".o")
    cmd = [cxx, "-std=c++20", f"-I{prefix}/include", *extra_cxxflags, "-c", source, "-o", obj]
    command_log.append(" ".join(str(c) for c in cmd))
    if run(cmd).returncode != 0:
        return False, "compile unexpectedly failed"
    cmd = [cxx, obj, f"-L{prefix}/lib", "-lsluice_core", "-o", build_dir / "negative.out"]
    command_log.append(" ".join(str(c) for c in cmd))
    r = run(cmd, capture_output=True)
    return r.returncode != 0, "link against core-only must fail with undefined async symbols"


def compile_object(build_dir, prefix, source, out_obj, extra_cxxflags, command_log):
    cxx = "g++"
    cmd = [cxx, "-std=c++20", "-Wall", "-Wextra", f"-I{prefix}/include",
           *extra_cxxflags, "-c", source, "-o", out_obj]
    command_log.append(" ".join(str(c) for c in cmd))
    return run(cmd, env=consumer_env()).returncode == 0


def parse_nm_symbols(obj_path, defined):
    flag = "--defined-only" if defined else "--undefined-only"
    out = run(["nm", flag, str(obj_path)], capture_output=True, text=True).stdout
    symbols = set()
    for line in out.splitlines():
        parts = line.split()
        if defined:
            if len(parts) >= 3:
                symbols.add(parts[2])
        else:
            if len(parts) == 2 and parts[0] == "U":
                symbols.add(parts[1])
    return sorted(symbols)


def build_archive_baseline(prefix, profile, scratch_dir):
    """Archive/object/symbol baseline: per-archive sha256, per-object member
    sha256 and defined symbols, the per-archive union of undefined references
    (sibling objects in the same or the other archive may define some of
    them), and the profile-level external undefined set (undefined references
    no object of the profile defines). The content is head-independent so a
    frozen file can be reproduced byte-for-byte by --check-frozen."""
    archives = []
    profile_defined = set()
    profile_undefined = set()
    for lib in sorted((prefix / "lib").glob("*.a")):
        members = [m for m in capture(["ar", "t", str(lib)]).splitlines() if m.strip()]
        objects = []
        undefined_refs = set()
        for member in members:
            data = subprocess.run(["ar", "p", str(lib), member],
                                  capture_output=True, check=True).stdout
            obj = scratch_dir / f"{lib.name}.{member}"
            obj.write_bytes(data)
            defined = parse_nm_symbols(obj, defined=True)
            objects.append({
                "member": member,
                "bytes": len(data),
                "sha256": sha256_bytes(data),
                "defined_symbols": defined,
            })
            profile_defined.update(defined)
            undefined = parse_nm_symbols(obj, defined=False)
            undefined_refs.update(undefined)
            profile_undefined.update(undefined)
            obj.unlink()
        archives.append({
            "path": f"lib/{lib.name}",
            "bytes": lib.stat().st_size,
            "sha256": sha256_bytes(lib.read_bytes()),
            "objects": objects,
            "undefined_references": sorted(undefined_refs),
        })
    return {
        "FORMAT": "f1-archive-baseline-v2",
        "PROFILE": profile,
        "ARCHIVES": archives,
        "EXTERNAL_UNDEFINED_SYMBOLS": sorted(profile_undefined - profile_defined),
    }


def standalone_compile_headers(build_dir, prefix, headers, macro_view, command_log):
    """Every installed header must compile as the sole include of a TU under
    the profile's macro view (#402 F1 standalone-compile obligation), except
    `*_impl.hpp` fragments, which are only valid after their primary header's
    declarations; each fragment must still be anchored to at least one
    standalone-compiling includer."""
    cxx = "g++"
    tu_dir = build_dir / "standalone"
    tu_dir.mkdir(parents=True, exist_ok=True)
    fragments = [h for h in headers if h.endswith("_impl.hpp")]
    regular = [h for h in headers if not h.endswith("_impl.hpp")]
    failed = []
    failed_direct = []
    compiled_ok = set()
    for rel in regular:
        tu = tu_dir / (rel.replace("/", "_") + ".cpp")
        tu.write_text(f"#include <{rel}>\nint main() {{}}\n")
        obj = tu.with_suffix(".o")
        cmd = [cxx, "-std=c++20", "-Wall", "-Wextra", f"-I{prefix}/include",
               *macro_view, "-c", tu, "-o", obj]
        command_log.append(" ".join(str(c) for c in cmd))
        if run(cmd, env=consumer_env()).returncode != 0:
            failed.append(rel)
            failed_direct.append(rel)
        else:
            compiled_ok.add(rel)
    exceptions = []
    for frag in fragments:
        needle = frag.rsplit("/", 1)[-1]
        includers = [other for other in regular
                     if needle in (prefix / "include" / other).read_text()]
        anchored = [i for i in includers if i in compiled_ok]
        exceptions.append({
            "header": frag,
            "included_by": includers,
            "included_by_standalone_ok": anchored,
        })
        if not anchored:
            failed.append(f"{frag} (unanchored _impl fragment)")
    return {
        "direct_total": len(regular),
        "direct_failed": failed_direct,
        "fragment_total": len(fragments),
        "failed": failed,
        "exceptions": exceptions,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="release", choices=["release", "debug"])
    ap.add_argument("--liburing", action="store_true")
    ap.add_argument("--uring-prefix", default="/tmp/f1-deps/uring-prefix")
    ap.add_argument("--manifest-out", required=True)
    ap.add_argument("--archive-baseline-out",
                    help="archive/object/symbol baseline JSON (required unless "
                         "--check-frozen derives it next to the frozen manifest)")
    ap.add_argument("--check-frozen", metavar="FROZEN_MANIFEST",
                    help="run the full verification, then reproduce the frozen "
                         "manifest byte-for-byte outside the volatile fields and "
                         "close the three-way baseline loop (committed frozen "
                         "file == manifest-bound sha256 == fresh baseline)")
    ap.add_argument("--profile-name", default=None,
                    help="manifest PROFILE label (derived from flags when omitted)")
    ap.add_argument("--keep-prefix", action="store_true")
    args = ap.parse_args()

    frozen_manifest = None
    frozen_data = None
    if args.check_frozen:
        frozen_manifest = Path(args.check_frozen)
        if not frozen_manifest.exists():
            sys.exit(f"frozen manifest not found: {frozen_manifest}")
        if not args.archive_baseline_out:
            sys.exit("--check-frozen needs --archive-baseline-out for the fresh baseline")
        frozen_data = json.loads(frozen_manifest.read_text())

    if not os.environ.get("TMPDIR"):
        os.environ["TMPDIR"] = "/tmp"
    profile = args.profile_name or ("P2-liburing" if args.liburing else "P0P1P3-noliburing")
    uring_prefix = Path(args.uring_prefix)
    prefix = Path(tempfile.mkdtemp(prefix="sluice-f1-prefix-"))
    build_dir = Path(tempfile.mkdtemp(prefix="sluice-f1-consumers-"))
    command_log_path = build_dir / "consumer-build-commands.txt"
    command_log = []
    results = []
    failures = []

    def gate(name, ok, detail=""):
        results.append((name, ok, detail))
        if not ok:
            failures.append(f"{name}: {detail}")
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    print(f"== F1 package verification — profile {profile} ==")
    print(f"prefix={prefix} consumer_build={build_dir}")

    # Provenance is measured before anything else can dirty the tree: the
    # production baseline is pinned to the frozen manifest under --check-frozen
    # (a frozen identity must not move with the branch tip) and discovered as
    # the merge-base with origin/master on a freeze run; the verification head
    # is the commit this run executes from, and a dirty tree means the recorded
    # head does not identify the built content. The run's own declared outputs
    # (manifest, archive baseline) are excluded from the cleanliness
    # measurement: they do not feed the build or the probes, and a freeze pass
    # over both profiles always runs with the first profile's artifacts already
    # on disk.
    production_baseline_sha = resolve_production_baseline(REPO, frozen_data)
    verification_head = git_out("rev-parse", "HEAD")
    origin_master_tip = git_out("rev-parse", "origin/master")
    declared_outputs = {str(Path(args.manifest_out).resolve()),
                        str(Path(args.archive_baseline_out).resolve())}
    dirty_paths = []
    # Raw capture: git_out strips leading whitespace, which would eat the
    # first line's status column and corrupt its path.
    status_out = run(["git", "status", "--porcelain"], capture_output=True, text=True,
                     cwd=REPO).stdout
    for line in status_out.splitlines():
        if not line:
            continue
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        if str(Path(path).resolve()) not in declared_outputs:
            dirty_paths.append(line)
    worktree_clean = not dirty_paths
    production_diff = (production_diff_paths(REPO, production_baseline_sha)
                       if production_baseline_sha else ["<no merge-base>"])
    production_diff_empty = not production_diff

    # 1. Configure + build (xmake native). The liburing switch is passed
    # EXPLICITLY both ways: an omitted flag keeps the previous cached config,
    # and a stale liburing-enabled archive would silently poison the
    # no-liburing profile.
    configure = ["xmake", "f", "-m", args.mode, "-y",
                 f"--liburing={'y' if args.liburing else 'n'}"]
    env = dict(os.environ)
    if args.liburing:
        env["CPLUS_INCLUDE_PATH"] = str(uring_prefix / "include")
        env["LIBRARY_PATH"] = str(uring_prefix / "lib")
        # The consumer binaries link the shared uring from the recorded prefix;
        # LIBRARY_PATH lets the link driver resolve -luring the same way.
        os.environ["LD_LIBRARY_PATH"] = (
            str(uring_prefix / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""))
        os.environ["LIBRARY_PATH"] = (
            str(uring_prefix / "lib") + os.pathsep + os.environ.get("LIBRARY_PATH", ""))
    else:
        for var in ("CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "LD_LIBRARY_PATH"):
            parts = [p for p in env.get(var, "").split(os.pathsep)
                     if p and not p.startswith(str(uring_prefix))]
            if parts:
                env[var] = os.pathsep.join(parts)
            else:
                env.pop(var, None)
    if run(configure, cwd=REPO, env=env).returncode != 0:
        sys.exit("xmake configure failed")
    for t in ("sluice_core", "sluice_async"):
        if run(["xmake", "-y", t], cwd=REPO, env=env).returncode != 0:
            sys.exit(f"xmake build {t} failed")

    # 2. Install into the fresh prefix (one target per install invocation).
    if run(["xmake", "install", "-o", prefix, "sluice_core"], cwd=REPO).returncode != 0:
        sys.exit("install sluice_core failed")
    if run(["xmake", "install", "-o", prefix, "sluice_async"], cwd=REPO).returncode != 0:
        sys.exit("install sluice_async failed")

    # 3. Manifest: walk the prefix, record declared usage requirements.
    headers = sorted(str(p.relative_to(prefix / "include"))
                     for p in (prefix / "include").rglob("*.hpp"))
    libs = sorted(str(p.relative_to(prefix)) for p in (prefix / "lib").iterdir())
    experimental = [h for h in headers if "experimental" in h]
    public_defines = ["SLUICE_HAS_LIBURING"] if args.liburing else []
    public_link_libs = ["uring"] if args.liburing else []
    kernel = capture(["uname", "-r"])
    arch = capture(["uname", "-m"])
    fs_source = capture(["stat", "-f", "-c", "%T", str(REPO)])
    fs_prefix = capture(["stat", "-f", "-c", "%T", str(prefix)])
    fs_build = capture(["stat", "-f", "-c", "%T", str(build_dir)])
    uring_version = ""
    if args.liburing:
        pc = uring_prefix / "lib" / "pkgconfig" / "liburing.pc"
        if pc.exists():
            for line in pc.read_text().splitlines():
                if line.startswith("Version:"):
                    uring_version = line.split(":", 1)[1].strip()
    macro_view = ["-DSLUICE_HAS_LIBURING"] if args.liburing else []

    baseline_source = ("pinned from frozen manifest" if frozen_data is not None
                       else "merge-base with origin/master")
    gate("F1_B_PROVENANCE_VERIFIED",
         bool(production_baseline_sha) and production_diff_empty and worktree_clean,
         f"(production baseline {production_baseline_sha[:12]} [{baseline_source}], "
         f"origin/master {origin_master_tip[:12]}, head {verification_head[:12]}, "
         f"production diff={[Path(p).name for p in production_diff]}, "
         f"worktree_clean={worktree_clean}, non-output dirt={dirty_paths[:3]})")

    # 3a. Standalone compile: every installed header as the sole include of a
    # TU, under the profile's macro view.
    standalone = standalone_compile_headers(build_dir, prefix, headers, macro_view,
                                            command_log)
    gate("F1_E_STANDALONE_HEADER_COMPILES", not standalone["failed"],
         f"({standalone['direct_total'] - len(standalone['direct_failed'])}/"
         f"{standalone['direct_total']} direct sole-include compiles; "
         f"{standalone['fragment_total']} anchored _impl fragments: "
         f"{[e['header'] for e in standalone['exceptions']]})")

    # 3b. Archive/object/symbol baseline.
    baseline_scratch = build_dir / "archive-baseline"
    baseline_scratch.mkdir(parents=True, exist_ok=True)
    baseline = build_archive_baseline(prefix, profile, baseline_scratch)
    baseline_path = Path(args.archive_baseline_out)
    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    baseline_path.write_text(json.dumps(baseline, indent=2) + "\n")
    baseline_sha = sha256_bytes(baseline_path.read_bytes())
    archive_object_count = sum(len(a["objects"]) for a in baseline["ARCHIVES"])
    symbol_count = sum(len(o["defined_symbols"]) for a in baseline["ARCHIVES"]
                       for o in a["objects"])
    gate("F1_E_ARCHIVE_BASELINE_RECORDED",
         len(baseline["ARCHIVES"]) == 2 and archive_object_count > 0 and symbol_count > 0,
         f"({len(baseline['ARCHIVES'])} archives, {archive_object_count} objects, "
         f"{symbol_count} defined symbols -> {baseline_path})")

    # 3c. The built archive must carry the profile's macro view: the real
    # uring backend references liburing and defines the UringConfig
    # constructor; the no-liburing archive must do neither. This catches a
    # stale cache-built archive masquerading as the other profile.
    async_archive = next((a for a in baseline["ARCHIVES"] if "async" in a["path"]), None)
    if async_archive is None:
        gate("F1_E_ARCHIVE_MATCHES_PROFILE", False, "(no async archive in baseline)")
    else:
        has_uring_refs = any("io_uring" in s for s in async_archive["undefined_references"])
        has_config_ctor = any("UringAsyncBackendC" in s and "UringConfigE" in s
                              for o in async_archive["objects"]
                              for s in o["defined_symbols"])
        expected = has_uring_refs and has_config_ctor if args.liburing \
            else not has_uring_refs and not has_config_ctor
        gate("F1_E_ARCHIVE_MATCHES_PROFILE", expected,
             f"(async archive io_uring refs={has_uring_refs}, "
             f"UringConfig ctor={has_config_ctor}, profile liburing={args.liburing})")

    manifest = {
        "PROFILE": profile,
        "PROFILE_NOTE": ("tiers are packaging labels; the F0 policy classes ride "
                         "inside them: P0/P1/P3 carry the CANONICAL surfaces, P1 "
                         "carries the OPTIONAL_CANDIDATE host closure (H-30/H-31 "
                         "rows: cancel.hpp + fiber.hpp + fiber_ctx.hpp) and the "
                         "COMPATIBILITY/INTERNAL/TEST_ONLY headers that the "
                         "current public closure reaches; no OPTIONAL_SUPPORTED "
                         "claim exists"),
        "PRODUCTION_BASELINE_SHA": production_baseline_sha,
        "PRODUCTION_DIFF_EMPTY": production_diff_empty,
        "ORIGIN_MASTER_TIP": origin_master_tip,
        "VERIFICATION_HEAD": verification_head,
        "VERIFICATION_HEAD_DIRTY": not worktree_clean,
        "GENERATED_UTC": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "COMPILER": compiler_id(),
        "ARCH": arch,
        "KERNEL": kernel,
        "FILESYSTEM_SOURCE_TREE": fs_source,
        "FILESYSTEM_SCRATCH_PREFIX": fs_prefix,
        "FILESYSTEM_CONSUMER_BUILD": fs_build,
        "BUILD_MODE": args.mode,
        "LIBURING": (str(uring_prefix) if args.liburing else "none"),
        "LIBURING_VERSION": uring_version,
        "PREFIX_LAYOUT": {"include": len(headers), "lib": libs},
        "HEADERS": headers,
        "STANDALONE_HEADER_TOTAL": len(headers),
        "STANDALONE_DIRECT_HEADER_COMPILES": (standalone["direct_total"]
                                              - len(standalone["direct_failed"])),
        "STANDALONE_FRAGMENT_EXCEPTIONS": standalone["exceptions"],
        "LIBRARIES": libs,
        "ARCHIVE_BASELINE": {
            "file": str(baseline_path),
            "sha256": baseline_sha,
            "archives": len(baseline["ARCHIVES"]),
            "objects": archive_object_count,
            "defined_symbols": symbol_count,
            "external_undefined_symbols": len(baseline["EXTERNAL_UNDEFINED_SYMBOLS"]),
        },
        "PUBLIC_DEFINES": public_defines,
        "PUBLIC_INCLUDE_PATHS": [f"<prefix>/include"],
        "PUBLIC_LINK_FLAGS": [f"-L<prefix>/lib"],
        "PUBLIC_LINK_LIBS": ["sluice_async", "sluice_core", *public_link_libs],
        "CONFIG_REQUIREMENTS": [
            "C++20",
            "thread support: no separate -pthread measured on this toolchain "
            "(glibc >= 2.34 merged libpthread); other toolchains may need "
            "-pthread for sluice_async consumers",
            "sanitizer ODR discipline: installed headers embed fiber_ctx "
            "layout by value (stackful_io_host.hpp, scheduler.hpp), so "
            "consumer TUs must be compiled with the same sanitizer "
            "configuration (ASan/TSan autodetect) as the installed archive",
            "installed headers contain SLUICE_ASYNC_INTERNAL_TESTING and "
            "SLUICE_*_MUTANT_* guarded regions; package consumers must NEVER "
            "define these macros (under liburing the combination also includes "
            "an uninstalled src/-only header and fails)",
            *([f"liburing headers/lib at {uring_prefix} (ODR: every TU must see "
               "SLUICE_HAS_LIBURING identical to the archive build)"] if args.liburing else
              ["the installed sluice_async archive was built without liburing; "
               "consumers must NOT define SLUICE_HAS_LIBURING"]),
        ],
        "EXPECTED_EXTERNAL_WORKLOADS": [
            "w01_direct", "w02_pipeline", "w03_external_loop_threadpool",
            "w03_external_loop_uring", "w04_stackful_candidate",
            "contract_request_lifetime", "contract_admission", "contract_cancel_effect",
            "contract_observer", "contract_progress", "contract_shutdown",
            "contract_durability", "contract_backend_availability", "odr_two_tu",
        ],
        "NEGATIVE_PROBES": [
            "experimental_headers_not_installed",
            "async_symbols_not_in_core_archive",
            *(["link_without_uring_fails", "odr_divergent_macro_views_detected"]
              if args.liburing else []),
        ],
    }
    for d in (prefix, build_dir):
        rd = str(Path(d).resolve())
        if rd == str(REPO) or rd.startswith(str(REPO) + os.sep):
            raise SystemExit(f"clean-room violation: scratch dir {d} is inside the repository")

    manifest_out = Path(args.manifest_out)
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"manifest written: {manifest_out}")

    gate("F1_B_PACKAGE_PREFIX_READY",
         len(headers) == 79 and len(libs) == 2,
         f"({len(headers)} headers, {len(libs)} libs; census 81 minus 2 experimental)")

    # 4. Clean-room include scan (source side of the firewall).
    scan = scan_consumer_includes()
    gate("CLEAN_ROOM_CONSUMER_INCLUDES_ANGLE_BRACKET_ONLY", not scan, str(scan[:3]))

    # 5. P0: core-only consumer — compiles, links, runs with no async archive.
    ok = compile_consumer(build_dir, prefix, [CONSUMERS / "w01_direct.cpp"],
                          build_dir / "w01_direct", [], ["-lsluice_core"], command_log)
    ran = ok and run([build_dir / "w01_direct"]).returncode == 0
    gate("P0_W01_CORE_ONLY_CLEAN_ROOM", ran,
         "compile+link+run against installed prefix; -lsluice_core only")

    # 6. P1: async consumers. Every async consumer TU sees the same macro view
    # as the archive (ODR rule, DAG X-12): the define comes from the manifest,
    # never hand-copied per consumer.
    # The external uring dependency is linked by explicit path so the audited
    # command stream is self-contained (no env LIBRARY_PATH reliance).
    uring_link = [f"-L{uring_prefix}/lib", "-luring"] if args.liburing else []
    p1_cases = [(name, f"{name.rsplit('_', 1)[0]}.cpp" if name.endswith("_threadpool")
                 else f"{name}.cpp",
                 ["threadpool"] if name.endswith("_threadpool") else [])
                for name in P1_CONSUMERS if name != "w04_stackful_candidate"]
    for name, src_name, argv in p1_cases:
        ok = compile_consumer(build_dir, prefix, [CONSUMERS / src_name],
                              build_dir / name, macro_view,
                              ["-lsluice_async", "-lsluice_core", *uring_link],
                              command_log)
        ran = ok and run([build_dir / name, *argv]).returncode == 0
        gate(f"P1_{name}", ran)

    # W-04: the suspension-shaped arms need a backend that can park a read on
    # a pipe (the threadpool backend preads and fails immediately), so the
    # probe reports the explicit unavailable outcome (exit 2) without
    # liburing and must fully pass with it — the w03 uring pattern.
    ok = compile_consumer(build_dir, prefix, [CONSUMERS / "w04_stackful_candidate.cpp"],
                          build_dir / "w04_stackful_candidate", macro_view,
                          ["-lsluice_async", "-lsluice_core", *uring_link],
                          command_log)
    if ok:
        r = run([build_dir / "w04_stackful_candidate"], capture_output=True, text=True)
        if args.liburing:
            gate("P2_W04_STRUCTURAL_STOP", r.returncode == 0,
                 f"(rc={r.returncode}: {r.stdout.strip()[:80]}{r.stderr.strip()[:80]})")
        else:
            gate("P1_W04_STRUCTURAL_ARMS_UNAVAILABLE", r.returncode == 2,
                 f"(rc={r.returncode}: {r.stdout.strip()[:80]})")
    else:
        gate("P1_W04_BUILD", False)

    # W-03 uring variant: without liburing the named-backend construction must
    # report the explicit unavailable outcome (exit code 2).
    uring_src = CONSUMERS / "w03_external_loop.cpp"
    ok = compile_consumer(build_dir, prefix, [uring_src],
                          build_dir / "w03_external_loop_uring", macro_view,
                          ["-lsluice_async", "-lsluice_core", *uring_link],
                          command_log)
    if ok:
        r = run([build_dir / "w03_external_loop_uring", "uring"], capture_output=True, text=True)
        if args.liburing:
            gate("P2_W03_URING_RUNTIME", r.returncode == 0,
                 f"(rc={r.returncode}, {'constructed' if r.returncode == 0 else r.stdout.strip()})")
        else:
            gate("P1_W03_URING_EXPLICIT_UNAVAILABLE", r.returncode == 2,
                 f"(rc={r.returncode}: {r.stdout.strip()[:80]})")
    else:
        gate("P1_W03_URING_BUILD", False)

    # 7. ODR discriminator: two TUs observe the SAME template specializations
    # (Result<std::size_t>/Request<std::size_t>) AND the layout of the
    # macro-sensitive UringAsyncBackend under one shared macro view; the
    # binary cross-checks both TUs' facts and prints the backend layout so
    # the frozen record can prove the probe is actually macro-sensitive.
    odr_bin = build_dir / "odr_two_tu"
    ok = compile_consumer(build_dir, prefix,
                          [CONSUMERS / "odr_two_tu_a.cpp", CONSUMERS / "odr_two_tu_b.cpp"],
                          odr_bin, macro_view,
                          ["-lsluice_async", "-lsluice_core", *uring_link],
                          command_log)
    odr_run = run([odr_bin], capture_output=True, text=True) if ok else None
    odr_ok = odr_run is not None and odr_run.returncode == 0
    backend_bytes = None
    if odr_ok:
        m = re.search(r"odr_backend_bytes=(\d+)", odr_run.stdout)
        backend_bytes = int(m.group(1)) if m else None
        odr_ok = backend_bytes is not None
    gate("F1_E_ODR_TWO_TU", odr_ok,
         f"(same-specialization + UringAsyncBackend layout cross-checked; "
         f"backend_bytes={backend_bytes}, macro_view={1 if args.liburing else 0})")
    manifest["ODR_DISCRIMINATOR"] = {
        "uring_backend_bytes": backend_bytes,
        "macro_view": 1 if args.liburing else 0,
        "same_specializations": ["Result<std::size_t>", "Request<std::size_t>"],
    }
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")

    # 7b. Negative divergent-macro-view probe (liburing profile): two TUs
    # report sizeof(UringAsyncBackend) under OPPOSITE SLUICE_HAS_LIBURING
    # views; the binary must observe the mismatch and fail. Deterministic
    # (compile-time constants, no scheduling): proves the discriminator can
    # actually tell the two views apart.
    if args.liburing:
        neg_dir = build_dir / "odr_divergent"
        neg_dir.mkdir(exist_ok=True)
        obj_a = neg_dir / "view_a.o"
        obj_b = neg_dir / "view_b.o"
        obj_main = neg_dir / "view_main.o"
        ok_a = compile_object(build_dir, prefix, CONSUMERS / "negative_odr_view_a.cpp",
                              obj_a, [], command_log)
        ok_b = compile_object(build_dir, prefix, CONSUMERS / "negative_odr_view_b.cpp",
                              obj_b, macro_view, command_log)
        ok_main = compile_object(build_dir, prefix, CONSUMERS / "negative_odr_view_main.cpp",
                                 obj_main, [], command_log)
        linked = False
        if ok_a and ok_b and ok_main:
            cmd = ["g++", "-std=c++20", str(obj_a), str(obj_b), str(obj_main),
                   f"-L{prefix}/lib", "-lsluice_async", "-lsluice_core", *uring_link,
                   "-o", str(neg_dir / "odr_divergent")]
            command_log.append(" ".join(cmd))
            linked = run(cmd, env=consumer_env()).returncode == 0
        r = run([neg_dir / "odr_divergent"], capture_output=True, text=True) if linked else None
        detected = (r is not None and r.returncode != 0
                    and "odr-divergent-mismatch" in r.stdout)
        detail = r.stdout.strip().splitlines()[-1] if r and r.stdout.strip() else "n/a"
        gate("NEGATIVE_ODR_DIVERGENT_VIEWS_DETECTED", detected,
             f"(rc={r.returncode if r else 'n/a'}: {detail}; divergent "
             "sizeof(UringAsyncBackend) must be observed and rejected)")

    # 8. Negative probes.
    neg_src = CONSUMERS / "negative_async_symbols_in_core.cpp"
    ok, why = expect_link_failure(build_dir, prefix, neg_src, [], command_log)
    gate("NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE", ok, why)
    gate("NEGATIVE_EXPERIMENTAL_HEADERS_NOT_INSTALLED", not experimental,
         str(experimental[:2]))
    if args.liburing:
        cxx = os.environ.get("CXX", "g++")
        obj = build_dir / "neg_uring.o"
        cmd = [cxx, "-std=c++20", f"-I{prefix}/include", "-DSLUICE_HAS_LIBURING", "-c",
               CONSUMERS / "contract_backend_availability.cpp", "-o", obj]
        command_log.append(" ".join(str(c) for c in cmd))
        compiled = run(cmd, env=consumer_env()).returncode == 0
        if compiled:
            link = run([cxx, obj, f"-L{prefix}/lib", "-lsluice_async", "-lsluice_core",
                        "-o", build_dir / "neg_uring.out"], capture_output=True)
            gate("NEGATIVE_LINK_WITHOUT_URING_FAILS", link.returncode != 0,
                 "undeclared uring link must fail under the liburing profile")
        else:
            gate("NEGATIVE_LINK_WITHOUT_URING_FAILS", False, "compile failed")

    # 9. Firewall gate over every recorded command.
    violations = check_clean_room(command_log, prefix)
    gate("CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY", not violations,
         str(violations[:2]))
    command_log_path.write_text("\n".join(command_log) + "\n")
    print(f"consumer build command log: {command_log_path}")

    # 10. Summary.
    print("\n== F1 gate summary ==")
    for name, ok, detail in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not args.keep_prefix:
        shutil.rmtree(prefix, ignore_errors=True)
    if failures:
        print(f"\nF1 package verification: {len(failures)} gate(s) FAILED")
        return 1

    if frozen_data is not None:
        frozen = frozen_data
        fresh = json.loads(manifest_out.read_text())
        frozen_baseline_ref = frozen.get("ARCHIVE_BASELINE", {})
        frozen_file = frozen_baseline_ref.get("file")
        for side in (frozen, fresh):
            side.get("ARCHIVE_BASELINE", {}).pop("file", None)
        diffs = []
        for key in sorted(set(frozen) | set(fresh)):
            if key in VOLATILE_MANIFEST_KEYS:
                continue
            if frozen.get(key) != fresh.get(key):
                diffs.append(key)
        frozen_baseline_ref = frozen.get("ARCHIVE_BASELINE", {})
        baseline_match = frozen_baseline_ref.get("sha256") == baseline_sha
        committed_path = Path(frozen_file) if frozen_file else None
        if committed_path is not None and not committed_path.is_absolute():
            committed_path = REPO / committed_path
        committed_match = (committed_path is not None and committed_path.exists()
                           and sha256_bytes(committed_path.read_bytes())
                           == frozen_baseline_ref.get("sha256"))
        if diffs or not baseline_match or not committed_match:
            print(f"\n--check-frozen FAILED: manifest key diffs={diffs}, "
                  f"fresh archive baseline match={baseline_match}, committed "
                  f"frozen baseline {frozen_file} match={committed_match}")
            return 1
        print(f"\nFROZEN_MANIFEST_REPRODUCED: {frozen_manifest} "
              f"(pinned production baseline {production_baseline_sha[:12]}, "
              f"verified at head {verification_head[:12]}; volatile fields "
              f"{sorted(VOLATILE_MANIFEST_KEYS)} excluded; committed frozen "
              f"baseline == manifest sha256 == fresh baseline "
              f"({baseline_sha[:12]} matched)")

    print("\nF1 package verification: all gates PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
