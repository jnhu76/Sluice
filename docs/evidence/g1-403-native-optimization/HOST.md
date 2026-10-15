# G1 host admission (native X99)

Admission record for #403 Phase G1, per the phase plan's host-inventory and
safe-performance-environment requirements. Everything here was read from the
host; nothing was assumed.

## Identity

| item | value |
|---|---|
| host | `E5`, native Linux (not WSL2) |
| kernel | `7.2.9-201.fc44.x86_64` (`#1 SMP PREEMPT_DYNAMIC Fri Oct 9 15:04:46 UTC 2026`) |
| distribution | Fedora Linux 44 (KDE Plasma Desktop Edition) |
| architecture | x86_64, 46-bit physical / 48-bit virtual addresses |

## CPU and topology

| item | value |
|---|---|
| model | Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz (family 6, model 63, stepping 2 — Haswell-EP) |
| sockets | 1 |
| cores per socket | 10 |
| threads per core | 2 (SMT) |
| logical CPUs | 20, all online (`0-19`) |
| SMT pairing | CPU *n* and CPU *n+10* share a core (cores 0-9) |
| L1d / L1i | 320 KiB each (32 KiB per core, 10 instances) |
| L2 | 2.5 MiB (256 KiB per core, 10 instances) |
| L3 | 25 MiB, one instance |
| NUMA | 1 node; node 0 = CPUs 0-19 (`numactl` is not installed) |
| bogoMIPS | 5786.49 |
| flags of interest | `rdtscp`, `constant_tsc`, `nonstop_tsc`, `arch_perfmon`, `pebs`, `avx2`, `bmi1/bmi2`, `erms`, `invpcid`, `pcid` |

The usable CPU count is 20, taken from `nproc` and `lscpu -e`, not assumed.

## Frequency, power and thermal state

| item | value |
|---|---|
| scaling governor | `schedutil` |
| scaling driver | `intel_pstate` (`no_turbo = 0`, i.e. turbo is permitted) |
| frequency range | 1200 MHz – 3500 MHz |
| observed idle frequency | 1200 MHz on CPU 0 (i.e. 49% of max, as `lscpu` reports) |
| package temperature at admission | 46 °C (cores 38–41 °C, `high` 83 °C, `crit` 93 °C) |
| mitigation state | `pti`, `ibrs`, `ibpb`, `stibp`, `ssbd`, `md_clear` active; SMT is not disabled |

Frequency is governed by `schedutil`, so a single-threaded and a multi-threaded
run do not necessarily execute at the same clock. Paired A/B runs are therefore
interleaved rather than run in blocks (see the experiment discipline in
`AB-COMPARISON.md`).

## Memory

| item | value |
|---|---|
| total | 62 GiB |
| available at admission | 58 GiB |
| swap | 8 GiB `zram0` (0 B used) |
| page size | 4096 B |

## Storage and filesystems

| device | size | rotational | filesystem | mount |
|---|---|---|---|---|
| `sda` (`GS-480`) | 447.1 G | no (SSD) | btrfs | `/`, `/home` |
| `sdb` (`ST4000HKVS002-3FC104`) | 3.6 T | yes (HDD) | ntfs | not mounted |
| `nvme0n1` (`GVL-512`) | 476.9 G | no (SSD) | ntfs (p3) | not mounted |
| `zram0` | 8 G | no | swap | `[SWAP]` |

`/tmp` is `tmpfs` (32 GiB, RAM-backed). `/var/tmp` is **not** tmpfs: it sits on
`/dev/sda3`, a btrfs volume on the `sda` SSD.

Consequence for the benchmark, which the G0 design also requires:

* `G0_TMP_DIR` (page-cache-resident fixtures) is on `tmpfs` — RAM, no device.
* `G0_DISK_DIR` (storage-backed fixtures) is on `btrfs` over `sda`, an SSD.

Results from the two are never compared with each other, and neither is
compared with the WSL2 G0 v2 figures, whose "disk" was a VHDX on a different
machine.

## Privileges, PMU and tracing

| item | value |
|---|---|
| `perf` | 7.2.9-201.fc44.x86_64 |
| `perf_event_paranoid` | 2 |
| `kptr_restrict` | 0 |
| `/proc/kallsyms` for an unprivileged reader | addresses zeroed (paranoid > 1) |
| kernel lockdown | `[none] integrity confidentiality` |
| `bpftrace` | v0.24.2 installed |
| BTF | `/sys/kernel/btf/vmlinux` present |
| tracefs | mounted |
| unprivileged `perf stat` | `cycles` / `instructions` are dropped; only `cycles:u` / `instructions:u` are counted |
| unprivileged `bpftrace` | refused: "bpftrace currently only supports running as the root user" |

Consequences:

* unprivileged PMU measurement is user-space only, and unprivileged eBPF is not
  available at all;
* kernel-stack profiling, kernel-mode PMU events and bpftrace need root.

No sysctl, governor or lockdown setting was changed to obtain that access, and
none is planned: the privileged steps run the tools as root instead of
loosening a host-wide setting. Anything that cannot be measured within the
granted privileges is reported as `PERMISSION_REQUIRED` or
`NOT_RUN_WITH_REASON` rather than estimated.

## Toolchain

| item | value |
|---|---|
| `g++` | GCC 16.2.1 20260819 (Red Hat 16.2.1-2) |
| `clang++` | 22.1.8 (Fedora 22.1.8-4.fc44) |
| liburing | 2.13 (`/usr/include/liburing.h`, `liburing.so.2.13`) |
| `xmake` | 3.0.9+20260621 |
| `cmake` | 4.3.0 |
| `python3` | 3.14.8 |
| `git` | 2.56.0 |
| `strace` | 7.2 |
| `sensors` | installed (coretemp) |
| `numactl` | not installed |

## Load at admission

```
load average: 0.08, 0.06, 0.07     (1/5/15 min)
Mem: 62Gi total, 4.4Gi used, 52Gi free
```

No TLC model checker, benchmark or profiling job was running. The heaviest
process was the agent's own CLI at 3.8% of one CPU. Measurement stages are
started only after re-checking the load, and the long formal runs are scheduled
so that they do not overlap with a benchmark stage (the phase plan requires
formal and performance runs to be separated in time).

## Safe-performance rules applied

1. Results from `tmpfs` and from the `btrfs`/SSD volume are reported separately
   and never compared with each other or with the WSL2 baseline.
2. Hot-cache (tmpfs, page-cache-resident) and storage-backed cases are separate
   case sets in the frozen matrix; the storage-backed ones are the `-cold` and
   `-disk` cases.
3. No production service configuration is touched; nothing outside this
   repository and the benchmark's own scratch directories is written.
4. No security protection is disabled: `perf_event_paranoid`, `kptr_restrict`,
   the governor and lockdown stay as recorded above.
5. No irreversible tuning is performed.
6. Filesystems and partitions are not remounted or modified.
7. If a temporary setting change ever becomes necessary, the original value is
   recorded first, the change is limited to the experiment, and it is restored
   afterwards. Nothing in this phase required one.
8. PMU and eBPF collection is scoped to the benchmark processes and their
   threads (per-process `perf record`, `bpftrace` probes filtered on the
   benchmark comms). System-wide sampling is not used.
