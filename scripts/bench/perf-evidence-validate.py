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
    if kind not in ("ladder", "cli", "perf"):
        errs.append(f"kind: expected ladder|cli|perf, got {kind!r}")
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


CHECKS = {"ladder": check_ladder, "cli": check_cli, "perf": check_perf}


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
