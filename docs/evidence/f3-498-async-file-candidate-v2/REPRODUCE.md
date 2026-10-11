# Fresh executable reproduction receipt (v2)

This is a new candidate capture, not adoption of B-01 and not a rewrite of v1.
Actual fresh execution completed successfully:

```sh
python3 docs/evidence/f3-498-async-file-candidate-v1/fresh_reproduce.py \
    --repo /home/jnhu/Source/Sluice-f3-498-pr-b \
    --output-dir /home/jnhu/f3-498-pr-b-fresh-v2 \
    --before 432dd1e9 --after 9f8c6b3d
```

Output ends FRESH_REPRODUCTION_PASS. To reproduce, select an absent output
directory and use the committed orchestrator with the chosen explicit refs.
No original capture workspace is required: it installs capture/inventory scripts,
builds/installs isolated exact before/after n/y snapshots, runs clean consumers,
generates negative-link source controls, records real statuses and writes exact
tracked-input provenance before finalization. Raw archive includes the generated
old-symbol source inputs and complete link diagnostics. Exact source SHAs are in
provenance.json; source inventory comparison covers both profiles and stages.

Both profiles: headers 79→78, library objects 50→49, async members 37→36;
only async/file.hpp and file.cpp.o removed. Installed W01/W02/W03-threadpool and
host construction/link/run pass; real W03 uring y passes, n explicitly returns
unavailable exit 2. Header/name syntax and four old-symbol link controls succeed
before and fail after; removed symbols have undefined-reference diagnostics.
Archive-member hashes, complete nm, resolved sources/objects, compiler closures,
link maps, source generator, command/status records and raw hashes are captured.

The first version was dated master a9165f0a→94507a57. After rebasing the unpushed
candidate onto reviewed test migration A432dd1e9, this fresh capture measures
A432dd1e9→B9f8c6b3d directly. Pending supplemental scope/smoke test fixes will
require their affected target reruns; library and package-consumer inputs are
unchanged and will receive explicit final input-equivalence mapping.

EXTERNAL_RISK_PENDING; F5_PENDING; DRAFT_NONADOPTED; OPTIONAL_W04_SUPPORTED=NO;
V1_RELEASE_READY=NO. This package receipt does not claim a full behavior matrix.
