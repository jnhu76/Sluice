#!/usr/bin/env python3
"""RE-H0 mechanically recomputable analysis (#277).

All medians, MADs, ratios, materiality classifications and ladder
verdicts for the RE-H0 campaign are produced by this module from the
session summary JSON; markdown reports never act as the data authority
(RE-H0-PREREGISTRATION.md, analysis rule A1).

Frozen vocabulary (preregistered before formal measurement):

  ratio direction  = cost candidate / baseline
      C_sem     = Z1b  / Z1      CAPABILITY COST
      T_backend = Z2   / Z1b     BACKEND ABSTRACTION TAX
      C_cont    = Z1bw / Z1b     CONTINUATION COST
      T_runtime = Z3   / Z1bw    RUNTIME/MEDIATION TAX
      T_pool    = L1   / L0      fixed-pool execution cost
      T_sluice  = L2   / L1      Sluice ThreadPool incremental tax

  PARITY       ratio <= 1.05 AND robust intervals overlap AND every
               independent instruction estimate agrees (<= 1.05)
  MATERIAL_TAX ratio >= 1.10 AND robust intervals disjoint AND every
               independent instruction estimate agrees (>= 1.10)
  GRAY         everything else, including any ratio < 1.0 (a floor
               baseline measured slower than its candidate is a
               comparability anomaly, never folded into parity)

Robust interval = median +/- 1.5 * MAD of the per-rep samples.
Wall samples exist per rep; instruction estimates are independent
double-difference points (P7/P14 pairs), so they enter as a
direction-consistency gate, not as an interval.

Fail-closed (analysis rule A2): a block with a missing/duplicated arm,
a failed rep, an unverified write, a same-work word_sum mismatch, a
binary sha mismatch, any recorded error text, or empty wall samples
raises SessionInvalid. A block whose instruction estimates are missing
raises IndeterminateMetric: the claim degrades to indeterminate, it is
never silently recomputed from wall alone.

Self-test diagnostics: python3 check_re_h0_analysis.py
"""

import math


class SessionInvalid(Exception):
    """The session block cannot support any verdict (fail closed)."""


class IndeterminateMetric(Exception):
    """A required metric is missing; the claim degrades, not substitutes."""


# ---------------------------------------------------------------------------
# robust statistics
# ---------------------------------------------------------------------------

def median(values):
    s = sorted(values)
    n = len(s)
    if n == 0:
        raise SessionInvalid("median of empty sample")
    mid = n // 2
    if n % 2:
        return s[mid]
    return (s[mid - 1] + s[mid]) / 2.0


def mad(values):
    m = median(values)
    return median([abs(v - m) for v in values])


def robust_interval(values):
    """median +/- 1.5*MAD (preregistered interval definition)."""
    m = median(values)
    d = 1.5 * mad(values)
    return m - d, m + d


def _intervals_overlap(a, b):
    return not (a[1] < b[0] or b[1] < a[0])


def _no_strong_separation(base_wall, cand_wall):
    """Preregistered separation test (see classify). Returns
    (degenerate, disjoint): degenerate = both series constant (MAD == 0);
    disjoint = robust intervals do not intersect."""
    disjoint = not _intervals_overlap(robust_interval(base_wall),
                                      robust_interval(cand_wall))
    degenerate = mad(base_wall) == 0.0 and mad(cand_wall) == 0.0
    return degenerate, disjoint


# ---------------------------------------------------------------------------
# ratio + classification
# ---------------------------------------------------------------------------

def ratio_and_delta(base_samples, cand_samples):
    """(ratio, absolute delta) of medians, direction candidate/baseline."""
    bm = median(base_samples)
    cm = median(cand_samples)
    if bm <= 0:
        raise SessionInvalid("non-positive baseline median")
    return cm / bm, cm - bm


_ABSENT = object()


def _instr_gate(base_instr, cand_instr, *, material: bool):
    """Direction consistency of the independent instruction estimates.

    Returns 'agree_material' | 'agree_parity' | 'disagree'.
    base_instr/cand_instr: lists of per-estimate values (same length),
    or _ABSENT to skip the gate (synthetic diagnostics only).
    """
    if base_instr is _ABSENT and cand_instr is _ABSENT:
        return "agree_parity" if not material else "agree_material"
    if base_instr is None or cand_instr is None:
        raise IndeterminateMetric(
            "instruction estimates missing: claim is indeterminate, "
            "never wall-only")
    if len(base_instr) == 0 or len(cand_instr) != len(base_instr):
        raise IndeterminateMetric("instruction estimate pairs malformed")
    ratios = []
    for b, c in zip(base_instr, cand_instr):
        if b <= 0:
            raise SessionInvalid("non-positive baseline instruction estimate")
        ratios.append(c / b)
    if material:
        return "agree_material" if all(r >= 1.10 for r in ratios) \
            else "disagree"
    return "agree_parity" if all(r <= 1.05 for r in ratios) else "disagree"


