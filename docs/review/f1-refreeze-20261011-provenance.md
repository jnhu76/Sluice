# F1 Versioned Dual-Profile Re-Freeze (F-W2-1, #489)

- **Status**: compensatory re-freeze of the F1 package baseline. Owner
  decision [#458 comment 6099156773](https://github.com/jnhu76/Sluice/issues/458#issuecomment-6099156773):
  `VERSIONED_DUAL_PROFILE_REFREEZE_NOW`. This record repairs package
  *provenance*; it does not authorize deletion of the historical F1 assets,
  is not an F2–F5 physical contraction, and claims no runtime/io_uring
  product conformance or v1 RELEASE_READY status beyond what the gates below
  measure.
- **Parent**: Phase M #458 (F-W2-1); Phase F #402 F1; issue #489. Historical
  baseline record: `f1-clean-room-package-baseline.md` (its facts stay dated
  to its own verification head; nothing there is re-claimed here).
- **Normative root**: `docs/explicit-io-v1-final-decision.md` v1-r3.

## 1. Why a re-freeze was required (F-W2-1)

The historical F1 manifests pin
`PRODUCTION_BASELINE_SHA = a34d96c64f5ac8647b7d0f57c5511e68301b0f0e`.
After that freeze, two production fixes merged to master through their own
reviewed production PRs:

- #481 (T-2): threadpool worker publishes a progress wake when it retires an
  unclaimable dispatch entry;
- #484 (T-4b): `cancel_key` signals when it retires a stale dispatch entry
  without a cancel win.

Both live in the three files that differ from the historical baseline
(measured at this re-freeze's branch point):

```text
include/sluice/async/threadpool_backend.hpp   (+5)
src/async/threadpool_backend.cpp              (+36/-5)
src/async/threadpool_test_seams.hpp           (+10)
```

`verify_f1_package.py` therefore reports `F1_B_PROVENANCE_VERIFIED = FAIL`
for both historical manifests at any HEAD containing the settlement fixes —
confirmed live at `0773a99e` by invoking the verifier's own
`resolve_production_baseline` / `production_diff_paths` on both historical
manifests (3-file production diff, clean worktree). This is the recorded
F-W2-1 failure. The failure is correct behavior: the historical manifests
prove historical code and stay valid as dated records; they cannot prove the
post-settlement tree, and nothing here claims they do.

## 2. Build-config change carried by this PR (root cause of a real gate FAIL)

The first re-freeze attempt on this machine produced a genuine
`--check-frozen` failure: every gate and every per-member object hash
passed, but the freshly regenerated archive baseline did not match the
frozen one (`ARCHIVE_BASELINE` diff, fresh baseline mismatch) for the
rebuilt archive. Root cause: this machine's GNU ar (binutils 2.46.1,
Fedora) is built without `--enable-deterministic-archives` and embeds each
member's real mtime/uid/gid, so identical objects archive to different
bytes across builds; the historical freeze ran on binutils whose default is
deterministic, which masked this. `libsluice_core.a` only appeared to
reproduce because xmake had not rebuilt it.

Per the no-masked-failure rule the cause was fixed, not the threshold:
`xmake/libraries.lua` now passes GNU ar's `-D` modifier (zeroed member
metadata; standard reproducible-builds practice) to both production static
library targets, scoped to `is_plat("linux")` (the Windows/clang-cl
archiver does not accept `-D`). Verification performed for this change:

- two full clean rebuilds (all objects recompiled) produce byte-identical
  `libsluice_core.a` and `libsluice_async.a`;
- the per-member object content is unchanged by `-D` (member sha256 sets
  are identical to the non-`-D` builds; only archive-level metadata
  differs), so this is not a content-affecting change;
- the complete dual-profile matrix below was executed from the fixed
  configuration.

