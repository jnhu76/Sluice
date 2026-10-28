# R1 Evidence Archive Record — research/direct-beyond-parity (Sluice #603)

Archive executed: 2026-10-28 (E5 host). Executor: E5 closure workflow R1
evidence-archive subagent. This record documents the inventory, contaminant
triage and disposition of the untracked `research/direct-beyond-parity/`
directory, which holds the P0–P6 Direct Beyond-Parity research evidence
announced COMPLETE in #603 but never previously delivered through Git.

## 1. Snapshot freezing

- Source: untracked tree `/home/jnhu/Source/Sluice/research/direct-beyond-parity/`
  (main worktree, branch master @ `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`,
  verified equal to `origin/master` and `git ls-remote refs/heads/master` in
  the R0 snapshot).
- Copy time (UTC): 2026-10-28T23:32:28Z, via `cp -a` into the isolated
  worktree `/home/jnhu/Source/Sluice-e5-evidence` created at
  `git worktree add ... -b research/direct-beyond-parity-evidence bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`.
- Copy fidelity: `sha256sum` over all 613 files of source and copy, sorted,
  `cmp` — byte-identical (0 differences). No file was added, removed, edited
  or re-encoded during archiving.
- The main worktree was not touched: no branch switch, no file move, no
  clean; `research/` there remains as the live evidence source.

## 2. Inventory (613 files, 6.7 MiB, all regular text files)

| Location | Files | Content |
|---|---|---|
| top level | 19 | MISSION, phase reports (P0 baseline, P1 cost floor, P2C1/P2C2 candidates, P3 mechanism study, P4 real workload, P5 interaction outcome, integration & independent review), preregistered plans, ledgers/registers, WSL2 handoff |
| p1-audit/ | 6 | P1 static audits (positioned reads, writes, composition read, resource/metadata ops, state change/persistence) |
| p1-raw/ | 98 | P1 raw ABBA/paired measurement cells, PMU/rusage/strace sweeps, kit identity, smoke ladders |
| p2-raw/ | 85 | P2C1 + P2C2 raw matrices, A/A, PMU, syscall counts, mutation receipts, disassembly (objdump), kit identity |
| p2-scripts/ | 2 | P2C1 matrix + mutation runners |
| p3-families/ | 4 | M1–M4 mechanism family analyses |
| p3-raw/ | 112 | M1–M4 raw experiment results + kit smoke |
| p4-raw/ | 44 | P4 W01 local workload + Parquet workload raw results, identity/env-pin records, strace/PMU physical work, A/A, negative controls |
| p4-scripts/ | 1 | P4 smoke runner |
| p5-raw/ | 233 | P5 A+B interaction matrices (release/debug/asan/asanubsan), 10 mutation-run directories with build logs and stderr receipts |
| p5-scripts/ | 2 | P5 combined matrix + mutation runners |
| probe-snapshot-600/ | 6 | #600 historical probe snapshot (ladder.cpp, mkfixture.cpp, paired.sh, paired2.sh, analyze_pairs.py, README) |
| closure/ | 1 | E5_CLOSURE_SNAPSHOT.md (R0, complete) + this record |

Notes:

- No binary files exist anywhere in the tree (`file` classification: text,
  scripts, C source only). Largest file is a 342 KB objdump disassembly
  transcript (`p2-raw/P2C1/recount-20261028/ladder-b1-binary.objdump.txt`).
- 36 zero-length files are `mutants-*-base.err` / `mutants-rules-control.err`
  stderr captures from mutation runs. They are retained as evidence: an empty
  stderr is the recorded receipt that the base leg produced no error output.
- `p2-raw/p2c1` and `p2-raw/P2C1` (case-differing directories) are both
  retained verbatim; renaming or merging them would rewrite evidence paths.
- `probe-snapshot-600/` is the #600 historical probe evidence; it is archived
  verbatim in place, not overwritten.

## 3. Contaminant triage

Checked for: build caches (`__pycache__`, `*.o`, `*.so`, `*.a`, `*.pyc`,
`build/`, `.xmake`), temp/editor droppings (`*~`, `*.swp`, `*.bak`, `*.orig`,
`*.rej`, `.#*`), non-regular files (symlinks, fifos, sockets), secret-shaped
strings (API keys, `ghp_`/`github_pat_`/`AKIA`/`xox`-tokens, private key
blocks, password/credential material), unexpectedly large binaries, and
unrelated data.

Findings:

- No build caches, object files or editor droppings. Filename matches such as
  `mutants-build.log` and `*-rebuild-sha256.txt` are text logs/receipts, not
  build products.
- No non-regular files; no symlinks.
- No secrets or credentials. The only `password`/`credential` matches are
  prose statements recording that the sudo credential was deliberately NOT
  recorded (e.g. `p1-raw/read-cold-4k.txt` header: "credential not recorded
  in any file").
- No oversized or unrelated files. Nothing was moved to quarantine and
  nothing was deleted; the archive is a verbatim snapshot plus the two
  archive-administration artifacts described in §4.

Disposition: quarantine empty; controlled-archive plan not triggered (total
6.7 MiB, well inside normal Git scope, largest file 342 KB).

## 4. EVIDENCE_MANIFEST.json

`research/direct-beyond-parity/EVIDENCE_MANIFEST.json` is a JSON array with
one entry per archived file, sorted by path. Every entry carries `path`
(repo-relative) and `sha256` (lowercase hex). Best-effort metadata fields —
derived only from the file's own header lines or its path, left `""` when not
derivable — are: `source_sha` (git SHA found in the file header), `host`
(`E5` when the file's own header states it), `arm` (path-derived experiment
arm/phase), `toolchain` (compiler versions stated in the header), `status`
(classification: MISSION/REPORT/PLAN/LEDGER/HANDOFF/AUDIT/RAW_RESULT/RUNNER/
ANALYSIS/HISTORICAL_PROBE_600/CLOSURE_SNAPSHOT/CLOSURE_ARCHIVE_RECORD),
`report_ref` (the governing report/plan document), `reproduction` (the `cmd:`
line from the file header, or the runner invocation for runner scripts).

Scope: the manifest indexes every archived file **except itself** — a file
cannot contain its own sha256. Verify with:

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

The manifest generator itself is a one-shot /tmp tool (per repository hygiene
rules the collector is not committed; `sha256sum` over the tree plus the
documented field rules above fully reproduce it).

Half-finished artifacts: at copy time `closure/` contained only the completed
`E5_CLOSURE_SNAPSHOT.md`; no partial work from other lanes was present,
nothing was skipped.

## 5. Delivery

- Branch: `research/direct-beyond-parity-evidence`, base = master
  `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`.
- Content: this archive only (the `research/direct-beyond-parity/` tree).
  No Sluice product source, build files, tests or public API changes are
  included; the PR touches nothing outside `research/direct-beyond-parity/`.
- PR: see §6.

## 6. PR record

(filled after PR creation)
