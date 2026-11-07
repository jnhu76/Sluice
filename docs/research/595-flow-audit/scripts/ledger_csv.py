#!/usr/bin/env python3
"""Deterministic build + validation of WORK_AND_WAIT_LEDGER.csv.

Repairs review finding R6.1: the v1 CSV was hand-joined and 11 of 32 rows
had shifted columns (dropped empty fields). This tool keeps the rows as
structured data and emits the file via csv.writer, so field alignment
cannot drift; `check` re-reads any file and enforces:

- exactly the 13 frozen columns, every row field-count equal;
- unique ids, kind in {work, wait};
- required fields non-empty (id, world, executor, item, frequency,
  source_location, evidence, evidence_grade, classification);
- evidence_grade tokens from the frozen vocabulary (multiple allowed,
  joined with '+');
- classification tokens from the audit vocabulary, where REQUIRED_* splits
  the old blanket "Required" into semantic responsibility vs this
  implementation (lock/scan/notification choice) being required;
- unmeasured quantities spelled UNMEASURED/UNAVAILABLE, never invented.

Exit 0 on success, nonzero on any violation. Deterministic counterexamples
(a truncated row and an overlong row) are exercised in
test_offline_checks.py.
"""
import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(HERE, "..", "WORK_AND_WAIT_LEDGER.csv")

COLUMNS = ["kind", "id", "world", "executor", "item", "frequency",
           "source_location", "evidence", "evidence_grade", "classification",
           "classification_target", "removal_condition", "correctness_obligation"]

GRADES = {"SOURCE_VERIFIED", "RAW_VERIFIED_BY_FLASH", "RECALCULATED",
          "HISTORICAL_REPORTED", "INFERRED", "CONFOUNDED", "PERTURBED",
          "UNVERIFIED", "UNMEASURED", "UNAVAILABLE"}
CLASSIFICATIONS = {
    "REQUIRED_SEMANTIC",   # the responsibility itself (identity, publication...)
    "REQUIRED_IMPL",       # this lock/scan/notification implementation choice
    "AMORTIZABLE", "ELIMINABLE", "CONDITIONAL", "ACCIDENTAL",
    "BENCH_PROTOCOL", "PHYSICAL", "INVARIANT", "DIAGNOSTIC",
}
REQUIRED_NON_EMPTY = ["kind", "id", "world", "executor", "item", "frequency",
                      "source_location", "evidence", "evidence_grade",
                      "classification"]

