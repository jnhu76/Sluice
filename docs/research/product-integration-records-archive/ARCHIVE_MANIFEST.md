# Archive Manifest — product-integration-records

Archived: 2026-10-30. Frozen record: contents are copies, byte-for-byte, of
the increment carried by `research/product-integration-records` (pushed
2026-10-29 without a PR, per that branch's INTEGRATION_LOG.md §8.4). Raw
samples were copied unmodified; no content edits, no omissions inside the
increment, no deletions.

## Provenance

- Source branch: `origin/research/product-integration-records`
- Source head: `4d621d91692a13c3bc4ebd5fcddaa8e5b18a8a35`
  ("research(integration): record archive push SHA in Phase 8 log")
- Records commit: `8c3adf6c3e6efdf473e8490ebc9855474dc06d2c`
  ("research(integration): archive E5 product integration records and
  Phase 3.3 final-head perf receipts", 38 files, +5025 lines, parent
  `4b962abb` = master history)
- Baseline at archive time: master `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`

## Increment scope (38 files, all paths relative to this directory)

- `INTEGRATION_LOG.md` — E5 product-integration execution log, Phases 0–8:
  repository snapshot and merge-identity determination (squash vs merge
  commits), Head SHA Guard receipts for #601/#610/#602 merges, #611 dedup,
  #612 re-anchor, GitHub write-back receipts, Phase 3.3 final-head ABBA
  perf receipts, independent reviews 3.4 (performance/integration, REVISE),
  3.4B (correctness, APPROVE), Phase 4 post-merge consolidation (CI matrix,
  3 admission mutations, installed-SDK consumer test, ABI stability), Phase
  7 independent integration audit, Phase 8 GitHub closeout.
- `INTEGRATION_PLAN.md` — pre-execution Phase 0 plan (merge conventions,
  PR snapshot, tree-hash gates, re-anchor method, backup-ref policy).
- `research/direct-beyond-parity/closure/perf-p2c1-finalhead/` — 36 files:
  Phase 3.3 final-HEAD perf regression receipts (ENV, PREREGISTERED_PLAN,
  SUMMARY, A/A and ABBA raw samples `aa-*.txt`/`ab-*.txt`, machid
  instruction-sequence dumps, `build-both.sh`, `run_aa.sh`, `run_ab.sh`).

## Overlap checks (executed 2026-10-30)

- vs `origin/master` (`84e1b7d5`): `git cat-file -e origin/master:<path>`
  for each of the 38 files — 0 already present.
- vs `origin/research/retention-wsl2-audit` (#614 content,
  `git ls-tree -r --name-only`, 4052 files): `comm -12` intersection with
  the 38 candidates — 0 files.

## Credential / host-private scan (executed 2026-10-30)

- `grep -riEn 'pass(word)?[:=]|token|secret|api[_-]?key|BEGIN [A-Z ]*PRIVATE KEY|ghp_[A-Za-z0-9]{20,}|github_pat_'`
  over all 38 files: 3 hits, all benign prose in INTEGRATION_LOG.md —
  line 250 (enumeration of scan patterns used by the campaign's own secret
  scan), line 295 ("secrets = 0 命中" result statement), line 330
  ("pass=818 problems=0" verification count). Disposition: kept unmodified;
  no credential values present.
- Supplementary scan for `AKIA[0-9A-Z]{16}|ssh-rsa|ssh-ed25519|\.pem|aws_|AWS_SECRET|xox[baprs]-`: 0 hits.
- Host-private information: `/home/jnhu/...` absolute paths, `host: E5`
  label, kernel/CPU/loadavg environment data appear in the receipts.
  Precedent: identical information classes are already published on master
  (331 files under `research/` contain `/home/jnhu`, e.g.
  `research/direct-beyond-parity/EVIDENCE_MANIFEST.json`; `host: E5` in
  `research/direct-beyond-parity/p1-raw/`) and in the #614 retention
  archive (109 files). Disposition: kept unmodified as raw-sample fidelity
  takes precedence; no hostnames or machine identifiers beyond this class.

## Fidelity verification

Each archived file was compared by sha256 against
`git show origin/research/product-integration-records:<path>`:
38 checked, 0 mismatches. The table below lists the archived sha256 per
file (paths relative to this directory).
```
156948ddccba1201667885faadae36a0facab0537e90ec180d21871f569ab1a5  INTEGRATION_LOG.md
e2084b4afa10acb45f77c653c206c90e1ce45d78a9552e9beeb41fd491a502b9  INTEGRATION_PLAN.md
d08693817db927871363ec863f8803de02b2f0f28d8b6e3782af1b2dc4eaea14  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ENV-P2C1-FINALHEAD.txt
c3705b8f16c3bf7d1c532863adfaf594c69090a61e4174e77d5886f8d555a383  research/direct-beyond-parity/closure/perf-p2c1-finalhead/PREREGISTERED_PLAN.txt
9bf892531537e8249c939bcc4f4953ef818033bb0073a86e380812dc3c709cee  research/direct-beyond-parity/closure/perf-p2c1-finalhead/SUMMARY.md
7e1f69cf39ca8105c292da77a12f61173f3645d38425eec1ade8f79ba241d59b  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-a2.txt
39262abeb66bdae54711b96e0617579ca078d49f91535950583ca4abd61a36d2  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-ac.txt
a2543a2fc9d8b5c7e50ba18d956b248b8bb8c7be78e125c10ee247370736e5c4  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-ax-seq.txt
626076699ccbfb43ffee9faa00a8bb8e9013236ab1142fb36972ab110d88f58f  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-ax-tail.txt
3672391647f883b69c39ef1e03ed82bc5c011270d6f22d8773114251dbb68a9b  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-w2.txt
da5c565757da97a027f14d3fa24939fcee16a759364250ad16ffbd70b91c22df  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-wc.txt
469fad9b5541bf2e4c08c0ba4a90addcc9fe2fa9ab7b73b3de14037770bb53bb  research/direct-beyond-parity/closure/perf-p2c1-finalhead/aa-wx.txt
64309af8efed38daaf6e9b68d7bf344efb002b731be1ce78b789ad6575cf32b5  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-comp-read-4k.txt
388546539f239e0778208b6cc54e4a27e86244df0a62144ea07831246d742654  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-cursor-4k.txt
7359822592128363453976a3f486e39c1fbeb291162a5ac3da909ac9734ad454  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-eofmix-4k.txt
795fad66e18ea21f7c31aae7261cae0858829e244be03593495dd5dab006e3be  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-neg-read-at.txt
ace37eef7904756a4e191d82d581f9263c5ed55794b364bdeeb3cf387058ddb0  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-neg-write-at.txt
043ef90bf79ac0eadb9ce6bc9b4190889935f17756fa8995c5abe8fd6a18d46a  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-write-all-at.txt
32489f8c8e7bcc197a7ff99238b8b3c104077d1b83c0eb1ecb7626a6b1f7b0e4  research/direct-beyond-parity/closure/perf-p2c1-finalhead/ab-write-all.txt
1369df083f0dd225e240c0b9679351d2c7e88609b54654a85a1fef04e69dfa53  research/direct-beyond-parity/closure/perf-p2c1-finalhead/build-both.sh
8435cd1b6efc94a75c9a58372c4e8df80e305e2c1d33c8c95a5c8603a8a8630c  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-file_info.txt
454089e2e8384cb39f892f8ed317c8f7e8d5aa820647daedb2c56f8cdeaa324e  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-read.txt
3a6002ca03f98872cc56b98d185517356d113475d6d8df32b8178c2f5536a10b  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-read_at.txt
7a2560cf350d31ed54e0b8e33996d1b831e1d480ed43b6eb99309dbad9a65376  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-read_exact.txt
5a20f361dd94ac2107103f798a1eedf7cabe9e3d011b0382d01515503005ce02  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-read_exact_at.txt
319724e1acc671830db20c28a0e4e4eeaa673051f06c2677789005d25b48e41c  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-write_all.txt
7910ddc7f8e539667632674390996b851bb1c378de6997e913938fe9facf59ab  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-write_all_at.txt
3a6002ca03f98872cc56b98d185517356d113475d6d8df32b8178c2f5536a10b  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/base-write_at.txt
8435cd1b6efc94a75c9a58372c4e8df80e305e2c1d33c8c95a5c8603a8a8630c  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-file_info.txt
454089e2e8384cb39f892f8ed317c8f7e8d5aa820647daedb2c56f8cdeaa324e  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-read.txt
3a6002ca03f98872cc56b98d185517356d113475d6d8df32b8178c2f5536a10b  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-read_at.txt
1defb9102d6212786bba7516222cd1a9569af75b7fa75a7baf1a06cc37722d61  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-read_exact.txt
2f63d39e1f9a26392d801d5dc06e21d704335a934ceae902a79e56cd31ed3bec  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-read_exact_at.txt
60f2af5f9bb04453862dee6318b9aa4d9db338d36cb065986ee9562b1451e230  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-write_all.txt
b973acff4dcad00118ff15764eb61a4bd07d01562a90f71fc0ca7ccaca0e8ee7  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-write_all_at.txt
3a6002ca03f98872cc56b98d185517356d113475d6d8df32b8178c2f5536a10b  research/direct-beyond-parity/closure/perf-p2c1-finalhead/machid/head-write_at.txt
8603c885b19096defc56a47e87e9a032af290a4d42669e53470427a49981a473  research/direct-beyond-parity/closure/perf-p2c1-finalhead/run_aa.sh
85803c2b227466def01a4b76fa6deb8660b406e8477ad16e5dcccfcfeece320f  research/direct-beyond-parity/closure/perf-p2c1-finalhead/run_ab.sh
```
