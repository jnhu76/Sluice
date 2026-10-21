# E5 native-Linux environment admission

Status: **ADMITTED with recorded restrictions**.
Measurement date: 2026-10-21. Host: E5 (owner-provided server, reached over
SSH as `jnhu`). All work was confined to `~/sluice-m2/`; the pre-existing
`~/Source/Sluice` checkout was not modified.

## 1. Access and credential handling

The owner supplied the login credential out of band. It was used only through
the interactive SSH client. No password was written to a prompt, command line,
environment variable, script, log, Git object, test receipt, or this report.
SSH host-key checking was left enabled and no SSH configuration or key material
was changed. If key-based access was unavailable, an interactive session was
used; no password was placed on a command line.

## 2. Host inventory

| Item | Value |
|---|---|
| OS | Fedora Linux 44 (KDE Plasma Desktop Edition) |
| Kernel | `7.2.9-201.fc44.x86_64` (`#1 SMP PREEMPT_DYNAMIC`, 2026-10-09) |
| CPU | Intel Xeon E5-2666 v3 @ 2.90 GHz, Haswell-EP |
| Topology | 10 cores / 20 threads, 2 threads per core, 1 socket |
| NUMA | 1 node (CPUs 0-19) |
| Cache | 25 MiB L3 (1 instance) |
| Memory | 67,252,563,968 bytes (62.6 GiB), no swap used |
| `/home` | `/dev/sda3`, Btrfs, `compress=zstd:1`, `ssd`, `discard=async` |
| `/tmp` | tmpfs, 32 GiB, `nosuid,nodev` |
| GPU/other | AMD GPU (`amdgpu`), Intel Wi-Fi (`iwlwifi`), NVMe (`nvme0`) |
| Compiler | g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2) |
| Build tool | xmake v3.0.6+20260117 |
| liburing | 2.13 |
| perf | 7.2.9-201.fc44.x86_64 |
| DMI | JUXIESHI X99-RS9, AMI BIOS 5.11 |
| Virtualization flags | VT-x present in `/proc/cpuinfo` |

### Bare-metal evidence

`systemd-detect-virt` reports `none`; `/proc/cpuinfo` has no `hypervisor` flag;
`/sys/hypervisor` does not exist; DMI identifies a concrete X99-RS9 board with
an AMI BIOS; local `coretemp`, `nvme`, `amdgpu` and `iwlwifi` hwmon devices are
present. This is consistent with bare metal. It is not a proof: the kernel log
could not be read (`dmesg_restrict=1`, no sudo), so a hypervisor log line could
not be checked. No nested-virtualization evidence was found.

## 3. Performance permissions and controls

| Control | Value | Consequence |
|---|---|---|
| `kernel.perf_event_paranoid` | `2` | Only user-space events (`:u`) are available; no kernel cycles/symbols |
| `kernel.kptr_restrict` | `0` | User-space addresses are readable; kernel symbols are not exported without perf privilege |
| `kernel.dmesg_restrict` | `1` | Kernel log and tracepoints unavailable without root |
| `kernel.perf_event_mlock_kb` | `516` | Perf ring memory limit |
| `RLIMIT_MEMLOCK` | 8192 KiB soft and hard | io_uring setup is sensitive to additional locked memory |
| sudo | not available | No system-level changes were attempted |
| Governor | `schedutil` / `intel_cpufreq` | Not changed |
| `intel_pstate/no_turbo` | `0` | Turbo allowed; not changed |
| THP | `always [madvise] never` | Not changed |
| irqbalance | active | Not changed |
| CPU max/min | 3500 / 1200 MHz | Observed telemetry only |

No kernel, network, firewall, SSH, governor, perf-paranoid, THP, or service
configuration was changed. No unrelated service was stopped. No system-wide
page cache was dropped. `perf record -m 64` and `taskset` are per-invocation
choices, not system changes.

### perf and io_uring interaction (measured limitation)

`perf record` with its default ring size makes `io_uring_queue_init()` fail with
`-ENOMEM`. A minimal `io_uring_queue_init(1, ...)` program reproduces this under
`perf record`, and `perf record -m 1` succeeds; the cause is the locked-memory
budget shared with the perf ring, not a Sluice defect. All reported profiles use
`perf record -m 64`, which leaves enough budget for a 1024-entry ring. This
limits the perf ring to 256 KiB; the runs reported zero lost samples.

## 4. CPU affinity and SMT

All timed runs used `taskset -c 2`. CPU 2's SMT sibling is CPU 12. The process
was pinned, but the sibling was not isolated by a cpuset and the machine has no
`isolcpus` reservation. Background work could therefore share the physical core
through CPU 12. This is a recorded limitation, not a hidden one; the paired,
rotated-arm design and the idle-window replication bound its effect.

Frequency telemetry was sampled immediately before and after each run
(`scaling_cur_freq`, kHz). Primary campaign: median ≈ 3.19 GHz, range ≈ 1.20-
3.49 GHz; the low samples are idle readings before the process boosts. The
replication showed one implausible 6.41 GHz reading; the value is a telemetry
artifact and is not used as a measurement.

## 5. Workload storage and cache state

The A/B fixture is a 64 MiB file at `/tmp/sluice-m2-fixture/data.bin`, created
on tmpfs by `pipeline_ab --create-fixture` with `bench::fill_pattern` and
`sync_data`. `/tmp` is memory-backed, so every read is cache-warm by
construction and no block-device I/O is exercised. This isolates the
submit → RequestCore → io_uring → CQE → publication path; it is not a storage
benchmark. The oracle phase verifies the exact byte pattern for every
repetition, so a corrupted or misplaced read is caught before its timing row is
accepted.

## 6. Background load during the campaign

The primary 15-repetition campaign ran with `loadavg(1)` between 1.15 and 3.20
(median 2.48). The owner's own `parquet_bench` process was observed during the
campaign: PIDs 23077 and later 32763, parent `bash benchmarks/parquet/run-bench.sh
both`, affinity 0-19, about one full CPU, then on CPU 8/9. It was not ours and
was not stopped. A later focused replication ran with `loadavg(1)` 0.17-1.54
(median 1.35, including our own pinned runner) and no sample at or above 2.0.

The primary and replication campaigns agree on sign and approximate magnitude;
the replication is the cleaner sample. Absolute ns/op is not compared across
machines or campaign windows; all claims are paired ratios within a repetition.

## 7. What was deliberately not done

- No kernel replacement, module change, sysctl change, or perf-paranoid change.
- No network, firewall, or SSH configuration change.
- No global governor or turbo change.
- No unrelated service stop (including `parquet_bench` and `irqbalance`).
- No `echo 3 > /proc/sys/vm/drop_caches` or equivalent.
- No modification of `~/Source/Sluice`, its worktrees, or other projects.
- No credential disclosure.
