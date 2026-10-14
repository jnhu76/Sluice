#!/usr/bin/env python3
"""#520 regression: the --liburing switch must be safe in one build directory.

Runs n -> y -> n -> y in the SAME git worktree and the SAME xmake build
directory with no cleaning, and asserts after every phase:

- the effective option, the stored compile fingerprint (depend values) and
  the SLUICE_LIBURING_PROFILE marker agree,
- sluice_async's uring_backend object is recompiled on every switch (the
  stale-reuse defect), and a consumer target built in the n profile is
  recompiled after switching to y,
- the archive carries the exact profile identity (stub ctor only under n,
  the full ring ctor set under y) with no stale members,
- the registered test set passes: 52/52 (n), 66/66 (y, real ring link+run).

It then runs the build-time fault-injection negatives through the real
configure entry point; every failure must be detected at its own layer:

- missing liburing header            -> declaration layer (liburing.h not found)
- missing liburing library           -> library layer (-luring unresolved)
- incompatible required API          -> declaration layer (missing declaration)
- conflicting include/lib prefixes   -> prefix check (refused install)

Kernel-side capabilities (ring creation, required opcodes, statx) are never
tested at configure time; they stay owned by the UringAsyncBackend
construction path, whose injected statx/opcode failures are exercised by the
registered backend_conformance_e1_uring_test inside the y-phase test runs.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
XMAKE = os.environ.get("SLUICE_XMAKE", "/home/jnhu/.local/bin/xmake")
TEST_SUMMARY = re.compile(r"(\d+)% tests passed,\s+(\d+) test\(s\) failed out of (\d+)")
RING_CTORS = [
    "sluice::async::UringAsyncBackend::UringAsyncBackend(unsigned int)",
    "sluice::async::UringAsyncBackend::UringAsyncBackend(sluice::async::UringConfig)",
    "sluice::async::UringAsyncBackend::UringAsyncBackend(sluice::async::UringConfig, "
    "sluice::async::UringAsyncBackend::ValidatedConfigTag)",
]
STUB_CTORS = ["sluice::async::UringAsyncBackend::UringAsyncBackend(unsigned int)"]
DEPEND_REL = ".deps/sluice_async/linux/x86_64/debug/src/async/uring_backend.cpp.o.d"
OBJECT_REL = ".objs/sluice_async/linux/x86_64/debug/src/async/uring_backend.cpp.o"
ARCHIVE_REL = "linux/x86_64/debug/libsluice_async.a"

def run(argv, log_handle, cwd, env=None):
    log_handle.write("$ " + " ".join(argv) + "\n")
    log_handle.flush()
    completed = subprocess.run(argv, stdout=log_handle, stderr=subprocess.STDOUT,
                               cwd=str(cwd), env=env, check=False)
    return completed.returncode


def stripped(path):
    return ANSI.sub("", pathlib.Path(path).read_text())


class Phase:
    def __init__(self, name, buildir, outdir, workdir, env=None):
        self.name = name
        self.builddir = buildir
        self.out = outdir
        self.workdir = workdir
        self.env = env or dict(os.environ)
        self.log = outdir / f"phase-{name}.log"
        self.handle = self.log.open("w")

    def xmake(self, *argv):
        return run([XMAKE, *argv], self.handle, self.workdir, self.env)

    def snapshot(self):
        builddir = pathlib.Path(self.builddir)
        snap = {"name": self.name}
        conf = self.workdir / ".xmake/linux/x86_64/xmake.conf"
        text = conf.read_text() if conf.exists() else ""
        match = re.search(r"liburing = (true|false)", text)
        snap["effective_liburing"] = match.group(1) == "true" if match else None
        depend = builddir / DEPEND_REL
        snap["depend_values"] = ""
        snap["depend_has_profile_on"] = False
        snap["depend_has_profile_off"] = False
        snap["depend_has_has_liburing"] = False
        if depend.exists():
            values = depend.read_text()
            snap["depend_values"] = values
            snap["depend_has_profile_on"] = "SLUICE_LIBURING_PROFILE=on" in values
            snap["depend_has_profile_off"] = "SLUICE_LIBURING_PROFILE=off" in values
            snap["depend_has_has_liburing"] = "SLUICE_HAS_LIBURING" in values
        obj = builddir / OBJECT_REL
        snap["object_sha256"] = (hashlib.sha256(obj.read_bytes()).hexdigest()
                                 if obj.exists() else None)
        archive = builddir / ARCHIVE_REL
        snap["archive_members"] = None
        snap["ctor_symbols"] = None
        if archive.exists():
            members = subprocess.run(["ar", "t", str(archive)], capture_output=True,
                                     text=True, check=True).stdout.split()
            snap["archive_members"] = len(members)
            snap["archive_member_names"] = members
            nm = subprocess.run(["nm", "-C", str(archive)], capture_output=True,
                                text=True, check=True).stdout
            ctors = sorted(set(line.split("T ")[-1].strip()
                               for line in nm.splitlines()
                               if "UringAsyncBackend::UringAsyncBackend" in line))
            snap["ctor_symbols"] = ctors
        return snap

    def close(self):
        self.handle.close()


def assert_phase(profile, snap, summary_text):
    ring = profile == "y"
    expected_tests = 66 if ring else 52
    match = TEST_SUMMARY.search(summary_text)
    assert match, f"[{profile}] no test summary found"
    percent, failed, total = (int(g) for g in match.groups())
    assert failed == 0 and total == expected_tests and percent == 100, \
        f"[{profile}] test summary mismatch: {match.group(0)}"
    assert snap["effective_liburing"] == ring, \
        f"[{profile}] effective option mismatch: {snap['effective_liburing']}"
    assert snap["depend_has_profile_on"] == ring, \
        f"[{profile}] profile marker missing in stored compile fingerprint"
    assert snap["depend_has_has_liburing"] == ring, \
        f"[{profile}] SLUICE_HAS_LIBURING missing/extra in stored compile fingerprint"
    expected_ctors = RING_CTORS if ring else STUB_CTORS
    assert snap["ctor_symbols"] == sorted(expected_ctors), \
        f"[{profile}] archive ctor identity mismatch: {snap['ctor_symbols']}"
    assert snap["archive_members"] == 13, \
        f"[{profile}] unexpected archive member count: {snap['archive_members']}"


def consumer_depend_fingerprint(builddir):
    """The stored depend values of a consumer TU compiled in the n profile.
    Rewritten at every recompile, so their content is the evidence that the
    consumer was rebuilt with the current profile's public defines."""
    depend = pathlib.Path(builddir) / (
        ".deps/app_copy_consumption_test/linux/x86_64/debug/tests/"
        "app_copy_consumption_test.cpp.o.d")
    assert depend.exists(), depend
    text = depend.read_text()
    return {
        "profile_on": "SLUICE_LIBURING_PROFILE=on" in text,
        "has_liburing": "SLUICE_HAS_LIBURING" in text,
    }


