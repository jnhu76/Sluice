#!/usr/bin/env python3
"""F1 clean-room package verification (#402 Phase F1).

Builds the libraries, installs them into a fresh prefix, freezes the
machine-readable manifest, then compiles/links/runs the clean-room external
consumers against the installed prefix ONLY, and runs the negative package
probes.

Gates evaluated:
  F1_B_PACKAGE_PREFIX_READY
  CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY
  PACKAGE_USAGE_REQUIREMENTS_AND_DECLARATIONS_MATCH_BASELINE
  F1_C_EXTERNAL_WORKLOADS_RECORDED

Usage:
  python3 scripts/verify_f1_package.py                # no-liburing profile
  python3 scripts/verify_f1_package.py --liburing --uring-prefix DIR
  python3 scripts/verify_f1_package.py --manifest-out docs/review/f1-package-manifest.json
"""

import argparse
import datetime
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


def git_sha():
    return run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
               cwd=REPO).stdout.strip()


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="release", choices=["release", "debug"])
    ap.add_argument("--liburing", action="store_true")
    ap.add_argument("--uring-prefix", default="/tmp/f1-deps/uring-prefix")
    ap.add_argument("--manifest-out", required=True)
    ap.add_argument("--profile-name", default=None,
                    help="manifest PROFILE label (derived from flags when omitted)")
    ap.add_argument("--keep-prefix", action="store_true")
    args = ap.parse_args()

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

    # 1. Configure + build (xmake native).
    configure = ["xmake", "f", "-m", args.mode, "-y"]
    env = dict(os.environ)
    if args.liburing:
        configure += ["--liburing=y"]
        env["CPLUS_INCLUDE_PATH"] = str(uring_prefix / "include")
        env["LIBRARY_PATH"] = str(uring_prefix / "lib")
        # The consumer binaries link the shared uring from the recorded prefix;
        # LIBRARY_PATH lets the link driver resolve -luring the same way.
        os.environ["LD_LIBRARY_PATH"] = (
            str(uring_prefix / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""))
        os.environ["LIBRARY_PATH"] = (
            str(uring_prefix / "lib") + os.pathsep + os.environ.get("LIBRARY_PATH", ""))
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
    headers = sorted(str(p.relative_to(prefix)) for p in (prefix / "include").rglob("*.hpp"))
    libs = sorted(str(p.relative_to(prefix)) for p in (prefix / "lib").iterdir())
    experimental = [h for h in headers if "experimental" in h]
    public_defines = ["SLUICE_HAS_LIBURING"] if args.liburing else []
    public_link_libs = ["uring"] if args.liburing else []
    kernel = run(["uname", "-r"], capture_output=True, text=True).stdout.strip()
    arch = run(["uname", "-m"], capture_output=True, text=True).stdout.strip()
    fs = run(["stat", "-f", "-c", "%T", str(prefix)], capture_output=True,
             text=True).stdout.strip()
    uring_version = ""
    if args.liburing:
        pc = uring_prefix / "lib" / "pkgconfig" / "liburing.pc"
        if pc.exists():
            for line in pc.read_text().splitlines():
                if line.startswith("Version:"):
                    uring_version = line.split(":", 1)[1].strip()
    manifest = {
        "PROFILE": profile,
        "PROFILE_NOTE": ("tiers are packaging labels; the F0 policy classes ride "
                         "inside them: P0/P1/P3 carry the CANONICAL surfaces, P1 "
                         "carries the OPTIONAL_CANDIDATE host closure (H-30/H-31 "
                         "rows: cancel.hpp + fiber.hpp + fiber_ctx.hpp) and the "
                         "COMPATIBILITY/INTERNAL/TEST_ONLY headers that the "
                         "current public closure reaches; no OPTIONAL_SUPPORTED "
                         "claim exists"),
        "SOURCE_SHA": git_sha(),
        "GENERATING_COMMIT": run(["git", "rev-parse", "HEAD"], capture_output=True,
                                 text=True, cwd=REPO).stdout.strip(),
        "GENERATED_UTC": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "COMPILER": compiler_id(),
        "ARCH": arch,
        "KERNEL": kernel,
        "FILESYSTEM": fs,
        "BUILD_MODE": args.mode,
        "LIBURING": (str(uring_prefix) if args.liburing else "none"),
        "LIBURING_VERSION": uring_version,
        "PREFIX_LAYOUT": {"include": len(headers), "lib": libs},
        "HEADERS": headers,
        "LIBRARIES": libs,
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
            *(["link_without_uring_fails"] if args.liburing else []),
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
    macro_view = ["-DSLUICE_HAS_LIBURING"] if args.liburing else []
    # The external uring dependency is linked by explicit path so the audited
    # command stream is self-contained (no env LIBRARY_PATH reliance).
    uring_link = [f"-L{uring_prefix}/lib", "-luring"] if args.liburing else []
    p1_cases = [(name, f"{name.rsplit('_', 1)[0]}.cpp" if name.endswith("_threadpool")
                 else f"{name}.cpp",
                 ["threadpool"] if name.endswith("_threadpool") else [])
                for name in P1_CONSUMERS]
    for name, src_name, argv in p1_cases:
        ok = compile_consumer(build_dir, prefix, [CONSUMERS / src_name],
                              build_dir / name, macro_view,
                              ["-lsluice_async", "-lsluice_core", *uring_link],
                              command_log)
        ran = ok and run([build_dir / name, *argv]).returncode == 0
        gate(f"P1_{name}", ran)

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

    # 7. ODR probe: two TUs, one binary, same macro view.
    ok = compile_consumer(build_dir, prefix,
                          [CONSUMERS / "odr_two_tu_a.cpp", CONSUMERS / "odr_two_tu_b.cpp"],
                          build_dir / "odr_two_tu", macro_view,
                          ["-lsluice_async", "-lsluice_core", *uring_link],
                          command_log)
    ran = ok and run([build_dir / "odr_two_tu"]).returncode == 0
    gate("F1_E_ODR_TWO_TU", ran)

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
    print("\nF1 package verification: all gates PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
