#!/usr/bin/env python3
"""run_host.py — CHUNK-E0 portable one-command host runner (#270).

Orchestration authority for running the FROZEN CHUNK-E0 chunk-size x depth
campaign on a rented/remote Linux host (x86_64 or aarch64) with one command:

    ./research/chunk-e0/run-host.sh --campaign H1 --host-id <id> --profile full

The runner NEVER re-implements the experiment. It delegates every measured
run to the existing CHUNK-E0 measurement driver (scripts/chunk_e0.py) and
the production engine bench (bench/chunk_e0_bench.cpp), reusing the frozen
same-work contract, seeded ordering, repetitions, median/MAD aggregation,
fail-closed gates and immutable-session layout. The scientific authority is
research/chunk-e0/CHUNK-E0-H0-PREREGISTRATION.md; the machine-readable
execution representation is research/chunk-e0/campaign.json, which the
runner validates against the driver's frozen constants at preflight and
fails closed on any divergence.

Profiles:
  full     environment -> build -> smoke -> OFFICIAL sweep -> validation
           -> aggregation -> plots -> host summary -> evidence archive.
           Only VALID + profile=full is CHUNK-E0 formal host evidence.
  verify   environment -> build -> small representative matrix
           (6 chunks x {d1,d4} x 2 reps) -> validation. PROFILE=VERIFY,
           NOT FORMAL EVIDENCE, no scientific promotion.
  smoke    build + a couple of tiny cells + hash/perf/fs correctness.
           SMOKE_ONLY.

CLI:
  --host-id <id>            explicit host identity (else auto-generated)
  --campaign <H1|H2|H0>     campaign label (does not change the frozen
                            matrix)
  --profile <full|verify|smoke>
  --preflight-only          fail-fast checks only; no measurement
  --resume <session>        continue an interrupted full session (identity
                            checked; RESUME REFUSED on mismatch)
  --print-install-command   print suggested package-install commands only

No automatic package installation, no sudo, no sysctl/governor/kernel
changes, no cloud-metadata or credential collection.

Status model: PREFLIGHT_FAILED / SMOKE_ONLY / VERIFY_ONLY / INCOMPLETE /
INVALID / VALID.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CAMPAIGN_DIR = REPO / "research" / "chunk-e0"
SCRIPTS_DIR = CAMPAIGN_DIR / "scripts"
RESULTS_DIR = CAMPAIGN_DIR / "results"
PLOTS_DIR = CAMPAIGN_DIR / "plots"
ARCHIVES_DIR = CAMPAIGN_DIR / "archives"
HOST_WORK = REPO / "build" / "chunk-e0-host"

RUNNER_SCHEMA = "chunk-e0-runner-1.0"
BENCH_TARGET = "chunk_e0_bench"
SAFETY_FACTOR = 1.5
CAMPAIGN_JSON = CAMPAIGN_DIR / "campaign.json"
PREREG = "research/chunk-e0/CHUNK-E0-H0-PREREGISTRATION.md (FROZEN)"

# Import the existing CHUNK-E0 measurement driver as the execution engine.
sys.path.insert(0, str(SCRIPTS_DIR))
import chunk_e0 as driver  # noqa: E402


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------

def run_cmd(args, **kw):
    try:
        p = subprocess.run(args, capture_output=True, text=True, **kw)
        return p.returncode, p.stdout.strip(), p.stderr.strip()
    except FileNotFoundError:
        return 127, "", f"{args[0]}: not found"
    except Exception as e:  # noqa: BLE001 — best-effort probe
        return 127, "", str(e)


def run_ok(args, **kw):
    rc, out, _ = run_cmd(args, **kw)
    return rc == 0, out


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def write_json(path: Path, data) -> None:
    path.write_text(json.dumps(data, indent=1) + "\n")


def read_json(path: Path) -> dict:
    return json.loads(path.read_text())


def now_utc_compact() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")


def log(msg: str) -> None:
    print(f"[runner] {msg}", flush=True)


def _try_mkdir(path: Path) -> bool:
    try:
        path.mkdir(parents=True, exist_ok=True)
        return True
    except Exception:  # noqa: BLE001
        return False


# --------------------------------------------------------------------------
# Environment capture (cross-platform, guarded; UNKNOWN when not detectable)
# --------------------------------------------------------------------------

def _first(*paths) -> str:
    for p in paths:
        try:
            v = Path(p).read_text().strip()
            if v:
                return v
        except Exception:  # noqa: BLE001
            continue
    return "UNKNOWN"


def _cpu_vendor(lscpu: str) -> str:
    for line in lscpu.splitlines():
        if line.startswith("Vendor ID"):
            v = line.split(":", 1)[1].strip().lower()
            if "intel" in v:
                return "intel"
            if "amd" in v:
                return "amd"
            return v
    return "UNKNOWN"


def _microarchitecture(lscpu: str, flags: str, model: str) -> dict:
    """Best-effort microarchitecture label. x86: conservative flags-based
    class; arm: known model names when present. NEVER guessed past the
    evidence; UNKNOWN otherwise."""
    arch = platform.machine()
    if arch in ("x86_64", "amd64"):
        if "avx512f" in flags and "avx512bw" in flags:
            cls = "x86-64 AVX-512 (Skylake-X/ICX/SPR-class or newer)"
        elif "avx2" in flags:
            cls = "x86-64 AVX2" + ("+ERMS" if "erms" in flags else "") + \
                " (Haswell-class or newer)"
        elif "avx" in flags:
            cls = "x86-64 AVX (Sandy/Ivy-Bridge class)"
        else:
            cls = "x86-64 pre-AVX"
        return {"label": cls, "method": "BEST_EFFORT (cpu flags)",
                "flags_evidence": flags[:400]}
    if arch in ("aarch64", "arm64"):
        low = model.lower()
        known = ["neoverse-v2", "neoverse-v1", "neoverse-n2", "neoverse-n1",
                 "neoverse", "cortex-a78", "cortex-a76", "cortex-a72",
                 "cortex-a55", "ampere", "graviton"]
        hit = next((k for k in known if k in low), None)
        if hit:
            return {"label": f"ARM64 {hit}",
                    "method": "BEST_EFFORT (model name)",
                    "flags_evidence": flags[:400]}
        return {"label": "UNKNOWN",
                "method": "model string has no known name",
                "flags_evidence": flags[:400]}
    return {"label": "UNKNOWN", "method": f"unsupported arch {arch}",
            "flags_evidence": flags[:400]}


def _cache_hierarchy() -> list:
    rc, out, _ = run_cmd(["lscpu", "-C"])
    if rc == 0 and out:
        return [l for l in out.splitlines() if l.strip()]
    rows = []
    try:
        base = Path("/sys/devices/system/cpu/cpu0/cache")
        for idx in sorted(base.glob("index*"), key=lambda p: p.name):
            try:
                rows.append({
                    "level": (idx / "level").read_text().strip(),
                    "type": (idx / "type").read_text().strip(),
                    "size": (idx / "size").read_text().strip(),
                    "line": (idx / "coherency_line_size").read_text().strip(),
                })
            except Exception:  # noqa: BLE001
                continue
    except Exception:  # noqa: BLE001
        pass
    return rows if rows else ["UNKNOWN"]


def _storage_class(fstype: str, lsblk: str) -> str:
    if fstype and "tmpfs" in fstype:
        return "tmpfs"
    low = lsblk.lower()
    if "nvme" in low:
        return "NVMe"
    if "sata" in low:
        ssd = "rota=\"0\"" in low or " rota 0 " in low or \
            re.search(r"rota\s+0", low)
        return "SATA" + (" SSD" if ssd else "")
    if any(k in low for k in ("iscsi", "nbd", "rbd", "ceph", "fuse", "9p")):
        return "network-block/virtual"
    if "rota=\"1\"" in low or "rota 1" in low:
        return "HDD"
    return "UNKNOWN (see lsblk line)"


def capture_environment(host_id: str, campaign: str) -> dict:
    """Capture the reproducibility facts for this host/session. Guarded:
    any field that cannot be detected reliably is UNKNOWN / UNAVAILABLE /
    NOT_APPLICABLE — never guessed. No credentials, no cloud metadata, no
    /home contents."""
    rc, lscpu, _ = run_cmd(["lscpu"])
    if rc != 0:
        lscpu = ""
    flags = ""
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.lower().startswith("flags"):
                    flags = line.split(":", 1)[1].strip()
                    break
    except Exception:  # noqa: BLE001
        flags = ""
    model_line = next((l for l in lscpu.splitlines()
                       if l.startswith("Model name")), "UNKNOWN")
    model = model_line.split(":", 1)[1].strip() if ":" in model_line \
        else "UNKNOWN"
    vendor = _cpu_vendor(lscpu)
    microarch = _microarchitecture(lscpu, flags, model)

    os_rel = "UNKNOWN"
    if Path("/etc/os-release").is_file():
        os_rel = Path("/etc/os-release").read_text().strip()
    distro_id = distro_version = "UNKNOWN"
    for line in os_rel.splitlines():
        if line.startswith("ID="):
            distro_id = line.split("=", 1)[1].strip().strip('"')
        if line.startswith("VERSION_ID="):
            distro_version = line.split("=", 1)[1].strip().strip('"')

    git_head, git_branch, git_dirty = "?", "?", "?"
    ok, out = run_ok(["git", "rev-parse", "HEAD"], cwd=REPO)
    if ok:
        git_head = out
    ok, out = run_ok(["git", "branch", "--show-current"], cwd=REPO)
    if ok:
        git_branch = out
    ok, out = run_ok(["git", "status", "--porcelain"], cwd=REPO)
    if ok:
        git_dirty = bool(out.strip())

    rc, findmnt, _ = run_cmd(["findmnt", "-no", "FSTYPE,OPTIONS,SOURCE",
                              "-T", str(HOST_WORK)])
    if rc != 0:
        findmnt = "UNKNOWN"
    fstype = findmnt.split()[0] if findmnt != "UNKNOWN" else "UNKNOWN"
    rc, lsblk, _ = run_cmd(["lsblk", "-d", "-o", "NAME,MODEL,ROTA,SIZE,TRAN"])
    if rc != 0:
        lsblk = ""

    dmi = _first("/sys/class/dmi/id/product_name",
                 "/sys/class/dmi/id/sys_vendor")
    rc, virt, _ = run_cmd(["systemd-detect-virt"])
    virt_out = virt if rc == 0 and virt else "none"
    container = "yes" if (Path("/.dockerenv").exists() or
                          Path("/run/.containerenv").exists()) else "no"

    mem_kb = "UNKNOWN"
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal"):
                mem_kb = line.split(":", 1)[1].strip()
                break
    except Exception:  # noqa: BLE001
        pass

    governor = "UNAVAILABLE"
    for p in ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
              "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"):
        v = _first(p)
        if v != "UNKNOWN":
            governor = v
            break
    no_turbo = "UNAVAILABLE"
    boost = "UNAVAILABLE"
    if Path("/sys/devices/system/cpu/intel_pstate/no_turbo").exists():
        no_turbo = _first("/sys/devices/system/cpu/intel_pstate/no_turbo")
    elif Path("/sys/devices/system/cpu/cpufreq/boost").exists():
        boost = _first("/sys/devices/system/cpu/cpufreq/boost")

    rc_page, page_size, _ = run_cmd(["getconf", "PAGESIZE"])
    rc_line, cache_line, _ = run_cmd(["getconf", "LEVEL1_DCACHE_LINESIZE"])

    env = {
        "schema": RUNNER_SCHEMA,
        "captured_at_utc": now_utc_compact(),
        "campaign": campaign,
        "host_id": host_id,
        "identity": {"hostname": platform.node() or "UNKNOWN",
                     "machine": platform.machine() or "UNKNOWN"},
        "git": {"commit": git_head, "branch": git_branch,
                "dirty": git_dirty},
        "os": {
            "uname": " ".join(os.uname()) or "UNKNOWN",
            "os_release": os_rel,
            "distro_id": distro_id,
            "distro_version": distro_version,
            "kernel": platform.release() or "UNKNOWN",
            "libc_version": _first("/proc/version"),
        },
        "cpu": {
            "vendor": vendor,
            "model": model,
            "model_line": model_line,
            "microarchitecture": microarch,
            "lscpu": lscpu,
            "smt": next((l for l in lscpu.splitlines()
                         if l.startswith("Thread(s) per core")), "UNKNOWN"),
            "cores_per_socket": next((l for l in lscpu.splitlines()
                                      if l.startswith("Core(s) per socket")),
                                     "UNKNOWN"),
            "sockets": next((l for l in lscpu.splitlines()
                             if l.startswith("Socket(s)")), "UNKNOWN"),
            "numa_nodes": next((l for l in lscpu.splitlines()
                                if l.startswith("NUMA node(s)")), "UNKNOWN"),
            "cache_hierarchy": _cache_hierarchy(),
            "cache_line_bytes": cache_line if rc_line == 0 and cache_line
            else _first("/sys/devices/system/cpu/cpu0/cache/index0/"
                        "coherency_line_size"),
        },
        "memory": {"mem_total": mem_kb, "page_size":
                   page_size if rc_page == 0 and page_size else "UNKNOWN"},
        "frequency": {"governor": governor,
                      "intel_pstate_no_turbo": no_turbo,
                      "arm_cpufreq_boost": boost,
                      "scaling_driver": _first(
                          "/sys/devices/system/cpu/cpu0/cpufreq/"
                          "scaling_driver")},
        "filesystem_storage": {
            "work_dir": str(HOST_WORK),
            "findmnt": findmnt,
            "lsblk": lsblk,
            "storage_class": _storage_class(fstype, lsblk),
        },
        "virtualization": {"systemd_detect_virt": virt_out,
                           "container": container, "dmi_product": dmi},
        "perf_event_paranoid": _first(
            "/proc/sys/kernel/perf_event_paranoid"),
        "data_src_sha256": sha256_file(driver.DATA_DIR / "src.bin")
        if (driver.DATA_DIR / "src.bin").is_file() else "UNKNOWN",
    }
    return env


def write_environment_txt(env: dict, path: Path) -> None:
    lines = [
        f"CHUNK-E0 host environment — {env['host_id']}",
        f"schema: {env['schema']}",
        f"captured_at_utc: {env['captured_at_utc']}",
        f"campaign: {env['campaign']}",
        "",
        "GIT",
        f"  commit: {env['git']['commit']}",
        f"  branch: {env['git']['branch']}",
        f"  dirty: {env['git']['dirty']}",
        "",
        "OS",
        f"  uname: {env['os']['uname']}",
        f"  distro: {env['os']['distro_id']} {env['os']['distro_version']}",
        f"  kernel: {env['os']['kernel']}",
        "",
        "CPU",
        f"  vendor: {env['cpu']['vendor']}",
        f"  model: {env['cpu']['model']}",
        f"  microarchitecture: {env['cpu']['microarchitecture']['label']}",
        f"  smt: {env['cpu']['smt']}",
        f"  cache line: {env['cpu']['cache_line_bytes']} B",
        "",
        "MEMORY",
        f"  mem_total: {env['memory']['mem_total']}",
        f"  page size: {env['memory']['page_size']} B",
        "",
        "FREQUENCY",
        f"  governor: {env['frequency']['governor']}",
        f"  intel no_turbo: {env['frequency']['intel_pstate_no_turbo']}",
        f"  arm cpufreq boost: {env['frequency']['arm_cpufreq_boost']}",
        "",
        "FILESYSTEM / STORAGE",
        f"  findmnt: {env['filesystem_storage']['findmnt']}",
        f"  storage class: {env['filesystem_storage']['storage_class']}",
        "",
        "VIRTUALIZATION",
        f"  systemd-detect-virt: "
        f"{env['virtualization']['systemd_detect_virt']}",
        f"  container: {env['virtualization']['container']}",
        f"  dmi: {env['virtualization']['dmi_product']}",
        "",
        "NOTE: HOST-LOCAL RESULT ONLY. This runner performs no cross-host",
        "causality attribution.",
        "",
    ]
    path.write_text("\n".join(lines))


# --------------------------------------------------------------------------
# Tool capability probe
# --------------------------------------------------------------------------

def probe_tools() -> dict:
    tools = {}
    for name in ("python3", "clang", "gcc", "xmake", "git", "perf",
                 "sha256sum", "zstd", "taskset"):
        path = shutil.which(name)
        ver = "UNKNOWN"
        if path:
            rc, out, _ = run_cmd([name, "--version"])
            if rc == 0:
                ver = out.splitlines()[0] if out else "UNKNOWN"
        tools[name] = {"found": path is not None, "path": path or None,
                       "version": ver}

    events = {"instructions:u": "UNAVAILABLE", "cycles:u": "UNAVAILABLE"}
    if tools["perf"]["found"]:
        for ev in events:
            rc, out, err = run_cmd(["perf", "stat", "-x,", "-e", ev, "true"])
            if rc == 0:
                events[ev] = "RELIABLE"
            elif "permission" in err.lower() or "not supported" in err.lower():
                events[ev] = "BLOCKED"
            else:
                events[ev] = "AVAILABLE_BUT_DEMOTED"
    # cycles:u is DEMOTED by the frozen PMU rule whenever perf is usable
    # (frequency-scaling confounder) — the probe only decides whether perf
    # itself works, it never upgrades cycles.
    if events["cycles:u"] == "RELIABLE":
        events["cycles:u"] = "AVAILABLE_BUT_DEMOTED"
    tools["perf_events"] = events
    return tools


def missing_dependencies(tools: dict) -> list:
    need = ["python3", "git", "xmake", "sha256sum", "perf"]
    missing = [n for n in need if not tools[n]["found"]]
    if not tools["clang"]["found"] and not tools["gcc"]["found"]:
        missing.append("compiler (clang or gcc)")
    return missing


def install_command_for(distro_id: str) -> dict:
    pkgs = {
        "fedora": "sudo dnf install -y python3 git xmake clang perf zstd",
        "rhel": "sudo dnf install -y python3 git xmake clang perf zstd",
        "ubuntu": ("sudo apt-get update && sudo apt-get install -y "
                   "python3 git xmake clang linux-tools-common "
                   "linux-tools-generic zstd"),
        "debian": ("sudo apt-get update && sudo apt-get install -y "
                   "python3 git xmake clang linux-perf zstd"),
    }
    if distro_id in pkgs:
        return {distro_id: pkgs[distro_id]}
    return {"generic": ("(no supported package manager detected — install "
                        "python3, git, xmake, a C++20 compiler and perf "
                        "manually)")}


# --------------------------------------------------------------------------
# Frozen-matrix validation (campaign.json <-> chunk_e0.py)
# --------------------------------------------------------------------------

def validate_frozen_matrix() -> dict:
    camp = read_json(CAMPAIGN_JSON)
    mismatches = []
    if camp.get("chunks_bytes") != driver.CHUNKS:
        mismatches.append("chunks_bytes")
    if camp.get("depths") != driver.DEPTHS:
        mismatches.append("depths")
    if camp.get("repetitions") != driver.ROUNDS:
        mismatches.append("repetitions")
    if camp.get("total_bytes") != driver.FILE_BYTES:
        mismatches.append("total_bytes")
    try:
        if int(str(camp.get("seed")), 16) != driver.PREREG_SEED:
            mismatches.append("seed")
    except (TypeError, ValueError):
        mismatches.append("seed")
    cells_per_round = len(driver.CHUNKS) * len(driver.DEPTHS)
    return {
        "campaign_json": str(CAMPAIGN_JSON),
        "schema": camp.get("schema"),
        "preregistration": camp.get("preregistration"),
        "aligned": not mismatches,
        "mismatches": mismatches,
        "cells_per_round": cells_per_round,
        "expected_runs": cells_per_round * driver.ROUNDS,
        "total_bytes": driver.FILE_BYTES,
        "chunks": driver.CHUNKS,
        "depths": driver.DEPTHS,
        "repetitions": driver.ROUNDS,
    }


# --------------------------------------------------------------------------
# Build (provenance-aware)
# --------------------------------------------------------------------------

def find_bench_binary() -> Path | None:
    hits = [Path(h) for h in glob.glob(
        str(REPO / "build" / "linux" / "*" / "release" / BENCH_TARGET))]
    hits = [h for h in hits if h.is_file()]
    if not hits:
        return None
    return max(hits, key=lambda h: h.stat().st_mtime)


def build_bench(tools: dict) -> dict:
    """Build the production-engine bench with the preregistered Release
    config, preferring clang. If clang is absent the default toolchain
    (gcc) is used and the session is marked BUILD_VARIANT — a separate
    evidence class, never silently merged with preferred-compiler
    evidence."""
    _try_mkdir(REPO / "build")
    preferred = tools["clang"]["found"]
    variant = "preferred(clang)" if preferred else "BUILD_VARIANT(gcc-default)"
    cfg_cmd = ["xmake", "f", "-m", "release", "-y"]
    if preferred:
        cfg_cmd += ["--toolchain=clang"]
    rc, out, err = run_cmd(cfg_cmd, cwd=REPO)
    if rc != 0 and preferred:
        log("clang toolchain config failed; falling back to default "
            "(BUILD_VARIANT)")
        variant = "BUILD_VARIANT(gcc-default)"
        rc, out, err = run_cmd(["xmake", "f", "-m", "release", "-y"],
                               cwd=REPO)
    if rc != 0:
        return {"ok": False, "stage": "configure",
                "error": err or out, "variant": variant}
    rc, out, err = run_cmd(["xmake", "build", BENCH_TARGET], cwd=REPO,
                           timeout=1800)
    if rc != 0:
        return {"ok": False, "stage": "build",
                "error": (err or out)[-2000:], "variant": variant}
    binary = find_bench_binary()
    if binary is None:
        return {"ok": False, "stage": "locate-binary",
                "error": "built but binary not found", "variant": variant}
    rc, ver, _ = run_cmd(["xmake", "--version"])
    cc = "UNKNOWN"
    if preferred:
        rc, cc, _ = run_cmd(["clang", "--version"])
    else:
        rc, cc, _ = run_cmd(["gcc", "--version"])
    cc = cc.splitlines()[0] if cc else "UNKNOWN"
    return {
        "ok": True,
        "binary_path": str(binary),
        "binary_sha256": sha256_file(binary),
        "binary_size": binary.stat().st_size,
        "compiler": "clang" if preferred else "gcc",
        "compiler_version": cc,
        "variant": variant,
        "xmake_version": ver.splitlines()[0] if ver else "UNKNOWN",
        "config": "release",
    }


# --------------------------------------------------------------------------
# Preflight
# --------------------------------------------------------------------------

def disk_budget() -> dict:
    camp = read_json(CAMPAIGN_JSON)
    total = camp["total_bytes"]
    est_required = 2 * total + (64 << 20)  # fixture + dst + evidence margin
    need = est_required * SAFETY_FACTOR
    free = "UNKNOWN"
    try:
        st = os.statvfs(str(HOST_WORK))
        free = st.f_bavail * st.f_frsize
    except Exception:  # noqa: BLE001
        pass
    return {
        "fixture_bytes": total,
        "destination_bytes": total,
        "evidence_margin_bytes": 64 << 20,
        "estimated_required_bytes": est_required,
        "safety_factor": SAFETY_FACTOR,
        "required_with_safety_bytes": need,
        "free_bytes": free,
        "sufficient": isinstance(free, int) and free > need,
    }


def preflight(tools: dict, matrix: dict, build: dict,
              profile: str = "full") -> dict:
    perf_events_ok = tools["perf_events"]["instructions:u"] != "UNAVAILABLE"
    checks = {
        "working_dir_writable": _try_mkdir(HOST_WORK),
        "campaign_matrix_aligned": matrix["aligned"],
        "compiler_present": tools["clang"]["found"] or tools["gcc"]["found"],
        "xmake_present": tools["xmake"]["found"],
        "build_ok": build["ok"],
        "disk_budget_ok": disk_budget()["sufficient"],
        "bench_binary_sha256": build.get("binary_sha256", "MISSING"),
    }
    # Only profile=full hard-requires usable perf events: the FROZEN formal
    # gates record instructions:u per run. smoke/verify proceed with a
    # warning (runs will fail closed as gate errors, honestly recorded);
    # perf unavailability degrades classification, it never crashes the
    # benchmark itself.
    perf_required = profile == "full"
    checks["perf_usable"] = (tools["perf"]["found"] and perf_events_ok) \
        if perf_required else perf_events_ok
    ok = all(v is True for k, v in checks.items() if k !=
             "bench_binary_sha256" and (k != "perf_usable" or perf_required))
    disk = disk_budget()
    warning = None
    if not perf_required and not checks["perf_usable"]:
        warning = ("perf events unavailable: smoke/verify gate errors will "
                   "record this; no formal evidence possible on this host")
    return {
        "pass": ok and checks["bench_binary_sha256"] != "MISSING",
        "checks": checks,
        "disk": disk,
        "build": build,
        "warning": warning,
        "runtime_estimate": {"expected_runs": matrix["expected_runs"],
                             "note": "rough estimate only; never a "
                                     "scientific conclusion"},
    }


# --------------------------------------------------------------------------
# Session status model
# --------------------------------------------------------------------------

def set_session_status(session_dir: Path, status: str) -> None:
    (session_dir / "status.txt").write_text(status + "\n")


def session_status(session_dir: Path) -> str:
    try:
        return (session_dir / "status.txt").read_text().strip()
    except Exception:  # noqa: BLE001
        return "UNKNOWN"


# --------------------------------------------------------------------------
# Fixture + driver binding (host-scoped)
# --------------------------------------------------------------------------

def host_data_dir(host_id: str) -> Path:
    d = HOST_WORK / host_id / "data"
    d.mkdir(parents=True, exist_ok=True)
    return d


def bind_driver(host_id: str, build: dict) -> None:
    """Point the imported measurement driver at the host-scoped data dir
    and bench binary (cross-platform path override; frozen scientific
    constants are untouched)."""
    driver.DATA_DIR = host_data_dir(host_id)
    driver.BENCH = Path(build["binary_path"])
    driver.RESULTS = RESULTS_DIR


def ensure_fixture(host_id: str, build: dict) -> str:
    """Generate the 1 GiB deterministic src fixture once per host (if
    missing or wrong-sized) and verify it against the canonical frozen
    pattern hash from campaign.json. The generator is pure splitmix64 with
    a fixed seed, so every host must produce byte-identical content;
    divergence FAILS CLOSED (never measure a drifted workload)."""
    bind_driver(host_id, build)
    src = driver.DATA_DIR / "src.bin"
    want = read_json(CAMPAIGN_JSON)["fixture_sha256"]
    if src.is_file() and src.stat().st_size == driver.FILE_BYTES \
            and sha256_file(src) == want:
        return want
    if src.exists():
        src.unlink()
    rc, out, err = run_cmd([
        str(build["binary_path"]), "--generate", "--src", str(src),
        "--file-bytes", str(driver.FILE_BYTES)], cwd=REPO)
    if rc != 0:
        log(f"fixture generation failed: {err or out}")
        sys.exit(1)
    got = driver.sha256_file(src)
    if got != want:
        log(f"FIXTURE FAIL CLOSED — sha256 {got} != canonical {want}")
        sys.exit(1)
    return got


def call_driver(args: list, env: dict) -> tuple[int, str, str]:
    return run_cmd([sys.executable, str(SCRIPTS_DIR / "chunk_e0.py")] + args,
                   cwd=REPO, env=env)


def driver_env(host_id: str, build: dict) -> dict:
    env = dict(os.environ)
    env["CHUNK_E0_DATA_DIR"] = str(host_data_dir(host_id))
    env["CHUNK_E0_BENCH"] = build["binary_path"]
    env["CHUNK_E0_RESULTS"] = str(RESULTS_DIR)
    return env


# --------------------------------------------------------------------------
# Profiles
# --------------------------------------------------------------------------

def run_smoke(session_id: str, host_id: str, matrix: dict, build: dict,
              env: dict) -> Path:
    """PROFILE=smoke: a couple of tiny cells + hash/perf/fs correctness.
    NEVER mixed into formal results."""
    src_sha = ensure_fixture(host_id, build)
    manifest = {
        "base": "master (post-#269)", "kind": "smoke",
        "campaign": args.campaign, "host_id": host_id,
        "profile": "smoke", "runner_schema": RUNNER_SCHEMA,
        "data_src_sha256": src_sha, "file_bytes": matrix["total_bytes"],
        "build": {k: v for k, v in build.items() if k != "ok"},
        "preregistration": PREREG,
        "note": "PROFILE=SMOKE. NOT FORMAL EVIDENCE.",
    }
    sd = driver.new_session(session_id, "smoke correctness check", manifest)
    set_session_status(sd, "SMOKE_ONLY")
    gates = driver.Gates(sd)
    for i, (c, d) in enumerate(((65536, 1), (1048576, 1))):
        driver.bench_run(sd, gates, manifest, f"smoke-{i:02d}", c, d)
    gates.persist(manifest | {"runs_total": 2}, runs_total=2)
    (sd / "notes.md").write_text(
        f"# {session_id} — notes\n\nPROFILE=SMOKE. NOT FORMAL EVIDENCE. "
        "2 cells (64K d1, 1M d1) at 1 GiB, same-work + hash fail-closed.\n")
    (sd / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    write_environment_txt(env, sd / "environment.txt")
    if len(gates.errors) > 0:
        set_session_status(sd, "INVALID")
        log(f"smoke FAILED: {len(gates.errors)} gate errors")
    else:
        log("smoke PASS: 2 runs, 0 gate errors, hash OK")
    return sd


def run_verify(session_id: str, host_id: str, matrix: dict, build: dict,
               env: dict) -> Path:
    """PROFILE=verify: small representative matrix (campaign.json
    verify_matrix), validation, NO scientific promotion."""
    src_sha = ensure_fixture(host_id, build)
    camp = read_json(CAMPAIGN_JSON)
    vm = camp["verify_matrix"]
    manifest = {
        "base": "master (post-#269)", "kind": "verify",
        "campaign": args.campaign, "host_id": host_id,
        "profile": "verify", "runner_schema": RUNNER_SCHEMA,
        "data_src_sha256": src_sha, "file_bytes": matrix["total_bytes"],
        "verify_matrix": vm,
        "build": {k: v for k, v in build.items() if k != "ok"},
        "preregistration": PREREG,
        "note": "PROFILE=VERIFY. NOT FORMAL EVIDENCE. NO SCIENTIFIC "
                "PROMOTION.",
    }
    sd = driver.new_session(session_id, "verify profile (not formal)",
                            manifest)
    set_session_status(sd, "VERIFY_ONLY")
    gates = driver.Gates(sd)
    cells = [(c, d) for c in vm["chunks_bytes"] for d in vm["depths"]
             for _ in range(vm["repetitions"])]
    for i, (c, d) in enumerate(cells):
        driver.bench_run(sd, gates, manifest, f"v{i:03d}", c, d)
    gates.persist(manifest | {"runs_total": len(cells)},
                  runs_total=len(cells))
    (sd / "notes.md").write_text(
        f"# {session_id} — notes\n\nPROFILE=VERIFY. NOT FORMAL EVIDENCE. "
        f"{len(cells)} runs (6 chunks x d{{1,4}} x 2 reps) at 1 GiB. "
        "No sweet-spot claims are made from this profile.\n")
    (sd / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    write_environment_txt(env, sd / "environment.txt")
    if len(gates.errors) > 0:
        set_session_status(sd, "INVALID")
        log(f"verify FAILED: {len(gates.errors)} gate errors")
        return sd
    log(f"verify PASS: {len(cells)} runs, 0 gate errors, hash OK")
    rows = driver.cells_stats(driver.load_runs(session_id))
    with (sd / "summary.csv").open("w", newline="") as f:
        f.write("chunk,depth,n,mibps_median,instructions_per_byte\n")
        for (c, d), s in sorted(rows.items()):
            f.write(f"{c},{d},{s['n']},{s['mibps_median']:.2f},"
                    f"{s['instructions_per_byte']:.4f}\n")
    write_json(sd / "summary.json", [
        {"chunk": c, "depth": d,
         **{k: v for k, v in s.items() if k not in ("chunk", "depth")}}
        for (c, d), s in sorted(rows.items())])
    return sd


def run_full(session_id: str, host_id: str, campaign: str, build: dict,
             matrix: dict, resume: bool = False) -> Path:
    """PROFILE=full: the OFFICIAL frozen sweep, delegated to chunk_e0.py
    (generate -> sweep -> summarize), then validation, plots, host summary
    and packaging. Only VALID + full is formal evidence."""
    denv = driver_env(host_id, build)
    rc, out, err = call_driver(["generate", session_id], denv)
    if rc != 0 and "already exists" not in (err + out):
        log(f"generate failed: {err or out}")
        sys.exit(1)
    sweep_args = ["sweep", session_id]
    if resume:
        sweep_args.append("--resume")
    rc, out, err = call_driver(sweep_args, denv)
    if rc != 0:
        log(f"sweep failed: {err or out}")
        sys.exit(1)
    _, git_head, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=REPO)
    write_json(RESULTS_DIR / session_id / "runner.json", {
        "schema": RUNNER_SCHEMA,
        "host_id": host_id,
        "campaign": campaign,
        "git_commit": git_head or "?",
        "build_binary_sha256": build.get("binary_sha256"),
        "file_bytes": matrix["total_bytes"],
        "note": "runner identity snapshot for --resume verification",
    })
    rc, out, err = call_driver(["summarize", session_id], denv)
    if rc != 0:
        log(f"summarize failed: {err or out}")
        sys.exit(1)
    return RESULTS_DIR / session_id


# --------------------------------------------------------------------------
# Validation of a completed official session (VALID / INVALID)
# --------------------------------------------------------------------------

def validate_session(session_id: str, matrix: dict) -> dict:
    sd = RESULTS_DIR / session_id
    errors = []
    runs = driver.load_runs(session_id)
    run_ids = [r["run_id"] for r in runs]
    if len(run_ids) != len(set(run_ids)):
        errors.append("duplicate run_id")
    if any(not r.get("ok") for r in runs):
        errors.append("non-ok run recorded in valid session")

    cells = [(c, d) for c in driver.CHUNKS for d in driver.DEPTHS]
    expected_ids = driver.ordered_run_ids(cells, driver.ROUNDS)
    have = set(run_ids)
    missing = [i for i in expected_ids if i not in have]
    extra = [i for i in have if i not in set(expected_ids)]
    if missing:
        errors.append(f"missing run_ids: {missing[:5]}")
    if extra:
        errors.append(f"unexpected run_ids: {sorted(extra)[:5]}")

    counts: dict[tuple, int] = {}
    for r in runs:
        counts[(r["chunk"], r["depth"])] = counts.get(
            (r["chunk"], r["depth"]), 0) + 1
    for c, d in cells:
        if counts.get((c, d), 0) != matrix["repetitions"]:
            errors.append(f"cell ({c},{d}) reps={counts.get((c, d), 0)} "
                          f"!= {matrix['repetitions']}")

    gates = read_json(sd / "gates.json")
    if gates.get("gate_errors", -1) != 0:
        errors.append(f"gate_errors={gates['gate_errors']}")
    if gates.get("runs_total") != matrix["expected_runs"]:
        errors.append(f"runs_total={gates.get('runs_total')} != "
                      f"{matrix['expected_runs']}")

    perf_rows = set()
    perf_path = sd / "raw" / "perf.csv"
    if perf_path.is_file():
        for line in perf_path.read_text().splitlines():
            parts = line.split(",")
            if len(parts) >= 2:
                perf_rows.add(parts[0])
    missing_perf = [i for i in run_ids if i not in perf_rows]
    if missing_perf:
        errors.append(f"perf.csv missing {len(missing_perf)} rows")

    return {"session": session_id, "valid": not errors, "errors": errors,
            "runs": len(runs), "expected_runs": matrix["expected_runs"],
            "gate_errors": gates.get("gate_errors", -1)}


# --------------------------------------------------------------------------
# Host summary + packaging
# --------------------------------------------------------------------------

def write_host_summary(session_id: str, env: dict, build: dict,
                       analysis: dict, status: str, profile: str,
                       validity: dict | None) -> Path:
    sd = RESULTS_DIR / session_id
    ss = analysis.get("sweet_spots", {})
    lines = [
        "HOST SUMMARY — CHUNK-E0 (#270)",
        f"HOST ID: {env['host_id']}",
        f"CAMPAIGN: {env['campaign']}",
        f"SESSION: {session_id}",
        f"STATUS: {status}",
        f"PROFILE: {profile}",
        f"VALIDITY: {'VALID' if validity and validity['valid'] else 'INVALID'}",
        "",
        f"ISA: {env['identity']['machine']}",
        f"CPU: {env['cpu']['vendor']} {env['cpu']['model']}",
        f"MICROARCH: {env['cpu']['microarchitecture']['label']}",
        f"OS: {env['os']['distro_id']} {env['os']['distro_version']}",
        f"KERNEL: {env['os']['kernel']}",
        f"FILESYSTEM: {env['filesystem_storage']['findmnt']}",
        f"STORAGE: {env['filesystem_storage']['storage_class']}",
        f"VIRTUALIZATION: {env['virtualization']['systemd_detect_virt']}",
        f"BUILD VARIANT: {build.get('variant', 'UNKNOWN')}",
        "",
        "TESTED RANGE:",
    ]
    for d in (1, 2, 4, 8):
        s = ss.get(str(d), {})
        lines.append(f"  depth {d}:")
        lines.append(f"    peak: {s.get('tested_range_peak_chunk', 'N/A')} "
                     f"@ {s.get('tested_range_peak_mibps', 'N/A')} MiB/s")
        lines.append(f"    95%:  {s.get('p95_point_chunk', 'N/A')}")
        lines.append(f"    plateau: {s.get('plateau_entry_chunk', 'N/A')}")
        lines.append(f"    knee: {s.get('knee_label', 'N/A')} "
                     f"({s.get('knee_chunk', 'N/A')})")
    pareto = analysis.get("pareto", {})
    lines.append("")
    lines.append(f"PARETO SWEET REGION "
                 f"({pareto.get('frontier_size', 0)} non-dominated points):")
    for f in pareto.get("frontier", [])[:8]:
        lines.append(f"  chunk={f['chunk']} depth={f['depth']} "
                     f"inflight={f['in_flight_bytes']}B "
                     f"{f['mibps_median']} MiB/s "
                     f"{f['instructions_per_byte']} instr/B")
    lines.append("")
    lines.append("CLAIM LEVEL: HOST-LOCAL ONLY")
    lines.append("")
    lines.append("WARNINGS: cycles:u is DEMOTED by the frozen PMU rule "
                 "(frequency-scaling confounder); no IPC claims.")
    if profile != "full":
        lines.append(f"PROFILE={profile.upper()} — NOT FORMAL EVIDENCE.")
    lines.append("")
    path = sd / "HOST-SUMMARY.md"
    path.write_text("\n".join(lines) + "\n")
    return path


def package_session(session_id: str, env: dict) -> dict:
    """Create the portable evidence archive (tar.zst, fallback tar.gz)
    containing the immutable session + plots + ARCHIVE-MANIFEST + a
    SHA256SUMS file; prints the archive's own sha256."""
    ARCHIVES_DIR.mkdir(parents=True, exist_ok=True)
    sd = RESULTS_DIR / session_id
    tools = probe_tools()
    ext = "tar.zst" if tools["zstd"]["found"] else "tar.gz"
    name = f"chunk-e0-{env['campaign']}-{env['host_id']}-{session_id}.{ext}"
    archive = ARCHIVES_DIR / name
    if archive.exists():
        archive.unlink()

    stage = ARCHIVES_DIR / f".stage-{session_id}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    shutil.copytree(sd, stage / "session")
    if PLOTS_DIR.is_dir():
        shutil.copytree(PLOTS_DIR, stage / "plots")

    write_json(stage / "session" / "ARCHIVE-MANIFEST.json", {
        "schema": RUNNER_SCHEMA,
        "session": session_id,
        "host_id": env["host_id"],
        "campaign": env["campaign"],
        "members": ["session/", "plots/"],
        "note": "Evidence archive for CHUNK-E0 (#270). No credentials, no "
                "cloud metadata.",
    })
    # Inner checksum manifest covering every staged member; import_host.py
    # verifies it after extraction (outer .sha256 protects the archive
    # itself, SHA256SUMS protects the members).
    sums = []
    for p in sorted(stage.rglob("*")):
        if p.is_file():
            sums.append(f"{sha256_file(p)}  {p.relative_to(stage)}")
    (stage / "SHA256SUMS").write_text("\n".join(sums) + "\n")

    if ext == "tar.zst":
        rc, out, err = run_cmd(["tar", "-I", "zstd", "-cf", str(archive), "."],
                               cwd=stage)
    else:
        rc, out, err = run_cmd(["tar", "-czf", str(archive), "."], cwd=stage)
    if rc != 0:
        shutil.rmtree(stage, ignore_errors=True)
        return {"ok": False, "error": err or out, "archive": str(archive)}
    shutil.rmtree(stage, ignore_errors=True)

    sha = sha256_file(archive)
    (ARCHIVES_DIR / f"{name}.sha256").write_text(f"{sha}  {name}\n")
    return {"ok": True, "archive": str(archive), "sha256": sha,
            "compressor": ext}


