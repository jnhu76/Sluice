# Combined COPY/TAIL integration evidence

Exact tested commit: `f717e170331df8e098634fe60462b7dd256f8d7b`.
Parents: TAIL final head `64058ca9` and adopted COPY/master merge
`3530890b2147db38daa9b5813de002c158343090`.
All edits, builds, tests and captures ran on jnhu@192.168.31.75.

Four separate source/profile/build roots: Debug/Release x liburing=n/y.
72 target executions PASS (14 per n profile; 22 per real-liburing profile),
plus four COPY CLI suites, each six successful scenarios and two real
RLIMIT_FSIZE write failures. TAIL CLI covers real signal subprocesses and
held-read settlement. Scope/public request, canonical ThreadPool/real Uring,
shutdown, File-close and W-01/W-03 regressions are captured alongside the
six application oracles. Commands, configuration and every target output
are retained by profile. This evidence-only commit does not change C++.

The initial runner passed all 14 debug-n targets, then failed because xmake
accepts one explicit build target: `sluice-copy sluice-tail` was invalid.
The failure propagated under set -euo pipefail. The failed apps-build.log and
initial-orchestration.log are retained. The corrected executed-wrapper.sh
builds each app separately; its resume reused only the 14 exact debug-n PASS
entries with matching raw logs, source SHA and unchanged isolated profile.
All other profiles ran fresh. No test failure was retried or hidden.

To reproduce without resume, from the repository on the Linux host:

```sh
F3_COMBINED_ROOT=/absolute/new/isolated-directory bash docs/evidence/f3-copy-tail-combined-20261011/reproduce.sh
```

The reproducer refuses an existing output root, archives the exact tested
commit, records every command and exits nonzero for any config/build/test/CLI
failure. `.xmake` state is separate for every profile. Prior independently
verified ASan/UBSan and TSan evidence remains in each slice receipt; the
integration changed no TAIL C++ and only incorporated previously verified
COPY code, so no standalone sanitizer repeat is claimed.

Narrow independent review of this final evidence/docs head and formal TAIL
merge/adoption remain coordinator gates. W-04 support and v1 release readiness
remain NO. No runtime source/header/object deletion is authorized by this record.
