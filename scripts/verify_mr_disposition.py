#!/usr/bin/env python3
"""Mechanical consistency check for the M-R consumer disposition overlay.

Reads the M0 census (docs/review/m-consumer-edges.json), the tier-2 isolation
record (docs/review/m-r-tier2-isolation-record.json) and the final projection
(docs/review/m-r-consumer-disposition-final.json) and verifies that the M-R
family gate claim is recomputable without trusting any prose:

  1. every census M-R consumer record has at least one overlay entry
     (no unexplained originals) and no overlay entry names a record outside
     the census M-R set;
  2. census_path/original_state agree with the census row (NEW-edge ids are
     only unique within their edge, so identity is the id+path pair);
  3. a (consumer_id, subscope) key is unique: no subscope is counted both
     migrated and isolated;
  4. field discipline per disposition: MIGRATED carries evidence_commit;
     POLICY_ISOLATED carries an isolation_unit that exists in the tier-2
     record plus downstream_owner and exit_condition; DOWNSTREAM_MECHANISM_
     RETAINED carries a downstream owner in {F2, F3, F4} and an exit
     condition; COMPAT_TEST_RETAINED / NOT_A_CONSUMER carry a note or an
     exit condition;
  5. cross-check against the tier-2 record, both directions: every census
     row referenced by a unit's census_rows has an overlay entry that is
     isolated/retained or explains its migration (provenance note), and
     every overlay isolation_unit reference is a member of that unit's
     census_rows.

Cross-family references in census_rows (rows of other families plus blocker
rows) are reported, not failed: they are expected bookkeeping.

Usage: python3 scripts/verify_mr_disposition.py [--root REPO]
"""

import argparse
import json
import sys
from pathlib import Path

