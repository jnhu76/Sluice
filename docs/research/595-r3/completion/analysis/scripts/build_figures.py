#!/usr/bin/env python3
"""#595 R3-completion paper figures from analysis/recomputed.json (frozen raw only).

Conventions:
  cpu / latency panels: relative change (candidate - baseline)/baseline in %,
      NEGATIVE = improvement (less CPU / lower latency). Zero line drawn.
  throughput panels: same formula, POSITIVE = improvement.
  Error bars: bootstrap 95% percentile CI over 24 pairs (seed 5950, 10k resamples).
Colors (Okabe-Ito, colorblind-safe, grayscale-distinguishable by marker/hatch):
  A0 blue #0072B2, A1 orange #E69F00, B green #009E73, B_R2 magenta #CC79A7,
  NOT_RUN gray #BBBBBB.
"""
import csv
import json
import os

os.environ.setdefault("SOURCE_DATE_EPOCH", "1762400000")  # fixed epoch: byte-reproducible SVG

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch, Rectangle

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
ANA = os.path.join(ROOT, "analysis")
FIG = os.path.join(ROOT, "figures")
FD = os.path.join(FIG, "figure-data")

C_A0, C_A1, C_B, C_BR2, C_GRAY = "#0072B2", "#E69F00", "#009E73", "#CC79A7", "#BBBBBB"
C_PASS, C_REGR, C_INCON, C_NOTR = "#009E73", "#D55E00", "#E69F00", "#999999"

plt.rcParams.update({
    "font.size": 8, "axes.titlesize": 8, "axes.labelsize": 8,
    "xtick.labelsize": 8, "ytick.labelsize": 8, "legend.fontsize": 7,
    "axes.grid": True, "grid.alpha": 0.25, "grid.linewidth": 0.4,
    "axes.spines.top": False, "axes.spines.right": False,
    "figure.facecolor": "white", "savefig.facecolor": "white",
    "pdf.fonttype": 42,
    "svg.hashsalt": "sluice-595-r3c",
})

R = json.load(open(os.path.join(ANA, "recomputed.json")))


def blk(name):
    return R["paired_blocks"][name]


def effect(block, metric):
    """Return est/lo/hi in worse-positive orientation (tput = loss)."""
    m = blk(block)["metrics"][metric]
    return m["mean"] * 100, m["ci_lo"] * 100, m["ci_hi"] * 100


def user_ori(block, metric):
    """User orientation: cpu/lat est as-is (neg=improve); tput negated (pos=improve)."""
    est, lo, hi = effect(block, metric)
    if metric == "tput":
        return -est, -hi, -lo
    return est, lo, hi


def save(fig, name):
    fig.savefig(os.path.join(FIG, f"{name}.svg"), metadata={})
    fig.savefig(os.path.join(FIG, f"{name}.png"), dpi=220, metadata={})
    plt.close(fig)
    print("wrote", name)


