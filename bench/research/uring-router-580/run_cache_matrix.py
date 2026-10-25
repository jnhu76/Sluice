#!/usr/bin/env python3
"""#580 R9 cache-locality matrix runner (research-only).

Stages (default: all):
  probe    instrumented count matrix (timed line discarded, counters kept):
           pipeline D x working-set sweep + external split/single subset
  pairs    uninstrumented paired timing: (warm, interfered) + A/A controls
  flush    clflush mechanism-sensitivity pairs (cache_ab_flush vs cache_ab)
  external W-03 replica pairs incl. split-core and driver-side interference

Every run is one process. Runs alternate execution order by repetition index
(AB/BA); rows are annotated with pair/slot/cond and appended to the output
JSONL. Rows with ok=false are kept but excluded by the reducer. Pinning is
done inside the harness (pthread affinity); no global host parameters change.
"""
import argparse
import json
import os
import subprocess
import sys
import time

WS = {"warm": 0, "64k": 64 * 1024, "512k": 512 * 1024, "4m": 4 * 1024 * 1024,
      "16m": 16 * 1024 * 1024}


def run_one(binary, args, out_path, annot):
    proc = subprocess.run([binary] + args, capture_output=True, text=True, timeout=600)
    timed = None
    counters = None
    for line in proc.stdout.splitlines():
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if obj.get("kind") == "timed":
            timed = obj
        elif obj.get("kind") == "counters":
            counters = obj
    if timed is None:
        timed = {"kind": "timed", "ok": False,
                 "failure": "no timed line: " + proc.stderr.strip()[-200:]}
    timed.update(annot)
    with open(out_path, "a") as fh:
        fh.write(json.dumps(timed) + "\n")
    if counters is not None:
        counters.update(annot)
        with open(out_path, "a") as fh:
            fh.write(json.dumps(counters) + "\n")
    return timed


def pipeline_args(depth, ws, ops, warmup, label):
    return ["--scenario", "pipeline", "--depth", str(depth), "--ops", str(ops),
            "--warmup", str(warmup), "--pin-mode", "single", "--cpu-a", "2",
            "--ws-bytes", str(ws), "--label", label]


def external_args(depth, ws, pin, side, ops, warmup, label):
    return ["--scenario", "external", "--depth", str(depth), "--ops", str(ops),
            "--warmup", str(warmup), "--pin-mode", pin, "--cpu-a", "2", "--cpu-b", "3",
            "--interference-side", side, "--ws-bytes", str(ws), "--label", label]


def stage_probe(bin_dir, out_path, reps=2, shape_filter=None):
    probe = os.path.join(bin_dir, "cache_probe")
    for depth in (16, 64, 128, 256, 512):
        for ws_name, ws in WS.items():
            for rep in range(reps):
                args = pipeline_args(depth, ws, 20000, 2000, f"probe_d{depth}_{ws_name}")
                run_one(probe, args, out_path,
                        {"stage": "probe", "shape": f"pipe_d{depth}_ws{ws_name}",
                         "rep": rep, "ws_name": ws_name})
                print(f"probe pipe d{depth} {ws_name} rep{rep}", file=sys.stderr)
    for ws_name in ("warm", "16m"):
        for pin in ("single", "split"):
            for rep in range(reps):
                args = external_args(64, WS[ws_name], pin, "submitter", 10000, 500,
                                     f"probe_ext_{ws_name}_{pin}")
                run_one(probe, args, out_path,
                        {"stage": "probe", "shape": f"ext_d64_{pin}_{ws_name}",
                         "rep": rep, "ws_name": ws_name})
                print(f"probe ext {pin} {ws_name} rep{rep}", file=sys.stderr)
    args = external_args(64, WS["16m"], "split", "driver", 10000, 500,
                         "probe_ext_16m_driver")
    run_one(probe, args, out_path,
            {"stage": "probe", "shape": "ext_d64_split_driver_16m", "rep": 0,
             "ws_name": "16m"})
    print("probe ext split driver 16m", file=sys.stderr)


def pair_runs(bin_dir, out_path, pairs, reps, stage, shape_filter=None):
    if shape_filter:
        pairs = [(k, v) for k, v in pairs if shape_filter in k]
    for rep in range(reps):
        for shape_key, runs in pairs:
            executions = runs if rep % 2 == 0 else list(reversed(runs))
            pair_id = f"{shape_key}#r{rep}"
            for slot, (binary, args, cond) in enumerate(executions):
                annot = {"stage": stage, "pair": pair_id, "rep": rep, "slot": slot,
                         "cond": cond, "shape": shape_key}
                run_one(os.path.join(bin_dir, binary),
                        args + ["--label", f"{stage}_{shape_key}_{cond}"],
                        out_path, annot)
        print(f"{stage} rep{rep} done", file=sys.stderr)


