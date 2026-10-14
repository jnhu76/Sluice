# Pre-F6 infrastructure stabilization — #520, #519, #521

Post-F5 build/test infrastructure slice. Baseline
`f99acbc3` (post-F5 origin/master; F5 = PR #518, merged). Branch
`codex/pre-f6-infra-520-519-521`, three commits, each independently
revertable:

| Commit | Issue | Content |
|---|---|---|
| `5d611b28` | #520 | ring-profile switch identity (`SLUICE_LIBURING_PROFILE`), build-time liburing capability detection, `scripts/verify_liburing_config_switch.py` |
| `7f9d43c7` | #519 | target-local Clang-only `-Wno-unused-private-field` on the M4 mutation target |
| `cdb2ca0f` | #521 | `SLUICE_TSAN_TESTING` seam: the request-scope zero-allocation oracle compiles only outside TSan |

No production semantic change: `--liburing=n` keeps the honest stub and
`--liburing=y` the real ring; `SLUICE_HAS_LIBURING` remains the only public
usage requirement and is still defined only in the enabled branch; the
installed surface is unchanged (25 headers, 13 async archive members —
measured below, both ring profiles). One new public compile definition
(`SLUICE_LIBURING_PROFILE=on|off`) is added to `sluice_async` and the
stackful seam targets as configuration identity; it carries no layout
statement. `SLUICE_TSAN_TESTING` is test-target-local and never installed.
This is recorded as a post-F5 infrastructure change; no F5 evidence file
was modified (the B-01 v6 capture, matrix and installed probes under
`docs/evidence/f5-final-package-contraction-20261013/` are untouched
history).

Host: Fedora 44, kernel 7.2.9-201.fc44, GCC 16.2.1, Clang 22.1.8, liburing
2.13 (`/usr/include/liburing.h`, `/usr/lib64/liburing.so.2.13`), xmake
`/home/jnhu/.local/bin/xmake` (recorded in `matrix/toolchain.log`).

## #520 — stale ring profile reused across a same-directory switch

RED at `f99acbc3` (`red/f520-same-dir-switch/`, shared worktree
`/tmp/f520-work`, shared build dir `/tmp/f520-shared-build`, n→y→n→y with no
cleaning):

- y1 built the real-ring profile (`sluice_async` uring object
  `bfe265ef…`, stackful seam `9bf40c51…`).
- The y→n switch recompiled the ring-only seam targets but left
  `sluice_async` and `stackful_host_test` at their ring-profile objects —
  the n2 archive kept ring ctors under `--liburing=n`.
- The n→y switch reused the stub objects everywhere: every y2 seam object
  equals the n2 stub hash `b5f4a300…` — `--liburing=y` resolving to stub
  identity.
- `step-12y-tests.log`: the ring consumer `uring_public_consumer_probe`
  fails to link against the reused archive (`undefined reference to
  sluice::async::UringAsyncBackend::UringAsyncBackend(sluice::async::UringConfig)`,
  exit 255) — the stale layout fails loudly at a consumer, after the
  archive was already wrong.

Root cause (traced through the installed xmake sources at
`~/.local/share/xmake`, `core/project/depend.lua` `is_changed` and
`core/cache/object.lua`): the per-object depend check compares the stored
flag lists index by index, so a define appended at the tail by a config
switch (the conditional `SLUICE_HAS_LIBURING`) is invisible — additions are
not detected, only removals and in-place changes. The conditional define
was therefore structurally unable to trigger recompilation in the n→y
direction.

Fix: `SLUICE_LIBURING_PROFILE=on|off` is defined unconditionally and
publicly on `sluice_async` (and on the stackful seam targets, which exist
under both profiles), always occupying the same flag position with a
profile-dependent value; every switch changes an existing index, and the
affected target and all consumers recompile. `xmake/uring.lua` owns the
option and the capability detection.

Build-time capability detection (configure only, never executes a binary):
a hidden option `__sluice_liburing_capability` whose check compiles and
links a probe of the exact surface the uring TUs use — 24 required
functions (including `io_uring_queue_init`, `io_uring_get_probe_ring`,
`io_uring_submit_and_wait`, `io_uring_prep_statx`,
`io_uring_prep_cancel64`) and 12 required constants — with the resolved
header and library directories pinned explicitly, and an explicit refusal
when header and library resolve to different installation prefixes. Kernel
capabilities (ring creation, required opcodes, statx) stay owned by the
`UringAsyncBackend` construction path with its existing runtime probes;
nothing new was added there.

GREEN at `cdb2ca0f` (`switch-regression/`, `identity.txt` records
`worktree_head=cdb2ca0f…`):

- n→y→n→y in one build dir with no cleaning: every phase asserts the
  effective option, the stored depend fingerprints of `sluice_async` and a
  consumer (`SLUICE_LIBURING_PROFILE=on/off` plus
  `SLUICE_HAS_LIBURING` matching the profile), a changed
  `uring_backend.cpp.o` hash on every switch, the archive ctor identity
  (3 ring ctors under y, the stub ctor only under n), 13 members in both
  profiles, and 52/52 (n) / 66/66 (y) registered tests.
- Four fault-injection negatives through the real `xmake f` entry point
  (mount namespaces via `unshare -r -m`): missing header → declaration
  layer (`liburing.h: No such file`), missing library → link layer
  (`required external symbols` + undefined references), incompatible
  required API → declaration layer (`was not declared`),
  conflicting include/library prefixes → prefix refusal. The
  missing-library case fails at configure now — the RED's late consumer
  link failure became an early, accurate configure failure.
- `CLEANING_NOT_REQUIRED_FOR_SUPPORTED_WORKFLOW = YES`: no forced rebuild
  anywhere; the supported workflow is configure + build, and the switch
  itself performs exactly the recompilation the profile change implies.

## #519 — Clang build failure of the D1 mutation targets

RED at `f99acbc3`: the full Clang asanubsan build fails at
`include/sluice/async/request_scope.hpp:204` — `policy_` unused
`[-Werror,-Wunused-private-field]` (`red/f519-full2.log`). Per target:
only `request_scope_mut_destructor_skips_settle` fails (`red/f519-m1.log`);
`request_scope_mut_finish_cancels_cleanup` already compiled (`red/f519-m2.log`)
— the issue's count was conservative, one target is affected.

The mutation removes the destructor's settle call, which is also the TU's
only read of `policy_`; the warning is default-on under Clang and promoted
by `-Werror`. The fix scopes `-Wno-unused-private-field` to that one
target and to Clang tools only (`{tools = {"clang", "clang_cl"}}`); the
global `-Werror` and every GCC profile are untouched.

Kills at `cdb2ca0f` (`mutation-killers/`, clang asanubsan, distinguishable
from compile failure/timeout/sanitizer crash — both mutants build first,
then die on assertions):

- M4 `SLUICE_D1_MUTANT_DESTRUCTOR_SKIPS_SETTLE`: exit 134, the recorded
  kill signature — `Request` contract violation abort on destroying a
  scope with unsettled accepted work. First kill at
  `destructor_cleanup_preserves_the_original_exception_and_still_releases`
  (`killers-m1-bt.log`, request_scope_test.cpp:688). With the suite order
  temporarily rearranged in a throwaway build so the ledger's recorded
  killer runs first, `destructor_settles_on_early_return_with_drain_policy`
  kills the mutant the same way (`killers-m1-iso-bt.log`, :606); the test
  file was restored and the working tree is clean.
- M6 `SLUICE_D1_MUTANT_FINISH_CANCELS_CLEANUP`: exit 1, `1 of 25 request
  scope tests failed` on the named case
  `explicit_finish_does_not_invoke_the_cleanup_cancel`
  (`killers-m2-run.log`).

RequestScope semantics are unchanged: the non-mutant suite passes 25/25 in
every non-TSan profile and 24/24 under TSan (#521), and all six matrix
profiles are green.

## #521 — the request-scope zero-allocation oracle under TSan

Audit (tree-wide): the global allocation seam — strong `operator
new/new[]/delete/delete[]` replacements, `g_allocations`,
`AllocationProbe` — exists only in `tests/request_scope_test.cpp`; no
other test, app or production TU overrides global allocation. Exactly one
case depends on it: `submit_path_allocates_nothing_after_acceptance`, with
seven assertions — acceptance zero-alloc ("reserve, acceptance, and the
post-accept commit allocate nothing"), take/release zero-alloc, capacity
rejection zero-alloc, the rejection result, explicit finish, and pipeline
reclaim — plus the acceptance itself.

The TSan runtime ships its own allocator interceptors, so the strong
overrides collided at link time (`multiple definition of 'operator new'`)
and the F5 runner excluded the whole `request_scope_test` target from
TSan (expected 51 with a required link-failure record). The seam now
compiles only when the target defines `SLUICE_TSAN_TESTING` (set by
`request_scope_target` in TSan mode): TSan builds, links and runs the
remaining 24 RequestScope cases for real; every non-TSan profile owns the
zero-allocation oracle (25 cases). The request_scope_uring variant carries
the same oracle in the ring profiles. No non-allocation assertion was
removed; the excluded case's coverage mapping is: debug-n, release-n,
asanubsan-n (registered runs), debug-y/release-y via
`request_scope_uring_test`.

Evidence at `cdb2ca0f`, bound to the matrix's own binaries
(`request-scope-tsan/`):

- `head-dbg-rs.log`: 25 of 25 passed (oracle included); seam symbols
  present (3).
- `head-tsan-rs.log`: 24 of 24 passed under
  `TSAN_OPTIONS=halt_on_error=1`; seam symbols absent (0).
- `matrix/tsan-n.log`: the full 52-test registered run including
  `request_scope_test`, green.

## Integrated verification (all at `cdb2ca0f`)

- Six-profile matrix from fresh build dirs (`matrix/`): debug-n 52/52,
  debug-y 66/66, release-n 52/52, release-y 66/66, asanubsan-n 52/52,
  tsan-n 52/52 — `PRE_F6_MATRIX_SIX_PROFILES_PASS`. The ring profiles
  exercise the real io_uring backend on this host
  (`backend_conformance_e1_uring_test` and the other ring-only tests).
- Same-directory switch regression + negatives: `switch-regression/`.
- Mutation killers: `mutation-killers/`.
- B-01 v7 package capture (`b01-v7-capture/`): the F5 v6 harness
  (`reproduce.py`, `persist.py` — copied verbatim, unmodified) run as
  `--before 14a27a83 --after cdb2ca0f`, four clean-room builds (both SHAs
  × both ring profiles). Counts before [32 objects, 30 members, 49
  headers] → after [15, 13, 25] in both ring profiles: the installed
  header set stays 25 and `libsluice_async.a` keeps 13 members. The W-01
  (`direct_w01_consumer_probe` closure), W-02 and W-03 consumers link and
  run from the fresh prefixes in both profiles, alongside the contract
  families and the W-04 candidate host. Durable subset in
  `b01-v7-capture/durable/` (928 text artifacts); the unmodified harness
  printed `F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS 14a27a83… cdb2ca0f…`
  (its version tag names the script generation, the SHAs name the run);
  the execution tree is re-derivable with `reproduce.py`.
- Installed probes (`installed-probes/probes-n`, `probes-y`): 25 headers
  each compile standalone in both macro views, the cross-TU ODR probe
  links and runs, removed headers are absent, with the 49-header before
  prefix as the positive control.
- Historical data: the F5 matrix, B-01 v6 capture, installed probes and
  known-preexisting findings are unmodified; the F5 `tsan-n` exclusion
  record remains the truthful description of the F5 head, and this slice
  supersedes it going forward.

## Reproduction

```
# matrix (six profiles)
python3 docs/evidence/pre-f6-infra-stabilization-20261014/matrix/run-matrix.py \
    --repo <checkout at cdb2ca0f> --output <fresh dir>
# same-dir switch cycle + negatives (needs user namespaces)
python3 scripts/verify_liburing_config_switch.py --workdir <checkout> \
    --builddir <fresh> --output <fresh dir>
# mutation killers (clang asanubsan)
xmake f -y -m asanubsan --liburing=n --ccache=n --toolchain=clang \
    --ldflags=--target=x86_64-redhat-linux-gnu -o <fresh>
xmake build -y request_scope_mut_destructor_skips_settle request_scope_mut_finish_cancels_cleanup
# (build them one at a time; run each; M4 exits 134, M6 exits 1)
# B-01 v7 capture
python3 docs/evidence/pre-f6-infra-stabilization-20261014/b01-v7-capture/reproduce.py \
    --repo <clone> --before 14a27a83 --after cdb2ca0f --output <fresh>
python3 docs/evidence/pre-f6-infra-stabilization-20261014/b01-v7-capture/persist.py \
    --capture <output> --destination <fresh>
# installed probes
python3 docs/evidence/f5-final-package-contraction-20261013/installed-probes/run-probes.py \
    --prefix <v7>/after-installed-n --before-prefix <v7>/before-installed-n \
    --output <fresh>/probes-n --source-sha cdb2ca0f…
```

## Notes and limitations

- The configure-time capability probes re-run on every
  `xmake f --liburing=y` (two compile+link probes, sub-second): capability
  state is external and must not be cached across configures. The
  negatives prove they really re-run — each injection bites at its own
  layer on a fresh configure.
- The negatives need user namespaces; on hosts without them the driver
  prints a skip notice and continues (this host ran all four).
- `lib.detect.check_cxxsnippets` reads links and search paths only from
  its `configs` argument; links passed outside `configs` silently skip the
  link step. The capability probes pin `includedirs`, `linkdirs` and
  `links` through `configs` for exactly this reason (the missing-library
  negative catches a regression here).
- The `raise` used by the capability refusals lives in the option-check
  sandbox; the project-file scope has no `raise`, so
  `_report_liburing_paths` returns the refusal message for the check
  callback to raise.
- The M4 isolation experiment temporarily reordered `tests[]` in the
  working tree; it was reverted before recording (tree clean at
  `cdb2ca0f`).