def write_fd(name, header, rows):
    with open(os.path.join(FD, f"{name}.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


# ---------------------------------------------------------------- F2
def fig_f2():
    fig, (axa, axb) = plt.subplots(1, 2, figsize=(6.9, 2.7),
                                   gridspec_kw={"width_ratios": [1.55, 1.0]})
    # (a) D2 single-shot: old B_R2 vs A_R2 (matched SHA), new B NOT_RUN
    metrics = [("cpu", "CPU / op"), ("tput", "throughput"), ("p50", "p50"), ("p95", "p95"), ("p99", "p99")]
    xs, labels = [], []
    rows = []
    for i, (m, lab) in enumerate(metrics):
        est, lo, hi = user_ori("LEGACY-W02-D2 (R2-D2)", m)
        x = i
        thr = -3.0 if m == "tput" else 5.0
        axa.errorbar(x, est, yerr=[[est - lo], [hi - est]], fmt="o", color=C_BR2,
                     capsize=2.5, lw=1.1, ms=4.5, zorder=3)
        axa.plot([x - 0.32, x + 0.32], [thr, thr], ls=":", lw=1.0, color="0.35", zorder=2)
        fail = (m == "tput" and lo < -3.0) or (m != "tput" and hi > 5.0)
        axa.annotate("FAIL" if fail else "pass", (x, hi), textcoords="offset points",
                     xytext=(0, 4), ha="center", fontsize=7,
                     color=C_REGR if fail else "#444444")
        xs.append(x); labels.append(lab)
        rows.append([lab, f"{est:+.3f}", f"{lo:+.3f}", f"{hi:+.3f}", thr, "FAIL" if fail else "pass"])
    axa.axhline(0, color="black", lw=0.8, zorder=1)
    axa.set_xticks(xs, labels)
    axa.set_ylabel("relative change  (%)")
    axa.set_title("(a) W-02 D2 single refill:  $B_{R2}-A_{R2}$  (old defer policy)", loc="left")
    axa.set_xlim(-0.6, len(metrics) - 0.4)
    axa.annotate("negative = better (CPU, latency)\npositive = better (throughput)",
                 xy=(0.02, 0.03), xycoords="axes fraction", fontsize=6.5, color="0.3")
    axa.annotate("R3 ready-aware B in D2: NOT RUN\n(R3 tree has no W-02 research arm)",
                 xy=(0.98, 0.05), xycoords="axes fraction", ha="right", fontsize=7,
                 color=C_NOTR, bbox=dict(fc="white", ec=C_NOTR, lw=0.6, pad=2.0))
    write_fd("F2a_d2_single_refill_effects",
             ["metric", "est_pct", "ci95_lo_pct", "ci95_hi_pct", "threshold_pct", "bound_check"],
             rows)
    rows = [["A0", 6, 6, 1.0], ["A1", 6, 6, 1.0], ["B", 3, 6, 2.0]]
    write_fd("F2b_d2_two_ready_positive_control",
             ["arm", "pure_submit_enters", "data_ops", "ops_per_enter"], rows)
    # (b) D2 two-ready positive control (mechanism window)
    arms = ["A0", "A1", "B"]
    enters = [6, 6, 3]
    spe = [1.0, 1.0, 2.0]
    x = range(3)
    axb.bar(x, enters, width=0.55, color=[C_A0, C_A1, C_B], edgecolor="black", lw=0.5)
    for xi, (e, s) in enumerate(zip(enters, spe)):
        axb.annotate(f"{e} enters\n{s:.0f} ops/enter", (xi, e), textcoords="offset points",
                     xytext=(0, 3), ha="center", fontsize=7)
    axb.set_xticks(list(x), arms)
    axb.set_ylabel("pure-submit io_uring_enter calls")
    axb.set_title("(b) D2 two reads ready: enters (diagnostic)", loc="left")
    axb.set_ylim(0, 8.2)
    axb.annotate("1 invocation x 3 phases;\nB batches both reads (to_submit=2)",
                 xy=(0.98, 0.86), xycoords="axes fraction", ha="right", fontsize=6.5, color="0.3")
    fig.tight_layout()
    save(fig, "F2_d2_regression")


# ---------------------------------------------------------------- F3
def fig_f3():
    fig, axes = plt.subplots(2, 2, figsize=(6.9, 4.9))
    axa, axb = axes[0]
    axc, axd = axes[1]
    mech = R["mechanism"]

    # (a) pure-submit enters, d16 (3 phases x 20000 ops per run)
    vals = [mech["enter"][f"d16-{a}"]["pure_submit"] for a in ("A0", "A1", "B")]
    rows = [[a, mech["enter"][f"d16-{a}"]["pure_submit"]] for a in ("A0", "A1", "B")]
    write_fd("F3a_pure_submit_enters_d16", ["arm", "pure_submit_enters"], rows)
    axa.bar(range(3), vals, width=0.55, color=[C_A0, C_A1, C_B], edgecolor="black", lw=0.5)
    axa.axhline(30000, ls="--", lw=0.9, color=C_NOTR)
    axa.legend([plt.Line2D([0], [0], ls="--", lw=0.9, color=C_NOTR)],
               ["50% existence threshold (30,000)"], loc="upper right", frameon=False,
               fontsize=6.5)
    axa.annotate("29,225  (-51.3%)", (2, vals[2]), textcoords="offset points",
                 xytext=(0, 4), ha="center", fontsize=7, color=C_B, fontweight="bold")
    for xi, v in enumerate(vals[:2]):
        axa.annotate(f"{v:,}", (xi, v), textcoords="offset points", xytext=(0, 3),
                     ha="center", fontsize=6.5)
    axa.set_xticks(range(3), ["A0", "A1", "B"])
    axa.set_ylabel("io_uring_enter (pure submit)")
    axa.set_title("(a) submit enters, W-03 d16", loc="left")
    axa.set_ylim(0, 72000)

    # (b) batch-size distribution (weighted by enter count)
    hist = mech["enter"]["d16-B"]["to_submit_hist"]
    sizes = sorted(int(k) for k in hist)
    counts = [hist[str(s)] if str(s) in hist else hist[s] for s in sizes]
    rows = [["B", s, counts[i]] for i, s in enumerate(sizes)]
    rows += [["A0", 1, 60000], ["A1", 1, 60000]]
    write_fd("F3b_batch_size_histogram", ["arm", "to_submit", "enter_count"], rows)
    axb.bar([s - 0.18 for s in sizes], counts, width=0.32, color=C_B, edgecolor="black",
            lw=0.5, label="B")
    axb.bar([0.18], [60000], width=0.32, color=C_A0, edgecolor="black", lw=0.5, label="A0 = A1")
    axb.set_yscale("log")
    axb.set_ylim(0.7, 2.2e5)
    axb.set_xticks(sorted(set(sizes + [1])), [str(s) for s in sorted(set(sizes + [1]))])
    axb.set_xlabel("reads per io_uring_enter (to_submit)")
    axb.set_ylabel("enters (log scale)")
    axb.set_title("(b) batch size per enter", loc="left")
    axb.legend(loc="upper right", frameon=False)
    for s, c in zip(sizes, counts):
        if c > 0:
            axb.annotate(f"{c:,}" if c >= 100 else str(c), (s - 0.18, c),
                         textcoords="offset points", xytext=(0, 3), ha="center", fontsize=6)

    # (c) cap/suffix accounting for B (timed phase, 20000 ops)
    rows = [["group-prefix ops (1355 groups x 8)", 10840], ["single/suffix ops", 9160],
            ["group enters", 1355], ["single enters (span=1)", 9160]]
    write_fd("F3c_cap_suffix_accounting", ["component", "count"], rows)
    axc.bar([0], [10840], width=0.5, color=C_B, edgecolor="black", lw=0.5, label="group prefix (cap=8)")
    axc.bar([0], [9160], bottom=[10840], width=0.5, color="white", edgecolor="black",
            lw=0.5, hatch="////", label="single + suffix (span=1)")
    axc.bar([1], [1355], width=0.5, color=C_B, edgecolor="black", lw=0.5)
    axc.bar([1], [9160], bottom=[1355], width=0.5, color="white", edgecolor="black",
            lw=0.5, hatch="////")
    axc.annotate("20,000 ops", (0, 10840 + 9160), textcoords="offset points", xytext=(0, 4),
                 ha="center", fontsize=7)
    axc.annotate("10,515 enters\n(= flushes, no defer)", (1, 1355 + 9160),
                 textcoords="offset points", xytext=(0, 4), ha="center", fontsize=7)
    axc.set_xticks([0, 1], ["ops", "enters"])
    axc.set_ylabel("count (ops, resp. enters)")
    axc.set_title("(c) cap=8 + suffix accounting (B, timed phase)", loc="left")
    axc.set_ylim(0, 23500)
    axc.legend(loc="upper right", frameon=False, fontsize=6.5)

    # (d) instrumented PMU (diagnostic only)
    evs = ["cycles", "instructions"]
    rows = []
    for a in ("A0", "A1", "B"):
        for e in evs:
            rows.append([a, e, mech["pmu"][f"d16-{a}"].get(e, 0)])
    write_fd("F3d_pmu_diagnostic", ["arm", "event", "count"], rows)
    x = [0, 1]
    w = 0.26
    for i, (a, c) in enumerate(zip(("A0", "A1", "B"), (C_A0, C_A1, C_B))):
        v = [mech["pmu"][f"d16-{a}"][e] / 1e6 for e in evs]
        axd.bar([xi + (i - 1) * w for xi in x], v, width=w, color=c, edgecolor="black",
                lw=0.5, label=a)
    axd.set_xticks(x, ["cycles", "instructions"])
    axd.set_ylabel("count (millions)")
    axd.set_title("(d) PMU counts, d16 (diagnostic)", loc="left")
    axd.set_ylim(0, 1220)
    axd.legend(loc="upper right", frameon=False, ncols=3, columnspacing=0.9,
               borderaxespad=0.2)
    axd.annotate("B cycles -14.1% vs A0\nsingle instrumented run, NOT gate-grade",
                 xy=(0.02, 0.99), xycoords="axes fraction", fontsize=6.5, color="0.3",
                 va="top")
    for fig_ax in (axa, axb, axc, axd):
        fig_ax.set_axisbelow(True)
    fig.tight_layout()
    save(fig, "F3_batch_mechanism")


# ---------------------------------------------------------------- F4
def fig_f4():
    fig, ax = plt.subplots(figsize=(6.9, 2.9))
    panels = [
        ("LEGACY-W02-D2 (R2-D2)", "W-02 D2\n$B_{R2}-A_{R2}$", C_BR2, "o"),
        ("PROT-W02-D3 (R2)", "W-02 D3\n$B_{R2}-A_{R2}$", C_BR2, "s"),
        ("PROT-W02-D16 (R2)", "W-02 D16\n$B_{R2}-A_{R2}$", C_BR2, "D"),
        ("AA-W03-D16", "A/A W-03 d16\n(A0 vs A0)", "#888888", "P"),
        ("AA-W02-D2", "A/A W-02 D2\n($A_{R2}$ vs $A_{R2}$)", "#888888", "X"),
    ]
    rows = []
    for bi, (name, lab, col, mk) in enumerate(panels):
        for mi, m in enumerate(("p50", "p95", "p99")):
            est, lo, hi = user_ori(name, m)
            x = mi + (bi - 2) * 0.13
            ax.errorbar(x, est, yerr=[[est - lo], [hi - est]], fmt=mk, color=col,
                        capsize=2.0, lw=0.9, ms=3.6, zorder=3,
                        label=lab.replace("\n", " ") if mi == 0 else None)
            rows.append([lab.replace("\n", " "), m, f"{est:+.3f}", f"{lo:+.3f}", f"{hi:+.3f}"])
    ax.axhline(0, color="black", lw=0.8)
    ax.axhline(5, ls="--", lw=0.9, color=C_REGR)
    ax.axhline(-5, ls="--", lw=0.9, color=C_REGR)
    ax.annotate("non-inferiority bound  +5%", (-0.52, 5.8), fontsize=6.5,
                color=C_REGR, ha="left")
    ax.annotate("reference  -5%", (-0.52, -5.8), fontsize=6, color="0.4", ha="left",
                va="top")
    ax.set_xticks([0, 1, 2], ["p50", "p95", "p99"])
    ax.set_xlim(-0.55, 2.62)
    ax.set_ylim(-40, 11.5)
    ax.set_ylabel("relative latency change  (%)\ncandidate vs baseline, negative = lower")
    ax.set_title("Tail-latency guardrails: measured blocks only (95% CI, 24 pairs each)", loc="left")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.10), frameon=False, ncols=3,
              fontsize=6.5, columnspacing=1.0)
    ax.annotate("NOT RUN (R3 B side, blocked): D1, 64 KiB d16, threadpool, SMT/cross-physical, saturation",
                xy=(0.5, -0.34), xycoords="axes fraction", ha="center", fontsize=7, color=C_NOTR)
    write_fd("F4_tail_effects", ["block", "metric", "est_pct", "ci95_lo_pct", "ci95_hi_pct"], rows)
    fig.subplots_adjust(bottom=0.32, top=0.88, left=0.13, right=0.98)
    save(fig, "F4_tail_guardrails")


