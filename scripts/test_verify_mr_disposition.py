#!/usr/bin/env python3
"""Corruption discriminators for verify_mr_disposition.py.

Every case mutates the real census/tier-2/overlay triple in memory and
asserts the validator reports the specific failure that mutation must
produce. The clean baseline must pass: a validator that fails (or passes)
everything would not distinguish anything. Includes the two reviewer
counterexamples from the #472 narrow re-review:

  A. a consumer id appended to the wrong unit's census_rows must fail
     against its true unit (c-x01-001 belongs to T2-08, not T2-04);
  B. an id resolving to no census record must fail loudly; only the
     whitelisted blocker rows (B-01/B-05) may stay unresolved.

Usage: python3 scripts/test_verify_mr_disposition.py
"""

import copy
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location(
    "verify_mr_disposition", ROOT / "scripts" / "verify_mr_disposition.py")
vmd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vmd)


def load_real():
    return (
        json.loads((ROOT / "docs/review/m-consumer-edges.json").read_text(
            encoding="utf-8")),
        json.loads((ROOT / "docs/review/m-r-tier2-isolation-record.json").read_text(
            encoding="utf-8")),
        json.loads((ROOT / "docs/review/m-r-consumer-disposition-final.json").read_text(
            encoding="utf-8")),
    )


def unit(tier2, uid):
    return next(i for i in tier2["items"] if i["id"] == uid)


def entry(overlay, cid, disposition=None):
    for e in overlay["dispositions"]:
        if e["consumer_id"] == cid and (
                disposition is None or e["final_disposition"] == disposition):
            return e
    raise AssertionError(f"no entry for {cid}/{disposition}")


CASES = []


def case(name, expect):
    def deco(fn):
        CASES.append((name, expect, fn))
        return fn
    return deco


@case("clean baseline passes", None)
def _(c, t, o):
    pass


@case("counterexample A: c-x01-001 appended to T2-04 (true unit T2-08)",
      "borne by this unit")
def _(c, t, o):
    unit(t, "T2-04")["census_rows"].append("c-x01-001")


@case("counterexample A names the true unit in the failure", "T2-08")
def _(c, t, o):
    unit(t, "T2-04")["census_rows"].append("c-x01-001")


@case("counterexample B: unknown id c-fake-999 in T2-04 census_rows",
      "matches no census record")
def _(c, t, o):
    unit(t, "T2-04")["census_rows"].append("c-fake-999")


@case("non-whitelisted blocker row B-99 fails like any unknown id",
      "matches no census record")
def _(c, t, o):
    unit(t, "T2-11")["census_rows"].append("B-99")


@case("overlay entry borne by the wrong unit",
      "borne by this unit")
def _(c, t, o):
    entry(o, "c-x01-037")["isolation_unit"] = "T2-04"


@case("duplicate subscope key", "duplicate subscope key")
def _(c, t, o):
    o["dispositions"].append(copy.deepcopy(o["dispositions"][0]))


@case("overlay entry outside the census", "is not a census record")
def _(c, t, o):
    entry(o, "c-x16-008")["consumer_id"] = "c-fake-001"


@case("original_state drift vs census", "original_state")
def _(c, t, o):
    entry(o, "c-x16-008")["original_state"] = "COMPAT_TEST_RETAINED"


@case("unknown disposition", "unknown disposition")
def _(c, t, o):
    entry(o, "c-x16-008")["final_disposition"] = "PARTLY_MIGRATED"


@case("MIGRATED without evidence_commit",
      "MIGRATED without evidence_commit")
def _(c, t, o):
    entry(o, "c-x01-024", "MIGRATED").pop("evidence_commit")


@case("INCLUDE_HYGIENE_COMPLETED without evidence_commit",
      "INCLUDE_HYGIENE_COMPLETED without evidence_commit")
def _(c, t, o):
    entry(o, "c-x06-004").pop("evidence_commit")


@case("ALREADY_CANONICAL must not claim an isolation unit",
      "must not carry an isolation_unit")
def _(c, t, o):
    entry(o, "c-x01-020", "ALREADY_CANONICAL")["isolation_unit"] = "T2-05"


@case("POLICY_ISOLATED without exit_condition",
      "POLICY_ISOLATED without exit_condition")
def _(c, t, o):
    entry(o, "c-x01-037").pop("exit_condition")


@case("census M-R row loses every overlay entry", "has no overlay entry")
def _(c, t, o):
    o["dispositions"] = [e for e in o["dispositions"]
                         if e["consumer_id"] != "c-x01-029"]


@case("unit census_rows drops a row the overlay still isolates under it",
      "does not list")
def _(c, t, o):
    unit(t, "T2-03")["census_rows"].remove("c-x01-022")


def main():
    census, tier2, overlay = load_real()
    failed = 0
    for name, expect, mutate in CASES:
        c = copy.deepcopy(census)
        t = copy.deepcopy(tier2)
        o = copy.deepcopy(overlay)
        mutate(c, t, o)
        if expect is None:
            if (c, t, o) != (census, tier2, overlay):
                ok = False
                failures = ["<baseline mutation was not a no-op>"]
            else:
                failures = vmd.run_checks(c, t, o)[0]
                ok = not failures
        else:
            if (c, t, o) == (census, tier2, overlay):
                ok = False
                failures = ["<mutation was a no-op>"]
            else:
                failures = vmd.run_checks(c, t, o)[0]
                ok = bool(failures) and any(expect in f for f in failures)
        if ok:
            print(f"  ok:   {name}")
        else:
            failed += 1
            print(f"  FAIL: {name}")
            for f in failures[:3]:
                print(f"        -> {f}")
    total = len(CASES)
    print(f"\n{total - failed}/{total} discriminator cases behaved as required")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
