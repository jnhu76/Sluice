# R9 Archive Increment — closure/ evidence appended to the research branch

Archive increment executed: 2026-10-29 (E5 host). Executor: E5 closure
closeout (release lane). This record extends `r1-evidence-archive.md` (which
documented the 2026-10-28T23:32:28Z freeze, 615 files, branch
`research/direct-beyond-parity-evidence` commits `292e0734`+`c2a57a77`, PR
#610) with the closure-phase evidence produced after that freeze by lanes
R3–R8, the serial perf lane and the two R9 audit lanes.

## 1. Scope and fidelity

- Source: the live untracked evidence tree
  `/home/jnhu/Source/Sluice/research/direct-beyond-parity/` on master
  `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (unchanged; re-verified
  `git ls-remote origin refs/heads/master` before this increment).
- Content diff against the branch: byte-comparison (`cmp`) of every file
  present in both trees → **0 changed files**; the increment is purely
  additive plus the two administration files listed in §4
  (`EVIDENCE_MANIFEST.json` extension and `WSL2_VALIDATION_HANDOFF.md`
  rewrite).
- **201 new text files**, all under `research/direct-beyond-parity/closure/`:
  lane reports (`r2-pr-boundary.md`, `r3-c1-review.md`, `r4-p2c1.md`,
  `r5-prep.md`, `r5-native/*`, `r5-async-host.md`, `r5-r0-verdict.md`,
  `r6-ac1.md` + `r6-ac1/*`, `r7-ac2.md` + `r7-ac2/*`, `r8-ac3.md` +
  `r8-ac3/*`, `DUCKDB_FIO_R0_PREREG.md`), the serial perf lane
  (`perf-p2c1/*` raw samples, PMU diagnostics, machine-identity files,
  hash-pinned runner/analyzer/fixture sources) and the three files this
  closeout adds (§4). No existing archived file was modified, deleted or
  re-encoded; git history retains every prior version.

## 2. Binary disposition (3 ELF executables NOT committed)

`closure/perf-p2c1/` contains three ELF binaries that are reproducible build
products of the committed sources (`ladder.cpp`, `mkfixture.cpp`,
`build-both.sh`), retained in the live tree and the preserved perf worktrees
(`Sluice-perf-p2c1-base` @ `77e5399a`, `Sluice-perf-p2c1-head` @ `70d48f5e`)
for reproduction. Per repository discipline (commit the evidence, not the
collector) and the R6 probe precedent (source committed, binary pinned by
hash), they are excluded from the Git archive and pinned here:

| File | sha256 | Built from |
|---|---|---|
| `perf-p2c1/ladder-base` | `89084b1d1bd953c2da5202fc789034700269271b0c2d29f04c8c4408c66b5c0d` | `ladder.cpp` @ `4455d98d` kit + `77e5399a` library (see `perf-p2c1/SUMMARY.md` §1) |
| `perf-p2c1/ladder-cand` | `3af44bf9020284dad412635f9f3912c1f7cf06232128b73517468868513e9840` | `ladder.cpp` @ `4455d98d` kit + `70d48f5e` library |
| `perf-p2c1/mkfixture` | `bde3c72404682ac33d510b36a0b166433700d02260253b8ccd0eb9e404913ace` | `mkfixture.cpp` (byte-exact #600 snapshot) |

The two ladder hashes match those recorded in `perf-p2c1/SUMMARY.md` §1.
Everything else in the increment is plain text (no other non-text file exists
in the diff set).

## 3. Manifest extension

`research/direct-beyond-parity/EVIDENCE_MANIFEST.json` is extended in place
with one entry per newly archived file (201 new closure files + the 2 review
transcriptions + this record + the rewritten handoff), using the same field
rules as R1 §4 (path + sha256, best-effort `source_sha`/`host`/`arm`/
`toolchain`/`status`/`report_ref`/`reproduction` derived only from each
file's own header or path, `""` when not derivable; manifest still does not
index itself). Entries for pre-existing files are untouched **except**
`direct-beyond-parity/WSL2_VALIDATION_HANDOFF.md`, whose sha256 now reflects
the rewritten content (the superseded P6-era version remains retrievable at
branch commit `292e0734`). Verification command is unchanged:

```sh
python3 - <<'EOF'
import json, hashlib, pathlib
root = pathlib.Path(".")
for e in json.load(open("research/direct-beyond-parity/EVIDENCE_MANIFEST.json")):
    d = hashlib.sha256((root / e["path"]).read_bytes()).hexdigest()
    assert d == e["sha256"], e["path"]
print("all manifest sha256 verified")
EOF
```

## 4. Closeout-authored files in this increment

- `closure/r9-independent-correctness-review.md` — transcription of the R9
  §12.1 audit result (verdict APPROVE; findings + independently-re-checked
  list).
- `closure/r9-independent-performance-review.md` — transcription of the R9
  §12.2 audit result (verdict APPROVE; findings F1–F6 + 12-check list).
- `closure/r9-archive-increment.md` — this record.
- `direct-beyond-parity/WSL2_VALIDATION_HANDOFF.md` — rewritten as the frozen
  handoff for the actual product candidate (Base `77e5399a` = PR #602 head,
  Candidate `70d48f5e` = PR #612 head), superseding the P6-era draft that
  targeted the local research candidate `deaf0301`. No WSL2 command was run
  in the producing session.

## 5. Delivery

- Branch: `research/direct-beyond-parity-evidence` (unchanged base master
  `bdfd0d8d…`); Research PR: https://github.com/jnhu76/Sluice/pull/610
  (OPEN, not merged) — updated in place by this increment.
- Cross-reference: `closure/r6-ac1*` is additionally carried by the AC-1
  design PR https://github.com/jnhu76/Sluice/pull/611 (branch
  `research/r6-ac1-vectored-semantics`, commit `999b5305`, OPEN); the two
  research branches intentionally overlap on that subtree. `DUCKDB_FIO_R0_PREREG.md`
  is additionally carried by dududu PR https://github.com/jnhu76/dududu/pull/7
  per the R5 lane's recorded intent.
- Increment commit SHA: recorded in the follow-up commit on this branch (a
  record cannot contain its own commit SHA).

## 6. Boundary compliance

No PR merged; no product source, build file, test or public API change
included (diff confined to `research/direct-beyond-parity/`); v2-r1 untouched;
no force-push; no history rewrite (branch is append-only); frozen worktrees
untouched; no benchmark run by this lane.
