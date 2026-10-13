# F3 actual merged-master closeout receipt

Actual tested master: `f4a9723410539025cffa3422c5f6841ef795c755`.
Accepted PR513 HEAD: `350e789810f16078d528e572a0b124e71bd6da97`.
Both full Git trees equal `c75c5cee5afd127ae2cae4d136187fb6d152a225`.
All C++ builds/runs/package collection occurred via SSH at
jnhu@192.168.31.75 in new /home/jnhu/f3-postmerge-closeout-20261013 worktrees.
Original /home/jnhu/Source/Sluice remained clean at7f5eed6acab33c2a78390ab7e594b4e3f77f1107,
branch docs/489-f-w2-1-dual-profile-refreeze. No original/historical worktree cleanup.

Four NEW Debug/Release × no-/real-liburing application configurations pass209 commands;
ASan/UBSan and TSan each pass both native-Clang profiles (50 commands).
Actual18 sanitizer executable hashes/nm inventories,24 app-object references and four
configured compile databases are preserved. Four NEW installed packages each have
2core /33async /56headers; all17 retired-header negatives,13 authentic historical-object
relinks, required W01/W02/W03 and retained mechanisms pass. Real-uring W03 runs;
no-liburing profile reports explicit unavailable for its uring case. Host neutrality and
non-busy progress are witnessed by source/link firewall and recorded epoll traces.
F4 integrity errors=[] and21 deliberate corruptions reject. GCC sanitizer missing-runtime
link failures remain prior unavailable evidence; Clang PASS does not relabel GCC.
Actual merged-master four CI jobs succeed:
https://github.com/jnhu76/Sluice/actions/runs/38332409440 .

Each PASS binds actual command argv/cwd, exit, stdout/stderr SHA and execution source.
summary.json and raw-archives.json provide machine-readable identity/counts/digests.
Each tar includes a FILE_INDEX.json covering all included members. Build directories
are excluded; binaries need rebuilding for runtime replay. Installed packages, historical
relink objects, source snapshots, compile databases, instrumentation inventories and
raw command logs remain included. Archives authenticate execution evidence, not a claim
that integrity verification repeats runtime execution.

## Reproduce

Use a NEW isolated root and full Git history, without changing original/master checkout.
Create detached source atf4a9723410539025cffa3422c5f6841ef795c755.
Run `python3 scripts/verify_f4_postmerge.py --source <source> --master-sha f4a9723410539025cffa3422c5f6841ef795c755 --output <absent-output>`.
For integrity-only replay extract package archive and use `--verify-evidence <packages>`.
Run21 controls with `python3 tests/package/f4_retirement/test_postmerge.py <packages>`.
App archive contains run_app_matrix.py and run_app_sanitizers_native_allflags.py;
adapt root/repository paths to NEW absent prefixes, retain sourceSHA/config/targets/oracles.
Use XMAKE_STATS=false (recorded coordination environment) to disable detached telemetry.
Clang sanitizer configuration uses native redhat target for cflags/cxxflags/ldflags/shflags.
`capture_f3_sanitizer_instrumentation.py <sanitizer-build-root> <absent-output>`
records actual instrumentation; configured compile database commands/exits are included.
Actual published W04 page fetched2026-10-13 retains explicit current-support denial;
HTML digest in summary.json. Support remains NO, conditional N/A audit retained.

## Owner and scope

The human actively approved REJECT_REMOVED_OPTION and conditional merge in this chat,
then directed ready/head-CI/merge/new-master verification. Delegated transcription:
https://github.com/jnhu76/Sluice/pull/513#issuecomment-6136092532 .
It binds exact350e7898, risk versionb588a51cd2bb1c08f9275f4a44748b27392644ecb72486263e8f7becf1f61415,
app rawbb7e8d963be532b04bb217aec6a05efd00a935084290239b2d258c793c6fee94,
and supplemental00bbcb0f9ae0ab47dfb0511661f7e470e7b3e0c1e98494f504b4fa93f5539a73.
This is active human chat approval, not an agent signature or fabricated GitHub APPROVED.
Scripts must omit TAIL --workers; all spellings/values reject usage1 before target open;
external usage UNKNOWN. No window/shim/new runtime. COPY actual workers retain semantics.
The original unsigned candidate risk file is historical; this record supersedes its pending state.

D-H1..5/T2-08, R1–R6, seven DOC_CLAIM and OB1..3 mappings remain in
../f3-app-host-closeout-20261012/scope-and-oracles.md; new execution repeats those actual
app/regression/CLI oracles. R4 watchdog observations do not promise unconditional
settlement latency. No thread-creation/join exception injection or full W04 support claimed.
Existing excluded legacy APIs and compatibility shims are not restored.

F3 technical exit supports scoped adoption after independent review; #474 closure is gated
on that acceptance. F4 scoped retirement remains adopted/#505/#506 closed.
PhaseM terminal/adopted. #402 remains OPEN for F2/F5 then F6 final audit; V1release ready NO.
This receipt changes documentation/evidence only; it does not change measured build/test/
production inputs. Any later documentation-only master has input equivalence to this testedSHA,
not a relabeled execution identity.
