# Sluice Agent Guide

This file gives repository working rules. Technical target authority is the
[v1 Architecture and Contract Reference](docs/explicit-io-v1-final-decision.md).

## Authority and reading order

1. Read the root specification's relevant requirement IDs and glossary.
2. Read the [v1 conformance ledger](docs/roadmap/v1-conformance.md) for the affected slice.
3. Consult the [docs navigation page](docs/README.md) for what each docs record is
   (active vs archived) before treating any of them as input; execution slices are
   GitHub #390 → #391–#403.
4. Inspect `include/`, `src/`, `apps/`, `xmake.lua` and `xmake/` to establish current behavior.
5. Inspect the tests/models and exact configurations supporting the affected claim.

`docs/archive/**` holds superseded authorities, dated snapshots and research
campaign reports; `formal/` at the repository root holds Lean/TLA+ sources whose
v1 disposition is owned by issue #391. Archive content and formal theorems carry
no authority over the target.

The root specifies what v1 must do. C++ establishes what a particular commit
currently does. A mismatch is a conformance gap, not permission to rewrite the
contract from the code. ADRs derive local decisions from root requirement IDs;
they cannot independently override product scope or public behavior.

Mission/ADR-0001/ADR-0002, their old conformance roadmap and historical issues
are rationale or dated evidence under their supersession notices. Their old
FROZEN/CLOSED labels do not establish v1 conformance. Use Git history or archived
assets only when a current task needs their evidence, not as automatic authority.

## Engineering order

1. Identify the workload, requirement IDs and current-to-target gap.
2. If public behavior or responsibility must change beyond the root, amend the
   root before or together with implementation; record compatibility and evidence.
3. Add the semantic oracle, deterministic regression or protocol model needed by
   this slice before or together with changing the code.
4. Implement one reviewable slice while keeping retained consumers usable.
5. Verify the affected paths and update the conformance ledger with exact evidence.
6. Retire replaced mechanisms only after consumer/migration obligations are met.
7. Optimize measured hotspots while preserving the same contracts.

Follow MIG-01's dependency order and phase gates. Do not postpone lifetime,
publication or teardown verification until after a structural rewrite. Tests
that merely repeat current implementation behavior are not a normative oracle.

## Architecture discipline

- Keep direct execution usable without RequestCore or Scheduler/Fiber.
- Keep File semantics shared across invocation forms and backends.
- Keep request, observer, progress and host responsibilities distinct.
- Do not add hidden blocking fallback or abandon accepted borrowed work.
- Do not preserve a subsystem merely because it existed or was formalized.
- Do not delete a required obligation merely because it has no current consumer.
- Prefer the smallest mechanism that satisfies the accepted workload and root.
- No speculative public framework for hypothetical future users.

## Evidence and documentation

Every architecture-changing PR cites affected root IDs, current and target
behavior, implementation owner, workload, validation and remaining limitations.
Use the ledger statuses defined by GOV-02. An old model or finite test result is
not proof of a new production protocol. Formal actions must map to actual C++
transitions, and safety/liveness assumptions must be separate.

Target requirements live in the root. ADRs hold derived choices and rationale.
Implementation snapshots identify their baseline. README files summarize and
link; they must not create another contract. Historical artifacts retain clear
supersession labels rather than silently regaining authority.

## C++ comments

Comments describe the current implementation. Keep concise explanations of
non-obvious why/what/how. Historical rationale, issue/PR chronology and research
campaign narrative belong in documentation. Put requirement-to-code mappings in
the ledger/ADR rather than repeating the specification in production comments.

## Performance and repository hygiene

Use `profile -> hotspot -> local hypothesis -> isolated change -> benchmark`.
No measured hotspot means no speculative optimization task.

Keep `xmake.lua` and `xmake/` aligned with actual supported targets. New tests,
models, scripts or CI must serve a current requirement; historical tooling has
no automatic inheritance right. Keep commits narrow and reviewable. Do not merge
unless explicitly instructed.
