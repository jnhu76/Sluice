#!/usr/bin/env python3
"""rx1_failure_cases — failure-case analysis for RX-1 (task brief § 37).

For every formal run where C and E disagree (or both are wrong), dump the
decision-relevant features and the fired-rule diagnostics so each case can be
categorized as one of:

    TRUE INFORMATION GAIN
    EXTERNAL SIGNAL ALREADY SUFFICIENT
    SAMPLING MISS
    MULTI-CAUSE / UNKNOWN
    ENVIRONMENT INVALID
    CLASSIFIER DESIGN FAILURE

Categorization itself is human review work; this tool lines the cases up.
"""

from __future__ import annotations

import glob
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RX1 = REPO / "research" / "rx1"
sys.path.insert(0, str(RX1 / "scripts"))
import rx1_classify as rc  # noqa: E402


def main():
    arts = []
    for p in sorted((RX1 / "results" / "formal").glob("run_*.scored.json")):
        arts.append(json.loads(p.read_text()))
    attr = [a for a in arts if a.get("ground_truth_valid") and not a.get("tax")]
    print(f"formal scored runs: {len(arts)}; valid attribution runs: {len(attr)}\n")
    shown = 0
    for a in attr:
        t = a["ground_truth_label"]
        c, e = a["classifier_C_prediction"], a["classifier_E_prediction"]
        if c == e == t:
            continue
        shown += 1
        f = a["features"]
        w = a["workload"]
        fired = rc.fired_rules(f)
        print(f"--- run {a['run_index']:04d}  truth={t}")
        print(f"    shape: {w['op']}/{w['request_size']}B depth={w['pipeline_depth']} "
              f"cap={w['request_capacity']} workers={w['configured_workers']}")
        print(f"    C={c}  E={e}")
        print(f"    thr={f['throughput_mbs']:.0f}MB/s cores={f['cpu_cores_used']:.2f} "
              f"pwu={f['per_worker_util']:.2f} psi={f['psi_cpu_some_us_per_s']:.0f} "
              f"rej_app={f['app_submit_rejections']:.0f}")
        print(f"    slot_occ={f['sluice_slot_occ_mean']:.3f} frac_cap={f['sluice_frac_slot_at_capacity']:.2f} "
              f"frac_act={f['sluice_frac_active_at_configured']:.2f} d_mean={f['sluice_dispatch_occ_mean']:.1f} "
              f"rej_delta={f['sluice_rejections_delta']:.0f} n_samp={f['sluice_sample_count']:.0f}")
        print(f"    fired: {json.dumps(fired)}")
    if shown == 0:
        print("no disagreements or errors among valid runs")


if __name__ == "__main__":
    main()
