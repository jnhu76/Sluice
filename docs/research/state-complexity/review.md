# M0/M1 Independent Review Record

Three independent reviews were executed against the study at BASE
`8fdcd99c`: Review A (architecture & semantics), Review B (performance &
evidence), and the owner review of the open PR (closeout below).
All returned REVISE; every P1/P2 finding was dispositioned and the fixes
landed in the documents and artifacts before the final conclusions. The
verbatim findings and their resolutions are recorded here.

## Review A — Architecture & Semantics (verdict: REVISE → resolved)

Verified: 30+ load-bearing source citations resolve; complexity derivations
correct (hot/cold classification honest); all three invocation forms
traced; no diagnostic-path-as-hot-path confusion; no production changes;
no conflict with recorded ledger evidence.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| A1 | P1 | H3 evidence misreported: the artifact shows a freelist regression on the fifo pattern at caps 8–256, absent from the doc; "W-02 shape" label on saturated-random wrong; example numbers didn't match the artifact | **Fixed**: m1-results.md regenerated from the committed run; the fifo regression table (+15–21% at caps 8–64, disjoint ranges) is disclosed; saturated-random relabeled adversarial; H3 decision changed to REJECT (free list) + INVESTIGATE (scan share) |
| A2 | P2 | H1 `user_data` encoding silently narrows the cookie domain (2^63→2^48) and requires capacity ≤ 32768; undocumented | **Fixed**: envelope (1+15+48 bits) and both domain obligations stated in m1-hypotheses.md and m1-results.md; carried into the PROPOSE_UPSTREAM sketch and next-round test matrix |
| A3 | P2 | H2 counter as sketched would violate BACKEND-04 if settlement/destruction read it | **Fixed**: scope restriction added — counter serves the per-pass verdict and wait fast paths only; `internal_work_retired()` and destructor quiescence keep the authoritative scan |
| A4 | P2 | Layer B attributions not reproducible from committed artifacts | **Fixed**: resolved per-symbol attribution tables committed for both profiles (`results/perf-uring-d64-attribution.txt` 711 samples, `results/perf-threadpool-d64-attribution.txt` 680 samples) with method + commands in `results/layerb-provenance.txt` |
| A5 | P3 | M1 tables didn't match cited artifacts | Fixed: all tables regenerated from the committed 9-rep decision run |
| A6 | P3 | `publish_one` citation range wrong | Fixed (owner-side drain vs callee separated) |
| A7 | P3 | L2 "D≈1" over-broad under concurrent submitters | Fixed (single-submitter qualifier added) |
| A8 | P3 | W-03 registration edges and observer cancel/retire missing from the DAG | Added (both O(1), cold; no verdict changes) |
| A9 | P3 | inventory: `workers_.reserve` exception; unclosed backtick | Fixed |
| A10 | P3 | Re-basing obligation (G4/G6, RG5 mutant) unstated; bench collector duty | Fixed: re-basing obligation recorded in m1-results.md evidence boundaries; runner's standing duty, consumers and success criteria documented in `bench/mechanisms/run_mechanisms.sh` header and `bench/mechanisms/README.md` |

## Review B — Performance & Evidence (verdict: REVISE → resolved)