def classify(base_wall, cand_wall, base_instr=_ABSENT, cand_instr=_ABSENT):
    """Frozen PARITY / MATERIAL_TAX / GRAY vocabulary.

    base_wall/cand_wall: per-rep per-op samples (>= 2 values).
    Instruction estimates, when supplied, gate the verdict: a MATERIAL
    call needs every estimate >= 1.10, a PARITY call every estimate
    <= 1.05; disagreement collapses to GRAY.
    """
    if len(base_wall) < 2 or len(cand_wall) < 2:
        raise SessionInvalid("classification needs >= 2 samples per arm")
    ratio, _ = ratio_and_delta(base_wall, cand_wall)
    degenerate, disjoint = _no_strong_separation(base_wall, cand_wall)
    if ratio < 1.0:
        return "GRAY"
    if ratio <= 1.05:
        # degenerate series carry no separation evidence: the ratio band
        # alone decides; real variance uses the strict overlap rule.
        parity_sep = (not disjoint) or degenerate
        gate = _instr_gate(base_instr, cand_instr, material=False)
        return "PARITY" if (parity_sep and gate == "agree_parity") else "GRAY"
    if ratio >= 1.10:
        material_sep = disjoint or degenerate
        gate = _instr_gate(base_instr, cand_instr, material=True)
        return "MATERIAL_TAX" if (material_sep and gate == "agree_material") \
            else "GRAY"
    return "GRAY"


# ---------------------------------------------------------------------------
# row access + fail-closed block validation
# ---------------------------------------------------------------------------

Z_LADDER_ARMS = ["z1", "z1b", "z1bw", "z2", "z3"]
E1_LADDER_ARMS = ["L0", "L1", "L2"]


def _rows_for(rows, fs, op, request_size, depth):
    out = []
    for r in rows:
        if (r.get("fs") == fs and r.get("op") == op
                and r.get("request_size") == request_size
                and r.get("depth") == depth):
            out.append(r)
    return out


def _validate_block(rows, fs, op, request_size, depth, required_arms,
                    expected_binary_sha256=None):
    """Fail-closed session-block checks (analysis rule A2)."""
    if not rows:
        raise SessionInvalid(f"empty block {fs}/{op}/r{request_size}/d{depth}")
    seen = {}
    for r in rows:
        arm = r.get("arm")
        if arm in seen:
            raise SessionInvalid(f"duplicate combo row: {arm}")
        seen[arm] = r
        for key in ("fs", "op", "request_size", "depth"):
            want = {"fs": fs, "op": op, "request_size": request_size,
                    "depth": depth}[key]
            if r.get(key) != want:
                raise SessionInvalid(
                    f"row {arm} field {key}={r.get(key)!r} != block {want!r}")
    for arm in required_arms:
        if arm not in seen:
            raise SessionInvalid(f"missing arm {arm} in block")
    for arm, r in seen.items():
        if not r.get("ok"):
            raise SessionInvalid(f"arm {arm} reports ok=false")
        note = r.get("error_note")
        if note:
            raise SessionInvalid(
                f"arm {arm} carries error_note (unexpected terminal/error "
                f"text): {note!r}")
        wall = r.get("wall_ns_per_op_samples")
        if not wall or len(wall) < 2:
            raise SessionInvalid(f"arm {arm} has < 2 wall samples")
        if r.get("instr_u_per_op_estimates", _ABSENT) is None:
            raise IndeterminateMetric(
                f"arm {arm} instruction estimates missing")
        if expected_binary_sha256 is not None:
            if r.get("binary_sha256") != expected_binary_sha256:
                raise SessionInvalid(
                    f"arm {arm} binary sha mismatch vs manifest")
        if op == "read":
            if r.get("word_sum") is None:
                raise SessionInvalid(f"arm {arm} read missing word_sum")
        else:
            if not r.get("write_verified"):
                raise SessionInvalid(f"arm {arm} write not verified")
    if op == "read":
        sums = {r.get("word_sum") for r in seen.values()}
        if len(sums) != 1:
            raise SessionInvalid(
                f"same-work mismatch: word_sum differs across arms {sums}")
    return seen