Because this PR therefore carries a build-config change (and no production
`src/`+`include/` change), it is **not** a pure docs-only re-freeze: the
change, its root cause and its verification responsibility are stated here
and in the PR description. The provenance pinning is unaffected
(`PRODUCTION_DIFF_EMPTY` measures `src`/`include` only, and remains true);
the frozen archives were built at `VERIFICATION_HEAD = 4b71b598…`, whose
only delta from the production baseline is this archiver flag.

## 3. New baseline identity

```text
LIVE_MASTER_SHA          = 0773a99e58f2aec7413426879b73ea958b2a8d8e   (origin/master at freeze)
NEW_PRODUCTION_BASELINE  = 0773a99e58f2aec7413426879b73ea958b2a8d8e   (merge-base HEAD origin/master)
VERIFICATION_HEAD        = 4b71b598ad78153748cabc36dd7e58f8e9fd71d4   (clean; adds only the
                            deterministic-archiver build flag on top of the baseline)
PRODUCTION_DIFF_EMPTY    = TRUE  (both profiles; src/include identical to baseline)
```

The new baseline contains the T-2/T-4b settlement fixes; the re-freeze proves
the current production code, it does not roll anything back.

## 4. Environment (measured per run, recorded in the manifests)

```text
TOOLCHAIN  = g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2); xmake v3.0.9+20260621;
             GNU ar (GNU binutils) 2.46.1-1.fc44 (deterministic mode via -D, §2)
ARCH       = x86_64
KERNEL     = Linux 7.2.5-200.fc44.x86_64; kernel.io_uring_disabled = 0
FILESYSTEM = source tree btrfs; scratch install prefix + consumer build dir tmpfs (/tmp)
LIBURING   = 2.13 built from source (github.com/axboe/liburing, tag liburing-2.13,
             tarball sha256 618e34dbea408fc9e33d7c4babd746036dbdedf7fce2496b1178ced0f9b5b357)
             into /tmp/f1-deps/uring-prefix (liburing.a/.so.2.13 + lib/pkgconfig/liburing.pc)
```

Both profiles were configured with the verifier's explicit both-ways switch
(`xmake f -m release --liburing=n` / `--liburing=y`), and each run's
`F1_E_ARCHIVE_MATCHES_PROFILE` gate proved the built archive carries its
profile's macro view (the adjudicated fix for the historical stale-cache
finding; the repo's pre-existing cached config was debug+liburing and was
overwritten by the explicit reconfigure).

## 5. Frozen artifacts (this re-freeze, versioned and additive)

```text
docs/review/f1-refreeze-20261011-manifest-noliburing.json
docs/review/f1-refreeze-20261011-archive-noliburing.json
docs/review/f1-refreeze-20261011-manifest-liburing.json
docs/review/f1-refreeze-20261011-archive-liburing.json
docs/review/f1-refreeze-20261011-provenance.md          (this record)
```

Historical assets (`f1-package-manifest-*.json`,
`f1-archive-baseline-*.json`, `f1-clean-room-package-baseline.md`) are
untouched by this PR (byte-identical Git blobs; verified in review).

Single non-measurement normalization: each manifest's
`ARCHIVE_BASELINE.file` was rewritten from the `/tmp` scratch path to the
committed repo-relative path above, per the F1 protocol's freeze-then-commit
flow. No other field was edited; the archive/object/symbol hashes,
environment fields, gate-derived values, profile configuration and header
inventory are exactly as measured.

## 6. Gate results (final verifier state, both profiles, exit 0)