# --------------------------------------------------------------------------
# Signal handling
# --------------------------------------------------------------------------

_ACTIVE_SESSION: list[Path] = []


def _on_signal(signum, frame):
    log(f"received signal {signum}; marking session INCOMPLETE")
    for sd in _ACTIVE_SESSION:
        try:
            set_session_status(sd, "INCOMPLETE")
            notes = sd / "notes.md"
            if notes.exists():
                notes.write_text(notes.read_text() +
                                 f"\n\nINTERRUPTED by signal {signum} — "
                                 "SESSION INCOMPLETE.\n")
        except Exception:  # noqa: BLE001
            pass
    sys.exit(128 + signum)


# --------------------------------------------------------------------------
# CLI / main
# --------------------------------------------------------------------------

def generate_host_id(campaign: str) -> str:
    arch = platform.machine().replace("_", "-")
    vendor = "unknown"
    rc, out, _ = run_cmd(["lscpu"])
    if rc == 0:
        vendor = _cpu_vendor(out)
    return f"{campaign.lower()}-{arch}-{vendor}-{now_utc_compact()}"


def parse_args(argv):
    ap = argparse.ArgumentParser(
        description="CHUNK-E0 portable one-command host runner (#270).")
    ap.add_argument("--host-id", default=None,
                    help="explicit host identity (auto-generated if absent)")
    ap.add_argument("--campaign", default="H1", choices=["H1", "H2", "H0"],
                    help="campaign label (does not alter the frozen matrix)")
    ap.add_argument("--profile", choices=["full", "verify", "smoke"],
                    default="full")
    ap.add_argument("--preflight-only", action="store_true",
                    help="fail-fast checks only; no measurement")
    ap.add_argument("--resume", metavar="SESSION",
                    help="continue an interrupted full session")
    ap.add_argument("--print-install-command", action="store_true",
                    help="print suggested package-install commands and exit")
    return ap.parse_args(argv)


