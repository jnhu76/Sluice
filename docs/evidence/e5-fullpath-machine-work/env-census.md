# 环境普查与资格认定（P0-A）

- 日期：2026-11-18（本机时间 00:34–00:55 CST 区间内执行）
- 执行者：环境测量员（工作流子代理），全程与编译/测试串行，未并行启动任何编译或重负载
- 预期目标：Xeon E5-2666 v3 原生 Linux —— **核实为属实（裸机，见 §A.2）**
- 约定：测不到的项目一律标 `NOT_OBSERVABLE` 并写明原因；无任何推断值充当测量值
- 原始数据（临时文件，含完整 turbostat/perf 输出）：`/tmp/ts-B{1,2,4,10}.csv`、
  `/tmp/perf-B{1,2,4}-cpu*.csv`、`/tmp/perf-B10-cpu*.csv`、`/tmp/ts-C{1,10}-perf.csv`、
  `/tmp/perf-C{1,10}-perf-cpu*.csv`、`/tmp/governor-original.txt`、`/tmp/aa-probe.txt`
  （/tmp 为瞬态；本文档内已嵌入全部关键读数）

## A. 只读普查

### A.1 CPU / 内核 / 拓扑

来源：`lscpu`、`lscpu -e`、`uname -a`、`/proc/cpuinfo`。

| 项 | 读数 |
| --- | --- |
| Model name | Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz |
| Family/Model/Stepping | 6 / 63 / 2 |
| Vendor ID | GenuineIntel |
| CPU(s) | 20（0-19 在线） |
| Core(s) per socket / Socket(s) | 10 / 1 |
| Thread(s) per core | 2（CPU0-9 为核 0-9 第一超线程，CPU10-19 为对应伴核，`lscpu -e` 确认） |
| NUMA | 1 节点（node0 = 0-19） |
| L1d/L1i/L2/L3 | 320KiB×10 / 320KiB×10 / 2.5MiB×10 / 25MiB×1 |
| 频率边界 | max 3500 MHz / min 1200 MHz（`lscpu` 与 cpufreq 一致） |
| 微码 | microcode 0x49（/proc/cpuinfo，20/20 一致） |
| 相关指令集标志 | sse4_2, avx, avx2, bmi1, bmi2, f16c, aes, rdrand, popcnt；无 avx512 |
| 内核 | `Linux E5 7.3.3-200.fc44.x86_64 #1 SMP PREEMPT_DYNAMIC Fri Nov 6 20:34:09 UTC 2026`（Fedora 44 系） |
| 内存 / swap | 64 GiB（57.3 GiB free）/ 8 GiB（0 used），census 时点 |

### A.2 裸机 vs 虚拟机 → **裸机**

证据（均为本机直接读取）：

1. `systemd-detect-virt` → `none`，rc=1（1 = none/裸机；本版不支持 `--all` 选项，已改用基本形式）。
2. `lscpu` 输出无 `Hypervisor vendor` 字段。
3. `/proc/cpuinfo` 无 `hypervisor` flag（grep `hypervisor_vendor_id|Xen|KVM|VMware` 为空）。
4. DMI：`/sys/class/dmi/id/`：sys_vendor=JUXIESHI，board_name=X99-RS9，bios_vendor=American Megatrends Inc.，bios_version=5.11 —— 真实主板/BIOS 产品标识，非云厂商虚拟机型。

结论：**裸机**（消费级 X99 主板 + 至强 E5-2666 v3，AWS 定制型号）。

### A.3 cpufreq / intel_pstate 状态

来源：`/sys/devices/system/cpu/cpu*/cpufreq/*`、`/sys/devices/system/cpu/intel_pstate/*`、`cpupower frequency-info`。

| 项 | 读数 |
| --- | --- |
| scaling_driver | `intel_cpufreq`（intel_pstate **passive** 模式） |
| scaling_governor（全部 20 CPU） | `schedutil`（逐 CPU 读取确认 20/20 一致） |
| scaling_min_freq / scaling_max_freq（cpu0） | 1200000 / 3500000（其余 19 CPU 相同，见 C 节存档） |
| cpuinfo_min_freq / cpuinfo_max_freq | 1200000 / 3500000 |
| cpuinfo_transition_latency | 20000（20 µs） |
| scaling_available_governors | conservative ondemand userspace powersave performance schedutil |
| intel_pstate status / no_turbo | passive / 0（turbo 未禁用） |
| num_pstates / turbo_pct / min_perf_pct / max_perf_pct | 24 / 25 / 34 / 100 |
| cpupower boost | Supported: yes, Active: yes |
| cpupower frequency-info（CPU2） | hardware limits 1.20–3.50 GHz，当前 policy 1.20–3.50 GHz |

