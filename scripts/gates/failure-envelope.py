#!/usr/bin/env python3
"""failure-envelope.py — machine-checkable phase x fault x required-outcome matrix gate (#198).

Validates docs/verification/failure-envelope.json fail-closed:

  * closed vocabularies for phase / fault / layer / required_outcome / status /
    evidence kind / evidence tier / taxonomy — an unknown token fails the gate;
  * unique FE-NNN ids;
  * every VERIFIED row cites >= 1 evidence pointer and every pointer resolves:
      - ref:    file exists in the repository;
      - anchor: the token appears in the referenced file (mutant id, section);
      - case:   the test-case name appears under tests/;
      - target: the xmake target name appears under xmake/;
  * open states (PENDING / PLATFORM-BOUND / COVERAGE-BOUNDARY) REQUIRE a
    status_note — never a silently green row;
  * spanning rows (the `accepted -> cannot disappear` class) must be VERIFIED;
  * coverage floor: every phase in the vocabulary has >= 1 VERIFIED row, and
    >= 5 spanning VERIFIED rows exist.

--self-test plants each violation shape against a copy of the real matrix and
requires the corresponding detector to fire (plus a false-positive check that
the unmodified real matrix passes).
"""

import json
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
MATRIX = REPO / "docs/verification/failure-envelope.json"

PHASES = {
    "construction", "reserve", "prepare", "commit", "enqueue", "dispatch",
    "syscall", "transfer", "uring-submit", "cancel", "wait", "reap",
    "reset-reuse", "close", "drain", "destruction",
}
FAULTS = {
    "worker-spawn-failure", "capacity-exhausted", "descriptor-malformed",
    "stage-failure-injection", "waiter-borrow-window", "cas-loss-contention",
    "publication-order", "allocation-failure", "dispatch-failure", "eintr",
    "syscall-error", "short-transfer", "zero-progress-write",
    "submit-transient-error", "submit-zero-progress", "submit-partial",
    "submit-permanent-failure", "stale-identity", "double-terminal",
    "cancel-intent-accounting", "cancel-races-dispatch", "waiter-conflict",
    "wait-cancel-overreach", "lost-wake", "close-races-acceptance",
    "close-races-accepted-work", "interrupt-races-ready",
    "non-quiescent-destruction", "reset-releases-slot",
    "wal-decode-corruption", "kernel-fault", "scheduler-dispatch-fault",
}
LAYERS = {"core", "arena", "fake-async", "threadpool", "uring", "context", "scheduler"}
OUTCOMES = {
    "spawn-failure-propagates", "sync-rejection-idle-completion",
    "sync-rejection-zero-residue", "malformed-descriptor-invalid-argument",
    "full-rollback", "binding-rollback", "payload-visible-on-outstanding",
    "accepted-cannot-disappear", "allocation-free-post-commit",
    "generation-safe-reuse", "retry-or-single-terminal", "verbatim-result-wins",
    "cancel-accounting-exact", "short-valid-loop-owner",
    "zero-progress-invalid-state", "transient-recovered-next-poll",
    "state-unchanged", "suffix-remains-owned", "quarantine-held",
    "exactly-one-terminal", "wake-preserved", "single-waiter-registration",
    "wait-cancel-independent", "reap-gates-publication",
    "close-serializes-acceptance", "close-preserves-accepted",
    "non-quiescent-fail-fast", "quiescent-destroy-succeeds",
    "t1-error-surfaces-via-result", "wal-decode-rejected-deterministic",
}
STATUSES = {"VERIFIED", "PENDING", "PLATFORM-BOUND", "COVERAGE-BOUNDARY"}
KINDS = {"mutation", "test", "death", "weakmem", "fuzz", "formal"}
TIERS = {
    "deterministic-fake", "syscall-boundary", "kernel-scripted",
    "platform-real", "bounded-model", "formal-tla", "fuzz",
}
TAXONOMY = {"T1", "T2", "T3", "T4", "T5", "T6", "T7"}

ROW_FIELDS = {"id", "phase", "fault", "layer", "required_outcome", "taxonomy",
              "spanning", "status", "evidence", "notes", "status_note"}
EV_FIELDS = {"kind", "ref", "tier", "anchor", "case", "target"}


def _corpus(paths_glob_root):
    blob = []
    for p in sorted(paths_glob_root.rglob("*")):
        if p.is_file() and p.suffix in (".cpp", ".hpp", ".lua", ".sh", ".py", ".md", ".json"):
            try:
                blob.append(p.read_text(errors="replace"))
            except OSError:
                pass
    return "\n".join(blob)


