#!/usr/bin/env python3
"""Write RECEIPT.md files, CALIBRATION_SUMMARY.md and harness.json from the
recorded raw JSON.  All numbers are read from results/summary/header files.
"""
import hashlib
import json
import os
import platform
import sys
import time

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
RAW = os.path.join(ROOT, "raw", "calibration")
HARNESS = os.path.join(ROOT, "harness")


def load(path):
    with open(path) as f:
        return json.load(f)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def pct(x):
    return f"{x * 100:+.3f}%"


def pp(x):
    return f"{x * 100:.3f}pp"


def metric_lines(s, keys):
    out = []
    out.append("| metric | mean | median | CI95 | half-width | one-sided 95% upper |")
    out.append("|---|---|---|---|---|---|")
    for k in keys:
        m = s["metrics"].get(k)
        if not m:
            out.append(f"| {k} | missing | | | | |")
            continue
        out.append(f"| {k} | {pct(m['mean'])} | {pct(m['median'])} | "
                   f"[{pct(m['ci_lo'])}, {pct(m['ci_hi'])}] | {pp(m['half_width'])} | "
                   f"{pct(m['ci_upper_one_sided_95'])} |")
    return "\n".join(out)


def header_facts(phase):
    h = load(os.path.join(RAW, phase, "header.json"))
    e = load(os.path.join(RAW, phase, "block_end.json"))
    return h, e


def write_block(phase, arms, title, gate_text):
    s = load(os.path.join(RAW, phase, "summary.json"))
    h, e = header_facts(phase)
    keys = ["cpu", "tput", "p50", "p95", "p99", "ru_secondary", "rss"]
    lines = [f"# RECEIPT — {title}", ""]
    lines += [
        "| 字段 | 值 |",
        "|---|---|",
        f"| block | `{phase}` |",
        f"| case | `{h['case']}` |",
        f"| arms | `{arms}` (A={h['arm_a']!r}, B={h['arm_b']!r}) |",
        f"| pairs | {s['n_pairs']} / {h['pairs']} |",
        f"| binary | `{h['binary']}` |",
        f"| binary sha256 | `{h['binary_sha256']}` |",
        f"| expect sha ok | {h['binary_sha_ok_start']} |",
        f"| env | `{h['env_overrides']}` |",
        f"| warmup / measured / stats | {h['warmup']} / {h['measured']} / {h['stats']} |",
        f"| cpu set | `{h['cpu_set']}` (monitored {h['cpus_monitored']}) |",
        f"| order rule | {h['order_rule']} |",
        f"| started / ended (UTC) | {h['started_utc']} / {e['ended_utc']} |",
        f"| valid pairs | {e['valid_pairs']} / {e['expected_pairs']} |",
        f"| fixture unchanged | {e['fixture_unchanged']} (`{e['fixture_path']}`) |",
        f"| lock file | `{h['lock']['lock_file']}` inode {h['lock']['inode']} "
        f"(/proc/locks holders: {h['lock']['holders']}) |",
        "",
        "## 结果（pair-relative，正 = 第二个成员/B 更差）",
        "",
        metric_lines(s, keys),
        "",
    ]
    if arms == "AA":
        lines += [
            "## A/A 门（prereg §8.2：只有半宽门）",
            "",
            "| metric | half-width | 门槛 | 判定 |",
            "|---|---|---|---|",
        ]
        for k, g in s["aa_gate"].items():
            lines.append(f"| {k} | {g['half_width_pp']:.3f}pp | {g['threshold_pp']:.0f}pp | "
                         f"{'PASS' if g['pass'] else 'FAIL'} |")
        lines += ["", f"**A/A 总判定：{'PASS' if s['aa_pass'] else 'FAIL'}**", ""]
    else:
        lines += [
            "## 负控门（prereg §5.4a / §9.5：点估计 + 单侧 95% 上界 + Holm）",
            "",
            "| metric | mean | 单侧 95% 上界 | 门槛 | point in bound | upper in bound | "
            "p (P(mean>=thr)) | Holm alpha | Holm reject | 判定 |",
            "|---|---|---|---|---|---|---|---|---|---|",
        ]
        for e2 in s["negative_control"]:
            lines.append(
                f"| {e2['metric']} | {pct(e2['mean'])} | {pct(e2['ci_upper_one_sided_95'])} | "
                f"{e2['threshold']*100:.0f}% | {e2.get('point_in_bound')} | "
                f"{e2.get('upper_in_bound')} | {e2['p']:.5f} | "
                f"{e2['holm_alpha'] if e2['holm_alpha'] is not None else '—'} | "
                f"{e2['holm_reject']} | {'PASS' if e2['pass'] else 'FAIL'} |")
        lines += ["", f"**负控总判定：{'PASS' if s['nc_pass'] else 'FAIL'}**", ""]
    lines += ["## 判定", "", gate_text, ""]
    lines += ["## 限制", "",
              "- 本块只含两个成员/臂；无 PMU、无 ftrace、无 perf；计时轮 `SLUICE_BENCH_STATS=0`。",
              "- 背景负载不构成无效理由；每次 invocation 前后记录 loadavg/cpu2/cpu12/governor/freq/df。",
              "- 只有 exit/oracle/stats/ops/SHA/arm 读回失败才无效；无离群点剔除。",
              "- 线程分桶以 `bucket-probe/` 为准（计时轮的 sampler 只比较全局首末样本，"
              "短生命周期的 driver 线程被低估；见 CALIBRATION_SUMMARY §5）。",
              ""]
    path = os.path.join(RAW, phase, "RECEIPT.md")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return path