### A.4 温度

来源：`sensors`、`/sys/class/thermal/`（census 时点，近空闲状态）。

- coretemp-isa-0000：Package id 0 = +48.0 °C；各 Core +38 ~ +42 °C；high = +83 °C，crit = +93 °C
- nvme-pci-0200：Composite +39.9 °C（crit +94.8 °C）
- /sys/class/thermal：thermal_zone0（x86_pkg_temp）= 48000；thermal_zone1（iwlwifi_1）= 47000
- 满载时温度见 §B.4（10 核满载 PkgTmp 65–69 °C，距 crit 尚远，无降频）

### A.5 后台负载快照

来源：`uptime`；`top -bn1`（一次快照）。

- uptime：`00:34:47 up 24 min, 1 user, load average: 0.30, 0.30, 0.31`（1/5/15 分钟）
- top：392 tasks，0 running，391 sleeping；%Cpu(s)：0.2 us / 0.4 sy / 99.3 id / 0.0 st
- 占 CPU 的进程仅本代理自身的宿主进程：node（4.7%）、zcode-c+ ×2（4.7% 各）；无其他持续负载
- 结论：测量期间机器近空闲；turbostat 报告的 st 字段为 0（无 stolen time，旁证非虚拟机）

### A.6 工具可用性（逐项）

| 工具/能力 | 结论 | 证据 |
| --- | --- | --- |
| perf | **可用**（普通用户即可） | `perf --version` = `perf version 7.3.3-200.fc44.x86_64`；`perf stat -e instructions true` 成功（instructions:u = 148,100）；/proc/sys/kernel/perf_event_paranoid=2（允许自身进程的用户态计数，不允许无特权系统级/内核计数），kptr_restrict=0 |
| turbostat | **可用（需 sudo）** | `sudo turbostat -q -i 1 -n 2` 试跑成功，逐核 Busy%/Bzy_MHz/TSC_MHz 输出正常（APERF/MPERF） |
| bpftrace | **可用（需 sudo）** | `bpftrace --version` = v0.24.2；`sudo bpftrace -e 'BEGIN{printf("bpftrace-trivial-probe-ok\n");exit();}'` 输出探针字符串成功（Attached 1 probe） |
| bpftool | 可用 | `bpftool version` = v7.6.0，libbpf v1.6，features: llvm, skeletons（未做实际加载探针） |
| /sys/kernel/debug/tracing | 普通用户 **EPERM**；**sudo 可读** | 用户态 `ls`/`cat` 均 Permission denied；sudo 下 `ls` 正常、`tracing_on=1` |
| mincore | 可用（syscall 27） | `/usr/include/asm/unistd_64.h` 定义 `__NR_mincore 27`；fincore 实测走 mincore 回退成功（下条） |
| fincore | 可用 | `fincore /usr/bin/env /usr/bin/ls` 正常输出 RES/PAGES/SIZE；用户态尝试 cachestat 得 EPERM 后回退 mincore 成功 |
| cachestat | **内核提供，本环境普通用户被拒** | `__NR_cachestat 451`（头文件）；`syscall(451)` 正确参数布局：root 下 ret=0（nr_cache=13），普通用户 ret=-1 EPERM。注：首次探测误用 458 得 EOVERFLOW，属探测方法错误，已用头文件编号 451 复测 |
| rdmsr | **不存在** | `which rdmsr` → not found（MSR 访问经 /dev/cpu/N/msr 由 turbostat 完成） |
| stress / stress-ng | **不存在** | `which` 均 not found（忙循环用 shell `while :` 实现，无需二者） |
| cpupower | 可用 | `cpupower frequency-info` 正常输出 |
| taskset | 可用 | 在 B/C 实测中反复使用成功 |
| MSR 设备 | 可用（仅 root） | `ls -l /dev/cpu/0/msr` = `crw------- root root`；turbostat 读取正常 |

### A.7 NOT_OBSERVABLE 项

