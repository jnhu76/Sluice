# #600 Direct Excess-Cost Evidence Index

Provenance: BASE_SHA bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57 → candidate
ea2fc4fca1a6 (this branch). Host: E5 native Linux (Xeon E5-2666 v3, Fedora 44,
kernel 7.2.11, GCC 16.2.1), governor=performance @3.5GHz, nmi_watchdog=0,
perf_event_paranoid=1 during measurement. Libraries:
baseline sha256 489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a,
candidate sha256 d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0
(xmake -m release --liburing=n, deterministic archives). Both hashes reproduce
through dududu's clean-room SDK install.

Artifacts in this directory:
- DIRECT_COST_DAG.md — E0 freeze + source call DAG.
- DIRECT_MACHINE_COST_LEDGER.md — E1/E2/E3/E4 static + measured ladder.
- PHASE_I_COST_ATTRIBUTION.md — Phase I answers and verdict.
- PHASE3_VALIDATION.md — V1 correctness matrix, V2 paired performance,
  V3 dududu application pairing, C2/D3 records.

Workspace (not committed, reproducible from this index): the ladder probe
(A0/A1/A2/A3/AX single file + fixture generator), paired ABBA runners, python
pair analyzer, disassembly extracts (read_at/read_exact_at baseline vs
candidate), raw series text.

Key raw series (medians, ABBA 16 pairs, taskset cpu2, tmpfs 64 MiB fixture):
A2−A1 excess: 0B +12.8ns t=+148 | 1B +19.3 t=+16.4 | 64B +19.0 t=+12.1 |
512B +16.1 t=+14.8 | 4KiB +15.8 t=+19.5 | 64KiB +6.2 t=−0.66 n.s.;
AX−A2 4KiB +32.0 t=+13.8; A2−A0 4KiB +19.3 t=+12.5; A/A sd 5.0ns.
Candidate−baseline: 0B −13.3 t=−179.8 | 1B −16.4 t=−10.0 | 64B −15.8 t=−11.4 |
512B −16.2 t=−10.1 | 4KiB −16.6 t=−12.4 (replicate −14.2 t=−10.2) |
64KiB −3.2 t=−0.03 n.s. | read_exact_at 4KiB −33.8 t=−22.4.
dududu R1 (medium, 57 reads/181,996,415B, frozen sha 6116e69c…):
Replay/sluice −82.5µs t=−0.20 | Replay/native control +536µs t=+2.15 |
T3/p2_sluice −31.9µs t=−0.07 | T3/p1_native control −429µs t=−0.42.
Mutation kills named in PHASE3_VALIDATION.md. Syscalls/op = 1.000 exact, both.

Commands (reproduction skeleton):
  xmake f -m release --liburing=n -y && xmake build sluice_core
  xmake test                      # 58/58 (release; also debug/asan/asan+ubsan)
  xmake build blocking_file_mut_admit_invalid_range && \
    ./build/.../blocking_file_mut_admit_invalid_range   # expect FAIL named test
  python3 scripts/verify_v2_package.py --manifest-out <path>  # all gates PASS
