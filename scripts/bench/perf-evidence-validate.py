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
import random
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
    if kind not in ("ladder", "cli", "perf", "overload", "e1tax",
                    "tax0capacity", "tax0u0router", "tax0u0witness"):
        errs.append(f"kind: expected ladder|cli|perf|overload|e1tax|"
                    f"tax0capacity|tax0u0router|tax0u0witness, got "
                    f"{kind!r}")
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


# e1tax (#221 G0 / E1): the abstraction-tax matrix artifact. Fail-closed
# same-work accounting (completed == expected ops/bytes, zero errors, read
# word sums verified), recomputed medians (anti-hand-typing), and derived
# tax rows cross-checked against the cell medians they claim to derive
# from. Ladder keys mirror scripts/bench/perf-attribution.py E1_LADDERS.
E1_LADDERS = {"L0_raw", "L1_pool", "L2_sluice"}
E1_OPS = {"read", "write"}
E1_MEDIAN_TOLERANCE_NS = 1.0


def check_e1tax(art: dict) -> list[str]:
    errs = []
    params = art.get("params")
    if not isinstance(params, dict):
        return ["params: missing (matrix/ops/ladders/sizes/depths/workers/"
                "total_bytes/reps/warmup)"]
    if params.get("matrix") not in ("smoke", "full", "custom"):
        errs.append(f"params.matrix: expected smoke|full|custom, got "
                    f"{params.get('matrix')!r}")
    for key, allowed in (("ops", E1_OPS), ("ladders", E1_LADDERS)):
        vals = params.get(key)
        if not isinstance(vals, list) or not vals or \
                any(v not in allowed for v in vals):
            errs.append(f"params.{key}: expected non-empty subset of "
                        f"{sorted(allowed)}, got {vals!r}")
    if params.get("matrix") == "custom":
        for key in ("sizes", "depths", "workers"):
            if not isinstance(params.get(key), list) or not params.get(key):
                errs.append(f"params.{key}: custom matrix requires the "
                            f"explicit sweep axes")
    for k in ("total_bytes", "reps"):
        if not isinstance(params.get(k), int) or params.get(k, 0) < 1:
            errs.append(f"params.{k}: expected int >= 1")
    if not isinstance(params.get("warmup"), int) or params.get("warmup", -1) < 0:
        errs.append("params.warmup: expected int >= 0")

    cells = art.get("cells")
    if not isinstance(cells, list) or not cells:
        return errs + ["cells: empty"]
    seen_keys = set()
    by_group: dict = {}
    for i, c in enumerate(cells):
        where = (f"cells[{i}] ({c.get('op')}/{c.get('request_size')}/"
                 f"d{c.get('depth')}/w{c.get('workers')}/{c.get('ladder')})")
        if c.get("op") not in E1_OPS or c.get("ladder") not in E1_LADDERS:
            errs.append(f"{where}: unknown op/ladder")
            continue
        key = (c.get("op"), c.get("request_size"), c.get("depth"),
               c.get("workers"), c.get("ladder"))
        if key in seen_keys:
            errs.append(f"{where}: duplicate cell key")
        seen_keys.add(key)
        rs = c.get("request_size")
        tb = c.get("bytes")
        if not isinstance(rs, int) or rs < 4096 or rs % 4096 != 0:
            errs.append(f"{where}: request_size must be a multiple of 4096")
            continue
        if not isinstance(tb, int) or tb < rs or tb % rs != 0:
            errs.append(f"{where}: bytes must be a positive multiple of "
                        f"request_size")
            continue
        expected_ops = tb // rs
        for k, want in (("ops", expected_ops),
                        ("expected_ops", expected_ops),
                        ("completed_ops", expected_ops),
                        ("bytes", tb), ("expected_bytes", tb),
                        ("completed_bytes", tb)):
            if c.get(k) != want:
                errs.append(f"{where}: {k} is {c.get(k)!r}, must equal "
                            f"{want} (same-work guarantee)")
        if c.get("errors") != 0:
            errs.append(f"{where}: errors {c.get('errors')} recorded (row "
                        f"is not valid evidence)")
        if c.get("op") == "read" and c.get("word_sum_ok") is not True:
            errs.append(f"{where}: word_sum_ok missing/false — read cells "
                        f"must verify the deterministic word sum")
        samples = c.get("wall_ns_samples")
        if not isinstance(samples, list) or not samples:
            errs.append(f"{where}: wall_ns_samples missing/empty (zero "
                        f"repetitions are not evidence)")
            continue
        if any(not isinstance(s, (int, float)) or s <= 0 for s in samples):
            errs.append(f"{where}: wall_ns_samples contains a non-positive "
                        f"or non-numeric value (unknown unit / corruption)")
            continue
        reps = params.get("reps")
        if isinstance(reps, int) and len(samples) != reps:
            errs.append(f"{where}: {len(samples)} samples != params.reps "
                        f"{reps}")
        s_min, s_med, s_max = (c.get("wall_ns_min"), c.get("wall_ns_med"),
                               c.get("wall_ns_max"))
        for k in ("wall_ns_min", "wall_ns_med", "wall_ns_max"):
            if not isinstance(c.get(k), (int, float)):
                errs.append(f"{where}: {k} missing")
        if all(isinstance(v, (int, float)) for v in (s_min, s_med, s_max)):
            if not (s_min <= s_med <= s_max):
                errs.append(f"{where}: wall_ns_min<=med<=max violated")
            if abs(min(samples) - s_min) > 0 or abs(max(samples) - s_max) > 0:
                errs.append(f"{where}: wall_ns_min/max do not match samples")
            srt = sorted(samples)
            n = len(srt)
            med = srt[n // 2] if n % 2 == 1 else (srt[n // 2 - 1] +
                                                  srt[n // 2]) / 2
            if abs(med - s_med) > E1_MEDIAN_TOLERANCE_NS:
                errs.append(f"{where}: wall_ns_med {s_med} != recomputed "
                            f"median {med}")
        if not isinstance(c.get("lifecycle_setup_ns"), int) or \
                not isinstance(c.get("lifecycle_teardown_ns"), int):
            errs.append(f"{where}: lifecycle setup/teardown timings missing "
                        f"(steady-state scope must be explicit, not assumed)")
        g = (c.get("op"), c.get("request_size"), c.get("depth"),
             c.get("workers"))
        by_group.setdefault(g, {})[c.get("ladder")] = c

    # diagnostics: unavailable tooling must carry a reason, never fake zeros
    diag = art.get("diagnostics")
    if not isinstance(diag, dict):
        errs.append("diagnostics: missing perf/bpftrace availability block")
    else:
        for tool in ("perf", "bpftrace"):
            blk = diag.get(tool)
            if not isinstance(blk, dict):
                errs.append(f"diagnostics.{tool}: missing availability block")
            elif blk.get("available") is not True and \
                    not str(blk.get("reason", "")).strip():
                errs.append(f"diagnostics.{tool}: available=false without a "
                            f"reason (missing counters must be recorded as "
                            f"unavailable, never as zeros)")

    # derived: recomputed from the cell medians (anti-hand-typing); a run
    # that measured all three ladders must carry its tax rows.
    derived = art.get("derived")
    if not isinstance(derived, list):
        errs.append("derived: missing tax metrics")
        derived = []
    recomputed: dict = {}
    for g, per in by_group.items():
        if not all(l in per for l in E1_LADDERS):
            continue
        recomputed[g] = (per["L0_raw"]["wall_ns_med"],
                         per["L1_pool"]["wall_ns_med"],
                         per["L2_sluice"]["wall_ns_med"])
    ladders = params.get("ladders") if isinstance(params.get("ladders"),
                                                  list) else []
    if set(ladders) == E1_LADDERS and not recomputed:
        errs.append("derived: full-ladder run produced no comparable "
                    "L0/L1/L2 groups")
    seen_derived = set()
    for d in derived:
        g = (d.get("op"), d.get("request_size"), d.get("depth"),
             d.get("workers"))
        where = f"derived ({'/'.join(str(x) for x in g)})"
        if g in seen_derived:
            errs.append(f"{where}: duplicate derived row")
        seen_derived.add(g)
        if g not in recomputed:
            errs.append(f"{where}: no matching L0/L1/L2 cells")
            continue
        l0, l1, l2 = recomputed[g]
        for k, want in (("l0_ns_med", l0), ("l1_ns_med", l1),
                        ("l2_ns_med", l2),
                        ("threadpool_direct_tax_ns", l1 - l0),
                        ("sluice_incremental_tax_ns", l2 - l1)):
            got = d.get(k)
            if not isinstance(got, (int, float)) or abs(got - want) > 1.0:
                errs.append(f"{where}: {k} is {got!r}, cells say {want}")
        ops_count = by_group[g]["L0_raw"].get("ops")
        if isinstance(ops_count, int) and ops_count > 0:
            got = d.get("l2_l1_per_request_ns")
            want = (l2 - l1) / ops_count
            if not isinstance(got, (int, float)) or abs(got - want) > 1e-9:
                errs.append(f"{where}: l2_l1_per_request_ns is {got!r}, "
                            f"expected {want}")
    if set(ladders) == E1_LADDERS and seen_derived != set(recomputed):
        for g in set(recomputed) - seen_derived:
            errs.append(f"derived: missing tax row for "
                        f"{'/'.join(str(x) for x in g)}")
    return errs


def _median_of(samples: list) -> float | None:
    srt = sorted(samples)
    n = len(srt)
    if not n:
        return None
    return srt[n // 2] if n % 2 == 1 else (srt[n // 2 - 1] + srt[n // 2]) / 2


def _tax0_ols(xs: list, ys: list) -> dict | None:
    """Least-squares y = a + b*x with R², mirroring the runner's descriptive
    slope fit exactly (same summation semantics) so the validator can check
    the stored slope bit-for-bit within double rounding."""
    n = len(xs)
    if n < 2:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    a = my - b * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    r2 = (1.0 - ss_res / ss_tot) if ss_tot > 0 else None
    return {"a": a, "b": b, "r2": r2}


def _tax0_close(got, want: float) -> bool:
    """Deterministic-double recomputation tolerance (not exact string
    equality: JSON round-trips the full repr, so 1e-9 relative only absorbs
    summation-order noise)."""
    return isinstance(got, (int, float)) and not isinstance(got, bool) and \
        abs(got - want) <= 1e-9 * max(1.0, abs(want))


def _tax0_samples_from_rows(rows: list, capacities: list, key: str) -> dict:
    """Per-capacity per-op samples recomputed from the preserved raw rows
    (row counter / ops) — independent of the derived block, so a tampered
    derived.samples list fails even when internally self-consistent."""
    out: dict = {}
    for c in capacities:
        out[c] = [r[key] / r["ops"] for r in rows
                  if r.get("request_capacity") == c
                  and isinstance(r.get(key), (int, float))
                  and isinstance(r.get("ops"), (int, float))
                  and r.get("ops")]
    return out


# derived per-op metric name -> raw row counter it must be recomputed from
_TAX0_ROW_KEY = {"instructions_per_op": "instructions_user",
                 "cycles_per_op": "cycles_user",
                 "wall_ns_per_op": "wall_ns"}


def check_tax0capacity(art: dict) -> list[str]:
    """Kind `tax0capacity` (#250 TAX-0B/EXP-0): one experimental variable
    (request capacity C) at a fixed workload. Fail-closed on: same-work
    drift across rows, missing user-mode instruction/cycle counters,
    unpinned placement, hand-typed derived statistics, and execution order
    that does not match the predeclared randomized sequence — where "match"
    means the exact deterministic output of the predeclared seed under the
    runner's generator contract, and "hand-typed" covers the headline OLS
    slope (a/b/R²) and baseline deltas, which are recomputed from the raw
    row counters, not trusted from the derived block."""
    errs: list[str] = []
    params = art.get("params")
    if not isinstance(params, dict):
        return ["params: missing (experiment/backend/capacities/depth/"
                "reps/seed/...)"]
    if params.get("experiment") != "TAX-0B-EXP0":
        errs.append(f"params.experiment: expected 'TAX-0B-EXP0', got "
                    f"{params.get('experiment')!r}")
    backend = params.get("backend")
    if backend not in ("threadpool", "uring"):
        errs.append(f"params.backend: expected threadpool|uring, got "
                    f"{backend!r}")
    caps = params.get("capacities")
    if not isinstance(caps, list) or not caps or len(set(caps)) != len(caps) \
            or any(not isinstance(c, int) or c < 1 for c in caps):
        return errs + [f"params.capacities: expected a non-empty unique "
                       f"int list, got {caps!r}"]
    depth = params.get("depth")
    if not isinstance(depth, int) or depth < 1:
        return errs + ["params.depth: expected int >= 1"]
    for c in caps:
        if c < depth:
            errs.append(f"params.capacities: {c} < depth {depth} (the "
                        f"pipeline would exceed the arena)")
    reps = params.get("reps")
    if not isinstance(reps, int) or reps < 1:
        errs.append(f"params.reps: expected int >= 1, got {reps!r}")
    rs = params.get("request_size")
    tb = params.get("total_bytes")
    if not isinstance(rs, int) or rs < 4096 or rs % 4096 != 0:
        errs.append(f"params.request_size: must be a multiple of 4096")
    if not isinstance(tb, int) or tb < rs or (rs and tb % rs != 0):
        errs.append(f"params.total_bytes: must be a positive multiple of "
                    f"request_size")
    if not isinstance(params.get("seed"), int):
        errs.append("params.seed: missing (predeclared order seed)")
    # Official capacity evidence is defined by the user-mode counters; a
    # --no-perf smoke artifact must never be committed as evidence.
    if not isinstance(params.get("perf_events"), list) or \
            not params.get("perf_events"):
        errs.append("params.perf_events: missing — committed artifacts "
                    "must carry instructions:u/cycles:u (smoke-only "
                    "--no-perf runs are not evidence)")

    # Placement discipline: official rows must record the taskset CPU set.
    env_extra = art.get("environment_extra")
    if not isinstance(env_extra, dict):
        errs.append("environment_extra: missing (governor/SMT/lscpu/"
                    "taskset provenance)")
    elif not isinstance(env_extra.get("taskset_cpus"), str) or \
            not env_extra.get("taskset_cpus").strip():
        errs.append("environment_extra.taskset_cpus: missing — official "
                    "EXP-0 evidence must be pinned via taskset")
    if not isinstance(art.get("environment_id"), str) or \
            not art.get("environment_id"):
        errs.append("environment_id: missing")

    # Execution order: predeclared seed -> rounds -> flat sequence; the
    # rows must appear in exactly that order.
    order = art.get("execution_order")
    if not isinstance(order, dict):
        return errs + ["execution_order: missing (randomized blocked "
                       "order is part of the frozen design)"]
    if order.get("seed") != params.get("seed"):
        errs.append("execution_order.seed != params.seed")
    rounds = order.get("rounds")
    if not isinstance(rounds, list) or len(rounds) != reps:
        errs.append(f"execution_order.rounds: expected {reps} rounds")
        rounds = []
    for i, rnd in enumerate(rounds):
        if sorted(rnd if isinstance(rnd, list) else []) != sorted(caps):
            errs.append(f"execution_order.rounds[{i}]: not a permutation "
                        f"of capacities")
    flat = order.get("flat")
    want_flat = [c for rnd in rounds for c in (rnd or [])]
    if flat != want_flat:
        errs.append("execution_order.flat does not match rounds")
    # Structural self-consistency is not enough: the stored order must BE
    # the deterministic output of the predeclared seed under the runner's
    # generator contract. A hand-shuffled but internally consistent order
    # (rounds a permutation, flat == rounds, rows matching flat) still
    # fails here.
    if isinstance(params.get("seed"), int) and isinstance(reps, int):
        rng = random.Random(params["seed"])
        seed_rounds = [rng.sample(caps, k=len(caps)) for _ in range(reps)]
        if order.get("rounds") != seed_rounds:
            errs.append("execution_order.rounds: not the deterministic "
                        "output of the predeclared seed (generator "
                        "contract: random.Random(seed).sample(capacities, "
                        "k=len(capacities)) per round)")
        seed_flat = [c for rnd in seed_rounds for c in rnd]
        if order.get("flat") != seed_flat:
            errs.append("execution_order.flat: not the predeclared "
                        "seed-derived sequence")

    rows = art.get("rows")
    if not isinstance(rows, list) or not rows:
        return errs + ["rows: empty"]
    expected_ops = tb // rs if isinstance(tb, int) and isinstance(rs, int) \
        and rs else None
    if len(rows) != reps * len(caps):
        errs.append(f"rows: {len(rows)} rows != reps {reps} x capacities "
                    f"{len(caps)}")
    per_cap: dict = {c: 0 for c in caps}
    for i, r in enumerate(rows):
        where = f"rows[{i}] (C={r.get('request_capacity')})"
        if r.get("experiment") != "TAX-0B-EXP0":
            errs.append(f"{where}: experiment is {r.get('experiment')!r}")
        if r.get("execution_order_index") != i:
            errs.append(f"{where}: execution_order_index "
                        f"{r.get('execution_order_index')!r} != {i}")
        if i < len(want_flat) and r.get("request_capacity") != want_flat[i]:
            errs.append(f"{where}: capacity does not match the predeclared "
                        f"execution order position")
        cap = r.get("request_capacity")
        if cap in per_cap:
            per_cap[cap] += 1
        if r.get("backend") != backend or r.get("op") != params.get("op"):
            errs.append(f"{where}: backend/op drift vs params")
        if r.get("active_depth") != depth or \
                r.get("request_size") != rs or r.get("total_bytes") != tb:
            errs.append(f"{where}: workload geometry drift vs params "
                        f"(same-work violation)")
        if expected_ops is not None and r.get("ops") != expected_ops:
            errs.append(f"{where}: ops {r.get('ops')!r} != {expected_ops} "
                        f"(same-work guarantee)")
        if r.get("semantic_validation") is not True:
            errs.append(f"{where}: semantic_validation not true")
        if r.get("child_exit_code") not in (0, None):
            errs.append(f"{where}: child_exit_code "
                        f"{r.get('child_exit_code')} (invalid evidence)")
        for k in ("wall_ns", "user_ns", "sys_ns"):
            v = r.get(k)
            if not isinstance(v, (int, float)) or v <= 0:
                errs.append(f"{where}: {k} missing/non-positive")
        for k in ("instructions_user", "cycles_user"):
            v = r.get(k)
            if not isinstance(v, (int, float)) or v <= 0:
                errs.append(f"{where}: {k} missing/non-positive — primary "
                            f"metric absent, not zero")
        if backend == "uring":
            if r.get("real_uring") is not True:
                errs.append(f"{where}: uring row without real_uring=true "
                            f"(stub evidence is not capacity evidence)")
            if r.get("uring_queue_depth") != params.get("uring_queue_depth"):
                errs.append(f"{where}: uring_queue_depth drift vs params")
            if r.get("worker_count") is not None:
                errs.append(f"{where}: worker_count set on an uring row")
        else:
            if not isinstance(r.get("worker_count"), int) or \
                    r.get("worker_count", 0) < 1:
                errs.append(f"{where}: worker_count missing on a "
                            f"threadpool row")
            if r.get("real_uring") is not False:
                errs.append(f"{where}: real_uring must be false on a "
                            f"threadpool row")
    for c, n in per_cap.items():
        if n != reps:
            errs.append(f"rows: capacity {c} has {n} rows != reps {reps}")

    # Same-work mechanical proof must be present and valid.
    sw = art.get("same_work")
    if not isinstance(sw, dict):
        errs.append("same_work: missing mechanical same-work proof block")
    elif sw.get("valid") is not True:
        errs.append("same_work.valid is not true — EXP-0 invalid, numbers "
                    "must not be interpreted")

    # derived: medians recomputable from preserved raw samples
    # (anti-hand-typing), baseline = min capacity, deltas carry BOTH
    # absolute and percentage, slope present for the primary metric.
    derived = art.get("derived")
    if not isinstance(derived, dict):
        return errs + ["derived: missing per-cell statistics"]
    if derived.get("baseline_capacity") != min(caps):
        errs.append("derived.baseline_capacity != min(capacities)")
    metrics = derived.get("per_op_metrics")
    if not isinstance(metrics, dict):
        return errs + ["derived.per_op_metrics: missing"]
    for c in caps:
        cell = metrics.get(str(c))
        if not isinstance(cell, dict):
            errs.append(f"derived.per_op_metrics[{c}]: missing")
            continue
        for name in ("instructions_per_op", "cycles_per_op", "wall_ns_per_op"):
            m = cell.get(name)
            if not isinstance(m, dict):
                errs.append(f"derived.per_op_metrics[{c}].{name}: missing")
                continue
            samples = m.get("samples")
            if not isinstance(samples, list) or len(samples) != reps:
                errs.append(f"derived.per_op_metrics[{c}].{name}.samples: "
                            f"expected {reps} raw samples")
                continue
            if any(not isinstance(s, (int, float)) or s <= 0
                   for s in samples):
                errs.append(f"derived.per_op_metrics[{c}].{name}.samples: "
                            f"non-positive/non-numeric sample")
                continue
            # FC-EXP0-8 seal: the stored samples are not an independent
            # authority — they must be exactly the per-op series
            # recomputed from the raw rows (execution order preserved),
            # so tampering samples+median together while leaving the rows
            # and the OLS/delta blocks untouched still fails here.
            raw = _tax0_samples_from_rows(rows, [c], _TAX0_ROW_KEY[name])[c]
            if len(raw) != reps or len(raw) != len(samples) or \
                    any(not _tax0_close(s, w) for s, w in zip(samples, raw)):
                errs.append(f"derived.per_op_metrics[{c}].{name}.samples: "
                            f"stored list is not the per-op series "
                            f"recomputed from the raw rows")
                raw = samples  # still median-check whatever is stored
            srt = sorted(raw)
            n = len(srt)
            want = srt[n // 2] if n % 2 == 1 else \
                (srt[n // 2 - 1] + srt[n // 2]) / 2
            got = m.get("median")
            if not isinstance(got, (int, float)) or \
                    abs(got - want) > 1e-9 * max(1.0, abs(want)):
                errs.append(f"derived.per_op_metrics[{c}].{name}.median "
                            f"{got!r} != recomputed {want}")
    slopes = derived.get("capacity_slope_ols")
    if not isinstance(slopes, dict) or \
            "instructions_per_op" not in slopes:
        errs.append("derived.capacity_slope_ols: missing the primary "
                    "instructions-per-op slope")
    dvb = derived.get("delta_vs_baseline")
    if not isinstance(dvb, dict) or "instructions_per_op" not in dvb:
        errs.append("derived.delta_vs_baseline: missing the primary "
                    "instructions-per-op deltas")
    else:
        for c in caps:
            d = dvb.get("instructions_per_op", {}).get(str(c))
            if not isinstance(d, dict) or "absolute" not in d or \
                    "percent" not in d:
                errs.append(f"derived.delta_vs_baseline."
                            f"instructions_per_op[{c}]: needs BOTH "
                            f"absolute and percent")

    # Independent recomputation from the preserved raw rows: per-op medians
    # -> OLS a/b/R² -> baseline deltas must all match the stored derived
    # block (anti-tampering: the headline slope and deltas are validated
    # against the raw counters, not against derived.samples). The primary
    # metric instructions_per_op is required; cycles_per_op is enforced
    # alongside it because its counters are required on every row above.
    if isinstance(reps, int) and isinstance(slopes, dict) \
            and isinstance(dvb, dict):
        base = min(caps)
        med_rows: dict = {}
        recompute_ok = True
        for name, key in (("instructions_per_op", "instructions_user"),
                          ("cycles_per_op", "cycles_user")):
            per_cap = _tax0_samples_from_rows(rows, caps, key)
            med_rows[name] = {}
            for c in caps:
                if len(per_cap[c]) != reps:
                    errs.append(f"derived.{name}: capacity {c} has "
                                f"{len(per_cap[c])} usable rows != reps "
                                f"{reps} — headline stats not recomputable")
                    recompute_ok = False
                else:
                    med_rows[name][c] = _median_of(per_cap[c])
        if recompute_ok:
            for name in ("instructions_per_op", "cycles_per_op"):
                want = _tax0_ols([float(c) for c in caps],
                                 [med_rows[name][c] for c in caps])
                got = slopes.get(name)
                if not isinstance(got, dict) or want is None:
                    errs.append(f"derived.capacity_slope_ols.{name}: "
                                f"missing/not recomputable from raw rows")
                    continue
                for k in ("a", "b"):
                    if not _tax0_close(got.get(k), want[k]):
                        errs.append(f"derived.capacity_slope_ols.{name}."
                                    f"{k}: stored {got.get(k)!r} != "
                                    f"recomputed from raw rows {want[k]!r}")
                if isinstance(want["r2"], (int, float)):
                    if not _tax0_close(got.get("r2"), want["r2"]):
                        errs.append(f"derived.capacity_slope_ols.{name}."
                                    f"r2: stored {got.get('r2')!r} != "
                                    f"recomputed {want['r2']!r}")
                elif got.get("r2") is not None:
                    errs.append(f"derived.capacity_slope_ols.{name}.r2: "
                                f"stored {got.get('r2')!r} != recomputed "
                                f"None")
                base_med = med_rows[name][base]
                for c in caps:
                    d = dvb.get(name, {}).get(str(c))
                    want_abs = med_rows[name][c] - base_med
                    want_pct = 100.0 * want_abs / base_med if base_med \
                        else None
                    if not _tax0_close((d or {}).get("absolute"), want_abs):
                        errs.append(f"derived.delta_vs_baseline.{name}"
                                    f"[{c}].absolute: stored "
                                    f"{(d or {}).get('absolute')!r} != "
                                    f"recomputed {want_abs!r}")
                    if not _tax0_close((d or {}).get("percent"), want_pct):
                        errs.append(f"derived.delta_vs_baseline.{name}"
                                    f"[{c}].percent: stored "
                                    f"{(d or {}).get('percent')!r} != "
                                    f"recomputed {want_pct!r}")
    return errs

# TAX-0 EXP-U0 preregistered constants (must mirror the runner's frozen
# values; the validator fails an artifact whose envelope/thresholds were
# edited after the fact).
TAX0U0_MODES = ("production_forward", "reverse_ablation")
TAX0U0_BASELINE_ENVELOPE = {"b_min": 5.0, "b_max": 7.0, "r2_min": 0.98}
TAX0U0_DECISION = {"strong_reduction": 0.80, "partial_reduction": 0.50,
                   "not_supported_reduction": 0.20,
                   "reverse_abs_b_max": 1.5}


def check_tax0u0(art: dict) -> list[str]:
    """Kinds `tax0u0router` / `tax0u0witness` (#250 TAX-0 EXP-U0): the
    Uring router-scan causal-ablation experiment. Fail-closed on: same-work
    drift across rows OR across scan-mode arms, missing user-mode counters
    (official kind only), unpinned placement (official only), execution
    order that does not match the predeclared seed-derived (scan_mode, C)
    sequence, scan-witness inconsistencies (lookup calls != ops, misses or
    control/transport lookups in the no-cancel workload, forward iteration
    counts not growing with C), and derived statistics (medians, per-mode
    OLS slopes, slope reductions, baseline gate) that are not recomputable
    from the raw rows."""
    errs: list[str] = []
    kind = art.get("kind")
    witness_only = kind == "tax0u0witness"
    params = art.get("params")
    if not isinstance(params, dict):
        return ["params: missing (experiment/modes/capacities/depth/"
                "reps/seed/...)"]
    if params.get("experiment") != "TAX-0-EXP-U0":
        errs.append(f"params.experiment: expected 'TAX-0-EXP-U0', got "
                    f"{params.get('experiment')!r}")
    if params.get("witness_only") is not witness_only:
        errs.append(f"params.witness_only {params.get('witness_only')!r} "
                    f"does not match kind {kind!r}")
    modes = params.get("router_scan_modes")
    if modes != list(TAX0U0_MODES):
        errs.append(f"params.router_scan_modes: expected {list(TAX0U0_MODES)}"
                    f", got {modes!r}")
    caps = params.get("capacities")
    if not isinstance(caps, list) or not caps or len(set(caps)) != len(caps) \
            or any(not isinstance(c, int) or c < 1 for c in caps):
        return errs + [f"params.capacities: expected a non-empty unique "
                       f"int list, got {caps!r}"]
    depth = params.get("depth")
    if not isinstance(depth, int) or depth < 1:
        return errs + ["params.depth: expected int >= 1"]
    for c in caps:
        if c < depth:
            errs.append(f"params.capacities: {c} < depth {depth}")
    reps = params.get("reps")
    if not isinstance(reps, int) or reps < 1:
        errs.append(f"params.reps: expected int >= 1, got {reps!r}")
    rs = params.get("request_size")
    tb = params.get("total_bytes")
    if not isinstance(rs, int) or rs < 4096 or rs % 4096 != 0:
        errs.append("params.request_size: must be a multiple of 4096")
    if not isinstance(tb, int) or tb < rs or (rs and tb % rs != 0):
        errs.append("params.total_bytes: must be a positive multiple of "
                    "request_size")
    if params.get("op") is not None and params.get("op") != "read":
        errs.append("params.op: EXP-U0 is a READ experiment")
    if not isinstance(params.get("seed"), int):
        errs.append("params.seed: missing (predeclared order seed)")
    if witness_only:
        if params.get("perf_events") is not None:
            errs.append("params.perf_events: witness artifacts carry no "
                        "perf counters")
    else:
        if not isinstance(params.get("perf_events"), list) or \
                not params.get("perf_events"):
            errs.append("params.perf_events: missing — official EXP-U0 "
                        "artifacts must carry instructions:u/cycles:u")
        env_extra = art.get("environment_extra")
        if not isinstance(env_extra, dict):
            errs.append("environment_extra: missing (governor/SMT/lscpu/"
                        "taskset provenance)")
        elif not isinstance(env_extra.get("taskset_cpus"), str) or \
                not env_extra.get("taskset_cpus").strip():
            errs.append("environment_extra.taskset_cpus: missing — "
                        "official EXP-U0 evidence must be pinned")
    if not isinstance(art.get("environment_id"), str) or \
            not art.get("environment_id"):
        errs.append("environment_id: missing")

    # Execution order: the stored rounds must BE the deterministic output
    # of the predeclared seed under the runner's generator contract over
    # the (scan_mode, capacity) cells.
    order = art.get("execution_order")
    if not isinstance(order, dict):
        return errs + ["execution_order: missing"]
    cells = sorted([(m, c) for m in TAX0U0_MODES for c in caps])
    cell_labels = [f"{m}|C={c}" for m, c in cells]
    if order.get("cells") != cell_labels:
        errs.append("execution_order.cells: not the sorted (mode, C) "
                    "product of params")
    rounds = order.get("rounds")
    if not isinstance(rounds, list) or len(rounds) != reps:
        errs.append(f"execution_order.rounds: expected {reps} rounds")
        rounds = []
    if isinstance(params.get("seed"), int) and isinstance(reps, int):
        rng = random.Random(params["seed"])
        seed_rounds = [[f"{m}|C={c}" for m, c in
                        rng.sample(cells, k=len(cells))]
                       for _ in range(reps)]
        if rounds != seed_rounds:
            errs.append("execution_order.rounds: not the deterministic "
                        "output of the predeclared seed (generator "
                        "contract: random.Random(seed).sample(sorted "
                        "(mode x capacity cells)) per round)")

    rows = art.get("rows")
    if not isinstance(rows, list) or not rows:
        return errs + ["rows: empty"]
    expected_ops = tb // rs if isinstance(tb, int) and isinstance(rs, int) \
        and rs else None
    if len(rows) != reps * len(cells):
        errs.append(f"rows: {len(rows)} rows != reps {reps} x cells "
                    f"{len(cells)}")
    want_flat = [lab for rnd in rounds for lab in (rnd or [])]
    per_cell: dict = {lab: 0 for lab in cell_labels}
    fwd_iters_by_cap: dict = {c: [] for c in caps}
    rev_iters_by_cap: dict = {c: [] for c in caps}
    for i, r in enumerate(rows):
        mode = r.get("router_scan_mode")
        cap = r.get("request_capacity")
        lab = f"{mode}|C={cap}"
        where = f"rows[{i}] ({lab})"
        if i < len(want_flat) and lab != want_flat[i]:
            errs.append(f"{where}: does not match the predeclared "
                        f"execution order position ({want_flat[i]})")
        if lab in per_cell:
            per_cell[lab] += 1
        if r.get("experiment") != "TAX-0-EXP-U0":
            errs.append(f"{where}: experiment is {r.get('experiment')!r}")
        if r.get("execution_order_index") != i:
            errs.append(f"{where}: execution_order_index "
                        f"{r.get('execution_order_index')!r} != {i}")
        if r.get("backend") != "uring" or r.get("op") != "read":
            errs.append(f"{where}: EXP-U0 rows are uring reads")
        if r.get("real_uring") is not True:
            errs.append(f"{where}: real_uring must be true (stub evidence "
                        f"is not EXP-U0 evidence)")
        if r.get("active_depth") != depth or \
                r.get("request_size") != rs or r.get("total_bytes") != tb:
            errs.append(f"{where}: workload geometry drift vs params "
                        f"(same-work violation)")
        if r.get("uring_queue_depth") != params.get("uring_queue_depth"):
            errs.append(f"{where}: uring_queue_depth drift vs params")
        if expected_ops is not None and r.get("ops") != expected_ops:
            errs.append(f"{where}: ops {r.get('ops')!r} != {expected_ops} "
                        f"(same-work guarantee)")
        if r.get("semantic_validation") is not True:
            errs.append(f"{where}: semantic_validation not true")
        if r.get("child_exit_code") not in (0, None):
            errs.append(f"{where}: child_exit_code "
                        f"{r.get('child_exit_code')}")
        for k in ("wall_ns", "user_ns", "sys_ns"):
            v = r.get(k)
            if not isinstance(v, (int, float)) or v <= 0:
                errs.append(f"{where}: {k} missing/non-positive")
        # Scan-witness shape (no-cancel READ workload): exactly one
        # operation-CQE lookup per op, all hits, zero misses, zero control
        # and zero transport lookups.
        if r.get("op_cookie_lookup_calls") != r.get("ops"):
            errs.append(f"{where}: op_cookie_lookup_calls "
                        f"{r.get('op_cookie_lookup_calls')!r} != ops "
                        f"{r.get('ops')!r} — witness inconsistency")
        if r.get("lookup_hits") != r.get("ops") or \
                r.get("lookup_misses") != 0:
            errs.append(f"{where}: lookup hits/misses inconsistent with a "
                        f"no-cancel all-hit workload")
        if r.get("control_cookie_lookup_calls") != 0:
            errs.append(f"{where}: control-CQE contamination "
                        f"({r.get('control_cookie_lookup_calls')})")
        if r.get("transport_cookie_lookup_calls") != 0:
            errs.append(f"{where}: transport lookup calls != 0")
        it = r.get("op_lookup_iterations_total")
        if not isinstance(it, (int, float)) or it <= 0:
            errs.append(f"{where}: op_lookup_iterations_total missing")
        elif cap in fwd_iters_by_cap and mode == "production_forward":
            fwd_iters_by_cap[cap].append(it)
        elif cap in rev_iters_by_cap and mode == "reverse_ablation":
            rev_iters_by_cap[cap].append(it)
        if not witness_only:
            for k in ("instructions_user", "cycles_user"):
                v = r.get(k)
                if not isinstance(v, (int, float)) or v <= 0:
                    errs.append(f"{where}: {k} missing/non-positive — "
                                f"primary metric absent, not zero")
    for lab, n in per_cell.items():
        if n != reps:
            errs.append(f"rows: cell {lab} has {n} rows != reps {reps}")

    # Witness direction fingerprint: forward per-op iterations must grow
    # with C (strictly, on medians); reverse must stay ~flat (slope small).
    ops0 = expected_ops or (rows[0].get("ops") or 0)
    if ops0:
        fwd_meds = {}
        rev_meds = {}
        for c in caps:
            if fwd_iters_by_cap[c]:
                fwd_meds[c] = _median_of([v / ops0 for v in
                                          fwd_iters_by_cap[c]])
            if rev_iters_by_cap[c]:
                rev_meds[c] = _median_of([v / ops0 for v in
                                          rev_iters_by_cap[c]])
        srt = sorted(fwd_meds)
        for a, b in zip(srt, srt[1:]):
            if not fwd_meds[b] > fwd_meds[a]:
                errs.append(f"scan witness: forward iterations/op not "
                            f"strictly increasing with C "
                            f"({a}->{b}: {fwd_meds[a]}->{fwd_meds[b]})")
        if len(rev_meds) >= 2:
            rev_line = _tax0_ols([float(c) for c in sorted(rev_meds)],
                                 [rev_meds[c] for c in sorted(rev_meds)])
            if rev_line and abs(rev_line["b"]) > 0.1:
                errs.append(f"scan witness: reverse iterations/op slope "
                            f"{rev_line['b']:.4f} not ~flat (|b| > 0.1)")

    # Same-work mechanical proof across ALL rows AND BOTH arms.
    sw = art.get("same_work")
    if not isinstance(sw, dict):
        errs.append("same_work: missing mechanical same-work proof block")
    else:
        if sw.get("valid") is not True:
            errs.append("same_work.valid is not true — EXP-U0 invalid, "
                        "numbers must not be interpreted")
        ws = sorted({r.get("word_sum") for r in rows})
        if len(ws) != 1 or rows and ws[0] != rows[0].get("expected_word_sum"):
            errs.append("same_work: word_sum drift across rows/arms")

    # derived: every stored statistic recomputable from the raw rows.
    derived = art.get("derived")
    if not isinstance(derived, dict):
        return errs + ["derived: missing per-cell statistics"]
    metrics = derived.get("per_op_metrics")
    if not isinstance(metrics, dict):
        return errs + ["derived.per_op_metrics: missing"]
    row_keys = {"instructions_per_op": "instructions_user",
                "cycles_per_op": "cycles_user",
                "wall_ns_per_op": "wall_ns",
                "user_ns_per_op": "user_ns",
                "sys_ns_per_op": "sys_ns",
                "scan_iterations_per_op": "op_lookup_iterations_total"}
    for cell_label in cell_labels:
        cell = metrics.get(cell_label)
        if not isinstance(cell, dict):
            errs.append(f"derived.per_op_metrics[{cell_label}]: missing")
            continue
        mode, cap_s = cell_label.split("|C=")
        cap = int(cap_s)
        for name, key in row_keys.items():
            m = cell.get(name)
            if not isinstance(m, dict):
                if name in ("instructions_per_op", "cycles_per_op") and \
                        witness_only:
                    continue  # no perf counters in witness artifacts
                errs.append(f"derived.per_op_metrics[{cell_label}]."
                            f"{name}: missing")
                continue
            samples = m.get("samples")
            raw = [r[key] / (expected_ops or r["ops"]) for r in rows
                   if r.get("router_scan_mode") == mode
                   and r.get("request_capacity") == cap
                   and isinstance(r.get(key), (int, float))]
            if not isinstance(samples, list) or len(samples) != len(raw):
                errs.append(f"derived.per_op_metrics[{cell_label}].{name}"
                            f".samples: expected {len(raw)} raw samples")
                continue
            if any(not _tax0_close(s, w) for s, w in zip(samples, raw)):
                errs.append(f"derived.per_op_metrics[{cell_label}].{name}"
                            f".samples: not the per-op series recomputed "
                            f"from the raw rows")
            want = _median_of(raw)
            got = m.get("median")
            if not _tax0_close(got, want):
                errs.append(f"derived.per_op_metrics[{cell_label}].{name}"
                            f".median {got!r} != recomputed {want!r}")

    # Per-mode capacity slopes recomputed from raw-row medians.
    slopes = derived.get("capacity_slopes_ols")
    if not isinstance(slopes, dict):
        errs.append("derived.capacity_slopes_ols: missing")
    else:
        for mode in TAX0U0_MODES:
            for name, key in (("instructions_per_op", "instructions_user"),
                              ("scan_iterations_per_op",
                               "op_lookup_iterations_total")):
                xs, ys, usable = [], [], True
                for c in caps:
                    vals = [r[key] / (expected_ops or r["ops"]) for r in
                            rows if r.get("router_scan_mode") == mode
                            and r.get("request_capacity") == c
                            and isinstance(r.get(key), (int, float))]
                    if len(vals) != reps:
                        usable = False
                    if vals:
                        xs.append(float(c))
                        ys.append(_median_of(vals))
                got = slopes.get(f"{name}|{mode}")
                if not usable:
                    continue
                if name == "instructions_per_op" and witness_only:
                    if got is not None:
                        errs.append(f"derived.capacity_slopes_ols."
                                    f"{name}|{mode}: present in a witness "
                                    f"artifact without perf counters")
                    continue
                want = _tax0_ols(xs, ys) if len(xs) >= 2 else None
                if want is None:
                    if got is not None:
                        errs.append(f"derived.capacity_slopes_ols.{name}|"
                                    f"{mode}: present but not recomputable")
                    continue
                if not isinstance(got, dict):
                    errs.append(f"derived.capacity_slopes_ols.{name}|"
                                f"{mode}: missing")
                    continue
                for k in ("a", "b"):
                    if not _tax0_close(got.get(k), want[k]):
                        errs.append(f"derived.capacity_slopes_ols.{name}|"
                                    f"{mode}.{k}: stored {got.get(k)!r} != "
                                    f"recomputed {want[k]!r}")
                if isinstance(want["r2"], (int, float)):
                    if not _tax0_close(got.get("r2"), want["r2"]):
                        errs.append(f"derived.capacity_slopes_ols.{name}|"
                                    f"{mode}.r2: stored {got.get('r2')!r} "
                                    f"!= recomputed {want['r2']!r}")

        # Slope reductions recomputable from the stored/recomputed slopes.
        red = derived.get("slope_reductions")
        if isinstance(red, dict):
            for rel, slope_name in (("instruction_slope",
                                     "instructions_per_op"),
                                    ("iteration_slope",
                                     "scan_iterations_per_op")):
                stored = red.get(rel)
                if not isinstance(stored, dict):
                    if not witness_only or slope_name != \
                            "instructions_per_op":
                        errs.append(f"derived.slope_reductions.{rel}: "
                                    f"missing")
                    continue
                f_line = slopes.get(f"{slope_name}|production_forward")
                r_line = slopes.get(f"{slope_name}|reverse_ablation")
                if isinstance(f_line, dict) and isinstance(r_line, dict) \
                        and r_line.get("b") not in (None, 0) and \
                        f_line.get("b"):
                    want_red = 1.0 - abs(r_line["b"] / f_line["b"])
                    if not _tax0_close(stored.get("reduction"), want_red):
                        errs.append(f"derived.slope_reductions.{rel}."
                                    f"reduction: stored "
                                    f"{stored.get('reduction')!r} != "
                                    f"recomputed {want_red!r}")

    # Baseline reproduction gate: envelope must be the preregistered
    # constants and the pass flag recomputable from the forward slope.
    base = derived.get("baseline_reproduction")
    if not isinstance(base, dict):
        if not witness_only:
            errs.append("derived.baseline_reproduction: missing")
    else:
        if base.get("envelope") != TAX0U0_BASELINE_ENVELOPE:
            errs.append("derived.baseline_reproduction.envelope: not the "
                        "preregistered constants (post-hoc widening is "
                        "forbidden)")
        f_line = (slopes or {}).get(
            "instructions_per_op|production_forward")
        if isinstance(f_line, dict) and f_line.get("b") is not None:
            want_pass = (
                TAX0U0_BASELINE_ENVELOPE["b_min"] <= f_line["b"]
                <= TAX0U0_BASELINE_ENVELOPE["b_max"]
                and isinstance(f_line.get("r2"), (int, float))
                and f_line["r2"] >= TAX0U0_BASELINE_ENVELOPE["r2_min"])
            if base.get("pass") is not want_pass:
                errs.append(f"derived.baseline_reproduction.pass: stored "
                            f"{base.get('pass')!r} != recomputed "
                            f"{want_pass!r}")
    return errs


CHECKS = {"ladder": check_ladder, "cli": check_cli, "perf": check_perf,
          "overload": check_overload, "e1tax": check_e1tax,
          "tax0capacity": check_tax0capacity,
          "tax0u0router": check_tax0u0, "tax0u0witness": check_tax0u0}


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


def _valid_e1tax() -> dict:
    """A minimal structurally-valid e1tax artifact (one L0/L1/L2 group)."""
    art = _valid_ladder()
    art["kind"] = "e1tax"
    art["params"] = {"matrix": "custom", "ops": ["read"],
                     "ladders": ["L0_raw", "L1_pool", "L2_sluice"],
                     "sizes": [4096], "depths": [1], "workers": [1],
                     "total_bytes": 4096, "reps": 3, "warmup": 1,
                     "latency": False}
    def cell(ladder, samples):
        med = sorted(samples)[1]
        return {"op": "read", "ladder": ladder, "request_size": 4096,
                "depth": 1, "workers": 1, "ops": 1, "bytes": 4096,
                "expected_ops": 1, "completed_ops": 1,
                "expected_bytes": 4096, "completed_bytes": 4096,
                "errors": 0, "word_sum_ok": True,
                "wall_ns_samples": samples, "wall_ns_min": min(samples),
                "wall_ns_med": med, "wall_ns_max": max(samples),
                "wall_ns_p25": samples[0], "wall_ns_p75": samples[2],
                "user_ns_med": 100, "sys_ns_med": 100, "maxrss_kb_max": 4096,
                "lifecycle_setup_ns": 500, "lifecycle_teardown_ns": 500}

    cells = [cell("L0_raw", [1000, 1100, 1200]),
             cell("L1_pool", [1400, 1500, 1600]),
             cell("L2_sluice", [2150, 2250, 2350])]
    art["cells"] = cells
    art["derived"] = [{
        "op": "read", "request_size": 4096, "depth": 1, "workers": 1,
        "ops": 1, "l0_ns_med": 1100, "l1_ns_med": 1500, "l2_ns_med": 2250,
        "threadpool_direct_tax_ns": 400, "sluice_incremental_tax_ns": 750,
        "sluice_overhead_ratio": 0.5, "l1_l0_per_request_ns": 400.0,
        "l2_l1_per_request_ns": 750.0,
    }]
    art["diagnostics"] = {
        "perf": {"available": False, "reason": "not requested (--perf)",
                 "mode": None, "perf_event_paranoid": 2},
        "bpftrace": {"available": False, "reason": "not requested"},
    }
    return art


def _valid_tax0capacity() -> dict:
    """A minimal structurally-valid tax0capacity artifact (2 capacities ×
    3 reps, threadpool arm). The derived block is built with the same
    recomputation helpers the validator enforces, so any tampering the
    tests plant is a real divergence, not a factory inconsistency."""
    caps, reps, seed = [8, 32], 3, 0x54415830
    ops = 4
    art = _valid_ladder()
    art["kind"] = "tax0capacity"
    art["params"] = {"experiment": "TAX-0B-EXP0", "backend": "threadpool",
                     "op": "read", "capacities": caps, "depth": 8,
                     "request_size": 4096, "total_bytes": ops * 4096,
                     "reps": reps, "seed": seed, "worker_count": 1,
                     "perf_events": ["instructions:u", "cycles:u"]}
    rng = random.Random(seed)
    rounds = [rng.sample(caps, k=len(caps)) for _ in range(reps)]
    art["execution_order"] = {"seed": seed, "rounds": rounds,
                              "flat": [c for r in rounds for c in r]}
    art["environment_extra"] = {"taskset_cpus": "0,2,4,6"}
    art["environment_id"] = "envfingerprint0"
    art["same_work"] = {"valid": True}
    # deterministic per-op counters with an exact capacity slope (instr +6,
    # cycles +2) and a per-rep ±1 wiggle inside each cell
    med = {c: {"instructions_user": (4000 + 6 * c + 1) * ops,
               "cycles_user": (3000 + 2 * c + 1) * ops} for c in caps}
    seen: dict = {c: 0 for c in caps}
    rows = []
    for i, cap in enumerate(art["execution_order"]["flat"]):
        wiggle = seen[cap] - 1
        seen[cap] += 1
        rows.append({"experiment": "TAX-0B-EXP0",
                     "execution_order_index": i, "request_capacity": cap,
                     "backend": "threadpool", "op": "read",
                     "active_depth": 8, "request_size": 4096,
                     "total_bytes": ops * 4096, "ops": ops,
                     "semantic_validation": True, "child_exit_code": 0,
                     "wall_ns": 5000 + i, "user_ns": 2500, "sys_ns": 2500,
                     "instructions_user": med[cap]["instructions_user"] +
                     wiggle * ops,
                     "cycles_user": med[cap]["cycles_user"] + wiggle * ops,
                     "worker_count": 1, "real_uring": False})
    art["rows"] = rows
    per_op: dict = {str(c): {} for c in caps}
    med_of: dict = {}
    for name, key in (("instructions_per_op", "instructions_user"),
                      ("cycles_per_op", "cycles_user"),
                      ("wall_ns_per_op", "wall_ns")):
        med_of[name] = {}
        by_cap = _tax0_samples_from_rows(rows, caps, key)
        for c in caps:
            samples = by_cap[c]
            per_op[str(c)][name] = {"n": len(samples),
                                    "median": _median_of(samples),
                                    "samples": samples}
            med_of[name][c] = _median_of(samples)
    deltas = {}
    for name in ("instructions_per_op", "cycles_per_op"):
        b = med_of[name][min(caps)]
        deltas[name] = {str(c): {"absolute": med_of[name][c] - b,
                                 "percent": 100.0 * (med_of[name][c] - b) / b}
                        for c in caps}
    slopes = {name: _tax0_ols([float(c) for c in caps],
                              [med_of[name][c] for c in caps])
              for name in ("instructions_per_op", "cycles_per_op")}
    art["derived"] = {"per_op_metrics": per_op,
                      "baseline_capacity": min(caps),
                      "delta_vs_baseline": deltas,
                      "capacity_slope_ols": slopes}
    return art


def _valid_tax0u0(witness_only: bool = False) -> dict:
    """A minimal structurally-valid EXP-U0 artifact: 2 modes x 2 capacities
    x 3 reps, synthetic but internally consistent (rows, seed-derived
    order, recomputable medians/slopes/reductions, preregistered
    envelope). Mirrors the runner's emission contract."""
    caps = [8, 128]
    reps = 3
    seed = 0x55304C55
    depth, q, rs_ = 8, 8, 4096
    ops = (1 << 20) // rs_
    tb = ops * rs_
    art = _valid_ladder()
    cells = sorted([(m, c) for m in TAX0U0_MODES for c in caps])
    rng = random.Random(seed)
    rounds = [rng.sample(cells, k=len(cells)) for _ in range(reps)]
    rows = []
    base_instr = {"production_forward": {8: 1000.0 * ops, 128: 1480.0 * ops},
                  "reverse_ablation": {8: 1000.0 * ops, 128: 1010.0 * ops}}
    base_cyc = {"production_forward": {8: 3000.0 * ops, 128: 3260.0 * ops},
                "reverse_ablation": {8: 3000.0 * ops, 128: 3040.0 * ops}}
    fwd_iters = {8: 4.5, 128: 124.5}     # ~C - D/2
    rev_iters = {8: 4.5, 128: 4.6}       # ~D/2, flat
    for i, (m, c) in enumerate([x for rnd in rounds for x in rnd]):
        jitter = 1.0 + 0.001 * (i % 3)
        row = {
            "experiment": "TAX-0-EXP-U0",
            "git_sha": "0" * 40,
            "router_scan_mode": m,
            "backend": "uring", "real_uring": True,
            "filesystem": "tmpfs", "op": "read",
            "request_size": rs_, "active_depth": depth,
            "request_capacity": c, "uring_queue_depth": q,
            "total_bytes": tb, "ops": ops,
            "rep": sum(1 for r in rows
                       if r["router_scan_mode"] == m
                       and r["request_capacity"] == c),
            "execution_order_index": i,
            "wall_ns": int(2_000_000_000 * jitter),
            "user_ns": int(1_000_000_000 * jitter),
            "sys_ns": int(500_000_000 * jitter),
            "op_cookie_lookup_calls": ops,
            "op_lookup_iterations_total": int(
                (fwd_iters if m == "production_forward" else rev_iters)[c]
                * ops * jitter),
            "op_lookup_iterations_max": c,
            "control_cookie_lookup_calls": 0,
            "transport_cookie_lookup_calls": 0,
            "lookup_hits": ops, "lookup_misses": 0,
            "matched_router_index_sum": int(ops * (c - 4.5)),
            "matched_router_index_max": c - 1,
            "semantic_validation": True,
            "word_sum": 12345678901234567890,
            "expected_word_sum": 12345678901234567890,
            "child_exit_code": 0,
            "environment_id": "abcd0123abcd0123",
        }
        if not witness_only:
            row["instructions_user"] = int(base_instr[m][c] * jitter)
            row["cycles_user"] = int(base_cyc[m][c] * jitter)
            row["perf_raw"] = "synthetic"
        rows.append(row)
    art["kind"] = "tax0u0witness" if witness_only else "tax0u0router"
    art["binary"] = {"path": "tax0u0_router_bench",
                     "sha256": "f" * 64, "size": 12345,
                     "mtime": 1756600000.0}
    art["params"] = {"experiment": "TAX-0-EXP-U0",
                      "question": "...", "hypothesis": "...",
                      "router_scan_modes": list(TAX0U0_MODES),
                      "capacities": caps, "depth": depth,
                      "uring_queue_depth": q, "request_size": rs_,
                      "total_bytes": tb, "reps": reps,
                      "warmup_rounds": 0 if witness_only else 2,
                      "seed": seed, "fs_label": "tmpfs",
                      "taskset": "0,2", "witness_only": witness_only,
                      "perf_events": None if witness_only
                      else ["instructions:u", "cycles:u"]}
    art["execution_order"] = {
               "generator": "...", "seed": seed, "reps": reps,
               "cells": [f"{m}|C={c}" for m, c in cells],
               "rounds": [[f"{m}|C={c}" for m, c in rnd]
                          for rnd in rounds]}
    art["environment_extra"] = {"taskset_cpus": "0,2"}
    art["environment_id"] = "abcd0123abcd0123"
    art["rows"] = rows
    art["same_work"] = {"ops_expected": ops,
                         "ops_observed": [ops],
                         "bytes_expected": tb, "bytes_observed": [tb],
                         "depth": depth, "request_size": rs_,
                         "word_sum_expected": 12345678901234567890,
                         "word_sum_observed": [12345678901234567890],
                         "validation_all_true": True,
                         "valid": True}
    # Reuse the runner's derived computation by importing the pure helpers
    # from the runner module (same repo, no side effects).
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "perf_attribution", REPO / "scripts" / "bench" /
        "perf-attribution.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    art["derived"] = mod.tax0u0_derived(rows, caps)
    return art


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

    def test_valid_e1tax_passes(self):
        self.assertEqual(validate_artifact(_valid_e1tax()), [])

    def test_e1tax_detectors_fire(self):
        # completed ops below the same-work expectation (the ladder did
        # less work than it claims — timings are not comparable)
        art = _valid_e1tax()
        art["cells"][2]["completed_ops"] = 0
        self.assert_invalid(art, "completed_ops")
        # zero repetitions are not evidence
        art = _valid_e1tax()
        art["cells"][0]["wall_ns_samples"] = []
        self.assert_invalid(art, "wall_ns_samples missing/empty")
        # a non-numeric sample (unknown unit / corrupted artifact)
        art = _valid_e1tax()
        art["cells"][0]["wall_ns_samples"] = [1000, "1100ns", 1200]
        self.assert_invalid(art, "non-numeric value")
        # sample count contradicting params.reps
        art = _valid_e1tax()
        art["cells"][0]["wall_ns_samples"] = [1000, 1200]
        self.assert_invalid(art, "!= params.reps")
        # median inconsistent with the raw samples (hand-typed table)
        art = _valid_e1tax()
        art["cells"][1]["wall_ns_med"] = 9999
        self.assert_invalid(art, "recomputed median")
        # min/max ordering violated
        art = _valid_e1tax()
        art["cells"][0]["wall_ns_min"] = 5000
        self.assert_invalid(art, "wall_ns_min<=med<=max")
        # recorded I/O errors invalidate the row
        art = _valid_e1tax()
        art["cells"][0]["errors"] = 2
        self.assert_invalid(art, "errors 2 recorded")
        # a read cell without the word-sum verification
        art = _valid_e1tax()
        art["cells"][0]["word_sum_ok"] = False
        self.assert_invalid(art, "word_sum_ok missing/false")
        # derived tax row contradicting the cell medians
        art = _valid_e1tax()
        art["derived"][0]["sluice_incremental_tax_ns"] = 1
        self.assert_invalid(art, "cells say")
        # a full-ladder run missing one group's tax row
        art = _valid_e1tax()
        art["derived"] = []
        self.assert_invalid(art, "missing tax row")
        # duplicate cell keys
        art = _valid_e1tax()
        art["cells"].append(dict(art["cells"][0]))
        self.assert_invalid(art, "duplicate cell key")
        # unavailable diagnostics without a reason (fake-zero class)
        art = _valid_e1tax()
        art["diagnostics"]["perf"] = {"available": False}
        self.assert_invalid(art, "without a reason")
        # diagnostics block missing entirely
        art = _valid_e1tax()
        del art["diagnostics"]
        self.assert_invalid(art, "diagnostics")
        # lifecycle scope must be explicit (steady-state claim)
        art = _valid_e1tax()
        del art["cells"][0]["lifecycle_setup_ns"]
        self.assert_invalid(art, "lifecycle setup/teardown")
        # unknown ladder in params
        art = _valid_e1tax()
        art["params"]["ladders"] = ["L0_raw", "L3_uring"]
        self.assert_invalid(art, "params.ladders")

    def test_valid_tax0u0_passes(self):
        self.assertEqual(validate_artifact(_valid_tax0u0(False)), [])
        self.assertEqual(validate_artifact(_valid_tax0u0(True)), [])

    def test_tax0u0_detectors_fire(self):
        # scan-mode tampering: relabel a row's mode -> cell/order mismatch.
        art = _valid_tax0u0(False)
        art["rows"][0]["router_scan_mode"] = (
            "production_forward"
            if art["rows"][0]["router_scan_mode"] == "reverse_ablation"
            else "reverse_ablation")
        self.assert_invalid(art, "does not match the predeclared "
                              "execution order")

        # seed/order tampering: rotate one stored round.
        art = _valid_tax0u0(False)
        eo = art["execution_order"]
        eo["rounds"][0] = eo["rounds"][0][1:] + eo["rounds"][0][:1]
        self.assert_invalid(art, "not the deterministic output of the "
                              "predeclared seed")

        # scan-iteration tampering: raw row counter no longer matches the
        # stored derived samples.
        art = _valid_tax0u0(False)
        fwd = next(r for r in art["rows"]
                   if r["router_scan_mode"] == "production_forward")
        fwd["op_lookup_iterations_total"] = int(
            fwd["op_lookup_iterations_total"] * 0.5)
        self.assert_invalid(art, "not the per-op series recomputed from "
                              "the raw rows")

        # slope tampering: stored OLS b drifts from the recomputation.
        art = _valid_tax0u0(False)
        art["derived"]["capacity_slopes_ols"][
            "instructions_per_op|production_forward"]["b"] = 0.123
        self.assert_invalid(
            art, "capacity_slopes_ols.instructions_per_op|"
                 "production_forward.b")

        # same-work tampering: one row moves a different byte payload.
        art = _valid_tax0u0(False)
        art["rows"][1]["word_sum"] = 1
        self.assert_invalid(art, "word_sum drift")

        # witness inconsistency: lookup calls drift from ops.
        art = _valid_tax0u0(False)
        art["rows"][2]["op_cookie_lookup_calls"] += 1
        self.assert_invalid(art, "op_cookie_lookup_calls")

        # envelope tampering: post-hoc widening of the preregistered gate.
        art = _valid_tax0u0(False)
        art["derived"]["baseline_reproduction"]["envelope"] = {
            "b_min": 0.0, "b_max": 100.0, "r2_min": 0.0}
        self.assert_invalid(art, "not the preregistered constants")

        # witness artifact smuggling perf-less performance evidence.
        art = _valid_tax0u0(True)
        art["kind"] = "tax0u0router"
        art["params"]["witness_only"] = False
        art["params"]["perf_events"] = ["instructions:u"]
        self.assert_invalid(art, "instructions_user missing/non-positive")

    def test_valid_tax0capacity_passes(self):
        self.assertEqual(validate_artifact(_valid_tax0capacity()), [])

    def test_tax0capacity_detectors_fire(self):
        # A. self-consistent but NOT seed-derived order: reverse every
        # round, then rebuild flat AND rows to match — permutations check,
        # flat check, and row-order check all pass; only the seed-contract
        # recomputation can catch it
        art = _valid_tax0capacity()
        order = art["execution_order"]
        order["rounds"] = [list(reversed(r)) for r in order["rounds"]]
        order["flat"] = [c for r in order["rounds"] for c in r]
        by_cap: dict = {}
        for r in art["rows"]:
            by_cap.setdefault(r["request_capacity"], []).append(r)
        art["rows"] = []
        for i, c in enumerate(order["flat"]):
            r = dict(by_cap[c].pop(0))
            r["execution_order_index"] = i
            art["rows"].append(r)
        self.assert_invalid(art, "predeclared seed")
        # B. headline OLS slope corrupted: b nudged off the raw-row
        # recomputation
        art = _valid_tax0capacity()
        art["derived"]["capacity_slope_ols"]["instructions_per_op"]["b"] \
            += 0.125
        self.assert_invalid(art, "recomputed from raw rows")
        # intercept zeroed out
        art = _valid_tax0capacity()
        art["derived"]["capacity_slope_ols"]["instructions_per_op"]["a"] = 0.0
        self.assert_invalid(art, "capacity_slope_ols")
        # C. baseline delta corrupted (absolute, primary metric)
        art = _valid_tax0capacity()
        art["derived"]["delta_vs_baseline"]["instructions_per_op"]["32"] \
            ["absolute"] += 1.5
        self.assert_invalid(art, "delta_vs_baseline")
        # C'. delta percent corrupted on the secondary metric
        art = _valid_tax0capacity()
        art["derived"]["delta_vs_baseline"]["cycles_per_op"]["32"] \
            ["percent"] = 0.0
        self.assert_invalid(art, "delta_vs_baseline")
        # stored median tampered while samples are kept (hand-typed table)
        art = _valid_tax0capacity()
        art["derived"]["per_op_metrics"]["32"] \
            ["instructions_per_op"]["median"] += 1.0
        self.assert_invalid(art, "!= recomputed")
        # FC-EXP0-8: samples AND median tampered together, consistently —
        # raw rows and the OLS/delta blocks untouched; only the raw-row
        # binding catches it
        art = _valid_tax0capacity()
        cell = art["derived"]["per_op_metrics"]["32"]["instructions_per_op"]
        cell["samples"] = [s + 1.0 for s in cell["samples"]]
        cell["median"] = _median_of(cell["samples"])
        self.assert_invalid(art, "recomputed from the raw rows")


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