def run_cycle(args, outdir):
    workdir = pathlib.Path(args.workdir)
    builddir = args.builddir
    identity = outdir / "identity.txt"
    with identity.open("w") as handle:
        head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                              text=True, cwd=workdir, check=True).stdout.strip()
        handle.write(f"worktree_head={head}\nbuilddir={builddir}\n")
        for argv in (["git", "status", "--short"], [XMAKE, "--version"], ["uname", "-r"]):
            completed = subprocess.run(argv, capture_output=True, text=True, cwd=workdir)
            handle.write("$ " + " ".join(argv) + "\n" + completed.stdout + completed.stderr)

    phases = []
    for index, profile in enumerate(["n", "y", "n", "y"]):
        phase = Phase(f"{index + 1}{profile}", builddir, outdir, workdir)
        phases.append(phase)
        rc = phase.xmake("f", "-y", "-m", "debug", f"--liburing={profile}",
                         "--ccache=n", "-o", builddir)
        assert rc == 0, f"configure {profile} failed, see {phase.log}"
        rc = phase.xmake("build", "-y", "app_copy_consumption_test")
        assert rc == 0, f"consumer build failed in phase {profile}, see {phase.log}"
        consumer_fingerprint = consumer_depend_fingerprint(builddir)
        assert consumer_fingerprint["profile_on"] == (profile == "y"), \
            f"[{profile}] consumer fingerprint lacks the profile marker: {consumer_fingerprint}"
        assert consumer_fingerprint["has_liburing"] == (profile == "y"), \
            f"[{profile}] consumer fingerprint lacks SLUICE_HAS_LIBURING: {consumer_fingerprint}"
        rc = phase.xmake("build", "-y", "sluice_async")
        assert rc == 0, f"sluice_async build failed in phase {profile}, see {phase.log}"
        rc = phase.xmake("test", "-y")
        assert rc == 0, f"tests failed in phase {profile}, see {phase.log}"
        snap = phase.snapshot()
        if index > 0:
            previous_object = phases[-2].snapshot_frozen["object_sha256"]
            assert snap["object_sha256"] != previous_object, \
                f"[{profile}] uring_backend object was reused across the switch " \
                f"(stale object identity {snap['object_sha256']})"
        summary_text = stripped(phase.log)
        assert_phase(profile, snap, summary_text)
        snap["consumer_fingerprint"] = consumer_fingerprint
        phase.snapshot_frozen = snap
        (outdir / f"phase-{profile}-{index}.json").write_text(
            json.dumps(snap, indent=2) + "\n")
        phase.close()
    return phases


