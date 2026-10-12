# F4 installed retirement evidence — 2026-10-12

Technical candidate evidence for #508/#510, issues #506/#505, parent #402.
Authority: root v1-r4 PROD-03/W-01–W-03/MIG-01/02/03, F0 §1.7/1.8,
adopted Phase M M-F A3a/B1 and M-A1 P2/P3, and #475 HOLD/KEEP.
No scope amendment, new runtime authority or compatibility shim is introduced.
No production authority moves in the verifier slice. Its contraction audit
finds no additional mechanism eligible for removal. The experimental union
contains exactly the 28 reviewed production-file deletions and no extra
production, test or build-input change. Independent rollback trees remain.

| Tree | Measured commit | Core | Async | Total | Headers |
|---|---|---:|---:|---:|---:|
| B0 | f3521b45ead3963162e518758ac1820004adcfc2 | 13 | 33 | 46 | 73 |
| H0 | a38df5e9a7bee3603a439857f036de2b5a136bf2 | historical source only | — | — | 17 target headers present |
| C508 | 6e5fedf37132964fd224ebc1fdfc4e18741972e2 | 12 | 33 | 45 | 71 |
| C510 | 0529c2b6f8f221aab3112c963ee0491a632229f8 | 3 | 33 | 36 | 58 |
| C508510 | ab2cea2784db05be8b05b5a776c757856beb4395 | 2 | 33 | 35 | 56 |

Combined Git tree: 07c1a3d5c40207e1a519592b9d2e10333674f2ed.
Integration base is B0; the reviewed latest candidate production inputs equal
those of their original deletion commits. Only docs/evidence changed afterward.

The accepted run-02 has 16 fresh build/install configurations: four current
library trees × no-/real-liburing × Debug/Release. All 17 headers have separate
baseline and negative include verdicts; API compile/link/runtime probes cover
all 17 (pool implementation fragment uses its parent). Strong callable symbols
owned by deleted core objects disappear from both candidate archives; baseline
objects fail real candidate relinking. Four header-only interfaces explicitly
have no exclusive callable ABI. No negative probe includes source-tree headers.

W-01 core-only, W-02 bounded cleanup, W-03 application-owned ThreadPool epoll
loop and real-liburing loop ran successfully. No-liburing uring returns exit 2
UNAVAILABLE. Binary nm and real strace prove the W-03 dependency/progress path.
Future<T>, WaitPolicy/default_wait_policy(), Completion, RequestScope and
retained Scheduler/Fiber/StackfulIoHost closure compile and link. W-04 remains
OPTIONAL_CANDIDATE, not supported. All installed public-closure probes pass.

GCC 16.2.1, Clang 22.1.8, xmake 3.0.6, real system liburing 2.13. Exact kernel,
compiler versions, macro views, defined/undefined nm inventories, linker maps,
arguments and source/input hashes are in raw logs. All 32 normal-profile
archives reproduce byte-for-byte between two independent fresh installations.
Nine deliberate integrity corruptions are rejected. The final stronger
integrity verifier also accepts the actual run-02 evidence with zero errors.

Sanitizer availability was remeasured. GCC linkers cannot find their specified
libasan/libtsan runtime files; Clang runtime execution works. Actual combined
libraries plus corrected installed consumers passed Clang ASan+UBSan and TSan
in both profiles. The original W-02 consumer triggered a real ASan UAF and TSan
race: buffers declared after RequestScope were freed before settlement. #511
is the narrow consumer lifetime correction, also included in #512. Original
failures remain in sanitizers-01; corrected executions are sanitizers-02.
These finite executions do not prove all legal interleavings or full W-04.

## Raw material and reproduction

Archive: `d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38.tar.gz`.
SHA256: `d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38`.
The verified FILE_INDEX.json contains 9,677 indexed files, including accepted
installed headers/static libraries, full command stdout/stderr and hashes,
original failures, corrected sanitizer records and exact verifier source
snapshots. The main installed experiment recorded 1,850 commands.
Run-01 is explicitly calibration FAIL, never silently relabeled PASS.
Run-02 executes the committed d5b89d03 runner; final integrity checks use the
stronger 8b1086e2 code lineage. Later documentation does not change measured
library input hashes. Complete SHAs are in evidence.json and source archives.
No F1 or historical B-01 frozen manifest was rewritten.

The files are persistent on jnhu@192.168.31.75 under
/home/jnhu/f4-retirement-verifier-20261012 and committed here as a
content-addressed compressed archive. Reproduction instructions and full
per-interface machine records are inside it. Integrity-only mode rechecks
records and hashes; it does not repeat compilation or runtime execution.
The original /home/jnhu/Source/Sluice checkout is protected and unchanged.

Owner packets are separate: f4-508-owner-decision.md and
f4-510-owner-decision.md. Every external-use field is UNKNOWN and every human
verdict is PENDING. Only an identified human phase owner can approve scoped
breakage or select a bounded window, binding the final PR HEAD, digest, exact
surface set and risk version. No merge, issue closure, F5 completion or v1
release readiness is claimed. CodeRabbit CLI was unavailable; manual exact
source/closure review and deterministic verifier self-tests are recorded.
