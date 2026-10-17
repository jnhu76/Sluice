# Captures behind lsan-poisoned-shutdown-disposition.md and prog02-settlement-notification-oracle.md

rg1-lsan-red-stderr.txt — LSan report at the campaign baseline `c1facc52` (pre-fix, GCC ASan+LSan, debug, liburing=y): direct leak PoisonedState at the poison_with_in_flight_cqe witness, 19 allocations / 4016-byte summary region; matches the PR B baseline capture in site and shape
rg1-lsan-green-stdout.txt / rg1-lsan-green-stderr.txt — post-fix witness scenario output (forked child, exit 0)
rg1-tp-asan-stdout.txt — ThreadPool shutdown suite under ASan (control, unchanged path)
rg5-mutant-red.txt — progress_source_uring_mut_final_probe_dropped kill: FAIL k1_k2_cqe_before_drain_recovered_by_final_probe, exit 1 (pre-fix-head capture; re-run at 5fd8e109 in matrix/mutation-kills.txt)
