# F-10 RED capture — probe/production dependency-identity divergence

- Source under test: `5ab32ae7590158c1a8231cba8e4dfb29d16f20b2` (PR #522 head at
  review round 2), git worktree `/tmp/f10-head-work`.
- Driver: `scripts/verify_liburing_identity_binding.py --workdir /tmp/f10-head-work
  --output /tmp/f10-red-capture` (script committed in this commit).
- `traces.log` — what the toolchain itself resolves in each environment
  (`g++ -E -H` for `#include <liburing.h>`, `g++ -Wl,--trace` for `-luring`).
- `divergence-configure.log` — `xmake f --liburing=y` under
  `C_INCLUDE_PATH=<full header copy>` + `LIBRARY_PATH=<library copy>`: the old
  capability check ACCEPTS and prints
  `/tmp/f10-red-capture/stub-good/include/liburing.h + /tmp/f10-red-capture/fakelib/liburing.so`,
  files the production compile and link never open (the toolchain keeps
  `/usr/include/liburing.h + /usr/lib64/liburing.so`).
- `negative-deficient-header.log` — a CPATH-installed header missing
  `io_uring_get_probe_ring` is refused at the declaration layer already on this
  head (the pinned `-I` ranks behind `CPATH`, so probe and production compile
  the same deficient file); this negative stays valid under the fix.
- `precedence-study.log` — the raw resolution-order facts the candidate list in
  `xmake/uring.lua` contradicted: C++ include order CPATH → CPLUS_INCLUDE_PATH →
  default dirs (C_INCLUDE_PATH not consulted for C++; env paths rank ahead of
  `-I`), linker order `-L` → configured dirs → `LIBRARY_PATH`.
- Scratch paths under /tmp are ephemeral; every committed log is the captured
  stdout/stderr of the recorded commands.
