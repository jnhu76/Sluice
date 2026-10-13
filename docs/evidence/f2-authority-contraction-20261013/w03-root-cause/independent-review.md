# W-03 closure — independent adversarial review record

An independent adversarial review ran against the worktree at implementation
head `25734314` (it began at `b63a532e` and re-verified `25734314` after the
write-result portability fix landed mid-review). It was asked to disprove, not
agree: gate mechanics, phase-1 causal assertions (including an exhaustive
production signal-site enumeration for the `epoch_delta==1` claim), the
corrected loop's new-race surface, oracle preservation against
`git show 996f434f:tests/...`, the classification-A crux, the baseline
comparison, the evidence docs' accuracy, and minimality.

## Verdicts (reviewer's own words, condensed)

1. **Handoff race proven?** Yes — forced deterministically, with independent
   observations (core lookup, fd readability, epoch count, delivery count,
   park expiry), not derived from the fix.
2. **Does the fix repair the actual missing dependency?** Yes — the
   application eventfd is written strictly after the mutex store and polled
   alongside the library fd, restoring exactly the store-before-signal edge
   the retired carrier had structurally; phase 2/3 verify both directions.
3. **Library defect incorrectly reclassified as a harness defect?** No —
   classification A is consistent with the root spec (PROG-01: waking the
   driver is not a completion guarantee; PROG-02 obligations attach to
   actionable work/control transitions, both completed) and the exhaustive
   signal-site audit; the missing edge is application-owned and unfixable
   library-side.
4. **New deadlock/lost wake?** No deadlock in correct builds; close always
   follows join; no write-vs-close race on success paths.
5. **W-03 oracle still independently meaningful?** Yes — assertions
   additive, R14 byte-identical, the fd-only park still exercised.
6. **Source changes minimal?** Yes — production diff empty through the
   current HEAD.

## Findings and dispositions

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | MAJOR | "gates re-run at the corrected head" claims were ahead of the in-tree artifacts (gates.json still bound to `996f434f`, ci/runs.json stale) | Resolved by completing the re-collection: matrix/B-01/probes re-run at `25734314`, `ci/runs.json` + `ci/head-run.json` updated with the successful CI run `38387419975`, `gates.json` re-collected. Binding asserted by `collect-gates.py`. |
| 2 | MINOR | The doc misattributed the CI abort to `late`'s live binding; the actual fail-fast is the live external notification registration (early return never detaches) | Fixed in `w03-root-cause.md` (mechanism now stated as the un-retired registration in `~AsyncIoContext`). |
| 3 | MINOR | Early returns inside the forced-pause window park the helper until the 120 s alarm instead of failing fast | Disclosed as a limitation in `w03-root-cause.md`; bounded by the suite alarm and reachable only when the oracle is already failing for another reason. A scope-gate release guard would sharpen it; deferred to avoid churning the full gate cycle for failure-path ergonomics. |
| 4 | MINOR | Head drift: evidence bound to `b63a532e` while HEAD moved to `25734314` | Resolved: all binding documents updated to `25734314`; the intermediate campaign's `environment.txt` deliberately records its own `b63a532e` head. |
| 5 | MINOR | The corrected W-03 loop retains the file's uniform abort-on-failure style and leaks the eventfd on failure paths | Disclosed in `w03-root-cause.md`; pre-existing style, failure-only, process-lifetime. |
| 6 | NOTE | The deterministic test's preamble comment narrates the root cause (AGENTS.md discourages narration in C++) | Kept: consistent with the file's pervasive comment style; the authoritative record lives here and in `w03-root-cause.md`. |
| 7 | NOTE | A stale pre-fix binary under the repo-local `build/` (project buildir points elsewhere) nearly misled the review | Removed after the review; developers must build via the configured buildir. |
| 8 | NOTE | The cited CI run `38379190713` was absent from `ci/runs.json` | Resolved: all branch runs through `25734314` are now recorded. |

The reviewer independently re-ran the deterministic test live (exact phase
traces reproduced; 0 failures in 24 loaded runs and 20 idle runs) and
re-derived the pre-fix stress attribution. Blocking findings are resolved as
above; the closure record stands.
