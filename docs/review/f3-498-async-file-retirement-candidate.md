# F3 #498 PR B: async File wrapper retirement candidate

Current decision update (2026-10-12): [scoped owner decision](f3-498-owner-decision.md) records ACCEPT_SCOPED_EXTERNAL_BREAKAGE and versioned package contraction authorization subject to final gates. Designated-owner confirmation is effective; final implementation gates remain open. No merge or package adoption is recorded. Earlier pending-decision statements below describe the evidence capture before this submission.

Status: DRAFT_NONADOPTED; EXTERNAL_RISK_PENDING; F5_PENDING.
Original baseline: a9165f0a (PR #499 audit merge). Current unmerged stack base:
PR A final evidence head d38430cf (code3f846afa); tested candidate code41fc1b45
is unchanged by final evidence-only rebase. See final-rebase-equivalence.json. This paired deletion candidate removes
src/async/file.cpp and include/sluice/async/file.hpp only. It creates no new
runtime state, authority, probe, counter or lease. Production authority remains
File shared validation and RequestCore settlement; Completion, Fiber/fiber_ctx,
StackfulIoHost and ApplicationRuntime remain physically retained.

Authority: root v1-r4 SEM-01/03/04/05/06, ERR-01, REQ, LIFE, HOST,
PROG, SHUT, MIG-01/02, GOV-02; F0 policy §1.7; #498. Workload: canonical
positional single reads/writes and data/all synchronization, optional narrow
host closure. #475 exact HOLD/KEEP surfaces do not enumerate async/file.hpp;
this fact does not waive F0 external-risk and F5 package gates.

## Historical publication and proposed deliberate deprecation

The v0.0.1 API reference describes selected interfaces as stable-ish with
deliberate-deprecation expectations; release maturity is experimental. The exact
header async/file.hpp is absent from that tag (git show returns path missing).
This does not resolve external use of its later publication. F1 installed the
header and exported its callable archive implementation. Original F1 manifests
and archive baselines remain immutable historical before evidence.

A future adopted cut would announce removal of this header and the four exact
symbols below as a source/install/link incompatibility. No deprecation deadline,
owner acceptance, external-use count of zero or supported optional W-04 profile
is inferred. The phase owner must select explicit scoped risk acceptance or a
bounded deprecation window for EACH surface before adoption.

## Per-surface owner decision packet

### sluice::async::await_read_at

Historical public exposure: include/sluice/async/file.hpp; F1 installed header;
file.cpp.o defines its out-of-line symbol. Header-history reference: be07443d.
Proposed deliberate deprecation: remove callable wrapper and declaration in the
same candidate cut. Migration: AsyncIoContext::submit_read(ReadOp{file, buffer, length, offset}) -> Request<size_t>; observe/progress/consume, or IoTaskContext::read inside the optional StackfulIoHost. This is semantic migration, not a
drop-in RuntimeTaskContext + caller Completion signature replacement; legacy
multi-worker runtime completed-return syntax has no equivalent v1 product
spelling. Preserve admission errors separately from accepted terminal failures,
File validation/precedence, effects and borrowed lifetime through real terminal.
UNKNOWN_EXTERNAL_USE=UNKNOWN; owner #402/#475 F5; owner decision=PENDING.
No bounded window or breakage acceptance has been supplied. Rollback before
adoption: restore both paired files from a9165f0a and discard this candidate
package; after publication rollback requires an explicit versioned owner cut.

### sluice::async::await_write_at

Historical public exposure: include/sluice/async/file.hpp; F1 installed header;
file.cpp.o defines its out-of-line symbol. Header-history reference: e61aefd9.
Proposed deliberate deprecation: remove callable wrapper and declaration in the
same candidate cut. Migration: AsyncIoContext::submit_write(WriteOp{file, buffer, length, offset}) -> Request<size_t>; observe/progress/consume, or IoTaskContext::write inside the optional StackfulIoHost. This is semantic migration, not a
drop-in RuntimeTaskContext + caller Completion signature replacement; legacy
multi-worker runtime completed-return syntax has no equivalent v1 product
spelling. Preserve admission errors separately from accepted terminal failures,
File validation/precedence, effects and borrowed lifetime through real terminal.
UNKNOWN_EXTERNAL_USE=UNKNOWN; owner #402/#475 F5; owner decision=PENDING.
No bounded window or breakage acceptance has been supplied. Rollback before
adoption: restore both paired files from a9165f0a and discard this candidate
package; after publication rollback requires an explicit versioned owner cut.

### sluice::async::await_sync_data

Historical public exposure: include/sluice/async/file.hpp; F1 installed header;
file.cpp.o defines its out-of-line symbol. Header-history reference: 48f3a248.
Proposed deliberate deprecation: remove callable wrapper and declaration in the
same candidate cut. Migration: AsyncIoContext::submit_sync_data(SyncDataOp{file}) -> Request<void>; observe/progress/consume. This is semantic migration, not a
drop-in RuntimeTaskContext + caller Completion signature replacement; legacy
multi-worker runtime completed-return syntax has no equivalent v1 product
spelling. Preserve admission errors separately from accepted terminal failures,
File validation/precedence, effects and borrowed lifetime through real terminal.
UNKNOWN_EXTERNAL_USE=UNKNOWN; owner #402/#475 F5; owner decision=PENDING.
No bounded window or breakage acceptance has been supplied. Rollback before
adoption: restore both paired files from a9165f0a and discard this candidate
package; after publication rollback requires an explicit versioned owner cut.

### sluice::async::await_sync_all

Historical public exposure: include/sluice/async/file.hpp; F1 installed header;
file.cpp.o defines its out-of-line symbol. Header-history reference: 5c87e466.
Proposed deliberate deprecation: remove callable wrapper and declaration in the
same candidate cut. Migration: AsyncIoContext::submit_sync_all(SyncAllOp{file}) -> Request<void>; observe/progress/consume. This is semantic migration, not a
drop-in RuntimeTaskContext + caller Completion signature replacement; legacy
multi-worker runtime completed-return syntax has no equivalent v1 product
spelling. Preserve admission errors separately from accepted terminal failures,
File validation/precedence, effects and borrowed lifetime through real terminal.
UNKNOWN_EXTERNAL_USE=UNKNOWN; owner #402/#475 F5; owner decision=PENDING.
No bounded window or breakage acceptance has been supplied. Rollback before
adoption: restore both paired files from a9165f0a and discard this candidate
package; after publication rollback requires an explicit versioned owner cut.

## Consumer/evidence ownership and contraction audit

PR A owns migration of six File semantic test TUs and include-only edges in
async_sync_admission/progress_source_ownership/uring_backend_smoke. PR A and its
scope/smoke supplements are incorporated in this unmerged stack. Corrected full
Debug/Release n/y matrix and ASanUBSan pass; static TSan scope has its separately
recorded allocation-override link limitation; a fresh shared-TSan scope
configuration executes all25 assertions successfully with instrumentation retained. Neither PR A nor this candidate
has been merged/adopted by this execution. See the final receipt for exact SHAs.
Semantic precheck/error/composition/durability observations remain obligations;
old runtime helper call sequences are not the oracle. Formal/docs references
remain historical or need explicit current ownership mapping before adoption.

The only physically contracted authority-independent implementation is the
runtime File wrapper TU and its declaration exposure. No canonical authority
moves. No ApplicationRuntime/Scheduler/Group/WaitPolicy/Completion or host stack
mechanism is eligible for removal from this narrow slice. No hidden blocking
fallback is introduced.

## New package evidence and remaining gates

Candidate identifier: F3_498_ASYNC_FILE_CANDIDATE_V1_A9165F0A_DEBUG_N_Y.
Fresh before/after archived snapshots each have isolated project, .xmake, build,
installed prefix and clean consumer directories. Capture actual resolved library
sources/objects, complete archive ar/nm, installed headers and compiler closures;
compile/link/run retained canonical and narrow host consumers from installed
prefix only; negative probes check removed header and each name; record exact
commands and manifests. [Candidate evidence](../evidence/f3-498-async-file-candidate-v1/REPRODUCE.md) maps
before a9165f0a to production after 94507a57. Both profiles measure 79→78
headers, 50→49 library objects, 37→36 async members; W01/W02/W03
threadpool installed consumers and optional host construction link/run pass;
W03 real uring passes y, while n reports explicit unavailable (exit 2).
Removed-header/name syntax controls and each old-symbol link positive/negative
control distinguish baseline presence from candidate absence. The completed affected matrix and corrected-input mapping are in the
[final receipt](f3-498-pr-b-final-receipt.md); the original dated v1 package
capture remains immutable.
Historical F1 verifier freeze provenance is not applicable to a changed package:
its production-diff-empty gate is intentionally not relabeled PASS or bypassed.
This new candidate evidence does not overwrite/adopt the old B-01 baseline.

Pending: independent final review; each external-risk owner decision;
F5 versioned package adoption/publication; explicit merge authorization.
PHYSICAL_CONTRACTION_ADOPTED=NO; OPTIONAL_W04_SUPPORTED=NO;
V1_RELEASE_READY=NO.
