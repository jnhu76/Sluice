# Probe snapshot — #600 comparator ladder kit (P0 persistence)

Copied 2026-10-27 from the uncommitted #600 research workspace
`/home/jnhu/sluice-600-direct-research/` into this mission directory, because
the originals have no repository buildable entity and no recorded compile
command (see `../P0_BASELINE_AND_OWNERSHIP.md` §1.4). The ladder probe file
self-declares "Research probe: lives in the #600 workspace, not the
repository"; this snapshot does not change that status — it only preserves the
exact bytes so the N0/N1 comparator identities survive the workspace. Whether
any of this enters the repository is an owner decision.

| File | Origin | sha256 (copy == original, verified at copy time) |
|---|---|---|
| `ladder.cpp` | `bench/ladder.cpp` | `d879b2685b14539efa6552b9918776a401ee5633339df4db1e58c70c252fe884` |
| `mkfixture.cpp` | `bench/mkfixture.cpp` | `63bd88583ad6e543c27fae109a870ec2f7eb6085e888b3402a65be4714bf02c8` |
| `paired.sh` | `bench/paired.sh` | `6a796f9cc2e6e46d45fde93fa20ac13b003e2bf8f1827f93399b3796000de4ed` |
| `paired2.sh` | `bench/paired2.sh` | `c119e737cdb4433b04a6f6c187c53ebe2d75b61857cf8b4b5c366334ded371b6` |
| `scripts/analyze_pairs.py` | `scripts/analyze_pairs.py` | `8d672d7e4337ce656f248ed5daba0c166093cb59e35df746183da60159819fd3` |

Reproduction anchors (verified 2026-10-27 on the E5 host):

- Fixture: `mkfixture.cpp` is fully deterministic (fixed LCG byte pattern, no
  seed). `g++ -O2 -std=c++20 mkfixture.cpp -o mkfixture && ./mkfixture
  <path> 67108864` regenerates a 64 MiB fixture byte-identical to the #600
  fixture `/tmp/sluice600/data64m.bin`
  (sha256 `bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a`;
  `cmp` clean).
- Ladder rebuild (inferred command; the #600-measured binaries' exact compile
  command was never recorded):
  `g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror -I<sluice>/include
  ladder.cpp <libsluice_core.a> -o ladder`.
- Raw series produced by these scripts remain in
  `/home/jnhu/sluice-600-direct-research/evidence/` (workspace only, not
  snapshotted here).
