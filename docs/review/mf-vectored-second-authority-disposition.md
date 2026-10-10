# M-F vectored + second-authority disposition (#461, Phase M closeout wave 2)

- **Status**: DISPOSITION_INPUT_COMPLETE — PENDING_OWNER_DECISION on ruling A
  (vectored five questions) and ruling B (D-09(1) second authority), plus the
  independent U-03/U-04 external-use policy which this record does **not**
  decide. Nothing here is adopted by itself.
- **Baseline**: live master `94b5cfcdace6ee15c75c909dae09665eda2d1a26`
  (all anchors re-verified on this tree this wave; #461's evidence base was
  `1c2a9ab8` — deltas called out inline).
- **Authority order applied**: root v1-r3 > ADR-0004 (adopted, not touching
  this family) > #402 current text > F0/F1 adopted artifacts > #458 §3.3 >
  ledger > live implementation. ADR-0002 is HISTORICAL/SUPERSEDED and is used
  only as historical evidence.

## 0. Live-consumer delta since #461 (recorded first, honestly)

M-R tier-1 (PRs #468/#471, after #461 was written) migrated the test-face
consumers: `sluice/file.hpp` is now included by exactly **one** test file
(`tests/semantic_range_test.cpp:2`, two direct constructions `:134/:145`
exercising only `read_at`/`write_at` overflow rejections), down from six at
census time. The M0 census rows themselves are unchanged historical record;
the live state is narrower. This narrows the evidence base; it does not
change the ruling questions, and zero-consumer counts remain **non-decisions**
(root `:129`: "Existing usage, formalization, or zero consumers alone neither
preserves nor deletes a feature").

## 1. B-07 five questions — answered with live evidence

**Q1 — does root require vectored? NO.** PROD-03 (`docs/explicit-io-v1-final-decision.md:127`):
"vectored I/O … not automatically admitted because a platform supports them".
No MIG-02 row mentions vectored (whole-doc grep: only `:127`). No adopted ADR
does (ADR-0002's vectored paragraphs are under the supersession banner).

**Q2 — is it in v1? NO, and the channel is explicit.** root `:129`: a
capability enters v1 "through an accepted workload or a named
correctness/resource obligation, with its cost and evidence". Ledger:56
vectored = DEFERRED_OUT_OF_SCOPE.

**Q3 — is the existing capability intentionally retired? NO decision exists.**
MIG-02 root:933 places the whole legacy Reader/Writer/FileReader/FileWriter/
IoContext family under "Retire from canonical surface after consumer/migration
audit"; vectored lives inside that family. Retirement can only be a
*consequence* of ruling A+B — not the current state (DEFERRED is not a
retirement decision).

**Q4 — should canonical `File`/`blocking::*` gain vectored? No accepted
workload exists today.** The complete live-consumer picture at `94b5cfcd`:

- The **only** chain to native `::writev` is
  `WalWriter::write_record_vec` → `wal::write_record_vec` (3-slice
  header/payload/trailer assembly, `src/wal.cpp:84-106`) →
  `Writer::write_all_vec` (dispatches **virtual** `write_vec`,
  `src/writer.cpp:48-98`) → `FileWriter::write_vec` → `::writev`
  (`src/file.cpp:393-394`). But `WalWriter` has **zero construction sites**
  in src/include/apps/tests — the chain is installed, compiled, and never
  entered.
- Native `::readv` (`src/file.cpp:147-148`) is **unreachable in-tree**: the
  convenience face `Reader::read_vec_all` never dispatches virtual
  `read_vec` (it loops `read_some` directly, `src/reader.cpp:81-103`), and
  `read_vec_all` has **zero callers** anywhere. `FileReader::read_vec`'s
  native override is reachable only by calling it directly — no in-tree
  caller does (NEW-01b asymmetry).
- The factory `BlockingIoContext` — the only production constructor of
  FileReader/FileWriter (`src/io_context.cpp:12/:23`) — has **zero**
  construction sites itself. The whole legacy stream world
  (FileReader/FileWriter/wal/copy/observed/buffer family) has zero live
  in-tree production consumers; its installed headers ship in both frozen
  F1 manifests (`sluice/file.hpp` :93, `sluice/io_context.hpp` :95, plus
  reader/writer/wal/copy/observed/memory_io_context).
- The one live test consumer (`tests/semantic_range_test.cpp`) exercises
  only `read_at`/`write_at` overflow rejection — not vectored.

Observable-capability cost/benefit if canonical were to gain it: writev
batches a 3-slice WAL record into **one** syscall vs three `write_all` calls
(observable in syscall counts under strace, not in bytes); readv today buys
nothing (dead convenience face). Costs of carrying it: iovec assembly +
IOV_MAX chunking (`src/file.cpp:22-53`), per-slice partial-progress
semantics (`writer.cpp:58-64` rebuilds partial slices), slice-lifetime
borrows during the op, `VectorStats` instrumentation surface.

**Q5 — is compat-only isolation available?** Conditionally: policy §1.1(1)
holds (MIG-02 disposition exists for the family), §1.1(3) holds (no root
requirement), §1.1(2) — "presumable external consumers" — is **unknown**
(U-03/U-04). §1.7's five exit conditions, esp. (5) the explicit
external-use decision, are a #402 human-review obligation. So A4 is legal
only *after* U-03/U-04 are decided.

## 2. D-09(1) — FileReader/FileWriter second authority

Confirmed at HEAD: each owns a raw `fd_` with independent
open/close/move/sync/vectored (`src/file.cpp:67-85/:309-328/:587-597`;
members `include/sluice/file.hpp:59-64/:118-122`), zero semantic edge to
canonical `File` (no `file_resource.hpp` use in `file.cpp`; F0 DAG X-06
negative finding stands). Canonical `File` offers
open/close/is_open/native_handle/access (`include/sluice/file_resource.hpp:73-95`);
`blocking::*` has no vectored op. Known quality debt bound to this face: the
A1 GAP — **8** fabrications of `permission_denied` on closed fds
(`src/file.cpp:97/:139/:189/:237/:340/:385/:435/:486`) where the shared
oracle requires `invalid_state` (ledger:149), plus ledger:150 composition
codes.

Named retention obligations search (B2's precondition): none found. The
family's only would-be consumer classes are (i) the WAL write chain — itself
unconsumed and workload-unadmitted (M-A2), (ii) tests — semantic_range only,
migratable, (iii) external users — unknown, gated by U-03/U-04. "It exists in
tests/installed form" is not a retention obligation (policy: existence is not
an obligation).

## 3. Options compared (KEEP / ISOLATE / REIMPLEMENT / REMOVE)

| option | maps to | preconditions | observable consequence | cost |
|---|---|---|---|---|
| REIMPLEMENT (canonical gains vectored) | A1/A2 | root amendment via PROD-03+GOV-04: an accepted workload + cost/evidence; none exists today; read side must not copy NEW-01b asymmetry silently; wal byte/error mapping | canonical face grows; wal could migrate later | root churn + oracles + B-01 re-freeze; **no channel satisfied today** |
| KEEP as-is (undecided drift) | none | — | DEFERRED drifts into de-facto disposition; the #461 forbidden baseline | zero now, unbounded later; **prohibited** |
| ISOLATE (compat-only, sunset) | A4 (+B2 shape) | U-03/U-04 external-use decision FIRST (§1.7(5), human review); sunset conditions named | surface stays installed, labeled, non-canonical | carried maintenance; needs F5 package alignment |
| REMOVE with the L-01 family (explicit capability-exit) | A3 + B1 | explicit root-level retirement decision; U-03/U-04 decision FIRST (before any header deletion); accepted-degradation record for wal's writev single-syscall property | headers 79→fewer, archive members drop, link breaks for any external user — exactly why §1.7(5) precedes deletion | F4 batch + B-01 same-slice re-freeze |

## 4. Recommendation (PENDING_OWNER_DECISION — evidence-ordered, not authority)

**Candidate ruling: A3 + B1** — vectored is **not admitted** to canonical v1
(no accepted workload or named obligation exists); FileReader/FileWriter have
**no identified retention obligation**; the legacy stream family, vectored
included, proceeds to F4 retirement per MIG-02 root:933 **after** the
U-03/U-04 external-use human decision. Sub-choice recorded for the owner:

- A3a: `wal::write_record_vec`/`write_all_vec` retire **with** the family
  (wal itself has zero consumers — no in-tree retype is owed; record the
  writev single-syscall property as accepted-degradation-on-exit).
- A3b: if the owner wants wal to survive, wal retypes to scalar
  `write_all` (three syscalls per record — observable syscall-count change;
  byte stream identical) — but that first requires admitting a wal workload
  (M-A2/D-12), which today has no channel satisfied either.

**Alternative for the owner: A4 + B2** (compat isolation with sunset) if the
external-use assessment (U-03/U-04) returns "likely external consumers"; its
precondition and exit conditions are in the table above.

Both positive forms need owner action; neither is adopted here. The M-F
family gate `VECTORED+SECOND-AUTHORITY_DECIDED` closes when the owner picks
A×B and the U-03/U-04 policy lands; then #458 §8 backfills and F4 gets its
input (X-06/X-07/X-13 + D-09/D-10 name-collision ledger entries, T2-10
oracle re-run, ledger:149/150 rows re-recorded as "retired-with-GAP" or
fixed-if-B2).

## 5. U-03/U-04 — separately accounted, handed to F5 (not decided here)

`sluice/file.hpp` and `sluice/io_context.hpp` (plus the reader/writer/wal/
copy/observed/memory_io_context headers riding the same decision) are F1
frozen installed headers; external use is unknown; in-repo counts are not
evidence. This record registers the obligation and defers to the #475-style
human decision path: keep-installed / bounded-hold / contraction-with-
breakage-acceptance, with B-01 re-freeze on any change. It must not be
absorbed into ruling A/B and must not be resolved by agent default.

## 6. Counterexample registry (what any future arm must respect)

1. NEW-01b read/write dispatch asymmetry (`src/reader.cpp:81-103` vs
   `src/writer.cpp:48-98`) — any canonical reimplementation must either not
   reproduce it or record why it does.
2. A1 GAP 8× `permission_denied` fabrication — retirement records "GAP at
   retirement"; a B2-style retention must fix it (`invalid_state` + oracle
   re-run) rather than keep it.
3. Zero-reachability ≠ zero-external-use (U-03/U-04).
4. wal's writev single-syscall observability — an A3b retype must record the
   syscall-count difference explicitly.
