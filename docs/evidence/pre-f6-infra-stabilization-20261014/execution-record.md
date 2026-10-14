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

- n1 built the stub profile: `sluice_async` uring object `bfe265ef…` and
  stackful seam `9bf40c51…` (stub ctor only).
- The n→y switch recompiled nothing of the existing targets
  (`step-5y-build.log` has zero compile lines): the tail-appended
  `SLUICE_HAS_LIBURING` is invisible to the depend check, so `sluice_async`
  and `stackful_host_test` keep the n1 stub objects — the after-y1 archive
  carries only the stub ctor (zero `UringConfig` symbols in its nm dump)
  under `--liburing=y`. The y-only seam targets compile for the first time
  in this directory (ring objects `b5f4a300…`, absent from the after-y1
  capture), and the ring consumer then fails loudly:
  `uring_public_consumer_probe` cannot link against the stale archive
  (`undefined reference to
  sluice::async::UringAsyncBackend::UringAsyncBackend(sluice::async::UringConfig)`,
  exit 255; `step-6y-ring-consumer.log`, `step-12y-tests.log`).
- The y→n switch is equally invisible (`step-8n-build.log` and
  `step-9n-tests.log` have zero compile lines): the stub objects persist,
  which under n is coincidentally the correct content, so the n2 tests
  pass 52/52; the ring seam objects remain in the build directory.
- The second n→y switch changes nothing again — the stub archive is
  retained under `--liburing=y` and the consumer link failure repeats. The
  defect is symmetric: no switch in either direction recompiles anything.

Root cause (traced through the installed xmake sources at
`~/.local/share/xmake`, `core/project/depend.lua` `is_changed` and
`core/cache/object.lua`): the per-object depend check compares the stored
flag lists index by index from the current list's side, so a define
appended at the tail by a config switch (the conditional
`SLUICE_HAS_LIBURING`) is invisible in both directions — an addition is
not detected and neither is the removal of a tail entry. The conditional
define was therefore structurally unable to trigger recompilation on any
switch.

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
— the issue overcounted, one target is affected.

The mutation removes the destructor's settle call, which is also the TU's
only read of `policy_`; the warning is default-on under Clang and promoted
by `-Werror`. The fix scopes `-Wno-unused-private-field` to that one
target and to Clang tools only (`{tools = {"clang", "clang_cl"}}`); the
global `-Werror` and every GCC profile are untouched.

Kills at `cdb2ca0f` (`mutation-killers/`, clang asanubsan, distinguishable
from compile failure/timeout/sanitizer crash — both mutants build first,
then die on assertions; `killers-exit-codes.log` records the exit codes):

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

- `head-dbg-rs.log`: 25 of 25 passed (oracle included). The debug binary
  defines the seam: 3 `g_allocations`/`AllocationProbe` symbols and 6
  strong `operator new/delete` replacements.
- `head-tsan-rs.log`: 24 of 24 passed under
  `TSAN_OPTIONS=halt_on_error=1`. Zero seam symbols; the only operator
  new/delete definitions left are the TSan runtime's own (20, identical
  count in `request_core_ownership_test`, which never had a seam). The
  F5-era failure was the link error `multiple definition of 'operator
  new'`; the target now links.
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

## F-10 correction (2026-10-14, review round 2) — probe/production dependency identity

