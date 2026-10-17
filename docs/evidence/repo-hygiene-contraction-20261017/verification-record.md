# Verification record — repository hygiene contraction

Executed at the hygiene head (branch `repo-hygiene`), host: WSL2 Linux
6.18.54.1-microsoft-standard, xmake 2.9.7, gcc 15 (system), liburing
/dev present (`/usr/include/liburing.h`), OpenJDK 25. Base `9845cee1`.
Content-verification head: `628968a9` (commits after it are docs-only);
the TLA gate ran at `a645baed` with its script diff-verified comment/echo-only
since (see below).

## Build and test matrix (all commands from the repository root)

Final full-matrix re-run at `628968a9` (the content head; every cell names
its liburing flag explicitly — an `xmake f` without the flag carries the
previous cell's setting over):

| Cell | Command | Result |
|---|---|---|
| debug, liburing=n | `xmake f -m debug -y --liburing=n && xmake test -v` | 56/56 passed, exit 0 |
| release, liburing=n | `xmake f -m release -y --liburing=n && xmake test` | 56/56 passed, exit 0 |
| debug, liburing=y | `xmake f -m debug -y --liburing=y && xmake test -v` | 74/74 passed, exit 0 |
| release, liburing=y | `xmake f -m release -y --liburing=y && xmake test` | 74/74 passed, exit 0 |

An earlier full matrix at the same content state first ran the liburing=n
cells without an explicit flag and is superseded by the re-run above. A
debug+liburing=y re-run after the final build-graph fix
(`uring_request_core_integration_test` target name) was 74/74, exit 0.

Base-comparison control: at `9845cee1` in a separate worktree the same
matrix cells were run to falsify two rename-induced regressions found
mid-round (a payload string grown past a 16-byte buffer, and a read-back
memcmp against the old content tag); both were fixed in commit `9387af2e`
and the suites pass again.

## Mutation kills

The mutation set is 59 manual targets over 53 injection defines at base
and head; the rename touched 18 target names and 49 defines, while 4
defines (backing 8 targets) are byte-identical at base and head — the
decomposition is in `test-retention.md`. All 59 targets were rebuilt and
executed at `9217609f` (debug, liburing=y); every one exited
nonzero (killed by its oracle). Per-target results:
`mutant-kill-matrix.txt` in this directory.

## Formal gates

| Gate | Command | Result |
|---|---|---|
| ProgressSource model | `./scripts/verify_progress_source_model.sh` | PASS — "ProgressSource model: all expected outcomes reproduced.", exit 0 (21 configurations); re-run at `628968a9` after the message rewording |
| RequestCore/ObserverCore/ShutdownCore models | `./scripts/verify_tla.sh` | PASS — 55/55 expected verdicts; full run on the build server (details below) |

### verify_tla.sh server run (2026-10-17)

Ran at branch head `a645baed` (the commits after it in this branch touch
only docs/evidence and the changelog). Host: jnhu@192.168.31.75 (Fedora
Linux, kernel 7.2.9-201.fc44.x86_64, 20 cores, OpenJDK 25.0.4.1),
worktree `/tmp/sluice-hygiene` at `a645baed`, jar cache
`~/.cache/sluice/formal/tla2tools.jar` matching the script-pinned SHA-256
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.
Concurrency `SLUICE_TLA_JOBS=3 SLUICE_TLA_WORKERS=6`. Finished
2026-10-17 14:00:33 +0800 with `VERIFY_TLA: PASS`: all 55 dispatched
configurations reproduced their expected verdicts — 9 positive
configurations ended "Model checking completed. No error has been
found." and 46 negative configurations reproduced exactly their named
invariant or temporal-property violation (both counts legible in the
captured log). The long pole is rcore-live at 5,359,732 distinct
states; the decisive run took roughly 25 wall-clock minutes (an earlier
launch of the same gate at 13:16:19 with 10 jobs x 2 workers was
aborted after projecting a comparable long pole, and its relaunch
truncated the shared log, so the exact start was not preserved; on the
local 2-worker lane the same model projects 60-80 minutes). Raw
captured log: `tla-server-run-20261017.log` in this directory.
Reproduce with `./scripts/verify_tla.sh` at the recorded head; the
bootstrap re-verifies the pinned jar checksum. Commits after `a645baed`
on this branch touch `scripts/verify_tla.sh` only in comments and echo
labels and one `run_violate` diagnostic message string (the expected
invariant is identical; diff-verified: zero other changed lines), so the
dispatch set, expected verdicts and jar pinning of the recorded run are
the ones in force at this branch's head.

`verify_formal.sh` is retired with the Lean corpus it gated.

## Package boundary (G9)

| Gate | Command | Result |
|---|---|---|
| Verifier self-test | `python3 scripts/test_verify_v2_package.py` | Ran 14 tests, OK, exit 0 (re-run at `628968a9` after the comment rewording) |
| Comment-guard self-test + scan | `python3 scripts/test_check_cpp_comment_authority.py && python3 scripts/check_cpp_comment_authority.py .` | OK, exit 0 |
| Clean-room noliburing | `python3 scripts/verify_v2_package.py --manifest-out /tmp/pkg-manifest-noliburing-628968a9.json` | PASS — all gates, exit 0 at the committed clean head `628968a9` (an earlier clean run at `4d80cdd3` also passed; the mid-round 46/47 run failed only the dirty-worktree provenance gate) |
| Clean-room liburing | `python3 scripts/verify_v2_package.py --liburing --uring-prefix /usr --manifest-out /tmp/pkg-manifest-liburing-628968a9.json` | PASS — all gates, exit 0, same clean head |

## Naming and reference integrity

- Campaign-token file paths under active dirs (`src include tests apps
  bench scripts formal .github`) with the token set
  {e1,e2,e4,b1a,b1b,b1c,b2,c1,c2,d1,f0,f1,f3,f4,g0,h1,i1,mr,pr-b} matched
  on path components: 32 at base -> 0 at head (reproducible with the
  stated set; the review draft's unqualified "30" is superseded).
- Campaign-labeled test-side `/tmp` template strings and the negative-probe
  symbol added to the sweep by the review round (16 strings with `b1a`,
  `b2`, `d1`, `f1`, `tail_e4` prefixes): 16 at base -> 0 at head; the
  affected suites re-run green in the final matrix.
- Campaign vocabulary in gate-script and build-graph prose (catalogue
  comments in `xmake/tests.lua`, stage-labeled echo headers in
  `scripts/verify_tla.sh`, residual "D1/B2/F1/PR B" phrasing in
  `scripts/verify_v2_package.py`, duplicated campaign phrasing in
  `scripts/verify_progress_source_model.sh`): removed in the review round;
  only pointers to dated campaign records (Git history) remain.
- Stage-prefixed mutant define families (`SLUICE_{E1,B1C,C1,C2E,E2,B2,D1}_MUTANT*`):
  7 at base -> 0 at head.
- Script filenames carrying stage labels: 7 at base -> 0 at head.
- Tree-wide grep for every retired script/directory/workflow name and every
  renamed symbol over the active tree: only intentional historical mentions
  remain (the ledger's dated hygiene section, `docs/README.md`'s
  evidence-layout paragraph, frozen records, the superseded ADRs, the
  CHANGELOG's dated release history, and the root's decision-table
  reference to the retired "H1-era" process).
- Xmake target-name uniqueness: all target declarations unique (the graph
  configures and builds; `xmake test` discovers exactly the registered set).

## Justified naming exceptions (active tree)

1. W-01/W-02/W-03 workload vocabulary (`bench_w0*_*.cpp`,
   `direct_w01_consumer_probe`, clean-room `w0*_*.cpp` consumers, the
   `w03-handoff` phase tags) — current contract IDs named by the root.
2. ADR-0004 file name and its M1-M12 mutant numbering — adopted decision
   vocabulary.
3. Ledger groups G1-G10, release gates RG1-RG7, owner decisions D1-D3,
   requirement IDs and V-numbers — current contract identifiers.
4. Dated-evidence directory names under `docs/evidence/` and the superseded
   ADRs (0001, 0002 with views) under `docs/adr/` — historical records
   under supersession notices; not rewritten.
5. `docs/archive/**`, `research/RESULTS.md`, CHANGELOG dated sections and
   the app READMEs' `5f62b55b` notes — labeled historical records.
