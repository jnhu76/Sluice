#!/usr/bin/env python3
"""Pre-F6 infrastructure stabilization (#520/#519/#521) six-profile matrix.

Same shape as the F5 matrix capture, with one contract change from #521:
the TSan profile now executes request_scope_test for real (the zero-
allocation seam compiles only outside TSan), so tsan-n expects the full
52 registered tests and there is no whole-target exclusion to police.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess

XMAKE = "/home/jnhu/.local/bin/xmake"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SUMMARY = re.compile(r"(\d+)% tests passed,\s+(\d+) test\(s\) failed out of (\d+)")

REGISTERED = [
    "app_copy_consumption_test", "app_copy_dir_fsync_test", "app_copy_pipeline_oracle_test",
    "app_grep_consumption_test", "app_hash_consumption_test", "app_tail_cli_oracle_test",
    "app_tail_consumption_test", "app_tail_lifecycle_oracle_test", "async_sync_admission_test",
    "backend_conformance_e1_threadpool_test", "blocking_file_read_test",
    "blocking_file_sequential_test", "blocking_file_state_test", "blocking_file_sync_all_test",
    "blocking_file_sync_data_test", "blocking_file_write_test", "direct_composition_fault_test",
    "direct_composition_test", "direct_cursor_position_test", "direct_w01_consumer_probe",
    "explicit_file_ref_test", "file_access_precedence_test", "file_close_semantics_test",
    "file_info_identity_test", "file_open_contract_test", "file_read_test", "file_resource_test",
    "file_sync_all_test", "file_sync_data_test", "file_write_test",
    "progress_source_ownership_test", "public_request_release_violation_test",
    "public_request_test", "request_core_consumer_probe", "request_core_ownership_test",
    "request_core_protocol_test", "request_core_publication_test", "request_scope_test",
    "semantic_durability_reference_test", "semantic_effect_outcome_test",
    "semantic_errno_mapping_test", "semantic_open_test", "semantic_range_test",
    "semantic_reference_case_test", "semantic_short_io_reference_test",
    "semantic_validation_precedence_test", "shutdown_lifecycle_test", "stackful_host_test",
    "threadpool_core_cutover_test", "threadpool_external_loop_test",
    "threadpool_progress_race_test", "uring_submit_boundary_test",
]
RING_ONLY = ["semantic_backend_conformance_test", "uring_public_consumer_probe",
             "uring_backend_smoke_test", "request_core_ownership_uring_test",
             "progress_source_ownership_uring_test", "uring_progress_race_test",
             "uring_external_loop_test", "uring_registration_lifecycle_test",
             "public_request_uring_test", "public_request_release_violation_uring_test",
             "shutdown_lifecycle_uring_test", "backend_conformance_e1_uring_test",
             "request_scope_uring_test", "uring_core_cutover_test"]

PROFILES = {
    "debug-n": {"mode": "debug", "liburing": "n", "expected": 52},
    "debug-y": {"mode": "debug", "liburing": "y", "expected": 66},
    "release-n": {"mode": "release", "liburing": "n", "expected": 52},
    "release-y": {"mode": "release", "liburing": "y", "expected": 66},
    "asanubsan-n": {"mode": "asanubsan", "liburing": "n", "expected": 52,
                    "toolchain": "clang", "ldflags": "--target=x86_64-redhat-linux-gnu",
                    "env": {"ASAN_OPTIONS": "detect_leaks=1",
                            "UBSAN_OPTIONS": "halt_on_error=1"}},
    "tsan-n": {"mode": "tsan", "liburing": "n", "expected": 52,
               "toolchain": "clang", "ldflags": "--target=x86_64-redhat-linux-gnu",
               "env": {"TSAN_OPTIONS": "halt_on_error=1"}},
}

parser = argparse.ArgumentParser()
parser.add_argument("--repo", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--profiles", default=",".join(PROFILES))
args = parser.parse_args()

repo = pathlib.Path(args.repo).resolve()
output = pathlib.Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=True)
assert len(REGISTERED) == 52 and len(set(REGISTERED)) == 52
assert len(RING_ONLY) == 14, len(RING_ONLY)


def run(argv, label, environment=None):
    log = output / (label + ".log")
    with open(log, "w") as handle:
        handle.write("$ " + " ".join(argv) + "\n")
        handle.flush()
        completed = subprocess.run(argv, stdout=handle, stderr=subprocess.STDOUT,
                                   env=environment, cwd=str(repo), check=False)
    text = ANSI.sub("", log.read_text())
    return {"argv": argv, "exit_code": completed.returncode, "log": log.name,
            "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(), "text": text}


toolchain = output / "toolchain.log"
head_sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                          cwd=str(repo), check=True).stdout.strip()
with open(toolchain, "w") as handle:
    handle.write("# worktree head: " + head_sha + "\n")
    for argv in ([XMAKE, "--version"], ["g++", "--version"], ["clang++", "--version"],
                 ["uname", "-r"],
                 ["bash", "-c", "ls /usr/include/liburing.h && ldconfig -p | grep liburing"]):
        handle.write("$ " + " ".join(argv) + "\n")
        completed = subprocess.run(argv, capture_output=True, text=True, check=False)
        handle.write(completed.stdout + completed.stderr + "\n")

records = []
for name in args.profiles.split(","):
    spec = PROFILES[name]
    builddir = "/tmp/pre-f6-build-" + name
    configure = [XMAKE, "f", "-m", spec["mode"], "--liburing=" + spec["liburing"],
                 "--ccache=n", "-o", builddir, "-y"]
    if "toolchain" in spec:
        configure += ["--toolchain=" + spec["toolchain"]]
    if "ldflags" in spec:
        configure += ["--ldflags=" + spec["ldflags"]]
    environment = dict(os.environ, **spec.get("env", {}))
    config = run(configure, name + "-configure", environment)
    if config["exit_code"] != 0:
        raise AssertionError(("configure failed", name, config))
    xmake_conf = repo / ".xmake/linux/x86_64/xmake.conf"
    toolchain_cache = repo / ".xmake/linux/x86_64/cache/toolchain"
    with open(output / (name + "-xmake.conf"), "w") as handle:
        handle.write(xmake_conf.read_text())
    with open(output / (name + "-toolchain-cache.txt"), "w") as handle:
        handle.write(toolchain_cache.read_text())

    test_argv = [XMAKE, "test", "-y"]
    tested = run(test_argv, name, environment)
    match = SUMMARY.search(tested["text"])
    if tested["exit_code"] != 0 or not match:
        raise AssertionError(("test run failed", name, tested["exit_code"]))
    passed, failed, total = (int(g) for g in match.groups())
    if failed != 0 or total != spec["expected"] or passed != 100:
        raise AssertionError(("unexpected summary", name, match.group(0)))
    records.append({"profile": name, "mode": spec["mode"], "liburing": spec["liburing"],
                    "builddir": builddir, "tests": total, "failed": failed,
                    "configure": {k: v for k, v in config.items() if k != "text"},
                    "test": {k: v for k, v in tested.items() if k != "text"}})

(output / "matrix.json").write_text(json.dumps(records, indent=2) + "\n")
for record in records:
    print(record["profile"], "tests=%d failed=%d" % (record["tests"], record["failed"]))
print("PRE_F6_MATRIX_SIX_PROFILES_PASS")
