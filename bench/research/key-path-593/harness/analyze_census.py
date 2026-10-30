#!/usr/bin/env python3
"""#593 Phase C census analysis: per-op mechanism ratios from observe dumps."""
import json
import os
import sys

CENSUS = "/home/jnhu/sluice-593-research/evidence/attribution/census"


def total_ops(jsonl_path):
    ops = 0
    records = 0
    for line in open(jsonl_path):
        r = json.loads(line)
        if r.get("case_id", "").startswith("CAL"):
            continue
        records += 1
        ops += 3 * r["request_count"]  # throughput + byte-oracle + latency passes
    return ops, records


def load_obs(path):
    d = {}
    for line in open(path):
        if line.startswith("OBS "):
            _, name, val = line.split()
            d[name] = int(val)
    return d


def main():
    rows = []
    for f in sorted(os.listdir(CENSUS)):
        if not f.endswith(".obs"):
            continue
        tag = f[:-4]
        ops, records = total_ops(os.path.join(CENSUS, tag + ".jsonl"))
        d = load_obs(os.path.join(CENSUS, f))
        rows.append((tag, ops, records, d))

    def ratio(tag, key, d, ops):
        return d.get(key, 0) / ops

    keys = [
        "cqe_total", "handle_one_cqe", "control_cqe", "finalize_terminal",
        "core_offer_terminal", "core_release_execution", "pub_pending_push",
        "pub_pending_pop", "publish_one", "core_begin_publication",
        "core_complete_publication", "deliver_event", "core_claim_observer",
        "core_retire_observer", "request_ready_check", "core_consume",
        "ctx_submit_read", "ctx_access_lock", "backend_validate", "core_reserve",
        "core_accept", "prepared_write", "delivery_init_lock",
        "dispatch_after_accept", "dispatch_push", "dispatch_one_locked",
        "core_claim_execution", "dispatch_remove_exact", "ledger_append",
        "ledger_pop", "backend_poll_progress", "run_pass", "pass_productive",
        "pass_empty", "wait_one", "drive_admit", "ps_snapshot", "ps_wait_call",
        "ps_park_poll", "ps_drain_read", "ps_signal", "backend_signal",
        "probe_immediate", "pass_immediate_remains", "pass_retry_remains",
        "core_outstanding_summary", "core_health_failed", "ps_wait_health",
        "service_controls", "service_control_scans", "ps_observe_control",
        "peek_batch_calls", "cq_ready_bound_sum", "batch_got_sum", "cqe_seen",
        "get_events", "submit_enter", "submit_sq_ready_sum", "submit_rc_sum",
        "router_lookup", "router_probe_sum", "reconcile_calls",
    ]
    print(f'{"counter":26s} ' + " ".join(f"{t.replace('w02_uring_','u').replace('w03_uring_','U').replace('w02_threadpool_','tp').replace('w02_','x'):>10s}" for t, _, _, _ in rows))
    for k in keys:
        vals = []
        for tag, ops, recs, d in rows:
            vals.append(d.get(k, 0) / ops)
        print(f"{k:26s} " + " ".join(f"{v:10.4f}" for v in vals))
    print()
    print(f'{"ops (denominator)":26s} ' + " ".join(f"{ops:>10d}" for _, ops, _, _ in rows))
    print(f'{"records":26s} ' + " ".join(f"{recs:>10d}" for _, _, recs, _ in rows))
    # derived K4 views
    print()
    print("derived (per-op where noted):")
    for tag, ops, recs, d in rows:
        cqe = d["cqe_total"]
        print(f"  {tag}: ops={ops} CQE/op={cqe/ops:.3f} "
              f"CQEs/peek_batch={d['cqe_total']/max(1,d['peek_batch_calls']):.2f} "
              f"enters/op={d['submit_enter']/ops:.3f} "
              f"SQE/enter={d['submit_rc_sum']/max(1,d['submit_rc_positive']):.2f} "
              f"sq_ready_at_submit={d['submit_sq_ready_sum']/max(1,d['submit_enter']):.2f} "
              f"passes/op={d['run_pass']/ops:.4f} "
              f"productive%={100*d['pass_productive']/max(1,d['run_pass']):.1f} "
              f"parks/op={d['ps_park_poll']/ops:.4f} "
              f"locks/CQE(dispatch_hot)={(d['handle_one_cqe']+d['pub_pending_pop']+d['pub_pending_push'])/max(1,cqe):.2f} "
              f"core_mtx/op={(d['core_reserve']+d['core_accept']+d['core_claim_execution']+d['core_offer_terminal']+d['core_release_execution']+d['core_begin_publication']+d['core_complete_publication']+d['core_claim_observer']+d['core_retire_observer']+d['core_consume'])/ops:.2f} "
              f"router_probes/lookup={d['router_probe_sum']/max(1,d['router_lookup']):.2f}")


if __name__ == "__main__":
    main()