DISPOSITIONS = {
    "MIGRATED",
    "POLICY_ISOLATED",
    "COMPAT_TEST_RETAINED",
    "NOT_A_CONSUMER",
    "DOWNSTREAM_MECHANISM_RETAINED",
}
MECHANISM_OWNERS = {"F2", "F3", "F4"}
ISOLATION_OWNERS = {"F2", "F3", "F4", "F2+F3", "F2+harness"}


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args()
    root = Path(args.root)

    census = load(root / "docs/review/m-consumer-edges.json")
    tier2 = load(root / "docs/review/m-r-tier2-isolation-record.json")
    overlay = load(root / "docs/review/m-r-consumer-disposition-final.json")

    census_index = {}
    for edge in census["EDGES"]:
        for c in edge["consumers"]:
            census_index[(c["consumer_id"], c["path"])] = c
    mr_ids = {
        cid for (cid, _p), c in census_index.items() if c["family"] == "M-R"
    }

    units = {i["id"]: i for i in tier2["items"]}
    entries = overlay["dispositions"]

    failures = []

    def fail(msg):
        failures.append(msg)

    # --- 1/2: coverage, census agreement, field discipline
    seen_keys = set()
    covered = {}
    for e in entries:
        key = (e["consumer_id"], e["subscope"])
        if key in seen_keys:
            fail(f"duplicate subscope key {key}")
        seen_keys.add(key)
        row = census_index.get((e["consumer_id"], e["census_path"]))
        if row is None:
            fail(f"{e['consumer_id']}: ({e['consumer_id']}, {e['census_path']}) "
                 "is not a census record")
            continue
        if row["family"] != "M-R":
            fail(f"{e['consumer_id']}: census family {row['family']} != M-R")
            continue
        if e["original_state"] != row["migration_state"]:
            fail(f"{e['consumer_id']}: original_state {e['original_state']} != "
                 f"census {row['migration_state']}")
        d = e["final_disposition"]
        if d not in DISPOSITIONS:
            fail(f"{e['consumer_id']}: unknown disposition {d}")
            continue
        covered.setdefault(e["consumer_id"], []).append(e)
        if d == "MIGRATED" and not e.get("evidence_commit"):
            fail(f"{key}: MIGRATED without evidence_commit")
        if d == "POLICY_ISOLATED":
            unit = e.get("isolation_unit")
            if unit not in units:
                fail(f"{key}: isolation_unit {unit!r} not in tier-2 record")
            if e.get("downstream_owner") not in ISOLATION_OWNERS:
                fail(f"{key}: POLICY_ISOLATED owner {e.get('downstream_owner')!r}")
            if not e.get("exit_condition"):
                fail(f"{key}: POLICY_ISOLATED without exit_condition")
        if d == "DOWNSTREAM_MECHANISM_RETAINED":
            if e.get("downstream_owner") not in MECHANISM_OWNERS:
                fail(f"{key}: mechanism owner {e.get('downstream_owner')!r} "
                     "not in {F2, F3, F4}")
            if not e.get("exit_condition"):
                fail(f"{key}: DOWNSTREAM_MECHANISM_RETAINED without exit_condition")
        if d in ("COMPAT_TEST_RETAINED", "NOT_A_CONSUMER"):
            if not (e.get("exit_condition") or e.get("note")):
                fail(f"{key}: {d} without note or exit_condition")

    unexplained = sorted(mr_ids - set(covered))
    for cid in unexplained:
        fail(f"census M-R record {cid} has no overlay entry")
    outside = sorted(
        e["consumer_id"] for e in entries
        if (e["consumer_id"], e["census_path"]) not in census_index
    )
    for cid in outside:
        fail(f"overlay entry {cid} is not a census record")

    # --- 3: migrated-and-isolated conflict is impossible because subscope keys
    # are unique and each entry carries one disposition; assert the invariant
    # explicitly so the rule stays executable.
    per_key = {}
    for e in entries:
        per_key.setdefault((e["consumer_id"], e["subscope"]), set()).add(
            e["final_disposition"])
    for key, ds in per_key.items():
        if len(ds) > 1:
            fail(f"{key}: counted as multiple dispositions {sorted(ds)}")

    # --- 5: tier-2 cross-check, both directions
    referenced_mr = {}
    cross_family = []
    for uid, unit in units.items():
        for rid in unit["census_rows"]:
            rows = [c for (i, _p), c in census_index.items() if i == rid]
            if not rows:
                cross_family.append((uid, rid, "blocker-row (not a consumer id)"))
                continue
            fams = {r["family"] for r in rows}
            if "M-R" in fams and len(fams) == 1:
                referenced_mr.setdefault(rid, []).append(uid)
            elif "M-R" in fams:
                # id reused across edges: match against the overlay paths
                paths = {e["census_path"] for e in entries
                         if e["consumer_id"] == rid}
                if any(r["path"] in paths for r in rows):
                    referenced_mr.setdefault(rid, []).append(uid)
                else:
                    cross_family.append((uid, rid, "id reused, no M-R path match"))
            else:
                cross_family.append((uid, rid, "+".join(sorted(fams))))

    for cid, uids in sorted(referenced_mr.items()):
        es = covered.get(cid, [])
        if not es:
            fail(f"{cid}: referenced by {uids} but absent from the overlay")
            continue
        has_isolated = any(
            e["final_disposition"] in ("POLICY_ISOLATED", "COMPAT_TEST_RETAINED")
            for e in es)
        if not has_isolated:
            if not all(e.get("note") for e in es if
                       e["final_disposition"] == "MIGRATED"):
                fail(f"{cid}: referenced by {uids} with only unexplained "
                     "MIGRATED subscopes")

    for e in entries:
        unit = e.get("isolation_unit")
        if unit and unit in units:
            if e["consumer_id"] not in units[unit]["census_rows"]:
                fail(f"({e['consumer_id']}, {e['subscope']}): unit {unit} "
                     "does not list this consumer_id in census_rows")

    # --- summary
    by_disp = {}
    for e in entries:
        by_disp[e["final_disposition"]] = by_disp.get(e["final_disposition"], 0) + 1
    print(f"census M-R records:      {len(mr_ids)}")
    print(f"overlay consumer ids:    {len(covered)}")
    print(f"overlay subscope entries:{len(entries)}")
    for d in sorted(by_disp):
        print(f"  {d:32s} {by_disp[d]}")
    print(f"tier-2 census_rows referencing M-R ids: {len(referenced_mr)}")
    print(f"tier-2 census_rows outside M-R:        {len(cross_family)}")
    for uid, rid, fam in cross_family:
        print(f"  {uid}: {rid} ({fam})")

    if failures:
        print(f"\nFAIL: {len(failures)} problem(s)")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nOK: M-R disposition overlay is total, exclusive and consistent "
          "with the census and the tier-2 record")
    return 0


if __name__ == "__main__":
    sys.exit(main())
