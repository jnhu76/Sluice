#!/usr/bin/env python3
"""Structural validator for committed performance-evidence artifacts.

Machine enforcement for docs/verification/performance-engineering.md: a
performance claim is backed by machine-readable evidence whose STRUCTURE is
validated here — not by prose. This deliberately does NOT check absolute
speeds (no "must exceed X GB/s" thresholds on shared runners); it checks
that an artifact records what a claim needs to be attributable and
reproducible:

  - artifact schema version (the runner and validator evolve together; a
    stale-shape artifact fails loudly instead of silently);
  - executable provenance: path + sha256 + size of the binary that actually
    ran (the git SHA says what was checked out; the hash says what was
    executed — a stale binary under a fresh checkout cannot pose as
    evidence for that checkout);
  - baseline/candidate git SHA + dirty state (+ a provenance note when the
    tree was dirty, so a dirty measurement can never masquerade as clean);
  - build mode / compiler / environment fingerprint (CPU, kernel, WSL,
    filesystem, tool versions);
  - workload parameters, iterations, warmups;
  - raw per-iteration samples;
  - derived statistics consistent with the raw samples (min <= med <= max,
    recomputed median within rounding tolerance);
  - CLI rows are semantically comparable evidence: outputs_equal must be
    true and every competitor on the same workload must have produced the
    same output hash (timing from runs with different output semantics is
    not a speed comparison; deliberate semantics studies must mark rows
    non_comparable, which excludes them from ratio evidence);
  - perf artifacts record the measured command's exit status plus an
    explicit exit_semantics classifier, and the status must be valid data
    under that classifier.

Usage:
  perf-evidence-validate.py                 # validate committed artifacts
  perf-evidence-validate.py --file F [...]  # validate specific files
  perf-evidence-validate.py --self-test     # prove the detectors fire

Exit code: 0 iff every checked artifact is structurally valid.
An empty evidence directory is a pass (nothing to validate); the gate
exists to keep committed artifacts honest, not to force artifacts to exist.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_EVIDENCE_DIR = REPO / "docs" / "results" / "performance-attribution"

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MD5_RE = re.compile(r"^[0-9a-f]{32}$")
VALID_DATA_EXIT_CODES = {0, 1}  # grep family: 0=match 1=no-match 2=error
HEX40_TOLERANCE_MEDIAN = 1.0  # ns; %.1f rounding + mean-of-middle pair

# Sustained RSS boundedness: the #199 overload bench records a separate
# sustained phase (fixed capacity, no latency sampling, fixed-size sample
# reservoirs) and the validator requires the RSS to plateau. delta_kb is the
# signed end-minus-start RSS over that interval; a delta above this bound is
# a growth trend the harness cannot explain (its own storage is bounded).
RSS_PLATEAU_MAX_KB = 256
# A sustained phase shorter than this cannot support a boundedness claim.
SUSTAINED_ROUNDS_MIN = 500

REQUIRED_SCHEMA = 2
# Measured-command exit classifiers (runner --exit-semantics): which exit
# codes still count as measurement data.
EXIT_SEMANTICS = {"grep-family": {0, 1}, "strict-zero": {0}}

_MISSING = object()


def _dig(obj, path, default=None):
    cur = obj
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            return default
        cur = cur[k]
    return cur


def _check_binary_block(art: dict, key: str) -> list[str]:
    """Validate an executable-provenance block (path/sha256/size/mtime)."""
    errs = []
    binp = art.get(key)
    if not isinstance(binp, dict):
        return [f"{key}: missing executable provenance "
                f"(path/sha256/size/mtime)"]
    if not isinstance(binp.get("path"), str) or not binp.get("path"):
        errs.append(f"{key}.path: missing/empty")
    sha = binp.get("sha256")
    if not isinstance(sha, str) or not SHA256_RE.match(sha):
        errs.append(f"{key}.sha256: expected 64-hex digest, got {sha!r}")
    size = binp.get("size")
    if not isinstance(size, int) or size < 1:
        errs.append(f"{key}.size: expected positive int, got {size!r}")
    if not isinstance(binp.get("mtime"), (int, float)):
        errs.append(f"{key}.mtime: missing")
    return errs


def check_common(art: dict) -> list[str]:
    errs = []
    if not isinstance(art, dict):
        return ["artifact is not a JSON object"]
    kind = art.get("kind")
    if kind not in ("ladder", "cli", "perf", "overload"):
        errs.append(f"kind: expected ladder|cli|perf|overload, got {kind!r}")
    if art.get("schema") != REQUIRED_SCHEMA:
        errs.append(f"schema: expected {REQUIRED_SCHEMA}, got "
                    f"{art.get('schema')!r} (re-measure with the current "
                    f"runner so the artifact shape matches the validator)")
    # The measured executable is part of the common contract for every
    # kind: ladder/cli/perf all bind the binary that actually ran.
    errs += _check_binary_block(art, "binary")
    # Optional second provenance block (e.g. the CLI workload generator);
    # validated with the same rules when present.
    if "workload_gen" in art:
        errs += _check_binary_block(art, "workload_gen")
    # The runner records the measurement timestamp inside the environment
    # fingerprint (it identifies when the environment was probed).
    if not isinstance(_dig(art, ("env", "time")), str) or \
            not _dig(art, ("env", "time")):
        errs.append("env.time: missing/empty measurement timestamp")

    env = art.get("env")
    if not isinstance(env, dict):
        return errs + ["env: missing"]

    git = env.get("git")
    if not isinstance(git, dict):
        errs.append("env.git: missing")
    else:
        sha = git.get("sha")
        if not isinstance(sha, str) or not SHA_RE.match(sha):
            errs.append(f"env.git.sha: expected 40-hex commit, got {sha!r}")
        dirty = git.get("dirty")
        if dirty not in (True, False, None):
            errs.append(f"env.git.dirty: expected bool|null, got {dirty!r}")
        # A dirty-tree measurement is only admissible with an explicit
        # provenance note saying what was dirty and why.
        if dirty is True and not str(art.get("note", "")).strip():
            errs.append("env.git.dirty is true but artifact carries no "
                        "provenance note")

    build = env.get("build")
    if not isinstance(build, dict):
        errs.append("env.build: missing")
    else:
        # Canonical performance evidence is Release-only (Debug timing data
        # is a benchmark artifact, docs/verification/performance-attribution).
        if build.get("mode") != "release":
            errs.append(f"env.build.mode: expected 'release', got "
                        f"{build.get('mode')!r}")
        for k in ("compiler", "compiler_version"):
            if not build.get(k):
                errs.append(f"env.build.{k}: missing")

    system = env.get("system")
    if not isinstance(system, dict):
        errs.append("env.system: missing")
    else:
        for k in ("kernel", "platform", "cpu", "logical_cpus", "glibc",
                  "python"):
            if _dig(system, (k,), _MISSING) is _MISSING:
                errs.append(f"env.system.{k}: missing")
    if not isinstance(_dig(env, ("environment", "wsl")), str):
        errs.append("env.environment.wsl: missing (WSL2|WSL1|native|unknown)")

    fs = env.get("filesystem")
    if not isinstance(fs, dict) or "input" not in fs or "output" not in fs:
        errs.append("env.filesystem: input/output mount records missing")
    else:
        for side in ("input", "output"):
            rec = fs.get(side)
            if rec is None:
                continue  # null allowed (e.g. suppressed output)
            if not isinstance(rec, dict) or not rec.get("mount_point") or \
                    not rec.get("type"):
                errs.append(f"env.filesystem.{side}: incomplete mount record")

    tools = env.get("tools")
    if not isinstance(tools, dict) or "gnu_grep" not in tools or \
            "ripgrep" not in tools:
        errs.append("env.tools: gnu_grep/ripgrep version records missing")
    return errs


def check_ladder(art: dict) -> list[str]:
    errs = []
    params = art.get("params")
    if not isinstance(params, dict):
        return ["params: missing (bytes/iters/warmup/buffer_size)"]
    for k in ("bytes", "iters"):
        if not isinstance(params.get(k), int) or params.get(k, 0) < 1:
            errs.append(f"params.{k}: expected int >= 1")
    # The runner allows a zero warmup (measurement only after warmup runs
    # is not required); it must still be a non-negative integer.
    if not isinstance(params.get("warmup"), int) or params.get("warmup", -1) < 0:
        errs.append("params.warmup: expected int >= 0")
    rows = art.get("rows")
    if not isinstance(rows, list) or not rows:
        return errs + ["rows: empty"]
    for i, r in enumerate(rows):
        where = f"rows[{i}] ({r.get('stage')}/{r.get('workload')})"
        try:
            ns_min, ns_med, ns_max = (float(r["ns_min"]), float(r["ns_med"]),
                                      float(r["ns_max"]))
            samples = [float(s) for s in r["ns_samples"]]
            iters = int(r["iters"])
            bytes_ = int(r["bytes"])
        except (KeyError, TypeError, ValueError) as e:
            errs.append(f"{where}: malformed numeric field ({e})")
            continue
        if bytes_ < 1:
            errs.append(f"{where}: bytes < 1")
        # A row may legitimately come from a filtered run, but it must not
        # contradict the artifact-level iteration count.
        if isinstance(params.get("iters"), int) and iters != params["iters"]:
            errs.append(f"{where}: row iters {iters} != params.iters "
                        f"{params['iters']}")
        if not (ns_min <= ns_med <= ns_max):
            errs.append(f"{where}: ns_min<=ns_med<=ns_max violated")
        if len(samples) != iters:
            errs.append(f"{where}: {len(samples)} samples != iters {iters}")
            continue
        if abs(min(samples) - ns_min) > 0 or abs(max(samples) - ns_max) > 0:
            errs.append(f"{where}: ns_min/ns_max do not match raw samples")
        srt = sorted(samples)
        n = len(srt)
        med = srt[n // 2] if n % 2 == 1 else (srt[n // 2 - 1] + srt[n // 2]) / 2
        if abs(med - ns_med) > HEX40_TOLERANCE_MEDIAN:
            errs.append(f"{where}: ns_med {ns_med} != recomputed median {med}")
        gbps = float(r.get("gbps_med", 0.0))
        if ns_med > 0 and not math.isclose(gbps, bytes_ / ns_med,
                                           rel_tol=1e-3, abs_tol=1e-3):
            errs.append(f"{where}: gbps_med inconsistent with bytes/ns_med")
    if not isinstance(art.get("derived"), list):
        errs.append("derived: missing core-increment metrics")
    # A full-matrix run (no stage/workload filter) must carry its derived
    # core-increment metrics; a filtered run may legitimately have none.
    if params.get("stages") is None and params.get("workloads") is None:
        d = art.get("derived")
        if not isinstance(d, list) or not d:
            errs.append("derived: full-matrix run has no core-increment "
                        "metrics")
    return errs


def check_cli(art: dict) -> list[str]:
    errs = []
    params = art.get("params")
    if not isinstance(params, dict):
        return ["params: missing (bytes/iters/warmup)"]
    for k in ("bytes", "iters"):
        if not isinstance(params.get(k), int) or params.get(k, 0) < 1:
            errs.append(f"params.{k}: expected int >= 1")
    if not isinstance(params.get("warmup"), int) or params.get("warmup", -1) < 0:
        errs.append("params.warmup: expected int >= 0")
    rows = art.get("rows")
    if not isinstance(rows, list) or not rows:
        return errs + ["rows: empty"]
    saw_outputs_equal = False
    for i, r in enumerate(rows):
        where = f"rows[{i}] ({r.get('tool')}/{r.get('workload')})"
        try:
            s_min, s_med, s_max = (float(r["s_min"]), float(r["s_med"]),
                                   float(r["s_max"]))
            samples = [float(s) for s in r["s_samples"]]
            bytes_ = int(r["bytes"])
        except (KeyError, TypeError, ValueError) as e:
            errs.append(f"{where}: malformed numeric field ({e})")
            samples = None
            s_min = s_med = s_max = bytes_ = 0
        rcs = r.get("exit_codes")
        if not isinstance(rcs, list) or not rcs:
            errs.append(f"{where}: exit_codes missing")
        else:
            bad = sorted(set(rcs) - VALID_DATA_EXIT_CODES)
            if bad:
                errs.append(f"{where}: tool-error exit codes {bad} recorded "
                            f"(row is not valid evidence)")
        if r.get("tool_error"):
            errs.append(f"{where}: tool_error flag set")
        md5 = r.get("output_md5")
        if not isinstance(md5, str) or not MD5_RE.match(md5):
            errs.append(f"{where}: output_md5 missing/malformed")
        ob = r.get("output_bytes")
        if not isinstance(ob, int) or ob < 0:
            errs.append(f"{where}: output_bytes missing/negative")
        if "outputs_equal" in r:
            saw_outputs_equal = True
            # Fail closed: a false outputs_equal means the competitors did
            # not share one output contract (e.g. a binary-file
            # short-circuit), so their timings are not comparable evidence.
            # Only an explicitly marked non_comparable row may carry false,
            # and such rows are excluded from ratio claims by policy.
            if r["outputs_equal"] is not True and r.get("non_comparable") is not True:
                errs.append(f"{where}: outputs_equal is false — timing is "
                            f"not valid comparative evidence (deliberate "
                            f"semantics studies must mark rows non_comparable)")
        # Timing statistics must be present, ordered, and consistent with
        # the raw per-iteration samples (same class of check as the ladder;
        # a hand-typed CLI table must not pass).
        if samples is not None:
            if not (s_min <= s_med <= s_max):
                errs.append(f"{where}: s_min<=s_med<=s_max violated")
            if isinstance(params.get("iters"), int) and \
                    len(samples) != params["iters"]:
                errs.append(f"{where}: {len(samples)} samples != params.iters "
                            f"{params['iters']}")
                continue
            if abs(min(samples) - s_min) > 1e-12 or \
                    abs(max(samples) - s_max) > 1e-12:
                errs.append(f"{where}: s_min/s_max do not match raw samples")
            srt = sorted(samples)
            n = len(srt)
            med = srt[n // 2] if n % 2 == 1 else (srt[n // 2 - 1] + srt[n // 2]) / 2
            if abs(med - s_med) > 1e-9:
                errs.append(f"{where}: s_med {s_med} != recomputed median {med}")
            gbps = float(r.get("gbps_med", 0.0))
            if s_med > 0 and not math.isclose(gbps, bytes_ / 1e9 / s_med,
                                              rel_tol=1e-3, abs_tol=1e-3):
                errs.append(f"{where}: gbps_med inconsistent with bytes/s_med")
    if not saw_outputs_equal:
        errs.append("rows: no outputs_equal differential check recorded")
    # Group check (independent of the runner's outputs_equal flag): every
    # competitor on the same workload must have produced the same output
    # bytes, verified from the raw per-row hashes themselves.
    by_wl: dict = {}
    for r in rows:
        if isinstance(r, dict):
            by_wl.setdefault(r.get("workload"), set()).add(r.get("output_md5"))
    for wl, md5s in by_wl.items():
        if len(md5s) > 1:
            errs.append(f"rows: workload {wl!r} has {len(md5s)} distinct "
                        f"output_md5 values — competitor outputs are not "
                        f"byte-identical")
    return errs


def check_perf(art: dict) -> list[str]:
    errs = []
    counters = art.get("counters")
    if not isinstance(counters, dict) or not counters:
        return ["counters: empty"]
    for k, v in counters.items():
        if not isinstance(v, (int, float)):
            errs.append(f"counters.{k}: non-numeric {v!r}")
    if not isinstance(art.get("cmd"), list) or not art.get("cmd"):
        errs.append("cmd: measured command missing")
    if not isinstance(art.get("raw"), str) or not art.get("raw"):
        errs.append("raw: verbatim perf output missing (modifier state like "
                    "':u' user-space-only counters is only visible there)")
    # The measured command's exit status is part of the evidence: perf still
    # reports partial counters for a child that failed, so the artifact must
    # record the status AND state how to classify it. Both are enforced
    # fail-closed (the run that exits 2 is a tool error, not a measurement).
    rc = art.get("child_exit_code")
    if not isinstance(rc, int) or isinstance(rc, bool):
        errs.append("child_exit_code: missing/non-int (perf must record the "
                    "measured command's exit status)")
    else:
        sem = art.get("exit_semantics")
        allowed = EXIT_SEMANTICS.get(sem) if isinstance(sem, str) else None
        if allowed is None:
            errs.append(f"exit_semantics: expected one of "
                        f"{sorted(EXIT_SEMANTICS)}, got {sem!r}")
        elif rc not in allowed:
            errs.append(f"child_exit_code {rc} is invalid data under "
                        f"exit_semantics {sem!r}")
    if "derived" in art and not isinstance(art.get("params"), dict):
        errs.append("derived: per-request ratios present but the divisor "
                    "(params.requests) is not recorded")
    return errs



def check_overload(art: dict) -> list[str]:
    """#199 sustained-overload artifact: the accounting must prove the bound
    that fired was ADMISSION CAPACITY (every burst attempt refused, every
    refill accepted, high-water == capacity, final in-flight 0, post-drain
    admission probe accepted); the latency percentiles must be recomputable
    from the raw samples (anti-hand-typing, same class as ladder/cli)."""
    errs = []
    params = art.get("params")
    if not isinstance(params, dict):
        return ["params: missing (capacities/rounds/burst/complete_k/rss_every)"]
    caps = params.get("capacities")
    if not isinstance(caps, list) or not caps or             any(not isinstance(c, int) or c < 1 for c in caps):
        errs.append("params.capacities: expected non-empty list of ints >= 1")
        caps = []
    for k in ("rounds", "burst", "complete_k", "rss_every",
              "sustained_rss_every", "reservoir"):
        if not isinstance(params.get(k), int) or params.get(k, 0) < 1:
            errs.append(f"params.{k}: expected int >= 1")
    if not isinstance(params.get("sustained_rounds"), int) or \
            params.get("sustained_rounds", 0) < SUSTAINED_ROUNDS_MIN:
        errs.append(f"params.sustained_rounds: expected int >= "
                    f"{SUSTAINED_ROUNDS_MIN} (a boundedness claim needs a "
                    f"sustained interval)")
    rows = art.get("rows")
    if not isinstance(rows, list) or not rows:
        return errs + ["rows: empty"]
    seen_caps = set()
    for i, r in enumerate(rows):
        where = f"rows[{i}] (capacity {r.get('capacity')})"
        cap = r.get("capacity")
        if not isinstance(cap, int) or cap < 1:
            errs.append(f"{where}: capacity missing/invalid")
            continue
        if cap in seen_caps:
            errs.append(f"{where}: duplicate capacity row")
        seen_caps.add(cap)
        if isinstance(caps, list) and cap not in caps:
            errs.append(f"{where}: capacity not declared in params.capacities")
        rounds, burst, k_complete = (params.get("rounds"), params.get("burst"),
                                     params.get("complete_k"))
        # --- accounting: the resource-bound distinction, fail-closed ---
        a = r.get("accounting")
        if not isinstance(a, dict):
            errs.append(f"{where}: accounting missing")
        else:
            if a.get("high_water_inflight") != cap:
                errs.append(f"{where}: high_water_inflight "
                            f"{a.get('high_water_inflight')} != capacity {cap}")
            if a.get("final_inflight") != 0:
                errs.append(f"{where}: final_inflight != 0")
            if a.get("post_drain_probe_accepted") is not True:
                errs.append(f"{where}: post_drain_probe_accepted is not true "
                            f"(recovery not proven)")
            exp_refusals = rounds * burst + 1 if rounds and burst else None
            if exp_refusals is not None and a.get("refusals") != exp_refusals:
                errs.append(f"{where}: refusals {a.get('refusals')} != expected "
                            f"{exp_refusals} (a burst attempt was accepted — the "
                            f"window was not full)")
            exp_refills = rounds * k_complete if rounds and k_complete else None
            if exp_refills is not None and a.get("refill_accepts") != exp_refills:
                errs.append(f"{where}: refill_accepts {a.get('refill_accepts')} != "
                            f"expected {exp_refills} (capacity not reclaimed)")
            for key in ("drain_ns", "post_drain_probe_ns"):
                v = a.get(key)
                if not isinstance(v, (int, float)) or v < 0:
                    errs.append(f"{where}: accounting.{key} missing/negative")
        # --- static probes from production types ---
        st = r.get("static")
        if not isinstance(st, dict):
            errs.append(f"{where}: static probes missing")
        else:
            for key in ("sizeof_slot_handle", "sizeof_completion_size_t",
                        "sizeof_completion_void", "sizeof_request_handle",
                        "sizeof_read_op"):
                v = st.get(key)
                if not isinstance(v, int) or v < 1:
                    errs.append(f"{where}: static.{key} missing/invalid")
        # --- percentile consistency with raw samples ---
        for ph in ("accept", "refuse"):
            blk = r.get(ph)
            if not isinstance(blk, dict) or not isinstance(blk.get("samples_ns"), list):
                errs.append(f"{where}: {ph}.samples_ns missing")
                continue
            samples = blk["samples_ns"]
            if not samples:
                errs.append(f"{where}: {ph}.samples_ns empty")
                continue
            if blk.get("n") != len(samples):
                errs.append(f"{where}: {ph}.n {blk.get('n')} != sample count "
                            f"{len(samples)}")
            p50, p95, p99 = (blk.get("p50_ns"), blk.get("p95_ns"),
                             blk.get("p99_ns"))
            for name, val in (("p50_ns", p50), ("p95_ns", p95), ("p99_ns", p99)):
                if not isinstance(val, (int, float)) or val < 0:
                    errs.append(f"{where}: {ph}.{name} missing/invalid")
            if all(isinstance(v, (int, float)) for v in (p50, p95, p99)):
                if not (p50 <= p95 <= p99):
                    errs.append(f"{where}: {ph} p50<=p95<=p99 violated")
                srt = sorted(samples)

                def nearest_rank(pct_val):
                    idx = int(pct_val / 100.0 * len(srt))
                    return srt[min(idx, len(srt) - 1)]

                for name, val, want in (("p50_ns", p50, nearest_rank(50)),
                                        ("p95_ns", p95, nearest_rank(95)),
                                        ("p99_ns", p99, nearest_rank(99))):
                    if val != want:
                        errs.append(f"{where}: {ph}.{name} {val} != recomputed "
                                    f"nearest-rank {want}")
        # --- RSS series shape (growth must be visible, not summarized away) ---
        rss = r.get("rss_series_kb")
        if not isinstance(rss, list) or len(rss) < 2:
            errs.append(f"{where}: rss_series_kb must have >= 2 points")
        else:
            prev_round = None
            for pt in rss:
                if not (isinstance(pt, list) and len(pt) == 2 and
                        isinstance(pt[0], int) and isinstance(pt[1], int) and
                        pt[1] >= 0):
                    errs.append(f"{where}: rss point malformed {pt!r}")
                    break
                if prev_round is not None and pt[0] <= prev_round:
                    errs.append(f"{where}: rss rounds not strictly increasing")
                    break
                prev_round = pt[0]
        # --- sustained RSS boundedness phase (separate from latency stats) ---
        # The bench runs a fixed-capacity interval with NO latency recording
        # and fixed-size reservoirs, so no harness allocation can grow. The
        # validator requires the overload to have been sustained (counts) and
        # the RSS to plateau (delta <= bound).
        sus = r.get("sustained")
        if not isinstance(sus, dict):
            errs.append(f"{where}: sustained block missing (RSS boundedness "
                        f"phase required)")
        else:
            sr = params.get("sustained_rounds")
            if isinstance(sr, int):
                exp_refusals = sr * burst if burst else None
                if exp_refusals is not None and sus.get("refusals") != exp_refusals:
                    errs.append(f"{where}: sustained.refusals "
                                f"{sus.get('refusals')} != expected "
                                f"{exp_refusals} (sustained overload not "
                                f"maintained)")
                exp_refills = sr * k_complete if k_complete else None
                if exp_refills is not None and sus.get("refills") != exp_refills:
                    errs.append(f"{where}: sustained.refills {sus.get('refills')} "
                                f"!= expected {exp_refills} (capacity not "
                                f"reclaimed during sustained phase)")
            sus_rss = sus.get("rss_series_kb")
            if not isinstance(sus_rss, list) or len(sus_rss) < 3:
                errs.append(f"{where}: sustained.rss_series_kb must have "
                            f">= 3 points (start/mid/end)")
            else:
                prev_round = None
                for pt in sus_rss:
                    if not (isinstance(pt, list) and len(pt) == 2 and
                            isinstance(pt[0], int) and isinstance(pt[1], int) and
                            pt[1] >= 0):
                        errs.append(f"{where}: sustained rss point malformed "
                                    f"{pt!r}")
                        break
                    if prev_round is not None and pt[0] <= prev_round:
                        errs.append(f"{where}: sustained rss rounds not "
                                    f"strictly increasing")
                        break
                    prev_round = pt[0]
                if prev_round is not None:
                    first = sus_rss[0][1]
                    last = sus_rss[-1][1]
                    recomputed = last - first
                    delta = sus.get("delta_kb")
                    if delta != recomputed:
                        errs.append(f"{where}: sustained.delta_kb {delta} != "
                                    f"recomputed {recomputed} from the series")
                    if recomputed > RSS_PLATEAU_MAX_KB:
                        errs.append(f"{where}: sustained RSS delta "
                                    f"{recomputed} kB exceeds the plateau "
                                    f"bound {RSS_PLATEAU_MAX_KB} kB — RSS is "
                                    f"not bounded at fixed capacity")
    if isinstance(caps, list) and sorted(seen_caps) != sorted(caps):
        errs.append(f"rows: capacities {sorted(seen_caps)} do not cover "
                    f"params.capacities {sorted(caps)}")
    if not isinstance(art.get("derived"), list) or not art.get("derived"):
        errs.append("derived: missing per-capacity aggregate ratios")
    return errs


CHECKS = {"ladder": check_ladder, "cli": check_cli, "perf": check_perf, "overload": check_overload}


def validate_artifact(art: dict) -> list[str]:
    errs = check_common(art)
    kind = art.get("kind") if isinstance(art, dict) else None
    if kind in CHECKS:
        errs += CHECKS[kind](art)
    return errs


def validate_path(path: Path) -> list[str]:
    try:
        art = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        return [f"{path}: unreadable/invalid JSON ({e})"]
    errs = validate_artifact(art)
    return [f"{path}: {e}" for e in errs]


# ---------------------------------------------------------------------------
# self-test: plant one violation per detector and require it to fire
# ---------------------------------------------------------------------------


def _valid_binary() -> dict:
    return {"path": "/build/bin/grep_attribution_bench",
            "sha256": "a" * 64, "size": 123456, "mtime": 1755475200.0}


def _valid_overload() -> dict:
    art = _valid_ladder()
    art["kind"] = "overload"
    samples = [10, 20, 30, 40, 50]
    srt = sorted(samples)
    art["params"] = {"capacities": [16], "rounds": 5, "burst": 2,
                     "complete_k": 1, "rss_every": 2, "sustained_rounds": 600,
                     "sustained_rss_every": 200, "reservoir": 4096}

    def nr(p):
        return srt[min(int(p / 100.0 * len(srt)), len(srt) - 1)]

    art["rows"] = [{
        "capacity": 16,
        "static": {"sizeof_slot_handle": 16, "sizeof_completion_size_t": 64,
                   "sizeof_completion_void": 48, "sizeof_request_handle": 32,
                   "sizeof_read_op": 32},
        "accounting": {"refill_accepts": 5, "refusals": 11,
                       "expected_refusals": 11, "expected_refills": 5,
                       "high_water_inflight": 16, "final_inflight": 0,
                       "drain_ns": 100, "post_drain_probe_ns": 50,
                       "post_drain_probe_accepted": True},
        "sustained": {"refusals": 1200, "expected_refusals": 1200,
                      "refills": 600, "expected_refills": 600,
                      "delta_kb": 24,
                      "rss_series_kb": [[0, 4100], [200, 4112],
                                        [400, 4108], [600, 4124]]},
        "accept": {"n": 5, "p50_ns": nr(50), "p95_ns": nr(95),
                   "p99_ns": nr(99), "samples_ns": samples},
        "refuse": {"n": 5, "p50_ns": nr(50), "p95_ns": nr(95),
                   "p99_ns": nr(99), "samples_ns": samples},
        "initial_fill": {"n": 16, "p50_ns": 30},
        "rss_series_kb": [[0, 3800], [2, 3810], [5, 3820]],
    }]
    art["derived"] = [{"capacity": 16}]
    return art


def _valid_ladder() -> dict:
    samples = [100, 120, 110, 130, 115]
    med = sorted(samples)[2]
    return {
        "schema": 2,
        "kind": "ladder",
        "binary": _valid_binary(),
        "env": {
            "time": "2026-08-18T00:00:00+0000",
            "git": {"sha": "a" * 40, "dirty": False, "branch": "perf/x"},
            "build": {"mode": "release", "compiler": "clang++",
                      "compiler_version": "clang 21"},
            "system": {"kernel": "6.8", "platform": "x86_64", "cpu": "c",
                       "logical_cpus": 8, "glibc": "2.43", "python": "3.12"},
            "environment": {"wsl": "WSL2"},
            "filesystem": {"input": {"path": "/tmp", "mount_point": "/tmp",
                                     "type": "tmpfs"},
                           "output": None},
            "tools": {"gnu_grep": "grep 3.11", "ripgrep": None},
        },
        "params": {"bytes": 1 << 28, "iters": 5, "warmup": 1,
                   "buffer_size": 1 << 20, "stages": None, "workloads": None},
        "rows": [{
            "stage": "L4_sluice", "workload": "w", "bytes": 1 << 28,
            "iters": 5, "ns_min": min(samples), "ns_med": float(med),
            "ns_max": max(samples), "gbps_med": (1 << 28) / med,
            "matches": 1, "ns_samples": samples,
        }],
        "derived": [{"workload": "w"}],
    }


class ValidatorSelfTest(unittest.TestCase):
    def assert_invalid(self, art, needle):
        errs = validate_artifact(art)
        self.assertTrue(any(needle in e for e in errs),
                        f"detector for {needle!r} did not fire; errors: {errs}")

    def test_valid_ladder_passes(self):
        self.assertEqual(validate_artifact(_valid_ladder()), [])

    def test_valid_cli_and_perf_pass(self):
        art = _valid_ladder()
        art["kind"] = "cli"
        art["params"] = {"bytes": 1 << 30, "iters": 5, "warmup": 1}
        samples = [1.0, 1.2, 1.1, 1.3, 1.15]
        med = sorted(samples)[2]
        art["rows"] = [{"tool": "sluice-grep", "workload": "w",
                        "pattern": "p", "bytes": 1 << 30,
                        "s_min": min(samples), "s_med": med,
                        "s_max": max(samples), "s_samples": samples,
                        "gbps_med": (1 << 30) / 1e9 / med,
                        "exit_codes": [0], "tool_error": False,
                        "output_md5": "b" * 32, "output_bytes": 10,
                        "outputs_equal": True}]
        art.pop("derived")
        self.assertEqual(validate_artifact(art), [])
        art["kind"] = "perf"
        art.pop("params")
        art["cmd"] = ["bin"]
        art["counters"] = {"cycles": 1.0}
        art["child_exit_code"] = 0
        art["exit_semantics"] = "grep-family"
        art["raw"] = "1,,cycles:u"
        art.pop("rows")
        self.assertEqual(validate_artifact(art), [])

    def _valid_cli(self):
        art = _valid_ladder()
        art["kind"] = "cli"
        art["params"] = {"bytes": 1 << 30, "iters": 5, "warmup": 1}
        samples = [1.0, 1.2, 1.1, 1.3, 1.15]
        med = sorted(samples)[2]
        art["rows"] = [{"tool": "gnugrep", "workload": "w", "pattern": "p",
                        "bytes": 1 << 30, "s_min": min(samples),
                        "s_med": med, "s_max": max(samples),
                        "s_samples": samples,
                        "gbps_med": (1 << 30) / 1e9 / med,
                        "exit_codes": [0], "tool_error": False,
                        "output_md5": "b" * 32, "output_bytes": 0,
                        "outputs_equal": True}]
        art.pop("derived")
        return art

    def test_detectors_fire(self):
        # dirty tree without provenance note
        art = _valid_ladder()
        art["env"]["git"]["dirty"] = True
        self.assert_invalid(art, "provenance note")
        # non-release build mode
        art = _valid_ladder()
        art["env"]["build"]["mode"] = "debug"
        self.assert_invalid(art, "build.mode")
        # bad sha
        art = _valid_ladder()
        art["env"]["git"]["sha"] = "deadbeef"
        self.assert_invalid(art, "env.git.sha")
        # missing env fields
        art = _valid_ladder()
        del art["env"]["environment"]
        self.assert_invalid(art, "environment.wsl")
        # sample/iters mismatch
        art = _valid_ladder()
        art["rows"][0]["ns_samples"] = [1, 2, 3]
        self.assert_invalid(art, "samples != iters")
        # median inconsistent with samples (the even-iter bias class)
        art = _valid_ladder()
        art["rows"][0]["ns_med"] = 999.0
        self.assert_invalid(art, "recomputed median")
        # min/max inconsistent with samples
        art = _valid_ladder()
        art["rows"][0]["ns_min"] = 1
        self.assert_invalid(art, "ns_min/ns_max do not match")
        # gbps inconsistent
        art = _valid_ladder()
        art["rows"][0]["gbps_med"] = 0.001
        self.assert_invalid(art, "gbps_med inconsistent")
        # tool-error exit code in evidence
        art = self._valid_cli()
        art["rows"][0]["exit_codes"] = [0, 2]
        art["rows"][0]["tool_error"] = True
        self.assert_invalid(art, "tool-error exit codes")
        # CLI timing fields must exist, be ordered, and match samples —
        # a hand-typed table with no samples or broken ordering must fail
        art = self._valid_cli()
        del art["rows"][0]["s_samples"]
        self.assert_invalid(art, "malformed numeric field")
        art = self._valid_cli()
        art["rows"][0]["s_min"] = 9.0
        self.assert_invalid(art, "s_min<=s_med<=s_max violated")
        art = self._valid_cli()
        art["rows"][0]["s_med"] = 1.05  # not the median of the samples
        self.assert_invalid(art, "recomputed median")
        art = self._valid_cli()
        art["rows"][0]["gbps_med"] = 0.001
        self.assert_invalid(art, "gbps_med inconsistent")
        # row iters contradicting the artifact-level params (filtered run
        # posing as full) — ladder variant
        art = _valid_ladder()
        art["rows"][0]["iters"] = 3
        art["rows"][0]["ns_samples"] = [1, 2, 3]
        self.assert_invalid(art, "row iters 3 != params.iters 5")
        # perf artifact with no counters
        art = _valid_ladder()
        art["kind"] = "perf"
        art["cmd"] = ["bin"]
        art["counters"] = {}
        art["raw"] = "x"
        art.pop("rows")
        art.pop("params")
        art.pop("derived")
        self.assert_invalid(art, "counters: empty")
        # perf artifact without the verbatim raw output (modifier state
        # like ':u' is only visible there)
        art = _valid_ladder()
        art["kind"] = "perf"
        art["cmd"] = ["bin"]
        art["counters"] = {"cycles": 1.0}
        art.pop("rows")
        art.pop("params")
        art.pop("derived")
        self.assert_invalid(art, "raw: verbatim perf output missing")
        # derived per-request ratios without the recorded divisor
        art = _valid_ladder()
        art["kind"] = "perf"
        art["cmd"] = ["bin"]
        art["counters"] = {"cycles": 1.0}
        art["raw"] = "1,,cycles:u"
        art["child_exit_code"] = 0
        art["exit_semantics"] = "grep-family"
        art["derived"] = {"cycles_per_request": 0.5}
        art.pop("rows")
        art.pop("params")
        self.assert_invalid(art, "divisor")
        # stale/missing schema version
        art = _valid_ladder()
        del art["schema"]
        self.assert_invalid(art, "schema: expected 2")
        # missing executable provenance (the git-SHA-vs-stale-binary hole)
        art = _valid_ladder()
        del art["binary"]
        self.assert_invalid(art, "binary: missing executable provenance")
        # malformed binary digest
        art = _valid_ladder()
        art["binary"]["sha256"] = "deadbeef"
        self.assert_invalid(art, "binary.sha256")
        # outputs_equal false is not comparable evidence (binary-file
        # short-circuit class), unless explicitly marked non_comparable
        art = self._valid_cli()
        art["rows"][0]["outputs_equal"] = False
        self.assert_invalid(art, "outputs_equal is false")
        art["rows"][0]["non_comparable"] = True
        self.assertEqual(validate_artifact(art), [])
        # group check: diverging output hashes within one workload
        art = self._valid_cli()
        row2 = dict(art["rows"][0])
        row2["tool"] = "gnugrep"
        row2["output_md5"] = "c" * 32
        art["rows"].append(row2)
        self.assert_invalid(art, "distinct output_md5")
        # perf artifact without the measured command's exit status
        art = _valid_ladder()
        art["kind"] = "perf"
        art["cmd"] = ["bin"]
        art["counters"] = {"cycles": 1.0}
        art["raw"] = "x"
        art.pop("rows")
        art.pop("params")
        art.pop("derived")
        self.assert_invalid(art, "child_exit_code: missing/non-int")
        # exit status invalid under the declared semantics (2 = tool error)
        art["child_exit_code"] = 2
        art["exit_semantics"] = "grep-family"
        self.assert_invalid(art, "invalid data under")
        # unknown/missing semantics classifier
        art["exit_semantics"] = "whatever"
        self.assert_invalid(art, "exit_semantics")
        # warmup must allow 0 (runner contract) but reject negatives
        art = _valid_ladder()
        art["params"]["warmup"] = 0
        self.assertEqual(validate_artifact(art), [])
        art["params"]["warmup"] = -1
        self.assert_invalid(art, "params.warmup")
        # full-matrix ladder without derived core-increment metrics
        art = _valid_ladder()
        art["derived"] = []
        self.assert_invalid(art, "full-matrix")

    def test_valid_overload_passes(self):
        self.assertEqual(validate_artifact(_valid_overload()), [])

    def test_overload_detectors_fire(self):
        # high-water below capacity (window never filled)
        art = _valid_overload()
        art["rows"][0]["accounting"]["high_water_inflight"] = 15
        self.assert_invalid(art, "high_water_inflight")
        # a burst attempt was accepted -> refusals mismatch
        art = _valid_overload()
        art["rows"][0]["accounting"]["refusals"] = 10
        self.assert_invalid(art, "refusals 10 != expected 11")
        # capacity not reclaimed after completions
        art = _valid_overload()
        art["rows"][0]["accounting"]["refill_accepts"] = 4
        self.assert_invalid(art, "refill_accepts 4 != expected 5")
        # recovery not proven
        art = _valid_overload()
        art["rows"][0]["accounting"]["post_drain_probe_accepted"] = False
        self.assert_invalid(art, "post_drain_probe_accepted")
        # hand-typed percentile (recomputed nearest-rank mismatch)
        art = _valid_overload()
        art["rows"][0]["accept"]["p99_ns"] = 999
        self.assert_invalid(art, "recomputed nearest-rank")
        # percentile ordering violated
        art = _valid_overload()
        art["rows"][0]["refuse"]["p95_ns"] = 1
        self.assert_invalid(art, "p50<=p95<=p99 violated")
        # n contradicting the raw sample count
        art = _valid_overload()
        art["rows"][0]["accept"]["n"] = 4
        self.assert_invalid(art, "!= sample count")
        # static probe dropped
        art = _valid_overload()
        del art["rows"][0]["static"]["sizeof_slot_handle"]
        self.assert_invalid(art, "static.sizeof_slot_handle")
        # RSS summarized to a single point (a growth trend must be visible)
        art = _valid_overload()
        art["rows"][0]["rss_series_kb"] = [[0, 3800]]
        self.assert_invalid(art, ">= 2 points")
        # RSS rounds not increasing
        art = _valid_overload()
        art["rows"][0]["rss_series_kb"] = [[0, 3800], [0, 3810], [5, 3820]]
        self.assert_invalid(art, "not strictly increasing")
        # sustained phase missing entirely (boundedness unproven)
        art = _valid_overload()
        del art["rows"][0]["sustained"]
        self.assert_invalid(art, "sustained block missing")
        # sustained interval too short for a boundedness claim
        art = _valid_overload()
        art["params"]["sustained_rounds"] = 10
        self.assert_invalid(art, "sustained_rounds")
        # sustained overload not maintained (counts wrong)
        art = _valid_overload()
        art["rows"][0]["sustained"]["refusals"] = 0
        self.assert_invalid(art, "sustained.refusals")
        # sustained series collapsed to one point
        art = _valid_overload()
        art["rows"][0]["sustained"]["rss_series_kb"] = [[0, 4100]]
        self.assert_invalid(art, ">= 3 points")
        # sustained delta inconsistent with the recorded series
        art = _valid_overload()
        art["rows"][0]["sustained"]["delta_kb"] = 1
        self.assert_invalid(art, "delta_kb 1 != recomputed")
        # RSS NOT plateauing during the sustained phase (growth trend)
        art = _valid_overload()
        art["rows"][0]["sustained"]["rss_series_kb"] = [[0, 4100], [200, 4300],
                                                        [400, 4550], [600, 4800]]
        art["rows"][0]["sustained"]["delta_kb"] = 700
        self.assert_invalid(art, "exceeds the plateau bound")
        # capacity row not declared in params
        art = _valid_overload()
        art["rows"][0]["capacity"] = 32
        self.assert_invalid(art, "not declared in params.capacities")
        # missing derived ratios
        art = _valid_overload()
        art["derived"] = []
        self.assert_invalid(art, "derived: missing")
        # non-zero final in-flight
        art = _valid_overload()
        art["rows"][0]["accounting"]["final_inflight"] = 3
        self.assert_invalid(art, "final_inflight")

    def test_validate_path_reports_filename(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.json"
            p.write_text("{not json")
            errs = validate_path(p)
            self.assertTrue(any("invalid JSON" in e for e in errs))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=str(DEFAULT_EVIDENCE_DIR),
                    help=f"evidence directory (default {DEFAULT_EVIDENCE_DIR})")
    ap.add_argument("--file", action="append", default=[],
                    help="validate a specific artifact file (repeatable)")
    ap.add_argument("--self-test", action="store_true",
                    help="run the negative self-test suite")
    args = ap.parse_args()

    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ValidatorSelfTest)
        result = unittest.TextTestRunner(verbosity=1).run(suite)
        return 0 if result.wasSuccessful() else 1

    files = [Path(f) for f in args.file]
    if not files:
        d = Path(args.dir)
        if not d.is_dir():
            print(f"evidence dir {d} does not exist; nothing to validate")
            return 0
        files = sorted(d.glob("*.json"))
    if not files:
        print("no evidence artifacts found; nothing to validate")
        return 0

    failures = 0
    for f in files:
        errs = validate_path(f)
        if errs:
            failures += 1
            for e in errs:
                print(e)
        else:
            print(f"ok: {f}")
    if failures:
        print(f"PERF EVIDENCE VALIDATION FAILED: {failures}/{len(files)} "
              f"artifact(s) invalid")
        return 1
    print(f"perf evidence validation passed: {len(files)} artifact(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
