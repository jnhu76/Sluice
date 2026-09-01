# aligne1-validate-native-1 — notes

INVALID attempt (retained immutable): ran with the ORIGINAL 512 MiB
config; every run failed the driver's perf-availability gate because the
harness's perf invocation dropped the `-e` event list (bug found in
AMENDMENT-1 commit f3b38909). 48 runs, 48 gate errors, zero successful
runs. Bench-side same-work gates passed on the bench line (bytes/ops
intact) — the failure was harness wiring, not evidence corruption.
Superseded by aligne1-validate-native-6 (green).