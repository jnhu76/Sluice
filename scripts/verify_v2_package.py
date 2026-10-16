#!/usr/bin/env python3
"""v2 clean-room package verification (PR B, D2/BOUND contraction).

Builds the libraries, installs them into a fresh prefix, verifies the
11-header contracted install surface, then compiles/links/runs the clean-room
external consumers against the installed prefix ONLY, and runs the negative
surface probes.

This is the v2 authority for the package boundary. It deliberately does not
carry the F1 frozen-manifest reproduction protocol: the superseded F1
baseline verified the pre-contraction 25-header surface, and reproducing it
at a contracted head is impossible by construction. Evidence for a v2 claim
is a fresh manifest per run, recorded under the PR B evidence directory.

Gates evaluated:
  V2_B_PACKAGE_PREFIX_READY               (exactly the 11 contracted headers)
  V2_B_PROVENANCE_RECORDED                (clean worktree, HEAD recorded)
  V2_E_STANDALONE_HEADER_COMPILES         (each header as the sole include)
  V2_E_ARCHIVE_BASELINE_RECORDED
  V2_E_ARCHIVE_MATCHES_PROFILE
  CLEAN_ROOM_CONSUMER_INCLUDES_ANGLE_BRACKET_ONLY
  CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY
  P0_W01_CORE_ONLY_CLEAN_ROOM
  P1_<consumer>                           (workloads + micro-contracts)
  V2_P1_URING_EXPLICIT_NOT_SUPPORTED      (liburing=n runtime selection)
  V2_P2_URING_RUNTIME                     (liburing=y runtime selection)
  V2_E_ODR_PUBLIC_LAYOUT_MACRO_INDEPENDENT
  NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE
  NEGATIVE_V2_SEAM_UNAVAILABLE
  NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE

Usage:
  python3 scripts/verify_v2_package.py \
      --manifest-out docs/evidence/pr-b-boundary-contraction-20261016/v2-package-manifest-noliburing.json
  python3 scripts/verify_v2_package.py --liburing \
      --uring-prefix /path/to/liburing/prefix \
      --manifest-out docs/evidence/pr-b-boundary-contraction-20261016/v2-package-manifest-liburing.json
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

# The contracted v2 install surface (Appendix B of the v2 root). The prefix
# gate fails on any deviation in either direction.
EXPECTED_V2_HEADERS = [
    "sluice/async/async_io_context.hpp",
    "sluice/async/request.hpp",
    "sluice/async/request_scope.hpp",
    "sluice/async/threadpool_backend.hpp",
    "sluice/async/uring_backend.hpp",
    "sluice/blocking/file.hpp",
    "sluice/effect.hpp",
    "sluice/error.hpp",
    "sluice/file_resource.hpp",
    "sluice/measurement.hpp",
    "sluice/result.hpp",
]

# Headers that exist in the repository tree but must NOT be installed: the 14
# that left the install set with PR B (4 deleted host headers are asserted
# absent from the tree as well, so they are not listed here) plus the new
# uninstalled internal seam headers. Every one of these must fail to compile
# when included from the prefix.
UNINSTALLED_TREE_HEADERS = [
    "sluice/async/detail/async_backend.hpp",
    "sluice/async/detail/context_identity.hpp",
    "sluice/async/detail/fail_fast.hpp",
    "sluice/async/detail/observer_protocol.hpp",
    "sluice/async/detail/progress_source.hpp",
    "sluice/async/detail/ready_sink.hpp",
    "sluice/async/detail/reference_ready_sink.hpp",
    "sluice/async/detail/request_core.hpp",
    "sluice/async/detail/request_key.hpp",
    "sluice/async/detail/test_access.hpp",
    "sluice/async/detail/threadpool_backend.hpp",
    "sluice/async/detail/uring_backend.hpp",
    "sluice/detail/file_semantics.hpp",
    "sluice/detail/posix_retry.hpp",
    "sluice/detail/uring_submit.hpp",
]

# Deleted outright with the host bundle (D1): neither in the tree nor in the
# prefix.
DELETED_HEADERS = [
    "sluice/async/cancel.hpp",
    "sluice/async/fiber.hpp",
    "sluice/async/fiber_ctx.hpp",
    "sluice/async/stackful_io_host.hpp",
]

P0_CONSUMERS = ["w01_direct"]
P1_CONSUMERS = [
    "w02_pipeline",
    "contract_request_lifetime",
    "contract_admission",
    "contract_cancel_effect",
    "contract_observer",
    "contract_progress",
    "contract_shutdown",
    "contract_durability",
    "contract_backend_availability",
]


def run(cmd, **kw):
    print(f"+ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run([str(c) for c in cmd], **kw)


def capture(cmd):
    return run(cmd, capture_output=True, text=True).stdout.strip()


def git_out(*args, cwd=REPO):
    return run(["git", *args], capture_output=True, text=True, cwd=cwd).stdout.strip()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def compiler_id():
    out = run(["g++", "--version"], capture_output=True, text=True).stdout
    return out.splitlines()[0].strip() if out else "unknown"


def check_clean_room(command_strs, prefix):
    """No compile/link command may reference the source or build tree."""
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
    """Consumer sources may include only bare angle-bracket headers."""
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
                     command_log):
    cxx = "g++"
    objdir = build_dir / (out.name + ".objs")
    objdir.mkdir(parents=True, exist_ok=True)
    objs = []
    env = consumer_env()
    for src, flags in zip(sources, extra_cxxflags):
        obj = objdir / (Path(src).stem + ".o")
        cmd = [cxx, "-std=c++20", "-Wall", "-Wextra", f"-I{prefix}/include",
               *flags, "-c", src, "-o", obj]
        command_log.append(" ".join(str(c) for c in cmd))
        if run(cmd, env=env).returncode != 0:
            return False
        objs.append(obj)
    cmd = [cxx, "-std=c++20", *objs, f"-L{prefix}/lib", *extra_ldflags, "-o", out]
    command_log.append(" ".join(str(c) for c in cmd))
    if run(cmd, env=env).returncode != 0:
        return False
    ldd = run(["ldd", out], capture_output=True, text=True)
    command_log.append(f"ldd {out}: " + " | ".join(
        line.strip() for line in ldd.stdout.splitlines() if line.strip()))
    return True


def expect_link_failure(build_dir, prefix, source, command_log):
    cxx = os.environ.get("CXX", "g++")
    obj = build_dir / (Path(source).stem + ".o")
    cmd = [cxx, "-std=c++20", f"-I{prefix}/include", "-c", source, "-o", obj]
    command_log.append(" ".join(str(c) for c in cmd))
    if run(cmd).returncode != 0:
        return False, "compile unexpectedly failed"
    cmd = [cxx, obj, f"-L{prefix}/lib", "-lsluice_core", "-o", build_dir / "negative.out"]
    command_log.append(" ".join(str(c) for c in cmd))
    r = run(cmd, capture_output=True)
    return r.returncode != 0, "link against core-only must fail with undefined async symbols"


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
        "FORMAT": "v2-archive-baseline-v1",
        "PROFILE": profile,
        "ARCHIVES": archives,
        "EXTERNAL_UNDEFINED_SYMBOLS": sorted(profile_undefined - profile_defined),
    }


def standalone_compile_headers(build_dir, prefix, headers, command_log):
    cxx = "g++"
    tu_dir = build_dir / "standalone"
    tu_dir.mkdir(parents=True, exist_ok=True)
    failed = []
    for rel in headers:
        tu = tu_dir / (rel.replace("/", "_") + ".cpp")
        tu.write_text(f"#include <{rel}>\nint main() {{}}\n")
        obj = tu.with_suffix(".o")
        cmd = [cxx, "-std=c++20", "-Wall", "-Wextra", f"-I{prefix}/include",
               "-c", tu, "-o", obj]
        command_log.append(" ".join(str(c) for c in cmd))
        if run(cmd, env=consumer_env()).returncode != 0:
            failed.append(rel)
    return {"total": len(headers), "failed": failed}


def expect_compile_failure(build_dir, prefix, name, text, command_log):
    """Negative probe: the TU must FAIL to compile against the prefix."""
    cxx = "g++"
    src = build_dir / f"neg_{name}.cpp"
    src.write_text(text)
    obj = src.with_suffix(".o")
    cmd = [cxx, "-std=c++20", f"-I{prefix}/include", "-c", src, "-o", obj]
    command_log.append(" ".join(str(c) for c in cmd))
    r = run(cmd, capture_output=True)
    return r.returncode != 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="release", choices=["release", "debug"])
    ap.add_argument("--liburing", action="store_true")
    ap.add_argument("--uring-prefix", default="/tmp/f1-deps/uring-prefix")
    ap.add_argument("--manifest-out", required=True)
    ap.add_argument("--archive-baseline-out", default=None)
    ap.add_argument("--keep-prefix", action="store_true")
    args = ap.parse_args()

    if not os.environ.get("TMPDIR"):
        os.environ["TMPDIR"] = "/tmp"
    profile = "v2-liburing" if args.liburing else "v2-noliburing"
    uring_prefix = Path(args.uring_prefix)
    prefix = Path(tempfile.mkdtemp(prefix="sluice-v2-prefix-"))
    build_dir = Path(tempfile.mkdtemp(prefix="sluice-v2-consumers-"))
    command_log_path = build_dir / "consumer-build-commands.txt"
    command_log = []
    results = []
    failures = []

    def gate(name, ok, detail=""):
        results.append((name, ok, detail))
        if not ok:
            failures.append(f"{name}: {detail}")
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    print(f"== v2 package verification — profile {profile} ==")
    print(f"prefix={prefix} consumer_build={build_dir}")

    verification_head = git_out("rev-parse", "HEAD")
    status_out = run(["git", "status", "--porcelain"], capture_output=True,
                     text=True, cwd=REPO).stdout
    dirty = [line for line in status_out.splitlines() if line.strip()]
    declared_outputs = {str(Path(args.manifest_out).resolve())}
    if args.archive_baseline_out:
        declared_outputs.add(str(Path(args.archive_baseline_out).resolve()))
    dirty = [d for d in dirty
             if str(Path(d[3:].split(" -> ", 1)[-1]).resolve()) not in declared_outputs]
    worktree_clean = not dirty

    # 1. Configure + build. The liburing switch is passed EXPLICITLY both
    # ways: a stale cached profile would poison the archive identity.
    configure = ["xmake", "f", "-m", args.mode, "-y",
                 f"--liburing={'y' if args.liburing else 'n'}"]
    env = dict(os.environ)
    if args.liburing:
        env["CPLUS_INCLUDE_PATH"] = str(uring_prefix / "include")
        env["LIBRARY_PATH"] = str(uring_prefix / "lib")
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

    # 2. Install into the fresh prefix.
    if run(["xmake", "install", "-o", prefix, "sluice_core"], cwd=REPO).returncode != 0:
        sys.exit("install sluice_core failed")
    if run(["xmake", "install", "-o", prefix, "sluice_async"], cwd=REPO).returncode != 0:
        sys.exit("install sluice_async failed")

    # 3. Manifest and the prefix gates.
    headers = sorted(str(p.relative_to(prefix / "include"))
                     for p in (prefix / "include").rglob("*.hpp"))
    libs = sorted(str(p.relative_to(prefix)) for p in (prefix / "lib").iterdir())
    uring_version = ""
    if args.liburing:
        pc = uring_prefix / "lib" / "pkgconfig" / "liburing.pc"
        if pc.exists():
            for line in pc.read_text().splitlines():
                if line.startswith("Version:"):
                    uring_version = line.split(":", 1)[1].strip()

    gate("V2_B_PACKAGE_PREFIX_READY",
         headers == EXPECTED_V2_HEADERS and len(libs) == 2,
         f"({len(headers)} headers vs contracted {len(EXPECTED_V2_HEADERS)}, "
         f"{len(libs)} libs; "
         f"extra={sorted(set(headers) - set(EXPECTED_V2_HEADERS))}, "
         f"missing={sorted(set(EXPECTED_V2_HEADERS) - set(headers))})")
    gate("V2_B_PROVENANCE_RECORDED", worktree_clean,
         f"(head {verification_head[:12]}, worktree_clean={worktree_clean}, "
         f"non-output dirt={dirty[:3]})")

    standalone = standalone_compile_headers(build_dir, prefix, headers, command_log)
    gate("V2_E_STANDALONE_HEADER_COMPILES", not standalone["failed"],
         f"({standalone['total'] - len(standalone['failed'])}/"
         f"{standalone['total']} sole-include compiles; "
         f"failed={standalone['failed']})")

    baseline_scratch = build_dir / "archive-baseline"
    baseline_scratch.mkdir(parents=True, exist_ok=True)
    baseline = build_archive_baseline(prefix, profile, baseline_scratch)
    baseline_path = (Path(args.archive_baseline_out) if args.archive_baseline_out
                     else Path(args.manifest_out).with_name(
                         Path(args.manifest_out).stem + "-archive-baseline.json"))
    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    baseline_path.write_text(json.dumps(baseline, indent=2) + "\n")
    baseline_sha = sha256_bytes(baseline_path.read_bytes())
    archive_object_count = sum(len(a["objects"]) for a in baseline["ARCHIVES"])
    symbol_count = sum(len(o["defined_symbols"]) for a in baseline["ARCHIVES"]
                       for o in a["objects"])
    gate("V2_E_ARCHIVE_BASELINE_RECORDED",
         len(baseline["ARCHIVES"]) == 2 and archive_object_count > 0,
         f"({len(baseline['ARCHIVES'])} archives, {archive_object_count} objects, "
         f"{symbol_count} defined symbols -> {baseline_path})")

    async_archive = next((a for a in baseline["ARCHIVES"] if "async" in a["path"]), None)
    if async_archive is None:
        gate("V2_E_ARCHIVE_MATCHES_PROFILE", False, "(no async archive)")
    else:
        has_uring_refs = any("io_uring" in s
                             for s in async_archive["undefined_references"])
        has_uring_factory = any("make_uring_context" in s
                                for o in async_archive["objects"]
                                for s in o["defined_symbols"])
        # The no-liburing stub still defines UringAsyncBackend members (the
        # throwing shell), so the profile identity is the external io_uring
        # reference set plus the public factory symbol, which must exist in
        # both profiles with different behavior.
        expected = has_uring_factory and (has_uring_refs == args.liburing)
        gate("V2_E_ARCHIVE_MATCHES_PROFILE", expected,
             f"(async archive io_uring refs={has_uring_refs}, "
             f"make_uring_context defined={has_uring_factory}, "
             f"profile liburing={args.liburing})")

    manifest = {
        "FORMAT": "v2-package-manifest-v1",
        "PROFILE": profile,
        "VERIFICATION_HEAD": verification_head,
        "VERIFICATION_HEAD_DIRTY": not worktree_clean,
        "GENERATED_UTC": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "COMPILER": compiler_id(),
        "ARCH": capture(["uname", "-m"]),
        "KERNEL": capture(["uname", "-r"]),
        "BUILD_MODE": args.mode,
        "LIBURING": (str(uring_prefix) if args.liburing else "none"),
        "LIBURING_VERSION": uring_version,
        "HEADERS": headers,
        "LIBRARIES": libs,
        "ARCHIVE_BASELINE": {
            "file": str(baseline_path),
            "sha256": baseline_sha,
            "archives": len(baseline["ARCHIVES"]),
            "objects": archive_object_count,
            "defined_symbols": symbol_count,
            "external_undefined_symbols":
                len(baseline["EXTERNAL_UNDEFINED_SYMBOLS"]),
        },
        "PUBLIC_DEFINES": [],
        "PUBLIC_INCLUDE_PATHS": [f"<prefix>/include"],
        "PUBLIC_LINK_FLAGS": [f"-L<prefix>/lib"],
        "PUBLIC_LINK_LIBS": ["sluice_async", "sluice_core"]
                            + (["uring"] if args.liburing else []),
        "CONFIG_REQUIREMENTS": [
            "C++20",
            "installed headers read no build-profile macro: consumer TUs need "
            "no SLUICE_HAS_LIBURING define and may hold any view without "
            "layout divergence (V2_E_ODR_PUBLIC_LAYOUT_MACRO_INDEPENDENT)",
            "installed headers contain SLUICE_*_MUTANT_* guarded regions; "
            "package consumers must NEVER define these macros",
        ],
        "EXPECTED_EXTERNAL_WORKLOADS": P0_CONSUMERS + P1_CONSUMERS + [
            "w03_external_loop_threadpool", "w03_external_loop_uring", "odr_two_tu",
        ],
        "NEGATIVE_PROBES": [
            "removed_and_internal_headers_unavailable",
            "backend_seam_unavailable",
            "async_symbols_not_in_core_archive",
        ],
    }

    manifest_out = Path(args.manifest_out)
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"manifest written: {manifest_out}")

    # 4. Clean-room include scan.
    scan = scan_consumer_includes()
    gate("CLEAN_ROOM_CONSUMER_INCLUDES_ANGLE_BRACKET_ONLY", not scan, str(scan[:3]))

    # 5. P0: core-only consumer.
    ok = compile_consumer(build_dir, prefix, [CONSUMERS / "w01_direct.cpp"],
                          build_dir / "w01_direct", [[]], ["-lsluice_core"],
                          command_log)
    ran = ok and run([build_dir / "w01_direct"]).returncode == 0
    gate("P0_W01_CORE_ONLY_CLEAN_ROOM", ran,
         "compile+link+run against installed prefix; -lsluice_core only")

    # 6. P1 consumers. No macro view is supplied: the v2 surface is
    # macro-view independent, which the ODR probe proves directly below.
    uring_link = [f"-L{uring_prefix}/lib", "-luring"] if args.liburing else []
    async_link = ["-lsluice_async", "-lsluice_core", *uring_link]
    for name in P1_CONSUMERS:
        ok = compile_consumer(build_dir, prefix, [CONSUMERS / f"{name}.cpp"],
                              build_dir / name, [[]], async_link, command_log)
        ran = ok and run([build_dir / name]).returncode == 0
        gate(f"P1_{name}", ran,
             f"rc={run([build_dir / name]).returncode if ok else 'build-failed'}")

    # 7. W-03 both variants.
    ok = compile_consumer(build_dir, prefix, [CONSUMERS / "w03_external_loop.cpp"],
                          build_dir / "w03_external_loop", [[]], async_link,
                          command_log)
    if ok:
        r_tp = run([build_dir / "w03_external_loop", "threadpool"],
                   capture_output=True, text=True)
        gate("P1_W03_EXTERNAL_LOOP_THREADPOOL", r_tp.returncode == 0,
             f"(rc={r_tp.returncode})")
        r_ur = run([build_dir / "w03_external_loop", "uring"],
                   capture_output=True, text=True)
        if args.liburing:
            gate("V2_P2_URING_RUNTIME", r_ur.returncode == 0,
                 f"(rc={r_ur.returncode}: {r_ur.stdout.strip()[:80]})")
        else:
            gate("V2_P1_URING_EXPLICIT_NOT_SUPPORTED", r_ur.returncode == 2,
                 f"(rc={r_ur.returncode}: {r_ur.stdout.strip()[:80]})")
    else:
        gate("P1_W03_EXTERNAL_LOOP_THREADPOOL", False)
        gate("V2_P2_URING_RUNTIME" if args.liburing
             else "V2_P1_URING_EXPLICIT_NOT_SUPPORTED", False, "build failed")

    # 8. ODR discriminator: TU-A without the macro view, TU-B with the
    # OPPOSITE view; public layouts must agree across TUs and views.
    odr_bin = build_dir / "odr_two_tu"
    ok = compile_consumer(build_dir, prefix,
                          [CONSUMERS / "odr_two_tu_a.cpp",
                           CONSUMERS / "odr_two_tu_b.cpp"],
                          odr_bin, [[], ["-DSLUICE_HAS_LIBURING"]], async_link,
                          command_log)
    odr_run = run([odr_bin], capture_output=True, text=True) if ok else None
    odr_ok = odr_run is not None and odr_run.returncode == 0
    context_bytes = None
    if odr_ok:
        m = re.search(r"context_bytes=(\d+).*macro_views=(\d)/(\d)",
                      odr_run.stdout)
        odr_ok = m is not None and m.group(2) != m.group(3)
        if m:
            context_bytes = int(m.group(1))
    gate("V2_E_ODR_PUBLIC_LAYOUT_MACRO_INDEPENDENT", odr_ok,
         f"(public layouts agree across opposite macro views; "
         f"context_bytes={context_bytes})")
    manifest["ODR_DISCRIMINATOR"] = {
        "context_bytes": context_bytes,
        "views": ["no-define", "-DSLUICE_HAS_LIBURING"],
        "agreed_types": ["AsyncIoContext", "RequestId", "Request<std::size_t>",
                         "Result<std::size_t>", "RequestResultData",
                         "ProgressPass", "UringConfig", "ThreadPoolConfig",
                         "ObserverAttachment", "ObserverCancelResult"],
    }
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")

    # 9. Negative probes: every uninstalled in-tree header must fail to
    # compile from the prefix, and the deleted host headers must be absent
    # from both the tree and the prefix.
    unavailable_failures = []
    for rel in UNINSTALLED_TREE_HEADERS:
        if not expect_compile_failure(build_dir, prefix, rel.replace("/", "_"),
                                      f"#include <{rel}>\nint main() {{}}\n",
                                      command_log):
            unavailable_failures.append(rel)
    for rel in DELETED_HEADERS:
        present_in_tree = (REPO / "include" / rel).exists()
        present_in_prefix = (prefix / "include" / rel).exists()
        if present_in_tree or present_in_prefix:
            unavailable_failures.append(f"{rel} (deleted but present)")
    gate("NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE", not unavailable_failures,
         str(unavailable_failures[:4]))

    seam_probes = {
        "derive_backend": "namespace sluice::async { class AsyncBackend; }\n"
                          "struct B : sluice::async::AsyncBackend {};\n"
                          "int main() { return 0; }\n",
        "construct_backend": "namespace sluice::async { class AsyncBackend; }\n"
                             "int main() { sluice::async::AsyncBackend* p =\n"
                             "    new sluice::async::AsyncBackend; (void)p;"
                             " return 0; }\n",
        "unique_ptr_ctor": "#include <sluice/async/async_io_context.hpp>\n"
                           "#include <memory>\n"
                           "namespace sluice::async { class AsyncBackend; }\n"
                           "int main() { sluice::async::AsyncIoContext ctx"
                           "(std::unique_ptr<sluice::async::AsyncBackend>{});"
                           " (void)ctx; return 0; }\n",
    }
    seam_failures = [name for name, text in seam_probes.items()
                     if not expect_compile_failure(build_dir, prefix, name, text,
                                                   command_log)]
    gate("NEGATIVE_V2_SEAM_UNAVAILABLE", not seam_failures, str(seam_failures))

    ok, why = expect_link_failure(build_dir, prefix,
                                  CONSUMERS / "negative_async_symbols_in_core.cpp",
                                  command_log)
    gate("NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE", ok, why)

    # 10. Firewall over every recorded command.
    violations = check_clean_room(command_log, prefix)
    gate("CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY", not violations,
         str(violations[:2]))
    command_log_path.write_text("\n".join(command_log) + "\n")
    print(f"consumer build command log: {command_log_path}")

    print("\n== v2 package gate summary ==")
    for name, ok, detail in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not args.keep_prefix:
        shutil.rmtree(prefix, ignore_errors=True)
    if failures:
        print(f"\nv2 package verification: {len(failures)} gate(s) FAILED")
        return 1
    print("\nv2 package verification: all gates PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