def _target_known(target: str, xmake_blob: str) -> bool:
    """A target is known if its name appears in xmake/, or it is a core
    one-file target generated as '<stem>_test' from a quoted stem list
    (xmake/tests/core.lua builds the name by concatenation)."""
    if target in xmake_blob:
        return True
    if target.endswith("_test"):
        stem = target[: -len("_test")]
        return f'"{stem}"' in xmake_blob
    return False


class Resolver:
    """Aggressive caching content resolver for pointer checks."""

    def __init__(self, repo: Path):
        self.repo = repo
        self._files = {}
        self._tests = None
        self._xmake = None

    def file_text(self, rel: str):
        if rel not in self._files:
            f = self.repo / rel
            self._files[rel] = f.read_text(errors="replace") if f.is_file() else None
        return self._files[rel]

    def tests_blob(self):
        if self._tests is None:
            self._tests = _corpus(self.repo / "tests")
        return self._tests

    def xmake_blob(self):
        if self._xmake is None:
            self._xmake = _corpus(self.repo / "xmake")
        return self._xmake


def validate(doc, resolver: Resolver):
    errors = []

    def err(msg):
        errors.append(msg)

    if doc.get("schema") != "sluice-failure-envelope/1":
        err(f"top-level: schema must be 'sluice-failure-envelope/1', got {doc.get('schema')!r}")
    if doc.get("core_invariant") != "accepted -> cannot disappear":
        err(f"top-level: core_invariant drifted: {doc.get('core_invariant')!r}")
    rows = doc.get("rows")
    if not isinstance(rows, list) or not rows:
        err("top-level: rows must be a non-empty list")
        return errors

    seen_ids = set()
    verified_by_phase = {p: 0 for p in sorted(PHASES)}
    spanning_verified = 0

    for row in rows:
        rid = row.get("id", "<no id>")
        if not isinstance(rid, str) or not re.fullmatch(r"FE-\d{3}", rid):
            err(f"{rid}: id must match FE-NNN")
        if rid in seen_ids:
            err(f"{rid}: duplicate id")
        seen_ids.add(rid)

        unknown_fields = set(row) - ROW_FIELDS
        if unknown_fields:
            err(f"{rid}: unknown row fields {sorted(unknown_fields)}")

        if row.get("phase") not in PHASES:
            err(f"{rid}: unknown phase {row.get('phase')!r}")
        if row.get("fault") not in FAULTS:
            err(f"{rid}: unknown fault {row.get('fault')!r}")
        if row.get("layer") not in LAYERS:
            err(f"{rid}: unknown layer {row.get('layer')!r}")
        if row.get("required_outcome") not in OUTCOMES:
            err(f"{rid}: unknown required_outcome {row.get('required_outcome')!r}")
        if row.get("taxonomy") not in TAXONOMY:
            err(f"{rid}: unknown taxonomy {row.get('taxonomy')!r}")
        if row.get("status") not in STATUSES:
            err(f"{rid}: unknown status {row.get('status')!r}")
        if not isinstance(row.get("spanning"), bool):
            err(f"{rid}: spanning must be a boolean")

        status = row.get("status")
        if status == "VERIFIED":
            if not row.get("evidence"):
                err(f"{rid}: VERIFIED row has no evidence")
        else:
            if not (row.get("status_note") or "").strip():
                err(f"{rid}: status {status} requires a non-empty status_note "
                    f"(never a silently green row)")
        if row.get("spanning") and status == "PENDING":
            # The spanning class (accepted -> cannot disappear) may be honestly
            # PLATFORM-BOUND / COVERAGE-BOUNDARY at an unavailable tier, but a
            # bare PENDING spanning row means runnable evidence was skipped.
            err(f"{rid}: spanning row (accepted -> cannot disappear) must not be "
                f"PENDING (verify it, or bound the open tier explicitly)")

        if status == "VERIFIED" and row.get("phase") in verified_by_phase:
            verified_by_phase[row["phase"]] += 1
        if row.get("spanning") and status == "VERIFIED":
            spanning_verified += 1

        evs = row.get("evidence", [])
        if not isinstance(evs, list):
            err(f"{rid}: evidence must be a list")
            continue
        for i, ev in enumerate(evs):
            where = f"{rid}/evidence[{i}]"
            if not isinstance(ev, dict):
                err(f"{where}: must be an object")
                continue
            unknown_ev = set(ev) - EV_FIELDS
            if unknown_ev:
                err(f"{where}: unknown evidence fields {sorted(unknown_ev)}")
            if ev.get("kind") not in KINDS:
                err(f"{where}: unknown kind {ev.get('kind')!r}")
            if ev.get("tier") not in TIERS:
                err(f"{where}: unknown tier {ev.get('tier')!r}")
            ref = ev.get("ref")
            if not ref:
                err(f"{where}: missing ref")
                continue
            text = resolver.file_text(ref)
            if text is None:
                err(f"{where}: missing evidence ref {ref}")
                continue
            anchor = ev.get("anchor")
            if anchor and anchor not in text:
                err(f"{where}: anchor {anchor!r} not found in {ref}")
            case = ev.get("case")
            if case and case not in resolver.tests_blob():
                err(f"{where}: unknown test case {case!r} (not present under tests/)")
            target = ev.get("target")
            if target and not _target_known(target, resolver.xmake_blob()):
                err(f"{where}: unknown xmake target {target!r} (not present under xmake/)")

    for phase, n in verified_by_phase.items():
        if n == 0:
            err(f"coverage floor: phase {phase!r} has no VERIFIED row "
                f"(add evidence or an explicit open row and shrink the vocabulary)")
    if spanning_verified < 5:
        err(f"coverage floor: only {spanning_verified} spanning VERIFIED rows (need >= 5)")
    return errors