def stage_pairs(bin_dir, out_path, reps, ops=40000, warmup=2000, shape_filter=None):
    pairs = []
    for depth, ws_names in ((16, ("16m",)), (64, ("64k", "4m", "16m")),
                            (128, ("16m",)), (256, ("16m",)), (512, ("16m",))):
        for ws_name in ws_names:
            pairs.append((f"pipe_d{depth}_ws{ws_name}", [
                ("cache_ab", pipeline_args(depth, WS["warm"], ops, warmup, "x"), "warm"),
                ("cache_ab", pipeline_args(depth, WS[ws_name], ops, warmup, "x"), "ws" + ws_name)]))
    pairs.append(("pipe_d64_aa_warm", [
        ("cache_ab_aa_alpha", pipeline_args(64, WS["warm"], ops, warmup, "x"), "aa"),
        ("cache_ab_aa_beta", pipeline_args(64, WS["warm"], ops, warmup, "x"), "aa")]))
    pairs.append(("pipe_d64_aa_16m", [
        ("cache_ab_aa_alpha", pipeline_args(64, WS["16m"], ops, warmup, "x"), "aa16m"),
        ("cache_ab_aa_beta", pipeline_args(64, WS["16m"], ops, warmup, "x"), "aa16m")]))
    pairs.append(("pipe_d512_aa_warm", [
        ("cache_ab_aa_alpha", pipeline_args(512, WS["warm"], ops, warmup, "x"), "aa"),
        ("cache_ab_aa_beta", pipeline_args(512, WS["warm"], ops, warmup, "x"), "aa")]))
    pair_runs(bin_dir, out_path, pairs, reps, "pairs", shape_filter)


def stage_flush(bin_dir, out_path, reps, ops=40000, warmup=2000, shape_filter=None):
    pairs = []
    for depth in (64, 512):
        pairs.append((f"flush_d{depth}", [
            ("cache_ab", pipeline_args(depth, WS["warm"], ops, warmup, "x"), "warm"),
            ("cache_ab_flush", pipeline_args(depth, WS["warm"], ops, warmup, "x"), "flush")]))
    pair_runs(bin_dir, out_path, pairs, reps, "flush", shape_filter)


def stage_external(bin_dir, out_path, reps, ops=20000, warmup=1000, shape_filter=None):
    pairs = [
        ("ext_d64_split_ws16m", [
            ("cache_ab", external_args(64, WS["warm"], "split", "submitter", ops, warmup, "x"), "warm"),
            ("cache_ab", external_args(64, WS["16m"], "split", "submitter", ops, warmup, "x"), "ws16m")]),
        ("ext_d64_split_ws16m_driver", [
            ("cache_ab", external_args(64, WS["warm"], "split", "driver", ops, warmup, "x"), "warm"),
            ("cache_ab", external_args(64, WS["16m"], "split", "driver", ops, warmup, "x"), "ws16m")]),
        ("ext_d64_single_ws16m", [
            ("cache_ab", external_args(64, WS["warm"], "single", "submitter", ops, warmup, "x"), "warm"),
            ("cache_ab", external_args(64, WS["16m"], "single", "submitter", ops, warmup, "x"), "ws16m")]),
        ("ext_d64_split_aa_warm", [
            ("cache_ab_aa_alpha", external_args(64, WS["warm"], "split", "submitter", ops, warmup, "x"), "aa"),
            ("cache_ab_aa_beta", external_args(64, WS["warm"], "split", "submitter", ops, warmup, "x"), "aa")]),
    ]
    pair_runs(bin_dir, out_path, pairs, reps, "external", shape_filter)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", default="build-research")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage", default="all",
                    choices=["all", "probe", "pairs", "flush", "external"])
    ap.add_argument("--reps", type=int, default=15)
    ap.add_argument("--filter", default=None,
                    help="substring filter on shape keys (any stage)")
    args = ap.parse_args()
    open(args.out, "a").close()
    t0 = time.time()
    if args.stage in ("all", "probe"):
        stage_probe(args.bin_dir, args.out, 2, args.filter)
    if args.stage in ("all", "pairs"):
        stage_pairs(args.bin_dir, args.out, args.reps, shape_filter=args.filter)
    if args.stage in ("all", "flush"):
        stage_flush(args.bin_dir, args.out, args.reps, shape_filter=args.filter)
    if args.stage in ("all", "external"):
        stage_external(args.bin_dir, args.out, args.reps, shape_filter=args.filter)
    print(f"done in {time.time() - t0:.1f}s -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
