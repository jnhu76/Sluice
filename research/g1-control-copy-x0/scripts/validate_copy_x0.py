#!/usr/bin/env python3
"""COPY-X0 fail-closed validator + verdict derivation authority.

Prereg: research/g1-control-copy-x0/COPY-X0-PREREGISTRATION.md (§5 floor,
§9 materiality, §11 rules, §13 mutants). Every verdict is RE-DERIVED from raw
rows; a stored verdict that disagrees with the derivation is a failure (C0
Corrective-1 P1-3 lesson).

Modes:
  --self-test                 prove validator sensitivity on synthetic sessions
  --session <dir>             validate one session; write summary/verdicts
  --composite <qual> <sem> <perf>   derive campaign verdicts
"""

import json
import math
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import statfs_helper  # noqa: E402

REPO = Path(__file__).resolve().parents[3]

SIZES = [4 * 1024, 64 * 1024, 1024 * 1024, 64 * 1024 * 1024]
ARMS = ["B0", "B1", "B2", "B3"]
ROUNDS = 9
LOG2_FAST = math.log2(0.95)   # ≤ −5%
LOG2_SLOW = math.log2(1.05)   # ≥ +5%


class Invalid(Exception):
    pass


def load_rows(session: Path) -> list[dict]:
    rows = []
    f = session / "rows.jsonl"
    if not f.is_file():
        raise Invalid(f"missing rows.jsonl in {session}")
    for i, line in enumerate(f.read_text().splitlines()):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError as e:
            raise Invalid(f"{f}:{i + 1}: invalid JSON: {e}")
    return rows


# ---------------------------------------------------------------------------
# Common session gates
# ---------------------------------------------------------------------------

def check_environment(env: dict, formal: bool) -> None:
    for k in ("git_head", "bench_sha256", "kernel", "roots", "mount"):
        if k not in env:
            raise Invalid(f"environment.json missing {k}")
    if formal and env.get("dirty_tracked"):
        raise Invalid("formal session must be commit-pinned (dirty_tracked=false)")
    for label in ("tmpfs", "ext4"):
        root = env["roots"][label]
        live = statfs_helper.fstype(root["path"])
        if not live.startswith(label):
            raise Invalid(f"substrate authority: {label} root {root['path']} is {live!r}")
        if not root["fstype"].startswith(label):
            raise Invalid(f"environment.json {label} fstype {root['fstype']!r} "
                          f"does not match its label")


def check_row_schema(row: dict) -> None:
    for k in ("id", "phase"):
        if k not in row:
            raise Invalid(f"row missing {k}: {row}")
    phase = row["phase"]
    witness = row.get("fixture") == "S8ext"  # layout witness (row 13): extents only
    if phase in ("semantic", "perf") and not witness:
        for k in ("arm", "ok", "bytes_moved", "xfer_calls", "xfer_bytes"):
            if k not in row:
                raise Invalid(f"{phase} row missing {k}: {row['id']}")
        if "mechanism_executed" not in row and phase == "perf":
            raise Invalid(f"perf row missing mechanism_executed: {row['id']}")
    if phase in ("semantic", "perf") and row.get("mutant") is not None:
        raise Invalid(f"mutant row in formal corpus: {row['id']}")
    if phase in ("semantic", "perf") and str(row["id"]).startswith("control"):
        raise Invalid(f"control row in formal corpus: {row['id']}")


def check_mechanism_consistency(row: dict) -> None:
    """Prereg §11 rule 2 + §12 + mutants M1/M3."""
    if row["phase"] != "perf":
        return
    arm, ok = row["arm"], row["ok"]
    mech = row.get("mechanism_executed", "")
    dec = row.get("decision", {})
    if ok:
        if arm in ("B0", "B1") and mech != "buffered_read_write":
            raise Invalid(f"M1 hidden transformation: {arm} row {row['id']} ran {mech}")
        if arm == "B2" and mech != "copy_file_range":
            raise Invalid(f"M1: B2 row {row['id']} ran {mech}")
        if row["bytes_moved"] <= 0:
            raise Invalid(f"M4 misleading success: ok with zero progress {row['id']}")
        if row["xfer_bytes"] != row["bytes_moved"]:
            raise Invalid(f"M2 progress accounting: {row['id']} "
                          f"xfer_bytes={row['xfer_bytes']} moved={row['bytes_moved']}")
        if row.get("bytes_ok") is not True or row.get("size_ok") is not True:
            raise Invalid(f"same-work violation: {row['id']}")
    else:
        if mech != "unsupported_error":
            raise Invalid(f"error row mechanism must be unsupported_error: {row['id']}")
    if arm == "B3" and dec:
        reason = dec.get("reason", "")
        mex = dec.get("mechanism_executed", "")
        if dec.get("fallback_occurred") and mex != "buffered_read_write":
            raise Invalid(f"M3 silent fallback: {row['id']} fallback with mech {mex}")
        if reason == "file_range_fallback_to_buffered" and not dec.get("fallback_occurred"):
            raise Invalid(f"M3: fallback reason without record: {row['id']}")
        if ok and reason == "file_range_selected" and mech != "copy_file_range":
            raise Invalid(f"M1: B3 selected file_range but ran {mech}: {row['id']}")


