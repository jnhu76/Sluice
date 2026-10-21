# Research patches

These are **research artifacts, not product changes**. They are provided for
reproduction of the M2 campaign and must not be applied to a release branch or
merged as-is. `run.sh` does not apply them automatically. M2 did not modify
product `src/` or `include/`.

Base commit: `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01` (master).
All patches apply to that tree with `git apply`.

| Patch | Files | Insertions | Purpose |
|---|---:|---:|---|
| `h1.patch` | 9 | 353 | Packed `[slot 15 | cookie 48]` operation identity, decode-and-verify lookup, capacity/cookie domain bounds, identity envelope test and de-vacuumed stale-cookie integration test |
| `h2a.patch` | 2 | 19 | Single-lock bounded owed snapshot with preallocated storage |
| `h2b.patch` | 5 | 284 | Derived `owed_count_`, empty-scan skip, parity test and no-decrement mutant |
| `h2ab.patch` | 5 | 294 | H2a+H2b combined; not selected (no gain over H2b) |

SHA-256:

```
c9d6e86ec2abc13394d969d82be287779b9f958be26bc783ad92889187cca999  h1.patch
f31f4e0b8618089d0e1906160608b329656269ced7744988033e840fecc94d8a  h2a.patch
e36da40f00aae9003f4719e0973eab433d2bed042e805e1de16c8e66682b4c0e  h2b.patch
65fef1ef67a9aaf816aba740303f76cd67289fb60fa2247bd1a98656c37ea91c  h2ab.patch
```

## Applying

```bash
git worktree add ~/sluice-m2/wt/h2b 8fdcd99c
git -C ~/sluice-m2/wt/h2b apply docs/research/native-linux-m2/patches/h2b.patch
(cd ~/sluice-m2/wt/h2b && xmake f -m release --liburing=y -y && xmake build -g bench)
```

The patches are mutually exclusive when applied to the same tree (H2a and H2b
both touch the poll sweep and the header). Use one arm per worktree; `h2ab.patch`
is the hand-merged combination.

## Identity notes

- H1 changes the operation identity format. Its full consumer matrix is in
  `../h1-identity-matrix.md`; the patch includes the test seam and conformance
  test adjustments needed to keep the suite honest.
- H2b keeps the authoritative `any_event_owed_locked_()` scan in teardown. The
  derived counter is not settlement/shutdown/destruction authority (BACKEND-04).
- The H2 parity mutant `uring_owed_counter_parity_mut_no_decrement` is a
  research-only xmake target. It must fail the parity test; a passing mutant
  would invalidate the evidence.
