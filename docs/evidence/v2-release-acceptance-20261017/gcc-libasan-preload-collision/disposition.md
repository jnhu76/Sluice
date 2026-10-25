GCC shared-libasan × test-only LD_PRELOAD interposer collision

Discovered by the first full-matrix pass of this campaign (2026-10-17,
head `5fd8e109`): with the default GCC toolchain, both `-m asanubsan` cells
fail exactly one test, `app_tail_cli_oracle_test`, in its two
pending-signal cases; every other test in both cells passes, and the same
test passes in all six non-asanubsan cells including the GCC TSan cells.

Mechanism: the tail CLI oracle drives the real `sluice-tail` child with a
test-only LD_PRELOAD interposer (`tests/support/tail_pread_shim.c`) to
stage pending-signal settlements. GCC's AddressSanitizer runtime is the
shared `libasan.so`; ASan's link-order verification rejects any preloaded
library ahead of it and hard-exits the child before `main`:

    LD_PRELOAD=<libtail_pread_shim.so> <asanubsan>/sluice-tail -n 2 <file>
    ==465939==ASan runtime does not come first in initial library list;
    you should either link runtime to your application or manually preload
    it with LD_PRELOAD.
    exit=1

The harness reports this as "no ENTERED event (LD_PRELOAD intercept
failed?)" for `cli_pending_sigint` and `cli_pending_sigterm`. This is a
test-harness/sanitizer-runtime incompatibility, not a product defect: the
application binary is correct without the interposer, and TSan's shared
runtime performs no such check (its cells pass with the same interposer).

Disposition, following the PR B matrix precedent
(`docs/evidence/pr-b-boundary-contraction-20261016/verification-record.md`):
the asanubsan cells pin the clang toolchain, whose default static
sanitizer runtime is not in the dynamic link map and does not perform the
link-order check, so the interposer loads and the full suite — including
the pending-signal oracles — runs under ASan+LSan+UBSan. The recorded
matrix cells `asanubsan-n`/`asanubsan-y` are the clang builds. The GCC
asanubsan runs are preserved in this directory as the honest record of the
collision (56/74-test cells, 1 harness failure each):

- `asanubsan-n.log` / `asanubsan-y.log` — GCC `-m asanubsan` cell logs
  (exit 255, only `app_tail_cli_oracle_test` failed)
- `app_tail_cli_oracle_test-direct-repro.log` — direct binary run showing
  the two failed cases
- `manual-repro.txt` — the minimal LD_PRELOAD reproduction command

Re-evaluation trigger: if the interposer or the sanitizer toolchain
changes, re-run a GCC asanubsan cell before relying on this disposition.

Supersession (2026-10-25, branch fix/586-asan-preload-order, commit acff298f):
the interposer harness change named by the trigger above has happened. The
tail CLI oracle now preloads the dynamic ASan runtime — resolved at test
runtime from the test process's own link map — ahead of the shim, and the
tail CLI oracle passes 19/19 under GCC asan and asanubsan cells on Fedora 44
/ GCC 16.2.1 (issue #586). The 2026-10-17 clang pinning and the GCC cell
logs above stay as the honest record of the collision; re-run a GCC
asanubsan cell per the trigger above before re-admitting GCC asanubsan
cells to the standing matrix.