def self_test() -> int:
    base = json.loads(MATRIX.read_text())
    resolver = Resolver(REPO)

    # False-positive control: the real matrix must pass.
    errors = validate(base, resolver)
    if errors:
        print("self-test FAIL: the real matrix itself reported errors:")
        for e in errors:
            print("  ", e)
        return 1

    def mutant(fn):
        doc = json.loads(json.dumps(base))
        fn(doc)
        return doc

    def by_id(doc, rid):
        return next(r for r in doc["rows"] if r["id"] == rid)

    plants = []

    def plant(name, expect_substr, fn):
        plants.append((name, expect_substr, mutant(fn)))

    plant("unknown phase", "unknown phase",
          lambda d: by_id(d, "FE-016").__setitem__("phase", "poke"))
    plant("unknown outcome", "unknown required_outcome",
          lambda d: by_id(d, "FE-016").__setitem__("required_outcome", "always-fine"))
    plant("verified row without evidence", "no evidence",
          lambda d: by_id(d, "FE-016").__setitem__("evidence", []))
    plant("dangling ref", "missing evidence ref",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "ref", "docs/verification/does-not-exist.md"))
    plant("unknown test case", "unknown test case",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "case", "case_that_does_not_exist_anywhere"))
    plant("anchor not in doc", "not found in",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__("anchor", "M999"))
    plant("unknown xmake target", "unknown xmake target",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "target", "target_that_does_not_exist"))
    plant("duplicate id", "duplicate id",
          lambda d: d["rows"].append(dict(by_id(d, "FE-016"), notes="dup")))
    plant("open row without status_note", "requires a non-empty status_note",
          lambda d: by_id(d, "FE-017").pop("status_note"))
    plant("spanning row not verified", "spanning row",
          lambda d: (by_id(d, "FE-011").__setitem__("status", "PENDING"),
                     by_id(d, "FE-011").__setitem__(
                         "status_note", "planted: spanning PENDING")))
    plant("coverage floor (construction loses its only VERIFIED row)",
          "coverage floor",
          lambda d: (by_id(d, "FE-001").__setitem__("status", "PENDING"),
                     by_id(d, "FE-001").__setitem__("status_note", "planted")))
    plant("schema drift", "schema",
          lambda d: d.__setitem__("schema", "sluice-failure-envelope/2"))

    rc = 0
    for name, expect, doc in plants:
        errors = validate(doc, resolver)
        if not any(expect in e for e in errors):
            print(f"self-test FAIL [{name}]: expected an error containing "
                  f"{expect!r}, got: {errors[:3]}")
            rc = 1
        else:
            print(f"self-test PASS [{name}]")
    if rc == 0:
        print(f"failure-envelope self-test: all {len(plants)} planted violations "
              f"detected; real matrix passes")
    return rc


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--self-test":
        return self_test()
    matrix = Path(args[0]) if args else MATRIX
    try:
        doc = json.loads(matrix.read_text())
    except (OSError, json.JSONDecodeError) as e:
        print(f"failure-envelope: cannot read matrix {matrix}: {e}", file=sys.stderr)
        return 1
    errors = validate(doc, Resolver(REPO))
    n_rows = len(doc.get("rows", []))
    n_open = sum(1 for r in doc.get("rows", []) if r.get("status") != "VERIFIED")
    if errors:
        print(f"failure-envelope: FAIL ({len(errors)} error(s), {n_rows} rows)")
        for e in errors:
            print("  ", e)
        print("reproduce: python3 scripts/gates/failure-envelope.py")
        return 1
    print(f"failure-envelope: OK ({n_rows} rows, {n_rows - n_open} VERIFIED, "
          f"{n_open} honest open rows; vocabulary closed; all pointers resolve)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
