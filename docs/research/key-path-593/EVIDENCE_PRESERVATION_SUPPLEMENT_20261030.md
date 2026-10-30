# Evidence preservation supplement — 2026-10-30

Subject: PR #594 (`research/593-key-path`, head `2714aa65`) — the
`docs/research/key-path-593/` campaign record. This supplement is dated
2026-10-30 and records two things: that the historical preservation statement
in the record is kept unchanged, and the preservation state verified on
2026-10-30. It authorizes research archiving only.

## 1. The historical `EVIDENCE_PRESERVATION: PARTIAL` stands as written

`evidence-index.md` §7, `decision.md` §7 (`REPRODUCIBILITY` block) and
`README.md` §8 state `EVIDENCE_PRESERVATION: PARTIAL`: the raw evidence was,
at measurement and record time, a single persistent copy at
`/home/jnhu/sluice-593-research/evidence` on the E5 host, not replicated, not
in CI artifact storage, with only summaries/patches/identities hash-anchored.

That statement is a **historical fact as of the measurement time point**
(evidence frozen on the E5 host, record assembled 2026-10-26). It is kept
verbatim in this record and is not retroactively upgraded or rewritten by
this supplement. Sections 2 and 3 below are additive.

## 2. Preservation state verified on 2026-10-30: cross-host `VERIFIED`

The 2026-10-30 execution round (receipt:
`E5_PRESERVATION_RECEIPT.md`) records the following, each executed and
observed during that round:

- **Full per-file manifest generated on E5.**
  `E5_EVIDENCE_SHA256SUMS` (6,913 lines) and `E5_EVIDENCE_MANIFEST.json`
  (header: `generated_on 2026-10-30T07:13:33+08:00`, `file_count 6913`,
  `total_bytes 1423204159`) cover the whole evidence tree, including the
  raw per-rep JSONL and perf `.data` the record's own manifest did not
  anchor.
- **Manifest self-check clean on E5.** Manifest line count = `find` file
  count (6,913 = 6,913); both-direction verification clean; 0 failures.
- **Independent copy on a second physical host `VERIFIED`.** The backup at
  `/home/hoo/sluice-593-evidence-backup/evidence/` (WSL2 recording host, a
  different machine from E5) was fully re-verified on 2026-10-30:
  6,913 files / 1,423,204,159 bytes, manifest hashes identical on both
  sides, and `sha256sum -c` over all 6,913 entries → all OK, 0 non-OK.
- **Restore spot-check passed.** 10 files (covering all 7 top-level
  directories, the largest file, the sudo-recovered perf `.data` and the key
  result files) restored from the backup to E5 `/tmp/restore-check/`:
  10/10 sha256 identical and matching the manifest entries.
- **Drift vs the record's historical count is fully attributed.** The
  record states 6,870 files / 1,422,946,592 bytes (`evidence-index.md` §1);
  the 2026-10-30 tree is 6,913 files / 1,423,204,159 bytes. The delta is
  exactly the `worktree-snapshots-20261026/` directory (43 files, one write
  batch, mtime 2026-10-26 23:39:45): no deletions or modifications
  elsewhere. The historical 6,870 figure in §1 is likewise kept unchanged.
- **Receipts archived in `master`.** The receipt and both manifest files
  live at `docs/research/retention-wsl2-audit/e5-preservation/` on `master`,
  entered by the #614 squash commit
  `3306728aa0f38b2bf82913e9dbee8b973dde3ac7`
  (`research(retention-wsl2-audit): closure fixes, E5 preservation record
  and branch governance`).

This resolves gate 4 ("Raw evidence verified + independent preservation
plan", then `NOT MET`) of the archive adjudication in
`docs/research/retention-wsl2-audit/RESEARCH_594_ARCHIVE_DECISION.md`; the
receipt's overall verdict there is `VERIFIED`.

## 3. Disposition of this round: `RESEARCH_ARCHIVE_ONLY`

This round's adjudication is **research-archive only**:

- It authorizes archiving the #594 research record and its evidence
  reproduction entry. It does **not** authorize K4-b productization: the
  productization preconditions in `decision.md` §4 remain open, and product
  authority is retained by #595 (OPEN), whose §4 owns the K4-b counterfactual
  oracle repair work.
- #593 remains **OPEN**. PR #594 does not satisfy issue #593; closing #593
  citing #594 as completion would be a mis-attribution
  (`RESEARCH_594_ARCHIVE_DECISION.md` §4, verified in the deciding session
  via `gh issue view 593`).
- The archive adjudication verdict for the branch remains
  `KEEP_RESEARCH_PR_UNMERGED` until the remaining archive-merge gates are
  met and the Owner overrides the PR's own do-not-merge instruction.
