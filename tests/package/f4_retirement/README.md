# F4 installed surface experiment (manifest v1)

Authority: root v1-r4 PROD-03, W-01–W-03, MIG-01/02/03; F0 §1.7/1.8;
adopted Phase M exclusions; #475 HOLD/KEEP. This tool does not authorize removal.
No F1 frozen manifest is modified. No runtime authority moves in this tool slice;
no additional legacy mechanism is eligible for removal from this tool change.

The matrix uses five pre-created, clean, isolated worktrees in `--workspace`:
`baseline`, `historical-v001`, `candidate-508`, `candidate-510`, and
`integration-508-510`. Integration must contain exactly the reviewed deletion
production/build/test changes; construct it separately and inspect the diff.
Each execution requires an absent `--output` directory. Every installation
prefix and native build directory is new. The original repository is never
checked out, reset, cleaned or used as a build directory.

```sh
python3 scripts/verify_f4_retirement.py --baseline BASE40 --candidate-508 HEAD50840 --candidate-510 HEAD51040 --matrix --workspace /absolute/experiment --output /absolute/new-evidence
python3 scripts/verify_f4_retirement.py --verify-evidence /absolute/evidence
python3 tests/package/f4_retirement/test_verifier.py
python3 tests/package/f4_retirement/run_sanitizers.py /absolute/new-sanitizer-evidence /absolute/integration-worktree
```

Integrity-only mode checks the recorded evidence; it does not repeat execution
and cannot independently authenticate an untrusted producer. Keep the full raw
logs, manifest, probes, source identities, and content-addressed archive.
Fixtures are deterministic rejection tests, never package/runtime evidence.

Each header gets its own presence/include verdict. The detail pool fragment is
also measured through its public parent. Baseline API probes exercise independent
public observations: a submitted result and shutdown rejection, bytes read/written
and closed, stream reachability, copy length, buffer flush and WAL round trip.
Header-only interfaces have compile/link/runtime witnesses but no independently
owned callable ABI; a successful link of their previously compiled inline code is
not evidence of remaining installed support. Negative ABI witnesses relink the
same baseline object with the candidate core library and unchanged arguments.
Strong definitions owned by removed archive objects are compared against both
candidate archives. Weak shared template definitions are not treated as exclusive
legacy callable ownership.

Canonical consumers are copied into scratch, built from installed headers and
libraries, and run. W-03 ThreadPool and real-uring statuses remain separate. Exit 2
means UNAVAILABLE, never PASS. Binary nm records check Scheduler/Fiber closure.
W-04 only witnesses the retained optional candidate and never claims support.
Clang sanitizer consumers and libraries use matching instrumentation/layout.

External use remains UNKNOWN. Proposed acceptance is an engineering recommendation;
only an identified human phase owner can publish a decision bound to exact PR HEAD,
evidence digest and enumerated risk version. All generated records stay PENDING.