def resume_identity_ok(session_id: str, build: dict, matrix: dict,
                       host_id: str, campaign: str) -> dict:
    """Resume gate: the interrupted session must be a runner-created full
    sweep on THIS host, same git commit, same bench binary, same frozen
    workload. Any mismatch -> RESUME REFUSED (never mix sessions)."""
    sd = RESULTS_DIR / session_id
    if not sd.is_dir():
        return {"ok": False, "reason": "session dir not found"}
    try:
        manifest = read_json(sd / "manifest.json")
    except Exception:  # noqa: BLE001
        return {"ok": False, "reason": "session manifest unreadable"}
    if manifest.get("kind") != "sweep":
        return {"ok": False, "reason": f"session kind={manifest.get('kind')} "
                                       "!= sweep"}
    rj_path = sd / "runner.json"
    if not rj_path.is_file():
        return {"ok": False,
                "reason": "runner.json missing (not a runner-created "
                          "full session)"}
    rj = read_json(rj_path)
    problems = []
    if rj.get("host_id") != host_id:
        problems.append("host identity changed")
    if rj.get("campaign") != campaign:
        problems.append(f"campaign {rj.get('campaign')} != {campaign}")
    if rj.get("git_commit") not in (None, "?"):
        ok, out = run_ok(["git", "rev-parse", "HEAD"], cwd=REPO)
        if ok and out != rj["git_commit"]:
            problems.append("git commit changed")
    if rj.get("build_binary_sha256") != build.get("binary_sha256"):
        problems.append("bench binary sha256 changed")
    if rj.get("file_bytes") != matrix["total_bytes"]:
        problems.append("total bytes changed")
    if problems:
        return {"ok": False, "reason": "; ".join(problems)}
    return {"ok": True}


