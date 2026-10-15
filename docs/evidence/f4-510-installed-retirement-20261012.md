# B-01 F4 installed retirement verification — #510 (2026-10-12)

New versioned candidate evidence; historical F1/B-01 frozen files are unchanged.
Technical owner: F4 #505/#506, parent #402. Root v1-r4 PROD-03/W-01–W-03/
MIG-01/02/03 and F0 §1.7/1.8/#475 govern this packet.

Complete raw evidence and tooling are in Draft PR #512:
https://github.com/jnhu76/Sluice/pull/512
Content-addressed raw archive SHA256:
d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38

B0 f3521b45ead3963162e518758ac1820004adcfc2: core 13, async 33, headers 73.
Independent #508 candidate: core 12, async 33, headers 71.
Independent #510 candidate: core 3, async 33, headers 58.
Combined tested commit ab2cea2784db05be8b05b5a776c757856beb4395, tree
07c1a3d5c40207e1a519592b9d2e10333674f2ed: core 2, async 33, headers 56.
Combined results are actual isolated execution, not independent-candidate sums.
All four current trees were built/installed from fresh prefixes in both profiles
and Debug/Release (16 packages); 32 archives reproduced exact SHA256 bytes.
Every one of the 17 interfaces has separate baseline/include/API/retirement
records; exclusive strong symbols and real negative external relinks are audited.
See the companion per-interface owner packet for this PR's scoped surfaces.

Installed W01, W02 and W03 ThreadPool/real-uring run. No-liburing uring is
UNAVAILABLE, never PASS. Future/WaitPolicy/default_wait_policy, Completion,
RequestScope and retained Scheduler/Fiber/StackfulIoHost closure remain usable.
W04 support remains NO. Original W02 ASan UAF and TSan race are preserved;
Draft #511 corrects borrowed-buffer declaration order, also included in #512.
Clang ASan+UBSan and TSan then pass both profiles against unchanged combined
library production. Consumer correction adoption is an evidence prerequisite,
not a silent rewrite of the original test result. GCC runtime link failures
are separately recorded; no sudo installation was performed.

This update changes docs only; measured production/test/build hashes remain
identical. Final exact PR HEAD and input-equivalence review will be posted after
this documentation commit, so an owner can bind approval without a self-referential
commit hash. Every external-use field stays UNKNOWN. Human approval stays
PENDING; proposed acceptance is not signed. No master modification, merge,
issue closure, F5 completion or release readiness follows from technical PASS.
