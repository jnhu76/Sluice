#!/usr/bin/env python3
"""F2 (#402) configuration matrix and sanitizer scope capture.

Profiles:
  debug-n, debug-y, release-n, release-y
      Full registered-test and probe set in one xmake build, every non-mutant
      binary executed directly (per-target logs) plus `xmake test -v` for the
      CI-parity view.
  asanubsan-n, tsan-n
      Native Clang ASan+UBSan / TSan over the F2-affected scope: publication,
      identity, observer attach, cancellation, shutdown, backend conformance
      and the request/scope/cutover/race suites.

Each profile records: configure argv, build log, per-target build and run logs
with exit codes and log sha256, the effective xmake.conf and the toolchain cache
that names the compiler program actually used, results.tsv, and the raw
`xmake test -v` log. Mutant targets are compiled by the profile build but are
never executed here: their kill campaign is separate evidence.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import time

XMAKE = "/home/jnhu/.local/bin/xmake"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

PROFILES = {
    "debug-n": {"mode": "debug", "liburing": "n"},
    "debug-y": {"mode": "debug", "liburing": "y"},
    "release-n": {"mode": "release", "liburing": "n"},
    "release-y": {"mode": "release", "liburing": "y"},
    "asanubsan-n": {
        "mode": "asanubsan",
        "liburing": "n",
        "toolchain": "clang",
        "ldflags": "--target=x86_64-redhat-linux-gnu",
        "env": {"ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1"},
    },
    "tsan-n": {
        "mode": "tsan",
        "liburing": "n",
        "toolchain": "clang",
        "ldflags": "--target=x86_64-redhat-linux-gnu",
        "env": {"TSAN_OPTIONS": "halt_on_error=1"},
    },
}

SANITIZER_SCOPE = [
    "file_read_test",
    "file_write_test",
    "file_sync_data_test",
    "file_sync_all_test",
    "explicit_file_ref_test",
    "file_access_precedence_test",
    "request_scope_test",
    "public_request_test",
    "public_request_release_violation_test",
    "request_core_protocol_test",
    "request_core_publication_test",
    "threadpool_core_cutover_test",
    "runtime_waiter_observer_test",
    "backend_conformance_e1_threadpool_test",
    "shutdown_lifecycle_test",
    "threadpool_progress_race_test",
    "threadpool_external_loop_test",
    "progress_source_ownership_test",
]

RUN_TIMEOUT_SECONDS = 300
BUILD_TIMEOUT_SECONDS = 3600

# The registered-test inventory at the captured revision (libraries, the app
# front-ends and the mutant builds excluded). A profile that builds fewer
# runnable binaries than this is a build-set change, not a smaller matrix.
EXPECTED_TARGETS = {"debug-n": 53, "debug-y": 67, "release-n": 53, "release-y": 67,
                    "asanubsan-n": 18, "tsan-n": 18}

# Documented per-profile unavailability. request_scope_test supplies strong
# operator new/delete overrides to observe allocation, which collide with the
# TSan C++ runtime; the F3 matrix recorded the same outcome. The build log must
# carry the recorded reason, so a different link error cannot be absorbed here.
UNAVAILABLE = {
    ("tsan-n", "request_scope_test"): {
        "status": "LINK_UNAVAILABLE_STRONG_ALLOCATION_OVERRIDES",
        "requires": ["multiple definition of `operator new"],
    },
}


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def run(argv, log, environment, cwd, timeout):
    started = time.time()
    with open(log, "w") as handle:
        handle.write("$ " + " ".join(argv) + "\n")
        handle.flush()
        try:
            completed = subprocess.run(
                argv, stdout=handle, stderr=subprocess.STDOUT, env=environment,
                cwd=str(cwd), timeout=timeout, check=False)
            code = completed.returncode
        except subprocess.TimeoutExpired:
            handle.write("\n[TIMEOUT after %ss]\n" % timeout)
            code = 124
    return {
        "argv": argv,
        "cwd": str(cwd),
        "exit_code": code,
        "log": str(log),
        "log_sha256": sha256(log),
        "seconds": round(time.time() - started, 2),
    }


parser = argparse.ArgumentParser()
parser.add_argument("--repo", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--profiles", default=",".join(PROFILES))
args = parser.parse_args()

repo = pathlib.Path(args.repo).resolve()
output = pathlib.Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=False)
environment = dict(os.environ)
environment.setdefault("PATH", "/usr/bin:/bin")
for key in ("ASAN_OPTIONS", "TSAN_OPTIONS", "UBSAN_OPTIONS"):
    environment.pop(key, None)

sha = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"]).decode().strip()
summary = {"sha": sha, "profiles": {}}
commands = []
(output / "environment.txt").write_text(
    subprocess.check_output(["uname", "-a"]).decode()
    + subprocess.check_output(["/usr/bin/g++", "--version"]).decode().splitlines()[0] + "\n"
    + subprocess.check_output(["/usr/bin/clang++", "--version"]).decode().splitlines()[0] + "\n"
    + subprocess.check_output([XMAKE, "--version"]).decode().strip() + "\n")

for name in args.profiles.split(","):
    profile = PROFILES[name]
    spec = profile
    record = {"mode": spec["mode"], "liburing": spec["liburing"],
              "toolchain": spec.get("toolchain"), "sha": sha}
    profile_dir = output / name
    profile_dir.mkdir()
    builddir = profile_dir / "build"
    run_cwd = profile_dir / "run-cwd"
    run_cwd.mkdir()
    build_env = dict(environment)

    configure = [XMAKE, "f", "-m", spec["mode"], "--liburing=" + spec["liburing"],
                 "--ccache=n", "-o", str(builddir), "-y"]
    if "toolchain" in spec:
        configure += ["--toolchain=" + spec["toolchain"]]
    if "ldflags" in spec:
        configure += ["--ldflags=" + spec["ldflags"]]
    entry = run(configure, profile_dir / "config.log", build_env, repo, BUILD_TIMEOUT_SECONDS)
    if entry["exit_code"] != 0:
        raise AssertionError(("configure failed", name, entry))
    commands.append(dict(profile=name, stage="configure", **entry))

    shutil.copyfile(repo / ".xmake/linux/x86_64/xmake.conf", profile_dir / "xmake.conf")
    shutil.copyfile(repo / ".xmake/linux/x86_64/cache/toolchain",
                    profile_dir / "toolchain-cache.txt")
    record["xmake_conf_sha256"] = sha256(profile_dir / "xmake.conf")
    record["toolchain_cache_sha256"] = sha256(profile_dir / "toolchain-cache.txt")

    scope = SANITIZER_SCOPE if spec["mode"] in ("asanubsan", "tsan") else None
    if scope is None:
        build = run([XMAKE, "b", "-y", "-a"], profile_dir / "build.log", build_env, repo,
                    BUILD_TIMEOUT_SECONDS)
        commands.append(dict(profile=name, stage="build", **build))
        if build["exit_code"] != 0:
            raise AssertionError(("build failed", name, build))
    else:
        # The sanitizer profiles build only the F2-affected scope: building the
        # whole inventory (mutants included) under instrumentation would cost an
        # order of magnitude more machine time for evidence the debug and
        # release profiles already carry.
        unavailable = []
        for target in list(scope):
            build = run([XMAKE, "b", "-y", target], profile_dir / (target + ".build.log"),
                        build_env, repo, BUILD_TIMEOUT_SECONDS)
            commands.append(dict(profile=name, stage="build", target=target, **build))
            if build["exit_code"] == 0:
                continue
            waiver = UNAVAILABLE.get((name, target))
            text = (profile_dir / (target + ".build.log")).read_text()
            if waiver is not None and all(needle in text for needle in waiver["requires"]):
                scope.remove(target)
                unavailable.append({"target": target, **waiver})
                continue
            raise AssertionError(("scope target build failed", name, target, build))
        record["unavailable"] = unavailable

    mode_dir = builddir / "linux" / "x86_64" / spec["mode"]
    binaries = sorted(p for p in mode_dir.iterdir() if p.is_file() and p.stat().st_mode & 0o111)
    mutants = [p.name for p in binaries if "_mut_" in p.name]
    if scope is None:
        runnable = [p.name for p in binaries
                    if "_mut_" not in p.name and not p.name.startswith("sluice-")
                    and not p.name.startswith("lib")]
    else:
        runnable = sorted(scope)
        missing = [t for t in runnable if not (mode_dir / t).is_file()]
        assert not missing, ("sanitizer scope target not built", name, missing)
    record["mutants_built"] = mutants
    expected = EXPECTED_TARGETS[name]
    assert len(runnable) == expected or (name in ("asanubsan-n", "tsan-n") and
                                        len(runnable) + len(record.get("unavailable", [])) == expected), \
        ("unexpected runnable target count", name, len(runnable), expected, runnable)
    record["targets_run"] = runnable
    (profile_dir / "targets.txt").write_text("\n".join(runnable) + "\n")

    results = {}
    for target in runnable:
        binary = mode_dir / target
        run_env = dict(environment)
        run_env.update(spec.get("env", {}))
        entry = run([str(binary)],
                    profile_dir / (target + ".run.log"), run_env, run_cwd,
                    RUN_TIMEOUT_SECONDS)
        commands.append(dict(profile=name, stage="run", target=target, **entry))
        results[target] = "PASS" if entry["exit_code"] == 0 else "FAIL(%d)" % entry["exit_code"]
    for entry in record.get("unavailable", []):
        results[entry["target"]] = entry["status"]
    (profile_dir / "results.tsv").write_text(
        "".join("%s\t%s\n" % (t, r) for t, r in sorted(results.items())))

    if scope is None:
        test = run([XMAKE, "test", "-v"], profile_dir / "xmake-test.log", environment, repo,
                   BUILD_TIMEOUT_SECONDS)
        commands.append(dict(profile=name, stage="xmake-test", **test))
        (profile_dir / "xmake-test.summary.txt").write_text(
            "\n".join(ANSI.sub("", line) for line in
                      (profile_dir / "xmake-test.log").read_text().splitlines()
                      if re.search(r"\b(passed|failed|tests?)\b", line, re.I)) + "\n")
        record["xmake_test_exit"] = test["exit_code"]
        if test["exit_code"] != 0:
            raise AssertionError(("xmake test failed", name, test))

    record["results"] = results
    record["pass_count"] = sum(1 for v in results.values() if v == "PASS")
    record["total"] = len(results)
    summary["profiles"][name] = record
    (profile_dir / "sha.txt").write_text(sha + "\n")
    (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("profile", name, record["pass_count"], "/", record["total"],
          "xmake-test", record.get("xmake_test_exit", "scope"), flush=True)

failures = {p: [t for t, r in v["results"].items() if r.startswith("FAIL")]
            for p, v in summary["profiles"].items()}
bad = {p: v for p, v in failures.items() if v}
assert not bad, ("matrix failures", bad)
print("F2_MATRIX_PASS", sha, json.dumps({p: v["total"] for p, v in summary["profiles"].items()}))
