# AUD-005 (#643 Phase A): File close-releases-descriptor oracle repair

Test-only remediation of the P1 audit row AUD-005. The assertion at
`tests/file_resource_test.cpp:40` (base e16960ff) read
`fcntl(file.native_handle(), F_GETFD) == 0` after `File::close()` and was
inert in every reachable state:

- `File::close()` exchanges `fd_` to -1 before the native close and
  regardless of its outcome (`src/file_resource.cpp:110`), so the
  post-close `native_handle()` argument is -1 and `fcntl` returns -1, never 0.
- Every Sluice open carries `O_CLOEXEC` (`src/file_resource.cpp:32`), so a
  hypothetically still-live descriptor returns `FD_CLOEXEC` (= 1), never 0.

The named test `open_exposes_live_descriptor_and_close_releases_it` therefore
verified the "releases it" half of its own name with an assertion that cannot
fail; the close-skipping mutation below survived it.

## Repair (this branch)

`open_exposes_live_descriptor_and_close_releases_it` now follows the
established paradigm of `tests/file_close_semantics_test.cpp`
(`descriptor_is_live`, pre-close fd capture):

1. open a real temporary file, `File::open`, check `is_open()`;
2. capture `const int fd = file.native_handle()` BEFORE `close()`;
3. liveness precondition `fcntl(fd, F_GETFD) >= 0` (a live fd legitimately
   reports `FD_CLOEXEC`; never compared to 0);
4. `file.close()` Result must be success;
5. object reports closed: `!is_open()` and `native_handle() < 0`;
6. kernel oracle: `fcntl(fd, F_GETFD) == -1 && errno == EBADF` on the saved
   fd number — no allocation or open between the native close and this check;
7. repeated `file.close()` stays idempotent-success.

Tree-wide scan (grep `F_GETFD` in `tests/`, `src/`, `include/`, `apps/`,
`bench/`) found exactly one `== 0` instance, the repaired line; the remaining
uses are `< 0`/`>= 0` liveness forms or `== -1 && errno == EBADF` closed-proof
forms. Inside the repaired file the other two uses (`:36`, `:63`) are `< 0`
liveness checks and were already correct. No production file was touched;
`src/` and `include/` are unmodified on this branch.

## Mutation experiment (one-shot, disposable, never committed)

Mutation: `File::close()` returns success, `fd_` becomes -1, the kernel
`close(2)` is skipped (`docs/evidence/643-aud005-close-oracle-20261121/mutation.diff`,
guard `fd < 0 &&` is always false after the exchange, so `close_native` never
runs). Applied only in a disposable detached git worktree
`/tmp/sluice-aud005-mutant` at e16960ff; removed with
`git worktree remove --force` after capture (verified: worktree list clean,
directory gone). The mutation's leaked descriptors stayed inside the
disposable mutant processes; no mutation hook or retry-on-EINTR was added to
any tracked file, and the existing Linux EINTR close semantics in
`src/file_resource.cpp` are untouched.

Host: linux x86_64 (Fedora 44), gcc, xmake default platform config
(`xmake f -y`, release). Worktree `/tmp/sluice-643-aud005`, branch
`test/643-fd-close-oracle`, BASE_SHA e16960ff8be38d4c9773b40b595e7c1c63473d99
(origin/master).

| Record | Tree | Test | Result | Raw log |
| --- | --- | --- | --- | --- |
| BASE | e16960ff clean | old | exit 0, `all 3 file resource tests passed` | `base.log` |
| MUTANT | e16960ff + mutation.diff | old | exit 0, `all 3 file resource tests passed` — blindness demonstrated | `mutant.log` |
| FIXED | clean worktree, this branch | repaired | exit 0 (`file_resource_test`, `file_close_semantics_test` all pass) | `fixed.log` |
| KILLED | e16960ff + mutation.diff | repaired | exit 1, `FAIL: open_exposes_live_descriptor_and_close_releases_it` | `killed.log`, `killed.strace` |

Exact commands (in `/tmp/sluice-643-aud005` unless noted):

```sh
xmake f -y
xmake build file_resource_test file_close_semantics_test
xmake run file_resource_test
xmake run file_close_semantics_test

git -C /tmp/sluice-643-aud005 worktree add --detach /tmp/sluice-aud005-mutant e16960ff
# apply mutation.diff to /tmp/sluice-aud005-mutant/src/file_resource.cpp
cd /tmp/sluice-aud005-mutant && xmake f -y && xmake build file_resource_test
./build/linux/x86_64/release/file_resource_test          # MUTANT: exit 0
cp /tmp/sluice-643-aud005/tests/file_resource_test.cpp tests/file_resource_test.cpp
xmake build file_resource_test
./build/linux/x86_64/release/file_resource_test          # KILLED: exit 1
git checkout -- tests/file_resource_test.cpp src/file_resource.cpp
git -C /tmp/sluice-643-aud005 worktree remove --force /tmp/sluice-aud005-mutant
```

Killed assertion: within `open_exposes_live_descriptor_and_close_releases_it`
the only check observing kernel (not object) state is the saved-fd
`fcntl(fd, F_GETFD) != -1 || errno != EBADF` guard at the repaired
`tests/file_resource_test.cpp:44`; all other checks in the function observe
`fd_`/Result state the mutation preserves, so the kill lands there.
`killed.strace` shows the mechanism directly: after
`openat(...) = 3` there is no `close(3)`, and the second
`fcntl(3, F_GETFD)` returns `0x1 (FD_CLOEXEC)` — a live descriptor — so the
repaired guard fires; `fixed.strace` shows `close(3) = 0` followed by
`fcntl(3, F_GETFD) = -1 EBADF`. The traces also record the empirical core of
the audit row: a live O_CLOEXEC descriptor reports 1, never 0.

## Scope

Test-only change plus this evidence. No conformance-ledger status is altered
here; the AUD-005 ledger update belongs to the #643 orchestrator. Full
configuration matrix intentionally not run (orchestrator runs it later); this
slice ran the two named targets in the default gcc/linux release config.
