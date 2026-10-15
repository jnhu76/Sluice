# G0 (#403) acceptance receipt

```text
BEGIN_403_G0_FINAL_RECEIPT

ISSUE = 403
STAGE = G0
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
BENCHMARK_HEAD_SHA = 8c98b417 (v1) and the v2 revision in COMMIT.txt; all six measured binaries of each revision reproduce byte-identically

HOST = WSL2 (AMD Ryzen 7 5800H, 4 cores x 2 SMT, kernel 6.18.54.1-microsoft-standard-WSL2)
W01_BASELINE = direct 4 KiB read_at 1,021,337 ops/s p50 912 ns, 1.00 pread64/op
W02_THREADPOOL_BASELINE = d16: w1 220,366 / w4 111,757 / w8 108,043 ops/s (~9.9 syscalls/op)
W02_URING_BASELINE = d16 287,764 ops/s p50 73,670 ns, 1.0001 io_uring_enter/op
W03_THREADPOOL_BASELINE = d16 95,938 ops/s, 0.133 notifications/op
W03_URING_BASELINE = d16 143,620 ops/s, 0.198 notifications/op
W04 = UNSUPPORTED (not measured, not a G0 dependency)

REPETITIONS = 2 warmup + 7 measured per case, every repetition retained
METRIC_COMPLETENESS = throughput/latency/CPU/RSS/counters complete; segmented stage
  latency NOT MEASURED; allocation count, lock hold time, kernel symbols,
  physical device and per-thread CPU NOT_OBSERVABLE (each paired with a
  non-invasive method in MEASUREMENT.md section 6)
BOTTLENECKS_CONFIRMED = B-G0-01 (per-op cross-thread handshake, ~10 syscalls/op),
  B-G0-06 (18-45 mutex acquisitions/op on every backend); B-G0-02's inversion is
  measured with coherence/stall traffic as the leading hypothesis
BOTTLENECKS_REJECTED = B-G0-03 (scope scan 0.8-2.0%), B-G0-04 (core occupancy scan
  2.9%), B-G0-07 (ordered-consumer head-of-line), lock contention as the cause of
  the inversion (contended rate highest in the fastest configuration, zero on the
  fastest path)
ENVIRONMENT_LIMITATIONS = virtualized storage device, page-cache eviction only
  via fadvise, tmpfs for hot cases, autoMemoryReclaim=gradual, shared laptop,
  perf_event_paranoid=2 (user-space counters only), no cpufreq control

INDEPENDENT_REVIEW = see comment C
G1_CANDIDATES = C1 (notification suppression) READY, C2 (derived occupancy) READY,
  C9 (fewer separately locked steps) READY as investigation,
  C3 MORE_EVIDENCE_REQUIRED with contention premise withdrawn,
  C5 MORE_EVIDENCE_REQUIRED, C8 MORE_EVIDENCE_REQUIRED,
  C4/C6 NOT_WORTH_OPTIMIZING, C7 NO_OPTIMIZATION_JUSTIFIED
G1_DECISION = start with C1 and C2 only, each as its own reviewable slice with a
  measured before/after on this same matrix

COMMENT_ADMISSION_URL = https://github.com/jnhu76/Sluice/issues/403#issuecomment-6188705332
COMMENT_MEASUREMENT_URL = https://github.com/jnhu76/Sluice/issues/403#issuecomment-6188757379
COMMENT_FINAL_URL = see comment C
GITHUB_READBACK_VERIFIED = comments A and B verified by content hash; comment C
  verification reported with its result

PRODUCTION_CODE_CHANGED = NO (git diff BASE_SHA..HEAD -- src include is empty)
ROOT_SPEC_CHANGED = NO
FORMAL_PROOFS_REUSED_AS_PERFORMANCE_EVIDENCE = NO

G0_VERDICT = BASELINE_ESTABLISHED_WITH_ATTRIBUTION_AND_DISCLOSED_LIMITS (revision 2)
ISSUE_403_STATUS = OPEN

END_403_G0_FINAL_RECEIPT
```
