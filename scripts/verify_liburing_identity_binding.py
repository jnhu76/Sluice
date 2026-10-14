#!/usr/bin/env python3
"""#520/F-10 regression: probe and production must share one liburing identity.

The C++-effective resolution the production targets get is fixed by the
toolchain itself: for #include <liburing.h> gcc/clang search command-line -I
directories first, then CPATH, then CPLUS_INCLUDE_PATH, then the default
directories (C_INCLUDE_PATH is not consulted for C++ at all), and for
-luring the driver searches its configured directories and command-line -L
before LIBRARY_PATH.

This regression builds an environment where a candidate-list probe and the
toolchain disagree, and runs the real configure entry point against it:

- C_INCLUDE_PATH: a full, working copy of the resolved system liburing
  headers -- invisible to the C++ compile, visible to a candidate scan that
  honors C_INCLUDE_PATH,
- LIBRARY_PATH: a copy of the resolved system liburing shared object --
  checked last by the driver, first by the same candidate scan.

The contract under test: what `xmake f --liburing=y` accepts and prints must
be exactly the pair the toolchain itself resolves (compared by realpath).
It must also refuse, at the declaration layer, a CPATH-installed header
missing a required declaration, and it must print the resolved pair in a
clean environment.

The recorded fixture bytes of the committed GREEN run live in
docs/evidence/pre-f6-infra-stabilization-20261014/f10-correction/green/
identity-regression/fixtures.tar.gz (per-file hashes in SHA256SUMS). This
script never reads them: it rebuilds its own fixtures from the toolchain's
traced headers into --output on every run.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

XMAKE = os.environ.get("SLUICE_XMAKE", "/home/jnhu/.local/bin/xmake")
OK_LINE = re.compile(r"checking for liburing capability \.\.\. ok \((.*?) \+ (.*?)\)")
GET_PROBE_DECL = re.compile(
    r"struct io_uring_probe \*io_uring_get_probe_ring\(struct io_uring \*ring\)\s*\n\s*LIBURING_NOEXCEPT;\n")


def run(argv, log_handle, cwd, env=None):
    log_handle.write("$ " + " ".join(argv) + "\n")
    log_handle.flush()
    completed = subprocess.run(argv, stdout=log_handle, stderr=subprocess.STDOUT,
                               cwd=str(cwd), env=env, check=False)
    return completed.returncode


def trace_header(cxx, include_dir=None, env=None):
    """The liburing.h file the compiler itself opens for #include <liburing.h>."""
    argv = [cxx, "-E", "-H", "-x", "c++"]
    if include_dir is not None:
        argv += ["-I", include_dir]
    argv.append("-")
    completed = subprocess.run(argv, input="#include <liburing.h>\nint sluice_trace;\n",
                               capture_output=True, text=True, env=env, check=False)
    match = re.search(r"^\.\s+(\S*liburing\.h)$", completed.stderr, re.MULTILINE)
    return match.group(1) if match else None


def trace_library(cxx, library_dir=None, env=None):
    """The liburing file the linker itself opens for -luring."""
    argv = [cxx, "-x", "c++", "-"]
    if library_dir is not None:
        argv += ["-L", library_dir]
    argv += ["-luring", "-Wl,--trace", "-o", "/dev/null"]
    completed = subprocess.run(argv, input="#include <liburing.h>\n"
                               "int main() { return io_uring_queue_init(0, nullptr, 0) == 0 ? 0 : 1; }",
                               capture_output=True, text=True, env=env, check=False)
    text = completed.stdout + "\n" + completed.stderr
    match = re.search(r"^(\S*liburing\.so[\w.]*)$", text, re.MULTILINE)
    return match.group(1) if match else None


def same_file(a, b):
    return a is not None and b is not None and \
        os.path.realpath(a) == os.path.realpath(b)


def build_stubs(outdir, cxx, log_handle):
    """stub-good: full header copy; stub-bad: full copy minus one required
    declaration; fakelib: copy of the resolved shared object."""
    real_header = trace_header(cxx)
    real_library = trace_library(cxx)
    assert real_header and real_library, \
        f"could not resolve the system liburing pair ({real_header}, {real_library})"
    log_handle.write(f"resolved system header: {real_header}\n"
                     f"resolved system library: {real_library}\n")
    good = outdir / "stub-good" / "include"
    bad = outdir / "stub-bad" / "include"
    for stub in (good, bad):
        stub.mkdir(parents=True)
        shutil.copy(real_header, stub / "liburing.h")
        sibling = pathlib.Path(real_header).parent / "liburing"
        if sibling.is_dir():
            shutil.copytree(sibling, stub / "liburing", dirs_exist_ok=True)
    text = (good / "liburing.h").read_text()
    reduced, count = GET_PROBE_DECL.subn("", text)
    assert count == 1, f"expected exactly one get_probe_ring declaration block, matched {count}"
    (bad / "liburing.h").write_text(reduced)
    fakelib = outdir / "fakelib"
    fakelib.mkdir(parents=True)
    shutil.copy(real_library, fakelib / "liburing.so")
    return good, bad, fakelib


