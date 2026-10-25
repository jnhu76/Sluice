# R0 — Source and environment freeze (WSL2 exploratory round)

Date: 2026-10-25. Research entry: #580 (parent #563). WSL-only round; E5 native
admission is not attempted here.

## Source baseline

| Item | Value |
|---|---|
| Issue source reference | `5a75e3033ac7e525e1e4a04095a2edc4f76ed07d` (stale; refreshed below per issue instruction) |
| Actual master SHA | `46157fd1f4a1a4afec22d8834c86462b245b576b` |
| Worktree | `/home/hoo/Projects/sluice-580-wt`, branch `research/uring-router-580-wsl`, HEAD = master SHA, dirty state: clean at freeze |
| Production files at freeze | unmodified; all research instrumentation lives in this worktree only |

Files carrying the router mechanism at this SHA:

- `src/async/uring_transport.cpp` (SQ side: dispatch, cookie allocation,
  ledger, poison recovery, cancel, `find_live_router_*`, `retire_router_entry_`)
- `src/async/uring_completion.cpp` (CQ side: `reap_cqes`, `handle_one_cqe`,
  `finalize_operation_terminal_`)
- `src/async/uring_backend.cpp` (admission, `poll()`, publication, owed sweep)
- `src/async/uring_internal.hpp` (`CONTROL_TAG`/`COOKIE_MASK`, `TransportLedger`,
  `BoundedDispatchQueue`)
- `include/sluice/async/detail/uring_backend.hpp` (`RouterEntry`, backend state)

## Host and OS

| Item | Value |
|---|---|
| Platform | WSL2 (Hyper-V partition), `Linux 6.18.54.1-microsoft-standard-WSL2 x86_64` |
| Distribution | Ubuntu 26.04.1 LTS |
| CPU | AMD Ryzen 7 5800H (Cezanne, Zen 3), 8 logical CPUs visible to WSL |
| Memory | 16,379,576 KiB |
| Filesystems | `/` ext4 (project on it); `/tmp` tmpfs |
| Scheduler noise | WSL2 Hyper-V virtualization; no cpuset isolation. Disclosed for all timing results. |

## Toolchain

| Item | Value |
|---|---|
| GCC | 15.2.0 (Ubuntu 15.2.0-16ubuntu1) — active xmake toolchain |
| Clang | 21.1.8 (available, unused for the frozen binaries) |
| xmake | v3.1.2+HEAD.5d21e9d |
| liburing | 2.14 (`liburing-dev 2.14-1`, `/usr/lib/x86_64-linux-gnu/liburing.so.2`) |
| Build profile | `xmake f -m release -o build-ring --liburing=y` |
| Compile flags (uring TUs) | `g++ -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror -O3 -std=c++20 -DNDEBUG -DSLUICE_LIBURING_PROFILE=on` (plus `-DSLUICE_HAS_LIBURING`) |
| Frozen binary | `build-ring/linux/x86_64/release/bench_w02_pipeline`, SHA-256 `42e437318f7b25ad8bc3cbc995ec3a391dd67fca851bfa69eec4c69b9b6aedcd`, links `liburing.so.2` |

## Measurement capability

| Item | Value |
|---|---|
| `/proc/sys/kernel/perf_event_paranoid` | 2 |
| PMU user-space counters | available: `instructions:u`, `cycles:u`, `cache-misses:u` verified with `perf stat` |
| Kernel-space attribution | not available (paranoid=2); kernel time only via process accounting. No kernel symbols guessed. |
| CPU affinity | `taskset` available (used for benchmark pinning) |

## Real io_uring capability (kernel probe, `/tmp/uring_probe`, gcc -O2, liburing 2.14)

```
QUEUE_INIT_OK entries=64
OP_READ=1 OP_WRITE=1 OP_FSYNC=1 OP_STATX=1 OP_ASYNC_CANCEL=1
SUBMIT_AND_WAIT rc=1
CQE rc=0 res=1 user_data=42
```

`REAL_URING_AVAILABLE: YES` — ring creation, all backend-required opcodes
(READ/WRITE/FSYNC/STATX/ASYNC_CANCEL), submit-and-wait and CQE peek all work on
this kernel. The deterministic test seams are used only where the protocol needs
interleavings the kernel will not reproduce on demand, and such results are
marked seam-derived.

## Admission precondition status at freeze

- #578 (fio + DuckDB real-app admission) is OPEN and states in its body that no
  frozen R0/A-A baseline or adapter-parity A/B exists yet: **no #578 real-app
  evidence**.
- E5 native is out of scope for this round by protocol.
- Consequence: any WSL result below is exploratory mechanism/trigger evidence
  only. Formal #580 admission (native or real-app) remains unsatisfied.

## Historical evidence consulted

- #559/#560 (M0/M1 mechanism research), #561/#562 (M2 native validation),
  #578 (real-app admission, open), #577 (K>0 delivery enumeration, independent),
  #575 (methodology: A/A, path-name bias, interval discipline).
- `docs/research/native-linux-m2/h1-identity-matrix.md`,
  `h1-results.md` (decision MICROBENCH_ONLY: lookup 6-17× cheaper at high C in
  isolation, ≤3.7% end-to-end CPU on E5, mutex-dominated profile),
  `cpu-attribution.md` (base 116.46k instr/op at C=1024/d=1; H1 left
  instructions essentially unchanged; H2b owed-sweep removal was the
  instruction-scale win and was productized in PR #565 `d3603249`).
- H1 packed-cookie is **not** a candidate for revival (issue #580 forbids
  unchanged resurrection; the domain narrowing was the disqualifier).

## Addendum (mid-round): E5 native host authorized

During R6 the owner authorized moving the remaining measurement work from WSL2
to the E5 server (user instruction, 2026-10-25). All results produced after
that point on this host are labeled E5-native; the WSL2 results collected
before it remain recorded above and are not discarded. This upgrades the
available evidence class for the measurements that #580's formal admission
names (E5 native interleaved AB/BA), within this research round.

| Item | Value |
|---|---|
| Host | E5 (`192.168.31.75`, hostname `E5`), key auth |
| OS | Fedora Linux 44, kernel `7.2.11-200.fc44.x86_64` |
| CPU | Intel Xeon E5-2666 v3 @ 2.90GHz, 20 logical CPUs |
| Memory | 62 GiB |
| Server checkout | `~/Source/Sluice` at the same frozen SHA `46157fd1` (clean); research tree rsynced to `~/Source/Sluice-580-wt` (server production tree untouched) |
| Toolchain | gcc 16.2.1 (Red Hat), liburing 2.13 (`liburing-devel-2.13-2.fc44`), `/tmp` tmpfs |
| perf | `perf_event_paranoid=2`; `perf record` requires `-m 64` on this host (known from M2, `cpu-attribution.md`) |
| Load during runs | loadavg(1) 0.18-0.34; `taskset -c 2` pinning (same CPU as M2) |
| E5 binaries | `router_ab_baseline` SHA-256 `b03f44b0830723a08d04895fa5e4120af0af2a5bb6d6e109d7f8bf4a27b5f25c`, `router_ab_we2` `96417472a4e7238726b228afa96a269e09f03687a1c75143cccbdd054623c990` (gcc 16.2.1; distinct codegen from the WSL gcc 15.2 builds) |