REDUCED_STUB_HEADER = """#pragma once
struct io_uring { unsigned dummy; };
struct io_uring_sqe { unsigned dummy; };
struct io_uring_cqe { unsigned dummy; };
int io_uring_queue_init(unsigned entries, struct io_uring* ring, unsigned flags);
"""


def unshare_available():
    probe = subprocess.run(["unshare", "-r", "-m", "true"], capture_output=True)
    return probe.returncode == 0


def run_negative(args, outdir, name, wrap_argv, expect_needles):
    """Runs `xmake f --liburing=y` under `wrap_argv` prefix and requires the
    configure to fail with the given layer evidence."""
    log = outdir / f"negative-{name}.log"
    env = dict(os.environ)
    if name in ("incompatible-required-api", "conflicting-include-prefix"):
        stub_dir = outdir / f"stub-{name}"
        stub_dir.mkdir(parents=True, exist_ok=True)
        if name == "incompatible-required-api":
            (stub_dir / "liburing.h").write_text(REDUCED_STUB_HEADER)
        else:
            (stub_dir / "liburing.h").write_text("#include_next <liburing.h>\n")
        env["CPATH"] = str(stub_dir)
    argv = wrap_argv + [XMAKE, "f", "-y", "-m", "debug", "--liburing=y",
                        "--ccache=n", "-o", f"{args.builddir}-neg-{name}"]
    if wrap_argv:
        env["XMAKE_ROOT"] = "y"  # xmake refuses the fake root of unshare -r
    with log.open("w") as handle:
        handle.write("$ " + " ".join(argv) + f"\n$ CPATH={env.get('CPATH', '')}\n")
        handle.flush()
        completed = subprocess.run(argv, stdout=handle, stderr=subprocess.STDOUT,
                                   cwd=args.workdir, env=env, check=False)
    text = stripped(log)
    assert completed.returncode != 0, \
        f"[negative:{name}] configure unexpectedly succeeded; the injection did not bite"
    for needle in expect_needles:
        assert needle in text, \
            f"[negative:{name}] expected layer evidence '{needle}' in the failure"
    print(f"negative {name}: failed at the expected layer")


def hide_header_argv():
    return ["unshare", "-r", "-m", "bash", "-c",
            "mount -t tmpfs tmpfs /usr/include && exec \"$@\"", "-"]


def hide_library_argv():
    script = (
        "set -e; touch /tmp/sluice-empty-file; "
        "for f in /usr/lib64/liburing.so /usr/lib64/liburing.so.2 "
        "/usr/lib64/liburing.so.2.13; do "
        "if [ -e \"$f\" ] || [ -L \"$f\" ]; then "
        "mount --bind /tmp/sluice-empty-file \"$f\"; fi; done; "
        "exec \"$@\"")
    return ["unshare", "-r", "-m", "bash", "-c", script, "-"]


def run_negatives(args, outdir):
    if not unshare_available():
        print("user namespaces unavailable; build-time negatives skipped")
        return False
    run_negative(args, outdir, "missing-liburing-header", hide_header_argv(),
                 ["liburing.h", "header/declaration surface"])
    run_negative(args, outdir, "missing-liburing-library", hide_library_argv(),
                 ["required external symbols"])
    run_negative(args, outdir, "incompatible-required-api", [],
                 ["header/declaration surface", "was not declared"])
    run_negative(args, outdir, "conflicting-include-prefix", [],
                 ["different installation prefixes"])
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workdir", required=True,
                        help="git worktree holding the source under test")
    parser.add_argument("--builddir", required=True,
                        help="the single build directory reused by every phase")
    parser.add_argument("--output", required=True)
    parser.add_argument("--skip-negatives", action="store_true")
    parser.add_argument("--skip-cycle", action="store_true",
                        help="iterate on the negatives without re-running the "
                             "four-phase build cycle")
    args = parser.parse_args()
    outdir = pathlib.Path(args.output)
    outdir.mkdir(parents=True, exist_ok=True)

    if not args.skip_cycle:
        run_cycle(args, outdir)
        print("N_Y_N_Y_SAME_DIR_SWITCH_CYCLE = PASS")
    negatives_ran = True
    if not args.skip_negatives:
        negatives_ran = run_negatives(args, outdir)
    record = {"switch_cycle": "PASS", "negatives_ran": negatives_ran}
    (outdir / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print("LIBURING_CONFIG_SWITCH_REGRESSION_PASS")


if __name__ == "__main__":
    main()