```text
                                        noliburing (P0P1P3)   liburing (P2)
F1_B_PROVENANCE_VERIFIED                     PASS                PASS
F1_E_STANDALONE_HEADER_COMPILES              PASS                PASS   (78 direct + 1 anchored _impl = 79)
F1_E_ARCHIVE_BASELINE_RECORDED               PASS                PASS   (2 archives, 50 objects)
F1_E_ARCHIVE_MATCHES_PROFILE                 PASS                PASS
F1_B_PACKAGE_PREFIX_READY                    PASS                PASS   (79 headers, 2 libs)
CLEAN_ROOM_CONSUMER_INCLUDES_ANGLE_BRACKET_ONLY PASS            PASS
P0_W01_CORE_ONLY_CLEAN_ROOM                  PASS                PASS
P1_w02_pipeline                              PASS                PASS
P1_w03_external_loop_threadpool              PASS                PASS
P1_contract_* (8 families)                   PASS                PASS
P1_W04_STRUCTURAL_ARMS_UNAVAILABLE           PASS                —     (rc=2, measured pread/ESPIPE reason)
P2_W04_STRUCTURAL_STOP                       —                   PASS
P1_W03_URING_EXPLICIT_UNAVAILABLE            PASS                —     (rc=2)
P2_W03_URING_RUNTIME                         —                   PASS   (real io_uring backend, this kernel)
F1_E_ODR_TWO_TU                              PASS                PASS   (backend_bytes 48 vs 304)
NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE           PASS                PASS
NEGATIVE_EXPERIMENTAL_HEADERS_NOT_INSTALLED  PASS                PASS
NEGATIVE_LINK_WITHOUT_URING_FAILS            —                   PASS
NEGATIVE_ODR_DIVERGENT_VIEWS_DETECTED        —                   PASS
CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY PASS               PASS
```

New honest measurements on this toolchain (not adjusted to match the
historical GCC 15 / WSL2 records — the historical 1382/1501 defined-symbol
and 97/108 external-undefined counts belong to the historical toolchain):

```text
defined-symbol records         1316 (noliburing) / 1400 (liburing)
external undefined symbols      98 / 109
async archive io_uring refs      0 / 11
async-only defined symbols        — / 77 net new (real UringAsyncBackend members)
core archive                 identical across profiles (no uring code in core)
ODR discriminator sizeof(UringAsyncBackend) = 48 / 304
installed headers               79 both profiles (unchanged set)
```

The two profiles' archive baselines are genuinely different; each manifest
binds its own baseline by sha256, and the closure held at commit time
(committed baseline file == manifest-bound sha256, per profile).

## 7. Reproduction at the final PR head

Executed with a clean worktree at the exact PR HEAD after the artifacts were
committed (results recorded on #489 / the PR; the reproduction, not the
freeze, is the completion criterion):

```bash
python3 scripts/verify_f1_package.py \
  --check-frozen docs/review/f1-refreeze-20261011-manifest-noliburing.json \
  --manifest-out /tmp/m489-check-noliburing.json \
  --archive-baseline-out /tmp/m489-check-archive-noliburing.json

python3 scripts/verify_f1_package.py \
  --liburing --uring-prefix /tmp/f1-deps/uring-prefix \
  --check-frozen docs/review/f1-refreeze-20261011-manifest-liburing.json \
  --manifest-out /tmp/m489-check-liburing.json \
  --archive-baseline-out /tmp/m489-check-archive-liburing.json
```

`--check-frozen` pins `PRODUCTION_BASELINE_SHA = 0773a99e…` from each frozen
manifest (validated as an ancestor of HEAD), re-runs the full gate matrix,
reproduces the manifest outside `VOLATILE_MANIFEST_KEYS`
(`GENERATED_UTC`, `VERIFICATION_HEAD`, `VERIFICATION_HEAD_DIRTY`,
`ORIGIN_MASTER_TIP` — unchanged from the historical protocol; the key set
was not widened), and closes the three-way baseline loop: committed frozen
baseline sha256 == manifest sha256 == freshly regenerated baseline sha256.

## 8. Boundaries of this evidence

- Package provenance reproduction is not runtime/io_uring product
  conformance and not a v1 RELEASE_READY claim; those live in their own
  ledger slices.
- #458 Phase M: only F-W2-1's real completion state is backfilled; the #488
  governance review and the remaining Phase M family gates are not claimed.
- The historical `a34d96c6`-pinned manifests remain dated historical records;
  nothing here asserts they reproduce at the current HEAD.