ROWS = [
    # --- machine work ---
    ("work", "W01", "PRODUCT", "submitter",
     "ctx initiation precheck (fd/access)",
     "1/op", "M:async_io_context.cpp:249-269",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "public rejection reasons preserved; impl duplicates backend check behind one boundary",
     "public error contract"),
    ("work", "W02", "PRODUCT", "submitter",
     "backend validate_op (precheck_data_op + dst null)",
     "1/op", "M:uring_backend.cpp:430",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "", "backend trust boundary"),
    ("work", "W03", "PRODUCT", "submitter",
     "core reserve+accept (identity/generation/outstanding++)",
     "2 core-mutex/op", "M:request_core.cpp:165-209",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "", "unique identity + acceptance linearization"),
    ("work", "W04", "PRODUCT", "submitter",
     "prepared_ops_ slot write (borrow record)",
     "1/op", "M:uring_backend.cpp:479-486",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "", "caller buffer record until release_execution"),
    ("work", "W05", "PRODUCT", "submitter",
     "delivery-record init under dispatch_mtx_",
     "1 dispatch_mtx_/op", "M:uring_backend.cpp:488-500",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC+REQUIRED_IMPL", "semantic=owed accounting / impl=lock choice",
     "", "owed-event accounting per slot"),
    ("work", "W06", "PRODUCT", "submitter",
     "dispatch_after_accept: push + dispatch_one_locked (SQE prep/claim/ledger)",
     "1/op", "M:uring_transport.cpp:43-102,130-209",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "", "SQE prep/claim/ledger obligations"),
    ("work", "W07", "PRODUCT", "submitter",
     "io_uring_submit enter (normal non-zero read shape)",
     "1/op on verified shape only (zero-op/retry/control paths differ)",
     "M:uring_transport.cpp:87,218-235",
     "R5 contrasts C-A0 -22.09% / R-A0 -21.47% process-CPU/op; attribution of the delta to the enter call itself vs co-varying research-path changes NOT established",
     "RECALCULATED+HISTORICAL_REPORTED", "AMORTIZABLE", "implementation",
     "deferred flush designs must keep PROG-01 eventual dispatch; enter-count effects vs batch-shape effects not separable with existing arms",
     "accepted work reaches kernel (PROG-01 residue)"),
    ("work", "W08", "PRODUCT", "submitter",
     "cookie/router/ledger append per op",
     "1/op", "M:uring_transport.cpp:161-209",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "", "CQE routing + transport reconciliation"),
    ("work", "W09", "PRODUCT", "driver",
     "epoll wake + acknowledge + bounded passes (W03 loop shape is BENCH)",
     "per eventfd wake", "M:bench_w03:197-229 M:async_io_context.cpp:441-447",
     "notifications/passes counters in raw records",
     "RAW_VERIFIED_BY_FLASH", "BENCH_PROTOCOL+REQUIRED_SEMANTIC", "bench=loop shape / library=progress obligation",
     "", "progress owner obligations"),
    ("work", "W10", "PRODUCT", "driver",
     "reap: peek_batch32 + handle_one_cqe per dispatch_mtx_",
     "per CQE", "M:uring_completion.cpp:100,200-278",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC+REQUIRED_IMPL", "semantic=completion service / impl=per-CQE lock granularity",
     "lock granularity measurable only with lock-held instrumentation (not run)",
     "terminal arbitration per request"),
    ("work", "W11", "PRODUCT", "driver",
     "offer_terminal + release_execution",
     "2 core-mutex/request", "M:request_core.cpp:346-409 M:uring_completion.cpp:70-92",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "", "unique terminal + borrow retire before publish"),
    ("work", "W12", "PRODUCT", "driver",
     "publication: begin+complete+observer claim/retire+deliver_event",
     "4 core-mutex + 1 dispatch_mtx_ pop per request",
     "M:uring_backend.cpp:578-614,640-657",
     "structure source-verified; T4-T3 +59ns is PERTURBED lead only",
     "SOURCE_VERIFIED+PERTURBED", "REQUIRED_SEMANTIC", "semantic=publication completeness / impl=observer pair cost amortizable when no observer",
     "skip observer claim/retire when zero registrations (needs authority audit)",
     "publication completeness + observer semantics"),
    ("work", "W13", "PRODUCT", "caller",
     "front drain: ready()+take_result+verify",
     "BENCH per drain", "M:bench_w03:387-418",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "BENCH_PROTOCOL", "bench",
     "not product work; out-of-order observation would add reorder bookkeeping (C6)",
     "seq%depth safety 3 conditions (C6)"),
    ("work", "W14", "BENCH", "driver+submitter",
     "SharedLoop facts copy + cv.notify per pass",
     "W03 per pass", "M:bench_w03:231-243",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "BENCH_PROTOCOL", "bench",
     "", "harness-owned channel"),
    ("work", "W15", "RESEARCH", "submitter",
     "adapter DecisionRecord + ~10 counters per chunk",
     "per chunk", "C:r3_ready_batch.hpp:468-605",
     "R4 counters present in raw records",
     "RAW_VERIFIED_BY_FLASH", "ELIMINABLE", "implementation",
     "research line only; deletion requires counter-freeze decision, never enters product",
     "none (research telemetry)"),
    ("work", "W16", "RESEARCH", "submitter",
     "seam precheck (3rd validation) + dynamic_cast + whole-batch access_mtx_",
     "per presentation", "C:test_access.hpp:55-77",
     "d2serial tax structure consistent; per-step ns INFERRED not measured",
     "SOURCE_VERIFIED+INFERRED", "ACCIDENTAL", "implementation",
     "seam removal declined in phase-a Candidate A (not provably transport-identical for ineligible spans); needs semantic-equivalence evidence first",
     "none"),
    ("work", "W17", "RESEARCH", "submitter",
     "whole-batch dispatch_mtx_ critical section (admit xN + dispatch xN + 1 enter)",
     "per presentation", "C:uring_backend.cpp:504-590",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "CONDITIONAL", "implementation",
     "service-order effect vs driver publish opportunity; shorter sections or deferred flush (designs on hold)",
     "lock order and residue rules unchanged"),
    ("work", "W18", "RESEARCH", "submitter",
     "batch_eligible_read recheck per member",
     "per member", "C:uring_backend.cpp:437-439,537",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic",
     "eligibility authority is the backend predicate; adapter mirrors are the derived copies (C1 already bounded the scan)",
     "eligibility authority = backend"),
    ("work", "W19", "RESEARCH", "submitter",
     "outstanding() two-lock read",
     "per chunk on budgeted arms only",
     "C:r3_ready_batch.hpp:498 M:async_io_context.cpp:837-840",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "CONDITIONAL", "implementation",
     "research signal; product API exists (R3 table) but strategy value unproven",
     ""),
    ("work", "W20", "KERNEL", "io-wq kthreads",
     "opcode execution offloaded by kernel",
     "continuous during inflight",
     "kernel internal; observed as tasks under child /proc/<tgid>/task (1/invocation) plus top-level scan",
     "sampled-classified tick lower bounds only (cpu_scope_recalc.py); whether this CPU is inside child rusage/PROCESS_CPUTIME UNRESOLVED on this kernel; routing inline-vs-worker unobservable",
     "RECALCULATED+UNMEASURED", "PHYSICAL", "n/a",
     "not reducible from user space; io-wq ticks lower bound R/C <= A0 in sampled data",
     "data delivery"),
    ("work", "W21", "PRODUCT", "submitter",
     "zero-op inline publication path (no dispatch, no enter)",
     "len==0 only", "M:uring_backend.cpp:520-521,553-576",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic=zero-len semantics / off the 4k-read path",
     "", "zero-op terminal + owed event + signal"),
    # --- waits ---
    ("wait", "T1", "BENCH", "submitter",
     "app dependency wait (data ready at once)",
     "0 in warm bench", "M:bench_w03:343-357",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "PHYSICAL", "n/a",
     "", "warm buffered read caveat: conclusions do not extend to WAL/cold"),
    ("wait", "T2", "RESEARCH", "back ops",
     "group build/present serialization (positions 8-15; occupancy-8 refills)",
     "per full refill",
     "C:r3_ready_batch.hpp:468-605 C:bench_w03:399-413",
     "F5 corrected: cw0 pos0-7 1.17-1.30% vs pos8-15 9.04-9.19% tail>55us; cw8 26.32%; instrument-on R phases only",
     "PERTURBED+SOURCE_VERIFIED", "CONDITIONAL", "implementation",
     "research presentation choice; product-owned batching alternatives are designs on hold (R1)",
     "T0 semantics per-arm consistent"),
    ("wait", "T3", "PRODUCT", "both",
     "lock wait access_mtx_/dispatch_mtx_ (submitter vs driver)",
     "whenever contended",
     "master lock sites (all files)",
     "windows source-verified; magnitude never measured",
     "UNMEASURED", "CONDITIONAL", "implementation",
     "lock-held instrumentation would measure (not run)",
     ""),
    ("wait", "T4", "PRODUCT", "submitter",
     "submit must not wait (contract)",
     "0", "M:async_io_context.cpp:249-269 docs/explicit-io-contract.md",
     "contract + source",
     "SOURCE_VERIFIED", "INVARIANT", "semantic",
     "", ""),
    ("wait", "T5", "KERNEL", "kernel",
     "kernel completion instant per op",
     "per op", "kernel internal",
     "E10/E12 measured at reap/wake level only",
     "UNAVAILABLE", "PHYSICAL", "n/a",
     "kernel tracing only (not run)",
     ""),
    ("wait", "T6", "PRODUCT", "driver",
     "CQE-visible-but-unreaped backlog",
     "bounded by pass-entry cq_ready",
     "M:uring_completion.cpp:200-278",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "REQUIRED_SEMANTIC", "semantic=pass boundary mechanism",
     "", "reap entry bound"),
    ("wait", "T7", "PRODUCT", "driver+caller",
     "reaped-unpublished backlog (publication_pending_ FIFO)",
     "same pass", "M:uring_backend.cpp:640-657",
     "structure source-verified; T4-T3 +59ns PERTURBED lead",
     "SOURCE_VERIFIED+PERTURBED", "REQUIRED_SEMANTIC", "semantic",
     "", "publication sweep stays outside dispatch_mtx_"),
    ("wait", "T8", "BENCH", "submitter",
     "published-but-unobserved + front latch",
     "per drain", "M:bench_w03:387-418 (G4/G5 E16 archive)",
     "observe tail-mean A0 24.9us vs R 10.4us over DIFFERENT tail sets; cannot separate observation-order effects (R2)",
     "PERTURBED", "BENCH_PROTOCOL", "bench",
     "any_ready consumption would change bench task definition and buffer ownership (C6)",
     "verify + in-order endpoint preserved"),
    ("wait", "T9", "BENCH", "submitter",
     "cv wake granularity = one pass aggregate",
     "W03 each wake", "M:bench_w03:231-243",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "BENCH_PROTOCOL", "bench",
     "", "two-hop wake is W03 shape"),
    ("wait", "T10", "BENCH", "W02 thread",
     "wait_one park directly on eventfd (one hop)",
     "per settle", "M:request_scope.cpp:103 M:async_io_context.cpp:573-696",
     "source read on frozen tree",
     "SOURCE_VERIFIED", "BENCH_PROTOCOL", "bench",
     "", "W02 shape"),
    ("wait", "T11", "BENCH", "submitter",
     "d2serial full round-trip serialization per op",
     "per op at depth1",
     "case matrix",
     "arm fails with ~no batching (r3 counters)",
     "HISTORICAL_REPORTED", "DIAGNOSTIC", "bench",
     "isolate via product-path-only variant (none exists; see R1)",
     ""),
]