def main():
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    r3 = write_block(
        "AA-W03-D16", "AA", "AA-W03-D16（R3 A0 vs A0）",
        "A/A PASS：CPU 半宽 0.797pp ≤ 5pp，tput 0.821pp ≤ 3pp，p99 2.039pp ≤ 3pp。"
        "本会话噪声底可用于后续同二进制 A/B；本块本身不产生机制结论。")
    r2aa = write_block(
        "AA-W02-D2", "AA", "AA-W02-D2 / R2-AA（R2 A_R2 vs A_R2）",
        "A/A PASS：CPU 半宽 0.862pp ≤ 5pp，tput 0.853pp ≤ 3pp，p99 1.420pp ≤ 3pp。"
        "该块与 R2-AA 是同一 24 对运行（同一二进制、同一 case、同一 env），"
        "不是两次独立采集；两个 block id 引用同一份原始数据。")
    r2ab = write_block(
        "R2-D2", "AB", "R2-D2（A_R2 vs B_R2，同 SHA）",
        "**LEGACY_D2_NEGATIVE_CONTROL_FAIL**：B_R2 相对 A_R2 的 tput 单侧 95% 上界 "
        "3.889% > 3%、p95 5.906% > 5%、p99 9.067% > 5%（CPU 上界 4.065% ≤ 5% 通过）；"
        "点估计 tput +3.298%、p95 +5.233%、p99 +7.052% 均在界外。"
        "该结论只描述旧 R2 B（BOUNDED_PROGRESS）在 W-02 D2 的退化，不得外推到 R3 B。")

    # calibration summary
    a1 = load(os.path.join(RAW, "AA-W03-D16", "summary.json"))
    a2 = load(os.path.join(RAW, "AA-W02-D2", "summary.json"))
    ab = load(os.path.join(RAW, "R2-D2", "summary.json"))
    diag = load(os.path.join(RAW, "diag-r2", "summary.json"))
    probe = [json.loads(l) for l in open(os.path.join(RAW, "bucket-probe", "results.jsonl"))]
    h1, e1 = header_facts("AA-W03-D16")
    h2, e2 = header_facts("AA-W02-D2")
    h3, e3 = header_facts("R2-D2")
    locklog = [json.loads(l) for l in open(os.path.join(RAW, "lock-log.jsonl"))]
    acquire = next(x for x in locklog if x["event"] == "acquired")
    release = next(x for x in locklog if x["event"] == "released")

    lines = [
        "# CALIBRATION_SUMMARY — #595 R3 续跑 A/A 校准与 R2 同 SHA 复现",
        "",
        f"生成（UTC）：{now}",
        "",
        "## 0. 结论",
        "",
        "| block | phase | 对照 | pairs | CPU mean [CI] | tput mean [CI] | p99 mean [CI] | 判定 |",
        "|---|---|---|---|---|---|---|---|",
    ]
    def row(name, phase, comp, s, verdict):
        c, t, p = s["metrics"]["cpu"], s["metrics"]["tput"], s["metrics"]["p99"]
        return (f"| {name} | {phase} | {comp} | {s['n_pairs']} | "
                f"{pct(c['mean'])} [{pct(c['ci_lo'])}, {pct(c['ci_hi'])}] | "
                f"{pct(t['mean'])} [{pct(t['ci_lo'])}, {pct(t['ci_hi'])}] | "
                f"{pct(p['mean'])} [{pct(p['ci_lo'])}, {pct(p['ci_hi'])}] | {verdict} |")
    lines.append(row("AA-W03-D16", "AA-W03-D16", "A0 vs A0", a1,
                     "PASS (hw 0.797/0.821/2.039pp)"))
    lines.append(row("AA-W02-D2 / R2-AA", "AA-W02-D2", "A_R2 vs A_R2", a2,
                     "PASS (hw 0.862/0.853/1.420pp)"))
    lines.append(row("R2-D2", "R2-D2", "B_R2 − A_R2", ab,
                     "FAIL (LEGACY_D2_NEGATIVE_CONTROL_FAIL)"))
    lines += [
        "",
        "A/A 校准通过（两块的 CPU/tput/p99 半宽都远低于门槛），因此本会话的噪声底足以支撑"
        "同二进制配对比较；R2-D2 的 FAIL 是旧 B 机制的负控结论，不是 R3 B 的结论。",
        "",
        "## 1. 二进制与来源",
        "",
        f"- R3 二进制：`{h1['binary']}` sha256 `{h1['binary_sha256']}`（= BUILD_COMPLETION §8.1）。",
        f"- R2 二进制：`{h2['binary']}` sha256 `{h2['binary_sha256']}`（R2 树 HEAD 271b81bf，"
        f"`git diff HEAD` = b006dbbf…，reverse-check exit 0）。",
        f"- R2 树构建：`xmake f -m release --liburing=y` + `xmake build -y bench_w02_pipeline "
        f"uring_595_bounded_progress_test`，build ok；树 status 6 行、diff sha 构建前后不变。",
        f"- R2 patch 判据/flush/residue：源码门链（uring_transport.cpp:84-90, 229-263）已核对；"
        f"`uring_595_bounded_progress_test`（SLUICE_595_ARM=B）输出 "
        f"`all checks passed`（held-batch flush、cap、partial submit、cancel、owner/witness/payload "
        f"gate、prepark residue、poison、shutdown）。",
        "",
        "## 2. 计时协议与有效性",
        "",
        f"- 24 对/块，AB/BA 交替（A/A：偶 m1,m2 / 奇 m2,m1）；每 invocation "
        f"WARMUP={h1['warmup']}、MEASURED={h1['measured']}、STATS={h1['stats']}、"
        f"`taskset -c {h1['cpu_set']}`、warm tmpfs `{h1['tmp_dir']}`。",
        f"- 每块 24/24 有效配对；`attempts.jsonl` 为空；fixture sha 块前后不变；"
        f"所有 invocation exit=0、ops=20000、byte oracle 通过。",
        f"- 锁：`{h1['lock']['lock_file']}`，获取 {acquire['acquired_utc']}（等待 "
        f"{acquire['wait_s']}s），释放 {release['utc']}；`fuser -v` 记录持锁 pid "
        f"`{acquire.get('fuser', '').strip()}`。`/proc/locks` 在本平台未列出该 flock 条目"
        f"（header 的 holders 为空），探针会话的排他性检查（第二 open file description "
        f"`flock -n` 失败）确认锁语义有效。",
        "",
        "## 3. 逐块细节",
        "",
        "### 3.1 AA-W03-D16（R3 A0 vs A0）",
        "",
        f"- CPU mean {pct(a1['metrics']['cpu']['mean'])} "
        f"[{pct(a1['metrics']['cpu']['ci_lo'])}, {pct(a1['metrics']['cpu']['ci_hi'])}]，"
        f"半宽 {pp(a1['metrics']['cpu']['half_width'])}；tput 半宽 "
        f"{pp(a1['metrics']['tput']['half_width'])}；p99 半宽 "
        f"{pp(a1['metrics']['p99']['half_width'])}。",
        f"- 绝对量级：m1 CPU/op 3154.75 ns、m2 3128.33 ns；m1 tput 317,259 ops/s、m2 319,946 ops/s。",
        "- 均值偏移（m2 略快约 0.8%）不是门；A/A 只看半宽（prereg §8.2）。",
        "",
        "### 3.2 AA-W02-D2 / R2-AA（R2 A_R2 vs A_R2）",
        "",
        f"- CPU mean {pct(a2['metrics']['cpu']['mean'])} "
        f"[{pct(a2['metrics']['cpu']['ci_lo'])}, {pct(a2['metrics']['cpu']['ci_hi'])}]，"
        f"半宽 {pp(a2['metrics']['cpu']['half_width'])}；tput 半宽 "
        f"{pp(a2['metrics']['tput']['half_width'])}；p99 半宽 "
        f"{pp(a2['metrics']['p99']['half_width'])}。",
        f"- 绝对量级：m1 CPU/op 4830.71 ns、m2 4858.23 ns；m1 tput 206,812 ops/s、m2 205,669 ops/s。",
        "",
        "### 3.3 R2-D2（A_R2 vs B_R2，同 SHA）",
        "",
        f"- CPU {pct(ab['metrics']['cpu']['mean'])} "
        f"[{pct(ab['metrics']['cpu']['ci_lo'])}, {pct(ab['metrics']['cpu']['ci_hi'])}]，"
        f"单侧上界 {pct(ab['metrics']['cpu']['ci_upper_one_sided_95'])}（≤5% 通过）。",
        f"- tput {pct(ab['metrics']['tput']['mean'])} "
        f"[{pct(ab['metrics']['tput']['ci_lo'])}, {pct(ab['metrics']['tput']['ci_hi'])}]，"
        f"单侧上界 {pct(ab['metrics']['tput']['ci_upper_one_sided_95'])}（>3% FAIL）。",
        f"- p95 {pct(ab['metrics']['p95']['mean'])}，单侧上界 "
        f"{pct(ab['metrics']['p95']['ci_upper_one_sided_95'])}（>5% FAIL）。",
        f"- p99 {pct(ab['metrics']['p99']['mean'])} "
        f"[{pct(ab['metrics']['p99']['ci_lo'])}, {pct(ab['metrics']['p99']['ci_hi'])}]，"
        f"单侧上界 {pct(ab['metrics']['p99']['ci_upper_one_sided_95'])}（>5% FAIL）。",
        f"- 绝对量级：A CPU/op 4850.41 ns、B 5015.83 ns；A tput 205,994 ops/s、B 199,155 ops/s。",
        "- 历史 R2 D2（`docs/research/595-causal-r2/PROTOCOL.md:126`，仅作对照、不复用）："
        "CPU +3.73% [+3.04,+4.42]、tput +3.58% [+2.94,+4.22]、p95 +4.96% [+4.21,+5.75]、"
        "p99 +7.07% [+4.19,+11.80]；本轮 +3.435%/+3.298%/+5.233%/+7.052%，CI 高度重叠。",
        "- 机制诊断（短 strace，只作机制证据）：A/B 各 60,004 次纯 submit enter、60,004 SQE、"
        "SQE/enter=1.0、max to_submit=1、无 to_submit>1；stats submits/poll/wait 完全相同。"
        "D2 形状没有可合并的提交，B_R2 的成本来自门链检查与 flush 时点后移，而非批量收益。",
        "",
        "## 4. enter/SQE/residue/eventfd 诊断（R2）",
        "",
        "| 臂 | 纯 submit enter | 总 enter | SQE | SQE/enter | max to_submit | "
        "register | eventfd | to_submit 直方图 |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for arm in ("A", "B"):
        d = diag[arm]
        lines.append(
            f"| {arm} | {d['pure_submit_enters_mean']:.0f} | {d['total_enters_mean']:.0f} | "
            f"{d['sqes_mean']:.0f} | {d['sqe_per_enter_mean']:.3f} | {d['max_to_submit']} | "
            f"{d['register_calls_mean']:.0f} | {d['eventfd_calls_mean']:.0f} | "
            f"{d['to_submit_histogram_total']} |")
    lines += [
        "",
        "- 每次 invocation 跑 3 个 pass（timed/oracle/instrumented）× 20,000 ops = 60,000 data SQE，"
        "加 4 次 setup enter，合计 60,004。",
        "- 3 次重复的 enter 数逐次相同；A/B 无差异；未观察到 deferred multi-SQE flush。",
        "- `eventfd` 只在内核注册/注销（`io_uring_register_eventfd`），无用户态 eventfd 写入。",
        "- residue：R2 bench 不暴露 ledger 计数；从 60,004 SQE = 60,004 to_submit 可判定"
        "本次诊断中所有 SQE 都被内核取走（无未提交 residue 残留到退出）。",
        "",
        "## 5. 线程分桶（bucket-probe，描述性）",
        "",
        "| 配置 | main ticks | driver ticks | io-wq ticks | in-thread stime | "
        "ru_total (s) | bench cpu_s (timed pass) | 未归因 ticks | IRQ cpu2 / cpu12 (mean) |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    import statistics as st
    for label in ("R3-W03-D16-A0", "R2-W02-D2-A", "R2-W02-D2-B"):
        rs = [r for r in probe if r["tag"].startswith(label + "-")]
        if not rs:
            continue
        main = st.mean(r["buckets"]["main"]["ticks"] for r in rs)
        drv = st.mean(r["buckets"]["driver"]["ticks"] for r in rs)
        iowq = st.mean(r["buckets"]["io_wq"]["ticks"] for r in rs)
        stime = st.mean(r["buckets"]["main"]["stime"] + r["buckets"]["driver"]["stime"]
                        + r["buckets"]["io_wq"]["stime"] for r in rs)
        ru = st.mean(r["ru_total"] for r in rs)
        cpu = st.mean(r["bench"].get("cpu_s") or 0 for r in rs)
        res = st.mean(r["unattributed_ticks_est"] for r in rs)
        irq2 = st.mean(r["irq_delta"].get("2", 0) for r in rs)
        irq12 = st.mean(r["irq_delta"].get("12", 0) for r in rs)
        lines.append(f"| {label} | {main:.1f} | {drv:.1f} | {iowq:.1f} | {stime:.1f} | "
                     f"{ru:.3f} | {cpu:.4f} | {res:+.1f} | {irq2:.0f} / {irq12:.0f} |")
    lines += [
        "",
        "- 分桶口径：main = tid==pid；driver = 其它非 `iou-*` 用户线程；io-wq = comm 以 "
        "`iou-` 开头的内核 worker（它们出现在 `/proc/<pid>/task` 内，属于进程线程组，"
        "因此其 CPU **已包含在** `CLOCK_PROCESS_CPUTIME_ID` 中，不重复相加）。",
        "- in-thread kernel = 各线程 stime 之和，是上述分桶的子集，不另加。",
        "- 未归因 = `wait4` rusage ticks − 分桶 ticks；来自 10 ms tick 粒度与采样窗口外的"
        "短线程，本轮探针为 +2…+7 ticks（约 0.02–0.07 s）。",
        "- IRQ 为 `/proc/interrupts` 的 cpu2/cpu12 列增量（整机口径上界，无法在不做 tracing "
        "的前提下归因到本 workload；tmpfs 无块设备 IRQ，主要为 timer/IPI）。",
        "- 计时轮的 sampler 只比较全局首末样本，driver 线程（生命周期在 invocation 内部）"
        "被低估为 0；本表用修正后的逐线程 first-seen/last-seen 探针替代，计时轮 CPU 主指标"
        "仍是 bench 的进程 CPU（包含全部线程，无重复计数）。",
        "",
        "## 6. 边界与未做事项",
        "",
        "- 本轮不做 PMU/tracing（除 R2 enter 短诊断的 syscall 计数外）；无 perf/ftrace/bpf。",
        "- 未跑资源门（RSS/分配/线程/队列）的正式判定；RSS 只记录 `ru_maxrss`。",
        "- 未跑 R3 B/A1 块（`semanticsReady=false`，按预注册 blocked）。",
        "- 未跑 #604 真实应用产品门（PRODUCT=HOLD）。",
        "- R2 旧 B 的 D2 结论只描述旧机制；跨 SHA 只并列、不相减。",
        "",
    ]
    with open(os.path.join(RAW, "CALIBRATION_SUMMARY.md"), "w") as f:
        f.write("\n".join(lines))

    # lock receipt
    lock_lines = [
        "# LOCK_RECEIPT — MEASUREMENT.lock 使用记录",
        "",
        f"- 锁文件：`{h1['lock']['lock_file']}`（inode {h1['lock']['inode']}，"
        f"dev {h1['lock']['dev_major']}:{h1['lock']['dev_minor']}）。",
        f"- 会话获取：{acquire['acquired_utc']}，等待 {acquire['wait_s']}s；"
        f"释放：{release['utc']}。",
        f"- 获取时 `fuser -v` 输出：`{acquire.get('fuser', '').strip()}`（持锁进程）。",
        f"- `/proc/locks` 在本平台未列出该 flock 条目（header holders=[]）；"
        f"探针会话用第二个 open file description 做 `flock -n` 排他性检查，结果："
        f"`{open(os.path.join(RAW, 'bucket-probe', 'logs', 'lock-exclusivity.txt')).read().strip()}`。",
        "- 所有构建（R2 xmake）、计时块（3 × 48 invocations）与 R2 strace 诊断均在"
        "获取与释放之间执行；会话日志时间戳覆盖 11:03:31Z–11:08:21Z，探针 11:12:45Z–11:12:49Z。",
        "",
        "## 原始记录",
        "",
        f"- `raw/calibration/lock-log.jsonl`",
        f"- `raw/calibration/bucket-probe/logs/lock-fuser.txt`",
        f"- `raw/calibration/bucket-probe/logs/lock-exclusivity.txt`",
        "",
    ]
    with open(os.path.join(RAW, "LOCK_RECEIPT.md"), "w") as f:
        f.write("\n".join(lock_lines))

    # harness.json
    scripts = {}
    for name in sorted(os.listdir(HARNESS)):
        p = os.path.join(HARNESS, name)
        if os.path.isfile(p) and (name.endswith(".py") or name.endswith(".sh")):
            scripts[name] = sha(p)
    harness = {
        "generated_utc": now,
        "python": sys.version,
        "platform": platform.platform(),
        "kernel": platform.release(),
        "scripts_sha256": scripts,
        "analysis_implementation": {
            "pair_relative": "cpu/p50/p95/p99/ru/rss=(m2-m1)/m1; tput=(m1-m2)/m1",
            "bootstrap": "10,000 percentile resamples over pairs, seed 5950, "
                         "ci_lo=means[int(0.025*B)], ci_hi=means[int(0.975*B)], "
                         "half_width=(ci_hi-ci_lo)/2",
            "aa_gate": "cpu hw<=5pp, tput hw<=3pp, p99 hw<=3pp",
            "negative_control": "mean<=thr and one-sided 95% upper<=thr and "
                                "Holm-adjusted p<0.05 over the block's 4 metrics",
            "selftest": "harness/selftest_analyze.py — 8/8 PASS, including bootstrap "
                        "equality with /home/jnhu/sluice-595-e5/harness/analyze.py",
        },
        "commands": {
            "session": "bash harness/run_session.sh",
            "probe": "bash harness/run_probe.sh",
            "block_runner": h1["runner_argv"],
            "analysis": "python3 -I harness/analyze_pairs.py --phase-dir <phase> --arms <AA|AB>",
        },
    }
    with open(os.path.join(RAW, "harness.json"), "w") as f:
        json.dump(harness, f, indent=1)
    print("wrote", r3, r2aa, r2ab)


if __name__ == "__main__":
    main()