# ---------------------------------------------------------------- F5
def fig_f5():
    fig, ax = plt.subplots(figsize=(6.9, 3.5))
    ax.axis("off")
    cols = ["CPU", "tput", "p95", "p99", "mech / res"]
    rows = [
        ("W-03 d16 (4 KiB) main gate   B-A0", "N", "N", "N", "N", "D"),
        ("W-03 d4 / d64  B-A0", "N", "N", "N", "N", "N"),
        ("W-03 64 KiB d16  B-A0", "N", "N", "N", "N", "N"),
        ("W-03 threadpool d4/d16/d64  B-A0", "N", "N", "N", "N", "N"),
        ("W-03 placement (SMT 2,12 / xphys 2,8)", "N", "N", "N", "N", "N"),
        ("W-02 D2 single refill  old $B_{R2}$", "P", "R", "R", "R", ""),
        ("W-02 D3  old $B_{R2}$", "P", "P", "P", "P", ""),
        ("W-02 D16  old $B_{R2}$", "P", "P", "P", "P", ""),
        ("W-02 D2 single refill  new B", "N", "N", "N", "N", ""),
        ("D2 two-ready positive (mech)", "", "", "", "", "D"),
        ("D2 serial negative (mech)", "", "", "", "", "D"),
        ("W-02 saturation  B", "N", "N", "N", "N", "N"),
        ("resource budget  B vs A0", "", "", "", "", "I"),
    ]
    stat = {"P": (C_PASS, "PASS", ""), "R": (C_REGR, "REGR", ""),
            "I": (C_INCON, "INCON", ""), "N": (C_NOTR, "N/R", ""),
            "D": ("#56B4E9", "DIAG", "///")}
    ncol, nrow = len(cols) + 1, len(rows) + 1
    xs = [0.42, 0.545, 0.635, 0.725, 0.99]
    ax.text(0.005, 0.985, "unit (measured shape)", fontsize=7.5, fontweight="bold", va="top")
    for ci, c in enumerate(cols):
        ax.text(xs[ci] if ci < 4 else xs[4], 0.985, c, fontsize=7.5, fontweight="bold",
                ha="center" if ci < 4 else "right", va="top")
    for ri, (lab, *cells) in enumerate(rows):
        y = 0.985 - (ri + 1) * 0.068
        ax.text(0.005, y, lab, fontsize=7, va="center")
        for ci, s in enumerate(cells):
            if not s:
                continue
            fc, txt, hat = stat[s]
            cx = xs[ci] if ci < 4 else (xs[4] - 0.06)
            ax.add_patch(Rectangle((cx - (0.038 if ci < 4 else 0.10), y - 0.024),
                                   0.076 if ci < 4 else 0.185, 0.050,
                                   facecolor=fc, edgecolor="black", lw=0.4,
                                   hatch=hat, alpha=0.9, transform=ax.transAxes))
            ax.text(cx, y, txt, fontsize=6 if ci < 4 else 6, ha="center", va="center")
    handles = [Patch(facecolor=C_PASS, edgecolor="black", lw=0.4, label="PASS (pre-registered bound met)"),
               Patch(facecolor=C_REGR, edgecolor="black", lw=0.4, label="REGR = regression (bound exceeded)"),
               Patch(facecolor=C_INCON, edgecolor="black", lw=0.4, label="INCON = inconclusive (diagnostics only)"),
               Patch(facecolor=C_NOTR, edgecolor="black", lw=0.4, label="N/R = not run (blocked / no arm)"),
               Patch(facecolor="#56B4E9", edgecolor="black", lw=0.4, hatch="///",
                     label="DIAG = diagnostic-only evidence (instrumented, not a gate)")]
    ax.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, -0.16), ncols=2,
              frameon=False, fontsize=6.5)
    rows_csv = [[r[0]] + [stat[s][1] if s else "" for s in r[1:]] for r in rows]
    write_fd("F5_observed_regimes_matrix",
             ["unit", "cpu", "tput", "p95", "p99", "mechanism_or_resources"], rows_csv)
    ax.set_title("Observed regimes only - no interpolation to untested shapes.\n"
                 "Product (#604) adjudicated separately: HOLD_NO_REAL_APPLICATION.",
                 fontsize=7.5, loc="left", va="top")
    save(fig, "F5_observed_regimes")


def main():
    os.makedirs(FD, exist_ok=True)
    fig_f2()
    fig_f3()
    fig_f4()
    fig_f5()


if __name__ == "__main__":
    main()
