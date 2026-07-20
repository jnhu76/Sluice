# E13 Select — Evidence Safety & Source-Safe Verification (PR #18)

This document describes the source-safety guarantees of the PR #18 formal
verification tooling: how TLC runs are isolated from the source tree, how
the verifier guards against accidental deletion, and how every claim in
the README and review request is backed by a reproducible command.

## Source-safe verifier

`tools/formal/verify-e13-select-safety.sh` is the reproducible PR #18 gate.
It inherits the source-safety design of PR #17's `verify-e13-select-core.sh`:

1. **Isolated workspace.** Every TLC run happens inside a fresh
   `mktemp -d -t e13-select-safety.XXXXXX` workspace.  The spec `.tla` and
   `.cfg` files are *copied* into the workspace; the source tree is never
   read directly by TLC and never written to.

2. **Defensive cleanup trap.** A `trap cleanup EXIT` removes the workspace
   on exit.  The cleanup function verifies the path matches the expected
   mktemp pattern before `rm -rf`:

   ```bash
   cleanup() {
     if [[ -n "$outroot" ]] \
        && [[ "$outroot" == /tmp/e13-select-safety.* \
              || "$outroot" == "${TMPDIR:-/tmp}"/e13-select-safety.* ]]; then
       rm -rf -- "$outroot"
     fi
   }
   ```

   A non-matching path is never deleted, so a botched `outroot=` can never
   trigger an `rm -rf` of an unrelated directory.

3. **Per-run `-metadir`.**  Each TLC run uses its own `-metadir` under the
   workspace, so disk-backed state fingerprints and queues do not collide
   between concurrent runs and do not leak into the source tree.

4. **No source-tree mutation.**  The verifier contains no `git add`,
   `git commit`, `rm` of source paths, `mv`, `>` redirect into a source
   file, or any other operation that modifies the working tree.  It only
   reads and reports.

5. **`set -euo pipefail`.**  Any unexpected shell error stops the run
   immediately rather than producing partial / misleading output.

## Pre-existing untracked files preserved

The repository contains two pre-existing untracked files that predate this
PR and are explicitly out of scope:

- `tests/test_t3_simple.cpp`
- `tla2tools.jar`

The verifier and the PR's commits do **not** modify, delete, stage, or
commit either file.  `tla2tools.jar` is referenced read-only via the
`TLA2TOOLS_JAR` environment variable (defaulting to `$repo/tla2tools.jar`)
so contributors can point at a local TLC build without touching the tree.

## Reproducibility

Every claim in the README and review request is backed by a runnable
command.  The canonical sequence:

```bash
# PR #17 regression (still must pass):
TLC_WORKERS=1 tools/formal/verify-e13-select-core.sh

# PR #18 safety suite (the new gate):
TLC_WORKERS=1 tools/formal/verify-e13-select-safety.sh
```

Both scripts:

- Auto-detect `tla2tools.jar` and `java` from the environment.
- Print one line per gate (`PASS`, `REACH`, `NEG`, `RESTORE`, `FAIL`) with
  the matching state count, search depth, and runtime.
- Exit non-zero if any gate fails its expectation.

## Toolchain pinning

The verifier output reports the TLC version on the first line of every
run, e.g.:

```
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
```

PR #18 was validated on:

- TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
- OpenJDK 25.0.3+9-2-26.04.2-Ubuntu
- `TLC_WORKERS=1` (deterministic; state order is reproducible)

## Evidence catalogue

The PR #18 evidence set, grouped by category:

| Category | Count | Source |
|----------|-------|--------|
| Layered safety aggregates (P) | 7 | Contract 2/3-arm, Central 2/3/4-arm, Adapter 2/3-mix |
| Multi-group non-interference (O) | 2 | positive + reach witness |
| Widened refinement (X) | 1 | Central 3-arm |
| Contract negative models (R) | 8 + 1 restore | NEG-C1..C8 + restore |
| Central negative models (S) | 6 + 1 restore | NEG-S1..S6 + restore |
| Adapter negative models (T/U/V) | 15 + 1 restore | NEG-E1..E6 + NEG-T1..T5 + NEG-A1..A4 + restore |
| Per-law non-vacuity witnesses (W) | 20 | 9 Contract + 5 Central + 6 Adapter |
| PR #17 regression anchors | 5 | R1, R5, R7, R9, R10 reach configs |

Total: 65 distinct TLC runs, all gated by the verifier.

## Audit

To audit that the PR did not touch out-of-scope paths:

```bash
git diff master --stat
```

Every modified or added path must be under one of:

- `docs/spec/e13_select/`
- `docs/formal/`
- `docs/reviews/`
- `tools/formal/`

No path under `include/`, `src/`, `tests/`, `examples/`, `benchmarks/`,
CI configuration, or production build policy may appear in the diff.

To audit that the pre-existing untracked files are untouched:

```bash
git status --short tests/test_t3_simple.cpp tla2tools.jar
```

Both must remain `??` (untracked, unmodified) throughout the PR.