def _verdict_rows(seen, pairs):
    """pairs: list of (name, cand_arm, base_arm). Returns verdict dict."""
    out = {}
    for name, cand, base in pairs:
        br = seen[base]["wall_ns_per_op_samples"]
        cr = seen[cand]["wall_ns_per_op_samples"]
        ratio, delta = ratio_and_delta(br, cr)
        verdict = classify(
            br, cr,
            seen[base].get("instr_u_per_op_estimates", _ABSENT),
            seen[cand].get("instr_u_per_op_estimates", _ABSENT))
        out[name] = {
            "cand": cand,
            "base": base,
            "ratio": ratio,
            "delta_per_op": delta,
            "base_median_wall_ns": median(br),
            "cand_median_wall_ns": median(cr),
            "verdict": verdict,
        }
    return out


def _case_from(back_name, run_name, verdicts):
    back = verdicts[back_name]["verdict"]
    run = verdicts[run_name]["verdict"]
    if back == "MATERIAL_TAX" and run == "MATERIAL_TAX":
        return "CASE_D"
    if back == "MATERIAL_TAX":
        return "CASE_B"
    if run == "MATERIAL_TAX":
        return "CASE_C"
    if back == "PARITY" and run == "PARITY":
        return "CASE_A"
    return "CASE_GRAY"


# ---------------------------------------------------------------------------
# ladder verdicts
# ---------------------------------------------------------------------------

def validate_re1u_block(rows, fs, op, request_size, depth,
                        expected_binary_sha256=None):
    """Public fail-closed check for one RE-1U block (see _validate_block)."""
    return _validate_block(rows, fs, op, request_size, depth, Z_LADDER_ARMS,
                           expected_binary_sha256)


def re1u_ladder_verdict(rows, fs, op, request_size, depth,
                        expected_binary_sha256=None):
    """RE-1U decomposition for one block; CASE A/B/C/D mapping."""
    seen = _validate_block(rows, fs, op, request_size, depth, Z_LADDER_ARMS,
                           expected_binary_sha256)
    verdicts = _verdict_rows(seen, [
        ("C_sem", "z1b", "z1"),
        ("T_backend", "z2", "z1b"),
        ("C_cont", "z1bw", "z1b"),
        ("T_runtime", "z3", "z1bw"),
    ])
    return {
        "block": {"fs": fs, "op": op, "request_size": request_size,
                  "depth": depth},
        "C_sem": verdicts["C_sem"],
        "T_backend": verdicts["T_backend"],
        "C_cont": verdicts["C_cont"],
        "T_runtime": verdicts["T_runtime"],
        "case": _case_from("T_backend", "T_runtime", verdicts),
    }


def re1_ladder_verdict(rows, fs, op, request_size, depth, workers,
                       expected_binary_sha256=None):
    """RE-1 decomposition: T_pool = L1/L0, T_sluice = L2/L1."""
    seen = _validate_block(rows, fs, op, request_size, depth, E1_LADDER_ARMS,
                           expected_binary_sha256)
    for r in seen.values():
        if r.get("workers") != workers:
            raise SessionInvalid(
                f"row {r.get('arm')} workers={r.get('workers')} != "
                f"block {workers}")
    verdicts = _verdict_rows(seen, [
        ("T_pool", "L1", "L0"),
        ("T_sluice", "L2", "L1"),
    ])
    return {
        "block": {"fs": fs, "op": op, "request_size": request_size,
                  "depth": depth, "workers": workers},
        "T_pool": verdicts["T_pool"],
        "T_sluice": verdicts["T_sluice"],
    }


def pair_verdict(rows, fs, op, request_size, depth, cand_arm, base_arm,
                 workers=None, expected_binary_sha256=None):
    """RE-2 pairwise verdict against a block's own floor arm."""
    seen = _validate_block(rows, fs, op, request_size, depth,
                           sorted({cand_arm, base_arm}),
                           expected_binary_sha256)
    if workers is not None:
        for r in seen.values():
            if r.get("workers") not in (None, workers):
                raise SessionInvalid(
                    f"row {r.get('arm')} workers mismatch")
    verdicts = _verdict_rows(seen, [("pair", cand_arm, base_arm)])
    return verdicts["pair"]