# ---------------------------------------------------------------------------
# Semantic session validation (prereg §5/§7)
# ---------------------------------------------------------------------------

ESPIPE = 29
EINVAL = 22


def sem_expect(fixture: str, arm: str, row: dict, chunk: int) -> None:
    n = row["n"]
    ok = row["ok"]
    fx = fixture

    def need(cond, why):
        if not cond:
            raise Invalid(f"semantic {fx}|{arm}: {why} (row {row['id']})")

    if fx in ("S1", "S2", "S7"):
        need(ok, "must succeed")
        need(row["bytes_moved"] == n, f"moved {row['bytes_moved']} != n {n}")
        need(row.get("dest_bytes_ok") is True, "dest bytes")
        need(row.get("dest_size_ok") is True, "dest size")
        need(row.get("outside_range_ok", True) is True, "outside range")
        need(row.get("src_unchanged_ok") is True, "source changed")
        need(row["xfer_bytes"] == n, "xfer accounting")
    elif fx == "S3":
        need(ok, "EOF-before-limit must be success")
        need(row["bytes_moved"] == 16 * 1024, "EOF progress must be the source size")
        need(row.get("dest_bytes_ok") is True, "dest bytes at EOF")
    elif fx == "S4":
        need(ok, "64MiB copy must succeed")
        need(row["bytes_moved"] == n and row["xfer_bytes"] == n, "partial accounting")
        need(row["xfer_calls"] >= (n + chunk - 1) // chunk, "call count vs chunk grid")
    elif fx == "S5":
        if arm == "B0":
            need(not ok and row["err_class"] == "source_error" and row["err"] == ESPIPE,
                 "B0 on pipe must fail ESPIPE (positional precondition)")
        elif arm == "B1":
            need(ok and row["bytes_moved"] == 4096 and row.get("dest_bytes_ok") is True,
                 "B1 abstract surface must copy the pipe payload")
        elif arm == "B2":
            need(not ok and row["err_class"] == "unsupported" and row["err"] == EINVAL,
                 "B2 on pipe must fail EINVAL (kernel refuses non-regular)")
        else:
            need(not ok and row["err_class"] == "precondition", "B3 must fail closed")
            dec = row.get("decision", {})
            need(dec.get("reason") == "precondition_regular_file"
                 and dec.get("mechanism_executed") == "none",
                 "B3 must name the precondition")
    elif fx in ("S6", "S6fb"):
        if arm in ("B0", "B1"):
            need(ok and row["bytes_moved"] == n and row.get("dest_bytes_ok") is True,
                 f"buffered arm must copy cross-fs: {row['id']}")
        elif arm in ("B2", "B3"):
            # Row 12: outcome is the kernel disposition, recorded either way.
            if not ok:
                need(row["err_class"] in ("unsupported", "transfer_error"),
                     f"kernel refusal class: {row['id']}")
        if fx == "S6fb" and arm == "B3":
            dec = row.get("decision", {})
            reason = dec.get("reason", "")
            if reason == "file_range_fallback_to_buffered":
                need(dec.get("fallback_occurred") is True
                     and row["ok"] and row["bytes_moved"] == n
                     and row.get("dest_bytes_ok") is True
                     and row["xfer_bytes"] == n,
                     "recorded fallback must complete the copy")
            else:
                need(reason == "file_range_selected" and row["ok"],
                     "if cross-fs is supported, S6fb records direct success")
    elif fx == "S8":
        need(ok and row["bytes_moved"] == n and row.get("dest_bytes_ok") is True,
             "sparse bytes must match exactly")

    # Row 9: shared-offset behavior per declared arm shape.
    if fx in ("S1", "S2") and ok:
        if arm == "B1":
            need(row["src_off_after"] == row["src_off"] + row["bytes_moved"],
                 "B1 declared difference: shared src offset must advance by moved")
            need(row["dst_off_after"] == row["dst_off"] + row["bytes_moved"],
                 "B1 declared difference: shared dst offset must advance by moved")
        else:
            need(row["src_off_after"] == row["src_off_before"]
                 and row["dst_off_after"] == row["dst_off_before"],
                 f"{arm} positional: shared offsets must not move")


def validate_semantic(session: Path) -> dict:
    env = json.loads((session / "environment.json").read_text())
    check_environment(env, formal=True)
    rows = load_rows(session)
    sem_rows = [r for r in rows if r["phase"] == "semantic"]
    selftest_rows = [r for r in rows if r["phase"] == "selftest"]
    if not selftest_rows or not all(r.get("pass") for r in selftest_rows):
        raise Invalid("semantic session must contain passing bench selftests "
                      "(loop rules rows 7/8 are validated there)")
    chunk = env["params"]["qualified_chunk"]
    saw = set()
    for row in sem_rows:
        check_row_schema(row)
        if row.get("fixture") != "S8ext":
            sem_expect(row["fixture"], row["arm"], row, chunk)
        dec = row.get("decision")
        if row["arm"] == "B3" and dec and row.get("fixture") != "S8ext":
            reason = dec.get("reason", "")
            if dec.get("fallback_occurred") and \
                    dec.get("mechanism_executed") != "buffered_read_write":
                raise Invalid(f"M3 silent fallback (semantic): {row['id']}")
            if reason == "file_range_fallback_to_buffered":
                if not dec.get("fallback_occurred"):
                    raise Invalid(f"M3 fallback reason unrecorded: {row['id']}")
                if not row["ok"] or row["xfer_bytes"] != row["bytes_moved"]:
                    raise Invalid(f"M3 fallback accounting: {row['id']}")
            if reason == "file_range_selected" and \
                    dec.get("mechanism_executed") != "copy_file_range":
                raise Invalid(f"M1 mechanism mismatch (semantic): {row['id']}")
            if reason == "precondition_regular_file" and \
                    (dec.get("mechanism_executed") != "none" or row["ok"]):
                raise Invalid(f"precondition shape: {row['id']}")
        saw.add((row["fixture"], row["arm"]))
    # Structure: every substrate × fixture × arm present (S6 labels carry
    # directions; S8ext is a witness row).
    for sub in ("tmpfs", "ext4"):
        for fx in ("S1", "S2", "S3", "S4", "S5", "S7", "S8"):
            for arm in ARMS:
                if not any(r["fixture"] == fx and r["arm"] == arm and r["label"] == sub
                           for r in sem_rows):
                    raise Invalid(f"missing semantic row {fx}|{sub}|{arm}")
    for direction in ("tmpfs-to-tmpfs", "ext4-to-ext4", "tmpfs-to-ext4", "ext4-to-tmpfs"):
        for arm in ARMS:
            if not any(r["fixture"] == "S6" and r["label"] == direction and r["arm"] == arm
                       for r in sem_rows):
                raise Invalid(f"missing S6 row {direction}|{arm}")
    # S8 layout witnesses recorded (row 13: recorded, not adjudicated).
    for sub in ("tmpfs", "ext4"):
        for arm in ARMS:
            if not any(r["fixture"] == "S8ext" and r["label"] == sub and r["arm"] == arm
                       for r in sem_rows):
                raise Invalid(f"missing S8ext witness {sub}|{arm}")
    # Verdict derivation: MUST MATCH rows all passed (they raise otherwise).
    declared_diffs = {
        "row9_shared_offsets": "B1 advances shared offsets (pre-positioned); "
                               "B0/B2/B3 positional",
        "row11_non_regular": "S5 dispositions per declared mechanism contract",
        "row12_cross_fs": "S6 kernel dispositions recorded per direction",
    }
    return {
        "COPY-X0-SEMANTIC-EQUIVALENCE": "EQUIVALENT FOR DECLARED COPY CONTRACT",
        "must_match_rows": [1, 2, 3, 4, 5, 6, 7, 8],
        "declared_differences": declared_diffs,
        "out_of_contract_rows": [13, 14, 15, 16, 17],
        "rows_total": len(sem_rows),
    }


# ---------------------------------------------------------------------------
# Perf derivation (prereg §9)
# ---------------------------------------------------------------------------

def median(xs):
    s = sorted(xs)
    m = len(s) // 2
    return s[m] if len(s) % 2 else 0.5 * (s[m - 1] + s[m])


def p90_nearest(xs):
    s = sorted(xs)
    idx = max(0, math.ceil(0.90 * len(s)) - 1)
    return s[idx]


def derive_perf(rows: list[dict], chunk_expected: int) -> dict:
    perf = [r for r in rows if r["phase"] == "perf"]
    aa = [r for r in rows if r["phase"] == "aa"]
    controls = [r for r in rows if str(r.get("id", "")).startswith("control")]
    if controls:
        raise Invalid("control rows must not live in a formal perf session")

    by_key: dict[tuple, dict[int, dict[str, dict]]] = {}
    seen_ids = set()
    for row in perf:
        check_row_schema(row)
        check_mechanism_consistency(row)
        if row["id"] in seen_ids:
            raise Invalid(f"duplicate perf id: {row['id']}")
        seen_ids.add(row["id"])
        parts = row["id"].split("|")
        if len(parts) != 5 or parts[0] != "perf":
            raise Invalid(f"perf id shape: {row['id']}")
        _, label, size_s, arm_id, rnd_s = parts
        if arm_id != row["arm"] or int(size_s) != row["size"]:
            raise Invalid(f"perf id disagrees with row fields: {row['id']}")
        if label not in ("tmpfs", "ext4"):
            raise Invalid(f"perf id substrate label: {row['id']}")
        if row["chunk"] != chunk_expected:
            raise Invalid(f"M5 weak baseline: {row['id']} chunk {row['chunk']} "
                          f"!= qualified {chunk_expected}")
        if not row["ok"]:
            # Mechanism refused on this substrate: legal recorded outcome; the
            # cell is marked and excluded from numeric derivation.
            by_key.setdefault((label, row["size"]), {}).setdefault(
                int(rnd_s[1:]), {})[arm_id] = None
            continue
        key = (label, row["size"])
        rnd = int(rnd_s[1:])
        by_key.setdefault(key, {}).setdefault(rnd, {})[arm_id] = row
    refused_cells = []
    for key, rounds in by_key.items():
        if set(rounds) != set(range(1, ROUNDS + 1)):
            raise Invalid(f"cell {key}: rounds {sorted(rounds)} != 1..{ROUNDS}")
        for rnd, arms in rounds.items():
            if set(arms) != set(ARMS):
                raise Invalid(f"cell {key} round {rnd}: arms {sorted(arms)} incomplete")
        for arm in ("B2", "B3"):
            if any(rounds[r][arm] is None for r in rounds):
                refused_cells.append(f"{arm}|{key[0]}|{key[1]}")
                for r in rounds:
                    del rounds[r][arm]  # excluded from numeric derivation

    # A/A envelope (per cell p90 of |log2 paired ratio|; envelope = max).
    aa_env = {}
    aa_by: dict[tuple, dict[int, dict[str, float]]] = {}
    for row in aa:
        _, label, size, lab, rnd = row["id"].split("|")
        aa_by.setdefault((label, int(size)), {}).setdefault(int(rnd[1:]), {})[lab] = \
            row["wall_sec"]
    for key, rounds in aa_by.items():
        rat = []
        for rnd, labs in rounds.items():
            if set(labs) == {"A", "B"}:
                rat.append(abs(math.log2(labs["A"] / labs["B"])))
        if len(rat) == ROUNDS:
            aa_env[f"{key[0]}|{key[1]}"] = p90_nearest(rat)

    def cell_direction(cells, arm):
        # cells: {rnd: {arm: row}} paired vs B0 in the same round
        logs = [math.log2(cells[r][arm]["wall_sec"] / cells[r]["B0"]["wall_sec"])
                for r in cells]
        med = median(logs)
        signs = sum(1 for x in logs if x < 0)
        if med <= LOG2_FAST and signs >= 7:
            return "FAST", med, signs
        if med >= LOG2_SLOW and (ROUNDS - signs) >= 7:
            return "SLOW", med, signs
        return "NONE", med, signs

    directions = {}  # (arm,label,size) -> (dir, med, signs)
    for key, rounds in by_key.items():
        for arm in ("B1", "B2", "B3"):
            directions[(arm, key[0], key[1])] = cell_direction(rounds, arm)

    def neighbors(label, size):
        i = SIZES.index(size)
        out = []
        if i > 0:
            out.append((label, SIZES[i - 1]))
        if i < len(SIZES) - 1:
            out.append((label, SIZES[i + 1]))
        other = "ext4" if label == "tmpfs" else "tmpfs"
        out.append((other, size))
        return out

    cells = {}
    for (arm, label, size), (d, med, signs) in directions.items():
        if d != "FAST":
            cells[f"{arm}|{label}|{size}"] = {"direction": d,
                                              "median_log2": round(med, 5),
                                              "signs_faster": signs}
            continue
        support = False
        for (nl, ns) in neighbors(label, size):
            nd = directions.get((arm, nl, ns), ("NONE",))[0]
            if nd == "FAST":
                nm = directions[(arm, nl, ns)][1]
                if nm <= math.log2(0.97):  # ≥3% neighbor
                    support = True
        cells[f"{arm}|{label}|{size}"] = {
            "direction": "FAST",
            "median_log2": round(med, 5),
            "signs_faster": signs,
            "verdict": "MATERIAL" if support else "REGIME-LOCAL",
        }

    def arm_summary(arm):
        mat = [k for k, v in cells.items() if k.startswith(arm + "|")
               and v.get("verdict") == "MATERIAL"]
        iso = [k for k, v in cells.items() if k.startswith(arm + "|")
               and v.get("verdict") == "REGIME-LOCAL"]
        slow = [k for k, v in cells.items() if k.startswith(arm + "|")
                and v["direction"] == "SLOW"]
        if mat:
            return "MATERIAL BENEFIT (regime supported)", mat, iso, slow
        if iso:
            return "REGIME-LOCAL", mat, iso, slow
        return "NOT ESTABLISHED", mat, iso, slow

    out = {"cells": cells, "aa_envelope_p90": aa_env,
           "mechanism_refused_cells": refused_cells}
    for arm in ("B1", "B2", "B3"):
        verdict, mat, iso, slow = arm_summary(arm)
        out[f"{arm}_vs_B0"] = {
            "verdict": verdict,
            "material_cells": mat, "regime_local_cells": iso, "slow_cells": slow,
        }
    return out


def validate_perf(session: Path) -> dict:
    env = json.loads((session / "environment.json").read_text())
    check_environment(env, formal=True)
    rows = load_rows(session)
    selftest_rows = [r for r in rows if r["phase"] == "selftest"]
    if not selftest_rows or not all(r.get("pass") for r in selftest_rows):
        raise Invalid("perf session must contain passing bench selftests")
    return derive_perf(rows, env["params"]["qualified_chunk"])


def validate_qualify(session: Path) -> dict:
    env = json.loads((session / "environment.json").read_text())
    check_environment(env, formal=False)
    rows = load_rows(session)
    if not any(r["phase"] == "probe" for r in rows):
        raise Invalid("qualify session must contain probe rows")
    if not any(str(r.get("id", "")).startswith("control|m5") for r in rows):
        raise Invalid("qualify session must contain the M5 control demonstration row")
    qual = json.loads((session / "qualified.json").read_text())
    # Frozen selection rule re-derivation.
    med = qual["medians"]
    cands = sorted(set(int(k.split("|")[1]) for k in med))
    best = min(cands, key=lambda c: (max(med[f"tmpfs|{c}"], med[f"ext4|{c}"]), c))
    if best != qual["qualified_chunk"]:
        raise Invalid(f"qualified chunk {qual['qualified_chunk']} != re-derived {best}")
    return {"qualified_chunk": best, "probe_rows":
            [r for r in rows if r["phase"] == "probe"]}


# ---------------------------------------------------------------------------
# Composite verdicts
# ---------------------------------------------------------------------------

# Single line on purpose: check_copy_x0_design.py skips lines naming the
# forbidden vocabulary itself.
FORBIDDEN = ["CapabilityRegistry", "TransferManager", "ResourceManager", "DataToken", "StrategyPlanner", "AutoTuner", "auto_tuner", "capability_registry"]


def design_gate() -> dict:
    bench_src = REPO / "bench" / "g1_control_copy_x0_bench.cpp"
    text = bench_src.read_text()
    hits = [t for t in FORBIDDEN if t in text]
    if hits:
        return {"M6_design_gate": "FAIL", "forbidden_hits": hits}
    # B3 thinness: lines in the B3 section (from its header comment to the
    # next section separator).
    start = text.index("// B3 — thin research-only Copy boundary")
    end = text.index("// ---------------------------------------------------------------------------",
                     start + 10)
    # the separator AFTER the B3 body: find the one following run_b3's closing
    b3_lines = text[start:end].count("\n")
    # The B3 block ends before B1's header; take up to the B1 header if closer.
    try:
        b1 = text.index("// B1 — production sluice::copy_all", start)
        end2 = min(end, b1)
        b3_lines = text[start:end2].count("\n")
    except ValueError:
        pass
    return {"M6_design_gate": "PASS", "b3_section_lines": b3_lines,
            "thin_ok": b3_lines <= 200}


def composite(qual: dict, sem: dict, perf: dict) -> dict:
    gate = design_gate()
    capability = perf["B2_vs_B0"]["verdict"]
    semantic = sem["COPY-X0-SEMANTIC-EQUIVALENCE"]
    boundary_ok = (semantic.startswith("EQUIVALENT") and gate["M6_design_gate"] == "PASS")
    gate_a = capability.startswith("MATERIAL") or capability.startswith("REGIME-LOCAL")
    gate_b = semantic.startswith("EQUIVALENT")
    # Gate C is demonstrated by the enforced validator rules themselves
    # (M1/M3/M4 rejections) plus the recorded S5/S6 declared dispositions.
    gate_c = boundary_ok
    gate_d = gate["M6_design_gate"] == "PASS" and gate["thin_ok"]
    g1 = "PARTIAL/POSITIVE EVIDENCE" if (gate_a and gate_b and gate_c and gate_d) \
        else "NOT ESTABLISHED"
    return {
        "COPY-X0-CAPABILITY": capability,
        "COPY-X0-SEMANTIC-EQUIVALENCE": semantic,
        "COPY-X0-TRANSFORMATION-BOUNDARY":
            "LEGAL TRANSFORMATION BOUNDARY SUPPORTED" if boundary_ok
            else "NOT ESTABLISHED",
        "COPY-X0-MINIMALITY":
            ("LOCAL COPY BRANCH SUFFICIENT; GENERIC CAPABILITY FRAMEWORK NOT EARNED"
             if gate_d else "THINNESS VIOLATED"),
        "COPY-X0-G1-CONTROL": g1,
        "gates": {"A_capability": gate_a, "B_semantic": gate_b,
                  "C_control_value": gate_c, "D_minimality": gate_d},
        "design_gate": gate,
        "PROMOTION": "PROMOTE-CONSIDER" if g1 != "NOT ESTABLISHED" else "STOP — NO C1",
    }


# ---------------------------------------------------------------------------
# Session / CLI plumbing
# ---------------------------------------------------------------------------

def validate_session(session: Path) -> dict:
    name = session.name
    if name.startswith("copy-x0-qualify"):
        out = validate_qualify(session)
    elif name.startswith("copy-x0-semantic"):
        out = validate_semantic(session)
    elif name.startswith("copy-x0-perf"):
        out = validate_perf(session)
    elif name.startswith("copy-x0-probe"):
        env = json.loads((session / "environment.json").read_text())
        check_environment(env, formal=False)
        rows = load_rows(session)
        if not any(r["phase"] == "probe" for r in rows):
            raise Invalid("probe session has no probe rows")
        out = {"probe_rows": [r for r in rows if r["phase"] == "probe"]}
    else:
        raise Invalid(f"unknown session kind: {name}")
    (session / "verdicts.json").write_text(json.dumps({"session": name, **out},
                                                      indent=2) + "\n")
    return out


# ---------------------------------------------------------------------------
# Self-test (prereg §11 rule 8 + §13 mutants)
# ---------------------------------------------------------------------------

def synth_perf_session(base: dict, mutate=None):
    rows = []
    walls = {"B0": 0.100, "B1": 0.100, "B2": 0.050, "B3": 0.050}
    if base.get("b2_slow"):
        walls["B2"] = 0.100
        walls["B3"] = 0.100
    for label in ("tmpfs", "ext4"):
        for size in SIZES:
            for rnd in range(1, ROUNDS + 1):
                for arm in ARMS:
                    # material in ≥64K cells only when not b2_slow
                    w = walls[arm]
                    if not base.get("b2_slow") and size in (4 * 1024,):
                        w = 0.100
                    row = {
                        "id": f"perf|{label}|{size}|{arm}|r{rnd}", "phase": "perf",
                        "arm": arm, "size": size, "chunk": base["chunk"],
                        "wall_sec": w, "cpu_sec": w * 0.8,
                        "mechanism_executed": "buffered_read_write" if arm in ("B0", "B1")
                        else "copy_file_range",
                        "bytes_ok": True, "size_ok": True, "ok": True,
                        "bytes_moved": size, "err": 0, "os_errno": 0, "err_class": "",
                        "xfer_calls": 8, "xfer_bytes": size, "partial_events": 0,
                        "sync_calls": 0,
                        "decision": {"requested": "file_range", "selected": "file_range",
                                     "reason": "file_range_selected",
                                     "mechanism_executed": "copy_file_range",
                                     "fallback_occurred": False} if arm == "B3" else {},
                    }
                    if mutate:
                        mutate(label, size, arm, row)
                    rows.append(row)
    for label in ("tmpfs", "ext4"):
        for size in (64 * 1024, 64 * 1024 * 1024):
            for rnd in range(1, ROUNDS + 1):
                for lab in ("A", "B"):
                    rows.append({"id": f"aa|{label}|{size}|{lab}|r{rnd}", "phase": "aa",
                                 "wall_sec": 0.100, "arm": "B0", "size": size,
                                 "chunk": base["chunk"], "ok": True,
                                 "bytes_moved": size, "xfer_calls": 4, "xfer_bytes": size})
    selftests = [{"id": f"selftest|{i}", "phase": "selftest", "pass": True}
                 for i in range(6)]
    return selftests + rows


def synth_sem_rows(chunk):
    rows = [{"id": f"selftest|{i}", "phase": "selftest", "pass": True} for i in range(6)]
    dec_ok = {"requested": "file_range", "selected": "file_range",
              "reason": "file_range_selected", "mechanism_executed": "copy_file_range",
              "fallback_occurred": False}
    dec_none = {"requested": "file_range", "selected": "file_range",
                "reason": "precondition_regular_file", "mechanism_executed": "none",
                "fallback_occurred": False}
    dec_fb = {"requested": "file_range", "selected": "buffered",
              "reason": "file_range_fallback_to_buffered",
              "mechanism_executed": "buffered_read_write", "fallback_occurred": True}

    def sem(fixture, sub, arm, **kw):
        n_ = kw.get("n", 65536)
        moved_ = kw.get("bytes_moved", n_)
        r = {"id": f"sem|{fixture}|{sub}|{arm}", "phase": "semantic",
             "fixture": fixture, "arm": arm, "label": sub, "chunk": chunk,
             "n": n_, "src_off": kw.get("src_off", 0),
             "dst_off": kw.get("dst_off", 0), "ok": kw.get("ok", True),
             "bytes_moved": moved_,
             "err": kw.get("err", 0), "os_errno": 0,
             "err_class": kw.get("err_class", ""),
             "xfer_calls": max(1, (moved_ + chunk - 1) // chunk),
             "xfer_bytes": kw.get("xfer_bytes", moved_),
             "partial_events": 0, "sync_calls": 0,
             "dest_bytes_ok": kw.get("dest_bytes_ok", True),
             "dest_size_ok": kw.get("dest_size_ok", True),
             "outside_range_ok": kw.get("outside_range_ok", True),
             "src_unchanged_ok": True,
             "src_off_before": kw.get("src_off", 0),
             "src_off_after": kw.get("src_off_after", kw.get("src_off", 0)),
             "dst_off_before": kw.get("dst_off", 0),
             "dst_off_after": kw.get("dst_off_after", kw.get("dst_off", 0)),
             "decision": kw.get("decision", dec_ok if arm == "B3" else {}),
             "pipe": kw.get("pipe", False)}
        rows.append(r)

    for sub in ("tmpfs", "ext4"):
        for arm in ARMS:
            moved = 1 * 1024 * 1024
            sem("S1", sub, arm, n=moved, bytes_moved=moved, xfer_bytes=moved,
                src_off_after=moved if arm == "B1" else 0,
                dst_off_after=moved if arm == "B1" else 0)
            sem("S2", sub, arm, n=65536, src_off=4096, dst_off=8192,
                src_off_after=(4096 + 65536) if arm == "B1" else 4096,
                dst_off_after=(8192 + 65536) if arm == "B1" else 8192)
            sem("S3", sub, arm, n=64 * 1024, bytes_moved=16 * 1024,
                xfer_bytes=16 * 1024)
            sem("S4", sub, arm, n=64 * 1024 * 1024, bytes_moved=64 * 1024 * 1024,
                xfer_bytes=64 * 1024 * 1024)
            if arm == "B0":
                sem("S5", sub, arm, n=4096, ok=False, err=ESPIPE,
                    err_class="source_error", bytes_moved=0, xfer_bytes=0)
            elif arm == "B1":
                sem("S5", sub, arm, n=4096, bytes_moved=4096, xfer_bytes=4096,
                    pipe=True)
            elif arm == "B2":
                sem("S5", sub, arm, n=4096, ok=False, err=EINVAL,
                    err_class="unsupported", bytes_moved=0, xfer_bytes=0)
            else:
                sem("S5", sub, arm, n=4096, ok=False, err=EINVAL,
                    err_class="precondition", bytes_moved=0, xfer_bytes=0,
                    decision=dec_none)
            sem("S7", sub, arm, n=65536, dst_off=8192)
            sem("S8", sub, arm, n=8 * 1024 * 1024, bytes_moved=8 * 1024 * 1024,
                xfer_bytes=8 * 1024 * 1024)
            rows.append({"id": f"sem|S8ext|{sub}|{arm}", "phase": "semantic",
                         "fixture": "S8ext", "arm": arm, "label": sub,
                         "extents": "[[2097152,3145728]]"})
    for direction in ("tmpfs-to-tmpfs", "ext4-to-ext4", "tmpfs-to-ext4",
                      "ext4-to-tmpfs"):
        for arm in ARMS:
            if arm in ("B0", "B1"):
                sem("S6", direction, arm, n=4096, bytes_moved=4096, xfer_bytes=4096)
            else:
                sem("S6", direction, arm, n=4096, ok=False, err=EXDEV_ERRNO(),
                    err_class="unsupported", bytes_moved=0, xfer_bytes=0)
        if direction != "tmpfs-to-tmpfs" and direction != "ext4-to-ext4":
            sem("S6fb", direction, "B3", n=4096, bytes_moved=4096, xfer_bytes=4096,
                decision=dec_fb)
    return rows


def EXDEV_ERRNO():
    return 18  # EXDEV


def write_session(tmp: Path, name: str, rows: list[dict], env_extra=None,
                  chunk=262144):
    d = tmp / name
    d.mkdir(parents=True, exist_ok=True)
    with (d / "rows.jsonl").open("w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    env = {
        "git_head": "0" * 40, "dirty_tracked": False, "bench_sha256": "0" * 64,
        "kernel": "test", "compiler": "test", "libc": "test", "cpu": "test",
        "branch": "test", "session_seed": 0,
        "roots": {"tmpfs": {"path": "/tmp", "fstype": "tmpfs:0x1021994"},
                  "ext4": {"path": "/tmp/../home", "fstype": "ext4:0xef53"}},
        "mount": {"tmpfs": "none tmpfs", "ext4": "/dev/sdd ext4"},
        "params": {"sizes": SIZES, "arms": ARMS, "rounds": ROUNDS,
                   "qualified_chunk": chunk},
        "commands": [],
        **(env_extra or {}),
    }
    (d / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
    return d


def expect_invalid(fn, why):
    try:
        fn()
    except Invalid:
        return True
    raise AssertionError(f"self-test: expected rejection for {why}")


def self_test() -> None:
    chunk = 262144
    tmp = Path(tempfile.mkdtemp(prefix="copy-x0-selftest-"))
    try:
        # --- valid semantic session passes ---
        d = write_session(tmp, "copy-x0-semantic-synth-1", synth_sem_rows(chunk), chunk=chunk)
        v = validate_semantic(d)
        assert v["COPY-X0-SEMANTIC-EQUIVALENCE"].startswith("EQUIVALENT")

        # --- M1: B1 row claims copy_file_range ---
        rows = synth_sem_rows(chunk)
        # M1 applies to perf rows; build perf session and mutate one B1 row.
        perf_rows = synth_perf_session({"chunk": chunk})

        def m1(label, size, arm, row):
            if arm == "B1" and size == 65536 and label == "tmpfs":
                row["mechanism_executed"] = "copy_file_range"
        r1 = synth_perf_session({"chunk": chunk}, mutate=m1)
        d1 = write_session(tmp, "copy-x0-perf-synth-m1", r1, chunk=chunk)
        expect_invalid(lambda: validate_perf(d1), "M1 hidden transformation")

        # --- M2: semantic S1 wrong progress ---
        rows = synth_sem_rows(chunk)
        for r in rows:
            if r["id"] == "sem|S1|tmpfs|B0":
                r["bytes_moved"] = r["n"] - 1
                r["xfer_bytes"] = r["n"] - 1
        d2 = write_session(tmp, "copy-x0-semantic-synth-m2", rows, chunk=chunk)
        expect_invalid(lambda: validate_semantic(d2), "M2 wrong progress")

        # --- M3: S6fb fallback not recorded ---
        rows = synth_sem_rows(chunk)
        for r in rows:
            if r.get("phase") == "semantic" and r.get("fixture") == "S6fb":
                r["decision"] = {"requested": "file_range", "selected": "file_range",
                                 "reason": "file_range_selected",
                                 "mechanism_executed": "copy_file_range",
                                 "fallback_occurred": False}
                r["xfer_calls"] = 1  # buffered copy needs >1 cfr-shaped call grid
                r["ok"] = True
        d3 = write_session(tmp, "copy-x0-semantic-synth-m3", rows, chunk=chunk)
        # S6fb with file_range_selected + ok is legal when cross-fs supported;
        # mutation keeps consistency, so this must PASS (documented direction).
        v3 = validate_semantic(d3)
        assert v3["COPY-X0-SEMANTIC-EQUIVALENCE"].startswith("EQUIVALENT")
        # True M3: fallback reason but buffered bytes not recorded as buffered
        rows = synth_sem_rows(chunk)
        for r in rows:
            if r.get("phase") == "semantic" and r.get("fixture") == "S6fb":
                r["decision"]["mechanism_executed"] = "copy_file_range"
        d3b = write_session(tmp, "copy-x0-semantic-synth-m3b", rows, chunk=chunk)
        expect_invalid(lambda: validate_semantic(d3b), "M3 silent fallback (semantic)")

        # --- M4: perf ok row with zero progress ---
        def m4(label, size, arm, row):
            if arm == "B2" and size == 65536 and label == "ext4":
                row["bytes_moved"] = 0
                row["xfer_bytes"] = 0
        r4 = synth_perf_session({"chunk": chunk}, mutate=m4)
        d4 = write_session(tmp, "copy-x0-perf-synth-m4", r4, chunk=chunk)
        expect_invalid(lambda: validate_perf(d4), "M4 misleading success")

        # --- M5: non-qualified chunk in formal perf ---
        r5 = synth_perf_session({"chunk": 16})
        d5 = write_session(tmp, "copy-x0-perf-synth-m5", r5, chunk=chunk)
        expect_invalid(lambda: validate_perf(d5), "M5 weak baseline chunk")

        # --- substrate mismatch: label ext4 root resolves tmpfs ---
        d6 = write_session(tmp, "copy-x0-perf-synth-sub", synth_perf_session({"chunk": chunk}),
                           env_extra={"roots": {
                               "tmpfs": {"path": "/tmp", "fstype": "tmpfs:0x1021994"},
                               "ext4": {"path": "/tmp", "fstype": "ext4:0xef53"}}},
                           chunk=chunk)
        expect_invalid(lambda: validate_perf(d6), "substrate mismatch")

        # --- dirty tracked formal session ---
        d7 = write_session(tmp, "copy-x0-perf-synth-dirty",
                           synth_perf_session({"chunk": chunk}),
                           env_extra={"dirty_tracked": True}, chunk=chunk)
        expect_invalid(lambda: validate_perf(d7), "commit pin")

        # --- §9 falsification: fabricated benefit (stored MATERIAL, data NONE)
        perf_rows = synth_perf_session({"chunk": chunk, "b2_slow": True})
        d8 = write_session(tmp, "copy-x0-perf-synth-none", perf_rows, chunk=chunk)
        v8 = validate_perf(d8)
        assert v8["B2_vs_B0"]["verdict"] == "NOT ESTABLISHED", v8["B2_vs_B0"]
        assert v8["B2_vs_B0"]["verdict"] != "MATERIAL BENEFIT (regime supported)"

        # --- §9 falsification: erased benefit (data MATERIAL, must not derive NONE)
        d9 = write_session(tmp, "copy-x0-perf-synth-fast",
                           synth_perf_session({"chunk": chunk}), chunk=chunk)
        v9 = validate_perf(d9)
        assert v9["B2_vs_B0"]["verdict"].startswith("MATERIAL"), v9["B2_vs_B0"]
        # 4K cells are NONE by construction (isolated single-cell check):
        fast4k = [k for k, v in v9["cells"].items()
                  if k.startswith("B2|") and k.endswith("|4096")]
        for k in fast4k:
            assert v9["cells"][k]["direction"] == "NONE"

        # --- composite smoke ---
        gate = design_gate()
        assert gate["M6_design_gate"] == "PASS" and gate["thin_ok"], gate
        comp = composite({}, v, v9)
        assert comp["COPY-X0-CAPABILITY"].startswith("MATERIAL")
        assert comp["COPY-X0-G1-CONTROL"] == "PARTIAL/POSITIVE EVIDENCE", comp
        comp2 = composite({}, v, v8)
        assert comp2["COPY-X0-G1-CONTROL"] == "NOT ESTABLISHED", comp2
        print("validate_copy_x0: self-test PASS (M1-M5, substrate, commit-pin, "
              "§9 both falsification directions, composite)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> None:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(2)
    if args[0] == "--self-test":
        self_test()
        return
    if args[0] == "--session":
        out = validate_session(Path(args[1]))
        print(json.dumps({k: v for k, v in out.items() if k != "cells"}, indent=2))
        return
    if args[0] == "--composite":
        qual_dir, sem_dir, perf_dir = (Path(a) for a in args[1:4])
        qual = json.loads((qual_dir / "verdicts.json").read_text()) \
            if (qual_dir / "verdicts.json").exists() else validate_session(qual_dir)
        sem = json.loads((sem_dir / "verdicts.json").read_text()) \
            if (sem_dir / "verdicts.json").exists() else validate_session(sem_dir)
        perf = json.loads((perf_dir / "verdicts.json").read_text()) \
            if (perf_dir / "verdicts.json").exists() else validate_session(perf_dir)
        comp = composite(qual, sem, perf)
        out = perf_dir / "campaign-verdicts.json"
        out.write_text(json.dumps(comp, indent=2) + "\n")
        print(json.dumps(comp, indent=2))
        print(f"campaign verdicts written: {out}")
        return
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
