# Receipt — unit `liburing`: --liburing=y / --liburing=n sequential legs in one worktree

- date: 2026-10-29, start 23:50:43+08:00, end 23:58:45+08:00
- executor: WSL2 verification subagent (unit: liburing)
- repo baseline: master @ `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
  (verified: `git -C /tmp/sluice-val-liburing log -1` → `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b perf(direct): P2C1 read compositions run prevalidated register-resident steps (#608) (#612)`)
- worktree: `/tmp/sluice-val-liburing` (detached HEAD at the commit above; both legs ran here sequentially — the only worktree used)
- toolchain: xmake v3.1.2+HEAD.5d21e9d, gcc via xmake default, 8 CPUs, WSL2 kernel `6.18.54.1-microsoft-standard-WSL2`

## Command-matrix adjustment (only one)

The scout matrix line said `cd /tmp/sluice-val-release` — a cross-unit template
artifact. This unit's designated worktree is `/tmp/sluice-val-liburing`
(per the ask: "一切构建/测试先 cd 进该 Worktree 再执行"), so both legs ran
there. Every other command was executed verbatim and accepted by xmake v3.1.2:
the `--liburing=y|n` switch is real (`option("liburing")` at
`xmake/uring.lua:36-39`, default `false`). No other adjustment was needed.

## Environment (verified before leg 1)

```
$ ls -la /usr/include/liburing.h
-rw-r--r-- 1 root root 62116 Feb 13  2026 /usr/include/liburing.h
$ ldconfig -p | grep -i liburing
	liburing.so.2 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/liburing.so.2
	liburing.so (libc6,x86_64) => /usr/lib/x86_64-linux-gnu/liburing.so
	liburing-ffi.so.2 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/liburing-ffi.so.2
	liburing-ffi.so (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/liburing-ffi.so
$ cat /proc/sys/kernel/io_uring_disabled
0
```

Matches the pre-existing E5 environment receipt `04-liburing.txt`
(liburing 2.14 via pkg-config/dpkg, same header size/timestamp).

Static enumeration reference (receipts/cmd-01-test-set-enumeration.txt):
58 test targets under `--liburing=n`, +21 `has_config("liburing")`-only
targets under `--liburing=y`, total 79.

## Leg 1 — enabled (`--liburing=y`)

```
$ cd /tmp/sluice-val-liburing        # matrix said /tmp/sluice-val-release; adjusted (see above)
$ xmake f -c -m release --liburing=y -y
checking for platform ... linux (x86_64)
checking for liburing capability ... ok (/usr/include/liburing.h + /usr/lib/gcc/x86_64-linux-gnu/15/../../../x86_64-linux-gnu/liburing.so)
checking for sluice_liburing_capability ... ok
warning: Ascend SDK not found
exit=0        # wall 2.599s (1.01s user) via `time`
$ xmake -y
[100%]: build ok, spent 2.098s
BUILD_EXIT=0  # wall 2.699s
$ xmake test -y
100% tests passed, 0 test(s) failed out of 79, spent 3.993s
TEST_EXIT=0   # wall 1:27.61, 563.76s user (includes on-demand build of all test targets)
```

- Expected 79/79 → observed **79/79 passed, 0 failed** (`grep -c ' passed '`
  on the color-stripped log = 79; 0 skipped; the only `grep -i failed` hits are
  compile lines for the source file `src/async/fail_fast.cpp`, not failures).
- Capability probe ran at configure and resolved the real header + shared
  object — consistent with `xmake/uring.lua:117-301` and the "never silently
  falls back" contract (`xmake/uring.lua:4-5`).
- All 21 `has_config("liburing")`-only targets registered (`xmake/tests.lua:126`)
  and passed (verified individually, 21/21), including
  `uring_backend_smoke_test` ("the smoke drives real submissions",
  `xmake/tests.lua:124-125`) and `uring_public_consumer_probe` ("The probe
  witnesses the consumer-side definition contract", `xmake/tests.lua:125-127`,
  i.e. `SLUICE_HAS_LIBURING` reaches consumers through sluice_async's public
  usage requirement, `xmake/libraries.lua:66-81`).
- Note: the `xmake -y` leg-1 log is short (only `sluice_core` recompiled 2
  files + archive) because test targets are `set_default(false)`
  (`xmake/tests.lua:64-69` et al.) and are built on demand by `xmake test`;
  the full compile volume for all 79 test binaries appears inside the leg-1
  test log. Raw logs: `/tmp/liburing-val-logs/leg1-{configure,build,test}.log`.
- WSL2 status: this leg is NEW evidence on this host (no prior local y-leg run
  existed in this audit), matching the scout's "on WSL2 this leg is NEW
  evidence, not a reproduction" note.

## Leg 2 — disabled (`--liburing=n`), same worktree, immediately after leg 1

```
$ cd /tmp/sluice-val-liburing
$ xmake f -c -m release --liburing=n -y
checking for platform ... linux (x86_64)
checking for sluice_liburing_capability ... ok
warning: Ascend SDK not found
exit=0        # wall 2.794s (0.95s user)
$ xmake -y
[100%]: build ok, spent 3.294s
BUILD_EXIT=0  # wall 5.331s
$ xmake test -y
100% tests passed, 0 test(s) failed out of 58, spent 4.019s
TEST_EXIT=0   # wall 14.568s, 55.03s user
```

- Expected 58/58 (E5 reference r4 §6) → observed **58/58 passed, 0 failed**
  (`grep -c ' passed '` = 58).
- No capability probe at configure (absence of the "checking for liburing
  capability" line) — probe is gated on `has_config("liburing")`
  (`xmake/uring.lua:121`).
- Zero uring-only tests in the run set: the only "uring" matches in the log are
  `uring_submit_boundary_test` (registered unconditionally; listed in cmd-01's
  n-leg 58) and stub-profile recompiles of the backend sources.
- Honest-stub macro evidence (post-leg-2 `compile_commands.json`, 37 command
  entries carrying the define):
  - `grep -o 'SLUICE_HAS_LIBURING' compile_commands.json | wc -l` → **0**
  - `grep -o '\-D *SLUICE_LIBURING_PROFILE=[a-z]*' compile_commands.json | sort | uniq -c`
    → `37 -DSLUICE_LIBURING_PROFILE=off`
  Raw logs: `/tmp/liburing-val-logs/leg2-{configure,build,test}.log`.

## Summary

| leg | configure | build | test | result |
|---|---|---|---|---|
| `--liburing=y` | exit 0 (probe ok) | exit 0 | exit 0 | 79/79 passed, 0 failed |
| `--liburing=n` | exit 0 (no probe) | exit 0 | exit 0 | 58/58 passed, 0 failed |

Both expectations from the command matrix hold exactly (79/79 and 58/58).
No test was skipped; no assertion was weakened; nothing was rebuilt to force
green. Status: **ADAPTED** (sole adjustment: worktree path per unit assignment;
both legs otherwise verbatim and fully green).
