# PR A remote receipt — 2026-10-12

Authoritative final run: `/home/jnhu/f3-498-execution/pr-a-final-matrix`.
Tested C++/configuration SHA: `3f846afa37101546fa151adbd2e19c1b08f04ea0`.
[summary.json](summary.json) is the directly reviewable receipt; the final
commit adds evidence/docs only. Initial attempts at
`/home/jnhu/f3-498-execution/pr-a-matrix` remain unchanged, including their
FAIL statuses; they are not the authoritative final matrix.

Debug/Release × liburing=n/y: 40/40, 52/52, 40/40, 52/52 targets PASS.
Both real-ring smoke runs report all eight cases passed. ASan/UBSan: 13/13
PASS. TSan: twelve targets PASS with the static runtime; the unchanged
RequestScope suite reports 25/25 PASS with the installed shared TSan runtime.
The static scope link failed because its allocation-probe new/delete overrides
collide with strong definitions in tsan_cxx.a. Switching that one suite to
`-shared-libsan` retains every assertion and every `-fsanitize=thread` compile/
link flag; nm and ldd prove its instrumented calls and actual runtime loading.
No sanitizer or allocation oracle is disabled. These executions are evidence
for exercised schedules, not a protocol completeness proof.

Every profile has its own source worktree, `.xmake`, fresh `build-final` output
and logs. All final builds use `/usr/bin/g++` or `/usr/bin/clang++`,
`CCACHE_DISABLE=1` and `--ccache=n`. A full strict fresh-output rerun followed
the discovery that the earlier PATH selected system ccache wrappers despite
xmake's cache option; those earlier receipts are retained as history.

Actual resolved source/object lists contain 50 library objects, all present;
archive members remain core=13, async=37. Each install has the same 79 headers
as adopted #499, with no path delta. All nine migrated/include-only compiler
closures and linked consumer symbols contain zero legacy facade matches.
Installed-only canonical Request/RequestScope plus optional StackfulIoHost
compile/link/execute probes pass in all four profiles. No source, archive
member, installed header or public API is retired here, and this is not a
new B-01 contraction/adoption baseline.

Four existing mutants are killed by actual executions: scope commit drops
ownership, timeout releases a live slot, destructor skips settlement, and
io_uring zero-op dispatches instead of publishing at acceptance. Their logs
retain the observed invariant/fail-fast failures. strace records actual
fdatasync/fsync and /dev/null EINVAL for the two durability test families.

## Reproduction

Extract `raw-server-build-test-package.tar.gz` into a new remote receipt
directory. `raw-sha256.json` hashes every extracted raw file by relative path.
The archive includes driver/capture scripts, all build/run logs, exact SHA and
configuration receipts, dependency files, full selected-consumer nm output,
link maps, installed inventories/hashes, resolved build graphs, syscall traces,
and preserved first-attempt failures. Installed binary bytes are omitted; their
archive hashes and full build/install receipts are retained. Historical F1/F3
assets are unchanged.

Create new detached worktrees of the tested SHA before replaying. Update the
scripts' original absolute worktree/receipt prefixes to those new directories;
keep one source and output directory per profile. The recorded base commands are:

```sh
export CCACHE_DISABLE=1
xmake f -y -o build-final --ccache=n --cxx=/usr/bin/g++ --cc=/usr/bin/gcc     -m debug --liburing=n
xmake build -v -j 2 TARGET
timeout 240 xmake run TARGET
```

`run-profile.sh` carries the exact 40/52 target lists and Debug/Release/n/y
configuration selection. `capture-package.sh` compiles its probe using only the
fresh installed include prefix and installed archives; `resolve.lua` records
xmake's actual resolved sources, objects and target dependencies.

Sanitizers use Clang 22 on this Fedora server, whose runtimes reside in
`/usr/lib/clang/22/lib/x86_64-redhat-linux-gnu`:

```sh
xmake f -y -o build-final --ccache=n --toolchain=clang     --cxx=/usr/bin/clang++ --cc=/usr/bin/clang -m asanubsan --liburing=n     --ldflags=--target=x86_64-redhat-linux-gnu
# static TSan: same configuration with -m tsan
# scope shared TSan: -m tsan and the exact linker option below
--ldflags='--target=x86_64-redhat-linux-gnu -shared-libsan -Wl,-rpath,/usr/lib/clang/22/lib/x86_64-redhat-linux-gnu'
```

`run-scope-tsan-shared.sh` preserves the actual shared-runtime build/run/nm/ldd
commands. `run-controls.sh` treats nonzero mutant exits as observations, not
regular passing tests. The real-liburing profiles require successful real-ring
construction; an unavailable backend must remain an honest NOT-RUN limitation.
Independent final-head review and owner merge authorization remain separate.
