# Performance Feedback Ledger

Performance findings from attribution rounds, mirroring the rules of
[`app-feedback-ledger.md`](app-feedback-ledger.md): an entry records
*evidence and status only*. It does NOT authorize implementation — every
CORE-side candidate additionally needs the methodology of
[`docs/verification/performance-engineering.md`](../verification/performance-engineering.md)
(attribution → scaling signature → decomposition → economics → placement),
and any concurrency-semantic change needs the architecture/formal gate.

## Rules

- seeded only with measured evidence (artifact-backed where marked);
- conservative classification: an aggregate cost is not called a per-request
  handoff tax until decomposition proves it;
- status vocabulary: `APP_FIXED` / `INVESTIGATE` / `CORE_CANDIDATE` /
  `BOUNDARY_CANDIDATE` / `OPTIONAL_CANDIDATE` / `DEFERRED` / `REJECTED`;
- a `CORE_CANDIDATE` still requires Common-Tax or material-Cliff-Weakness
  evidence plus an engineering-economics and placement decision before any
  Core change.

## Ledger

| ID | App/workload | Symptom | APP normalized? | Core ratio (measured) | Scaling signature | Classification | Placement | Evidence | Status |
| -- | ------------ | ------- | --------------- | --------------------- | ----------------- | -------------- | --------- | -------- | ------ |
| PF-001 | sluice-grep, sparse patterns | V1 per-line `std::search` matcher dominated wall time | yes (V2 chunk scan; dense-anchor emit cost remains, documented) | n/a (APP layer) | bytes-class (`T≈α·bytes`, α improved 2.5–3.1×) | APP | APP-local (done) | `docs/results/performance-attribution/round1-grep-*-ladder.json`; differential oracle test | APP_FIXED (sparse/binary); dense-anchor APP follow-up open |
| PF-002 | sluice-grep, all workloads | aggregate Core increment L4−L3 ≈ 45–60 ms/GiB on sparse/binary rows @ 1 MiB chunks (78–132 on dense-anchor/short rows; per-workload values in the artifact `derived`) | yes | Core share/overhead ratio per workload in ladder `derived` | NOT yet characterized (needs buffer-size/request-count sweep) | INVESTIGATE (internal composition unproven — hypothesis: per-request fixed cost) | TBD (pending decomposition) | `docs/results/performance-attribution/round1-grep-v2-ladder.json` | INVESTIGATE |
| PF-003 | sluice-copy | async control plane (mutex+cond+clock) ≈ 30% of wall time on the round-1 host | yes (copy meets `cp`-parity bar; V3) | ~30% control-plane share (sampling) | NOT yet characterized | INVESTIGATE (Common-Tax candidate across copy+grep only after decomposition) | TBD | `docs/applications/file-tools-findings.md` (perf profile, re-verified) | INVESTIGATE |
| PF-004 | sluice-grep, dense-anchor patterns (`the`, `e`) | matched-line emit path (`std::string` per line, API-bound) keeps dense rows ≈1.05× vs V1; GNU grep/rg still 2–4× ahead | partially (documented algorithm-class gap) | n/a (APP layer) | output-volume-class | APP | APP-local (future campaign; kwset/SIMD class) | round-1 CLI artifacts; methodology §4 known-gap rule | DEFERRED (future APP campaign) |

## Detail

### PF-001 — grep sparse matcher (APP_FIXED for sparse/binary)

The V1 gap was APP: per-line `std::search` re-derived search state per line.
The V2 chunk-level scan (anchor memchr + memcmp verify, incremental line
cursor, borrow-free SWAR newline count) improved sparse/binary rows
2.4–2.9× with byte-identical semantics (differential oracle against the
frozen V1 reference). Dense/common-anchor rows remain emit-bound (PF-004).
No Core change was made or justified by this finding.

### PF-002 — grep aggregate Core increment (INVESTIGATE)

The ladder measures an aggregate Core increment (L4 − L3) under the
round-1 workload/backend/buffer configuration. It proves a Core-owned cost
exists after APP normalization; it does NOT decompose the increment among
runtime lifecycle, admission, submit, handoff, syscall interaction,
wait/wake, reap, or Fiber resume — that is the Core Cost Decomposition
experiment (roadmap Milestone 7). "Per-request handoff tax" wording is
prohibited until that experiment runs. Candidate follow-up (from round-1
diagnosis, NOT implemented): deduped clock queries for undeadlined waits —
it touches Scheduler wait semantics and belongs behind the
architecture/formal gate.

### PF-003 — copy control-plane cost (INVESTIGATE)

Sampling shows the async coordination share (mutex ~14%, cond ~13%,
clock ~6%) on sluice-copy. Sampling may attribute Core-mediated cost into
libc/kernel symbols, so this share is a coordination-class signal, not a
decomposed Core measurement. Needs: same-session ladder-style increments
for copy, scaling signature vs request count, and cross-workload
(Common-Tax) evidence before any Core candidate.

### PF-004 — grep dense-anchor emit path (DEFERRED)

Dense rows pay one `std::string` per matched line through the public sink
API. Improving this is an APP/algorithm-class campaign (kwset-class skip
loop, SIMD candidate filters) explicitly out of round-1 scope; it must not
be presented as runtime-overhead evidence.
