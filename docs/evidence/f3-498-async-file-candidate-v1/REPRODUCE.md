# F3 #498 async File candidate package evidence

ID: F3_498_ASYNC_FILE_CANDIDATE_V1; DRAFT_NONADOPTED.
Before a9165f0a997f46d65411967100f87a1cb2627b2b;
after 94507a57c71704513497ecee3cc01d2bed3b1f24.
Captured before/after Debug n/y at /home/jnhu/f3-498-pr-b-prep.
Each has isolated project/config/build/install/clean-consumer paths.

The after capture was first built from the before archive with exactly the
paired wrapper files removed. provenance.json proves every tracked src/include/
xmake input byte-identical to after commit, in both isolated trees; those two
paths are absent. No docs-only evidence commit invalidates these build inputs.
The reproduction run.sh now archives exact before/after committed SHAs directly.

Both profiles: installed headers 79 -> 78, core+async objects 50 -> 49,
async archive members 37 -> 36. Removed header is only sluice/async/file.hpp;
removed archive member is only file.cpp.o. Canonical Request/context and optional
host dependency closures retain 10/18/23 project headers; task_result retains
35 (legacy facade is intentionally still retained in this narrow cut).

All installed-only W01, W02, W03 threadpool consumers compile/link/run. Real
W03 uring passes in before/after y; n explicitly returns unavailable (exit 2).
Host construction/link/run passes n/y. These are package workload checks, not
full affected regressions or supported optional W04 evidence.
Header and four old-name syntax probes pass baseline and fail candidate.
Four independent old-signature forward-declaration/address link probes link
baseline and fail candidate with undefined references. They do not depend on
including the removed header to manufacture link failure. Exact argv/status
are in negative-link-results.json; raw diagnostics are in the raw archive.

Fresh reproduction uses the committed executable orchestrator (output directory
must be absent):

```sh
python3 docs/evidence/f3-498-async-file-candidate-v1/fresh_reproduce.py \
    --repo /path/to/Sluice --output-dir /fresh/path/to/capture \
    --before 432dd1e9 --after 9f8c6b3d
```

It extracts and places capture/inventory scripts, archives the exact supplied
commits into isolated profile roots, builds/installs libraries, captures actual
source/object/archive/header/host evidence and runs installed consumers. It
generates all four old-signature link source controls, captures their raw sources,
commands and actual statuses, checks undefined-reference diagnostics after
removal, runs W03 real uring y and explicit unavailable n, independently checks
EVERY tracked production/build input against each commit, and writes provenance
and results before finalization. No original uncommitted capture workspace is
required. Original dated v1 evidence remains immutable; fresh results reside in
the chosen output directory/results.

Raw captures include complete defined/undefined archive symbols, resolved
sources and objects, installed inventory, consumer linker maps and symbols,
compiler dependencies, configuration/build/install logs and command records.
raw-sha256.json hashes uncompressed capture paths in the compressed archive.
Library archives are bound in before/after manifests by sha256, with individual
archive-member sha256. Historical F1/B01 files have not been edited.

Pending: stack reviewed PR A, run affected four-profile regressions/sanitizers,
independent review, per-surface external-risk owner decision and F5 adoption.
No versioned package adoption, external-risk approval, W04 support or release
readiness is established by this candidate evidence.
