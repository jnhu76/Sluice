#!/usr/bin/env python3
"""Capture actual instrumentation from existing isolated sanitizer builds."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("build_root", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
args.output.mkdir(exist_ok=False, parents=True)
records = []
for binary in sorted(args.build_root.glob("*/linux/x86_64/*/*")):
    if not binary.is_file() or not binary.name.startswith("app_"):
        continue
    mode = binary.parent.name
    command = ["nm", "-C", str(binary)]
    run = subprocess.run(command, capture_output=True, text=True, check=False)
    log = args.output / (binary.parent.parent.parent.parent.name + "-" + binary.name + ".nm")
    log.write_text(run.stdout + run.stderr)
    expected = "__asan_init" if mode == "asanubsan" else "__tsan_init"
    records.append(dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), mode=mode, command=command, exit_code=run.returncode, log=log.name, log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(), instrumentation=expected, present=expected in run.stdout, ubsan_handler_present="__ubsan_handle_" in run.stdout if mode == "asanubsan" else None))
objects = []
for obj in sorted(args.build_root.rglob("*.o")):
    if "asanubsan" not in obj.parts or "apps" not in obj.parts:
        continue
    run = subprocess.run(["nm", "-u", str(obj)], capture_output=True, text=True, check=False)
    if "__asan_" in run.stdout or "__ubsan_" in run.stdout:
        objects.append(dict(path=str(obj), sha256=hashlib.sha256(obj.read_bytes()).hexdigest(), command=["nm", "-u", str(obj)], exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
report = dict(records=records, app_objects=objects)
(args.output / "instrumentation.json").write_text(json.dumps(report, indent=2) + "\n")
assert len(records) == 18, len(records)
assert all(r["exit_code"] == 0 and r["present"] and r["ubsan_handler_present"] is not False for r in records)
assert objects and any("__ubsan_" in o["stdout"] for o in objects)
print("PASS: 18 executed test binaries instrumented; app object sanitizer references recorded")
