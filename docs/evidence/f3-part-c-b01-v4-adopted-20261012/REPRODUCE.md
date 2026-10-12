# Adopted scoped B-01 v4 — actual master postmerge capture

Cut: F3_PART_C_B01_FACADE_CUT_V4_20261012.
Status: ADOPTED_SCOPED after the confirmed per-surface/F5 authorization,
independent final-HEAD technical reviews, green normal CI, explicitly authorized
sequential merges and actual master verification below. No whole-F5, whole-F3,
optional-supported W-04 or V1 release-readiness claim.

| Unit | Final reviewed source | Actual merge |
| --- | --- | --- |
| #500 COPY | 49936edabf6b0bbd6955c1e363846c62392bb46d | 9572bd254a32ef9c346dffb9d79dc04e924651a4 |
| #503 waiter | 0d11cde73484844b0a1c3201b182c1ab910ec37a | a04044a6a0b8bbe300f0d3f08f09e42c6bbcabb2 |
| #504 facade | 4964613aad16aefad1b9f3e70af8215fe5d8bc9a | e123d42a42dadc384f64d46ea77458d26104c07c |

[Confirmed owner scope and sequential merge authorization](../../review/f3-part-c-owner-decision-20261012.md).
All final code/head reviews are isolated-agent technical verdicts, not GitHub
formal APPROVED. Normal PR CI passed each final HEAD; normal master-push CI
also passed each merge (runs38198571563,38198811803,38199106877).

Server: jnhu@192.168.31.75. Repository identity /home/jnhu/Source/Sluice.
Original worktree remains clean at 7f5eed6acab33c2a78390ab7e594b4e3f77f1107.
Every new build/install is under the dedicated
/home/jnhu/f3-part-c/merge-gates-20261012-c8QZZL5S directory; no previous
worktree/build is reset, cleaned or reused.

Actual post-#500 master COPY test:24/24. Actual post-#503 waiter test:5/5.
Final e123d42a master: complete Debug/Release n53/53 targets each and real
y67/67 each. Two additional independent Clang22 y source/config/build/install
profiles execute six full risk targets each: waiter, RequestCore, RequestScope,
StackfulIoHost, COPY pipeline and TAIL lifecycle. ASan/UBSan and shared TSan PASS;
raw argv and runtime symbols verify instrumentation. Cache disabled throughout.
Full source/config/install paths, compiler versions, log hashes and source
tree identities are captured. No retry/sleep inflation or oracle weakening.

Fresh package reproduction:

    cd /home/jnhu/f3-part-c/c2-facade-20261012
    python3 docs/evidence/f3-part-c-b01-v4-candidate-20261012/reproduce.py --repo "$PWD" --before cb977a12b951098d65942f4b4020189c7dad1a5e --after e123d42a42dadc384f64d46ea77458d26104c07c --output /home/jnhu/f3-part-c/merge-gates-20261012-c8QZZL5S/master-package-v4
    python3 docs/evidence/f3-part-c-b01-v4-candidate-20261012/completion_probe.py /home/jnhu/f3-part-c/merge-gates-20261012-c8QZZL5S/master-package-v4

Use a new absent output path for replay. The general reproduction helper prints
CANDIDATE_FRESH_PACKAGE_PASS regardless of ref; this capture's source is actual
merged master e123d42a, recorded in the archive hashes and summary.json, rather
than relabeling previous candidate products.

Both independent package profiles physically measure baseline49/36/78 ->
actual master46/33/73. Removed sources/members: application_runtime.cpp,
await_op_helpers.cpp, group.cpp and their three .cpp.o members. Removed installed
headers: application_runtime.hpp, await_op_helpers.hpp, task_result.hpp,
group.hpp, evented_wait_policy.hpp. Previously retired async/file.cpp/.hpp is
not counted again. Actual resolved objects exist; removed objects/symbols/
headers are absent; old header/link probes fail as intended. All retained
compiler dependency sets match before/after and no removed symbol remains
unresolved in retained archives.

W-01, W-02, W-03 ThreadPool, eight package contract consumers, Scheduler/Fiber/
host, default Future/WaitPolicy and actual Completion submit/result/reset
compile/link/run from fresh installed prefixes. Real W-03 uring and W-04
candidate execute in y; n explicitly reports UNAVAILABLE exit2. Candidate
host evidence does not establish optional supported status.

summary.json binds actual installed files and server binary package archives
by SHA256. raw-master-package-evidence.tar.gz contains inventories, symbols,
dependency sets, linker maps, probe output and toolchain receipts. Compressed
matrix/gate logs have uncompressed hashes in matrix-manifest.json.
Original F1, B-01 v3 and premerge v4 candidate captures remain immutable.

F3_APPLICATION_RUNTIME_FACADE_RETIREMENT=VERIFIED
F5_SCOPED_B01_V4=ADOPTED
MASTER_LIBRARY_OBJECTS=46
MASTER_ASYNC_ARCHIVE_MEMBERS=33
MASTER_INSTALLED_HEADERS=73
APPLICATION_RUNTIME_TEST_EDGES=0
UNOWNED_FIVE_WAITER_OBLIGATIONS=0
F3_FULL_RETIREMENT=NO
OPTIONAL_W04_SUPPORTED=NO
V1_RELEASE_READY=NO
