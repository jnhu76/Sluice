# R3 平台冻结证书（E5 原生）

记录时间：2026-11-05T11:46–11:50 +08:00
执行者：E5 基线冻结员（workflow subagent）
本轮职责：平台判定 + 环境记录 + 隔离 worktree + 基线构建。**未运行任何 benchmark 计时。**

## 0. 平台判定结果

**PASS —— 原生 E5，可用于本轮计时与 tracing。**

| 判据 | 观测值 | 结论 |
|---|---|---|
| `lscpu` Model name | `Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz` | 匹配 E5-2666 v3 |
| `/proc/cpuinfo` model name（去重） | 同一字符串，20 个 processor | 一致，无异构核 |
| `uname -a` | `Linux E5 7.2.11-200.fc44.x86_64 #1 SMP PREEMPT_DYNAMIC Thu Oct 15 15:58:58 UTC 2026 x86_64 GNU/Linux` | 原生 Linux |
| WSL 标记 | `/mnt/c` 不存在；`/proc/sys/fs/binfmt_misc/WSLInterop` 不存在；`/proc/version` 无 microsoft 字样 | 非 WSL2 |
| 发行版 | Fedora Linux 44 (KDE Plasma Desktop Edition) | 裸机 |

因此 `frozen=true` 的平台前提成立；WSL2 数据未被使用，也不能替代本轮 E5 计时与 tracing。

证据原件：`../raw/lscpu.txt`、`../raw/cpuinfo-modelname.txt`、`../raw/uname-os.txt`。

## 1. 宿主身份

- hostname：`E5`
- CPU：Intel Xeon E5-2666 v3 @ 2.90GHz，1 socket × 10 core × 2 thread = 20 logical，NUMA 1 node（0-19）
- 缓存：L1d 320 KiB（10 实例）、L1i 320 KiB（10 实例）、L2 2.5 MiB（10 实例）、L3 25 MiB（1 实例）
- 频率范围：min 1200 MHz / max 3500 MHz；`intel_pstate` `no_turbo=0`（turbo 可用）
- SMT：active（`/sys/devices/system/cpu/smt/active` = 1）
- **`cpu2` 的 SMT 兄弟 = `cpu12`**（`thread_siblings_list=2,12`）

## 2. 工具链

| 项 | 值 | 来源 |
|---|---|---|
| g++ | `g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)` | `g++ --version` |
| xmake | `xmake v3.1.2+HEAD.5d21e9d` | `xmake --version` |
| python3 | `Python 3.14.8` | `python3 --version` |
| liburing | `2.13` | `pkg-config --modversion liburing` |
| liburing 头 | `/usr/include/liburing.h`（62106 bytes, Jan 16 2026） | `ls -l` |
| liburing 库 | `/lib64/liburing.so` | `ldconfig -p` |
| 编译器实际入口 | `/usr/lib64/ccache/g++`（xmake show 观测） | `xmake show -t bench_w02_pipeline` |

证据原件：`../raw/toolchain.txt`。

## 3. 内核与调度旋钮

| 旋钮 | 值 | 路径 / 命令 |
|---|---|---|
| `scaling_governor`（cpu2） | `schedutil` | `/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor` |
| `scaling_driver`（cpu2） | `intel_cpufreq` | 同上目录 |
| `scaling_cur_freq`（cpu2，冻结时采样） | 1197325 kHz | 同上目录 |
| 全部 20 个 CPU 的 governor | 20/20 均为 `schedutil` | 逐 CPU 读取后 `uniq -c` |
| `perf_event_paranoid` | `2` | `/proc/sys/kernel/perf_event_paranoid` |
| `unprivileged_bpf_disabled` | `2` | `/proc/sys/kernel/unprivileged_bpf_disabled` |

证据原件：`../raw/kernel-sysctls.txt`。

## 4. 存储与挂载

`df -h /tmp`：

```
Filesystem      Size  Used Avail Use% Mounted on
tmpfs            32G  2.5M   32G   1% /tmp
```

精确字节：`tmpfs 33626267648 total / 2592768 used / 33623674880 avail`。

挂载摘要（`mount`）：

```
/dev/sda3 on /     type btrfs (rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2,...)
/dev/sda3 on /home type btrfs (rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2,...)
tmpfs     on /tmp  type tmpfs (rw,nosuid,nodev,seclabel,size=32838152k,nr_inodes=1048576,inode64,usrquota)
tmpfs     on /dev/shm type tmpfs (rw,nosuid,nodev,seclabel,inode64,usrquota)
tmpfs     on /run  type tmpfs (...)
```

`/tmp` 是独立 tmpfs（32 GiB），warm-cache 协议可直接落在 `/tmp`。证据原件：`../raw/df-tmp.txt`、`../raw/mount.txt`。

## 5. 对测量协议的直接影响（必须带入 R3 协议解释）

1. **governor 为 `schedutil`，不是 `performance`。** 冻结时 cpu2 实测 1.2 GHz，而 max 为 3.5 GHz；频率随负载浮动，会对 ns/op 级与延迟分位数引入额外方差。本轮未修改 governor（修改属系统级配置，不在本 ask 授权范围）。→ 计时轮必须记录每轮 governor 与 freq 采样，且在跨臂比较中保持同一状态。
2. **`perf_event_paranoid=2`。** 非 root 只能做 per-process 用户态计数；系统级 `perf stat`/内核事件、以及需要 `-a` 的采集需 root。
3. **`unprivileged_bpf_disabled=2`。** 非特权 `bpftrace` 不可用；bpftrace 类 tracing 必须 root。本轮 tracing 计划须据此调整或显式降权为 UPROBE 之外的手段。
4. **SMT 开启且 cpu2 的兄弟是 cpu12。** `taskset -c 2` 只约束主线程落点，**不阻止**其它进程落在 cpu12 上共享物理核的执行资源。→ 计时轮需同时确认 cpu12 空闲（或以 `taskset -c 2,12` 一并征用后再限定单线程），否则跨臂噪声不可控。这是本轮最重要的平台风险。
5. `/tmp` 为 tmpfs，容量 32 GiB、占用 1%，warm tmpfs 协议有能力支撑 24 对 × 多 case 的工作集；仍须在每轮前记录 `df` 复核。

## 6. 未做 / 不可验证事项

- 未采集 `spec_store_bypass`、`mds` 等漏洞缓解开关对吞吐的定量影响；`lscpu` 显示 Meltdown=PTI、MDS=SMT vulnerable 等缓解已启用（见 `../raw/lscpu.txt`），其对绝对吞吐的影响本轮未量化。
- 未修改任何 governor、turbo、SMT、THP、CPU isolation 设置；上述表内为**冻结时现状**，不是调优后状态。
- 未验证 root 可用性；第 5 节第 2、3 条的 root 依赖能力本轮未实测。

## 7. 复现命令

```bash
lscpu
grep -i "model name" /proc/cpuinfo | sort -u
uname -a
g++ --version
xmake --version
python3 --version
pkg-config --modversion liburing
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
cat /proc/sys/kernel/perf_event_paranoid
cat /proc/sys/kernel/unprivileged_bpf_disabled
df -h /tmp
mount | grep -E ' on / | on /tmp | on /home | on /dev/shm '
cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
```
