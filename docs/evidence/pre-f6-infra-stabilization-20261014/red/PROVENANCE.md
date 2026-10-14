# Provenance of the RED captures

All files in this directory are raw logs from `f99acbc3` (post-F5
origin/master) before any of this slice's changes, captured in throwaway
git worktrees — the source content is `f99acbc3` exactly.

- `f519-full2.log` — full Clang asanubsan build (`xmake -a`) in the
  `/tmp/f519-work` worktree after
  `xmake f -y -m asanubsan --liburing=n --ccache=n --toolchain=clang
  --ldflags=--target=x86_64-redhat-linux-gnu -o /tmp/f519-full2-build`;
  fails at `include/sluice/async/request_scope.hpp:204`
  (`-Werror,-Wunused-private-field`).
- `f519-m1.log` — `xmake build -y request_scope_mut_destructor_skips_settle`
  in the same worktree/config: the only mutation target that fails.
- `f519-m2.log` — `xmake build -y request_scope_mut_finish_cancels_cleanup`
  in the same worktree/config: builds successfully.
- `f520-same-dir-switch/` — n→y→n→y in the `/tmp/f520-work` worktree with
  the shared build dir `/tmp/f520-shared-build`, no cleaning between
  steps; per-phase snapshots under `after-*` (object sha256s, archive
  members, demangled archive symbols, uring ctor symbols, effective
  `xmake.conf`); `identity.txt` records the worktree head and build dir;
  `step-6y-ring-consumer.log` shows the ring consumer target definition,
  `step-12y-tests.log` the failing y-phase test run (exit 255 on the
  `uring_public_consumer_probe` link).