- RAPL 能耗边界（Power limits, PL1/PL2 详情）：未单独读取 /sys/class/power_supply 与 RAPL sysfs（超出本 ask 范围）；仅 turbostat PkgWatt（见 §B）为实测。
- BIOS 版本之外的固件设置（是否锁定 turbo 等）：NOT_OBSERVABLE（不做 BIOS 操作，按 ask 禁令）。
  但 `cpupower` 报告 boost Supported+Active 且 §B 实测单核 3.45 GHz，旁证 turbo 解锁可用。

## B. 频率实测（governor=schedutil 基线）

方法（每档 20 s，本机近空闲，串行执行）：

- 忙循环：每目标物理核一个 worker，`taskset -c <cpu> timeout 20 sh -c 'while :; do :; done'`（纯用户态紧循环，无 fork/无系统调用）
- 频率权威源：`sudo turbostat -q -i 5 -n 5 -s Core,CPU,Busy%,Bzy_MHz,TSC_MHz,PkgTmp,PkgWatt -o /tmp/ts-B<K>.csv`（APERF/MPERF，间隔 5 s × 5）
- instructions 维度：每 worker `perf stat -e instructions,cycles,task-clock -x,`（普通用户级，`-x,` CSV 输出）
- 档位取 **1 / 2 / 4 / 全部物理核（10）**：worker 钉在 CPU0..K-1（物理核第一超线程），SMT 伴核空闲
- turbostat 首末区间含启动/收尾（worker 未全程占满区间），**稳态取区间 2–4**

### B.1 turbostat 稳态结果（区间 2–4，仅列出被测核）

| 档位 | 被测核 | Busy% | Bzy_MHz（区间2/3/4） | PkgTmp | PkgWatt |
| --- | --- | --- | --- | --- | --- |
| 1 核 | CPU0 | 98.45–98.71 | 3455 / 3444 / 3447 | 57 °C | ~34.9 W |
| 2 核 | CPU0,1 | 97.57–97.80（两者相同） | 3389 / 3388 / 3400 | 59–61 °C | ~34.9 W |
| 4 核 | CPU0-3 | 100.00（全核） | 3200 / 3200 / 3200（精确） | 61–62 °C | ~34.9 W |
| 10 核 | CPU0-9 | 100.00（全核） | 3200 / 3200 / 3200（精确） | 65–67–69 °C | 36.6–36.7 W |

（首区间为爬坡，如 1 核档首区间 CPU0 84.11% / 3401 MHz；末区间 worker 已结束，如 10 核档末区间 ~15-19%。）

### B.2 perf instructions 维度（20 s 全窗口含爬坡，worker 自身计数）

| 档位 | 核 | instructions（20s） | cycles（20s） | 窗口平均 MHz | IPC |
| --- | --- | --- | --- | --- | --- |
| 1 核 | cpu0 | 177.498G | 67.597G | 3380 | 2.626 |
| 2 核 | cpu0 / cpu1 | 172.482G / 165.995G | 65.913G / 65.899G | 3296 / 3297 | 2.617 / 2.519 |
| 4 核 | cpu0-3 | 166.605/168.613/158.552/168.000 G | 63.577–63.686G | 3179–3184 | 2.490–2.652 |
| 10 核 | cpu0-9 | 157.442–168.273 G | 63.522–63.653G | 3176–3182 | 2.476–2.647 |

（perf 窗口平均 MHz 低于 turbostat 稳态值，因含频率爬坡段；两者互为印证。）

### B.3 两点回答

1. **单核最大 turbo ≈3.5 GHz 可达吗？** —— 可达性成立（≈），稳态实测 3.44–3.46 GHz
   （APERF/MPERF 平均，Busy%≈98.5，governor=schedutil；performance governor 下为 3.47，见 §C）。
   未观察到瞬时精确 3500 的 APERF/MPERF 读数（区间平均性质所限），不做该断言。
2. **多核同时满载的实际可持续频率？** —— 2 核 3.39 GHz；**4 核起即钳制在 3.20 GHz**
   （all-core turbo 桶），10 核满载 15 s 稳态读数三次均为精确 3200 MHz，温度 65–69 °C
   （crit 93 °C）、PkgWatt ~36.7 W，**无温度降频，极稳定**。

### B.4 禁令遵守

- 未做任何 BIOS/MSR 写/turbo 解锁/超频/持久化；`no_turbo=0`、`max_perf_pct=100` 均为系统既有状态，只读未改。
- instructions 维度全程保留（§B.2），未以提频掩盖指令开销。

## C. governor 处置

前提核实：当前 governor = schedutil（非 performance）×20，sudo 可用 → 触发本节流程。

