# F1 provenance fixture writer retirement

CI failed after the drift test assertions, in strict TemporaryDirectory
cleanup: .git/objects was nonempty at rmdir. This is a fixture lifetime
failure; no C++ production regression was observed.

On the server Git 2.56.0, TRACE2 at unmodified HEAD 15778728 showed 39
git maintenance run --auto --quiet --detach launches in one 10-test run.
That run happened to pass cleanup; the CI failure itself is retained at
/home/jnhu/f3-step4-integration-E6t2gd/f1-failed.log. The detached writer
mechanism is directly observed; the exact CI failing schedule was not replayed.

The helper options maintenance.auto=false and gc.auto=0 eliminate the source
repository commit/fetch maintenance, leaving two detached receive-pack launches
on the bare origin fixtures. Command-line receive.autogc does not propagate
through this local receive transport; each bare fixture local receive.autogc
is therefore explicitly false before push. The final trace records zero
maintenance launches, and all 10 semantic provenance tests pass. Strict cleanup
and every existing assertion are unchanged. No retries or ignore_errors added.
Verifier calls outside the helper are read-only cat-file, merge-base and diff.

Run on the server from the repository:

    GIT_TRACE2_EVENT=/absolute/new/trace.json python3 scripts/test_verify_f1_frozen_provenance.py

Causal verification counts child_start argv containing maintenance: before=39,
helper-only=2, final=0. Trace excerpts retain receive-pack hierarchy and detach
records; full traces remain /home/jnhu/f3-f1-provenance-fix-20261011.
The earlier intermediate helper-only trace is a diagnostic experiment,
not passing evidence for the final no-background-writer property.
No C++ inputs changed; the combined matrix at f717e170 remains applicable.
