{
  "argv": [
    "/usr/sbin/perf",
    "stat",
    "-x,",
    "-e",
    "task-clock,instructions,cycles",
    "-o",
    "/home/jnhu/Source/Sluice/docs/evidence/621-progress-wait-path-20261118/p1/aa2/C3b-w02-uring-4k-d16/raw/pair-04-A.perf",
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
