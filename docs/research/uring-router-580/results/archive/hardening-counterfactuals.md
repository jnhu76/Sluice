# Archive hardening counterfactuals (owner review P2/P3)

Owner review of the archive found two script defects (P2, P3). Both were
fixed and verified by targeted counterfactual tests — no performance
matrix was re-run, no historical evidence touched. This file records the
tests so a reviewer can reproduce them; the assertion/cleanup code under
test was extracted verbatim from the scripts with `awk`/`sed`, not retyped.

## P2 — worktree cleanup must not bypass git's failure signal

Defect (at `c75ce5ea`): on `git worktree remove --force` failure the
cleanup fell back to `rm -rf "$DEST"`, deleting a still-registered
worktree and stranding its registration.

Fix: on removal failure, keep the directory, print the porcelain
registration status and the manual removal command; the `rm -rf` path now
exists only for the never-registered temporary-dest case.

Counterfactual (locked worktree — git refuses `worktree remove` on a
locked worktree, a legal failure a caller can produce):

```sh
W=/tmp/p2cf-wt
git worktree add --detach "$W" 46157fd1
touch "$W/left-behind.txt" && git worktree lock "$W"
# extract cleanup verbatim from the script (and from c75ce5ea for old),
# then run with WORKTREE_OWNED=1 KEEP=0 MADE_TMP_DEST=0
```

| Version | Directory after cleanup | Registration | Behavior |
|---|---|---|---|
| old (`c75ce5ea`) | **deleted** by `rm -rf` despite the lock | 1 stale entry (points at nothing) | git's refusal bypassed, data destroyed |
| new (hardened) | **preserved** | 1 live entry | warning + porcelain status + manual removal command printed |

## P3 — cache smoke must assert the result fields, not just timing presence

Defect (at `c75ce5ea`): the smoke asserted only `wall_s > 0` and
`ops == 2000`, so a harness that reported a failed run in-band
(`ok:false`, short `completed_ops`, `warmup_failure`) while still exiting 0
and emitting a timing record would have passed.

Fix: the smoke now also asserts `ok is True`, `completed_ops == 2000` and
empty `warmup_failure` (it no longer relies on the harness's exit-code
convention for correctness, only for fail-fast convenience).

Counterfactual (verbatim-extracted assertion blocks run against doctored
rows; healthy row passes both):

| Doctored row | old assertions | new assertions |
|---|---|---|
| `ok:false` + failure text | rc=0 (**passed a failed run**) | rc=1 `AssertionError: run reported failure: oracle mismatch at op 1337` |
| `completed_ops:1000` | rc=0 | rc=1 `AssertionError: completed 1000 of 2000` |
| `warmup_failure:"fixture setup failed"` | rc=0 | rc=1 `AssertionError: warmup failure: fixture setup failed` |
| healthy row | rc=0 | rc=0 |

## Post-hardening regression

A full `--mode verify` run (patch admission, all builds, gate checks)
passes after the change; the cleanup change only alters the failure path,
and the smoke change only adds assertions that hold on every healthy run.

## Owner disposition recorded with this hardening

#580 stays CLOSED; `NO_TRIGGER_NO_CHANGE`, `HOT_PATH_ROBUST` (scoped) and
`ARCHIVE_READY` stand; production change NONE; merge of PR #592 deferred
until this hardening is verified and separately owner-authorized; the
same-session nature of the archive reviews remains a disclosed limitation
of the evidence, not a blocker.
