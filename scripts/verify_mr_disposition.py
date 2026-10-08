#!/usr/bin/env python3
"""Mechanical consistency check for the M-R consumer disposition overlay.

Reads the M0 census (docs/review/m-consumer-edges.json), the tier-2 isolation
record (docs/review/m-r-tier2-isolation-record.json) and the final projection
(docs/review/m-r-consumer-disposition-final.json) and verifies that the M-R
family gate claim is recomputable without trusting any prose:

  1. every census M-R consumer record has at least one overlay entry
     (no unexplained originals) and no overlay entry names a record outside
     the census M-R set; identity is the (consumer_id, path) pair everywhere,
     because NEW-edge ids are only unique within their edge;
  2. census_path/original_state agree with the census row;
  3. a (consumer_id, subscope) key is unique: no subscope is counted both
     migrated and isolated;
  4. field discipline per disposition: MIGRATED / ALREADY_CANONICAL /
     INCLUDE_HYGIENE_COMPLETED carry evidence_commit and no isolation_unit;
     POLICY_ISOLATED carries an isolation_unit that exists in the tier-2
     record plus downstream_owner and exit_condition; DOWNSTREAM_MECHANISM_
     RETAINED carries a downstream owner in {F2, F3, F4} and an exit
     condition; COMPAT_TEST_RETAINED / NOT_A_CONSUMER carry a note or an
     exit condition;
  5. cross-check against the tier-2 record, both directions and unit-exact:
     every census row referenced by a unit's census_rows has an overlay
     entry borne by exactly that unit (POLICY_ISOLATED or COMPAT_TEST_
     RETAINED with isolation_unit == the referencing unit), and every
     overlay isolation_unit reference is a member of that unit's
     census_rows. A reference that resolves to no census record fails
     unless it is a whitelisted blocker row (B-01/B-05); a consumer id
     borne by a different unit also fails.

Cross-family references in census_rows (rows of other families) are
reported, not failed: they are expected bookkeeping.

Discriminator suite: scripts/test_verify_mr_disposition.py.

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
    "ALREADY_CANONICAL",
    "INCLUDE_HYGIENE_COMPLETED",
}
EVIDENCE_DISPOSITIONS = {
    "MIGRATED",
    "ALREADY_CANONICAL",
    "INCLUDE_HYGIENE_COMPLETED",
}
UNIT_BORNE = {"POLICY_ISOLATED", "COMPAT_TEST_RETAINED"}
MECHANISM_OWNERS = {"F2", "F3", "F4"}
ISOLATION_OWNERS = {"F2", "F3", "F4", "F2+F3", "F2+harness"}
BLOCKER_REFS = {"B-01", "B-05"}


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def run_checks(census, tier2, overlay):
    failures = []
    lines = []

    def fail(msg):
        failures.append(msg)

    census_index = {}
    by_id = {}
    for edge in census["EDGES"]:
        for c in edge["consumers"]:
            census_index[(c["consumer_id"], c["path"])] = c
            by_id.setdefault(c["consumer_id"], []).append(c)
    mr_records = {
        key for key, c in census_index.items() if c["family"] == "M-R"
    }

    units = {i["id"]: i for i in tier2["items"]}
    entries = overlay["dispositions"]

    # --- 1/2/4: coverage, census agreement, field discipline
    seen_keys = set()
    covered = {}
    for e in entries:
        key = (e["consumer_id"], e["subscope"])
        if key in seen_keys:
            fail(f"duplicate subscope key {key}")
        seen_keys.add(key)
        ckey = (e["consumer_id"], e["census_path"])
        row = census_index.get(ckey)
        if row is None:
            fail(f"{e['consumer_id']}: {ckey} is not a census record")
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
        covered.setdefault(ckey, []).append(e)
        if d in EVIDENCE_DISPOSITIONS:
            if not e.get("evidence_commit"):
                fail(f"{key}: {d} without evidence_commit")
            if e.get("isolation_unit"):
                fail(f"{key}: {d} must not carry an isolation_unit")
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

    unexplained = sorted(mr_records - set(covered))
    for ckey in unexplained:
        fail(f"census M-R record {ckey} has no overlay entry")

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

    # --- 5: tier-2 cross-check, both directions, unit-exact
    referenced_mr = set()
    cross_family = []
    for uid, unit in units.items():
        for rid in unit["census_rows"]:
            rows = by_id.get(rid)
            if not rows:
                if rid in BLOCKER_REFS:
                    cross_family.append((uid, rid, "blocker-row (whitelisted)"))
                else:
                    fail(f"{uid}: census_rows reference {rid!r} matches no "
                         "census record and is not a whitelisted blocker row")
                continue
            fams = {r["family"] for r in rows}
            mr_rows = [r for r in rows if r["family"] == "M-R"]
            if len(fams) == 1 and mr_rows:
                targets = [(rid, mr_rows[0]["path"])]
            elif mr_rows:
                overlay_paths = {e["census_path"] for e in entries
                                 if e["consumer_id"] == rid}
                hit = [r for r in mr_rows if r["path"] in overlay_paths]
                if not hit:
                    cross_family.append((uid, rid, "id reused, no M-R path match"))
                    continue
                targets = [(rid, r["path"]) for r in hit]
            else:
                cross_family.append((uid, rid, "+".join(sorted(fams))))
                continue
            for ckey in targets:
                referenced_mr.add(ckey)
                es = covered.get(ckey, [])
                if not es:
                    fail(f"{uid}: references {ckey} which has no overlay entry")
                    continue
                if not any(e["final_disposition"] in UNIT_BORNE
                           and e.get("isolation_unit") == uid for e in es):
                    borne_elsewhere = sorted(
                        e.get("isolation_unit") or e["final_disposition"]
                        for e in es)
                    fail(f"{uid}: references {ckey} but no overlay entry is "
                         f"borne by this unit (entries: {borne_elsewhere})")

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
    lines.append(f"census M-R records:      {len(mr_records)}")
    lines.append(f"overlay consumer rows:   {len(covered)}")
    lines.append(f"overlay subscope entries:{len(entries)}")
    for d in sorted(by_disp):
        lines.append(f"  {d:32s} {by_disp[d]}")
    lines.append(f"tier-2 census_rows referencing M-R rows: {len(referenced_mr)}")
    lines.append(f"tier-2 census_rows outside M-R:         {len(cross_family)}")
    for uid, rid, fam in cross_family:
        lines.append(f"  {uid}: {rid} ({fam})")
    return failures, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args()
    root = Path(args.root)

    census = load(root / "docs/review/m-consumer-edges.json")
    tier2 = load(root / "docs/review/m-r-tier2-isolation-record.json")
    overlay = load(root / "docs/review/m-r-consumer-disposition-final.json")

    failures, lines = run_checks(census, tier2, overlay)
    for ln in lines:
        print(ln)

    if failures:
        print(f"\nFAIL: {len(failures)} problem(s)")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nOK: M-R disposition overlay is total, exclusive and unit-exactly "
          "consistent with the census and the tier-2 record")
    return 0


if __name__ == "__main__":
    sys.exit(main())
