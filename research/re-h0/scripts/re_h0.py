#!/usr/bin/env python3
"""RE-H0 session runner (#277, authority #227; stop gate #262).

Drives the existing preregistered ladder binaries over a formal
measurement protocol and stores each session under an immutable
research/re-h0/results/<session-id>/ directory. The measured binaries
are UNCHANGED production-research code:

  tax0_z_ladder_bench      Z1/Z1b/Z1bw/Z2/Z3  (research/tax0/bench/)
  e1_abstraction_tax_bench L0/L1/L2           (bench/)

Frozen protocol (RE-H0-PREREGISTRATION.md; never tuned after formal
data exists):

  * cells   S = 4 KiB x d8 (256 MiB useful bytes/rep)
            L = 2 MiB x d2 (1 GiB useful bytes/rep), workers = 1 (z3)
  * arms    re1u: z1 z1b z1bw z2 z3
            re1:  L0 L1 L2 (e1, --workers = depth)
  * fs      btrfs (build/re-h0-data) primary; /tmp tmpfs control
  * reps    W launch: 11 measured reps (wall/user/sys samples)
            perf: two independent (R=7, R=14) double-difference pairs
            warmup = 2 for every launch
  * order   blocked-interleaved: per (fs,cell,op) block the arm order
            is shuffled with the frozen seed; an arm's launches stay
            adjacent (the R7/R14 pair must share machine state)
  * perf    perf stat -x, -e instructions:u,cycles:u wrapping every
            perf launch; per-rep work = (R14 - R7)/7 (double difference)
  * pin     taskset -c 2-9 for every launch (uniform)
  * write   sync + 0.3 s settle after every WRITE launch (both fs)
  * verify  z arms: --runner-verify + one runner-side byte compare per
            combo (after the last launch, TAX-0B precedent);
            e1 arms: internal untimed final verification
  * NO RETRIES: any nonzero exit, parse failure or same-work witness
            failure marks the combo invalid and fails the session
            (qual262 additionally trips the #262 campaign stop gate).
"""

import argparse
import csv
import datetime
import hashlib
import json
import os
import pathlib
import platform
import random
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
Z_BENCH = REPO / "build/linux/x86_64/release/tax0_z_ladder_bench"
E1_BENCH = REPO / "build/linux/x86_64/release/e1_abstraction_tax_bench"
RESULTS_ROOT = REPO / "research/re-h0/results"
SCRIPTS = pathlib.Path(__file__).resolve().parent

BTRFS_DATA = REPO / "build/re-h0-data"
TMPFS_DATA = pathlib.Path("/tmp/re-h0-data")

CELL_S = {"request_size": 4096, "depth": 8, "total_bytes": 256 * 1024 * 1024}
CELL_L = {"request_size": 2 * 1024 * 1024, "depth": 2,
          "total_bytes": 1024 * 1024 * 1024}
CELLS = {"S": CELL_S, "L": CELL_L}

# RE-2 representative envelope (preregistration P11; frozen shape)
CELLS_RE2 = {
    "4Kd1": {"request_size": 4096, "depth": 1,
             "total_bytes": 256 * 1024 * 1024},
    "4Kd8": {"request_size": 4096, "depth": 8,
             "total_bytes": 256 * 1024 * 1024},
    "64Kd2": {"request_size": 65536, "depth": 2,
              "total_bytes": 256 * 1024 * 1024},
    "2Md1": {"request_size": 2 * 1024 * 1024, "depth": 1,
             "total_bytes": 1024 * 1024 * 1024},
    "2Md2": {"request_size": 2 * 1024 * 1024, "depth": 2,
             "total_bytes": 1024 * 1024 * 1024},
}
RE2U_ARMS = ["z1b", "z2"]   # uring floor vs Sluice uring backend
RE2P_ARMS = ["L1", "L2"]    # pool floor vs Sluice ThreadPool path