Independent review finding `F10_PROBE_REAL_BUILD_DEPENDENCY_DIVERGENCE`
(P1, scope #520 only): the round-1 capability probe selected the header and
library by scanning candidate directories while the production build resolved
`<liburing.h>` and `-luring` through the toolchain — two different resolution
authorities that demonstrably diverge on this host. The empirical g++ 16
authority, measured rather than assumed
(`f10-correction/red/precedence-study.log`):

- C++ includes: command-line `-I` directories first, then `CPATH` →
  `CPLUS_INCLUDE_PATH` → the default directories; `C_INCLUDE_PATH` is not
  consulted for C++ at all.
- `-luring` link resolution: `-L` → driver-configured directories →
  `LIBRARY_PATH`.

RED at `5ab32ae7` (`f10-correction/red/`): with
`C_INCLUDE_PATH=<full header copy>` (invisible to g++ C++, honored by the old
candidate scan) plus `LIBRARY_PATH=<library copy>` (checked last by the
driver, first by the old scan), the old configure accepted and printed
`stub-good/include/liburing.h + fakelib/liburing.so` while the production
build resolved `/usr/include/liburing.h + /usr/lib64/liburing.so` — the probe
validated files the toolchain never opens
(`F10_PROBE_PRODUCTION_IDENTITY_DIVERGENCE`, `red/result.json`). The
deficient-CPATH negative already refused on the old code and stays valid:
with only `CPATH` set, the old scan's first existing candidate was the
`CPATH` directory itself, so the probe's pinned `-I` named the same
deficient file the toolchain resolves through `CPATH` — probe and
production agreed, and the declaration layer refused.

Corrected mechanism, implementation head
`9e04d3b54e8d4e0d7f5bf7cc86bb1622e55ea67b`: the resolved identity is
whatever the toolchain itself opens — `g++ -E -H` names the header, a
compile+link with `-Wl,--trace` names the library, both traced with the
ambient environment left in place, then re-traced with the resolved
directory pinned (`-I`/`-L`) and required to return exactly the same files.
The candidate scan and the top-level-prefix guard are deleted. The
capability probes run against exactly that pair. Version-different but
compatible installations are not rejected: when cleared-environment
resolution selects the same pair, nothing is bound and the supported
compile surface is unchanged; when it selects something else, the resolved
directories are bound publicly on `sluice_async` so production consumes the
probed pair (`__sluice_liburing_bind_includes`/`__sluice_liburing_bind_linkdirs`
in the stored config record which happened; the trace TUs use an empty
`main` so resolution stays independent of capability). Fail-fast is
confined to unverifiable consistency — an unresolvable header, an
unresolvable library, or a pin re-trace disagreement refuse at configure
time; a resolvable, pin-consistent pair is accepted regardless of which
directories it spans (there is deliberately no prefix-equality refusal).

GREEN at `9e04d3b5` (`f10-correction/green/`, `PROVENANCE.md` records every
driver, worktree head and command):

- `scripts/verify_liburing_identity_binding.py` (new RED/GREEN oracle):
  deficient-CPATH header refused at the declaration layer; the divergence
  environment accepted with the printed identity equal (realpath) to the
  toolchain's own resolution; clean-environment print equals the traced
  pair (`F10_IDENTITY_BINDING_REGRESSION_PASS`).
- Same-dir switch cycle + negatives re-baselined
  (`scripts/verify_liburing_config_switch.py`): n→y→n→y 52/66/52/66 with
  `bind_includes = false` in every y phase (clean-env resolution adds no
  flags); negatives re-layered — missing header → include-resolution layer,
  bind-mounted empty library files → symbol layer (an empty file still
  resolves; ld reads it as an empty linker script, so the honest failure is
  the missing symbol surface), reduced CPATH API → declaration layer, CPATH
  `#include_next` chain → accepted with the printed identity naming the stub
  and `bind_includes = true` recorded for consumers.
- Six-profile matrix (`PRE_F6_MATRIX_SIX_PROFILES_PASS`), B-01 v7 package
  capture (`--after 9e04d3b5`, harness verbatim, counts [15, 13, 25] in
  both ring profiles), and the installed header/ODR probes re-run green at
  `9e04d3b5`.
- Not re-run, with reason: the #519 mutation killers and the #521 TSan
  partition — the mutation surface and test sources are untouched by this
  change, and the matrix's `tsan-n` profile re-executed `request_scope_test`
  for real.

Supersessions in this file: the two Notes bullets on the mixed-prefix
refusal and `_report_liburing_paths` are marked in place; the #520 section's
round-1 description "an explicit refusal when header and library resolve to
different installation prefixes" states the `cdb2ca0f` behavior truthfully
and is superseded going forward by this section.

## Reproduction

```
# F-10 identity regression (probe/production dependency identity; head 9e04d3b5)
python3 scripts/verify_liburing_identity_binding.py \
    --workdir <checkout at 9e04d3b5> --output <fresh dir>
# matrix (six profiles) — the original gates at cdb2ca0f; the F-10 correction
# re-ran every driver below with the same commands against a worktree at
# 9e04d3b5 (--repo/--workdir <checkout at 9e04d3b5>, B-01 --after 9e04d3b5,
# installed probes --source-sha 9e04d3b5…)
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
  prints a skip notice and continues (this host ran all four). The
  missing-library injector hides the `/usr/lib64/liburing.so{,.2,.2.13}`
  names of this host's layout; on a different multiarch layout the
  injection would find nothing and the driver would fail loudly rather
  than pass silently. The matrix runner pins the F5 runner's interface
  (including its xmake path convention); the driver script takes
  `SLUICE_XMAKE` as an override.
- Superseded 2026-10-14 by the F-10 correction: this bullet describes the
  removed round-1 candidate-scan mechanism at `cdb2ca0f` (kept for the
  historical record). The mixed-prefix refusal compared the top-level
  directory component of the resolved header and library paths, so
  `/usr/local/include` + `/usr/lib64` was accepted by design; grossly
  different trees (an `/opt`-style stub alongside a system library) were
  refused. The current mechanism resolves through the toolchain's own
  traces with pin-equality verification and conditional public binding; see
  the F-10 correction section.
- The #520 fix is specific to the `--liburing` switch: xmake's depend
  check remains blind to tail-appended flags for any other config-time
  conditional flag a target might grow. Any future conditional define
  must either sit before the profile marker's fixed position or get its
  own always-present identity entry.
- `lib.detect.check_cxxsnippets` reads links and search paths only from
  its `configs` argument; links passed outside `configs` silently skip the
  link step. The capability probes pin `includedirs`, `linkdirs` and
  `links` through `configs` for exactly this reason (the missing-library
  negative catches a regression here).
- Superseded 2026-10-14 by the F-10 correction: `_report_liburing_paths` was
  removed with the candidate scan. The surviving sandbox fact is that the
  resolution, trace and refusal logic lives entirely inside the option-check
  callback — the only scope with `raise` (the project-file scope has none).
- The M4 isolation experiment temporarily reordered `tests[]` in the
  working tree; it was reverted before recording (tree clean at
  `cdb2ca0f`).
- Re-running the identical `xmake f` command in the same build directory
  under a changed environment does not re-run the option check (xmake's
  option cache), so the persisted resolution and binding stay from the
  previous probe. The identity remains internally consistent — a bound
  `-I` keeps production on the previously probed pair, and deleting that
  directory fails loudly — and any option change or `xmake f -c` re-probes.
  All recorded gates configure fresh build directories per phase.
- With binding disabled (clean-environment resolution), a consumer that
  adds its own `-I` containing a different `liburing.h` could shadow the
  resolved header at its own compile; that is inherent toolchain semantics
  outside `sluice_async`'s usage requirements, and no target in this
  repository adds such a path.
