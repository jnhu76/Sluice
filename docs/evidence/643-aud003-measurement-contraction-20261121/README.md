# 643 AUD-003 — installed measurement API contraction

Deterministic validation record for audit finding AUD-003 (#643, Phase B):
removal of the six zero-consumer legacy diagnostic structs from
`include/sluice/measurement.hpp`.

- BASE_SHA: `0601e1034eebab1853395f556a7fa4ff90a27bb0` (master, PR #646 head;
  AUD-005/006 dependency merged, CI green on that merge)
- PR-head change SHA: `f4f8dece058d1c2eb53e84c98be20efac7f99720`
  (`api/643-measurement-contraction`)
- Change: delete `sluice::SyscallStats`, `BufferStats`, `CopyStats`,
  `SyncStats`, `UringStats`, `VectorStats` (measurement.hpp:7-81, 75 lines);
  `AsyncStats` byte-for-byte unchanged; no other file touched by the C++ edit.
- Environment: Fedora 44, kernel 7.3.3-200.fc44.x86_64, g++ 16.2.1
  (Red Hat 16.2.1-3), xmake 3.1.2, Python 3.14.10, system liburing 2.13
  (`/usr/include/liburing.h`, resolved on default search paths).

## Consumer inventory (commands and results)

Repository-wide search at BASE `0601e103`:

    grep -rn 'SyscallStats\|BufferStats\|SyncStats\|UringStats\|VectorStats' \
        src include apps tests bench scripts xmake formal .github
    → only the five self-declarations in include/sluice/measurement.hpp
    (:7,:16,:56,:63,:72); zero other hits. At the PR head the same search
    returns zero hits in the live tree. (CopyStats is handled separately
    because sluice_copy::CopyStats is a distinct, live type in
    apps/sluice-copy/copy_task.hpp:36, reached unqualified through
    `using namespace sluice_copy` in its consumers.)

    grep -rn 'sluice::CopyStats' src include apps tests bench scripts xmake
    → zero hits at BASE (the declaration at measurement.hpp:35 is the
    unqualified `struct CopyStats` inside `namespace sluice`; no consumer
    ever spelled the qualified name) and zero at the PR head.

Non-archive documentation (`CHANGELOG.md`, `README.md`, `docs/README.md`)
mentions none of the six names; the only textual occurrences outside the
header are historical records under `docs/archive/` and the archived
`docs/research/575-native-e5-atomic-rmw-archive/` (v1-era audits whose
slice-P plan to prune measurement.hpp to AsyncStats only was never
executed).
`scripts/verify_v2_package.py` and `xmake/libraries.lua` reference the header
`sluice/measurement.hpp` (unchanged, still installed) but none of the six
types. Git history: the last in-tree consumers of all six structs were
deleted by `5679f23c` (retire #505-B, 2026-10-12), which did not touch
measurement.hpp itself; no compatibility commitment to these names exists
anywhere in history (v0.0.1 was tag-only, no stability promise).

In-repo consumers: **zero for all six types**; `AsyncStats` remains consumed
by `src/async/*`, the installed async headers, tests and bench (untouched).
External consumers: **unknown and unknowable from this repository** —
disclosed per D3, not assumed absent.

## Verification evidence

1. `xmake f -m debug -y --liburing=y && xmake -y && xmake test -y`
   → 84/84 passed (`debug-suite-liburing-y.log`).
2. `xmake f -m debug -y --liburing=n && xmake -y && xmake test -y`
   → 61/61 passed (`debug-suite-liburing-n.log`).
3. `scripts/verify_v2_package.py --mode release [--liburing --uring-prefix /usr]`
   in a **clean isolated worktree** at BASE and at the PR head:
   - `verify-base-{noliburing,liburing}.log` — BASE: all gates PASS (24/24
     each profile).
   - `verify-post-{noliburing,liburing}.log` — PR head: all gates PASS
     (24/24 each profile, `V2_B_PROVENANCE_RECORDED` clean at `f4f8dece`).
   - `v2-package-manifest-{base-,}{noliburing,liburing}.json` + archive
     baselines — the four recorded runs. The first liburing post attempt
     logged a provenance FAIL caused only by the previous profile's manifest
     sitting in the same worktree (non-output dirt); the functional gates
     all passed and the clean-worktree rerun recorded here is fully green.
4. `surface-comparison.txt` — pre/post archive-baseline comparison, both
   profiles: `libsluice_core.a` and `libsluice_async.a` **byte-identical**
   (same archive sha256, same per-object sha256, same defined symbols, same
   external undefined symbols), installed header set identical (11 headers).
   The six removed types were header-only and emitted no code.
5. `cleanroom_asyncstats_consumer.cpp` + `cleanroom-probes-post.log` —
   clean-room consumer compiled/linked/run against the installed prefix
   only (angle-bracket include of `<sluice/measurement.hpp>`, `-lsluice_core`):
   constructs, copies and reads back `AsyncStats`; `sizeof == 88` (11
   counters), identical binary behavior against a BASE-installed prefix.
   Six negative probes: `sluice::{SyscallStats,BufferStats,CopyStats,
   SyncStats,UringStats,VectorStats}` each **fail to compile** from the
   installed PR-head prefix (no retired name remains in the public header).
6. Sole-include standalone compilation of `measurement.hpp` (also covered
   as one of the verifier's 11 sole-include compiles).

## Limitations

- External consumers of the removed names cannot be enumerated; the
  source-breaking impact is disclosed (CHANGELOG `[Unreleased]`, D3), and
  no binary-compatibility claim beyond the recorded identical-archives
  evidence is made.
- The verifier runs record head `f4f8dece`; the final PR head differs from
  it only by this evidence directory and the ledger row (documentation-only,
  no C++/build input change).
- Sanitizer cells were not rerun for this slice: the change removes unused
  declarations and the release archives are byte-identical pre/post in both
  liburing profiles; the debug suites (84/84, 61/61) cover in-tree behavior.
- The package verifier's gates are content-insensitive for
  `measurement.hpp` beyond prefix membership and sole-include compilation
  (no package consumer uses `AsyncStats`); the removal-specific proof is
  the recorded clean-room positive/negative probes in this directory, not
  the verifier. Making the standing gate itself hold this boundary
  (AsyncStats consumer + retired-name negative probes in
  `tests/package/consumers/`) is a possible future owner slice, out of
  scope here.