RE1U_ARMS = ["z1", "z1b", "z1bw", "z2", "z3"]
RE1_ARMS = ["L0", "L1", "L2"]
OPS = ["read", "write"]

R_WALL = 11
WARMUP = 2
PERF_PAIRS = [(7, 14), (7, 14)]
PERF_EVENTS = ["instructions:u", "cycles:u"]
ORDER_SEED = 20260903
PIN_CPUS = "2-9"
WRITE_SETTLE_S = 0.3


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_first(path, default=None):
    try:
        return pathlib.Path(path).read_text().strip()
    except OSError:
        return default


def fs_type(path):
    try:
        out = subprocess.check_output(["df", "-T", str(path)],
                                      text=True).strip().splitlines()
        return out[-1].split()[1]
    except Exception:
        return None


def tool_version(cmd):
    try:
        return subprocess.check_output(cmd, text=True,
                                       stderr=subprocess.STDOUT).strip()
    except Exception:
        return None


def capture_environment(session_dir):
    env = {}
    env["timestamp_utc"] = datetime.datetime.now(
        datetime.timezone.utc).isoformat()
    def g(*a):
        r = subprocess.run(["git", *a], cwd=REPO, capture_output=True,
                           text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    env["git"] = {"head": g("rev-parse", "HEAD"),
                  "branch": g("rev-parse", "--abbrev-ref", "HEAD"),
                  "dirty": g("status", "--porcelain") != ""}
    env["system"] = {
        "kernel": platform.release(),
        "wsl": "microsoft" in platform.release().lower(),
        "cpu_model": next((l.split(":", 1)[1].strip()
                           for l in read_first("/proc/cpuinfo", "")
                           .splitlines() if l.startswith("model name")),
                          None),
        "logical_cpus": os.cpu_count(),
        "affinity": sorted(os.sched_getaffinity(0)),
        "governor": read_first(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "perf_event_paranoid": read_first(
            "/proc/sys/kernel/perf_event_paranoid"),
        "cache_line_bytes": read_first(
            "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size"),
        "page_size_bytes": os.sysconf("SC_PAGESIZE"),
    }
    env["filesystems"] = {
        "btrfs_data": {"dir": str(BTRFS_DATA), "type": fs_type(BTRFS_DATA),
                       "role": "primary real-storage path",
                       "mount": "btrfs compress=zstd:1 ssd on SATA SSD"},
        "tmpfs_data": {"dir": str(TMPFS_DATA), "type": fs_type(TMPFS_DATA),
                       "role": "storage-latency control; never supports a "
                               "real-I/O near-native claim"},
    }
    env["block_devices"] = read_first("/proc/partitions")
    env["tools"] = {"perf": tool_version(["perf", "--version"]),
                    "python": platform.python_version(),
                    "liburing": tool_version(
                        ["pkg-config", "--modversion", "liburing"])}
    env["build"] = {
        "mode": "release",
        "toolchain": "clang",
        "z_bench_sha256": sha256_file(Z_BENCH) if Z_BENCH.exists() else None,
        "e1_bench_sha256": sha256_file(E1_BENCH) if E1_BENCH.exists() else None,
        "compiler": (tool_version(["clang", "--version"]) or "").splitlines()[
            :1],
    }
    (session_dir / "environment.json").write_text(
        json.dumps(env, indent=2) + "\n")
    return env


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def z_cmd(arm, cell, op, data_root, reps):
    return [str(Z_BENCH), "--arm", arm, "--op", op,
            "--file", str(data_root / f"data-{cell['request_size']}.bin"),
            "--request-size", str(cell["request_size"]),
            "--total-bytes", str(cell["total_bytes"]),
            "--depth", str(cell["depth"]),
            "--workers", "1",
            "--reps", str(reps),
            "--warmup", str(WARMUP),
            "--runner-verify"]


def e1_cmd(ladder, cell, op, data_root, reps):
    return [str(E1_BENCH), "--ladder", ladder, "--op", op,
            "--file", str(data_root / f"data-{cell['request_size']}.bin"),
            "--request-size", str(cell["request_size"]),
            "--total-bytes", str(cell["total_bytes"]),
            "--depth", str(cell["depth"]),
            "--workers", str(cell["depth"]),
            "--reps", str(reps),
            "--warmup", str(WARMUP)]


def perf_wrap(cmd):
    return ["perf", "stat", "-x,", "-e", ",".join(PERF_EVENTS),
            "--", "taskset", "-c", PIN_CPUS] + cmd


def plain_wrap(cmd):
    return ["taskset", "-c", PIN_CPUS] + cmd


def parse_perf(stderr_text):
    counters = {}
    for line in stderr_text.splitlines():
        parts = line.split(",")
        if len(parts) >= 3 and parts[0]:
            key = parts[2].split(":")[0]
            try:
                counters[key] = float(parts[0])
            except ValueError:
                pass
    return counters


def runner_verify_write(data_path, total_bytes):
    """Byte-exact runner-side verification of a --runner-verify WRITE arm
    (same master block as the benches; outside every measurement window)."""
    import struct
    kblock = 4096
    seed = 0xE1E1E1E121212121
    master = bytearray(kblock)
    for i in range(kblock // 8):
        x = (seed + i + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        struct.pack_into("<Q", master, i * 8, x ^ (x >> 31))
    block = bytes(master)
    done = 0
    with open(data_path, "rb") as f:
        while done < total_bytes:
            chunk = f.read(kblock)
            if chunk != block:
                return False
            done += len(chunk)
    return True


class Session:
    def __init__(self, name):
        self.dir = RESULTS_ROOT / name
        if self.dir.exists():
            sys.exit(f"refusing to overwrite session dir: {self.dir}")
        (self.dir / "raw").mkdir(parents=True)
        self.manifest = {"protocol": {
            "r_wall": R_WALL, "warmup": WARMUP,
            "perf_pairs": PERF_PAIRS, "perf_events": PERF_EVENTS,
            "order_seed": ORDER_SEED, "pin_cpus": PIN_CPUS,
            "normalization": "double-difference (total(R14)-total(R7))/7/ops",
            "write_policy": "buffered writeback only, no fsync; sync+0.3s "
                            "settle after every WRITE launch",
            "retries": "NONE (fail-closed; #262 stop gate honored)"},
            "launches": [], "failures": []}
        self.rows = []

    def launch(self, cmd, tag, use_perf):
        full = perf_wrap(cmd) if use_perf else plain_wrap(cmd)
        proc = subprocess.run(full, capture_output=True, text=True,
                              env=dict(os.environ, LC_ALL="C"))
        (self.dir / "raw" / f"{tag}.cmd.txt").write_text(" ".join(full) + "\n")
        (self.dir / "raw" / f"{tag}.stderr.txt").write_text(proc.stderr)
        (self.dir / "raw" / f"{tag}.stdout.txt").write_text(proc.stdout)
        self.manifest["launches"].append(
            {"tag": tag, "rc": proc.returncode, "perf": use_perf})
        return proc

    def fail(self, msg):
        self.manifest["failures"].append(msg)
        print(f"[INVALID] {msg}")


def _invalid_row(family, arm, cell_key, cell, op, fs, note):
    return {
        "family": family, "fs": fs, "op": op,
        "request_size": cell["request_size"], "depth": cell["depth"],
        "workers": 1 if family.endswith("u") else cell["depth"],
        "arm": arm, "cell": cell_key, "ok": False, "ops": None,
        "total_bytes": cell["total_bytes"], "word_sum": None,
        "wall_ns_per_op_samples": [], "instr_u_per_op_estimates": None,
        "user_ns_per_op": None, "sys_ns_per_op": None,
        "write_verified": False, "binary_sha256": "", "error_note": note,
    }


def one_combo(session, family, arm, cell_key, cell, op, data_root, fs,
              sha):
    """All launches for one (family, arm, cell, op, fs) combo; returns a
    normalized row. Any failure records an INVALID combo (no retries)."""
    rs = cell["request_size"]
    data = data_root / f"data-{rs}.bin"
    base_tag = f"{family}-{fs}-{cell_key}-{op}-{arm}"
    if family.endswith("u"):
        cmd = lambda reps: z_cmd(arm, cell, op, data_root, reps)
    else:
        cmd = lambda reps: e1_cmd(arm, cell, op, data_root, reps)

    proc = session.launch(cmd(R_WALL), f"{base_tag}-W{R_WALL}", use_perf=False)
    if proc.returncode != 0:
        session.fail(f"{base_tag}: wall launch rc={proc.returncode}")
        return _invalid_row(family, arm, cell_key, cell, op, fs,
                            proc.stderr[-400:])
    try:
        bench = json.loads(proc.stdout.strip())
    except ValueError:
        session.fail(f"{base_tag}: unparseable bench JSON")
        return _invalid_row(family, arm, cell_key, cell, op, fs,
                            proc.stdout[-200:])
    if family == "re1" and not bench.get("all_reps_ok"):
        session.fail(f"{base_tag}: all_reps_ok false")
        return _invalid_row(family, arm, cell_key, cell, op, fs,
                            "all_reps_ok false")

    ests = []
    k = 0
    for r_lo, r_hi in PERF_PAIRS:
        p_lo = session.launch(cmd(r_lo), f"{base_tag}-P{r_lo}-{k}",
                              use_perf=True)
        k += 1
        p_hi = session.launch(cmd(r_hi), f"{base_tag}-P{r_hi}-{k}",
                              use_perf=True)
        k += 1
        if p_lo.returncode != 0 or p_hi.returncode != 0:
            session.fail(f"{base_tag}: perf launch rc="
                         f"{p_lo.returncode}/{p_hi.returncode}")
            return _invalid_row(family, arm, cell_key, cell, op, fs,
                                (p_lo.stderr + p_hi.stderr)[-400:])
        c_lo = parse_perf(p_lo.stderr)
        c_hi = parse_perf(p_hi.stderr)
        if "instructions" not in c_lo or "instructions" not in c_hi:
            session.fail(f"{base_tag}: missing instructions counter")
            return _invalid_row(family, arm, cell_key, cell, op, fs,
                                "perf counters missing")
        ests.append((c_hi["instructions"] - c_lo["instructions"])
                    / (r_hi - r_lo) / bench["ops"])

    ops = bench["ops"]
    reps = bench["reps"] if family.endswith("u") else bench["reps_out"]
    row = {
        "family": family,
        "fs": fs,
        "op": op,
        "request_size": rs,
        "depth": cell["depth"],
        "workers": 1 if family.endswith("u") else cell["depth"],
        "arm": arm,
        "cell": cell_key,
        "ok": True,
        "ops": ops,
        "total_bytes": cell["total_bytes"],
        "word_sum": reps[0]["word_sum"] if op == "read" else None,
        "wall_ns_per_op_samples": [r["wall_ns"] / ops for r in reps],
        "instr_u_per_op_estimates": ests,
        "user_ns_per_op": sum(r["user_ns"] for r in reps) / len(reps) / ops,
        "sys_ns_per_op": sum(r["sys_ns"] for r in reps) / len(reps) / ops,
        "write_verified": True,
        "binary_sha256": sha,
        "error_note": "",
    }
    if op == "write":
        if family.endswith("u"):
            # one byte-exact verify per combo, after the last launch
            # (file left by the final P launch; TAX-0B precedent)
            row["write_verified"] = runner_verify_write(data,
                                                        cell["total_bytes"])
        else:
            row["write_verified"] = bool(bench.get("all_reps_ok"))
        if not row["write_verified"]:
            row["ok"] = False
            session.fail(f"{base_tag}: write verification mismatch")
    return row


def prepare_data_files(cells, data_root):
    data_root.mkdir(parents=True, exist_ok=True)
    for key in cells:
        cell = cells[key]
        data = data_root / f"data-{cell['request_size']}.bin"
        if data.exists() and data.stat().st_size == cell["total_bytes"]:
            continue
        cmd = [str(Z_BENCH), "--arm", "z1", "--op", "write",
               "--file", str(data),
               "--request-size", str(cell["request_size"]),
               "--total-bytes", str(cell["total_bytes"]),
               "--depth", "1", "--reps", "1", "--warmup", "0"]
        subprocess.run(cmd, check=True, capture_output=True, text=True)


def shuffled_arms(arms, cell_key, op, fs):
    # deterministic across processes: seed from the stable block key
    # (never hash(): PYTHONHASHSEED randomizes str hashing per process)
    rng = random.Random(f"{ORDER_SEED}:{fs}:{cell_key}:{op}")
    order = list(arms)
    rng.shuffle(order)
    return order


def median_wall(row):
    s = sorted(row["wall_ns_per_op_samples"])
    return s[len(s) // 2]


# ---------------------------------------------------------------------------
# actions
# ---------------------------------------------------------------------------

def _common_session_setup(args, prefix):
    name = args.session or f"{prefix}-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    s = Session(name)
    capture_environment(s.dir)
    return s


def finish_session(s):
    (s.dir / "manifest.json").write_text(
        json.dumps(s.manifest, indent=1) + "\n")
    (s.dir / "summary.json").write_text(json.dumps(s.rows, indent=1) + "\n")
    if s.rows:
        fields = sorted({k for r in s.rows for k in r})
        with open(s.dir / "summary.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore",
                           lineterminator="\n")
            w.writeheader()
            w.writerows(s.rows)
    print(f"session: {s.dir}")
    return 1 if s.manifest["failures"] else 0


def action_qual262(args):
    """#262 pre-measurement stop gate: N perf-wrapped launches per
    (arm in {z2,z3} x op x cell in {S,L}). ANY unexpected terminal,
    teardown failure or accounting mismatch -> STOP, evidence kept,
    no retry, no continuation."""
    s = _common_session_setup(args, "re-h0-qual262")
    gates = {"expected": 0, "passed": 0, "unexpected_ecanceled": 0,
             "named_drain_stall": 0, "wait_error": 0, "teardown_failure": 0,
             "same_work_mismatch": 0, "other_failure": 0, "details": []}
    prepare_data_files(CELLS, BTRFS_DATA)
    prepare_data_files(CELLS, TMPFS_DATA)
    for cell_key in ("S", "L"):
        cell = CELLS[cell_key]
        for op in OPS:
            for arm in ("z2", "z3"):
                for i in range(args.n):
                    tag = f"qual-{cell_key}-{op}-{arm}-{i:02d}"
                    proc = s.launch(
                        z_cmd(arm, cell, op, BTRFS_DATA, 1), tag,
                        use_perf=True)
                    gates["expected"] += 1
                    if proc.returncode == 0:
                        try:
                            json.loads(proc.stdout.strip())
                            gates["passed"] += 1
                            continue
                        except ValueError:
                            kind = "same_work_mismatch"
                            detail = proc.stdout[-400:]
                    else:
                        text = (proc.stdout + proc.stderr).lower()
                        if "canceled" in text:
                            kind = "unexpected_ecanceled"
                        elif "drain" in text:
                            kind = "named_drain_stall"
                        elif "wait" in text:
                            kind = "wait_error"
                        elif "terminate" in text or "abort" in text:
                            kind = "teardown_failure"
                        else:
                            kind = "other_failure"
                        detail = (proc.stdout + proc.stderr)[-400:]
                    gates[kind] += 1
                    gates["details"].append(
                        {"tag": tag, "kind": kind, "evidence": detail})
                    s.fail(f"{tag}: {kind}")
                    (s.dir / "gates.json").write_text(
                        json.dumps(gates, indent=1) + "\n")
                    print("RE-H0 STOP: #262 stop gate tripped — evidence in "
                          f"{s.dir / 'gates.json'}; belongs in #262. "
                          "No retry, no continuation.")
                    finish_session(s)
                    return 2
    (s.dir / "gates.json").write_text(json.dumps(gates, indent=1) + "\n")
    print(f"#262 qualification: {gates['passed']}/{gates['expected']} "
          f"clean (0 surprises)")
    return finish_session(s)


def _run_formal(args, prefix, family, arms, cells):
    s = _common_session_setup(args, prefix)
    env = json.loads((s.dir / "environment.json").read_text())
    sha = (env["build"]["z_bench_sha256"] if family.endswith("u")
           else env["build"]["e1_bench_sha256"])
    for root, fs in ((BTRFS_DATA, "btrfs"), (TMPFS_DATA, "tmpfs")):
        prepare_data_files(cells, root)
        for cell_key in cells:
            cell = cells[cell_key]
            for op in OPS:
                order = shuffled_arms(arms, cell_key, op, fs)
                s.manifest.setdefault("arm_order", []).append(
                    {"fs": fs, "cell": cell_key, "op": op, "order": order})
                for arm in order:
                    row = one_combo(s, family, arm, cell_key, cell, op,
                                    root, fs, sha)
                    s.rows.append(row)
                    if row["ok"]:
                        est = row["instr_u_per_op_estimates"]
                        print(f"[OK] {fs} {cell_key} {op} {arm}: "
                              f"instr/op={est[0]:.0f}/{est[1]:.0f} "
                              f"wall/op={median_wall(row):.0f}ns")
                    if op == "write":
                        time.sleep(WRITE_SETTLE_S)
                subprocess.run(["sync"], check=False)
    return finish_session(s)


def action_re1(args):
    return _run_formal(args, "re-h0-re1", "re1", RE1_ARMS, CELLS)


def action_re1u(args):
    return _run_formal(args, "re-h0-re1u", "re1u", RE1U_ARMS, CELLS)


def action_re2(args):
    # two sub-ladders per cell: uring (z1b->z2) and pool (L1->L2)
    rc = 0
    rc |= _run_formal(args, "re-h0-re2u", "re2u", RE2U_ARMS, CELLS_RE2)
    rc |= _run_formal(args, "re-h0-re2p", "re2p", RE2P_ARMS, CELLS_RE2)
    return rc


def action_analyze(args):
    sys.path.insert(0, str(SCRIPTS))
    import re_h0_analysis as an
    sdir = RESULTS_ROOT / args.session
    rows = json.loads((sdir / "summary.json").read_text())
    manifest = json.loads((sdir / "manifest.json").read_text())
    if manifest.get("failures"):
        sys.exit(f"session has recorded failures; analysis refuses to "
                 f"aggregate: {sdir}")
    env = json.loads((sdir / "environment.json").read_text())
    sha_z = env["build"]["z_bench_sha256"]
    sha_e1 = env["build"]["e1_bench_sha256"]
    results = {"blocks": [], "errors": []}
    bad = False
    for fs in ("btrfs", "tmpfs"):
        for cell_key in ("S", "L"):
            cell = CELLS[cell_key]
            for op in OPS:
                for family in ("re1u", "re1"):
                    sel = [r for r in rows
                           if r.get("family") == family
                           and r.get("fs") == fs and r.get("op") == op
                           and r.get("cell") == cell_key]
                    if not sel:
                        continue
                    head = (f"{family} {fs} {cell_key} {op} "
                            f"r{cell['request_size']}d{cell['depth']}")
                    try:
                        if family == "re1u":
                            v = an.re1u_ladder_verdict(
                                sel, fs, op, cell["request_size"],
                                cell["depth"], expected_binary_sha256=sha_z)
                            print(f"{head}: "
                                  f"C_sem={v['C_sem']['ratio']:.3f}"
                                  f"({v['C_sem']['verdict']}) "
                                  f"T_backend={v['T_backend']['ratio']:.3f}"
                                  f"({v['T_backend']['verdict']}) "
                                  f"C_cont={v['C_cont']['ratio']:.3f}"
                                  f"({v['C_cont']['verdict']}) "
                                  f"T_runtime={v['T_runtime']['ratio']:.3f}"
                                  f"({v['T_runtime']['verdict']}) "
                                  f"-> {v['case']}")
                        else:
                            v = an.re1_ladder_verdict(
                                sel, fs, op, cell["request_size"],
                                cell["depth"], cell["depth"],
                                expected_binary_sha256=sha_e1)
                            print(f"{head}: "
                                  f"T_pool={v['T_pool']['ratio']:.3f}"
                                  f"({v['T_pool']['verdict']}) "
                                  f"T_sluice={v['T_sluice']['ratio']:.3f}"
                                  f"({v['T_sluice']['verdict']})")
                        results["blocks"].append(v)
                    except (an.SessionInvalid, an.IndeterminateMetric) as e:
                        results["errors"].append(
                            {"family": family, "fs": fs, "op": op,
                             "cell": cell_key, "error": str(e),
                             "kind": type(e).__name__})
                        bad = True
                        print(f"[ANALYSIS-FAIL] {head}: {type(e).__name__}: "
                              f"{e}")
        for cell_key, cell in CELLS_RE2.items():
            for op in OPS:
                for family, cand, base, sha in (
                        ("re2u", "z2", "z1b", sha_z),
                        ("re2p", "L2", "L1", sha_e1)):
                    sel = [r for r in rows
                           if r.get("family") == family
                           and r.get("fs") == fs and r.get("op") == op
                           and r.get("cell") == cell_key]
                    if len(sel) < 2:
                        continue
                    head = (f"{family} {fs} {cell_key} {op} "
                            f"r{cell['request_size']}d{cell['depth']}")
                    try:
                        v = an.pair_verdict(
                            sel, fs, op, cell["request_size"], cell["depth"],
                            cand, base,
                            workers=None if family == "re2u" else cell[
                                "depth"],
                            expected_binary_sha256=sha)
                        print(f"{head}: {cand}/{base}="
                              f"{v['ratio']:.3f}({v['verdict']})")
                        results["blocks"].append(
                            {"block": {"family": family, "fs": fs,
                                       "op": op, "cell": cell_key,
                                       "request_size": cell["request_size"],
                                       "depth": cell["depth"]},
                             "pair": v})
                    except (an.SessionInvalid, an.IndeterminateMetric) as e:
                        results["errors"].append(
                            {"family": family, "fs": fs, "op": op,
                             "cell": cell_key, "error": str(e),
                             "kind": type(e).__name__})
                        bad = True
                        print(f"[ANALYSIS-FAIL] {head}: {type(e).__name__}: "
                              f"{e}")
    (sdir / "analysis.json").write_text(json.dumps(results, indent=1) + "\n")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["env", "qual262", "re1", "re1u",
                                       "re2", "analyze"],
                    default="re1u", nargs="?")
    ap.add_argument("--session", default=None)
    ap.add_argument("--n", type=int, default=20,
                    help="qual262 launches per combo (frozen: 20)")
    args = ap.parse_args()
    if not Z_BENCH.exists() or not E1_BENCH.exists():
        sys.exit("bench binaries missing: xmake build -r "
                 "tax0_z_ladder_bench e1_abstraction_tax_bench")
    if args.action == "env":
        d = RESULTS_ROOT / (
            args.session
            or f"re-h0-env-{datetime.datetime.now():%Y%m%d-%H%M%S}")
        d.mkdir(parents=True, exist_ok=False)
        print(json.dumps(capture_environment(d), indent=2))
        return 0
    if args.action == "qual262":
        return action_qual262(args)
    if args.action == "re1":
        return action_re1(args)
    if args.action == "re1u":
        return action_re1u(args)
    if args.action == "re2":
        return action_re2(args)
    if args.action == "analyze":
        return action_analyze(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