def main(argv=None) -> int:
    global args
    args = parse_args(argv)
    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    host_id = args.host_id or generate_host_id(args.campaign)
    if args.host_id is None:
        log(f"auto host-id: {host_id}")

    if args.print_install_command:
        tools = probe_tools()
        missing = missing_dependencies(tools)
        if not missing:
            print("no missing dependencies detected")
            return 0
        distro_id = "unknown"
        try:
            for line in Path("/etc/os-release").read_text().splitlines():
                if line.startswith("ID="):
                    distro_id = line.split("=", 1)[1].strip().strip('"')
        except Exception:  # noqa: BLE001
            pass
        for k, v in install_command_for(distro_id).items():
            print(f"# {k}")
            print(v)
        return 0

    # ---- phase 1: matrix + tools + env ----
    matrix = validate_frozen_matrix()
    if not matrix["aligned"]:
        log(f"PREFLIGHT FAIL — campaign.json diverges from chunk_e0.py "
            f"frozen constants: {matrix['mismatches']}")
        return 1
    tools = probe_tools()
    missing = missing_dependencies(tools)
    if missing:
        log(f"PREFLIGHT FAIL — MISSING_DEPENDENCIES: {missing}")
        log("(no automatic installation; use --print-install-command for "
            "suggestions)")
        return 1

    # ---- phase 2: build ----
    build = build_bench(tools)
    if not build["ok"]:
        log(f"PREFLIGHT FAIL — build failed at {build['stage']}: "
            f"{build.get('error', '')[:300]}")
        return 1
    log(f"bench binary: {build['binary_path']} sha256="
        f"{build['binary_sha256'][:16]} variant={build['variant']}")

    # ---- phase 3: preflight ----
    pf = preflight(tools, matrix, build, profile=args.profile)
    if not pf["pass"]:
        log("PREFLIGHT FAIL — " + json.dumps(pf["checks"]))
        return 1
    log("PREFLIGHT PASS")
    if pf.get("warning"):
        log("WARNING — " + pf["warning"])
    log(f"expected runs (full): {matrix['expected_runs']}; "
        f"disk required ~{pf['disk']['required_with_safety_bytes'] / (1 << 30):.2f} GiB; "
        f"free {pf['disk']['free_bytes'] / (1 << 30):.2f} GiB")
    if args.preflight_only:
        log("preflight-only: stopping before measurement")
        return 0

    # ---- phase 4: session identity ----
    if args.resume and args.profile != "full":
        log("RESUME REFUSED — --resume only applies to --profile full")
        return 1
    if args.resume:
        session_id = args.resume
    elif args.profile == "full":
        session_id = (f"chunk-e0-{args.campaign.lower()}-{host_id}-"
                      f"full-{now_utc_compact()}")
    else:
        session_id = (f"chunk-e0-{args.campaign.lower()}-{host_id}-"
                      f"{args.profile}-{now_utc_compact()}")

    # Bind the driver to host-scoped paths BEFORE environment capture so
    # data_src_sha256 refers to the fixture this session will actually use.
    bind_driver(host_id, build)
    env = capture_environment(host_id, args.campaign)

    if args.resume:
        gate = resume_identity_ok(session_id, build, matrix, host_id,
                                  args.campaign)
        if not gate["ok"]:
            log(f"RESUME REFUSED — {gate['reason']}")
            return 1
        log(f"resuming session {session_id}")

    # ---- phase 5: measurement ----
    _ACTIVE_SESSION.append(RESULTS_DIR / session_id)
    if args.profile == "smoke":
        sd = run_smoke(session_id, host_id, matrix, build, env)
    elif args.profile == "verify":
        sd = run_verify(session_id, host_id, matrix, build, env)
    else:
        sd = run_full(session_id, host_id, args.campaign, build, matrix,
                      resume=bool(args.resume))

    # ---- phase 6: status / analysis / packaging ----
    if args.profile == "full":
        validity = validate_session(session_id, matrix)
        if not validity["valid"]:
            set_session_status(sd, "INVALID")
            log("SESSION INVALID — " + "; ".join(validity["errors"]))
            return 1
        set_session_status(sd, "VALID")
        log(f"SESSION VALID — {validity['runs']} runs, "
            f"{validity['gate_errors']} gate errors, ordering provenance OK")
        analysis = read_json(sd / "analysis.json")
        write_host_summary(session_id, env, build, analysis, "VALID", "full",
                           validity)
        rc, out, err = run_cmd([sys.executable,
                                str(SCRIPTS_DIR / "plot_chunk_e0.py"),
                                session_id], cwd=REPO)
        log(f"plots: {'generated' if rc == 0 else (err or out)}")
        pkg = package_session(session_id, env)
        if pkg["ok"]:
            log(f"ARCHIVE: {pkg['archive']}")
            log(f"ARCHIVE sha256: {pkg['sha256']}")
        else:
            log(f"packaging failed: {pkg.get('error', '')}")
    else:
        log(f"profile={args.profile} status={session_status(sd)} — "
            "NOT FORMAL EVIDENCE")

    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
