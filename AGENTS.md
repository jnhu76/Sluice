# Sluice Agent Guide

This file gives repository working rules. Technical target authority is the
[v1 Architecture and Contract Reference](docs/explicit-io-v1-final-decision.md).

## Authority and reading order

1. Read the root specification's relevant requirement IDs and glossary.
2. Read the [v1 conformance ledger](docs/roadmap/v1-conformance.md) for the affected slice.
3. Inspect `include/`, `src/`, `apps/`, `xmake.lua` and `xmake/` to establish current behavior.
4. Inspect the tests/models and exact configurations supporting the affected claim.

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

## Testing discipline

Tests are evidence for a behavior, invariant, legal interleaving or failure mode;
they are not post-hoc mirrors of an implementation.

Before changing behavior or protocol whose intended semantics are already known,
identify the relevant oracle, invariants, failure modes and legal interleavings.
Establish the required deterministic regression, semantic-oracle test or protocol
model before or together with the production change.

Do not finish a mechanism and then invent unit tests whose assertions merely
reproduce its private representation, state layout or call sequence. A regression
test added after finding and fixing a real defect is valid when it independently
reproduces the failure rather than being derived from the repaired implementation.

Prefer the lowest sufficient deterministic test level that directly
distinguishes the relevant contract or failure mode. Exercise the actual
public path when the claim concerns that path or cross-layer composition;
add lower-level controlled evidence when it is needed to expose the mechanism.
End-to-end testing is not the sole evidence mechanism: protocol transitions,
rare interleavings, lifetime boundaries, publication ordering, cancellation,
teardown, fault injection and backend-independent semantics may require
deterministic regressions, controlled concurrency tests, semantic oracles or
formal models.

An isolated test must earn its existence by naming the invariant or failure mode
it distinguishes. Before implementing the mechanism under test, state what can go
wrong and what independent observation demonstrates that it did not.

Formal/model evidence and production tests have different roles. A bounded model
result does not prove an implementation unless modeled actions, assumptions and
relevant C++ transitions are explicitly mapped. A passing production test likewise
does not establish protocol completeness outside the exercised executions.

Where a validation claim is externally observable, produce repeatable evidence
when practical, for example captured program output, a kernel/backend trace,
deterministic execution trace, model-check result with bounds, fault-injection
record or benchmark raw result. Record enough configuration and command information
for another reviewer to reproduce the claim.

During implementation run the smallest relevant deterministic test set needed for
fast feedback. At a slice/phase boundary and before merge authorization, run the
complete configuration matrix required by the affected conformance claim. Do not
repeatedly run an expensive full matrix after changes that cannot affect it, and
do not turn a failing protocol test green through sleeps, retry inflation or
weakened assertions.

## Architecture discipline

- Keep direct execution usable without RequestCore or Scheduler/Fiber.
- Keep File semantics shared across invocation forms and backends.
- Keep request, observer, progress and host responsibilities distinct.
- Do not add hidden blocking fallback or abandon accepted borrowed work.
- Do not preserve a subsystem merely because it existed or was formalized.
- Do not delete a required obligation merely because it has no current consumer.
- Prefer the smallest mechanism that satisfies the accepted workload and root.
- No speculative public framework for hypothetical future users.

## Mechanism justification

For v1 convergence work, apply the Mechanism Justification Gate recorded by the
roadmap (#390); issue #419 is the supporting audit/contraction inventory, not a
semantic authority.

For every new runtime state, flag, counter, token, lease, capability probe or
defensive guard, answer:

1. What fact or invariant does it represent, summarize or check, and which
   authority owns that fact?
2. What requirement, legal interleaving, external failure, physical/lifetime
   obligation, or concrete performance/evidence need justifies it?
3. Can it be derived from an existing authority?
4. If derived, why must it be materialized?
5. Classify it as authority, derived summary, compatibility state, physical
   obligation, or test-only evidence.
6. Which existing mechanism, if any, does it replace? `None` is valid when the
   mechanism is not a replacement.
7. If it replaces a mechanism, what migration and evidence conditions permit
   removal? Otherwise, this is not applicable.

Do not add runtime mechanism merely as defensive insurance for an unreachable
state. This does not waive root-required contract diagnostics. Derived summaries
and justified diagnostic/test instrumentation need not introduce an independent
semantic fact. Complexity is not evidence of redundancy: do not collapse a
mechanism that represents an independent physical/lifetime obligation merely
because another state correlates with it. Derived does not imply delete; cached
summaries may remain useful but must stay non-authoritative semantic sources of
truth.

Every migration slice must include an authority-contraction audit: identify which
legacy states, guards, tokens, leases, counters or capability representations
became redundant after authority moved, and which apparently redundant mechanisms
must remain because they protect a real legal interleaving or physical obligation.
A slice that moves no production authority may record that no existing mechanism
is yet eligible for removal.

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

Default to no explanatory comment. C++ comments are exceptional and exist only
when removing the comment would make a maintainer materially more likely to
introduce a local implementation mistake.

Keep only short notes for non-obvious local hazards, invariants or mechanism
constraints that the code cannot make clear by naming or structure alone. Typical
examples are an unsafe syscall retry, a subtle arithmetic invariant, or a test
seam behavior whose misuse would invalidate the test.

Do not use comments to narrate semantics, restate the root specification, cite
requirement IDs, declare an authority/contract, record issue or PR history,
explain review conclusions, map requirements to code, or preserve research and
platform arguments. Those belong in the root, conformance ledger, ADRs, tests or
other documentation as appropriate.

Apply the same rule to public headers and tests. Prefer descriptive names, types,
control flow and assertions over comments. If a comment merely paraphrases nearby
code or a test name, delete it. If a semantic explanation must remain traceable,
keep the explanation in the authoritative documentation and remove it from C++.
When in doubt, delete rather than compress a semantic narrative into a shorter
pseudo-contract.

## Performance and repository hygiene

Use `profile -> hotspot -> local hypothesis -> isolated change -> benchmark`.
No measured hotspot means no speculative optimization task.

Keep `xmake.lua` and `xmake/` aligned with actual supported targets. New tests,
models, scripts or CI must serve a current requirement; historical tooling has
no automatic inheritance right. Keep commits narrow and reviewable. Do not merge
unless explicitly instructed.
