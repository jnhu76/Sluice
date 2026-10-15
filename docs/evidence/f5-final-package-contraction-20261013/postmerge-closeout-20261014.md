# F5 post-merge closeout — 2026-10-14

Status update: **F5 = CLOSED (merged and post-merge verified)**. This dated
receipt supersedes the CANDIDATE status in this directory's README, which is
retained above as the pre-merge record.

## Merge facts

- Owner authorization recorded pre-merge in
  `installed-surface-risk-disposition.md` at branch tip `5ef4f801`
  (`ACCEPT_BREAKAGE_NOW` and the scoped #518 merge authorization; commit
  `5ef4f801 docs(f5): record owner ACCEPT_BREAKAGE_NOW and scoped #518 merge
  authorization`).
- Merge: `f99acbc3a64cf4eae356ef39b99d88e89183f579` = "Merge PR #518: F5
  final package and public surface contraction" (parents: `14a27a83`, the F2
  final-exit master, and `5ef4f801`, the F5 branch tip).
- Gate-content continuity: the F5 gates were executed at implementation head
  `a23a94cdfbfa90f6ad6aff2d1b6706c305a4245c`; `git diff a23a94cd 5ef4f801 --
  src include xmake.lua xmake apps tests scripts` is empty (the commits after
  the gate head are evidence/receipt docs only), so the merged code content
  is the executed gate content.

## Post-merge verification on actual master

Executed 2026-10-14 at master `573d616cc1ee79ec7a3b3876351799708237db3a`
(a descendant of `f99acbc3` carrying only the authorized #522 infrastructure
slice and its evidence), inside the PR #522 post-merge receipt
(`docs/evidence/pre-f6-infra-stabilization-20261014/postmerge/`,
`b01-v7/` + `installed-probes/`):

- B-01 v7 dual-profile package capture, F5 v6 harness verbatim, `--before
  14a27a83 --after 573d616c`:
  `F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS 14a27a83… 573d616c…`. After-counts
  in both ring profiles: 15 library objects, 13 `libsluice_async.a` members,
  25 installed headers — the contracted public surface F5 shipped.
- Installed probes, both macro views: 25 headers standalone-compile (0
  failures), cross-TU ODR link+run PASS, `removed_present` 0 — every header
  F5 removed is absent from the merged-master installed prefix — with the
  49-header pre-F5 prefix as the positive control.
- W-01/W-02/W-03 consumers link and run from the fresh merged-master
  prefixes in both ring profiles (inside the capture).
- The F5-era `tsan-n` exclusion (expected 51 with a required link-failure
  record) is superseded going forward by the #521 fix in PR #522: TSan now
  links and runs `request_scope_test` (52/52 with the target executed) —
  recorded in the same post-merge matrix.

The three pre-existing environment findings recorded by F5 are unchanged and
remain tracked in their own issues; this closeout does not relabel them.

Ledger: the 2026-10-14 closeout section in
`docs/roadmap/v1-conformance.md` records `F5 = CLOSED`. Issue #402 carries
this receipt; the issue itself remains open solely for the F6 final-audit
tail recorded by the PhaseM row.
