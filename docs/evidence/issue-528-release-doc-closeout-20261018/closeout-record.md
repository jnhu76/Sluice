# Issue #528 closeout — v2 release documentation convergence

## Scope and baseline

| Field | Value |
|---|---|
| Base | master `9f22bde6b6b441d0ff80bded7ecf7b9b1a78c8f3` (working tree clean) |
| Branch | `docs/v2-release-528-closeout` |
| Date | 2026-10-18 |
| Diff class | Markdown only (`git diff master...HEAD --name-only` → 5 files, all `.md`) |
| Production / test / build / formal / root-spec diff | NONE / NONE / NONE / NONE / NONE |

Findings handled (from `docs/evidence/f6-final-correspondence-20261014/findings.md`):
F6-F05 (CHANGELOG `[Unreleased]` refresh) and F6-F16 (acquire_execution /
ready-sink seam classification), plus the release-facing status drift that
#528's "+" scope covers. Out of scope and untouched: #541, #390, #403,
#537–#540, the v0.0.2 tag and GitHub release.

## F6-F05 — CHANGELOG curation

`[Unreleased]` now describes the net v0.0.1 → planned-v0.0.2 change for a
library user:

- Added: direct-execution surface (`File` + `sluice/blocking/file.hpp`),
  request execution with public `RequestId`, explicit backend factories,
  bounded progress / external event loops, observers, shutdown with
  retained results, `RequestScope`.
- Changed: four retained applications on the v2 surfaces (with the
  `--workers` CLI note), request-handle layout (48 bytes, recompile),
  package shape/profiles, tested environment range, repository hygiene
  contraction, changelog restructure.
- Removed: the v0.0.1-era synchronous core surface and cooperative async
  runtime (family-level enumeration verified against
  `git ls-tree -r v0.0.1 --name-only include/`), the stackful host
  bundle, the 25→11 install contraction (25 = the v2 campaign's
  pre-contraction interim, stated as such), and the closed public
  backend-extension seam.
- The D3 breaking banner, the 11-header enumeration, the environment
  range and the no-compatibility disclosure are retained (verified
  against `xmake/libraries.lua` and
  `docs/evidence/v2-release-acceptance-20261017/environment.txt`).

Scheduler history disposition: the old Fixed entry (#229, PR #231 — a
TSan test-seam repair merged at `d130f350`, 2026-08-26 22:59:23 +0800,
after the `v0.0.1` tag at 21:18:48 +0800) described a component the v2
contraction deleted (`Scheduler` has no definition in `src/` or
`include/`). Presenting it as a v0.0.2 fix would mislead; the repair
record stays in Git history and the `[0.0.1]` section is untouched. No
other Fixed entry is net-visible from v0.0.1: the intermediate campaign
fixes (io_uring owed-delivery residue, #543; observer retirement at the
delivery owner) predate any release of the semantics they fixed.

## F6-F16 — seam dispositions

Recorded as a dated section in `docs/roadmap/v2-conformance.md`, derived
from source at `9f22bde6`:

- `RequestCore::acquire_execution` = PROTOCOL_TEST_ONLY (no production
  caller; `accept` takes the first execution reference at
  `request_core.cpp:210`; executor handoff is `claim_execution`; test
  callers are the protocol suite; unreachable from the installed
  surface; mirrors the `AcquireExec` model action).
- Ready-sink: production attaches no routing sink (backend-owned default
  receives events); test injection only via the uninstalled
  `TestAccess::context_set_ready_sink`; no production rebind path;
  attaching caller owns the sink lifetime; `PUBLIC_API_IMPACT = NONE`.
  The `attach_progress_port` install-once contract is explicitly not
  projected onto `attach_ready_sink`.

## Release-facing status drift fixed

- `docs/roadmap/v2-conformance.md`: actual-surface row states the
  current 11-header allowlist (assessed-commit 25-header snapshot kept
  as the dated baseline); overall status records the PR #547 merge
  (`9f22bde6`) making the gate statuses effective per GOV-05, the
  master-vs-acceptance-head delta (docs + one test-only `9b7bfa63`),
  and that `RELEASE_TAG_CREATED` is not claimed; Release Gate table and
  the G10 row read VERIFIED with the PR-head evidence SHAs retained.
