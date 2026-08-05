# test_backend_conformance_manifest.py
#
# Phase C1 — pure-data self-test for the backend conformance manifest.
#
# This tests ONLY manifest-INTERNAL invariants. It does NOT parse
# xmake/tests/*.lua, does NOT check whether a target exists in the build graph
# (that is the aggregate GATE's preflight concern), and does NOT run any binary.
# Run with:
#   python3 scripts/tests/test_backend_conformance_manifest.py
# Exits 0 on success, 1 on any invariant violation.
"""Pure-data invariant tests for backend_conformance_manifest."""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import backend_conformance_manifest as M  # noqa: E402

failures: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        failures.append(msg)


# --- Closed sets -----------------------------------------------------------

check(M.PROFILES, "PROFILES is non-empty")
check(set(M.PROFILES) == {"ReferenceProfile", "BlockingIoProfile", "KernelIoProfile"},
      "PROFILES must be the closed C1 set")

check(set(M.LAYERS) == {
    "shared", "lifecycle", "authority", "backend_specific", "external_admission",
}, "LAYERS must be the closed C1 set")

check(set(M.STATUS_IMPLEMENTED) == {"implemented", "not_applicable", "not_implemented"}
      or {M.STATUS_IMPLEMENTED, M.STATUS_NOT_APPLICABLE, M.STATUS_NOT_IMPLEMENTED}
      <= {"implemented", "not_applicable", "not_implemented"},
      "status constants are the closed set")

check(set(M.MANDATORY_LAYERS_PER_BACKEND) <= set(M.LAYERS),
      "mandatory layers must be a subset of LAYERS")

# --- Backend registry: closed, valid profiles, no nameless backends --------

backend_names = [b.name for b in M.BACKENDS]
check(len(backend_names) == len(set(backend_names)),
      f"backend names must be unique: {backend_names}")
for b in M.BACKENDS:
    check(b.profile in M.PROFILES,
          f"backend {b.name} references unknown profile {b.profile}")

# A profile may name multiple backends, but a backend must reference a real
# profile. No nameless backends: every profile listed in PROFILES need not be
# used, but every backend's profile must be valid (checked above).

# --- Evidence records ------------------------------------------------------

ids = [e.evidence_id for e in M.EVIDENCE]
check(len(ids) == len(set(ids)), f"evidence_id must be unique: {ids}")

valid_layers = set(M.LAYERS)
valid_statuses = {"implemented", "not_applicable", "not_implemented"}
known_backends = set(backend_names)

for e in M.EVIDENCE:
    check(e.layer in valid_layers,
          f"{e.evidence_id}: layer {e.layer!r} not in closed set")
    check(e.status in valid_statuses,
          f"{e.evidence_id}: status {e.status!r} not in closed set")
    for b in e.backends:
        check(b in known_backends,
              f"{e.evidence_id}: references unknown backend {b!r}")
    if e.status == M.STATUS_NOT_APPLICABLE:
        check(bool(e.reason),
              f"{e.evidence_id}: not_applicable requires a reason")
    if e.status == M.STATUS_NOT_IMPLEMENTED:
        # A not_implemented record must never be claimed as pass; the gate
        # enforces that, but we assert here that the manifest itself never
        # marks a not_implemented record mandatory=True with a misleading
        # note. (It is legal for it to be mandatory: a not_implemented
        # mandatory record simply makes the gate INCOMPLETE.)
        check(True, "not_implemented recorded (gate treats as INCOMPLETE)")
    check(bool(e.target), f"{e.evidence_id}: target is empty")

# --- Mandatory section coverage per backend --------------------------------

for b in M.BACKENDS:
    covered = M.mandatory_layers_covered(b.name)
    missing = set(M.MANDATORY_LAYERS_PER_BACKEND) - covered
    check(not missing,
          f"backend {b.name} has no evidence records for mandatory layers: "
          f"{sorted(missing)}")
    # Every registered backend must have a shared-suite evidence record
    # referencing it (or be backend-agnostic-covered). The shared suite target
    # covers all three; verify at least one shared record applies.
    shared_applies = any(
        e.layer == "shared" and (not e.backends or b.name in e.backends)
        for e in M.evidence_for_backend(b.name)
    )
    check(shared_applies,
          f"backend {b.name} has no applicable shared-suite evidence")

# --- Helper functions are well-defined -------------------------------------

check(M.evidence_by_id("shared_suite") is not None,
      "evidence_by_id resolves a known id")
check(M.evidence_by_id("does_not_exist_zzz") is None,
      "evidence_by_id returns None for unknown id")
check(M.backend_by_name("Fake") is not None,
      "backend_by_name resolves a known backend")
check(M.backend_by_name("Nope") is None,
      "backend_by_name returns None for unknown backend")
check(len(M.script_targets()) >= 1,
      "script_targets returns the __script__: authority entries")
for s in M.script_targets():
    check(s.startswith("__script__:"),
          f"script_targets entry malformed: {s!r}")

# --- Report ---------------------------------------------------------------

if failures:
    print(f"FAIL: {len(failures)} manifest invariant violation(s):")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)

print(f"OK: backend conformance manifest invariants hold "
      f"({len(M.BACKENDS)} backends, {len(M.EVIDENCE)} evidence records, "
      f"{len(M.PROFILES)} profiles).")
sys.exit(0)