def configure(args, outdir, log_name, builddir_name, env):
    log = outdir / log_name
    builddir = outdir / builddir_name
    with log.open("w") as handle:
        for name in ("C_INCLUDE_PATH", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"):
            handle.write(f"$ {name}={env.get(name, '')}\n")
        handle.flush()
        rc = run([XMAKE, "f", "-y", "-m", "debug", "--liburing=y",
                  "--ccache=n", "-o", str(builddir)], handle, args.workdir, env)
    text = log.read_text()
    match = OK_LINE.search(text)
    return rc, text, (match.groups() if match else None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workdir", required=True,
                        help="git worktree holding the source under test")
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    outdir = pathlib.Path(args.output)
    outdir.mkdir(parents=True, exist_ok=True)

    head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                          text=True, cwd=args.workdir, check=True).stdout.strip()
    traces = outdir / "traces.log"
    with traces.open("w") as handle:
        handle.write(f"worktree_head={head}\ncxx={args.cxx}\n")
        handle.write(f"clean_env_header={trace_header(args.cxx)}\n")
        handle.write(f"clean_env_library={trace_library(args.cxx)}\n")
    good, bad, fakelib = build_stubs(outdir, args.cxx, traces.open("a"))

    # Negative: a CPATH-installed header missing a required declaration must
    # be refused at the declaration layer, by whatever authority resolves it.
    stub_env = dict(os.environ)
    stub_env["CPATH"] = str(bad)
    rc, text, _ = configure(args, outdir, "negative-deficient-header.log",
                            "build-negative", stub_env)
    assert rc != 0, "configure accepted a CPATH header missing io_uring_get_probe_ring"
    assert "header/declaration surface" in text, \
        "the refusal carried no declaration-layer evidence"
    assert "was not declared" in text, \
        "the refusal did not name the missing required declaration"
    print("negative deficient-cpath-header: refused at the declaration layer")

    # Divergence environment: C_INCLUDE_PATH + LIBRARY_PATH copies. Whatever
    # configure accepts and prints must be the pair the toolchain resolves.
    divergence_env = dict(os.environ)
    divergence_env["C_INCLUDE_PATH"] = str(good)
    divergence_env["LIBRARY_PATH"] = str(fakelib)
    env_header = trace_header(args.cxx, env=divergence_env)
    env_library = trace_library(args.cxx, env=divergence_env)
    with traces.open("a") as handle:
        handle.write(f"divergence_env_header_compiler_picks={env_header}\n")
        handle.write(f"divergence_env_library_linker_picks={env_library}\n")
    rc, text, printed = configure(args, outdir, "divergence-configure.log",
                                  "build-divergence", divergence_env)
    assert rc == 0, \
        "configure refused a fully working dependency pair; resolution is stricter than the toolchain"
    assert printed is not None, "configure accepted without printing the resolved identity"
    printed_header, printed_library = printed
    if not same_file(printed_header, env_header) or not same_file(printed_library, env_library):
        result = {
            "outcome": "F10_PROBE_PRODUCTION_IDENTITY_DIVERGENCE",
            "printed_identity": [printed_header, printed_library],
            "toolchain_identity": [env_header, env_library],
        }
        (outdir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"F10 RED: configure printed {printed_header} + {printed_library} "
              f"but the toolchain resolves {env_header} + {env_library}")
        sys.exit(1)
    print(f"divergence environment: printed identity matches the toolchain "
          f"({printed_header} + {printed_library})")

    # Clean environment: the printed identity is the resolved identity.
    rc, text, printed = configure(args, outdir, "identity-consistency.log",
                                  "build-consistency", dict(os.environ))
    assert rc == 0, "clean-environment configure failed"
    assert printed is not None, "clean-environment configure printed no identity"
    clean_header = trace_header(args.cxx)
    clean_library = trace_library(args.cxx)
    assert same_file(printed[0], clean_header), \
        f"printed header {printed[0]} != compiler resolution {clean_header}"
    assert same_file(printed[1], clean_library), \
        f"printed library {printed[1]} != linker resolution {clean_library}"
    (outdir / "result.json").write_text(json.dumps({
        "outcome": "F10_IDENTITY_BINDING_REGRESSION_PASS",
        "divergence_identity": [printed_header, printed_library],
        "clean_identity": [printed[0], printed[1]],
        "traced_clean_identity": [clean_header, clean_library],
    }, indent=2) + "\n")
    print("F10_IDENTITY_BINDING_REGRESSION_PASS")


if __name__ == "__main__":
    main()