Verified: H1 arms byte-identical in duties with the candidate charged extra
work; checksum sinks prevent DCE; all three negative controls bite by
construction and were observed CAUGHT; harness-only control correctly
scoped; key Layer A deltas robust (disjoint ranges); no "Sluice is X%
faster" claims.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| B1 | P1 | Layer B attribution numbers unverifiable; committed capture showed symbol-resolution failure | **Fixed**: same as A4 — resolved tables + provenance committed; percentages re-derived from the committed tables |
| B2 | P1 | `perf stat -o` without `--append` destroyed per-arm perf_stat.txt | **Fixed**: script uses `--append` with per-section headers; full 7-section perf_stat.txt re-captured and committed |
| B3 | P2 | Results prose cited an uncommitted run | Fixed: every number in m1-results.md now quotes the committed artifacts |
| B4 | P2 | Experiment code/results untracked; provenance can't pin them | Fixed: code and results committed in this PR; provenance now records dirty paths and the runner regenerates everything from the committed sources |
| B5 | P2 | H1 encoding bounds undocumented/untested | Fixed (see A2); the capacity-sweep ceiling and cookie-boundary oracle case are named as required next-round work in the proposal sketch |
| B6 | P2 | H2 arms not set/clear-parity equivalent on double-set, untested | Fixed: documented production invariant (control-ref pins the slot until delivery ⇒ no double-set before delivery) as part of the proposal duties; driver matches the precondition |
| B7 | P2 | H3 ρ=0.5 "inside noise" false — fifo regression undisclosed | Fixed (see A1) |
| B8 | P2 | Layer B point estimates without uncertainty | Fixed: Poisson 95% CIs added to every Layer B percentage; "ceiling" softened to single-run upper-bound estimates |
| B9 | P2 | saturated-random mislabeled as W-02 shape | Fixed (see A1); fifo-saturated noted as unmeasured with the expected-O(1) argument recorded |
| B10 | P3 | Interleaving claim over-broad; H2 timed region includes model checksum walk; H3 full oracle runs 3×/config | Documented honestly (m1-hypotheses.md and m1-results.md state which arms interleave and that H2 absolutes are inflated with deltas valid) |
| B11 | P3 | Runner defects: H2 ops override, zero-filled solo JSONL rows, JSONL protocol mixing, provenance flags | Fixed: per-binary ops env (SLUICE_M1_PASSES), solo emit suppression, provenance lists dirty paths and real flags, runner self-checks CAUGHT/PASS and fails on oracle divergence |
| B12 | P3 | Wording: "2-24x" wrong, lock-pair delta attribution, ledger 15-25ns vs measured 5.7ns, "confirms directionally", "resolve two-thirds" unmeasured | Fixed throughout (ledger now cites the M1-measured pair cost; scope64 case downgraded to sign-consistent/uninformative; modeled splits labeled as estimates) |
| B13 | P3 | Raw results in repo vs performance-methodology rule | Documented as a deliberate research-evidence exception (charter §11/§12 governs this study; no production change is claimed from it) — recorded in m1-results.md evidence boundaries |

## Post-review state

Both reviewers' P1/P2 items are resolved with committed evidence; remaining
P3 documentation caveats are recorded in place. The final decision table in
`m1-results.md` (H1 PROPOSE_UPSTREAM with envelope obligations; H2
PROPOSE_UPSTREAM with BACKEND-04 scope; H3 REJECT free list + INVESTIGATE
scan share) reflects the post-review evidence.

## Owner review of the open PR (2026-10-20) — closeout

A third review (GitHub review `5589871022`, verdict REVISE) examined this
PR at `31446946` together with the production cookie, transport-ledger,
cancellation and completion paths at the base commit. Its findings and
their closeout in this commit:

| # | Severity | Finding | Closeout |
|---|---|---|---|
| P1 | P1 | The H1 sketch encoded only the operation SQE, but `RouterEntry.cookie`, both `TransportLedger::Entry.cookie` values, the `io_uring_prep_cancel64` match target, the control SQE/CQE, `mark_consumed_control_locked_` and poison/Class-A recovery all carry or consume the same identity at baseline, so an SQE-only encoding breaks cancel matching and recovery routing; a complete authority/dataflow table, named deterministic oracles and an explicit domain-narrowing decision were required for research closeout | The H1 proposal sketch now states the full-consumer format rule (which fields move to the packed identity, which keep the bit-63 tag, where decode-and-verify is required), points to the complete 16-row producer/consumer/verification matrix with its negative controls in `docs/research/native-linux-m2/h1-identity-matrix.md` (native-Linux M2, PR #562), names the cancellation/recovery/stale-cookie/envelope oracles, and records that narrowing `request_capacity` to ≤ 32768 and the cookie horizon to `2^48` is an explicit owner/contract decision |
| P2 | P2 | The H2 conditional complexity was misstated: the sweep is O(1) only when zero events are owed; with any event owed it still scans all C slots, so it is not an O(owed) traversal. H2a (single-lock snapshot versus uring C per-slot lock pairs) and H2b (derived count skipping empty scans) must be analyzed as separate gains, and producer interleavings plus post-pass residue/event order belong in the next protocol-oracle matrix | The Layer A prose and the synthesis table now state O(1) empty skip and O(C) scan + O(K) delivery when nonempty; the proposal sketch decomposes H2a and H2b; the next-round matrix names the concurrent-producer and post-pass residue/event-order oracles |

Nonblocking caveats: the single-run Layer B percentages already carry
Poisson CIs and upper-bound framing; the ≈44% unresolved `[unknown]`
profile bucket is now explicitly excluded from claims in `m1-results.md`.

Subsequent native-Linux M2 evidence (PR #562) validates the P2 separation
empirically: H2a and H2b are independently confirmed mechanisms, H2b
dominates on the decisive 4 KiB/64 KiB configurations (single-lock
per-slot sweep removed), and H1's end-to-end share is bounded at ≤ 3.7%,
which leaves H1 MICROBENCH_ONLY and H2b the only production candidate.
