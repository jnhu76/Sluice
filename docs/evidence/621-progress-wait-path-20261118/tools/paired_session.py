#!/usr/bin/env python3
"""Paired A/A (future A/B) session runner for the #621 campaign.

One invocation measures one cell.  Arms are strictly serial and alternate
AB/BA by pair ordinal (odd -> AB, even -> BA).  Each arm runs under
``perf stat`` and ``taskset``, is timed with a monotonic clock and
``RUSAGE_CHILDREN``, and is checked by the oracle of its kind.  The runner
writes raw evidence, ``arms.jsonl``, ``pairs.tsv`` and ``meta.json``; it does
not compute a verdict and never handles credentials.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

TOOL_VERSION = "1.1"
ARM_TIMEOUT_S = 3600.0
SAMPLE_INTERVAL_S = 0.02
PERF = shutil.which("perf") or "/usr/sbin/perf"
TASKSET = shutil.which("taskset") or "/usr/sbin/taskset"

COUNTER_RE = re.compile(
    r"copied (\d+) bytes \(read_ops=(\d+) write_ops=(\d+) short_writes=(\d+) "
    r"sync=(\w+) pipeline_depth=(\d+) atomic=(\w+)\)"
)
PERF_SCALE_RE = re.compile(r"\(\s*\d+(?:\.\d+)?\s*%\s*\)")
PERF_BAD_MARKERS = (
    "<not counted>",
    "<not supported>",
    "not supported",
    "multiplex",
    "scaled",
)
PROC_STAT_FIELDS = (
    "user",
    "nice",
    "system",
    "idle",
    "iowait",
    "irq",
    "softirq",
    "steal",
    "guest",
    "guest_nice",
)
TSV_COLUMNS = (
    "pair",
    "order",
    "position",
    "arm",
    "kind",
    "cell",
    "valid",
    "invalid_reasons",
    "rc",
    "timed_out",
    "wall_s",
    "cpu_user_s",
    "cpu_sys_s",
    "cpu_total_s",
    "cpu_us",
    "perf_ok",
    "perf_task_clock",
    "perf_task_clock_unit",
    "perf_instructions",
    "perf_cycles",
    "oracle_ok",
    "oracle_detail",
    "read_ops",
    "write_ops",
    "short_writes",
    "bytes_copied",
    "dst_sha256",
    "case_measured_n",
    "case_measured_wall_s_mean",
    "case_measured_ops_per_sec_mean",
    "case_measured_cpu_s_mean",
    "freq_min_khz",
    "freq_max_khz",
    "temp_max_mc",
    "start",
    "end",
    "raw_stdout",
    "raw_stderr",
    "raw_perf",
    "raw_jsonl",
)


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def sha256_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def read_text(path: str):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def read_int(path: str):
    text = read_text(path)
    if text is None:
        return None
    try:
        return int(text.strip())
    except ValueError:
        return None


def mean(values):
    return sum(values) / len(values) if values else None


def sample_std(values):
    if len(values) < 2:
        return None
    m = mean(values)
    return (sum((v - m) ** 2 for v in values) / (len(values) - 1)) ** 0.5


def parse_cpu_list(text: str):
    cpus = set()
    for part in text.split(","):
        part = part.strip()
        if not part:
            raise ValueError(f"empty CPU entry in {text!r}")
        if "-" in part:
            lo_text, hi_text = part.split("-", 1)
            lo, hi = int(lo_text), int(hi_text)
            if lo > hi:
                raise ValueError(f"reversed CPU range {part!r}")
            cpus.update(range(lo, hi + 1))
        else:
            cpus.add(int(part))
    if not cpus:
        raise ValueError(f"no CPUs in {text!r}")
    return sorted(cpus)


def all_cpu_numbers():
    cpus = []
    base = "/sys/devices/system/cpu"
    for name in os.listdir(base):
        if not name.startswith("cpu") or not name[3:].isdigit():
            continue
        if os.path.exists(os.path.join(base, name, "cpufreq", "scaling_governor")):
            cpus.append(int(name[3:]))
    return sorted(cpus)


def governor_snapshot():
    snap = {}
    for cpu in all_cpu_numbers():
        root = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq"
        snap[str(cpu)] = {
            "governor": (read_text(os.path.join(root, "scaling_governor")) or "").strip() or None,
            "min_freq_khz": read_int(os.path.join(root, "scaling_min_freq")),
            "max_freq_khz": read_int(os.path.join(root, "scaling_max_freq")),
            "cur_freq_khz": read_int(os.path.join(root, "scaling_cur_freq")),
            "driver": (read_text(os.path.join(root, "scaling_driver")) or "").strip() or None,
            "energy_performance_preference": (
                read_text(os.path.join(root, "energy_performance_preference")) or ""
            ).strip()
            or None,
        }
    return snap


def no_turbo():
    return read_int("/sys/devices/system/cpu/intel_pstate/no_turbo")


def thermal_snapshot():
    zones = []
    base = "/sys/class/thermal"
    try:
        names = sorted(n for n in os.listdir(base) if n.startswith("thermal_zone"))
    except OSError:
        return zones
    for name in names:
        root = os.path.join(base, name)
        zones.append(
            {
                "zone": name,
                "type": (read_text(os.path.join(root, "type")) or "").strip() or None,
                "temp_mc": read_int(os.path.join(root, "temp")),
            }
        )
    return zones


def read_proc_stat():
    text = read_text("/proc/stat")
    if text is None:
        return {}
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if not parts or not parts[0].startswith("cpu"):
            continue
        try:
            out[parts[0]] = [int(v) for v in parts[1:]]
        except ValueError:
            continue
    return out


def proc_stat_delta(before, after, cpus):
    def one(name):
        b = before.get(name)
        a = after.get(name)
        if b is None or a is None:
            return None
        n = min(len(b), len(a))
        delta = [a[i] - b[i] for i in range(n)]
        total = sum(delta[:8])
        idle = (delta[3] if n > 3 else 0) + (delta[4] if n > 4 else 0)
        return {
            "before": b,
            "after": a,
            "delta": delta,
            "total_ticks": total,
            "idle_ticks": idle,
            "busy_ticks": total - idle,
            "busy_fraction": (total - idle) / total if total else None,
        }

    result = {"aggregate": one("cpu"), "per_cpu": {}}
    for cpu in cpus:
        result["per_cpu"][str(cpu)] = one(f"cpu{cpu}")
    return result


def meminfo_summary():
    text = read_text("/proc/meminfo")
    if text is None:
        return {}
    keep = {"MemTotal", "MemFree", "MemAvailable", "Dirty", "Writeback", "Cached"}
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].endswith(":"):
            key = parts[0][:-1]
            if key in keep:
                try:
                    out[key] = int(parts[1])
                except ValueError:
                    pass
    return out


STORAGE_IRQ_RE = re.compile(
    r"(nvme|ahci|sata|\bata\d|scsi|virtio|mmc|xvd|\bsd[a-z]|\bvd[a-z]|blk)",
    re.IGNORECASE,
)
PS_TOP_LIMIT = 20


def loadavg_snapshot():
    return {
        "at": now_iso(),
        "loadavg": list(os.getloadavg()),
        "loadavg_raw": (read_text("/proc/loadavg") or "").strip() or None,
    }


def background_load_snapshot():
    ps = run_capture(
        ["ps", "-eo", "pid,ppid,stat,pcpu,pmem,comm", "--sort=-pcpu"], timeout=10.0
    )
    lines = ps["stdout"].splitlines()
    rows = lines[1:]
    snapshot = loadavg_snapshot()
    snapshot.update(
        {
            "ps_rc": ps["rc"],
            "ps_stderr": ps["stderr"],
            "ps_header": lines[0] if lines else None,
            "ps_processes": len(rows),
            "ps_top_by_cpu": rows[:PS_TOP_LIMIT],
            "ps_top_truncated": len(rows) > PS_TOP_LIMIT,
        }
    )
    return snapshot


def irq_actions():
    text = read_text("/proc/interrupts")
    if text is None:
        return None
    actions = {}
    for line in text.splitlines():
        head, sep, rest = line.partition(":")
        if not sep or not head.strip().isdigit():
            continue
        tokens = rest.split()
        index = 0
        while index < len(tokens) and re.fullmatch(r"\d+", tokens[index]):
            index += 1
        actions[int(head.strip())] = " ".join(tokens[index:])
    return actions


def irq_affinity_snapshot():
    actions = irq_actions()
    try:
        names = os.listdir("/proc/irq")
    except OSError:
        names = []
    entries = []
    for name in sorted((n for n in names if n.isdigit()), key=int):
        root = os.path.join("/proc/irq", name)
        entries.append(
            {
                "irq": int(name),
                "actions": (actions or {}).get(int(name)),
                "smp_affinity": (read_text(os.path.join(root, "smp_affinity")) or "").strip()
                or None,
                "smp_affinity_list": (
                    read_text(os.path.join(root, "smp_affinity_list")) or ""
                ).strip()
                or None,
                "effective_affinity_list": (
                    read_text(os.path.join(root, "effective_affinity_list")) or ""
                ).strip()
                or None,
            }
        )
    storage = [
        entry
        for entry in entries
        if entry["actions"] and STORAGE_IRQ_RE.search(entry["actions"])
    ]
    return {
        "source": "/proc/interrupts + /proc/irq/*/smp_affinity[_list]",
        "interrupts_readable": actions is not None,
        "irq_total": len(entries),
        "storage_irqs": storage,
    }


def unescape_mount(text: str) -> str:
    return re.sub(
        r"\\([0-7]{3})",
        lambda m: chr(int(m.group(1), 8)),
        text,
    )


def existing_anchor(path: str) -> str:
    probe = os.path.abspath(path)
    while not os.path.exists(probe):
        parent = os.path.dirname(probe)
        if parent == probe:
            return probe
        probe = parent
    return probe


def mount_info(path: str):
    anchor = existing_anchor(path)
    real = os.path.realpath(anchor)
    mount = None
    text = read_text("/proc/self/mountinfo")
    if text:
        for line in text.splitlines():
            fields = line.split()
            if " - " not in line:
                continue
            sep = fields.index("-")
            pre = fields[:sep]
            post = fields[sep + 1:]
            if len(pre) < 6 or len(post) < 3:
                continue
            mp = unescape_mount(pre[4])
            if real == mp or real.startswith(mp.rstrip("/") + "/"):
                if mount is None or len(mp) > len(mount["mount_point"]):
                    mount = {
                        "mount_point": mp,
                        "options": pre[5],
                        "fs_type": post[0],
                        "source": unescape_mount(post[1]),
                        "super_options": post[2],
                    }
    info = {
        "path": path,
        "anchor": anchor,
        "realpath": real,
        "exists": os.path.exists(path),
        "mount": mount,
    }
    try:
        st = os.stat(anchor)
        info["st_dev"] = st.st_dev
    except OSError:
        info["st_dev"] = None
    try:
        vfs = os.statvfs(anchor)
        info["statvfs"] = {
            "f_bsize": vfs.f_frsize,
            "f_blocks": vfs.f_blocks,
            "f_bfree": vfs.f_bfree,
            "f_bavail": vfs.f_bavail,
        }
    except OSError:
        info["statvfs"] = None
    return info


def run_capture(argv, timeout=10.0):
    try:
        proc = subprocess.run(
            argv, capture_output=True, text=True, timeout=timeout, check=False
        )
        return {
            "argv": argv,
            "rc": proc.returncode,
            "stdout": proc.stdout.strip(),
            "stderr": proc.stderr.strip(),
        }
    except (OSError, subprocess.SubprocessError) as exc:
        return {"argv": argv, "rc": None, "stdout": "", "stderr": f"{type(exc).__name__}: {exc}"}


def build_config(binary: str):
    cfg = {
        "binary": binary,
        "sha256": sha256_file(binary),
        "size": os.path.getsize(binary),
        "mtime": datetime.fromtimestamp(os.path.getmtime(binary)).astimezone().isoformat(
            timespec="seconds"
        ),
    }
    if shutil.which("file"):
        cfg["file"] = run_capture(["file", "-b", binary])["stdout"]
    if shutil.which("readelf"):
        comment = run_capture(["readelf", "-p", ".comment", binary])
        cfg["comment"] = [
            line.strip()
            for line in comment["stdout"].splitlines()
            if line.strip() and not line.strip().startswith("String dump")
        ]
        dynamic = run_capture(["readelf", "-d", binary])
        needed = re.findall(r"Shared library: \[([^\]]+)\]", dynamic["stdout"])
        cfg["needed"] = needed
        cfg["liburing_linked"] = any("liburing" in n for n in needed)
        sections = run_capture(["readelf", "-S", binary])
        cfg["debug_info"] = ".debug_info" in sections["stdout"]
        cfg["dwarf"] = ".debug_info" in sections["stdout"]
    match = re.search(r"(.*/build[^/]*)/linux/[^/]+/([^/]+)/", binary)
    cfg["inferred_build_dir"] = match.group(1) if match else None
    cfg["inferred_profile"] = match.group(2) if match else None
    return cfg


def git_provenance(paths):
    for base in paths:
        if not base:
            continue
        probe = base if os.path.isdir(base) else os.path.dirname(base)
        head = run_capture(["git", "-C", probe, "rev-parse", "HEAD"])
        if head["rc"] == 0:
            status = run_capture(["git", "-C", probe, "status", "--porcelain"])
            return {
                "toplevel": run_capture(["git", "-C", probe, "rev-parse", "--show-toplevel"])[
                    "stdout"
                ],
                "head": head["stdout"],
                "dirty": bool(status["stdout"]),
                "status_porcelain": status["stdout"].splitlines(),
            }
    return None


class Sampler(threading.Thread):
    def __init__(self, cpus, zones):
        super().__init__(daemon=True)
        self.cpus = cpus
        self.zone_names = [z["zone"] for z in zones]
        self.zone_types = {z["zone"]: z["type"] for z in zones}
        self.stop_event = threading.Event()
        self.samples = []

    def run(self):
        while not self.stop_event.is_set():
            item = {"freq": {}, "temp": {}}
            for cpu in self.cpus:
                item["freq"][str(cpu)] = read_int(
                    f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq"
                )
            for zone in self.zone_names:
                item["temp"][zone] = read_int(f"/sys/class/thermal/{zone}/temp")
            self.samples.append(item)
            self.stop_event.wait(SAMPLE_INTERVAL_S)

    def stop(self):
        self.stop_event.set()

    def summary(self):
        out = {"samples": len(self.samples), "freq": {}, "thermal": {}}
        for cpu in self.cpus:
            vals = [
                s["freq"].get(str(cpu))
                for s in self.samples
                if s["freq"].get(str(cpu)) is not None
            ]
            out["freq"][str(cpu)] = (
                {
                    "min": min(vals),
                    "max": max(vals),
                    "last": vals[-1],
                    "n": len(vals),
                    "series": vals,
                }
                if vals
                else None
            )
        for zone in self.zone_names:
            vals = [
                s["temp"].get(zone)
                for s in self.samples
                if s["temp"].get(zone) is not None
            ]
            out["thermal"][zone] = {
                "type": self.zone_types.get(zone),
                "min": min(vals) if vals else None,
                "max": max(vals) if vals else None,
                "last": vals[-1] if vals else None,
                "n": len(vals),
            }
        return out


def perf_row(perf, event):
    row = perf["rows"].get(event)
    if row is None:
        for name, candidate in perf["rows"].items():
            if name.split(":")[0] == event.split(":")[0]:
                return candidate
    return row


def parse_perf_file(path, events):
    result = {
        "path": path,
        "exists": os.path.exists(path),
        "events_requested": list(events),
        "rows": {},
        "markers": [],
        "ok": False,
        "reasons": [],
    }
    text = read_text(path)
    if text is None:
        result["reasons"].append("perf_output_missing")
        return result
    result["raw"] = text
    by_event = {}
    by_base = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        lowered = stripped.lower()
        for marker in PERF_BAD_MARKERS:
            if marker in lowered:
                result["markers"].append(stripped)
                break
        if PERF_SCALE_RE.search(stripped):
            result["markers"].append(stripped)
        fields = stripped.split(",")
        if len(fields) < 5:
            continue
        value, unit, event, run_time, percent = fields[:5]
        row = {
            "value": value,
            "unit": unit,
            "event": event,
            "run_time": run_time,
            "percent_enabled": percent,
        }
        by_event[event] = row
        by_base[event.split(":")[0]] = row
    result["rows"] = by_event
    for event in events:
        row = by_event.get(event) or by_base.get(event.split(":")[0])
        if row is None:
            result["reasons"].append(f"perf_event_missing:{event}")
            continue
        value = row["value"].strip()
        if value in ("", "<not counted>", "<not supported>"):
            result["reasons"].append(f"perf_value_invalid:{event}:{value or 'empty'}")
            continue
        try:
            float(value)
        except ValueError:
            result["reasons"].append(f"perf_value_not_numeric:{event}:{value}")
            continue
        percent = row["percent_enabled"].strip()
        try:
            pct = float(percent)
        except ValueError:
            pct = None
        if pct is not None and pct < 99.999:
            result["reasons"].append(f"perf_scaled:{event}:{pct:.2f}%")
    if result["markers"]:
        result["reasons"].append("perf_marker_present")
    result["ok"] = not result["reasons"]
    return result


def parse_bench_jsonl(path):
    info = {"path": path, "exists": os.path.isfile(path), "records": [], "errors": []}
    if not info["exists"]:
        return info
    text = read_text(path)
    if text is None:
        info["errors"].append("jsonl_unreadable")
        return info
    for lineno, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            info["records"].append(json.loads(line))
        except json.JSONDecodeError as exc:
            info["errors"].append(f"line {lineno}: {exc}")
    return info


def oracle_sluice_copy(proc_rc, stdout, dst, src_sha, src_size, frozen_ops):
    details = []
    ok = True
    parsed = None
    match = COUNTER_RE.search(stdout or "")
    if match:
        parsed = {
            "bytes_copied": int(match.group(1)),
            "read_ops": int(match.group(2)),
            "write_ops": int(match.group(3)),
            "short_writes": int(match.group(4)),
            "sync": match.group(5),
            "pipeline_depth": int(match.group(6)),
            "atomic": match.group(7),
        }
    if proc_rc != 0:
        ok = False
        details.append(f"exit={proc_rc}")
    if parsed is None:
        ok = False
        details.append("stats_line_missing")
    else:
        if parsed["bytes_copied"] != src_size:
            ok = False
            details.append(f"bytes_copied={parsed['bytes_copied']}!=src_size={src_size}")
        if parsed["short_writes"] != 0:
            ok = False
            details.append(f"short_writes={parsed['short_writes']}")
    dst_sha = sha256_file(dst) if os.path.isfile(dst) else ""
    dst_matches = dst_sha == src_sha
    if not dst_matches:
        ok = False
        details.append("dst_sha_mismatch")
    frozen_update = None
    if ok and parsed is not None:
        ops = (parsed["read_ops"], parsed["write_ops"])
        if frozen_ops is None:
            frozen_update = ops
        elif ops != frozen_ops:
            ok = False
            details.append(
                f"ops_mismatch read_ops={ops[0]}/{frozen_ops[0]} "
                f"write_ops={ops[1]}/{frozen_ops[1]}"
            )
    oracle = {
        "ok": ok,
        "detail": details,
        "sluice_copy": {
            "parsed": parsed,
            "src_sha256": src_sha,
            "src_size": src_size,
            "dst": dst,
            "dst_sha256": dst_sha,
            "dst_matches_src": dst_matches,
            "frozen_read_ops": frozen_ops[0] if frozen_ops else None,
            "frozen_write_ops": frozen_ops[1] if frozen_ops else None,
        },
        "bench": None,
    }
    return oracle, frozen_update


def oracle_bench(proc_rc, jsonl_path, case, warmup, measured):
    details = []
    ok = True
    info = parse_bench_jsonl(jsonl_path)
    if proc_rc != 0:
        ok = False
        details.append(f"exit={proc_rc}")
    if not info["exists"]:
        ok = False
        details.append("jsonl_missing")
    if info["errors"]:
        ok = False
        details.extend(info["errors"])
    records = info["records"]
    target = [r for r in records if r.get("case_id") == case]
    cal = [r for r in records if r.get("case_id") == "CAL-oracle-self-test"]
    if not target:
        ok = False
        details.append("target_case_missing")
    if not cal:
        ok = False
        details.append("cal_self_test_missing")
    elif not all(r.get("ok") is True for r in cal):
        ok = False
        details.append("cal_self_test_failed")
    warm = [r for r in target if r.get("measured") is False]
    meas = [r for r in target if r.get("measured") is True]
    if len(warm) != warmup:
        ok = False
        details.append(f"warmup_records={len(warm)}!={warmup}")
    if len(meas) != measured:
        ok = False
        details.append(f"measured_records={len(meas)}!={measured}")
    failed = [r for r in target if r.get("ok") is not True]
    if failed:
        ok = False
        details.append(f"case_records_failed={len(failed)}")
    summary = None
    if meas:
        summary = {
            "records": len(target),
            "warmup_records": len(warm),
            "measured_records": len(meas),
            "measured_wall_s_mean": mean([r.get("wall_s") for r in meas if r.get("wall_s") is not None]),
            "measured_ops_per_sec_mean": mean(
                [r.get("ops_per_sec") for r in meas if r.get("ops_per_sec") is not None]
            ),
            "measured_cpu_s_mean": mean(
                [r.get("cpu_s") for r in meas if r.get("cpu_s") is not None]
            ),
            "measured_ops_mean": mean(
                [r.get("ops") for r in meas if r.get("ops") is not None]
            ),
            "measured": [
                {
                    "rep": r.get("rep"),
                    "wall_s": r.get("wall_s"),
                    "ops_per_sec": r.get("ops_per_sec"),
                    "cpu_s": r.get("cpu_s"),
                    "ops": r.get("ops"),
                }
                for r in meas
            ],
        }
    oracle = {
        "ok": ok,
        "detail": details,
        "sluice_copy": None,
        "bench": {
            "jsonl": jsonl_path,
            "records_total": len(records),
            "target_records": len(target),
            "cal_records": len(cal),
            "cal_ok": bool(cal) and all(r.get("ok") is True for r in cal),
            "warmup_records": len(warm),
            "measured_records": len(meas),
            "summary": summary,
        },
    }
    return oracle


def arm_argv(args, arm, binary, dst, jsonl_path):
    if args.kind == "sluice_copy":
        argv = [
            binary,
            "--buffer-size",
            str(args.buffer_size),
            "--pipeline-depth",
            str(args.pipeline_depth),
            "--workers",
            str(args.workers),
            "--sync",
            "none",
            "--no-atomic",
            args.src,
            dst,
        ]
        env = None
    else:
        argv = [binary]
        env = os.environ.copy()
        env["SLUICE_BENCH_WARMUP"] = str(args.warmup)
        env["SLUICE_BENCH_MEASURED"] = str(args.measured)
        env["SLUICE_BENCH_CASE_FILTER"] = "=" + args.case
        env["SLUICE_BENCH_JSONL"] = jsonl_path
        env["SLUICE_BENCH_TMP_DIR"] = args.tmp_dir
    return argv, env


def tsv_cell(value):
    if value is None:
        return ""
    text = str(value)
    return text.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def write_json_line(handle, record):
    handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    handle.flush()
    os.fsync(handle.fileno())


def write_tsv_row(handle, row):
    handle.write("\t".join(tsv_cell(row.get(c)) for c in TSV_COLUMNS) + "\n")
    handle.flush()
    os.fsync(handle.fileno())


def choose_runner_cpu(measured):
    try:
        present = sorted(
            int(name[3:])
            for name in os.listdir("/sys/devices/system/cpu")
            if name.startswith("cpu") and name[3:].isdigit()
        )
    except OSError:
        return None
    for cpu in present:
        if cpu not in measured:
            return cpu
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Paired A/A (future A/B) session runner for #621",
        allow_abbrev=False,
    )
    parser.add_argument("--cell", required=True)
    parser.add_argument("--kind", required=True, choices=("sluice_copy", "bench_w02", "bench_w03"))
    parser.add_argument("--pairs", type=int, required=True)
    parser.add_argument("--cpu", required=True)
    parser.add_argument("--perf-events", default="task-clock,instructions,cycles")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument(
        "--binary-b",
        default=None,
        help="optional B-arm binary for future A/B sessions; defaults to --binary",
    )
    parser.add_argument("--src")
    parser.add_argument("--dst-a")
    parser.add_argument("--dst-b")
    parser.add_argument("--buffer-size", type=int)
    parser.add_argument("--pipeline-depth", type=int)
    parser.add_argument("--workers", type=int)
    parser.add_argument("--case")
    parser.add_argument("--warmup", type=int)
    parser.add_argument("--measured", type=int)
    parser.add_argument("--tmp-dir")
    args = parser.parse_args()

    if args.pairs < 1:
        parser.error("--pairs must be >= 1")
    try:
        measured_cpus = parse_cpu_list(args.cpu)
    except ValueError as exc:
        parser.error(f"--cpu: {exc}")
    if re.search(r"[^0-9,\-]", args.cpu):
        parser.error("--cpu must contain only digits, commas and ranges")

    events = [e.strip() for e in args.perf_events.split(",") if e.strip()]
    if not events:
        parser.error("--perf-events must name at least one event")
    for event in events:
        if not re.fullmatch(r"[A-Za-z0-9_:./-]+", event):
            parser.error(f"unsupported perf event syntax: {event!r}")

    if args.kind == "sluice_copy":
        for name in ("src", "dst_a", "dst_b", "buffer_size", "pipeline_depth", "workers"):
            if getattr(args, name) is None:
                parser.error(f"--kind sluice_copy requires --{name.replace('_', '-')}")
        if args.buffer_size <= 0 or args.pipeline_depth <= 0 or args.workers <= 0:
            parser.error("--buffer-size, --pipeline-depth and --workers must be positive")
        for name in ("case", "warmup", "measured", "tmp_dir"):
            if getattr(args, name) is not None:
                parser.error(f"--{name.replace('_', '-')} is not valid with --kind sluice_copy")
    else:
        for name in ("case", "warmup", "measured", "tmp_dir"):
            if getattr(args, name) is None:
                parser.error(f"--kind {args.kind} requires --{name.replace('_', '-')}")
        if args.warmup < 0:
            parser.error("--warmup must be >= 0")
        if args.measured < 1:
            parser.error("--measured must be >= 1")
        if not args.case.strip() or re.search(r"\s", args.case):
            parser.error("--case must be a non-empty case id without whitespace")
        if args.case.startswith("="):
            args.case = args.case[1:]
        for name in ("src", "dst_a", "dst_b", "buffer_size", "pipeline_depth", "workers"):
            if getattr(args, name) is not None:
                parser.error(f"--{name.replace('_', '-')} is not valid with --kind {args.kind}")

    for label, path in (("--binary", args.binary), ("--binary-b", args.binary_b)):
        if path is None:
            continue
        if not os.path.isfile(path):
            parser.error(f"{label}: not a file: {path}")
        if not os.access(path, os.X_OK):
            parser.error(f"{label}: not executable: {path}")

    if args.kind == "sluice_copy":
        if not os.path.isfile(args.src):
            parser.error(f"--src: not a file: {args.src}")
        if os.path.realpath(args.dst_a) == os.path.realpath(args.dst_b):
            parser.error("--dst-a and --dst-b must differ")
        for label, dst in (("--dst-a", args.dst_a), ("--dst-b", args.dst_b)):
            if os.path.realpath(dst) == os.path.realpath(args.src):
                parser.error(f"{label} must not be the source")
            if os.path.exists(dst) and os.path.exists(args.src) and os.path.samefile(dst, args.src):
                parser.error(f"{label} must not be the source")
            try:
                os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
            except OSError as exc:
                parser.error(f"{label}: cannot create parent directory: {exc}")
    else:
        try:
            os.makedirs(args.tmp_dir, exist_ok=True)
        except OSError as exc:
            parser.error(f"--tmp-dir: cannot create directory: {exc}")
        if not os.path.isdir(args.tmp_dir):
            parser.error(f"--tmp-dir is not a directory: {args.tmp_dir}")

    out_dir = os.path.abspath(args.out_dir)
    if os.path.exists(out_dir) and not os.path.isdir(out_dir):
        parser.error(f"--out-dir is not a directory: {out_dir}")
    if os.path.isdir(out_dir):
        if os.listdir(out_dir):
            parser.error(f"--out-dir must be new or empty: {out_dir}")
    else:
        os.makedirs(out_dir)
    raw_dir = os.path.join(out_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)

    pairs_path = os.path.join(out_dir, "pairs.tsv")
    arms_path = os.path.join(out_dir, "arms.jsonl")
    meta_path = os.path.join(out_dir, "meta.json")

    binary_a = os.path.abspath(args.binary)
    binary_b = os.path.abspath(args.binary_b or args.binary)
    src_sha = sha256_file(args.src) if args.kind == "sluice_copy" else None
    src_size = os.path.getsize(args.src) if args.kind == "sluice_copy" else None

    runner_cpu = choose_runner_cpu(measured_cpus)
    runner_affinity = None
    if runner_cpu is not None:
        try:
            os.sched_setaffinity(0, {runner_cpu})
            runner_affinity = runner_cpu
        except OSError:
            runner_affinity = None

    media = {}
    if args.kind == "sluice_copy":
        media = {
            "src": mount_info(args.src),
            "dst_a": mount_info(args.dst_a),
            "dst_b": mount_info(args.dst_b),
        }
    else:
        media = {"tmp_dir": mount_info(args.tmp_dir)}

    zones = thermal_snapshot()
    meta = {
        "tool": "paired_session.py",
        "tool_version": TOOL_VERSION,
        "tool_sha256": sha256_file(os.path.abspath(__file__)),
        "cell": args.cell,
        "kind": args.kind,
        "pairs": args.pairs,
        "cpu": args.cpu,
        "measured_cpus": measured_cpus,
        "perf_events": events,
        "started": now_iso(),
        "finished": None,
        "runner_affinity_cpu": runner_affinity,
        "runner_affinity_requested_cpu": runner_cpu,
        "binaries": {
            "A": build_config(binary_a),
            "B": build_config(binary_b),
        },
        "git": git_provenance([os.getcwd(), os.path.dirname(binary_a)]),
        "protocol": {
            "order_rule": "odd pair ordinal -> AB; even pair ordinal -> BA",
            "serial": True,
            "perf_wrap": (
                "perf stat -x, -e <events> -o <raw perf> -- "
                "taskset -c <cpu> <binary> <binary argv>"
            ),
            "cpu_metric": "RUSAGE_CHILDREN user+sys delta, microseconds",
            "wall_metric": "time.monotonic around the perf/taskset child",
            "sluice_copy_argv": (
                ["--buffer-size", "<N>", "--pipeline-depth", "<N>", "--workers", "<N>",
                 "--sync", "none", "--no-atomic", "<src>", "<dst>"]
                if args.kind == "sluice_copy"
                else None
            ),
            "bench_env": (
                {
                    "SLUICE_BENCH_WARMUP": str(args.warmup),
                    "SLUICE_BENCH_MEASURED": str(args.measured),
                    "SLUICE_BENCH_CASE_FILTER": "=" + args.case,
                    "SLUICE_BENCH_JSONL": "<per-arm jsonl>",
                    "SLUICE_BENCH_TMP_DIR": args.tmp_dir,
                }
                if args.kind != "sluice_copy"
                else None
            ),
        },
        "case": (
            {
                "src": os.path.abspath(args.src),
                "src_size": src_size,
                "src_sha256": src_sha,
                "dst_a": os.path.abspath(args.dst_a),
                "dst_b": os.path.abspath(args.dst_b),
                "buffer_size": args.buffer_size,
                "pipeline_depth": args.pipeline_depth,
                "workers": args.workers,
            }
            if args.kind == "sluice_copy"
            else {
                "case": args.case,
                "warmup": args.warmup,
                "measured": args.measured,
                "tmp_dir": os.path.abspath(args.tmp_dir),
            }
        ),
        "media": media,
        "environment": {
            "kernel": os.uname().release,
            "machine": os.uname().machine,
            "hostname": socket.gethostname(),
            "python": sys.version,
            "governor_before": governor_snapshot(),
            "no_turbo_before": no_turbo(),
            "thermal_before": zones,
            "irq_affinity_before": irq_affinity_snapshot(),
            "proc_stat_before": read_proc_stat(),
            "meminfo_before": meminfo_summary(),
            "loadavg_before": os.getloadavg(),
            "governor_after": None,
            "no_turbo_after": None,
            "thermal_after": None,
            "irq_affinity_after": None,
            "proc_stat_after": None,
            "meminfo_after": None,
            "loadavg_after": None,
            "governor_all_performance": None,
        },
        "background_load_per_pair": [],
        "frozen_ops": None,
        "excluded_arms": [],
        "arms_total": 0,
        "arms_valid": 0,
        "arms_invalid": 0,
        "aborted": None,
    }

    gov_before = meta["environment"]["governor_before"]
    meta["environment"]["governor_all_performance_before"] = bool(gov_before) and all(
        item.get("governor") == "performance" for item in gov_before.values()
    )

    frozen_ops = None
    frozen_source = None
    tsv = open(pairs_path, "w", encoding="utf-8")
    arms = open(arms_path, "w", encoding="utf-8")
    tsv.write("\t".join(TSV_COLUMNS) + "\n")
    tsv.flush()
    os.fsync(tsv.fileno())

    try:
        for pair in range(1, args.pairs + 1):
            order = "AB" if pair % 2 == 1 else "BA"
            pair_background_load = background_load_snapshot()
            pair_background_load["pair"] = pair
            pair_background_load["order"] = order
            meta["background_load_per_pair"].append(pair_background_load)
            sequence = [("A", 1), ("B", 2)] if order == "AB" else [("B", 1), ("A", 2)]
            for arm, position in sequence:
                binary = binary_a if arm == "A" else binary_b
                if args.kind == "sluice_copy":
                    dst = args.dst_a if arm == "A" else args.dst_b
                    jsonl_path = None
                else:
                    dst = None
                    jsonl_path = os.path.join(raw_dir, f"pair-{pair:02d}-{arm}.jsonl")
                raw_prefix = os.path.join(raw_dir, f"pair-{pair:02d}-{arm}")
                perf_path = raw_prefix + ".perf"
                stdout_path = raw_prefix + ".stdout"
                stderr_path = raw_prefix + ".stderr"

                binary_argv, env = arm_argv(args, arm, binary, dst, jsonl_path)
                wrapped = [TASKSET, "-c", args.cpu] + binary_argv
                perf_argv = [
                    PERF,
                    "stat",
                    "-x,",
                    "-e",
                    args.perf_events,
                    "-o",
                    perf_path,
                    "--",
                ] + wrapped

                sampler = Sampler(measured_cpus, zones)
                sampler.start()
                proc_stat_before = read_proc_stat()
                ru_before = resource.getrusage(resource.RUSAGE_CHILDREN)
                start = now_iso()
                t0 = time.monotonic()
                timed_out = False
                try:
                    proc = subprocess.Popen(
                        perf_argv,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        env=env,
                        start_new_session=True,
                    )
                except OSError as exc:
                    sampler.stop()
                    sampler.join(timeout=5.0)
                    raise RuntimeError(f"cannot start arm command: {exc}") from exc
                try:
                    stdout, stderr = proc.communicate(timeout=ARM_TIMEOUT_S)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    stdout, stderr = proc.communicate()
                    timed_out = True
                except BaseException:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.communicate()
                    raise
                wall_s = time.monotonic() - t0
                end = now_iso()
                ru_after = resource.getrusage(resource.RUSAGE_CHILDREN)
                sampler.stop()
                sampler.join(timeout=5.0)
                proc_stat_after = read_proc_stat()

                with open(stdout_path, "w", encoding="utf-8") as f:
                    f.write(stdout)
                with open(stderr_path, "w", encoding="utf-8") as f:
                    f.write(stderr)
                with open(raw_prefix + ".cmd", "w", encoding="utf-8") as f:
                    json.dump({"argv": perf_argv, "binary_argv": binary_argv}, f, indent=2)
                    f.write("\n")

                cpu_user_s = ru_after.ru_utime - ru_before.ru_utime
                cpu_sys_s = ru_after.ru_stime - ru_before.ru_stime
                cpu_total_s = cpu_user_s + cpu_sys_s
                cpu_us = int(round(cpu_total_s * 1_000_000))

                perf = parse_perf_file(perf_path, events)
                if timed_out:
                    perf["ok"] = False
                    perf["reasons"].append(f"arm_timeout>{ARM_TIMEOUT_S:.0f}s")
                if not perf["exists"] and proc.returncode != 0:
                    perf["reasons"].append(f"perf_stat_exit={proc.returncode}")

                if args.kind == "sluice_copy":
                    oracle, frozen_update = oracle_sluice_copy(
                        proc.returncode, stdout, dst, src_sha, src_size, frozen_ops
                    )
                else:
                    oracle = oracle_bench(
                        proc.returncode, jsonl_path, args.case, args.warmup, args.measured
                    )
                    frozen_update = None

                invalid_reasons = []
                if timed_out:
                    invalid_reasons.append("arm_timeout")
                if proc.returncode != 0:
                    invalid_reasons.append(f"exit={proc.returncode}")
                invalid_reasons.extend(perf["reasons"])
                invalid_reasons.extend(oracle["detail"])
                invalid_reasons = list(dict.fromkeys(invalid_reasons))
                valid = not invalid_reasons

                if (
                    valid
                    and args.kind == "sluice_copy"
                    and frozen_update is not None
                    and frozen_ops is None
                ):
                    frozen_ops = frozen_update
                    frozen_source = {"pair": pair, "arm": arm}
                    oracle["sluice_copy"]["frozen_read_ops"] = frozen_ops[0]
                    oracle["sluice_copy"]["frozen_write_ops"] = frozen_ops[1]

                sampler_summary = sampler.summary()
                record = {
                    "pair": pair,
                    "order": order,
                    "position": position,
                    "arm": arm,
                    "kind": args.kind,
                    "cell": args.cell,
                    "binary": {
                        "role": arm,
                        "path": binary,
                        "sha256": meta["binaries"][arm]["sha256"],
                    },
                    "argv": binary_argv,
                    "perf_argv": perf_argv,
                    "env": (
                        {
                            k: env[k]
                            for k in (
                                "SLUICE_BENCH_WARMUP",
                                "SLUICE_BENCH_MEASURED",
                                "SLUICE_BENCH_CASE_FILTER",
                                "SLUICE_BENCH_JSONL",
                                "SLUICE_BENCH_TMP_DIR",
                                "SLUICE_BENCH_STATS",
                            )
                            if env is not None and k in env
                        }
                        if env is not None
                        else None
                    ),
                    "start": start,
                    "end": end,
                    "wall_s": wall_s,
                    "cpu_user_s": cpu_user_s,
                    "cpu_sys_s": cpu_sys_s,
                    "cpu_total_s": cpu_total_s,
                    "cpu_us": cpu_us,
                    "rc": proc.returncode,
                    "timed_out": timed_out,
                    "perf": perf,
                    "oracle": oracle,
                    "valid": valid,
                    "invalid_reasons": invalid_reasons,
                    "sampler": sampler_summary,
                    "pair_background_load": pair_background_load,
                    "proc_stat": {
                        "before": proc_stat_before,
                        "after": proc_stat_after,
                        "delta": proc_stat_delta(proc_stat_before, proc_stat_after, measured_cpus),
                    },
                    "raw": {
                        "stdout": stdout_path,
                        "stderr": stderr_path,
                        "perf": perf_path,
                        "jsonl": jsonl_path,
                    },
                    "stdout_tail": stdout[-4000:],
                    "stderr_tail": stderr[-4000:],
                }
                write_json_line(arms, record)

                sluice = oracle["sluice_copy"] or {}
                bench = oracle["bench"] or {}
                bench_summary = bench.get("summary") or {}
                freq_summary = sampler_summary.get("freq") or {}
                freq_values = [
                    v
                    for v in (
                        item.get("min") if isinstance(item, dict) else None
                        for item in freq_summary.values()
                    )
                    if v is not None
                ]
                freq_max_values = [
                    v
                    for v in (
                        item.get("max") if isinstance(item, dict) else None
                        for item in freq_summary.values()
                    )
                    if v is not None
                ]
                temp_values = [
                    item.get("max")
                    for item in (sampler_summary.get("thermal") or {}).values()
                    if isinstance(item, dict) and item.get("max") is not None
                ]
                row = {
                    "pair": pair,
                    "order": order,
                    "position": position,
                    "arm": arm,
                    "kind": args.kind,
                    "cell": args.cell,
                    "valid": valid,
                    "invalid_reasons": ";".join(invalid_reasons),
                    "rc": proc.returncode,
                    "timed_out": timed_out,
                    "wall_s": f"{wall_s:.9f}",
                    "cpu_user_s": f"{cpu_user_s:.6f}",
                    "cpu_sys_s": f"{cpu_sys_s:.6f}",
                    "cpu_total_s": f"{cpu_total_s:.9f}",
                    "cpu_us": cpu_us,
                    "perf_ok": perf["ok"],
                    "perf_task_clock": (perf_row(perf, "task-clock") or {}).get("value", ""),
                    "perf_task_clock_unit": (perf_row(perf, "task-clock") or {}).get(
                        "unit", ""
                    ),
                    "perf_instructions": (perf_row(perf, "instructions") or {}).get(
                        "value", ""
                    ),
                    "perf_cycles": (perf_row(perf, "cycles") or {}).get("value", ""),
                    "oracle_ok": oracle["ok"],
                    "oracle_detail": ";".join(oracle["detail"]) or "ok",
                    "read_ops": (sluice.get("parsed") or {}).get("read_ops", ""),
                    "write_ops": (sluice.get("parsed") or {}).get("write_ops", ""),
                    "short_writes": (sluice.get("parsed") or {}).get("short_writes", ""),
                    "bytes_copied": (sluice.get("parsed") or {}).get("bytes_copied", ""),
                    "dst_sha256": sluice.get("dst_sha256", ""),
                    "case_measured_n": bench_summary.get("measured_records", ""),
                    "case_measured_wall_s_mean": bench_summary.get(
                        "measured_wall_s_mean", ""
                    ),
                    "case_measured_ops_per_sec_mean": bench_summary.get(
                        "measured_ops_per_sec_mean", ""
                    ),
                    "case_measured_cpu_s_mean": bench_summary.get("measured_cpu_s_mean", ""),
                    "freq_min_khz": min(freq_values) if freq_values else "",
                    "freq_max_khz": max(freq_max_values) if freq_max_values else "",
                    "temp_max_mc": max(temp_values) if temp_values else "",
                    "start": start,
                    "end": end,
                    "raw_stdout": stdout_path,
                    "raw_stderr": stderr_path,
                    "raw_perf": perf_path,
                    "raw_jsonl": jsonl_path or "",
                }
                write_tsv_row(tsv, row)

                meta["arms_total"] += 1
                if valid:
                    meta["arms_valid"] += 1
                else:
                    meta["arms_invalid"] += 1
                    meta["excluded_arms"].append(
                        {"pair": pair, "arm": arm, "reasons": invalid_reasons}
                    )
                print(
                    f"pair {pair:02d}/{args.pairs} {order} arm {arm} "
                    f"wall={wall_s:.4f}s cpu={cpu_total_s:.4f}s rc={proc.returncode} "
                    f"perf={'ok' if perf['ok'] else 'INVALID'} "
                    f"oracle={'ok' if oracle['ok'] else 'INVALID'} "
                    f"valid={valid}",
                    flush=True,
                )
        if frozen_ops is not None:
            meta["frozen_ops"] = {
                "read_ops": frozen_ops[0],
                "write_ops": frozen_ops[1],
                "source": frozen_source,
            }
    except BaseException as exc:
        meta["aborted"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        meta["finished"] = now_iso()
        meta["environment"]["governor_after"] = governor_snapshot()
        meta["environment"]["no_turbo_after"] = no_turbo()
        meta["environment"]["thermal_after"] = thermal_snapshot()
        meta["environment"]["irq_affinity_after"] = irq_affinity_snapshot()
        meta["environment"]["proc_stat_after"] = read_proc_stat()
        meta["environment"]["meminfo_after"] = meminfo_summary()
        meta["environment"]["loadavg_after"] = os.getloadavg()
        if args.kind == "sluice_copy":
            meta["media_after"] = {
                "src": mount_info(args.src),
                "dst_a": mount_info(args.dst_a),
                "dst_b": mount_info(args.dst_b),
            }
        else:
            meta["media_after"] = {"tmp_dir": mount_info(args.tmp_dir)}
        gov = meta["environment"]["governor_after"]
        meta["environment"]["governor_all_performance"] = bool(gov) and all(
            item.get("governor") == "performance" for item in gov.values()
        )
        meta["environment_warnings"] = []
        if not meta["environment"]["governor_all_performance"]:
            meta["environment_warnings"].append("governor_not_performance_on_all_cpus")
        if not meta["environment"]["governor_all_performance_before"]:
            meta["environment_warnings"].append("governor_not_performance_before_session")
        if meta["runner_affinity_cpu"] is None:
            meta["environment_warnings"].append("runner_affinity_not_set")
        pair_loads = [
            entry["loadavg"][0]
            for entry in meta["background_load_per_pair"]
            if entry.get("loadavg")
        ]
        if pair_loads and max(pair_loads) > len(measured_cpus):
            meta["environment_warnings"].append("background_load_exceeds_measured_cpus")
        tsv.close()
        arms.close()
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2, ensure_ascii=False)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        print(
            f"session {args.cell}: arms={meta['arms_total']} "
            f"valid={meta['arms_valid']} invalid={meta['arms_invalid']} "
            f"out={out_dir}",
            flush=True,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