### C.1 三列记录（原值 / 实验值 / 恢复后读回）

存档文件：`/tmp/governor-original.txt`（含每 CPU scaling_min_freq/scaling_max_freq，一并存档）。
操作：`echo performance > /sys/devices/system/cpu/cpuN/cpufreq/scaling_governor`（仅 scaling_governor，其余 sysfs 未动）。

| CPU | 原值（存档） | 实验值（设置后读回） | 恢复后（读回验证） |
| --- | --- | --- | --- |
| cpu0–cpu19（20 行同值，逐 CPU 存档于 /tmp/governor-original.txt） | schedutil | performance | schedutil |

逐 CPU 明细：cpu0..cpu19 每行为 `schedutil / performance / schedutil`。恢复后逐行比对存档，脚本输出
`RESTORE-OK: all 20 CPUs governor == archived original (schedutil)`，无 MISMATCH 行。

### C.2 performance 档重跑（关键档位：1 核 + 10 物理核）

| 档位 | 稳态（区间 2–4）Bzy_MHz | Busy% | 与 schedutil 基线对比 |
| --- | --- | --- | --- |
| 1 核（cpu0） | 3468 / 3468 / 3473 | 98.97–99.04 | 基线 3444–3455 → **+15~20 MHz**（perf 窗口均频 3422 vs 3380） |
| 10 核（cpu0-9） | 3200 / 3200 / 3200 | 100.00 | 与基线**完全相同** |

解读：多核满载被 all-core turbo 桶（3.2 GHz）钳制，governor 无影响；单核 schedutil 已能推到
turbo 附近，performance 仅略增。instructions 维度照常记录（C10p 各核 152.2–168.4 G/20s，
窗口均频 3182–3184 MHz，与基线一致）。

### C.3 禁令遵守与恢复验证

- 仅临时修改 `scaling_governor`；未触 BIOS、未写 MSR、未动 no_turbo/max_perf_pct、未超频、无持久化（sysfs 不跨重启）。
- 恢复：逐 CPU 从存档回写 → 全量读回比对 → `RESTORE-OK`（20/20 = schedutil）。

## D. 粗 A/A 探针（perf stat -e instructions）

- 固定负载：`sh -c 'i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done'`（纯用户态 bash 算术循环，约 1 s）
- 命令：`perf stat -e instructions -- <负载>`，连续 3 次，governor=恢复后的 schedutil
- 原始输出（/tmp/aa-probe.txt）：

| run | instructions | wall elapsed |
| --- | --- | --- |
| 1 | 7,622,959,561 | 0.9689 s |
| 2 | 7,622,959,607 | 1.0451 s |
| 3 | 7,622,959,530 | 0.9841 s |

- 统计：mean = 7,622,959,566；样本 stdev = 38.7；**CV = 5.1e-6 %（≈0.000005%）**
  （极差 77 条指令 / 76 亿条）
- 结论：对确定性的用户态负载，`instructions` 计数在本机几乎零噪声（仅受中断扰动），
  可作为后续 A/A 的主维度；wall-time 噪声 ~8%（0.97–1.05 s，主因频率/调度抖动），
  时长类指标须配合频率控制或加大样本。

## 总体资格结论

| 项 | 结论 |
| --- | --- |
| 平台 | 裸机，Xeon E5-2666 v3（10C/20T，2.9 GHz base），Fedora 44 系内核 7.3.3-200.fc44 |
| 单核 turbo ≈3.5 GHz | 成立（稳态 APERF/MPERF 3.44–3.46 GHz；performance 档 3.47 GHz） |
| 多核满载可持续频率 | 2 核 3.39 GHz；4 核及以上固定 3.20 GHz，10 核满载稳定无降频（≤69 °C / 36.7 W） |
| 频率测量权威 | turbostat（sudo, APERF/MPERF）可用；perf 用户级 instructions/cycles 可用 |
| governor 状态 | 处置完成并已恢复验证（schedutil ×20 = 存档原值） |
| A/A 噪声 | instructions 计数 CV≈0（5e-6 %）；时长 CV 大（~8%） |
| 环境限制 | perf_event_paranoid=2（无特权系统级 perf）；cachestat 用户态 EPERM（root 可用）；rdmsr/stress/stress-ng 缺失；/sys/kernel/debug/tracing 仅 root |
| 判定 | **PASS_WITH_LIMITS**：全部测量均完成；限制仅为工具权限面（不影响本 ask 的测量目标） |
