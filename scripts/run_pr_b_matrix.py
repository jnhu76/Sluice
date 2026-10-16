#!/usr/bin/env python3
"""PR B verification matrix runner.

Builds and runs the full xmake test suite in isolated build directories for
every cell of the matrix {debug, release, asanubsan, tsan} x {liburing n, y},
builds the four apps in the release cells, and records per-cell configure
logs, run logs, xmake.conf and toolchain cache with sha256 digests into the
PR B evidence matrix directory.

Usage:
  python3 scripts/run_pr_b_matrix.py --out docs/evidence/pr-b-boundary-contraction-20261016/matrix
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CELLS = [
    ("debug-n", "debug", "n", []),
    ("debug-y", "debug", "y", []),
    ("release-n", "release", "n", []),
    ("release-y", "release", "y", []),
    # gcc's libasan linker script points at the uninstalled libasan runtime
    # on this host; clang carries a static sanitizer runtime, so the
    # sanitizer cells pin the clang toolchain.
    ("asanubsan-n", "asanubsan", "n", ["--toolchain=clang"]),
    ("asanubsan-y", "asanubsan", "y", ["--toolchain=clang"]),
    ("tsan-n", "tsan", "n", ["--toolchain=clang"]),
    ("tsan-y", "tsan", "y", ["--toolchain=clang"]),
]

APPS = ["sluice-copy", "sluice-hash", "sluice-grep", "sluice-tail"]

ANSI_RE = None


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def run(cmd, log_path, cwd):
    print(f"+ {' '.join(str(c) for c in cmd)}", flush=True)
    with open(log_path, "wb") as log:
        r = subprocess.run([str(c) for c in cmd], cwd=str(cwd), stdout=log,
                           stderr=subprocess.STDOUT)
    return r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-tsan", action="store_true",
                    help="record tsan cells as configure-only (unsupported "
                         "toolchain environments)")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    matrix = []
    failures = []

    for name, mode, liburing, extra in CELLS:
        builddir = Path(f"/tmp/pr-b-build-{name}")
        record = {"profile": name, "mode": mode, "liburing": liburing,
                  "builddir": str(builddir)}

        configure_cmd = ["xmake", "f", "-m", mode, f"--liburing={liburing}",
                         "--ccache=n", *extra, "-o", builddir, "-y"]
        rc = run(configure_cmd, out / f"{name}-configure.log", REPO)
        record["configure"] = {
            "argv": [str(c) for c in configure_cmd],
            "exit_code": rc,
            "log": f"{name}-configure.log",
            "log_sha256": sha256_bytes((out / f"{name}-configure.log").read_bytes()),
        }
        shutil.copy(REPO / ".xmake" / "linux" / "x86_64" / "xmake.conf",
                    out / f"{name}-xmake.conf")
        if rc != 0:
            record["error"] = "configure failed"
            matrix.append(record)
            failures.append(name)
            continue

        build_cmd = ["xmake", "build", "-y", "-g", "test"]
        rc = run(build_cmd, out / f"{name}-build.log", REPO)
        record["build"] = {"argv": [str(c) for c in build_cmd], "exit_code": rc}
        if rc != 0:
            record["error"] = "test-group build failed"
            matrix.append(record)
            failures.append(name)
            continue

        test_cmd = ["xmake", "test", "-y"]
        rc = run(test_cmd, out / f"{name}.log", REPO)
        import re
        log_text = re.sub(r"\x1b\[[0-9;]*m", "",
                          (out / f"{name}.log").read_text(errors="replace"))
        m = re.search(r"tests passed,\s*(\d+)\s*test\(s\) failed out of\s*(\d+)",
                      log_text)
        if m:
            failed = int(m.group(1))
            tests = int(m.group(2))
        else:
            m2 = re.search(r"(\d+)%\s*tests? passed.*out of\s*(\d+)", log_text)
            tests = int(m2.group(2)) if m2 else None
            failed = None if tests is None else rc
        record["test"] = {
            "argv": [str(c) for c in test_cmd],
            "exit_code": rc,
            "tests": tests,
            "failed": failed,
            "log": f"{name}.log",
            "log_sha256": sha256_bytes((out / f"{name}.log").read_bytes()),
        }
        if rc != 0 or (failed or 0) != 0:
            failures.append(name)

        if mode == "release":
            apps_rc = {}
            for app in APPS:
                arc = run(["xmake", "build", "-y", app], out / f"{name}-app-{app}.log",
                          REPO)
                apps_rc[app] = arc
                if arc != 0:
                    failures.append(f"{name}:{app}")
            record["apps"] = apps_rc

        matrix.append(record)

    (out / "matrix.json").write_text(json.dumps(matrix, indent=2) + "\n")
    print("\n== PR B matrix summary ==")
    for cell in matrix:
        line = f"  {cell['profile']}: "
        if "error" in cell:
            line += cell["error"]
        else:
            t = cell["test"]
            line += f"tests={t['tests']} failed={t['failed']} rc={t['exit_code']}"
            if "apps" in cell:
                line += " apps=" + ("ok" if all(v == 0 for v in cell["apps"].values())
                                    else str(cell["apps"]))
        print(line)
    if failures:
        print(f"\nmatrix failures: {failures}")
        return 1
    print("\nmatrix: all cells green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
