# aligne0-validate-native-1 — session notes

HARNESS VALIDATION on native Linux (frozen development subset: all 8 arms x
dirs {read,write} x sizes {4K,1M} x depths {1,8}). 128 runs, 0 gate errors,
64 summary rows. Same-work gates green; minflt=0 for all cells (prefaulted
reuse holds); data src/dst sha256 match environment.json.

## Native environment (recorded in addition to environment.json)

- Host: bare metal desktop (DMI vendor JUXIESHI; no `hypervisor` CPU flag;
  `systemd-detect-virt` absent, no VM signature). NOT WSL, not a container.
- Kernel: 7.1.9-200.fc44.x86_64 (Fedora 44, native, SMP PREEMPT_DYNAMIC).
- CPU: Intel Xeon E5-2666 v3 @ 2.90 GHz (Haswell-EP, 10 physical cores / 20
  threads, SMT on, 1 socket), turbo up to 3.5 GHz, schedutil governor
  (intel_cpufreq), L1d 32K/core, L2 256K/core, L3 25M shared.
- RAM: 62 GiB (3 GiB used at session time, 57 free).
- Page size: 4096 (getconf PAGE_SIZE). Huge pages: 0.
- Filesystem: btrfs on /dev/sda3 (subvol /home), mount opts
  rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2;
  storage device /dev/sda = GS-480 480 GB SATA SSD (ROTA=0). Data files in
  build/aligne0-data/ are pseudo-random deterministic bytes (splitmix64), so
  btrfs zstd:1 stores them effectively raw; page-cache warm regime means the
  timed path is uaccess copy, not device/compression writeback.
- libc: glibc 2.43; compiler: clang 22.1.8 (Release, warnings-as-errors OK);
  xmake 3.0.9; perf 7.1.9; liburing 2.x present (not used by this bench).
- perf_event_paranoid: 2 (user-space events OK unprivileged).
- git: research/align-e0 @ 7d7046fc (8600583 + one metadata-only text fix),
  clean.

## PMU reliability observation (d1 READ 4K window)

- instructions:u per op — STABLE and arm-invariant: 2382.1..2384.0 across
  a0..a7 (WSL2: 2397.6..2400.4). Double-difference attribution is clean.
- cycles:u per op — NOT RELIABLE in the double-difference sense: negative
  per-op values observed (a0 -2342, a7 -1176) meaning cycles(R14) did not
  strictly exceed cycles(R7); per-run frequency/turbo state differs between
  the two process runs. Same conclusion as WSL2 (unreliable), different
  cause (frequency scaling between process runs, not a virtualized
  counter). cycles/op is recorded but demoted; quantitative pair =
  instructions:u + in-process wall.

## Headline wall numbers (d1, read, same-work identical)

- 4K: a0 (16) 1489 ns/op (MAD 128) vs a1..a7 1234..1366 ns/op (MAD 26..249)
  → a0 approx +20% vs the fast group, MAD-separated from a1/a2/a4..a7 but
  NOT the 2.2x WSL2 gap. a3 window noisy (MAD 249).
- 1M: a0 177.8 us/op vs a1 171.8 us/op (+3.5%); overall MAD very high
  (24..120 us) at 1M — machine noise dominates 1M d1 windows on this host.

Interpretation: harness end-to-end correct on native (gates, hashing, perf
wiring, pointer alignment metadata). The validation window hints at a weak
directional a0 lag, far below WSL2 magnitude; the frozen ladder is the
adjudicator.