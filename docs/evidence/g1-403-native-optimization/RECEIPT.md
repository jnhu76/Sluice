# G1 final receipt

BEGIN_403_G1_FINAL

ISSUE = 403
STAGE = G1
BRANCH = codex/403-g1-native-optimization
PR = https://github.com/jnhu76/Sluice/pull/536 (NOT merged by process; merge is the owner's decision)
BASE_SHA = 10aaf58b (master with #533/#534); baseline freeze d0673a8b
G1_HEAD_SHA = see COMMIT.txt (receipt commit; evidence chain d0673a8b..this commit)

HOST = E5, Intel Xeon E5-2666 v3, 20 logical CPUs, 62 GiB (HOST.md; native Linux, not the G0 WSL2 host — baselines never mixed)

ADOPTED = C9-A 001512d4 (driver-side mutex merges); C1 4c86d3fe (notification-write suppression); C2 0891be17 (derived outstanding summary; occupancy() stays authority)
REJECTED = C9-G2 uring per-batch CQE lock (−3.4% primary, reverted); notification drain-skip (w1 −2.7%/w4 −3.0%, reverted)
DEFERRED = C9-G4 (expected sub-1% effect below the measured ~±5% A/B resolution)

MEASURED = paired interleaved A/B vs frozen B0 (ab/final): W02-tp-w4 +19.3% (15/15 reps ≥1.09, cpu/op −18.8%, p99 ×0.925); saturate +14.6%; W03-tp-w4 +29.5% (p99 ×0.948); W02-uring +2.0% / W03-uring +1.6% (inside the recorded ±3–5% cross-binary noise band); W01 +0.7% (binary byte-identical to B0); W02-tp-w1 −11.0% disclosed with park-cycle attribution

GATES = ring suite 61/61; no-ring suite 54/54; mutation 71/72 (sole documented F2 survivor uring_cutover_mut_drop_outcome); runner_failure_control 11/11; SANY PASS; TLC safety PASS rc=0; TLC liveness PASS rc=0 (depth 53, 64,316,784 fingerprints, 12/12 temporal branches, pinned jar 936a2620…); G0 matrix revalidation PASS (head ring w02 = A/B arm build b5c687ea…); sanitizers PERMISSION_REQUIRED (libtsan/libasan/libubsan absent; commands recorded); kernel-side PMU PERMISSION_REQUIRED (user-space-only campaign retained)

REVIEW = APPROVE_WITH_DISCLOSED_LIMITATIONS (final independent re-review; six proof obligations verified against retained raw JSONLs; nine non-blocking findings recorded in docs/roadmap/v1-conformance.md G1 entry)

LIMITATIONS = w1 −11.0% aggregate disclosed (attribution strace census raw not retained); uring deltas within noise band; sanitizer receipt outstanding pending privileged install; PMU kernel-side counters not observable at perf_event_paranoid=2; PR #535 (G1-R1 input) stays Draft + do-not-merge, never merged

EVIDENCE_COMMENTS = G1-A 6196869263; G1-B 6199853422; G1-C 6200018424; G1-D this receipt

END_403_G1_FINAL
