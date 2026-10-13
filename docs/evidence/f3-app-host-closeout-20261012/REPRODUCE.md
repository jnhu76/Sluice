# Reproduce this scoped candidate

All C++ build/run evidence was executed via SSH at jnhu@192.168.31.75.
Original repo /home/jnhu/Source/Sluice remained clean and unchanged.

Actual F4 master source de551ed09555b8ad4d118a250baf373062a01d0e:
`python3 scripts/verify_f4_postmerge.py --source <fresh-worktree> --master-sha de551ed09555b8ad4d118a250baf373062a01d0e --output <absent-directory>`.
Extract postmerge-f4 archive and run integrity-only `--verify-evidence <f4-postmerge-recheck-01>`.
The sealed note in this immutable raw archive incorrectly names8999; actual producer/source
identities de551 are authenticated. The tool note is corrected in this candidate; no rerun is claimed.
Independent technical reviewer accepted this exact merged-master evidence.

Application source613f9205308ecbcc7635f946fe0061f57b862e71: archive includes
run_app_matrix.py and run_app_sanitizers_native_allflags.py, full commands/status/logs,
red+green syscall witnesses, site capture and digests. Adapt root to a NEW isolated directory;
do not reuse existing worktrees/prefixes. Native Clang22 requires explicit redhat target in
cxxflags,cflags,ldflags,shflags for executable and preload-library runtime lookup.
ASan/UBSan and TSan each PASS on both profiles; earlier GNU/default/shflags attempts
remain BLOCKED and are preserved, not erased. Xmake detached statistics children were
terminated once to release inherited log pipes; intervention recorded. No test/build process killed.
App archive excludes build directories; logs and exact-source commands are evidence of execution,
not an assertion that binaries can be rerun without rebuilding. All209 normal matrix commands exit0.
All new syscall probes reset the independent destination sentinel per case and distinguish usage1,
migration diagnostics and absence of target opens; absence of leftover temporary output is asserted.

raw-archives.json binds SHA256/file counts; FILE_INDEX.json binds every archived member.
TAIL owner decision is still PENDING; PR513 Draft/#474 OPEN. No agent approval or automatic merge.
F4 adopted scoped/#505/#506 CLOSED; F2/F5/F6 pending, optional W04 unsupported, release NO.

Supplemental instrumentation archive binds18 actual executed test binary SHA256 and nm inventories, app object sanitizer references, and four native compile databases with -fsanitize flags. capture_f3_sanitizer_instrumentation.py reproduces nm collection against preserved builds; no runtime replay is claimed.
