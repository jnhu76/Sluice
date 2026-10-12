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
A432dd1e9→B9f8c6b3d directly. Supplemental scope/smoke test fixes are incorporated in corrected candidate
41fc1b45; their affected reruns and static-TSan scope limitation are recorded in
the final matrix receipt. Library/build/header/package-consumer inputs remain
byte-identical, as final-input-equivalence.json records. The original v2 exact
SHAs stay captured provenance rather than being relabeled as a later build.

EXTERNAL_RISK_PENDING; F5_PENDING; DRAFT_NONADOPTED; OPTIONAL_W04_SUPPORTED=NO;
V1_RELEASE_READY=NO. This package receipt does not claim a full behavior matrix.

Consumer-only correction rerun completed FRESH_REPRODUCTION_PASS using the same
verified library prefixes, with --reuse-built-prefixes appended to the command
above. W01/W02/W03 sources now come from each exact archived stage/profile tree.
Old-symbol link probes and real-uring runs use the same explicit clean environment
as consumer probes: PATH=/usr/bin:/bin, HOME=/home/jnhu, LANG=C.UTF-8, with no
CPATH/CPLUS_INCLUDE_PATH/LIBRARY_PATH/LD_LIBRARY_PATH inherited. No unchanged
library build was repeated. The reuse mode independently checks source/build
archive equivalence and regenerates source controls, command statuses and hashes.
