{
  "argv": [
    "/usr/sbin/perf",
    "stat",
    "-x,",
    "-e",
    "task-clock,instructions,cycles",
    "-o",
    "/home/jnhu/Source/Sluice/docs/evidence/621-progress-wait-path-20261118/p1/aa2/C8-w02-tp-4k-d64-w4/raw/pair-01-B.perf",
    "--",
    "/usr/sbin/taskset",
    "-c",
    "2,3",
    "/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline"
  ],
  "binary_argv": [
    "/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline"
  ]
}
