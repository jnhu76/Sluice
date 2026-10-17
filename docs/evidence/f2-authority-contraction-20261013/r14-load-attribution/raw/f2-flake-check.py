#!/usr/bin/env python3
"""Measure the load sensitivity of threadpool_external_loop_test on two trees.

Runs N concurrent instances per round and records exit codes, wall time and the
failed scenario names, so a load-induced poll timeout can be told apart from a
protocol failure (a stranded obligation fails fast and quickly, a starved
signaler thread stalls in the 2000 ms poll).
"""
import argparse
import json
import pathlib
import re
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--label", required=True)
parser.add_argument("--instances", type=int, default=20)
parser.add_argument("--rounds", type=int, default=3)
parser.add_argument("--output", required=True)
args = parser.parse_args()

output = pathlib.Path(args.output)
output.mkdir(parents=True, exist_ok=True)
records = []
for round_index in range(args.rounds):
    started = time.time()
    processes = []
    for index in range(args.instances):
        log = output / ("%s-r%d-i%d.log" % (args.label, round_index, index))
        handle = open(log, "w")
        processes.append((index, log, handle, time.time(),
                          subprocess.Popen([args.binary], stdout=handle,
                                           stderr=subprocess.STDOUT)))
    for index, log, handle, start, process in processes:
        code = process.wait()
        handle.close()
        text = log.read_text()
        failures = sorted(set(re.findall(r"^FAIL: (\S+)", text, re.M)))
        records.append({"round": round_index, "instance": index, "exit_code": code,
                        "seconds": round(time.time() - start, 3),
                        "failed_scenarios": failures, "log": log.name})
    print("round", round_index, "wall", round(time.time() - started, 2), flush=True)

summary = {
    "binary": args.binary,
    "label": args.label,
    "instances": args.instances,
    "rounds": args.rounds,
    "runs": len(records),
    "failures": sum(1 for r in records if r["exit_code"] != 0),
    "failed_scenarios": sorted({s for r in records for s in r["failed_scenarios"]}),
    "failure_seconds": sorted(r["seconds"] for r in records if r["exit_code"] != 0)[:20],
    "pass_seconds": sorted(r["seconds"] for r in records if r["exit_code"] == 0)[:20],
    "records": records,
}
(output / (args.label + "-summary.json")).write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps({k: v for k, v in summary.items() if k != "records"}))
