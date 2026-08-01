## Summary

<!-- One-paragraph description of what this PR does and why. -->

## Change classification

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor (no behavior change)
- [ ] Documentation only
- [ ] Build/CI
- [ ] Test only

## Architecture impact

**Does this PR affect an architecture boundary?** (async I/O ownership, operation
lifecycle, Completion publication, Scheduler wake/progress, backend submission,
cancellation, resource capacity, Runtime ownership, public async API)

- [ ] **No** — this PR does not touch an architecture boundary.
- [ ] **Yes** — this PR changes an architecture boundary. The following are
      completed:

### Architecture gate (required only if "Yes" above)

- [ ] Design document or ADR is linked: <!-- link -->
- [ ] Gate 0 classification is stated (capability, layer, F/I/A/M/O/U, ADR).
- [ ] Affected capability and owner are identified.
- [ ] Operation/state ownership is documented with a state machine.
- [ ] Submit failure remains transactional (AC-3).
- [ ] Completion publication authority is unchanged or explicitly approved (AC-5).
- [ ] Wake obligations and sleep domains are documented (AC-6).
- [ ] Resource bounds and queue-full behavior are documented (AC-7).
- [ ] Cancellation layer is identified (AC-9).
- [ ] Zig conformance classification is recorded or updated.
- [ ] Intentional divergences are entered in the divergence registry.
- [ ] Tests prove semantics rather than timing assumptions (AC-11).

## Testing

<!-- Commands run and results. "All tests pass" is not sufficient. -->

```text
xmake f -m debug --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

Additional gates (as applicable per AGENTS.md §6):

- [ ] Release build
- [ ] ASan/UBSan
- [ ] TSan
- [ ] Focused test: `SLUICE_TEST_FILTER=<name> xmake run <target>`

## Checklist

- [ ] Only intended files changed (`git diff --stat` reviewed)
- [ ] No unrelated formatting changes
- [ ] Comments explain invariants, not narration
- [ ] Documentation updated if public contract changed
