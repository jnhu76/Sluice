# B-01 v4 facade package candidate — 2026-10-12

ID: F3_PART_C_B01_FACADE_CANDIDATE_V4_20261012.
DRAFT_NONADOPTED; [confirmed owner decision](../../review/f3-part-c-owner-decision-20261012.md)
accepts all five breakages and authorizes this scoped cut subject to final gates.
Formal package adoption and merge authorization remain outstanding.
Original F1 and adopted B-01 v3 assets remain untouched.

Executed on jnhu@192.168.31.75 from /home/jnhu/f3-part-c/c2-facade-20261012:

    python3 docs/evidence/f3-part-c-b01-v4-candidate-20261012/reproduce.py --repo "$PWD" --before cb977a12b951098d65942f4b4020189c7dad1a5e --after 1d0c368f094bb1a6a2e3a9b3b8058d3ae14d43f9 --output /home/jnhu/f3-part-c/package-v4-candidate-20261012-r3
    python3 docs/evidence/f3-part-c-b01-v4-candidate-20261012/completion_probe.py /home/jnhu/f3-part-c/package-v4-candidate-20261012-r3

Select a new absent output path for a fresh replay. Every before/after n/y
profile has its own exact Git archive source, .xmake, build and install prefix.
Xmake commands execute inside that source root; compiler caching is disabled.
Toolchain capture records actual executable paths/kernel/versions.
commands.json records argv, cwd, return code and expected outcome.
Two initial attempts failed object-existence checks because xmake ran from
the evidence directory; their logs remain. Corrected r3 keeps those checks.

| Profile | Master before | Candidate after |
| --- | --- | --- |
| liburing=n | 49 objects / 36 async members / 78 headers | 46 / 33 / 73 |
| real liburing=y | 49 / 36 / 78 | 46 / 33 / 73 |

Removed source/object/member set: application_runtime.cpp, await_op_helpers.cpp,
group.cpp (each .cpp.o absent). Removed installed set: async/application_runtime.hpp,
async/await_op_helpers.hpp, async/task_result.hpp, async/group.hpp,
async/evented_wait_policy.hpp. Resolved objects physically exist; ar members and
nm definitions/undefined references are captured. No removed facade/helper/Group
definition or unresolved reference remains. Header probes fail for missing files;
original runtime/helper and independently threaded Group links change from PASS
to undefined references. Inline policy/templates are header removals, with no
false missing-out-of-line-symbol claim.

Request/RequestScope/context, Scheduler, host, Completion, Fiber/fiber_ctx,
Future/WaitPolicy compiler dependency sets are identical before/after.
W-01 core-only, W-02 scope pipeline, W-03 ThreadPool, eight contract consumers,
retained Scheduler/Fiber/host and default Future/WaitPolicy link/run from installed
prefixes. Actual Completion submit/result/reset separately links/runs on all four
prefixes. Real W-03 uring/W-04 candidate run in y; n is UNAVAILABLE/exit 2.
No supported W-04 claim follows.

summary.json binds source archive hashes, installed files and server binary
package archives by SHA256. raw-package-evidence.tar.gz contains inventories,
symbols, dependencies, linker maps, all probe logs and toolchains.
source-provenance.json records code/build tree identities. Later dependency/
documentation changes leave these production/test/build inputs unchanged.
The Completion supplement executes against unchanged captured prefixes.

Human per-surface acceptance, final independent exact-HEAD review, dependency
closure, cumulative CI and explicit merge authorization are adoption gates.
Master remains 49/36/78. F3_FULL_RETIREMENT=NO; OPTIONAL_W04_SUPPORTED=NO;
V1_RELEASE_READY=NO. A partial approval requires its own scoped recapture.