def build(path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(COLUMNS)
        for row in ROWS:
            w.writerow(row)
    print(f"wrote {path} ({len(ROWS)} rows + header)")


def check(path):
    problems = []
    if not os.path.exists(path):
        return [f"missing {path}"]
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        return ["empty file"]
    if rows[0] != COLUMNS:
        problems.append(f"header mismatch: {rows[0]}")
    ids = set()
    for i, row in enumerate(rows[1:], start=2):
        if len(row) != len(COLUMNS):
            problems.append(f"line {i}: {len(row)} fields (want {len(COLUMNS)})")
            continue
        rec = dict(zip(COLUMNS, row))
        for k in REQUIRED_NON_EMPTY:
            if not rec[k].strip():
                problems.append(f"line {i}: empty required field {k}")
        if rec["kind"] not in ("work", "wait"):
            problems.append(f"line {i}: kind {rec['kind']!r}")
        if rec["id"] in ids:
            problems.append(f"line {i}: duplicate id {rec['id']}")
        ids.add(rec["id"])
        for g in rec["evidence_grade"].split("+"):
            if g and g not in GRADES:
                problems.append(f"line {i}: unknown evidence_grade {g!r}")
        for c in rec["classification"].split("+"):
            if c and c not in CLASSIFICATIONS:
                problems.append(f"line {i}: unknown classification {c!r}")
    if len(rows) - 1 != len(ROWS):
        problems.append(f"{len(rows)-1} data rows (expected {len(ROWS)})")
    for p in problems:
        print(f"FAIL {p}")
    print(f"== ledger check: {len(rows)-1} rows, {len(problems)} problems ==")
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=["build", "check"])
    ap.add_argument("path", nargs="?", default=os.path.abspath(CSV_PATH))
    args = ap.parse_args(argv)
    if args.mode == "build":
        build(args.path)
        return 1 if check(args.path) else 0
    return 1 if check(args.path) else 0


if __name__ == "__main__":
    sys.exit(main())
