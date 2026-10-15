# Versioned B-01 package cut v3 — 2026-10-12

New cut ID: F3_498_B01_ASYNC_FILE_CUT_V3_20261012.
This is the scoped two-profile cut authorized by the confirmed phase owner.
Adoption occurs only with the authorized #502 merge after final review and CI;
this record is not a V1 release or optional W04 support claim.

Fresh execution compared merged #501 master f527456b5eaa10a74e872ee1d23b9dd6413f1f1d
with cumulative #502 source ecf7286f1eea8b22739f95b50edfcd94c2dace7b.

Reproduction command (select an absent output directory):
python3 docs/evidence/f3-498-async-file-candidate-v1/fresh_reproduce.py --repo /home/jnhu/Source/Sluice-f3-498-pr-b --output-dir /home/jnhu/f3-498-package-cut-v3 --before f527456b5eaa10a74e872ee1d23b9dd6413f1f1d --after ecf7286f1eea8b22739f95b50edfcd94c2dace7b

Execution ended FRESH_REPRODUCTION_PASS. Exact git archives, per-profile isolated
builds/installs, tracked-source comparisons, clean-room installed positive/negative
probes and complete archive symbol/member evidence were actually regenerated.
Both profiles measure 79 to 78 headers, 50 to 49 library objects and 37 to 36 async
archive members. Only async/file.hpp and file.cpp.o disappear; four exact wrapper
definitions disappear and their independent old-signature link probes change from
successful baseline links to undefined references. Real uring W03 passes in y,
and n explicitly reports unavailable exit 2.

package-artifacts.json binds the versioned two-profile installed-prefix archives
retained on the test server by SHA256 and every installed file's SHA256.
Raw captures and source provenance are committed here; the larger binary package
archives remain at the exact server paths recorded in that manifest. Copying these
fresh results into this new directory and assigning this cut ID does not mutate
the original candidate captures or historical frozen F1/B-01 assets.

The final documentation HEAD and eventual merge differ from the captured source
only through documentation/evidence. Exact final review must verify an empty diff
for production, test, build and installed-consumer inputs against the captured SHA.
Server matrix/sanitizer evidence remains applicable because those inputs are
unchanged; new package builds and probes are the affected-path verification.
Cumulative GitHub CI and exact final HEAD review are recorded in the PR metadata
and remote execution receipt rather than guessed from an embedded future SHA.

Postmerge verification must freshly archive/build/install the actual merged master
and measure both profiles; the candidate numbers alone do not satisfy that gate.
F3_FULL_RETIREMENT=NO; OPTIONAL_W04_SUPPORTED=NO; V1_RELEASE_READY=NO.