- `docs/README.md`: the completed public-surface contraction and host
  removal no longer listed as the ledger's open items.
- `apps/sluice-hash/README.md`: "retained Scheduler/Fiber mechanisms"
  reworded — no Scheduler/Fiber exists in the tree.
- `apps/sluice-copy/README.md`: `--workers` described through the public
  `make_threadpool_context` factory, not the removed injected-backend
  API.

## Validation (docs-only equivalent battery)

1. `git diff master...HEAD --check` — clean.
2. `git diff master...HEAD --name-only` — 5 files, all Markdown;
   `git diff master...HEAD -- docs/explicit-io-contract.md` empty;
   no `docs/evidence/**` record modified by the branch.
3. Installed-header count: `xmake/libraries.lua` allowlist resolves to
   exactly 11 headers, letter-identical to the CHANGELOG and ledger
   enumerations.
4. Every public API name in the new CHANGELOG entries checked against
   the installed headers (`blocking/file.hpp`, `file_resource.hpp`,
   `async_io_context.hpp`, `request.hpp`, `request_scope.hpp`,
   `threadpool_backend.hpp`, `uring_backend.hpp`), including the
   explicit-offset vs current-offset `read`/`write` split
   (`src/blocking_file.cpp` pread/pwrite vs read/write).
5. Gate provenance: `gh pr view 547` MERGED at `9f22bde6` (2026-10-17),
   `gh pr view 545` MERGED at `9845cee1`; all cited SHAs
   (`7efd86ec`, `ff7d6dde`, `a0874ae2`, `6411cff3`, `5fd8e109`,
   `72acc209`, `9b7bfa63`) exist and are ancestors of master; every
   cited evidence path exists.
6. Relative markdown links in the changed files resolve on disk.
7. App claims cross-checked against code: `copy_task.cpp:299`
   (`make_threadpool_context`), `hash_task.cpp:55`
   (`sluice::blocking::read_at`), `--workers` rejection strings in
   tail/grep/hash `cli_parse.cpp`.
8. No new public Scheduler/host/ready-sink compatibility promise
   anywhere in the diff; no CI_PASS claim. CI note: because the PR
   touches `apps/**`, the ci.yml `paths:` filter does trigger on it;
   the docs-only equivalent battery above is recorded regardless.

Docs-only change: the v2 release acceptance battery is inherited; no
matrix, TLA+ or sanitizer re-run is required or claimed (no source SHA
referenced by the acceptance evidence changed).

## Independent review

Two independent-context reviews of the branch (both re-ran the source
checks themselves; both against the pre-review tip, with the two
post-review P3 fixes noted below):

- Reviewer A (Architecture / Contract): **PASS_WITH_NOTES**. All eight
  attack items verified (root unchanged; v2-only; 11-header boundary;
  acquire_execution claims (a)–(e) line-verified; ready-sink claims
  verified; gate migration provenance; no evidence promotion; CHANGELOG
  API names letter-checked, `sizeof(Request<int>) == 48` confirmed by
  compilation).
- Reviewer B (Release / Provenance): **PASS_WITH_NOTES**. CHANGELOG
  completeness, Scheduler disposition, RG1–RG7 reality, README state,
  link/SHA provenance, release-blocker scan all verified; confirmed the
  `[0.0.1]` section byte-identical and no performance work presented as
  shipped.

No P0/P1 findings; no load-bearing P2. P3 notes applied after review:
the ready-sink serialization wording ("serialized against context
operations", not "other TestAccess setters") and the CHANGELOG
`--workers` CLI note. P3 notes recorded without change: the
`READY_SINK_REBIND_SUPPORTED` label is kept for the task's required
classification vocabulary while its value states the no-rebind fact; the
Scheduler Fixed-entry omission is the documented disposition above.

## Verdict

```text
F6_F05_STATUS = CLOSED (CHANGELOG curated for the v0.0.2 pre-release)
F6_F16_STATUS = CLOSED (dispositions recorded in the v2 ledger)
RELEASE_BLOCKERS = none found by either reviewer
RELEASE_TAG_CREATED = NO (v0.0.2 acceptance + tagging is a separate task)
```
