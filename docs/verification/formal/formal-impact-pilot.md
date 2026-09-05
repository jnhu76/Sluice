# FTLR-0 / SCIP-PILOT — C++ ↔ TLA+ formal impact recovery

> **Status: method-selection experiment (issue #299), awaiting adversarial
> human review.** This is NOT a production gate: it is not wired into
> `scripts/gates/pre-push.sh`, CI, or any workflow. It does not modify any
> formal claim, TLA+ model, bridge test, or production C++ semantic. The
> machine-readable experiment record is
> [docs/results/formal/ftlr0-scip-pilot.json](../../results/formal/ftlr0-scip-pilot.json).

## 1. The question

For #298 (FDG-0), the load-bearing question is: when a C++ correctness
authority changes, which formal claims might now be stale? This pilot tests
one bounded mechanism for answering it:

```text
git diff
  → changed C++ symbol (SCIP index, scip-clang)
  → bounded structural traversal (callers/callees)
  → registered formal anchor (spec/formal/anchors.json)
  → formal claim → TLA+ suites / trace / bridge evidence
```

with the permanent failure rule:

```text
unknown → visibly UNKNOWN / fail closed
never:  unknown → silently NO_FORMAL_IMPACT
```

Impact findings always carry `semantic disposition: UNDETERMINED`. The
resolver never claims a TLA+ update is (or is not) required; that verdict
belongs to bounded semantic review (human; the pilot's LLM experiment is
advisory only).

## 2. Components

| Component | Path | Role |
| --- | --- | --- |
| Anchor registry | `spec/formal/anchors.json` | the SOLE cross-domain authority (5 claims: F01/F03/F04/F06/F08, S1A ids) |
| SCIP index | `build/formal-impact/index.scip` (gitignored) | compiler-derived C++ symbol/reference structure (scip-clang 0.4.0, pinned by `scripts/formal/scip-clang.lock.json`) |
| Symbol graph | `build/formal-impact/graph.json` (gitignored) | deterministic derived view: symbols, reference edges, per-file definition positions |
| Resolver CLI | `scripts/formal/formal_impact.py` | `index` / `check` / `impact` / `explain` / `adjudicate` |
| Self-tests | `scripts/tests/test_formal_impact.py` | S1–S10 fail-closed behavior (22 cases, stdlib-only) |
| Evaluation driver | `scripts/formal/ftlr0_eval.py` | adversarial specimens T1–T10, depth matrix, baselines |

Design constraint honored: **only cross-domain edges are maintained by
hand** (anchor ↔ claim). All C++-internal relations are recovered
mechanically from the SCIP index; nothing about callers/callees is
recorded in the registry.

## 3. Usage

```bash
# one-time (or after C++ changes): toolchain + index
bash scripts/formal/bootstrap-scip-clang.sh
xmake project -k compile_commands        # after `xmake f` configuration
python3 scripts/formal/formal_impact.py index

# registry + anchor-resolution validation (S1–S3)
python3 scripts/formal/formal_impact.py check

# the experiment query
python3 scripts/formal/formal_impact.py impact --range master..HEAD
python3 scripts/formal/formal_impact.py impact --diff-file <patch> --json
python3 scripts/formal/formal_impact.py explain F08
```

Rebuild artifacts: the index and graph are gitignored and rebuilt by
`index` in a clean checkout (~20 s for the 94-TU production surface).
The index should be rebuilt whenever HEAD moves; a graph whose
`head_sha` differs from the diff head produces results explicitly flagged
as stale (hits kept, marked unverified).

## 4. Classification (fail-closed)

| State | Meaning |
| --- | --- |
| `DIRECT_FORMAL_IMPACT` | a changed symbol is itself a registered anchor |
| `STRUCTURAL_FORMAL_IMPACT` | a direction-monotonic bounded path (callers or callees, ≤ depth 2) reaches an anchor |
| `COARSE_FORMAL_IMPACT` | no symbol-level information, but the changed file hits a manifest `implementation_bindings` entry or anchor file (the pre-existing file-level layer, refined — never deleted) |
| `UNKNOWN_FORMAL_IMPACT` | risk condition without a safe relation: missing/stale graph, unattributed C++ change, unresolved registered anchor touched, frontier cap hit |
| `NO_FORMAL_IMPACT` | only when: no anchor hit, no path hit, no binding hit, and no risk condition |

Two engine-level rules were established by the adversarial specimens, not
assumed:

1. **Direction-monotonic paths.** Mixing caller and callee directions in
   one path lets a shared low-level utility (e.g. `retry_on_eintr`) connect
   two unrelated call trees (T4 false-positive pump). Paths are pure callee
   chains or pure caller chains.
2. **Namespace hubs are excluded.** Namespace definition nodes are
   referenced by every symbol spelling the qualifier; traversing them
   paints whole subsystems (T6 false-positive pump). They are excluded from
   changed-symbol attribution and traversal expansion.

## 5. Results (summary — full data in the results JSON)

- **Recall (adversarial T1–T10): 10/10** at depth 2. Explicit anchors only
  (depth 0) surfaces 6/10 claim sets and misses every helper / bypass /
  thunk case (T2/T3/T9/T10). File-level `implementation_bindings` alone has
  full recall on this small corpus but 19 false-positive claim flags versus
  3 for the symbol-level engine.
- **Depth experiment:** 0 → 6, 1 → 8, 2 → 10, 3 → 10 (recall hits /10);
  default depth 2 is the smallest full-recall depth; depth 3 adds nothing.
- **Fail-closed verified:** anchor rename ⇒ `check` exits 1 with
  `UNRESOLVED_ANCHOR` and the impact query flags the touched files
  (T6); cross-file move keeps SCIP symbol identity so the claim survives
  with a def-site-drift warning (T7); a rogue writer of the anchored state
  `Scheduler::wake_epoch_` is caught structurally without any annotation
  (T9); missing/failed index or malformed artifacts degrade to UNKNOWN or
  hard failure, never NO (S9/S10).
- **T9 design finding:** anchoring one data member (`wake_epoch_`) as a
  state authority extends coverage to *bypass* paths that no function-level
  annotation scheme would catch. The residual is stated in §7.
- **LLM adjudication experiment (advisory):** reduced (SCIP-candidate)
  context averaged 32% of the full-corpus prompt (5 specimens) with
  identical verdicts; the full-corpus mode forced the adjudicator to read
  and dismiss all 5 claims even for an unrelated change. The adjudicator
  was the implementing agent itself (disclosed in the results JSON); all
  specimens are comment-only by design, so this measures context reduction
  and claim targeting, not semantic discrimination.
- **Historical validation:** the R-F1 witness commit (8d4b72a0, PR #297)
  resolves to `DIRECT_FORMAL_IMPACT: F08` via
  `PhaseTag::worker_startup_before_publication ← Scheduler::run_impl` — a
  real formal-relevant commit recovered with the correct claim.

### Verdict

`SCIP_GRAPH_EARNED` — with honest scale caveats. The structural graph
earns its place on the three safety-relevant margins: helper/indirect
recall (10/10 vs 6/10 for annotation-only), false-positive pressure versus
file-level (19 → 3 on this corpus), and mechanically verified fail-closed
behavior on rename/move/missing-index. The caveats: at the current corpus
size (5 claims), file-level bindings already achieve full recall, so the
SCIP margin is precision and review targeting, not recall; and the
toolchain is a real cost (pinned 149 MB binary, ~20 s index, a
multi-config wrinkle below). Whether that trade holds at #298's larger
registry is exactly what the human review of this report should decide.

## 6. Anchor placement: source annotation vs external registry

The pilot compared the two placements (#299 §16) and chose **B — external
registry (`spec/formal/anchors.json`) as the sole authority**:

| Dimension | A. source comments | B. registry (chosen) |
| --- | --- | --- |
| discoverability while reading code | better | worse (offset by `explain`) |
| refactor survivability | comment survives renames visually, but the machine anchor breaks either way | equal (identity-based resolution + drift warning) |
| noise in production source | a machine-consumed key in every authority function; conflicts with the repo's comment discipline (AGENTS.md §8) | none |
| machine validation | needs a parser + a generation/validation step to avoid comment/registry drift | direct JSON schema validation (`check`) |
| duplicate-authority risk | two sources unless comments generate the registry | none — single authority |
| review ergonomics | anchor changes visible in diffs | registry diffs visible in one file |
| AI ergonomics | anchors must be grepped out of sources | one JSON file, schema-checked |
| SCIP symbol resolution | equal (same resolver) | equal |

Deliberately **not** maintained: both schemes at once. The registry is the
only place a C++ ↔ formal edge exists.

## 7. Known limitations (measured, not speculative)

1. **Semantic reachability is not solved.** SCIP recovers name/reference
   structure. A new computation that affects a formalized protocol without
   touching any anchored symbol or anchored state is invisible (T9 is
   caught only because `wake_epoch_` itself is anchored). Anchor
   granularity is a real design decision for #298, not a solved problem.
2. **Function-pointer installation sites.** Calls through the
   `CompletionBinding::publish` pointer link only to the pointer field, not
   the installed target. Editing an existing thunk is still recovered
   (T10: the thunk's own body references the anchor chain); a *brand-new*
   thunk installed into the slot is attributed only after reindexing.
3. **Nearest-preceding-definition attribution.** scip-clang 0.4.0 emits no
   enclosing ranges; reference attribution and hunk attribution both use
   nearest-preceding named definitions. Field-initializer and
   namespace-body references can mis-attribute in rare shapes (observed
   and mitigated for namespace hubs; other shapes remain possible).
4. **Multi-config surface.** The default xmake config compiles
   `uring_backend.cpp` in stub mode, so the F03 uring terminal-producer
   anchor resolves only with `--with-liburing=y` indexing. The registry
   declares this per-anchor (`config_gate`); `check` reports it as a
   visible declared gap (WARN, fail-closed on touch), not a silent miss.
5. **Toolchain pinning.** Results depend on scip-clang 0.4.0's output shape
   (verified empirically: `cxx` symbol scheme, no `SymbolInformation.kind`,
   no enclosing ranges). A version bump requires re-validating
   `scripts/formal/scip_index.py` against the new output.
6. **LLM experiment is weak evidence.** Single adjudicator (the
   implementing agent), comment-only specimens, single run. It measures
   context reduction and targeting only.

## 8. Scope boundaries (unchanged by this pilot)

- No production C++ semantic changes; no TLA+ model changes; no claim
  upgrades; no bridge-test rewrites.
- No pre-push or CI enforcement — the resolver is a manual command.
- `spec/tla/manifest.json` is untouched; `implementation_bindings` remain
  the coarse file-level layer that COARSE classification builds on. The
  registry refines it; it does not replace it.
- This registry is a pilot artifact and a precursor schema for the #298
  FDG-0 correspondence registry. Promotion into #298 requires the
  adversarial review this report asks for.
