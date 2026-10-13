# F2 blocker: W-03 external-loop stall under two-core starvation

Status: **OPEN**. The F2 head stalls `threadpool_external_loop_test`'s
`external_poll_loop_threadpool_w03` scenario under CPU starvation while the
baseline does not. This is recorded as a blocker for merge, not as a known
limitation: the asymmetry is measured, the failing check is identified, and the
mechanism is not yet established.

## Measurement

Both binaries are debug + real-liburing builds of the same tree pair
(baseline `0ed16588`, F2 head), run under `taskset -c 0,1` (two CPUs, the shape
of the CI runner) with ten concurrent copies of the suite, four rounds:

| Tree | Runs | Failures | Failure mode |
|---|---|---|---|
| baseline `0ed16588` | 40 | 0 | — |
| F2 head | 40 | 10 | `FAIL: external_poll_loop_threadpool_w03` |

Passing runs have the same duration on both trees (baseline ≈1.34 s, F2 ≈1.38 s
median), so the F2 tree is not uniformly slower: an occasional run stops making
progress in that scenario and runs out its wait budget.

The failing check, located by instrumenting every `return false;` in the suite
and mapping the reported line back to the source, is the host's wake wait inside
the late-completion loop:

```cpp
    for (;;) {
        if (r2.ready() && r3.ready() && w.ready() && late.ready())
            break;
        if (!host.wait_readable(std::chrono::milliseconds{5000}))   // <- the failing check
            return false;
        const std::size_t delivered = host.acknowledge_and_drive();
        ...
    }
```

It fails with a 5 s budget and also with a 60 s budget (measured with a rebuilt
binary, verified fresh by mtime), and the loop is the last phase of the
scenario, so four reads plus one late zero-op do not settle within the budget
once the stall interleaving occurs.

## Why this is recorded rather than fixed here

- The scenario's shape is byte-identical to the baseline's; the difference is in
  the code it exercises, so this is an F2-side stall in the wait/notification
  path taken with the `Request`-based helpers (`SharedRequest` for the late
  submission) under contention.
- A fix needs a deterministic reproduction first: the trigger is an interleaving
  that needs both a starvation schedule and the late submission, so it is not yet
  reproducible on an idle machine (0 failures in 72 idle-machine runs). The
  existing test seams (`wait_submit_entry_pause_`, `wait_pre_accept_commit_pause_`,
  the delivery/epilogue pauses) do not cover this loop's wake hand-off.
- Weakening the check (a longer budget alone) does not remove the stall: the
  request still does not settle until the budget expires, and the scenario exists
  precisely to catch a host that stops making progress.

## Consequences

- The `Linux debug build + tests` CI job on this branch failed once on exactly
  this scenario (run 38373071332, head `98439970`), aborting in the context
  destructor before the diagnostic was visible. The next run on the branch
  (`e9e47ce8`) passed all four jobs, and the run for the captured implementation
  head (`996f434f`) passed as well, so the failure is intermittent — consistent
  with the load-conditional stall measured above, and the reason this is a
  blocker rather than a closed item.
- The locally captured six-profile matrix passed all profiles on an unloaded
  20-core host (`gates.json`), so the stall is load-conditional.
- Merge is blocked until this is either fixed with a deterministic regression or
  shown to be a test-harness artifact by reproducing it on the baseline. The
  second branch is currently excluded by the measurement above.
